/*
 * test_workspace_calls.c — NEXT-STEPS §4 Phase 1, unit A8: a direct
 * source-level call from one workspace member into another.
 *
 * ── WHAT MAKES THIS FIXTURE WORTH ANYTHING ──────────────────────────────────
 *
 * Nothing here inserts a node or an edge into the store by hand. Four small C
 * repositories under tests/fixtures/workspace_plugin are copied to a scratch
 * tree, resolved into a workspace by the REAL resolver (hyp_wsr_resolve), each
 * indexed by the REAL pipeline, and then merged into the one workspace store by
 * the REAL graph-buffer merge. Every node this pass matches on was produced by
 * the indexer reading a file — because a fixture assembled from the rows the
 * matcher expects proves only that the matcher can read a table the test wrote.
 *
 * The fixture is arranged so the three outcomes that matter are all present at
 * once and cannot be confused for one another:
 *
 *   host_boot      -> plugin_register  ONE member declares and provides it
 *                                      => CROSS_MEMBER_CALLS
 *   host_draw      -> render           `widgets` provides it and `host` includes
 *                                      NOTHING from `widgets`
 *                                      => NEGATIVE CONTROL, no edge
 *   host_configure -> config_load      `plugin` and `bridge` both declare a
 *                                      shared_config.h and both provide it
 *                                      => AMBIGUOUS, reported, no edge
 *
 * ── THE TWO ENDS ────────────────────────────────────────────────────────────
 *
 * A call is dropped by two different resolvers depending on how many files the
 * repository has — pass_calls.c below 50, pass_parallel.c above it. Both record
 * the crossing now, and wsc_both_resolution_paths_agree runs the SAME fixture
 * through both and asserts the same edge, because "it works on my fixture" has
 * meant "it works on the path my fixture happened to take" here before.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "graph_buffer/graph_buffer.h"
#include "pipeline/pass_workspace_calls.h"
#include "pipeline/pipeline.h"
#include "store/store.h"
#include "store/workspace_resolve.h"

#include <sqlite3/sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WSC_FIXTURE_SRC "tests/fixtures/workspace_plugin"

enum { WSC_MEMBERS = 4, WSC_FILLER_FILES = 60 };

/* The member directory names, which are the fixture's own layout — NOT the
 * candidate set. The candidate set the pass uses is read from the workspace
 * registry inside the store; see wsc_registry_is_the_candidate_set. */
static const char *const wsc_dirs[WSC_MEMBERS] = {"host", "plugin", "bridge", "widgets"};

typedef struct {
    char root[256];   /* scratch tree */
    char cache[512];  /* <root>/cache — HYP_CACHE_DIR for the duration */
    char dbs[512];    /* <root>/dbs — one per-member index, never the cache */
    char toml[512];   /* <root>/hyponoia.toml */
    char *saved_cache;
    hyp_wsr_resolved_t ws;
} wsc_fix_t;

/* ── Recursive copy, so the checked-in fixture is never indexed in place ── */

static bool wsc_copy_file(const char *src, const char *dst) {
    FILE *in = hyp_fopen(src, "rb");
    if (!in) {
        return false;
    }
    FILE *out = hyp_fopen(dst, "wb");
    if (!out) {
        (void)fclose(in);
        return false;
    }
    char buf[4096];
    size_t n = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(in);
    (void)fclose(in);
    return fclose(out) == 0 && ok;
}

static bool wsc_copy_tree(const char *src, const char *dst) {
    if (th_mkdir_p(dst) != 0) {
        return false;
    }
    hyp_dir_t *d = hyp_opendir(src);
    if (!d) {
        return false;
    }
    bool ok = true;
    hyp_dirent_t *ent = NULL;
    while (ok && (ent = hyp_readdir(d)) != NULL) {
        if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }
        char s[1024];
        char t[1024];
        snprintf(s, sizeof(s), "%s/%s", src, ent->name);
        snprintf(t, sizeof(t), "%s/%s", dst, ent->name);
        ok = ent->is_dir ? wsc_copy_tree(s, t) : wsc_copy_file(s, t);
    }
    hyp_closedir(d);
    return ok;
}

/* ── Fixture ─────────────────────────────────────────────────────── */

