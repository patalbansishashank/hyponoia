/*
 * test_g1_replay.c — anchors tested by replaying real history.
 *
 * This suite does not construct a scenario. It takes a corpus of symbol
 * lifecycles mined from this repository's own commits, and for each one it
 * REPLAYS what a user lives through:
 *
 *     materialise the tree at the BEFORE commit  (git show, blob by blob)
 *   → index it with the real pipeline
 *   → read the symbol's span out of the PRODUCT's own index
 *   → write an anchor from that span's address and content hash
 *   → advance the same working tree to the AFTER commit
 *   → re-index into the SAME database, as a working tree does
 *   → hyp_anchor_resolve, and compare the answer to what history did.
 *
 * The gold column is not authored. Every expected status comes from a commit
 * that moved, edited, renamed, relocated or copied a real function; the file
 * beside this one (tests/fixtures/g1/g1_anchor_fixtures.json) carries the
 * commit pair, the line range and the recorded hash for each, and the
 * scripts that mined it are in the run record. A fixture that cannot be
 * reconstructed is REPORTED, never dropped: a replay that quietly covers 30
 * of 36 and prints green is the exact artifact this corpus exists to prevent.
 *
 * ── THE PARSER CAVEAT, AND WHY THIS SUITE IS IMMUNE TO IT ───────────────
 *
 * The corpus extracted spans with a brace-matching C parser; the product uses
 * tree-sitter. A boundary disagreement would produce a hash mismatch that has
 * nothing to do with anchoring. So the anchor written here is ALWAYS built
 * from the product's own span, never from the corpus's line range, and the
 * assertions are about RELATIONS (resolved / edited / orphaned / candidate
 * count) rather than literal hashes. The two parsers' agreement is then
 * asserted SEPARATELY, as a pinned census — see g1_span_agreement_census.
 *
 * ── WHY THE LINE NUMBER IS THE INTERESTING NEGATIVE ─────────────────────
 *
 * One fixture renames a function without moving it: same file, same start and
 * end line, and the body below the declaration is byte-identical. A resolver
 * that consulted line numbers re-attaches there with total confidence, and
 * would attach one function's recorded reasoning to a different function.
 * g1_line_number_lane_reattaches_to_the_wrong_symbol demonstrates that on the
 * live index, and G1-RENAME-001 asserts the product refuses it.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "../src/ask/ask_embed.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/identity.h"
#include "../src/memory/anchor.h"
#include "../src/pipeline/pipeline.h"
#include "../src/store/store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson/yyjson.h>

#ifndef _WIN32

/* ── Shape of the run ────────────────────────────────────────────── */

enum {
    G1_MAX_FIX = 64,
    G1_MAX_PAIRS = 64,
    G1_MAX_FILES = 16,
    G1_QN = 512,
    G1_PATH = 512,
    G1_CANDS = 8,
    G1_HASH = HYP_ADDR_SPAN_HASH_LEN + 1,
};

typedef struct {
    const char *id;
    const char *category;
    yyjson_val *val; /* the fixture object, alive for the whole run */
    int pair;

    /* Could the before-side be rebuilt at all? */
    bool reconstructed;
    char why[256];

    /* What the PRODUCT observed at the before commit. */
    int pb_start, pb_end;
    char pb_hash[G1_HASH];
    char pb_qn[G1_QN];
    char anchor[HYP_ANCHOR_MAX + 1];

    /* What the PRODUCT observed at the after commit for the symbol the
     * corpus says this one became (same name, or the renamed one). */
    bool pa_found;
    int pa_start, pa_end;
    char pa_hash[G1_HASH];
    char pa_qn[G1_QN];
    hyp_addr_rel_t relate;

    /* Did the after side of this fixture's pair actually get replayed? A pair
     * that cannot be advanced leaves every field below at its zero value, and
     * HYP_ANCHOR_ERROR is deliberately zero, so a zeroed struct is
     * indistinguishable from a real refusal to classify. This flag is what
     * tells the two apart: absent means look at why, empty means the product
     * looked and found nothing. */
    bool replayed;

    /* The classification. */
    hyp_anchor_status_t status;
    int cand_count;
    int scan_skipped;
    char res_file[G1_PATH];
    int res_start, res_end;
    char res_reason[256];
    char cand[G1_CANDS][G1_QN];
    int cand_shown;

    /* Same after-tree indexed into a FRESH database. Only run when the
     * in-place answer disagreed, so a stale-generation defect can never be
     * mistaken for an anchoring one. */
    bool fresh_run;
    hyp_anchor_status_t fresh_status;
    int fresh_cand_count;

    /* The content-only probe the copy-paste fixtures carry. */
    bool has_probe;
    hyp_anchor_status_t probe_status;
    int probe_count;
    char probe_cand[G1_CANDS][G1_QN];
    int probe_shown;
} g1_fix_t;

static yyjson_doc *g1_doc = NULL;
static g1_fix_t g1_fix[G1_MAX_FIX];
static int g1_n = 0;
static bool g1_done = false;
static bool g1_ok = false;
static char g1_fatal[2048];
static char g1_tmp[256];
static char g1_repo[HYP_PATH_MAX];

/* Census, pinned by g1_span_agreement_census. */
static int g1_span_agree = 0, g1_span_checked = 0;
static int g1_hash_agree = 0, g1_hash_checked = 0;
static int g1_qn_agree = 0, g1_qn_checked = 0;
static int g1_after_span_agree = 0, g1_after_span_checked = 0;
static int g1_index_runs = 0;
/* The project slug the corpus wrote its qualified names against. Read from
 * the corpus, never typed here: the live slug is derived from the checkout
 * path and is a per-machine fact, so only the part after it can be compared. */
static const char *g1_corpus_slug = NULL;
/* The lineage the corpus declares its commits belong to. Read from the corpus
 * so the precondition quotes the claim it is enforcing rather than a copy of
 * it that can drift. */
static const char *g1_corpus_lineage = NULL;

/* ── JSON accessors ──────────────────────────────────────────────── */

static const char *g1_str(yyjson_val *obj, const char *key) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return (v && yyjson_is_str(v)) ? yyjson_get_str(v) : NULL;
}

static int g1_int(yyjson_val *obj, const char *key, int dflt) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return (v && yyjson_is_int(v)) ? yyjson_get_int(v) : dflt;
}

/* ── Materialising a tree from history ───────────────────────────── */

/* A commit id or repo-relative path taken from the corpus. Anything outside
 * this set is refused rather than handed to a shell — the corpus is data, and
 * data that reaches a command line is checked first. */
static bool g1_safe(const char *s) {
    if (!s || !*s) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '/')) {
            return false;
        }
    }
    return strstr(s, "..") == NULL;
}

/* Write the blob at <commit>:<rel> into <tree>/<rel>. Fails closed: a missing
 * object, an unreadable pipe or a short write all report which fixture input
 * could not be rebuilt. */
static bool g1_materialise(const char *commit, const char *rel, const char *tree, char *err,
                           size_t errsz) {
    if (!g1_safe(commit) || !g1_safe(rel)) {
        snprintf(err, errsz, "unsafe corpus token \"%s\" or \"%s\"", commit ? commit : "(null)",
                 rel ? rel : "(null)");
        return false;
    }
    char dest[HYP_PATH_MAX];
    if (snprintf(dest, sizeof(dest), "%s/%s", tree, rel) >= (int)sizeof(dest)) {
        snprintf(err, errsz, "path too long: %s/%s", tree, rel);
        return false;
    }
    char dir[HYP_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", dest);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (th_mkdir_p(dir) != 0) {
            snprintf(err, errsz, "cannot create %s", dir);
            return false;
        }
    }
    char cmd[HYP_PATH_MAX * 2];
    if (snprintf(cmd, sizeof(cmd), "git -C \"%s\" show %s:%s", g1_repo, commit, rel) >=
        (int)sizeof(cmd)) {
        snprintf(err, errsz, "command too long for %s:%s", commit, rel);
        return false;
    }
    FILE *in = hyp_popen(cmd, "r");
    if (!in) {
        snprintf(err, errsz, "cannot run git for %s:%s", commit, rel);
        return false;
    }
    FILE *out = fopen(dest, "wb");
    if (!out) {
        hyp_pclose(in);
        snprintf(err, errsz, "cannot write %s", dest);
        return false;
    }
    char buf[65536];
    size_t got, total = 0;
    bool wrote = true;
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            wrote = false;
            break;
        }
        total += got;
    }
    fclose(out);
    int rc = hyp_pclose(in);
    if (rc != 0 || !wrote || total == 0) {
        snprintf(err, errsz, "git show %s:%s failed (rc=%d, %zu bytes)", commit, rel, rc, total);
        return false;
    }
    return true;
}

