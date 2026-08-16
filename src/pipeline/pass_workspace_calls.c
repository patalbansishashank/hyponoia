/*
 * pass_workspace_calls.c — resolve a direct source-level call from one
 * workspace member into another — the plugin case.
 *
 * The rationale, the edge type, the two signals and the fail-closed rule are
 * all in pass_workspace_calls.h. This file is the mechanism.
 */
#include "pipeline/pass_workspace_calls.h"

#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "graph_buffer/graph_buffer.h"

#include <sqlite3/sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    WS_PATH_BUF = 1024,
    WS_PROPS_BUF = 1024,
    WS_MAX_SPECIFIERS = 256, /* unresolved specifiers considered per file */
    WS_MAX_SITES = 65536,    /* recorded call sites considered per member */
    WS_MAX_FILE_ROWS = 65536,
};

/* ── The rendezvous key ──────────────────────────────────────────── */

bool hyp_workspace_specifier_key(const char *spec, char *out, size_t out_sz) {
    if (out && out_sz > 0) {
        out[0] = '\0';
    }
    if (!spec || !out || out_sz == 0) {
        return false;
    }
    char norm[WS_PATH_BUF];
    size_t n = 0;
    bool prev_slash = false;
    for (const char *p = spec; *p; p++) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/' && prev_slash) {
            continue;
        }
        if (n + 1 >= sizeof(norm)) {
            return false; /* refused, never truncated */
        }
        norm[n++] = c;
        prev_slash = (c == '/');
    }
    norm[n] = '\0';

    /* The caller-relative prefix is exactly the part that cannot survive the
     * crossing: a File node's path is relative to ITS OWN member's root. */
    const char *s = norm;
    for (;;) {
        if (s[0] == '.' && s[1] == '/') {
            s += 2;
        } else if (s[0] == '.' && s[1] == '.' && s[2] == '/') {
            s += 3;
        } else if (s[0] == '/') {
            s += 1;
        } else {
            break;
        }
    }
    size_t len = strlen(s);
    if (len == 0 || len >= out_sz) {
        return false;
    }
    memcpy(out, s, len + 1);
    return true;
}

/* ── Recording the two dropped facts (index time) ────────────────── */

int64_t hyp_pipeline_record_unresolved_call(hyp_gbuf_t *gbuf, int64_t caller_id,
                                            const char *callee) {
    if (!gbuf || caller_id <= 0 || !callee || !callee[0]) {
        return 0;
    }
    char qn[HYP_WS_QN_BUF];
    int written = snprintf(qn, sizeof(qn), "%s%s", HYP_WS_TAG_EXTERN, callee);
    if (written <= 0 || (size_t)written >= sizeof(qn)) {
        return 0;
    }
    int64_t ext_id = hyp_gbuf_upsert_node(gbuf, HYP_WS_LABEL_EXTERN, callee, qn, "", 0, 0, "{}");
    if (ext_id <= 0 || ext_id == caller_id) {
        return 0;
    }
    char esc[HYP_SZ_256];
    hyp_json_escape(esc, sizeof(esc), callee);
    char props[HYP_SZ_512];
    snprintf(props, sizeof(props), "{\"callee\":\"%s\"}", esc);
    hyp_gbuf_insert_edge(gbuf, caller_id, ext_id, HYP_WS_EDGE_UNRESOLVED_CALL, props);
    return ext_id;
}

int64_t hyp_pipeline_record_unresolved_import(hyp_gbuf_t *gbuf, int64_t file_id,
                                              const char *specifier) {
    if (!gbuf || file_id <= 0) {
        return 0;
    }
    char key[HYP_WS_QN_BUF];
    if (!hyp_workspace_specifier_key(specifier, key, sizeof(key))) {
        return 0;
    }
    char qn[HYP_WS_QN_BUF];
    int written = snprintf(qn, sizeof(qn), "%s%s", HYP_WS_TAG_INCLUDE, key);
    if (written <= 0 || (size_t)written >= sizeof(qn)) {
        return 0;
    }
    int64_t inc_id = hyp_gbuf_upsert_node(gbuf, HYP_WS_LABEL_INCLUDE, key, qn, "", 0, 0, "{}");
    if (inc_id <= 0 || inc_id == file_id) {
        return 0;
    }
    char esc[HYP_SZ_256];
    hyp_json_escape(esc, sizeof(esc), specifier);
    char props[HYP_SZ_512];
    snprintf(props, sizeof(props), "{\"specifier\":\"%s\"}", esc);
    hyp_gbuf_insert_edge(gbuf, file_id, inc_id, HYP_WS_EDGE_UNRESOLVED_IMPORT, props);
    return inc_id;
}