static void wsc_fix_end(wsc_fix_t *f) {
    if (f->saved_cache) {
        (void)hyp_setenv("HYP_CACHE_DIR", f->saved_cache, 1);
    } else {
        (void)hyp_unsetenv("HYP_CACHE_DIR");
    }
    if (f->root[0]) {
        th_rmtree(f->root);
    }
    free(f->saved_cache);
    memset(f, 0, sizeof(*f));
}

/* Copy the four repos, write the workspace TOML, and resolve it with the real
 * resolver. The TOML is the input a person would write; everything downstream —
 * ids, slugs, canonical roots, the registry — is derived from it by product
 * code, so nothing in this file is a hand-written member list. */
static bool wsc_fix_begin(wsc_fix_t *f) {
    memset(f, 0, sizeof(*f));
    const char *saved = getenv("HYP_CACHE_DIR");
    if (saved) {
        f->saved_cache = strdup(saved);
        if (!f->saved_cache) {
            return false;
        }
    }
    snprintf(f->root, sizeof(f->root), "/tmp/hyp-wsc-XXXXXX");
    if (hyp_mkdtemp(f->root) == NULL) {
        return false;
    }
    snprintf(f->cache, sizeof(f->cache), "%s/cache", f->root);
    snprintf(f->dbs, sizeof(f->dbs), "%s/dbs", f->root);
    if (th_mkdir_p(f->cache) != 0 || th_mkdir_p(f->dbs) != 0 ||
        hyp_setenv("HYP_CACHE_DIR", f->cache, 1) != 0) {
        return false;
    }

    char toml[2048];
    int n = snprintf(toml, sizeof(toml), "name = \"wscfix\"\n");
    for (int i = 0; i < WSC_MEMBERS; i++) {
        char src[1024];
        char dst[1024];
        snprintf(src, sizeof(src), "%s/%s", WSC_FIXTURE_SRC, wsc_dirs[i]);
        snprintf(dst, sizeof(dst), "%s/%s", f->root, wsc_dirs[i]);
        if (!wsc_copy_tree(src, dst)) {
            return false;
        }
        n += snprintf(toml + n, sizeof(toml) - (size_t)n, "\n[[repos]]\npath = \"%s\"\n", dst);
    }
    snprintf(f->toml, sizeof(f->toml), "%s/%s", f->root, HYP_WSR_TOML_NAME);
    if (th_write_file(f->toml, toml) != 0) {
        return false;
    }
    char err[512] = {0};
    return hyp_wsr_resolve(f->root, f->toml, NULL, &f->ws, err, sizeof(err)) == HYP_WSR_OK &&
           f->ws.member_count == WSC_MEMBERS;
}

/* The resolved member whose canonical root ends in `dir`. The mapping from a
 * fixture directory to a slug is the resolver's, never this file's. */
static const hyp_wsr_member_t *wsc_member(const wsc_fix_t *f, const char *dir) {
    for (int i = 0; i < f->ws.member_count; i++) {
        size_t rl = strlen(f->ws.members[i].root);
        size_t dl = strlen(dir);
        if (rl > dl && strcmp(f->ws.members[i].root + (rl - dl), dir) == 0 &&
            f->ws.members[i].root[rl - dl - 1] == '/') {
            return &f->ws.members[i];
        }
    }
    return NULL;
}

static void wsc_member_db(const wsc_fix_t *f, const char *slug, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/%s.db", f->dbs, slug);
}

/* Index one member with the REAL pipeline, into its OWN database. */
static bool wsc_index(const wsc_fix_t *f, const hyp_wsr_member_t *m, int declared_members) {
    char db[1024];
    wsc_member_db(f, m->slug, db, sizeof(db));
    (void)th_unlink_force(db);
    hyp_pipeline_t *p = hyp_pipeline_new(m->root, db, HYP_MODE_FULL);
    if (!p) {
        return false;
    }
    /* A8's gate, supplied by the caller exactly as the contract in pipeline.h
     * describes — and derived here from the resolver's own member count. */
    if (declared_members > 0) {
        hyp_pipeline_set_workspace_member_count(p, declared_members);
    }
    int rc = hyp_pipeline_run(p);
    hyp_pipeline_free(p);
    return rc == 0;
}

/* Merge one indexed member into the shared workspace store. This is the step
 * unit A12 owns for real; here it is the same product code (load the member's
 * graph, merge it in) driven by the test. */