/* ── Indexing ────────────────────────────────────────────────────── */

static bool g1_index(const char *tree, const char *db, char *proj, size_t projsz, char *err,
                     size_t errsz) {
    hyp_pipeline_t *p = hyp_pipeline_new(tree, db, HYP_MODE_FULL);
    if (!p) {
        snprintf(err, errsz, "pipeline_new failed for %s", tree);
        return false;
    }
    const char *name = hyp_pipeline_project_name(p);
    snprintf(proj, projsz, "%s", name ? name : "");
    int rc = hyp_pipeline_run(p);
    hyp_pipeline_free(p);
    g1_index_runs++;
    if (rc != 0) {
        snprintf(err, errsz, "pipeline_run(%s) returned %d", tree, rc);
        return false;
    }
    if (!proj[0]) {
        snprintf(err, errsz, "pipeline derived no project name for %s", tree);
        return false;
    }
    return true;
}

/* The current content hash of a node's span, read the way the product reads
 * it. Empty output means the span could not be read — never a hash. */
static bool g1_node_hash(const char *root, const hyp_node_t *n, char *out) {
    out[0] = '\0';
    char abs[HYP_PATH_MAX];
    if (!n->file_path ||
        snprintf(abs, sizeof(abs), "%s/%s", root, n->file_path) >= (int)sizeof(abs)) {
        return false;
    }
    char *text = hyp_ask_read_span_lines(abs, n->start_line, n->end_line, NULL);
    if (!text) {
        return false;
    }
    hyp_addr_span_hash(text, out);
    free(text);
    return true;
}

/* Look a symbol up by the QN the PRODUCT's own builder derives for it. */
static int g1_find(hyp_store_t *s, const char *proj, const char *rel, const char *name,
                   hyp_node_t *out, char *qn_out, size_t qn_sz) {
    char *qn = hyp_pipeline_fqn_compute(proj, rel, name);
    if (!qn) {
        return HYP_STORE_ERR;
    }
    snprintf(qn_out, qn_sz, "%s", qn);
    int rc = hyp_store_find_node_by_qn(s, proj, qn, out);
    free(qn);
    return rc;
}

/* ── The corpus's vocabulary, mapped to the contract's ───────────── */

static hyp_anchor_status_t g1_status_of(const char *name) {
    if (!name) {
        return HYP_ANCHOR_ERROR;
    }
    if (strcmp(name, "HYP_ANCHOR_RESOLVED") == 0) {
        return HYP_ANCHOR_RESOLVED;
    }
    if (strcmp(name, "HYP_ANCHOR_RESOLVED_EDITED") == 0) {
        return HYP_ANCHOR_RESOLVED_EDITED;
    }
    if (strcmp(name, "HYP_ANCHOR_ORPHANED") == 0) {
        return HYP_ANCHOR_ORPHANED;
    }
    if (strcmp(name, "HYP_ANCHOR_UNKNOWN_WORKSPACE") == 0) {
        return HYP_ANCHOR_UNKNOWN_WORKSPACE;
    }
    return HYP_ANCHOR_ERROR;
}

static hyp_addr_rel_t g1_rel_of(const char *name) {
    if (!name) {
        return HYP_ADDR_REL_INVALID;
    }
    if (strcmp(name, "HYP_ADDR_REL_SAME") == 0) {
        return HYP_ADDR_REL_SAME;
    }
    if (strcmp(name, "HYP_ADDR_REL_EDITED") == 0) {
        return HYP_ADDR_REL_EDITED;
    }
    if (strcmp(name, "HYP_ADDR_REL_CONTENT_ONLY") == 0) {
        return HYP_ADDR_REL_CONTENT_ONLY;
    }
    if (strcmp(name, "HYP_ADDR_REL_UNRELATED") == 0) {
        return HYP_ADDR_REL_UNRELATED;
    }
    return HYP_ADDR_REL_INVALID;
}

/* ── The replay ──────────────────────────────────────────────────── */

/* Every distinct commit pair in the corpus becomes one tree that is indexed,
 * advanced and re-indexed. Fixtures sharing a pair share the tree, so the
 * candidate scan sees every file any of them touches. */
typedef struct {
    const char *before;
    const char *after;
} g1_pair_t;

static g1_pair_t g1_pairs[G1_MAX_PAIRS];
static int g1_npairs = 0;
/* Why a pair stopped, kept per pair so a fixture's own, more specific reason
 * for not reconstructing is not overwritten by the pair's. */
static char g1_pair_err[G1_MAX_PAIRS][256];

static const char *g1_after_commit(yyjson_val *f) {
    yyjson_val *a = yyjson_obj_get(f, "after");
    if (a) {
        return g1_str(a, "commit");
    }
    yyjson_val *copies = yyjson_obj_get(f, "copies_after");
    if (copies && yyjson_is_arr(copies)) {
        return g1_str(yyjson_arr_get(copies, 0), "commit");
    }
    return NULL;
}

/* Collect the distinct repo-relative paths one side of a pair needs. */
static int g1_files_for(int pair, bool before, const char *out[G1_MAX_FILES]) {
    int n = 0;
    for (int i = 0; i < g1_n; i++) {
        if (g1_fix[i].pair != pair) {
            continue;
        }
        yyjson_val *f = g1_fix[i].val;
        const char *paths[G1_MAX_FILES];
        int np = 0;
        if (before) {
            paths[np++] = g1_str(yyjson_obj_get(f, "before"), "file_path");
        } else {
            yyjson_val *a = yyjson_obj_get(f, "after");
            if (a) {
                paths[np++] = g1_str(a, "file_path");
            }
            yyjson_val *copies = yyjson_obj_get(f, "copies_after");
            if (copies && yyjson_is_arr(copies)) {
                size_t idx, max;
                yyjson_val *c;
                yyjson_arr_foreach(copies, idx, max, c) {
                    if (np < G1_MAX_FILES) {
                        paths[np++] = g1_str(c, "file_path");
                    }
                }
            }
        }
        for (int k = 0; k < np; k++) {
            if (!paths[k]) {
                continue;
            }
            bool seen = false;
            for (int j = 0; j < n; j++) {
                if (strcmp(out[j], paths[k]) == 0) {
                    seen = true;
                }
            }
            if (!seen && n < G1_MAX_FILES) {
                out[n++] = paths[k];
            }
        }
    }
    return n;
}

static void g1_note_candidates(g1_fix_t *fx, const hyp_anchor_res_t *res, bool probe) {
    int shown = 0;
    for (int i = 0; i < res->candidate_count && shown < G1_CANDS; i++) {
        char line[G1_QN];
        snprintf(line, sizeof(line), "%s:%d-%d %s", res->candidates[i].file_path,
                 res->candidates[i].start_line, res->candidates[i].end_line,
                 res->candidates[i].qn);
        if (probe) {
            snprintf(fx->probe_cand[shown], G1_QN, "%s", line);
        } else {
            snprintf(fx->cand[shown], G1_QN, "%s", line);
        }
        shown++;
    }
    if (probe) {
        fx->probe_shown = shown;
    } else {
        fx->cand_shown = shown;
    }
}