/* ── Path-suffix matching ────────────────────────────────────────── */

/* Does `path` end with `key` on a path-segment boundary? "src/plugin_api.h"
 * ends with "plugin_api.h"; "src/myplugin_api.h" does not. Case-sensitive and
 * exact — the SQL LIKE that fetches candidates is a prefilter only (it is
 * ASCII-case-insensitive and treats % and _ as wildcards, so it can only ever
 * over-match), and this is what decides. */
static bool ws_path_has_suffix(const char *path, const char *key) {
    if (!path || !key || !key[0]) {
        return false;
    }
    size_t plen = strlen(path);
    size_t klen = strlen(key);
    if (klen > plen) {
        return false;
    }
    if (strcmp(path + (plen - klen), key) != 0) {
        return false;
    }
    return plen == klen || path[plen - klen - 1] == '/';
}

/* ── Small SQL helpers ───────────────────────────────────────────── */

typedef struct {
    char slug[HYP_WS_NAME_BUF];
    char decl_file[WS_PATH_BUF]; /* the matched file, inside that member */
} ws_answer_t;

/* Every unresolved specifier recorded against one file of one member. */
typedef struct {
    int count;
    bool overflowed;
    char keys[WS_MAX_SPECIFIERS][HYP_WS_QN_BUF];
} ws_specifiers_t;