static bool wsc_merge(const wsc_fix_t *f, hyp_store_t *ws, const hyp_wsr_member_t *m) {
    char db[1024];
    wsc_member_db(f, m->slug, db, sizeof(db));
    if (hyp_store_upsert_project(ws, m->slug, m->root) != HYP_STORE_OK) {
        return false;
    }
    hyp_gbuf_t *gb = hyp_gbuf_new(m->slug, m->root);
    if (!gb) {
        return false;
    }
    bool ok = hyp_gbuf_load_from_db(gb, db, m->slug) == 0 &&
              hyp_gbuf_merge_into_store(gb, ws) == 0;
    hyp_gbuf_free(gb);
    return ok;
}

/* Index every member and assemble the one workspace store. Returns the open
 * store (caller closes) or NULL. */
static hyp_store_t *wsc_build_workspace(wsc_fix_t *f, int declared_members) {
    for (int i = 0; i < f->ws.member_count; i++) {
        if (!wsc_index(f, &f->ws.members[i], declared_members)) {
            return NULL;
        }
    }
    char err[512] = {0};
    hyp_store_t *ws = hyp_wsr_store_open(&f->ws, err, sizeof(err));
    if (!ws) {
        return NULL;
    }
    for (int i = 0; i < f->ws.member_count; i++) {
        if (!wsc_merge(f, ws, &f->ws.members[i])) {
            hyp_store_close(ws);
            return NULL;
        }
    }
    return ws;
}

/* ── Assertions over the assembled graph ─────────────────────────── */

/* Count CROSS_MEMBER_CALLS edges whose caller QN ends in `caller_tail` and
 * whose target node lives in `target_project`. -1 on a query failure. */
static int wsc_count_edges(hyp_store_t *ws, const char *caller_tail, const char *target_project,
                           char *target_qn_out, size_t target_qn_sz) {
    struct sqlite3 *db = hyp_store_get_db(ws);
    if (!db) {
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT s.qualified_name, t.qualified_name, t.project FROM edges e "
                           "JOIN nodes s ON s.id = e.source_id "
                           "JOIN nodes t ON t.id = e.target_id "
                           "WHERE e.type = '" HYP_WS_EDGE_CROSS_MEMBER "' ORDER BY e.id",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    int matched = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *sq = (const char *)sqlite3_column_text(st, 0);
        const char *tq = (const char *)sqlite3_column_text(st, 1);
        const char *tp = (const char *)sqlite3_column_text(st, 2);
        if (caller_tail && caller_tail[0]) {
            size_t sl = sq ? strlen(sq) : 0;
            size_t cl = strlen(caller_tail);
            if (sl < cl || strcmp(sq + (sl - cl), caller_tail) != 0) {
                continue;
            }
        }
        if (target_project && target_project[0] && (!tp || strcmp(tp, target_project) != 0)) {
            continue;
        }
        if (target_qn_out && target_qn_sz > 0 && matched == 0) {
            snprintf(target_qn_out, target_qn_sz, "%s", tq ? tq : "");
        }
        matched++;
    }
    sqlite3_finalize(st);
    return matched;
}