/* One commit pair, end to end. */
static bool g1_run_pair(int pair, const char *tree, const char *db, char *err, size_t errsz) {
    const char *files[G1_MAX_FILES];

    /* ── the before tree ── */
    if (th_mkdir_p(tree) != 0) {
        snprintf(err, errsz, "cannot create %s", tree);
        return false;
    }
    int nf = g1_files_for(pair, true, files);
    for (int i = 0; i < nf; i++) {
        if (!g1_materialise(g1_pairs[pair].before, files[i], tree, err, errsz)) {
            return false;
        }
    }
    char proj[HYP_ADDR_SLUG_MAX + 1];
    if (!g1_index(tree, db, proj, sizeof(proj), err, errsz)) {
        return false;
    }

    /* ── write one anchor per fixture, from the product's own span ── */
    hyp_store_t *s = hyp_store_open_path_query(db);
    if (!s) {
        snprintf(err, errsz, "cannot open %s after indexing the before tree", db);
        return false;
    }
    for (int i = 0; i < g1_n; i++) {
        g1_fix_t *fx = &g1_fix[i];
        if (fx->pair != pair) {
            continue;
        }
        yyjson_val *before = yyjson_obj_get(fx->val, "before");
        const char *rel = g1_str(before, "file_path");
        const char *name = g1_str(yyjson_obj_get(fx->val, "symbol"), "before_name");
        hyp_node_t node = {0};
        int rc = g1_find(s, proj, rel, name, &node, fx->pb_qn, sizeof(fx->pb_qn));
        if (rc != HYP_STORE_OK) {
            snprintf(fx->why, sizeof(fx->why), "before index holds no node for %s in %s (rc=%d)",
                     name, rel, rc);
            continue;
        }
        fx->pb_start = node.start_line;
        fx->pb_end = node.end_line;
        bool readable = g1_node_hash(tree, &node, fx->pb_hash);
        hyp_node_free_fields(&node);
        if (!readable) {
            snprintf(fx->why, sizeof(fx->why), "span of %s in %s is unreadable", name, rel);
            continue;
        }
        hyp_addr_t addr;
        if (hyp_addr_init(&addr, NULL, proj, fx->pb_qn) != HYP_ADDR_OK ||
            !hyp_anchor_format(&addr, fx->pb_hash, fx->anchor, sizeof(fx->anchor))) {
            snprintf(fx->why, sizeof(fx->why),
                     "the product's qualified name for %s does not form an address", name);
            continue;
        }
        fx->reconstructed = true;

        /* The two parsers' agreement, counted rather than assumed. */
        g1_span_checked++;
        if (fx->pb_start == g1_int(before, "start_line", -1) &&
            fx->pb_end == g1_int(before, "end_line", -1)) {
            g1_span_agree++;
        }
        g1_hash_checked++;
        const char *corpus_hash = g1_str(before, "span_hash");
        if (corpus_hash && strcmp(corpus_hash, fx->pb_hash) == 0) {
            g1_hash_agree++;
        }
        /* The corpus wrote its qualified names against a project slug of
         * "hyponoia"; the slug is derived from the checkout path and is a
         * per-machine fact, so the comparison is on the part after it. */
        g1_qn_checked++;
        const char *corpus_qn = g1_str(before, "qualified_name");
        size_t plen = strlen(proj);
        size_t clen = strlen(g1_corpus_slug);
        if (corpus_qn && strncmp(corpus_qn, g1_corpus_slug, clen) == 0 && corpus_qn[clen] == '.' &&
            strncmp(fx->pb_qn, proj, plen) == 0 && fx->pb_qn[plen] == '.' &&
            strcmp(corpus_qn + clen + 1, fx->pb_qn + plen + 1) == 0) {
            g1_qn_agree++;
        }
    }
    hyp_store_close(s);

    /* ── advance the same working tree to the after commit ── */
    if (th_rmtree(tree) != 0 || th_mkdir_p(tree) != 0) {
        snprintf(err, errsz, "cannot replace the working tree at %s", tree);
        return false;
    }
    nf = g1_files_for(pair, false, files);
    for (int i = 0; i < nf; i++) {
        if (!g1_materialise(g1_pairs[pair].after, files[i], tree, err, errsz)) {
            return false;
        }
    }
    char proj2[HYP_ADDR_SLUG_MAX + 1];
    if (!g1_index(tree, db, proj2, sizeof(proj2), err, errsz)) {
        return false;
    }
    if (strcmp(proj, proj2) != 0) {
        snprintf(err, errsz, "the same tree indexed as \"%s\" then \"%s\"", proj, proj2);
        return false;
    }

    /* ── resolve ── */
    s = hyp_store_open_path_query(db);
    if (!s) {
        snprintf(err, errsz, "cannot open %s after re-indexing", db);
        return false;
    }
    bool any_disagreement = false;
    for (int i = 0; i < g1_n; i++) {
        g1_fix_t *fx = &g1_fix[i];
        if (fx->pair != pair || !fx->reconstructed) {
            continue;
        }
        hyp_anchor_res_t res;
        fx->replayed = true;
        fx->status = hyp_anchor_resolve(s, NULL, fx->anchor, &res);
        fx->cand_count = res.candidate_count;
        fx->scan_skipped = res.scan_skipped;
        fx->res_start = res.start_line;
        fx->res_end = res.end_line;
        snprintf(fx->res_file, sizeof(fx->res_file), "%s", res.file_path);
        snprintf(fx->res_reason, sizeof(fx->res_reason), "%s", res.reason);
        g1_note_candidates(fx, &res, false);
        hyp_anchor_res_free(&res);

        const char *want = g1_str(yyjson_obj_get(fx->val, "expected"), "hyp_anchor_status");
        if (!want || fx->status != g1_status_of(want)) {
            any_disagreement = true;
        }

        /* The identity contract's own verdict over the two observations,
         * both of them the product's. */
        yyjson_val *after = yyjson_obj_get(fx->val, "after");
        const char *aname = g1_str(yyjson_obj_get(fx->val, "symbol"), "after_name");
        fx->relate = HYP_ADDR_REL_INVALID;
        if (after && aname) {
            const char *arel = g1_str(after, "file_path");
            hyp_node_t an = {0};
            if (g1_find(s, proj, arel, aname, &an, fx->pa_qn, sizeof(fx->pa_qn)) == HYP_STORE_OK) {
                fx->pa_found = true;
                fx->pa_start = an.start_line;
                fx->pa_end = an.end_line;
                bool ok = g1_node_hash(tree, &an, fx->pa_hash);
                hyp_node_free_fields(&an);
                g1_after_span_checked++;
                if (fx->pa_start == g1_int(after, "start_line", -1) &&
                    fx->pa_end == g1_int(after, "end_line", -1)) {
                    g1_after_span_agree++;
                }
                if (ok) {
                    hyp_addr_t ba, aa;
                    if (hyp_addr_init(&ba, NULL, proj, fx->pb_qn) == HYP_ADDR_OK &&
                        hyp_addr_init(&aa, NULL, proj, fx->pa_qn) == HYP_ADDR_OK) {
                        fx->relate = hyp_addr_relate(&ba, fx->pb_hash, &aa, fx->pa_hash);
                    }
                }
            }
        }

        /* The content-only probe: an anchor carrying this content hash whose
         * qualified name is ABSENT. The copies are historical; the dead name
         * is a stand-in, and it is asserted absent before it is used. */
        yyjson_val *probe = yyjson_obj_get(yyjson_obj_get(fx->val, "expected"),
                                           "content_only_probe");
        if (probe) {
            fx->has_probe = true;
            char dead_qn[G1_QN];
            hyp_node_t dead = {0};
            int rc = g1_find(s, proj, "g1_absent_dir/g1_absent_file.c", "g1_absent_symbol", &dead,
                             dead_qn, sizeof(dead_qn));
            if (rc == HYP_STORE_OK) {
                hyp_node_free_fields(&dead);
                snprintf(fx->res_reason, sizeof(fx->res_reason),
                         "the probe's stand-in name is present in the index");
                fx->probe_count = -1;
            } else {
                hyp_addr_t da;
                char probe_anchor[HYP_ANCHOR_MAX + 1];
                if (hyp_addr_init(&da, NULL, proj, dead_qn) == HYP_ADDR_OK &&
                    hyp_anchor_format(&da, fx->pb_hash, probe_anchor, sizeof(probe_anchor))) {
                    hyp_anchor_res_t pres;
                    fx->probe_status = hyp_anchor_resolve(s, NULL, probe_anchor, &pres);
                    fx->probe_count = pres.candidate_count;
                    g1_note_candidates(fx, &pres, true);
                    hyp_anchor_res_free(&pres);
                } else {
                    fx->probe_count = -1;
                }
            }
        }
    }
    hyp_store_close(s);

    /* ── the stale-generation control ──
     * An in-place re-index that failed to retire the previous generation's
     * nodes would report a renamed symbol as still present, which looks
     * exactly like an anchoring defect. The same after tree goes into a fresh
     * database and the answer is recorded beside the first one. */
    if (any_disagreement) {
        char fresh_db[HYP_PATH_MAX];
        snprintf(fresh_db, sizeof(fresh_db), "%s.fresh.db", db);
        char fproj[HYP_ADDR_SLUG_MAX + 1];
        char ferr[256];
        if (g1_index(tree, fresh_db, fproj, sizeof(fproj), ferr, sizeof(ferr))) {
            hyp_store_t *fs = hyp_store_open_path_query(fresh_db);
            if (fs) {
                for (int i = 0; i < g1_n; i++) {
                    g1_fix_t *fx = &g1_fix[i];
                    if (fx->pair != pair || !fx->reconstructed) {
                        continue;
                    }
                    hyp_anchor_res_t res;
                    fx->fresh_status = hyp_anchor_resolve(fs, NULL, fx->anchor, &res);
                    fx->fresh_cand_count = res.candidate_count;
                    fx->fresh_run = true;
                    hyp_anchor_res_free(&res);
                }
                hyp_store_close(fs);
            }
        }
    }
    return true;
}