static bool ws_collect_specifiers(struct sqlite3 *db, const char *project, const char *file_path,
                                  ws_specifiers_t *out) {
    out->count = 0;
    out->overflowed = false;
    if (!file_path || !file_path[0]) {
        return true;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT inc.qualified_name FROM nodes f "
            "JOIN edges e ON e.source_id = f.id AND e.type = '" HYP_WS_EDGE_UNRESOLVED_IMPORT "' "
            "JOIN nodes inc ON inc.id = e.target_id "
            "WHERE f.project = ?1 AND f.file_path = ?2 AND f.label = 'File' "
            "ORDER BY inc.id",
            HYP_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        return false;
    }
    bool ok = sqlite3_bind_text(st, 1, project, HYP_NOT_FOUND, SQLITE_STATIC) == SQLITE_OK &&
              sqlite3_bind_text(st, 2, file_path, HYP_NOT_FOUND, SQLITE_STATIC) == SQLITE_OK;
    int step_rc = SQLITE_DONE;
    while (ok && (step_rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *qn = (const char *)sqlite3_column_text(st, 0);
        if (!qn || strncmp(qn, HYP_WS_TAG_INCLUDE, strlen(HYP_WS_TAG_INCLUDE)) != 0) {
            continue;
        }
        const char *key = qn + strlen(HYP_WS_TAG_INCLUDE);
        if (!key[0]) {
            continue;
        }
        if (out->count >= WS_MAX_SPECIFIERS) {
            out->overflowed = true;
            break;
        }
        snprintf(out->keys[out->count], sizeof(out->keys[0]), "%s", key);
        out->count++;
    }
    if (ok && step_rc != SQLITE_ROW && step_rc != SQLITE_DONE) {
        ok = false;
    }
    if (sqlite3_finalize(st) != SQLITE_OK) {
        ok = false;
    }
    return ok;
}

/* SIGNAL (a) for ONE candidate member: does `member` hold a file whose path
 * suffix-matches one of the caller's unresolved specifiers? Writes the matched
 * file into `answer` — it is the evidence recorded on the edge, not a
 * constraint on the target. Returns 1 yes, 0 no, -1 on a query failure. */
static int ws_member_declared(struct sqlite3 *db, const char *member, const ws_specifiers_t *specs,
                              ws_answer_t *answer) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT file_path FROM nodes "
                           "WHERE project = ?1 AND label = 'File' AND file_path LIKE '%' || ?2 "
                           "ORDER BY id",
                           HYP_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    int result = 0;
    for (int i = 0; i < specs->count && result == 0; i++) {
        if (sqlite3_reset(st) != SQLITE_OK || sqlite3_clear_bindings(st) != SQLITE_OK ||
            sqlite3_bind_text(st, 1, member, HYP_NOT_FOUND, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_text(st, 2, specs->keys[i], HYP_NOT_FOUND, SQLITE_STATIC) != SQLITE_OK) {
            result = -1;
            break;
        }
        int rows = 0;
        int step_rc = SQLITE_DONE;
        while (result == 0 && rows < WS_MAX_FILE_ROWS &&
               (step_rc = sqlite3_step(st)) == SQLITE_ROW) {
            rows++;
            const char *fp = (const char *)sqlite3_column_text(st, 0);
            /* LIKE was a prefilter; the segment-boundary check decides. */
            if (fp && ws_path_has_suffix(fp, specs->keys[i])) {
                snprintf(answer->slug, sizeof(answer->slug), "%s", member);
                snprintf(answer->decl_file, sizeof(answer->decl_file), "%s", fp);
                result = 1;
            }
        }
        /* A cap reached without deciding is NOT "no match" — it is "I stopped
         * looking", and reading the second as the first is how a bound turns
         * into a silently wrong answer. */
        if (result == 0 && (step_rc != SQLITE_DONE || rows >= WS_MAX_FILE_ROWS)) {
            result = -1;
        }
    }
    if (sqlite3_finalize(st) != SQLITE_OK) {
        result = -1;
    }
    return result;
}

typedef struct {
    int64_t id;
    char qn[HYP_WS_QN_BUF];
    char file[WS_PATH_BUF];
} ws_target_t;

/* SIGNAL (b): the member provides the callee, and provides it once.
 *
 * Two callables of that name inside one member is ambiguous by the same rule
 * that governs two members — a plugin that exports one symbol twice has not
 * told anyone which one the host links against. Returns 1 target, 0 none,
 * -1 query failure, -2 ambiguous. */
static int ws_pick_target(struct sqlite3 *db, const char *member, const char *callee,
                          ws_target_t *out) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT id, qualified_name, file_path FROM nodes "
                           "WHERE project = ?1 AND name = ?2 AND label IN ('Function','Method') "
                           "ORDER BY id",
                           HYP_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    if (sqlite3_bind_text(st, 1, member, HYP_NOT_FOUND, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, callee, HYP_NOT_FOUND, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(st);
        return -1;
    }
    int found = 0;
    ws_target_t first = {0};
    int step_rc = SQLITE_DONE;
    while (found < 2 && (step_rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (found == 0) {
            first.id = sqlite3_column_int64(st, 0);
            const char *qn = (const char *)sqlite3_column_text(st, 1);
            const char *fp = (const char *)sqlite3_column_text(st, 2);
            snprintf(first.qn, sizeof(first.qn), "%s", qn ? qn : "");
            snprintf(first.file, sizeof(first.file), "%s", fp ? fp : "");
        }
        found++;
    }
    int rc = 0;
    if (found < 2 && step_rc != SQLITE_DONE && step_rc != SQLITE_ROW) {
        rc = -1;
    } else if (found > 1) {
        rc = -2;
    } else if (found == 1) {
        *out = first;
        rc = 1;
    }
    if (sqlite3_finalize(st) != SQLITE_OK) {
        rc = -1;
    }
    return rc;
}

static void ws_report_ambiguity(hyp_workspace_calls_result_t *r, const char *caller_qn,
                                const char *callee, const char *members) {
    r->ambiguous++;
    hyp_log_info("workspace_calls.ambiguous", "caller", caller_qn ? caller_qn : "", "callee",
                 callee ? callee : "", "members", members ? members : "");
    if (r->reported >= HYP_WS_MAX_AMBIGUOUS) {
        return;
    }
    hyp_workspace_ambiguity_t *a = &r->report[r->reported++];
    snprintf(a->caller_qn, sizeof(a->caller_qn), "%s", caller_qn ? caller_qn : "");
    snprintf(a->callee, sizeof(a->callee), "%s", callee ? callee : "");
    snprintf(a->members, sizeof(a->members), "%s", members ? members : "");
}

/* ── One caller member ───────────────────────────────────────────── */

static bool ws_resolve_member(hyp_store_t *store, struct sqlite3 *db, const char *caller_project,
                              const hyp_workspace_repo_t *repos, int repo_count,
                              hyp_workspace_calls_result_t *r) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT caller.id, caller.qualified_name, caller.file_path, ext.name "
                           "FROM edges e "
                           "JOIN nodes caller ON caller.id = e.source_id "
                           "JOIN nodes ext ON ext.id = e.target_id "
                           "WHERE e.project = ?1 AND e.type = '" HYP_WS_EDGE_UNRESOLVED_CALL "' "
                           "ORDER BY e.id",
                           HYP_NOT_FOUND, &st, NULL) != SQLITE_OK) {
        return false;
    }
    if (sqlite3_bind_text(st, 1, caller_project, HYP_NOT_FOUND, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(st);
        return false;
    }

    bool ok = true;
    int scanned = 0;
    /* One file's specifiers are read once and reused for every call site in it;
     * the rows arrive grouped by caller because edges are inserted in file
     * order, and a cache miss is only a re-query, never a wrong answer. */
    char cached_file[WS_PATH_BUF] = {0};
    bool cache_valid = false;
    ws_specifiers_t specs = {0};

    int step_rc = SQLITE_DONE;
    while (ok && (step_rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (++scanned > WS_MAX_SITES) {
            ok = false;
            break;
        }
        int64_t caller_id = sqlite3_column_int64(st, 0);
        const char *caller_qn_raw = (const char *)sqlite3_column_text(st, 1);
        const char *caller_file_raw = (const char *)sqlite3_column_text(st, 2);
        const char *callee_raw = (const char *)sqlite3_column_text(st, 3);
        if (!callee_raw || !callee_raw[0] || !caller_file_raw || !caller_file_raw[0]) {
            continue;
        }
        char caller_qn[HYP_WS_QN_BUF];
        char caller_file[WS_PATH_BUF];
        char callee[HYP_WS_NAME_BUF];
        snprintf(caller_qn, sizeof(caller_qn), "%s", caller_qn_raw ? caller_qn_raw : "");
        snprintf(caller_file, sizeof(caller_file), "%s", caller_file_raw);
        snprintf(callee, sizeof(callee), "%s", callee_raw);
        r->candidates++;

        if (!cache_valid || strcmp(cached_file, caller_file) != 0) {
            /* An overflowed specifier list is a truncated question, and a
             * truncated question cannot produce a trustworthy refusal either. */
            if (!ws_collect_specifiers(db, caller_project, caller_file, &specs) ||
                specs.overflowed) {
                ok = false;
                break;
            }
            snprintf(cached_file, sizeof(cached_file), "%s", caller_file);
            cache_valid = true;
        }
        if (specs.count == 0) {
            continue; /* the crossing was never declared */
        }

        /* THE CANDIDATE SET. Derived, in this loop, from the registry rows —
         * there is no member list in this file. Only the FIRST answer is kept:
         * a second one refuses the site outright, so there is nothing a longer
         * array could be used for. */
        ws_answer_t answer = {0};
        ws_target_t target = {0};
        int answer_count = 0;
        bool within_member_ambiguous = false;
        char members_joined[HYP_WS_MEMBERS_BUF] = {0};
        for (int m = 0; m < repo_count && ok; m++) {
            const char *slug = repos[m].slug;
            if (!slug || strcmp(slug, caller_project) == 0) {
                continue; /* a member never crosses into itself */
            }
            ws_answer_t candidate = {0};
            int declared = ws_member_declared(db, slug, &specs, &candidate);
            if (declared < 0) {
                ok = false;
                break;
            }
            if (declared == 0) {
                continue; /* the crossing into THIS member was never declared */
            }
            ws_target_t candidate_target = {0};
            int picked = ws_pick_target(db, slug, callee, &candidate_target);
            if (picked == -1) {
                ok = false;
                break;
            }
            if (picked == 0) {
                continue; /* declared, but this member does not provide the name */
            }
            if (answer_count == 0) {
                answer = candidate;
                target = candidate_target;
                within_member_ambiguous = (picked == -2);
            }
            answer_count++;
            size_t used = strlen(members_joined);
            snprintf(members_joined + used, sizeof(members_joined) - used, "%s%s", used ? "," : "",
                     slug);
        }
        if (!ok) {
            break;
        }
        if (answer_count == 0) {
            continue;
        }
        if (answer_count > 1) {
            ws_report_ambiguity(r, caller_qn, callee, members_joined);
            continue;
        }
        if (within_member_ambiguous) {
            ws_report_ambiguity(r, caller_qn, callee, answer.slug);
            continue;
        }
        if (target.id <= 0 || target.id == caller_id) {
            continue;
        }

        char esc_callee[HYP_SZ_256];
        char esc_target[HYP_SZ_256];
        char esc_file[HYP_SZ_512];
        char esc_decl[HYP_SZ_512];
        hyp_json_escape(esc_callee, sizeof(esc_callee), callee);
        hyp_json_escape(esc_target, sizeof(esc_target), target.qn);
        hyp_json_escape(esc_file, sizeof(esc_file), target.file);
        hyp_json_escape(esc_decl, sizeof(esc_decl), answer.decl_file);
        char props[WS_PROPS_BUF];
        /* target_project / target_function / target_file are the vocabulary
         * pass_cross_repo already writes, so a consumer reading CROSS_* props
         * reads these unchanged. */
        snprintf(props, sizeof(props),
                 "{\"target_project\":\"%s\",\"target_function\":\"%s\",\"target_file\":\"%s\","
                 "\"callee\":\"%s\",\"declared_in\":\"%s\",\"strategy\":\"workspace_member\"}",
                 answer.slug, esc_target, esc_file, esc_callee, esc_decl);

        hyp_edge_t edge = {
            .project = caller_project,
            .source_id = caller_id,
            .target_id = target.id,
            .type = HYP_WS_EDGE_CROSS_MEMBER,
            .properties_json = props,
        };
        if (hyp_store_insert_edge(store, &edge) <= 0) {
            ok = false;
            break;
        }
        r->edges++;
    }
    if (ok && step_rc != SQLITE_ROW && step_rc != SQLITE_DONE) {
        ok = false;
    }
    if (sqlite3_finalize(st) != SQLITE_OK) {
        ok = false;
    }
    return ok;
}

/* ── Entry point ─────────────────────────────────────────────────── */

hyp_workspace_calls_result_t hyp_workspace_calls_match(hyp_store_t *store) {
    hyp_workspace_calls_result_t r = {0};
    if (!store) {
        return r;
    }
    struct sqlite3 *db = hyp_store_get_db(store);
    if (!db) {
        r.failed = true;
        return r;
    }

    hyp_workspace_repo_t *repos = NULL;
    int repo_count = 0;
    int rc = hyp_store_workspace_repos(store, &repos, &repo_count);
    if (rc == HYP_STORE_NOT_FOUND) {
        /* A pre-A1 per-repo store. ABSENT means "a workspace of one", which has
         * nowhere else to look — not a failure, and not an empty answer to a
         * question that was asked. */
        return r;
    }
    if (rc != HYP_STORE_OK || !repos) {
        r.failed = true;
        return r;
    }
    r.members = repo_count;
    if (repo_count < 2) {
        hyp_store_free_workspace_repos(repos, repo_count);
        return r;
    }

    /* Idempotence: the previous generation goes before this one is written, so
     * a re-run over an unchanged store lands on an identical graph. */
    for (int i = 0; i < repo_count; i++) {
        if (repos[i].slug && hyp_store_delete_edges_by_type(
                                 store, repos[i].slug, HYP_WS_EDGE_CROSS_MEMBER) != HYP_STORE_OK) {
            r.failed = true;
            hyp_store_free_workspace_repos(repos, repo_count);
            return r;
        }
    }

    for (int i = 0; i < repo_count; i++) {
        if (!repos[i].slug) {
            continue;
        }
        if (!ws_resolve_member(store, db, repos[i].slug, repos, repo_count, &r)) {
            r.failed = true;
            break;
        }
    }

    hyp_store_free_workspace_repos(repos, repo_count);

    char buf[HYP_SZ_32];
    char buf2[HYP_SZ_32];
    char buf3[HYP_SZ_32];
    snprintf(buf, sizeof(buf), "%d", r.edges);
    snprintf(buf2, sizeof(buf2), "%d", r.ambiguous);
    snprintf(buf3, sizeof(buf3), "%d", r.candidates);
    hyp_log_info("workspace_calls.done", "edges", buf, "ambiguous", buf2, "candidates", buf3);
    return r;
}