static int wsc_count_nodes_with_qn_prefix(hyp_store_t *ws, const char *prefix) {
    struct sqlite3 *db = hyp_store_get_db(ws);
    if (!db) {
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM nodes WHERE qualified_name LIKE ?1 || '%'", -1,
                           &st, NULL) != SQLITE_OK) {
        return -1;
    }
    int n = -1;
    if (sqlite3_bind_text(st, 1, prefix, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

static bool wsc_report_names(const hyp_workspace_calls_result_t *r, const char *callee) {
    for (int i = 0; i < r->reported; i++) {
        if (strcmp(r->report[i].callee, callee) == 0) {
            return true;
        }
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════
 * The rendezvous key
 * ═══════════════════════════════════════════════════════════════════ */

TEST(wsc_specifier_key_collapses_the_spellings_of_one_file) {
    char key[512];

    /* A11's Axis 2: unresolved relative specifiers reach the recording sites
     * RAW, so these three spellings of one file must not become three nodes. */
    ASSERT_TRUE(hyp_workspace_specifier_key("plugin_api.h", key, sizeof(key)));
    ASSERT_STR_EQ(key, "plugin_api.h");
    ASSERT_TRUE(hyp_workspace_specifier_key("./plugin_api.h", key, sizeof(key)));
    ASSERT_STR_EQ(key, "plugin_api.h");
    ASSERT_TRUE(hyp_workspace_specifier_key("../../plugin_api.h", key, sizeof(key)));
    ASSERT_STR_EQ(key, "plugin_api.h");

    /* Separators unify; the structure that IS meaningful survives. */
    ASSERT_TRUE(hyp_workspace_specifier_key("..\\include\\plugin_api.h", key, sizeof(key)));
    ASSERT_STR_EQ(key, "include/plugin_api.h");
    ASSERT_TRUE(hyp_workspace_specifier_key("include//plugin_api.h", key, sizeof(key)));
    ASSERT_STR_EQ(key, "include/plugin_api.h");

    /* Refused, never truncated — a truncated specifier is a well-formed
     * specifier for a different file. */
    ASSERT_FALSE(hyp_workspace_specifier_key("plugin_api.h", key, 4));
    ASSERT_STR_EQ(key, "");
    ASSERT_FALSE(hyp_workspace_specifier_key(NULL, key, sizeof(key)));
    ASSERT_FALSE(hyp_workspace_specifier_key("", key, sizeof(key)));
    ASSERT_FALSE(hyp_workspace_specifier_key("./", key, sizeof(key)));
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * The unit: the plugin case
 * ═══════════════════════════════════════════════════════════════════ */

TEST(wsc_plugin_case_crosses_the_member_boundary) {
    wsc_fix_t f;
    if (!wsc_fix_begin(&f)) {
        FAIL("fixture setup");
    }
    hyp_store_t *ws = wsc_build_workspace(&f, f.ws.member_count);
    if (!ws) {
        wsc_fix_end(&f);
        FAIL("workspace assembly");
    }

    /* The crossing left NO trace before this unit: the host's include of
     * plugin_api.h resolved to nothing in the host and neither did the call.
     * Both are recorded now, which is why there is anything to resolve. */
    ASSERT_GT(wsc_count_nodes_with_qn_prefix(ws, HYP_WS_TAG_INCLUDE), 0);
    ASSERT_GT(wsc_count_nodes_with_qn_prefix(ws, HYP_WS_TAG_EXTERN), 0);

    hyp_workspace_calls_result_t r = hyp_workspace_calls_match(ws);
    ASSERT_FALSE(r.failed);
    ASSERT_EQ(r.members, WSC_MEMBERS);

    const hyp_wsr_member_t *plugin = wsc_member(&f, "plugin");
    ASSERT_NOT_NULL(plugin);

    char target_qn[512] = {0};
    ASSERT_EQ(wsc_count_edges(ws, ".host_boot", plugin->slug, target_qn, sizeof(target_qn)), 1);
    /* The target is the DEFINITION in the other repository, reached by name —
     * plugin/src/plugin.c, not the header that declared it. */
    ASSERT_NOT_NULL(strstr(target_qn, "plugin_register"));
    ASSERT_NOT_NULL(strstr(target_qn, plugin->slug));

    /* Exactly one edge in the whole workspace. The fixture contains three
     * candidate crossings and only one of them is decidable. */
    ASSERT_EQ(wsc_count_edges(ws, NULL, NULL, NULL, 0), 1);
    ASSERT_EQ(r.edges, 1);

    hyp_store_close(ws);
    wsc_fix_end(&f);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * NEGATIVE CONTROL 1 — same symbol name, different member, not the callee
 * ═══════════════════════════════════════════════════════════════════ */

TEST(wsc_same_name_in_an_undeclared_member_is_not_a_callee) {
    wsc_fix_t f;
    if (!wsc_fix_begin(&f)) {
        FAIL("fixture setup");
    }
    hyp_store_t *ws = wsc_build_workspace(&f, f.ws.member_count);
    if (!ws) {
        wsc_fix_end(&f);
        FAIL("workspace assembly");
    }
    hyp_workspace_calls_result_t r = hyp_workspace_calls_match(ws);
    ASSERT_FALSE(r.failed);

    const hyp_wsr_member_t *widgets = wsc_member(&f, "widgets");
    ASSERT_NOT_NULL(widgets);

    /* `render` is defined exactly once in the entire workspace, at top level,
     * in `widgets`. host_draw calls it. If name equality were the matcher, this
     * would be an edge — and it must not be, because `host` includes nothing
     * from `widgets` and the two have never heard of each other. */
    ASSERT_EQ(wsc_count_edges(ws, ".host_draw", NULL, NULL, 0), 0);
    ASSERT_EQ(wsc_count_edges(ws, NULL, widgets->slug, NULL, 0), 0);

    /* And it is a refusal, not a silent miss: nothing was reported ambiguous
     * for it either, because no member ever answered. */
    ASSERT_FALSE(wsc_report_names(&r, "render"));

    hyp_store_close(ws);
    wsc_fix_end(&f);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Fail closed: two members answer, so nobody does
 * ═══════════════════════════════════════════════════════════════════ */

TEST(wsc_two_members_answering_is_reported_never_guessed) {
    wsc_fix_t f;
    if (!wsc_fix_begin(&f)) {
        FAIL("fixture setup");
    }
    hyp_store_t *ws = wsc_build_workspace(&f, f.ws.member_count);
    if (!ws) {
        wsc_fix_end(&f);
        FAIL("workspace assembly");
    }
    hyp_workspace_calls_result_t r = hyp_workspace_calls_match(ws);
    ASSERT_FALSE(r.failed);

    /* `plugin` and `bridge` both ship a shared_config.h and both define
     * config_load. The host cannot tell them apart and neither can this pass. */
    ASSERT_EQ(r.ambiguous, 1);
    ASSERT_TRUE(wsc_report_names(&r, "config_load"));
    ASSERT_EQ(wsc_count_edges(ws, ".host_configure", NULL, NULL, 0), 0);

    /* The refusal NAMES BOTH members — "a collision occurred" is not a report. */
    const hyp_wsr_member_t *plugin = wsc_member(&f, "plugin");
    const hyp_wsr_member_t *bridge = wsc_member(&f, "bridge");
    ASSERT_NOT_NULL(plugin);
    ASSERT_NOT_NULL(bridge);
    bool found = false;
    for (int i = 0; i < r.reported; i++) {
        if (strcmp(r.report[i].callee, "config_load") == 0) {
            ASSERT_NOT_NULL(strstr(r.report[i].members, plugin->slug));
            ASSERT_NOT_NULL(strstr(r.report[i].members, bridge->slug));
            found = true;
        }
    }
    ASSERT_TRUE(found);

    hyp_store_close(ws);
    wsc_fix_end(&f);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * NEGATIVE CONTROL 2 — the registry is the candidate set
 * ═══════════════════════════════════════════════════════════════════ */

TEST(wsc_registry_is_the_candidate_set) {
    wsc_fix_t f;
    if (!wsc_fix_begin(&f)) {
        FAIL("fixture setup");
    }
    hyp_store_t *ws = wsc_build_workspace(&f, f.ws.member_count);
    if (!ws) {
        wsc_fix_end(&f);
        FAIL("workspace assembly");
    }
    ASSERT_EQ(hyp_workspace_calls_match(ws).edges, 1);

    /* Take the answering member OUT of the registry and leave every node and
     * edge in place. The pass must lose the edge — which is what proves it
     * derived its candidates from hyp_store_workspace_repos() rather than from
     * whatever happened to be in the nodes table. Hardcoding a member list
     * would keep this test green, and that is the failure it exists to catch. */
    const hyp_wsr_member_t *plugin = wsc_member(&f, "plugin");
    const hyp_wsr_member_t *bridge = wsc_member(&f, "bridge");
    ASSERT_NOT_NULL(plugin);
    ASSERT_NOT_NULL(bridge);
    char sql[1024];
    snprintf(sql, sizeof(sql), "DELETE FROM workspace_repos WHERE slug = '%s';", plugin->slug);
    ASSERT_EQ(hyp_store_exec(ws, sql), HYP_STORE_OK);

    hyp_workspace_calls_result_t after = hyp_workspace_calls_match(ws);
    ASSERT_FALSE(after.failed);
    ASSERT_EQ(after.members, WSC_MEMBERS - 1);

    /* The edge is gone even though every node and every recorded crossing it
     * was built from is still sitting in the store, untouched. */
    ASSERT_EQ(wsc_count_edges(ws, ".host_boot", NULL, NULL, 0), 0);
    ASSERT_EQ(wsc_count_edges(ws, NULL, plugin->slug, NULL, 0), 0);

    /* And the derivation moves in the other direction too, which is the harder
     * half to fake: with `plugin` out of the registry, config_load stops being
     * ambiguous — `bridge` is now the only member that answers, so the call
     * that refused to resolve a moment ago resolves, to a different repo. */
    ASSERT_EQ(after.ambiguous, 0);
    ASSERT_EQ(after.edges, 1);
    char cfg_qn[512] = {0};
    ASSERT_EQ(wsc_count_edges(ws, ".host_configure", bridge->slug, cfg_qn, sizeof(cfg_qn)), 1);
    ASSERT_NOT_NULL(strstr(cfg_qn, "config_load"));

    hyp_store_close(ws);
    wsc_fix_end(&f);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * The gate: a workspace of one is byte-identical to before this unit
 * ═══════════════════════════════════════════════════════════════════ */

TEST(wsc_workspace_of_one_records_nothing) {
    wsc_fix_t f;
    if (!wsc_fix_begin(&f)) {
        FAIL("fixture setup");
    }
    const hyp_wsr_member_t *host = wsc_member(&f, "host");
    ASSERT_NOT_NULL(host);
    char db[1024];
    wsc_member_db(&f, host->slug, db, sizeof(db));

    /* Never declared at all — the state every existing caller is in. */
    ASSERT_TRUE(wsc_index(&f, host, 0));
    hyp_store_t *s = hyp_store_open_path(db);
    ASSERT_NOT_NULL(s);
    int undeclared_nodes = hyp_store_count_nodes(s, host->slug);
    ASSERT_EQ(wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_EXTERN), 0);
    ASSERT_EQ(wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_INCLUDE), 0);
    hyp_store_close(s);

    /* Declared, workspace of ONE. Same thing: there is nowhere else to look, so
     * recording every unresolvable callee would only mint a node per printf. */
    ASSERT_TRUE(wsc_index(&f, host, 1));
    s = hyp_store_open_path(db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_count_nodes(s, host->slug), undeclared_nodes);
    ASSERT_EQ(wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_EXTERN), 0);
    ASSERT_EQ(wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_INCLUDE), 0);
    hyp_store_close(s);

    /* Declared as one member of four: now, and only now, the graph grows. */
    ASSERT_TRUE(wsc_index(&f, host, WSC_MEMBERS));
    s = hyp_store_open_path(db);
    ASSERT_NOT_NULL(s);
    ASSERT_GT(hyp_store_count_nodes(s, host->slug), undeclared_nodes);
    ASSERT_GT(wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_EXTERN), 0);
    ASSERT_GT(wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_INCLUDE), 0);
    hyp_store_close(s);

    wsc_fix_end(&f);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * The two ends: sequential and parallel resolution record the same thing
 * ═══════════════════════════════════════════════════════════════════ */

TEST(wsc_both_resolution_paths_agree) {
    wsc_fix_t f;
    if (!wsc_fix_begin(&f)) {
        FAIL("fixture setup");
    }
    const hyp_wsr_member_t *host = wsc_member(&f, "host");
    ASSERT_NOT_NULL(host);
    char db[1024];
    wsc_member_db(&f, host->slug, db, sizeof(db));

    /* Under 50 files the pipeline resolves calls in pass_calls.c. */
    ASSERT_EQ(hyp_setenv("HYP_INDEX_SINGLE_THREAD", "1", 1), 0);
    ASSERT_TRUE(wsc_index(&f, host, WSC_MEMBERS));
    hyp_store_t *s = hyp_store_open_path(db);
    ASSERT_NOT_NULL(s);
    int seq_extern = wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_EXTERN);
    int seq_include = wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_INCLUDE);
    hyp_store_close(s);
    ASSERT_GT(seq_extern, 0);
    ASSERT_GT(seq_include, 0);
    (void)hyp_unsetenv("HYP_INDEX_SINGLE_THREAD");

    /* Over 50 files it resolves them in pass_parallel.c instead. The filler
     * files carry no calls and no includes, so the counts must not move: what
     * changes is which resolver runs, and that is the whole point. */
    for (int i = 0; i < WSC_FILLER_FILES; i++) {
        char path[1024];
        char body[128];
        snprintf(path, sizeof(path), "%s/src/filler_%d.c", host->root, i);
        snprintf(body, sizeof(body), "int host_filler_%d(void) { return %d; }\n", i, i);
        ASSERT_EQ(th_write_file(path, body), 0);
    }
    ASSERT_TRUE(wsc_index(&f, host, WSC_MEMBERS));
    s = hyp_store_open_path(db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_EXTERN), seq_extern);
    ASSERT_EQ(wsc_count_nodes_with_qn_prefix(s, HYP_WS_TAG_INCLUDE), seq_include);
    hyp_store_close(s);

    wsc_fix_end(&f);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Re-running is not re-writing, and "nothing to do" is not a failure
 * ═══════════════════════════════════════════════════════════════════ */

TEST(wsc_pass_is_idempotent) {
    wsc_fix_t f;
    if (!wsc_fix_begin(&f)) {
        FAIL("fixture setup");
    }
    hyp_store_t *ws = wsc_build_workspace(&f, f.ws.member_count);
    if (!ws) {
        wsc_fix_end(&f);
        FAIL("workspace assembly");
    }
    hyp_workspace_calls_result_t first = hyp_workspace_calls_match(ws);
    hyp_workspace_calls_result_t second = hyp_workspace_calls_match(ws);
    ASSERT_FALSE(second.failed);
    ASSERT_EQ(second.edges, first.edges);
    ASSERT_EQ(second.ambiguous, first.ambiguous);
    ASSERT_EQ(wsc_count_edges(ws, NULL, NULL, NULL, 0), first.edges);

    hyp_store_close(ws);
    wsc_fix_end(&f);
    PASS();
}

TEST(wsc_nothing_to_do_is_not_a_failure) {
    wsc_fix_t f;
    if (!wsc_fix_begin(&f)) {
        FAIL("fixture setup");
    }

    /* A NULL store answers zero, refuses nothing, and reports no failure. */
    hyp_workspace_calls_result_t none = hyp_workspace_calls_match(NULL);
    ASSERT_FALSE(none.failed);
    ASSERT_EQ(none.members, 0);

    /* A pre-A1 store has no registry. ABSENT means "a workspace of one" — never
     * "could not tell", and never an error. */
    hyp_store_t *legacy = hyp_store_open_path(TH_PATH(f.cache, "legacy.db"));
    ASSERT_NOT_NULL(legacy);
    ASSERT_EQ(hyp_store_upsert_project(legacy, "legacy", "/fix/legacy"), HYP_STORE_OK);
    hyp_workspace_calls_result_t pre_a1 = hyp_workspace_calls_match(legacy);
    ASSERT_FALSE(pre_a1.failed);
    ASSERT_EQ(pre_a1.members, 0);
    ASSERT_EQ(pre_a1.edges, 0);

    /* A registry with ONE member: enumerated, and then correctly left alone. */
    hyp_workspace_repo_t solo = {"legacy", "/fix/legacy", HYP_WSR_ROLE_MEMBER};
    ASSERT_EQ(hyp_store_workspace_bind(legacy, "legacy", &solo, 1, NULL, 0), HYP_STORE_OK);
    hyp_workspace_calls_result_t one = hyp_workspace_calls_match(legacy);
    ASSERT_FALSE(one.failed);
    ASSERT_EQ(one.members, 1);
    ASSERT_EQ(one.edges, 0);
    hyp_store_close(legacy);

    wsc_fix_end(&f);
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(workspace_calls) {
    RUN_TEST(wsc_specifier_key_collapses_the_spellings_of_one_file);
    RUN_TEST(wsc_plugin_case_crosses_the_member_boundary);
    RUN_TEST(wsc_same_name_in_an_undeclared_member_is_not_a_callee);
    RUN_TEST(wsc_two_members_answering_is_reported_never_guessed);
    RUN_TEST(wsc_registry_is_the_candidate_set);
    RUN_TEST(wsc_workspace_of_one_records_nothing);
    RUN_TEST(wsc_both_resolution_paths_agree);
    RUN_TEST(wsc_pass_is_idempotent);
    RUN_TEST(wsc_nothing_to_do_is_not_a_failure);
}