/* Run one git command in the corpus's repository for its exit status alone. */
static bool g1_git_ok(const char *args) {
    char cmd[HYP_PATH_MAX * 2];
    if (snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s >/dev/null 2>&1", g1_repo, args) >=
        (int)sizeof(cmd)) {
        return false;
    }
    FILE *p = hyp_popen(cmd, "r");
    if (!p) {
        return false;
    }
    return hyp_pclose(p) == 0;
}

/* The first fixture that replays a commit, so a violation names a row and not
 * just a hash. */
static const char *g1_fixture_for_commit(const char *sha) {
    for (int i = 0; i < g1_n; i++) {
        int p = g1_fix[i].pair;
        if (p >= 0 &&
            (strcmp(g1_pairs[p].before, sha) == 0 || strcmp(g1_pairs[p].after, sha) == 0)) {
            return g1_fix[i].id ? g1_fix[i].id : "(unnamed)";
        }
    }
    return "(no fixture)";
}

/*
 * THE PRECONDITION, checked against the objects rather than the apparatus.
 *
 * A git work tree is not the same thing as the commits this corpus replays.
 * Asking `rev-parse --is-inside-work-tree` checks that git is willing to talk;
 * it says nothing about whether these 28 commit pairs are reachable, and every
 * pair it cannot supply degrades into a fixture that looks classified and
 * never ran.
 *
 * Reachability is resolved against the REMOTE-TRACKING ref, deliberately. A
 * commit that lives only on an unpushed local branch is present on the machine
 * that mined the corpus and absent from every clone of it, so a check against
 * local refs passes exactly where the defect is and fails only where it lands.
 * The question this asks is the one the corpus's own lineage field declares:
 * can a clone of the remote see this commit?
 *
 * Both classes are fatal, and both are printed as they are found, because a
 * message buffer truncates and a count is always a floor.
 */
static void g1_check_lineage(void) {
    static const char *published = "refs/remotes/origin/dev";
    char args[HYP_PATH_MAX];
    int nmissing = 0, nunpub = 0;
    char first_missing[256] = {0}, first_unpub[256] = {0};

    snprintf(args, sizeof(args), "show-ref --verify --quiet %s", published);
    bool have_published = g1_git_ok(args);

    for (int p = 0; p < g1_npairs; p++) {
        const char *sides[2] = {g1_pairs[p].before, g1_pairs[p].after};
        for (int k = 0; k < 2; k++) {
            const char *sha = sides[k];
            if (!g1_safe(sha)) {
                snprintf(g1_fatal, sizeof(g1_fatal), "unsafe commit token \"%s\" in the corpus",
                         sha ? sha : "(null)");
                return;
            }
            snprintf(args, sizeof(args), "cat-file -e %s^{commit}", sha);
            if (!g1_git_ok(args)) {
                nmissing++;
                if (!first_missing[0]) {
                    snprintf(first_missing, sizeof(first_missing), "%s (%s)", sha,
                             g1_fixture_for_commit(sha));
                }
                printf("  [g1] the checkout at \"%s\" does not contain %s, replayed by %s\n",
                       g1_repo, sha, g1_fixture_for_commit(sha));
                continue;
            }
            if (!have_published) {
                continue;
            }
            snprintf(args, sizeof(args), "merge-base --is-ancestor %s %s", sha, published);
            if (!g1_git_ok(args)) {
                nunpub++;
                if (!first_unpub[0]) {
                    snprintf(first_unpub, sizeof(first_unpub), "%s (%s)", sha,
                             g1_fixture_for_commit(sha));
                }
                printf("  [g1] %s, replayed by %s, is reachable from no published branch\n", sha,
                       g1_fixture_for_commit(sha));
            }
        }
    }
    fflush(stdout);

    if (nmissing > 0) {
        snprintf(g1_fatal, sizeof(g1_fatal),
                 "%d of the commits this corpus replays are absent from the checkout at "
                 "\"%s\", the first being %s. The corpus declares \"%s\"; a venue whose clone "
                 "cannot resolve them replays fewer pairs than the corpus names, and the "
                 "fixtures in those pairs report an unclassified anchor that the product was "
                 "never asked to classify. Every absent commit is listed above.",
                 nmissing, g1_repo, first_missing,
                 g1_corpus_lineage ? g1_corpus_lineage : "no lineage");
        return;
    }
    if (!have_published) {
        snprintf(g1_fatal, sizeof(g1_fatal),
                 "%s is absent, so \"can a clone of the remote resolve this commit\" cannot be "
                 "answered here, and the corpus's declared lineage \"%s\" goes unchecked. Fetch "
                 "the remote's branches, or point HYP_G1_REPO at a checkout that tracks them.",
                 published, g1_corpus_lineage ? g1_corpus_lineage : "no lineage");
        return;
    }
    if (nunpub > 0) {
        snprintf(g1_fatal, sizeof(g1_fatal),
                 "%d of the commits this corpus replays are in this repository and reachable "
                 "from %s by no path, the first being %s. The corpus declares \"%s\". A commit "
                 "held only by a local branch replays on the machine that mined it and on no "
                 "clone of the remote, so this corpus needs a commit pair the remote publishes. "
                 "Every unreachable commit is listed above.",
                 nunpub, published, first_unpub,
                 g1_corpus_lineage ? g1_corpus_lineage : "no lineage");
        return;
    }
}

static void g1_cleanup(void) {
    if (g1_tmp[0]) {
        (void)th_rmtree(g1_tmp);
        g1_tmp[0] = '\0';
    }
    if (g1_doc) {
        yyjson_doc_free(g1_doc);
        g1_doc = NULL;
    }
}

/* Load the corpus and replay every fixture. Runs once. */
static void g1_run(void) {
    if (g1_done) {
        return;
    }
    g1_done = true;

    const char *repo = getenv("HYP_G1_REPO");
    snprintf(g1_repo, sizeof(g1_repo), "%s", (repo && *repo) ? repo : ".");
    if (strchr(g1_repo, '"')) {
        snprintf(g1_fatal, sizeof(g1_fatal), "repository path contains a quote: %s", g1_repo);
        return;
    }

    const char *corpus = getenv("HYP_G1_CORPUS");
    char corpus_path[HYP_PATH_MAX];
    snprintf(corpus_path, sizeof(corpus_path), "%s",
             (corpus && *corpus) ? corpus : "tests/fixtures/g1/g1_anchor_fixtures.json");
    FILE *cf = fopen(corpus_path, "rb");
    if (!cf) {
        snprintf(g1_fatal, sizeof(g1_fatal), "cannot open the fixture corpus at %s", corpus_path);
        return;
    }
    char *json = NULL;
    size_t jn = 0, jcap = 0;
    for (;;) {
        if (jn + 65536 + 1 > jcap) {
            jcap = jcap ? jcap * 2 : 262144;
            char *grown = realloc(json, jcap);
            if (!grown) {
                free(json);
                fclose(cf);
                snprintf(g1_fatal, sizeof(g1_fatal), "out of memory reading %s", corpus_path);
                return;
            }
            json = grown;
        }
        size_t got = fread(json + jn, 1, 65536, cf);
        jn += got;
        if (got < 65536) {
            break;
        }
    }
    fclose(cf);
    g1_doc = json ? yyjson_read(json, jn, 0) : NULL;
    free(json);
    if (!g1_doc) {
        snprintf(g1_fatal, sizeof(g1_fatal), "cannot parse the fixture corpus at %s", corpus_path);
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(g1_doc);
    yyjson_val *fixtures = root ? yyjson_obj_get(root, "fixtures") : NULL;
    if (!fixtures || !yyjson_is_arr(fixtures)) {
        snprintf(g1_fatal, sizeof(g1_fatal), "%s carries no fixtures array", corpus_path);
        return;
    }
    g1_corpus_slug = g1_str(root, "project_slug_assumed");
    if (!g1_corpus_slug || !*g1_corpus_slug) {
        snprintf(g1_fatal, sizeof(g1_fatal), "%s does not state the slug its names assume",
                 corpus_path);
        return;
    }
    g1_corpus_lineage = g1_str(root, "lineage");
    if (!g1_corpus_lineage || !*g1_corpus_lineage) {
        snprintf(g1_fatal, sizeof(g1_fatal),
                 "%s names commits and declares no lineage for them, so nothing can check that "
                 "a clone of the remote can resolve any of them",
                 corpus_path);
        return;
    }

    /* History is a PRECONDITION, not an option. A venue that checked out one
     * commit cannot replay 28 pairs, and reporting green there would claim a
     * gate that never ran. */
    char probe_cmd[HYP_PATH_MAX * 2];
    snprintf(probe_cmd, sizeof(probe_cmd), "git -C \"%s\" rev-parse --is-inside-work-tree", g1_repo);
    FILE *probe = popen(probe_cmd, "r");
    char line[64] = {0};
    bool in_repo = probe && fgets(line, sizeof(line), probe) && strncmp(line, "true", 4) == 0;
    if (probe) {
        pclose(probe);
    }
    if (!in_repo) {
        snprintf(g1_fatal, sizeof(g1_fatal),
                 "no git work tree at \"%s\": the replay needs this repository's history "
                 "(set HYP_G1_REPO, or check out with full history)",
                 g1_repo);
        return;
    }

    size_t idx, max;
    yyjson_val *f;
    yyjson_arr_foreach(fixtures, idx, max, f) {
        if (g1_n >= G1_MAX_FIX) {
            snprintf(g1_fatal, sizeof(g1_fatal), "corpus carries more than %d fixtures",
                     G1_MAX_FIX);
            return;
        }
        g1_fix_t *fx = &g1_fix[g1_n];
        memset(fx, 0, sizeof(*fx));
        fx->val = f;
        fx->id = g1_str(f, "id");
        fx->category = g1_str(f, "category");
        const char *bc = g1_str(yyjson_obj_get(f, "before"), "commit");
        const char *ac = g1_after_commit(f);
        if (!fx->id || !bc || !ac) {
            snprintf(g1_fatal, sizeof(g1_fatal), "fixture %d carries no id or no commit pair",
                     (int)idx);
            return;
        }
        fx->pair = -1;
        for (int i = 0; i < g1_npairs; i++) {
            if (strcmp(g1_pairs[i].before, bc) == 0 && strcmp(g1_pairs[i].after, ac) == 0) {
                fx->pair = i;
            }
        }
        if (fx->pair < 0) {
            if (g1_npairs >= G1_MAX_PAIRS) {
                snprintf(g1_fatal, sizeof(g1_fatal), "corpus carries more than %d commit pairs",
                         G1_MAX_PAIRS);
                return;
            }
            g1_pairs[g1_npairs].before = bc;
            g1_pairs[g1_npairs].after = ac;
            fx->pair = g1_npairs++;
        }
        snprintf(fx->why, sizeof(fx->why), "not replayed");
        g1_n++;
    }

    g1_check_lineage();
    if (g1_fatal[0]) {
        return;
    }

    char *tmp = th_mktempdir("hyp_g1");
    if (!tmp) {
        snprintf(g1_fatal, sizeof(g1_fatal), "cannot create a scratch directory");
        return;
    }
    snprintf(g1_tmp, sizeof(g1_tmp), "%s", tmp);
    atexit(g1_cleanup);

    /* The pipeline writes a per-run log beside whatever cache directory it
     * resolves. This run makes 56 of them, so it points the cache at its own
     * scratch and puts it back — a test must not leave litter in the
     * directory a live daemon is serving from. */
    const char *prev_cache = getenv("HYP_CACHE_DIR");
    char saved_cache[HYP_PATH_MAX];
    bool had_cache = prev_cache != NULL;
    if (had_cache) {
        snprintf(saved_cache, sizeof(saved_cache), "%s", prev_cache);
    }
    char cache[HYP_PATH_MAX];
    snprintf(cache, sizeof(cache), "%s/cache", g1_tmp);
    if (th_mkdir_p(cache) == 0) {
        setenv("HYP_CACHE_DIR", cache, 1);
    }

    printf("\n  [g1] replaying %d fixtures over %d commit pairs from %s\n", g1_n, g1_npairs,
           g1_repo);
    fflush(stdout);
    for (int p = 0; p < g1_npairs; p++) {
        char tree[HYP_PATH_MAX], db[HYP_PATH_MAX], err[256] = {0};
        snprintf(tree, sizeof(tree), "%s/p%02d/r", g1_tmp, p);
        snprintf(db, sizeof(db), "%s/p%02d/idx.db", g1_tmp, p);
        if (!g1_run_pair(p, tree, db, err, sizeof(err))) {
            snprintf(g1_pair_err[p], sizeof(g1_pair_err[p]), "%s", err);
            for (int i = 0; i < g1_n; i++) {
                if (g1_fix[i].pair == p && !g1_fix[i].reconstructed) {
                    snprintf(g1_fix[i].why, sizeof(g1_fix[i].why), "%s", err);
                }
            }
            printf("  [g1] pair %02d could not be replayed: %s\n", p, err);
        }
        (void)th_rmtree(tree);
    }
    if (had_cache) {
        setenv("HYP_CACHE_DIR", saved_cache, 1);
    } else {
        unsetenv("HYP_CACHE_DIR");
    }
    g1_ok = true;
}

/* ── Verdicts ────────────────────────────────────────────────────── */

static char g1_msg[1024];

static g1_fix_t *g1_by_id(const char *id) {
    for (int i = 0; i < g1_n; i++) {
        if (g1_fix[i].id && strcmp(g1_fix[i].id, id) == 0) {
            return &g1_fix[i];
        }
    }
    return NULL;
}

/* NULL when the replay agrees with history; otherwise what disagreed. */
static const char *g1_verdict(const char *id) {
    g1_run();
    if (g1_fatal[0]) {
        snprintf(g1_msg, sizeof(g1_msg), "the replay could not run: %s", g1_fatal);
        return g1_msg;
    }
    g1_fix_t *fx = g1_by_id(id);
    if (!fx) {
        snprintf(g1_msg, sizeof(g1_msg), "%s is not in the corpus", id);
        return g1_msg;
    }
    if (!fx->reconstructed) {
        snprintf(g1_msg, sizeof(g1_msg), "%s could not be reconstructed: %s", id, fx->why);
        return g1_msg;
    }
    /* Nothing below this point is the product's answer unless the after tree
     * was indexed and hyp_anchor_resolve actually ran. Reporting a zeroed
     * struct as a classification prints a refusal the product never made. */
    if (!fx->replayed) {
        snprintf(g1_msg, sizeof(g1_msg),
                 "%s: the after side of commit pair %d never replayed, so no anchor was "
                 "resolved for it and the fields below carry no answer — %s",
                 id, fx->pair,
                 g1_pair_err[fx->pair][0] ? g1_pair_err[fx->pair] : "no reason recorded");
        return g1_msg;
    }
    yyjson_val *want = yyjson_obj_get(fx->val, "expected");
    hyp_anchor_status_t want_status = g1_status_of(g1_str(want, "hyp_anchor_status"));
    if (fx->status != want_status) {
        snprintf(g1_msg, sizeof(g1_msg),
                 "%s: history says %s, the product says %s (%s) at %s:%d-%d%s%s", id,
                 hyp_anchor_status_str(want_status), hyp_anchor_status_str(fx->status),
                 fx->res_reason, fx->res_file, fx->res_start, fx->res_end,
                 fx->fresh_run ? " | fresh database says " : "",
                 fx->fresh_run ? hyp_anchor_status_str(fx->fresh_status) : "");
        return g1_msg;
    }
    if (want_status == HYP_ANCHOR_ORPHANED) {
        int want_cands = g1_int(want, "candidate_count", -1);
        if (want_cands >= 0 && fx->cand_count != want_cands) {
            snprintf(g1_msg, sizeof(g1_msg),
                     "%s: orphaned as history says, but with %d candidates where history has "
                     "%d (skipped %d)%s%s",
                     id, fx->cand_count, want_cands, fx->scan_skipped,
                     fx->cand_shown ? " first: " : "", fx->cand_shown ? fx->cand[0] : "");
            return g1_msg;
        }
        if (fx->scan_skipped != 0) {
            snprintf(g1_msg, sizeof(g1_msg),
                     "%s: the candidate scan skipped %d spans, so %d candidates is a floor and "
                     "the count cannot be asserted",
                     id, fx->scan_skipped, fx->cand_count);
            return g1_msg;
        }
    } else {
        /* Resolved either way: candidates are gathered only when orphaned. */
        if (fx->cand_count != 0) {
            snprintf(g1_msg, sizeof(g1_msg), "%s: %s carries %d candidates", id,
                     hyp_anchor_status_str(fx->status), fx->cand_count);
            return g1_msg;
        }
        yyjson_val *to = yyjson_obj_get(want, "resolved_to");
        if (to) {
            const char *file = g1_str(to, "file_path");
            if (file && strcmp(file, fx->res_file) != 0) {
                snprintf(g1_msg, sizeof(g1_msg), "%s: resolved into %s, history says %s", id,
                         fx->res_file, file);
                return g1_msg;
            }
        }
    }
    /* The identity contract's own verdict over the two observations, both of
     * them the product's own spans. Only checked where the symbol history
     * says it became is actually in the after index — the copy fixtures name
     * several successors rather than one, and their assertion is the probe. */
    hyp_addr_rel_t want_rel = g1_rel_of(g1_str(want, "hyp_addr_relate"));
    if (fx->pa_found && want_rel != HYP_ADDR_REL_INVALID && fx->relate != want_rel) {
        snprintf(g1_msg, sizeof(g1_msg),
                 "%s: history relates the two observations %s, the contract says %s", id,
                 hyp_addr_rel_str(want_rel), hyp_addr_rel_str(fx->relate));
        return g1_msg;
    }
    return NULL;
}

#define G1_REPLAY(id)                        \
    do {                                     \
        const char *_m = g1_verdict(id);     \
        if (_m) {                            \
            FAIL(_m);                        \
        }                                    \
    } while (0)

/* ════════════════════════════════════════════════════════════════════
 * The run itself, and what it covers
 * ════════════════════════════════════════════════════════════════════ */

TEST(g1_every_fixture_was_replayed_from_real_history) {
    g1_run();
    if (g1_fatal[0]) {
        FAIL(g1_fatal);
    }
    int by_cat[6] = {0};
    int rebuilt = 0, replayed = 0;
    for (int i = 0; i < g1_n; i++) {
        if (g1_fix[i].reconstructed) {
            rebuilt++;
        } else {
            printf("\n    UNRECONSTRUCTIBLE %s: %s", g1_fix[i].id, g1_fix[i].why);
        }
        /* Rebuilding the before side is half a replay. A pair that stops
         * before its after tree is indexed leaves the fixture holding a zeroed
         * result, which the contract defines as no classification and which
         * reads downstream as one fewer span checked. Counting the second half
         * separately is what names the pair instead of the count. */
        if (g1_fix[i].replayed) {
            replayed++;
        } else if (g1_fix[i].reconstructed) {
            printf("\n    NOT REPLAYED %s: pair %d stopped — %s", g1_fix[i].id, g1_fix[i].pair,
                   g1_pair_err[g1_fix[i].pair][0] ? g1_pair_err[g1_fix[i].pair]
                                                  : "no reason recorded");
        }
        const char *c = g1_fix[i].category;
        if (!c) {
            continue;
        }
        by_cat[strcmp(c, "moved_within_file") == 0          ? 0
               : strcmp(c, "body_changed") == 0             ? 1
               : strcmp(c, "renamed") == 0                  ? 2
               : strcmp(c, "cross_directory_move") == 0     ? 3
               : strcmp(c, "copy_paste") == 0               ? 4
                                                            : 5]++;
    }
    printf("\n    %d fixtures: moved %d, edited %d, renamed %d, cross-dir %d, copied %d, "
           "file-renamed %d; %d rebuilt, %d replayed, %d index runs\n    ",
           g1_n, by_cat[0], by_cat[1], by_cat[2], by_cat[3], by_cat[4], by_cat[5], rebuilt,
           replayed, g1_index_runs);
    ASSERT_EQ(g1_n, 36);
    ASSERT_EQ(rebuilt, 36);
    ASSERT_EQ(replayed, 36);
    ASSERT_EQ(g1_npairs, 28);
    ASSERT_TRUE(g1_ok);
    PASS();
}

/*
 * The parser caveat, measured rather than assumed. The corpus's spans come
 * from a brace-matching C parser and the product's from tree-sitter; nothing
 * guarantees they agree, and where they do not, a hash comparison would be
 * measuring the parsers rather than the anchor. The replay never depends on
 * this — it builds every anchor from the product's own span — so this census
 * is a FINDING, pinned like a characterization count. A boundary that moves
 * makes it fail and names the count; the exit condition is that it keeps
 * agreeing, and a disagreement is a real difference worth reading.
 */
TEST(g1_span_agreement_census) {
    g1_run();
    if (g1_fatal[0]) {
        FAIL(g1_fatal);
    }
    printf("\n    before spans %d/%d, hashes %d/%d, qualified names %d/%d, after spans %d/%d\n    ",
           g1_span_agree, g1_span_checked, g1_hash_agree, g1_hash_checked, g1_qn_agree,
           g1_qn_checked, g1_after_span_agree, g1_after_span_checked);
    /* A census counts what it was shown. If a pair stopped before its after
     * tree, the after-span totals below are short by exactly the fixtures in
     * that pair, and the difference is a replay that did not happen rather
     * than two parsers disagreeing. Say which, so the number is never the
     * first thing a reader has to interpret. */
    for (int i = 0; i < g1_n; i++) {
        if (g1_fix[i].reconstructed && !g1_fix[i].replayed) {
            snprintf(g1_msg, sizeof(g1_msg),
                     "the after-span census is short because commit pair %d never replayed, "
                     "taking %s with it: %s",
                     g1_fix[i].pair, g1_fix[i].id ? g1_fix[i].id : "(unnamed)",
                     g1_pair_err[g1_fix[i].pair][0] ? g1_pair_err[g1_fix[i].pair]
                                                    : "no reason recorded");
            FAIL(g1_msg);
        }
    }
    ASSERT_EQ(g1_span_checked, 36);
    ASSERT_EQ(g1_span_agree, 36);
    ASSERT_EQ(g1_hash_agree, 36);
    ASSERT_EQ(g1_qn_agree, 36);
    ASSERT_EQ(g1_after_span_checked, 31);
    ASSERT_EQ(g1_after_span_agree, 31);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Moved within a file — the same symbol, somewhere else in the file
 * ════════════════════════════════════════════════════════════════════ */

TEST(g1_move_001_handle_query_graph_slid_275_lines) {
    G1_REPLAY("G1-MOVE-001");
    g1_fix_t *fx = g1_by_id("G1-MOVE-001");
    /* The displacement is the point: the answer's line numbers are not the
     * ones the anchor was written at, and the classification did not move. */
    ASSERT_TRUE(fx->res_start != fx->pb_start);
    ASSERT_EQ(fx->res_start - fx->pb_start, 275);
    ASSERT_EQ(fx->relate, HYP_ADDR_REL_SAME);
    PASS();
}

TEST(g1_move_002_release_request_store) {
    G1_REPLAY("G1-MOVE-002");
    PASS();
}

TEST(g1_move_003_adr_list_sections_from_content) {
    G1_REPLAY("G1-MOVE-003");
    PASS();
}

TEST(g1_move_004_check_inline_props) {
    G1_REPLAY("G1-MOVE-004");
    PASS();
}

TEST(g1_move_005_mcp_cross_repo_create_project_store) {
    G1_REPLAY("G1-MOVE-005");
    PASS();
}

TEST(g1_move_006_json_str_at) {
    G1_REPLAY("G1-MOVE-006");
    PASS();
}

TEST(g1_move_007_cbm_run_c_lsp_cross) {
    G1_REPLAY("G1-MOVE-007");
    PASS();
}

TEST(g1_move_008_daemon_runtime_client_application_request) {
    G1_REPLAY("G1-MOVE-008");
    PASS();
}

/* Every moved fixture at once: the line numbers in the answer differ from the
 * line numbers the anchor was written at, and every one is still attached. */
TEST(g1_moves_all_slid_and_all_stayed_attached) {
    g1_run();
    if (g1_fatal[0]) {
        FAIL(g1_fatal);
    }
    int moved = 0, slid = 0;
    for (int i = 0; i < g1_n; i++) {
        g1_fix_t *fx = &g1_fix[i];
        if (!fx->category || strcmp(fx->category, "moved_within_file") != 0) {
            continue;
        }
        moved++;
        ASSERT_EQ(fx->status, HYP_ANCHOR_RESOLVED);
        ASSERT_EQ(fx->relate, HYP_ADDR_REL_SAME);
        if (fx->res_start != fx->pb_start) {
            slid++;
        }
    }
    ASSERT_EQ(moved, 8);
    ASSERT_EQ(slid, 8);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Body changed — still attached, and visibly edited
 * ════════════════════════════════════════════════════════════════════ */

TEST(g1_edit_001_write_diagnostics_edited_in_place) {
    G1_REPLAY("G1-EDIT-001");
    g1_fix_t *fx = g1_by_id("G1-EDIT-001");
    /* Edited in place: the start line did not move, so nothing but the
     * content could have told the two states apart. */
    ASSERT_EQ(fx->res_start, fx->pb_start);
    ASSERT_EQ(fx->relate, HYP_ADDR_REL_EDITED);
    PASS();
}

TEST(g1_edit_002_hyp_is_test_file) {
    G1_REPLAY("G1-EDIT-002");
    PASS();
}

TEST(g1_edit_003_handle_search_graph) {
    G1_REPLAY("G1-EDIT-003");
    PASS();
}

TEST(g1_edit_004_cbm_mcp_text_result) {
    G1_REPLAY("G1-EDIT-004");
    PASS();
}

TEST(g1_edit_005_application_mcp_request) {
    G1_REPLAY("G1-EDIT-005");
    PASS();
}

TEST(g1_edit_006_cbm_mem_init_with_cap) {
    G1_REPLAY("G1-EDIT-006");
    PASS();
}

TEST(g1_edit_007_ask_llama_open) {
    G1_REPLAY("G1-EDIT-007");
    PASS();
}

/* The sharpest discriminator in the corpus: an edit that also slides the
 * symbol by exactly one line. A resolver keyed on position would call it
 * moved; a resolver keyed on content alone would call it lost. */
TEST(g1_edit_008_handle_ask_edited_while_sliding_one_line) {
    G1_REPLAY("G1-EDIT-008");
    g1_fix_t *fx = g1_by_id("G1-EDIT-008");
    ASSERT_EQ(fx->res_start - fx->pb_start, 1);
    ASSERT_EQ(fx->status, HYP_ANCHOR_RESOLVED_EDITED);
    ASSERT_EQ(fx->relate, HYP_ADDR_REL_EDITED);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Renamed — orphaned, with the wrong answers named
 * ════════════════════════════════════════════════════════════════════ */

/*
 * THE TRAP. This rename does not move the function: same file, same start
 * line, same end line, and every line below the declaration byte-identical.
 * The renamed function sits exactly where the anchored one sat. A resolver
 * that consulted the position re-attaches here with total confidence and
 * reports RESOLVED — see g1_line_number_lane_reattaches_to_the_wrong_symbol,
 * which does exactly that against this same index.
 */
TEST(g1_rename_001_same_file_same_lines_must_orphan) {
    G1_REPLAY("G1-RENAME-001");
    g1_fix_t *fx = g1_by_id("G1-RENAME-001");
    ASSERT_EQ(fx->status, HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(fx->cand_count, 0);
    /* The named wrong answer is in the index, at the anchor's own position. */
    ASSERT_TRUE(fx->pa_found);
    ASSERT_EQ(fx->pa_start, fx->pb_start);
    ASSERT_EQ(fx->pa_end, fx->pb_end);
    /* The declaration line is inside the span, so the rename changed the
     * content too: neither address nor content relates the two. */
    ASSERT_EQ(fx->relate, HYP_ADDR_REL_UNRELATED);
    PASS();
}

TEST(g1_rename_002_pxc_collect_all_defs) {
    G1_REPLAY("G1-RENAME-002");
    PASS();
}

TEST(g1_rename_003_resolve_func_name) {
    G1_REPLAY("G1-RENAME-003");
    PASS();
}

TEST(g1_rename_004_cpp_out_of_line_parent_class) {
    G1_REPLAY("G1-RENAME-004");
    PASS();
}

TEST(g1_rename_005_ha_read_stdin) {
    G1_REPLAY("G1-RENAME-005");
    PASS();
}

/* The plausible-neighbour trap: the same commit adds three more near-identical
 * functions beside the renamed one. Four plausible answers, all wrong. */
TEST(g1_rename_006_four_plausible_neighbours_none_taken) {
    G1_REPLAY("G1-RENAME-006");
    g1_fix_t *fx = g1_by_id("G1-RENAME-006");
    ASSERT_EQ(fx->status, HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(fx->cand_count, 0);
    ASSERT_TRUE(fx->pa_found);
    ASSERT_EQ(fx->relate, HYP_ADDR_REL_UNRELATED);
    PASS();
}

TEST(g1_rename_007_activation_transaction_refusal_note) {
    G1_REPLAY("G1-RENAME-007");
    PASS();
}

TEST(g1_rename_008_louvain_free_adj_renamed_edited_and_moved_at_once) {
    G1_REPLAY("G1-RENAME-008");
    PASS();
}

/* No rename in the corpus re-attaches to anything, and none of them resolves.
 * The named wrong answer is present in the index in every case, which is what
 * makes the refusal meaningful. */
TEST(g1_renames_all_orphaned_with_the_wrong_answer_available) {
    g1_run();
    if (g1_fatal[0]) {
        FAIL(g1_fatal);
    }
    int renamed = 0, wrong_answer_present = 0;
    for (int i = 0; i < g1_n; i++) {
        g1_fix_t *fx = &g1_fix[i];
        if (!fx->category || strcmp(fx->category, "renamed") != 0) {
            continue;
        }
        renamed++;
        ASSERT_EQ(fx->status, HYP_ANCHOR_ORPHANED);
        ASSERT_EQ(fx->cand_count, 0);
        ASSERT_EQ(fx->scan_skipped, 0);
        if (fx->pa_found) {
            wrong_answer_present++;
            ASSERT_EQ(fx->relate, HYP_ADDR_REL_UNRELATED);
        }
    }
    ASSERT_EQ(renamed, 8);
    ASSERT_EQ(wrong_answer_present, 8);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Relocated — orphaned, with exactly one candidate carrying its evidence
 * ════════════════════════════════════════════════════════════════════ */

TEST(g1_crossdir_001_py_invalidate_possible_bindings) {
    G1_REPLAY("G1-CROSSDIR-001");
    g1_fix_t *fx = g1_by_id("G1-CROSSDIR-001");
    ASSERT_EQ(fx->status, HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(fx->cand_count, 1);
    /* The candidate is the relocated function, and the identity contract
     * calls the pair content-only: evidence, not a re-attachment. */
    ASSERT_EQ(fx->relate, HYP_ADDR_REL_CONTENT_ONLY);
    PASS();
}

TEST(g1_crossdir_002_macro_consume_fragment) {
    G1_REPLAY("G1-CROSSDIR-002");
    PASS();
}

TEST(g1_crossdir_003_has_filesystem_extension) {
    G1_REPLAY("G1-CROSSDIR-003");
    PASS();
}

TEST(g1_crossdir_004_ks_build) {
    G1_REPLAY("G1-CROSSDIR-004");
    PASS();
}

TEST(g1_crossdir_005_emit_property) {
    G1_REPLAY("G1-CROSSDIR-005");
    PASS();
}

TEST(g1_filerename_001_ts_nstack_push) {
    G1_REPLAY("G1-FILERENAME-001");
    g1_fix_t *fx = g1_by_id("G1-FILERENAME-001");
    ASSERT_EQ(fx->cand_count, 1);
    ASSERT_EQ(fx->relate, HYP_ADDR_REL_CONTENT_ONLY);
    PASS();
}

TEST(g1_filerename_002_ts_nstack_init) {
    G1_REPLAY("G1-FILERENAME-002");
    PASS();
}

/* A relocation and a file rename are the same observation, deliberately: the
 * contract does not claim to tell them apart, and neither does this. */
TEST(g1_relocations_are_one_candidate_and_never_a_reattachment) {
    g1_run();
    if (g1_fatal[0]) {
        FAIL(g1_fatal);
    }
    int relocated = 0;
    for (int i = 0; i < g1_n; i++) {
        g1_fix_t *fx = &g1_fix[i];
        if (!fx->category || (strcmp(fx->category, "cross_directory_move") != 0 &&
                              strcmp(fx->category, "file_rename_same_directory") != 0)) {
            continue;
        }
        relocated++;
        ASSERT_EQ(fx->status, HYP_ANCHOR_ORPHANED);
        ASSERT_EQ(fx->cand_count, 1);
        ASSERT_EQ(fx->scan_skipped, 0);
        ASSERT_EQ(fx->relate, HYP_ADDR_REL_CONTENT_ONLY);
    }
    ASSERT_EQ(relocated, 7);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Copy-paste — the original keeps its anchor; the content lane is ambiguous
 * ════════════════════════════════════════════════════════════════════ */

TEST(g1_copy_001_one_address_becomes_three_and_the_original_still_resolves) {
    G1_REPLAY("G1-COPY-001");
    g1_fix_t *fx = g1_by_id("G1-COPY-001");
    ASSERT_EQ(fx->status, HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(fx->cand_count, 0);
    /* And the content lane alone is three-way ambiguous over the same tree:
     * an anchor whose name is gone gets all three and attaches to none. */
    ASSERT_TRUE(fx->has_probe);
    ASSERT_EQ(fx->probe_status, HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(fx->probe_count, 3);
    PASS();
}

TEST(g1_copy_002_one_address_becomes_two) {
    G1_REPLAY("G1-COPY-002");
    g1_fix_t *fx = g1_by_id("G1-COPY-002");
    ASSERT_EQ(fx->status, HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(fx->probe_status, HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(fx->probe_count, 2);
    PASS();
}

TEST(g1_copy_003_suite_requested) {
    G1_REPLAY("G1-COPY-003");
    PASS();
}

TEST(g1_copy_004_single_file_battery) {
    G1_REPLAY("G1-COPY-004");
    PASS();
}

TEST(g1_copy_005_pipeline_battery) {
    G1_REPLAY("G1-COPY-005");
    PASS();
}

/* Every copy fixture at once: symbol identity first is what keeps a live
 * anchor unambiguous while its content sits at two or three addresses. */
TEST(g1_copies_never_make_a_live_anchor_ambiguous) {
    g1_run();
    if (g1_fatal[0]) {
        FAIL(g1_fatal);
    }
    int copies = 0, probed = 0, multiplicity = 0;
    for (int i = 0; i < g1_n; i++) {
        g1_fix_t *fx = &g1_fix[i];
        if (!fx->category || strcmp(fx->category, "copy_paste") != 0) {
            continue;
        }
        copies++;
        ASSERT_EQ(fx->status, HYP_ANCHOR_RESOLVED);
        ASSERT_EQ(fx->cand_count, 0);
        if (fx->has_probe) {
            probed++;
            yyjson_val *probe =
                yyjson_obj_get(yyjson_obj_get(fx->val, "expected"), "content_only_probe");
            ASSERT_EQ(fx->probe_status, HYP_ANCHOR_ORPHANED);
            ASSERT_EQ(fx->probe_count, g1_int(probe, "expected_candidate_count", -1));
            ASSERT_TRUE(fx->probe_count >= 2);
            multiplicity += fx->probe_count;
        }
    }
    ASSERT_EQ(copies, 5);
    ASSERT_EQ(probed, 5);
    /* Two of the five went one address to three, three went one to two. */
    ASSERT_EQ(multiplicity, 12);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The negative control, run against the live index
 * ════════════════════════════════════════════════════════════════════ */

/*
 * What a resolver keyed on position would have answered, executed rather than
 * described. The trap fixture's renamed function occupies the anchored
 * function's exact lines in the exact file, so a lookup by position returns a
 * node — a DIFFERENT symbol — and every check such a resolver could make
 * passes: the file matches, the start line matches, the end line matches.
 *
 * The product refuses it, and the two answers are asserted side by side so
 * that "orphaned" cannot be mistaken for the resolver failing to look.
 */
TEST(g1_line_number_lane_reattaches_to_the_wrong_symbol) {
    g1_run();
    if (g1_fatal[0]) {
        FAIL(g1_fatal);
    }
    g1_fix_t *fx = g1_by_id("G1-RENAME-001");
    if (!fx || !fx->reconstructed) {
        FAIL("the trap fixture was not replayed");
    }
    /* What a position-keyed resolver had to go on, and what it found. */
    ASSERT_TRUE(fx->pa_found);
    ASSERT_EQ(fx->pa_start, fx->pb_start);
    ASSERT_EQ(fx->pa_end, fx->pb_end);
    /* It would have re-attached to a different qualified name. */
    ASSERT_TRUE(strcmp(fx->pa_qn, fx->pb_qn) != 0);
    /* The product, given the same tree, refuses — and does not soften the
     * refusal into a candidate either. */
    ASSERT_EQ(fx->status, HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(fx->cand_count, 0);
    printf("\n    position lane would attach %s to %s at the same lines %d-%d\n    ", fx->pb_qn,
           fx->pa_qn, fx->pa_start, fx->pa_end);
    PASS();
}

#else /* _WIN32 */

/* The replay drives git through a pipe and rebuilds working trees by path.
 * The git-shelling suites in this tree take the same exit on Windows. */
TEST(g1_replay_is_posix_only) {
    SKIP_PLATFORM("the replay shells out to git to rebuild historical trees");
}

#endif /* _WIN32 */

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(g1_replay) {
#ifdef _WIN32
    RUN_TEST(g1_replay_is_posix_only);
#else
    RUN_TEST(g1_every_fixture_was_replayed_from_real_history);
    RUN_TEST(g1_span_agreement_census);

    RUN_TEST(g1_move_001_handle_query_graph_slid_275_lines);
    RUN_TEST(g1_move_002_release_request_store);
    RUN_TEST(g1_move_003_adr_list_sections_from_content);
    RUN_TEST(g1_move_004_check_inline_props);
    RUN_TEST(g1_move_005_mcp_cross_repo_create_project_store);
    RUN_TEST(g1_move_006_json_str_at);
    RUN_TEST(g1_move_007_cbm_run_c_lsp_cross);
    RUN_TEST(g1_move_008_daemon_runtime_client_application_request);
    RUN_TEST(g1_moves_all_slid_and_all_stayed_attached);

    RUN_TEST(g1_edit_001_write_diagnostics_edited_in_place);
    RUN_TEST(g1_edit_002_hyp_is_test_file);
    RUN_TEST(g1_edit_003_handle_search_graph);
    RUN_TEST(g1_edit_004_cbm_mcp_text_result);
    RUN_TEST(g1_edit_005_application_mcp_request);
    RUN_TEST(g1_edit_006_cbm_mem_init_with_cap);
    RUN_TEST(g1_edit_007_ask_llama_open);
    RUN_TEST(g1_edit_008_handle_ask_edited_while_sliding_one_line);

    RUN_TEST(g1_rename_001_same_file_same_lines_must_orphan);
    RUN_TEST(g1_rename_002_pxc_collect_all_defs);
    RUN_TEST(g1_rename_003_resolve_func_name);
    RUN_TEST(g1_rename_004_cpp_out_of_line_parent_class);
    RUN_TEST(g1_rename_005_ha_read_stdin);
    RUN_TEST(g1_rename_006_four_plausible_neighbours_none_taken);
    RUN_TEST(g1_rename_007_activation_transaction_refusal_note);
    RUN_TEST(g1_rename_008_louvain_free_adj_renamed_edited_and_moved_at_once);
    RUN_TEST(g1_renames_all_orphaned_with_the_wrong_answer_available);

    RUN_TEST(g1_crossdir_001_py_invalidate_possible_bindings);
    RUN_TEST(g1_crossdir_002_macro_consume_fragment);
    RUN_TEST(g1_crossdir_003_has_filesystem_extension);
    RUN_TEST(g1_crossdir_004_ks_build);
    RUN_TEST(g1_crossdir_005_emit_property);
    RUN_TEST(g1_filerename_001_ts_nstack_push);
    RUN_TEST(g1_filerename_002_ts_nstack_init);
    RUN_TEST(g1_relocations_are_one_candidate_and_never_a_reattachment);

    RUN_TEST(g1_copy_001_one_address_becomes_three_and_the_original_still_resolves);
    RUN_TEST(g1_copy_002_one_address_becomes_two);
    RUN_TEST(g1_copy_003_suite_requested);
    RUN_TEST(g1_copy_004_single_file_battery);
    RUN_TEST(g1_copy_005_pipeline_battery);
    RUN_TEST(g1_copies_never_make_a_live_anchor_ambiguous);

    RUN_TEST(g1_line_number_lane_reattaches_to_the_wrong_symbol);
#endif
}
