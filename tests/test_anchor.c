/*
 * test_anchor.c.
 *
 * Anchor resolution: symbol identity + content hash, never line numbers. The
 * suite pins the four states as implemented, the two assertion-table rows this
 * unit owns, and the substrate C4u/C5u will build on:
 *
 *   - "moved 200 lines -> still resolved" is asserted with the line numbers
 *     visibly CHANGING in the answer while the classification does not (C5u);
 *   - a hash match with a DIFFERENT qualified name is NEVER status resolved —
 *     it is a candidate, and a copy-paste yields ALL of them (I5, C4u);
 *   - an unknown workspace is a visible error NAMING the workspace, never an
 *     empty result;
 *   - ERROR is not a state of the anchor: "could not tell" never reads as any
 *     of the four classifications.
 *
 * Fixtures are tiny real trees in a temp dir plus an in-memory store — never
 * the user's index. Qualified names come from the REAL builder
 * (hyp_pipeline_fqn_compute), not hand-typed strings, for the same reason
 * test_identity.c does: a contract asserted against a literal only proves the
 * literal was typed twice.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "../src/foundation/identity.h"
#include "../src/memory/anchor.h"
#include "../src/pipeline/pipeline.h"
#include "../src/store/store.h"

#include <stdlib.h>
#include <string.h>

/* ── Fixture helpers ─────────────────────────────────────────────── */

/* The three lines of `foo`, exactly as a span reads them back: joined with
 * '\n', no trailing newline. */
#define AN_FOO_SPAN "int foo(void) {\n    return 1;\n}"
#define AN_FOO_FILE AN_FOO_SPAN "\n"

/* An in-memory store holding one project rooted at `root`. */
static hyp_store_t *an_store_one(const char *project, const char *root) {
    hyp_store_t *s = hyp_store_open_memory();
    if (!s) {
        return NULL;
    }
    if (hyp_store_upsert_project(s, project, root) != HYP_STORE_OK) {
        hyp_store_close(s);
        return NULL;
    }
    return s;
}

/* Upsert a Function node for `name` in `rel_path`, spanning the given lines,
 * with its REAL qualified name. Returns the malloc'd QN (caller frees) or
 * NULL. */
static char *an_put_node(hyp_store_t *s, const char *project, const char *rel_path,
                         const char *name, int start_line, int end_line) {
    char *qn = hyp_pipeline_fqn_compute(project, rel_path, name);
    if (!qn) {
        return NULL;
    }
    hyp_node_t n = {.project = project,
                    .label = "Function",
                    .name = name,
                    .qualified_name = qn,
                    .file_path = rel_path,
                    .start_line = start_line,
                    .end_line = end_line,
                    .properties_json = NULL};
    if (hyp_store_upsert_node(s, &n) <= 0) {
        free(qn);
        return NULL;
    }
    return qn;
}

/* The anchor string for (workspace, repo, qn) with an optional hash. */
static bool an_make(const char *workspace, const char *repo, const char *qn, const char *hash,
                    char *out, size_t out_sz) {
    hyp_addr_t a;
    if (hyp_addr_init(&a, workspace, repo, qn) != HYP_ADDR_OK) {
        return false;
    }
    return hyp_anchor_format(&a, hash, out, out_sz);
}

/* ════════════════════════════════════════════════════════════════════
 * The anchor string
 * ════════════════════════════════════════════════════════════════════ */

TEST(anchor_string_round_trips_with_and_without_hash) {
    hyp_addr_t a;
    ASSERT_EQ(hyp_addr_init(&a, NULL, "repo", "repo.src.a.foo"), HYP_ADDR_OK);

    /* Without a hash the anchor IS the address. */
    char plain[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&a, NULL, plain, sizeof(plain)));
    ASSERT_STR_EQ(plain, "hyp1:repo/repo#repo.src.a.foo");
    hyp_anchor_t p1;
    ASSERT_EQ(hyp_anchor_parse(plain, &p1), HYP_ADDR_OK);
    ASSERT_FALSE(p1.has_hash);
    ASSERT_STR_EQ(p1.span_hash, "");
    ASSERT_TRUE(hyp_addr_equal(&p1.addr, &a));

    /* With a hash it rides in FRONT — the QN stays the terminal field. */
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, hash);
    char with[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&a, hash, with, sizeof(with)));
    ASSERT_EQ((int)(strchr(with, '@') - with), HYP_ADDR_SPAN_HASH_LEN);
    hyp_anchor_t p2;
    ASSERT_EQ(hyp_anchor_parse(with, &p2), HYP_ADDR_OK);
    ASSERT_TRUE(p2.has_hash);
    ASSERT_STR_EQ(p2.span_hash, hash);
    ASSERT_TRUE(hyp_addr_equal(&p2.addr, &a));
    PASS();
}

TEST(anchor_string_refuses_rather_than_guessing) {
    hyp_anchor_t p;
    ASSERT_EQ(hyp_anchor_parse(NULL, &p), HYP_ADDR_ERR_NULL);
    ASSERT_EQ(hyp_anchor_parse("", &p), HYP_ADDR_ERR_SCHEME);
    ASSERT_EQ(hyp_anchor_parse("repo.src.a.foo", &p), HYP_ADDR_ERR_SCHEME);
    /* An uppercase or short hash prefix matches neither grammar. */
    ASSERT_EQ(hyp_anchor_parse("ABCDEF0123456789ABCDEF0123456789@hyp1:r/r#r.q", &p),
              HYP_ADDR_ERR_SCHEME);
    ASSERT_EQ(hyp_anchor_parse("abc@hyp1:r/r#r.q", &p), HYP_ADDR_ERR_SCHEME);
    /* A malformed hash is refused at compose time too, not passed through. */
    hyp_addr_t a;
    ASSERT_EQ(hyp_addr_init(&a, NULL, "repo", "repo.src.a.foo"), HYP_ADDR_OK);
    char buf[HYP_ANCHOR_MAX + 1];
    ASSERT_FALSE(hyp_anchor_format(&a, "nothex", buf, sizeof(buf)));
    /* And never a truncation: a buffer one byte short gets "", not a prefix. */
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, hash);
    char big[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&a, hash, big, sizeof(big)));
    char small[8];
    ASSERT_FALSE(hyp_anchor_format(&a, hash, small, sizeof(small)));
    ASSERT_STR_EQ(small, "");
    PASS();
}

TEST(anchor_qn_ending_in_at_hex_is_not_a_hash) {
    /* The QN is terminal and may contain '@' + 32 hex chars; a SUFFIX hash
     * would misread it. The prefix format cannot: composing this address with
     * no hash and parsing it back must yield the same QN and no hash. */
    char qn[128];
    snprintf(qn, sizeof(qn), "repo.gen.blob@%s", "0123456789abcdef0123456789abcdef");
    hyp_addr_t a;
    ASSERT_EQ(hyp_addr_init(&a, NULL, "repo", qn), HYP_ADDR_OK);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&a, NULL, anchor, sizeof(anchor)));
    hyp_anchor_t p;
    ASSERT_EQ(hyp_anchor_parse(anchor, &p), HYP_ADDR_OK);
    ASSERT_FALSE(p.has_hash);
    ASSERT_STR_EQ(p.addr.qn, qn);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * RESOLVED / RESOLVED_EDITED — the QN is the identity
 * ════════════════════════════════════════════════════════════════════ */

TEST(anchor_resolves_when_symbol_and_content_match) {
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/a.c"), AN_FOO_FILE), 0);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);
    char *qn = an_put_node(s, "repo", "src/a.c", "foo", 1, 3);
    ASSERT_NOT_NULL(qn);

    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, hash);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "repo", qn, hash, anchor, sizeof(anchor)));

    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &res), HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(res.relation, HYP_ADDR_REL_SAME);
    ASSERT_STR_EQ(res.project, "repo");
    ASSERT_STR_EQ(res.file_path, "src/a.c");
    ASSERT_EQ(res.start_line, 1);
    ASSERT_EQ(res.end_line, 3);
    ASSERT_STR_EQ(res.current_hash, hash);
    ASSERT_EQ(res.candidate_count, 0);
    hyp_anchor_res_free(&res);

    free(qn);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

TEST(anchor_without_recorded_hash_resolves_on_identity_alone) {
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/a.c"), AN_FOO_FILE), 0);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);
    char *qn = an_put_node(s, "repo", "src/a.c", "foo", 1, 3);
    ASSERT_NOT_NULL(qn);

    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "repo", qn, NULL, anchor, sizeof(anchor)));

    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &res), HYP_ANCHOR_RESOLVED);
    /* No hash was recorded, so no relation is claimed — INVALID here means
     * "no comparison happened", never "unrelated". */
    ASSERT_EQ(res.relation, HYP_ADDR_REL_INVALID);
    /* The current hash is still reported, so a caller can record a fresher
     * anchor without a second lookup. */
    char expect[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, expect);
    ASSERT_STR_EQ(res.current_hash, expect);
    hyp_anchor_res_free(&res);

    free(qn);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

TEST(anchor_edit_in_place_is_resolved_edited_still_attached) {
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/a.c"), AN_FOO_FILE), 0);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);
    char *qn = an_put_node(s, "repo", "src/a.c", "foo", 1, 3);
    ASSERT_NOT_NULL(qn);

    char old_hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, old_hash);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "repo", qn, old_hash, anchor, sizeof(anchor)));

    /* The symbol is edited in place: same QN, same lines, new body. */
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/a.c"), "int foo(void) {\n    return 2;\n}\n"), 0);

    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &res), HYP_ANCHOR_RESOLVED_EDITED);
    ASSERT_EQ(res.relation, HYP_ADDR_REL_EDITED);
    /* Still attached: the answer says where it is, and what it hashes to NOW. */
    ASSERT_STR_EQ(res.file_path, "src/a.c");
    char now[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash("int foo(void) {\n    return 2;\n}", now);
    ASSERT_STR_EQ(res.current_hash, now);
    ASSERT_STR_NEQ(res.current_hash, old_hash);
    hyp_anchor_res_free(&res);

    free(qn);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

TEST(anchor_moved_200_lines_stays_resolved) {
    /* C5u's substrate: `foo` slides 200 lines down its file. Resolution never
     * consulted a line number, so the classification cannot change — and the
     * ANSWER visibly does, which is what proves lines are output, not input. */
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/a.c"), AN_FOO_FILE), 0);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);
    char *qn = an_put_node(s, "repo", "src/a.c", "foo", 1, 3);
    ASSERT_NOT_NULL(qn);

    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, hash);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "repo", qn, hash, anchor, sizeof(anchor)));

    /* 200 lines of padding, then the identical span; the index re-observes
     * the move (same QN, new lines) as a re-index would. */
    char moved[8192];
    int off = 0;
    for (int i = 0; i < 200; i++) {
        off += snprintf(moved + off, sizeof(moved) - (size_t)off, "/* pad %d */\n", i);
    }
    snprintf(moved + off, sizeof(moved) - (size_t)off, "%s", AN_FOO_FILE);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/a.c"), moved), 0);
    char *qn2 = an_put_node(s, "repo", "src/a.c", "foo", 201, 203);
    ASSERT_NOT_NULL(qn2);
    ASSERT_STR_EQ(qn, qn2); /* the move changed no identity */
    free(qn2);

    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &res), HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(res.relation, HYP_ADDR_REL_SAME);
    ASSERT_EQ(res.start_line, 201);
    ASSERT_EQ(res.end_line, 203);
    hyp_anchor_res_free(&res);

    free(qn);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * ORPHANED — candidates with evidence, never a re-attachment (I5)
 * ════════════════════════════════════════════════════════════════════ */

TEST(anchor_orphaned_when_symbol_gone) {
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);

    char *qn = hyp_pipeline_fqn_compute("repo", "src/a.c", "foo");
    ASSERT_NOT_NULL(qn);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "repo", qn, NULL, anchor, sizeof(anchor)));

    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &res), HYP_ANCHOR_ORPHANED);
    /* No hash was recorded, so there is no evidence to scan for: zero
     * candidates is the honest answer, not a shortfall. */
    ASSERT_EQ(res.candidate_count, 0);
    ASSERT_NULL(res.candidates);
    hyp_anchor_res_free(&res);

    free(qn);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

TEST(anchor_cross_dir_move_yields_candidate_not_reattach) {
    /* `foo` moved from src/a.c to src/deep/b.c: the QN changed, the content
     * did not. The anchor is ORPHANED — and the move surfaces as a candidate
     * with CONTENT_ONLY evidence, never as a silent re-attachment. A file
     * RENAME produces exactly this observation too; the contract cannot tell
     * them apart and does not pretend to (identity.h). */
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/deep/b.c"), AN_FOO_FILE), 0);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);
    char *new_qn = an_put_node(s, "repo", "src/deep/b.c", "foo", 1, 3);
    ASSERT_NOT_NULL(new_qn);

    char *old_qn = hyp_pipeline_fqn_compute("repo", "src/a.c", "foo");
    ASSERT_NOT_NULL(old_qn);
    ASSERT_STR_NEQ(old_qn, new_qn);
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, hash);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "repo", old_qn, hash, anchor, sizeof(anchor)));

    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &res), HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(res.candidate_count, 1);
    ASSERT_NOT_NULL(res.candidates);
    ASSERT_STR_EQ(res.candidates[0].qn, new_qn);
    ASSERT_STR_EQ(res.candidates[0].file_path, "src/deep/b.c");
    ASSERT_EQ(res.candidates[0].relation, HYP_ADDR_REL_CONTENT_ONLY);
    ASSERT_EQ(res.scan_skipped, 0);
    hyp_anchor_res_free(&res);

    free(old_qn);
    free(new_qn);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

TEST(anchor_hash_match_with_different_qn_is_never_resolved) {
    /* THE I5 GUARD, and the false positive C4u exists to prevent: a copy of
     * `foo` pasted into a second file hashes identically, and the original
     * may not even exist any more. Whatever the scan finds, a hash match at a
     * DIFFERENT address must never come back as resolved — a false negative
     * loses a decision (recoverable); a false positive attaches last month's
     * reasoning to code that never had it (worse, and invisible). */
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/one.c"), AN_FOO_FILE), 0);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/two.c"), AN_FOO_FILE), 0);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);
    char *qn_one = an_put_node(s, "repo", "src/one.c", "foo", 1, 3);
    char *qn_two = an_put_node(s, "repo", "src/two.c", "foo", 1, 3);
    ASSERT_NOT_NULL(qn_one);
    ASSERT_NOT_NULL(qn_two);

    char *old_qn = hyp_pipeline_fqn_compute("repo", "src/a.c", "foo");
    ASSERT_NOT_NULL(old_qn);
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, hash);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "repo", old_qn, hash, anchor, sizeof(anchor)));

    hyp_anchor_res_t res;
    hyp_anchor_status_t st = hyp_anchor_resolve(s, NULL, anchor, &res);
    /* Never resolved, in either flavour. */
    ASSERT_NEQ((int)st, (int)HYP_ANCHOR_RESOLVED);
    ASSERT_NEQ((int)st, (int)HYP_ANCHOR_RESOLVED_EDITED);
    ASSERT_EQ(st, HYP_ANCHOR_ORPHANED);
    /* The copy-paste yields ALL candidates — both copies, attached to neither,
     * in deterministic (address-sorted) order. */
    ASSERT_EQ(res.candidate_count, 2);
    ASSERT_STR_EQ(res.candidates[0].qn, qn_one);
    ASSERT_STR_EQ(res.candidates[1].qn, qn_two);
    ASSERT_EQ(res.candidates[0].relation, HYP_ADDR_REL_CONTENT_ONLY);
    ASSERT_EQ(res.candidates[1].relation, HYP_ADDR_REL_CONTENT_ONLY);
    hyp_anchor_res_free(&res);

    free(old_qn);
    free(qn_one);
    free(qn_two);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * UNKNOWN_WORKSPACE — a visible error naming it, never an empty result
 * ════════════════════════════════════════════════════════════════════ */

TEST(anchor_unknown_workspace_is_a_visible_error_naming_it) {
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);

    char *qn = hyp_pipeline_fqn_compute("acme", "src/a.c", "foo");
    ASSERT_NOT_NULL(qn);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "acme", qn, NULL, anchor, sizeof(anchor)));

    /* Workspace of one: "acme" names no indexed project here. */
    hyp_anchor_res_t res;
    hyp_anchor_status_t st = hyp_anchor_resolve(s, NULL, anchor, &res);
    ASSERT_EQ(st, HYP_ANCHOR_UNKNOWN_WORKSPACE);
    /* NOT an empty result wearing any other state's clothes. */
    ASSERT_NEQ((int)st, (int)HYP_ANCHOR_ORPHANED);
    ASSERT_NEQ((int)st, (int)HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(res.candidate_count, 0);
 /* The error NAMES the workspace — the assertion row for this unit. */
    ASSERT_NOT_NULL(strstr(res.reason, "acme"));
    hyp_anchor_res_free(&res);

    /* Same gate when the caller states the workspace this index serves. */
    hyp_anchor_res_t res2;
    ASSERT_EQ(hyp_anchor_resolve(s, "hyponoia", anchor, &res2), HYP_ANCHOR_UNKNOWN_WORKSPACE);
    ASSERT_NOT_NULL(strstr(res2.reason, "acme"));
    ASSERT_NOT_NULL(strstr(res2.reason, "hyponoia"));
    hyp_anchor_res_free(&res2);

    free(qn);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The rendezvous namespace — same code path, workspace-wide (I2)
 * ════════════════════════════════════════════════════════════════════ */

TEST(anchor_rendezvous_resolves_across_repos) {
    /* A route lives in repo2; the anchor was written with no repo pinned
     * (workspace-scoped by SHAPE, not by a tag list). It must resolve against
     * the workspace even though repo1 knows nothing about it. */
    char *dir1 = th_mktempdir("hyp-anchor-r1");
    ASSERT_NOT_NULL(dir1);
    /* th_mktempdir returns a static buffer; keep dir1 across the second call. */
    char dir1_copy[256];
    snprintf(dir1_copy, sizeof(dir1_copy), "%s", dir1);
    char *dir2 = th_mktempdir("hyp-anchor-r2");
    ASSERT_NOT_NULL(dir2);

    hyp_store_t *s = an_store_one("repo1", dir1_copy);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_upsert_project(s, "repo2", dir2), HYP_STORE_OK);
    ASSERT_EQ(th_write_file(TH_PATH(dir2, "src/orders.c"), AN_FOO_FILE), 0);
    const char *route = "__route__GET__/orders";
    hyp_node_t n = {.project = "repo2",
                    .label = "Route",
                    .name = route,
                    .qualified_name = route,
                    .file_path = "src/orders.c",
                    .start_line = 1,
                    .end_line = 3,
                    .properties_json = NULL};
    ASSERT_TRUE(hyp_store_upsert_node(s, &n) > 0);

    /* The address derives workspace scope from the QN's shape. */
    hyp_addr_t a;
    ASSERT_EQ(hyp_addr_init(&a, "acme", "repo1", route), HYP_ADDR_OK);
    ASSERT_EQ((int)a.scope, (int)HYP_ADDR_SCOPE_WORKSPACE);
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, hash);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&a, hash, anchor, sizeof(anchor)));

    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, "acme", anchor, &res), HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(res.relation, HYP_ADDR_REL_SAME);
    ASSERT_STR_EQ(res.project, "repo2");
    hyp_anchor_res_free(&res);

    /* And a rendezvous name in NO project is orphaned, same code path. */
    hyp_addr_t gone;
    ASSERT_EQ(hyp_addr_init(&gone, "acme", "repo1", "__route__GET__/nowhere"), HYP_ADDR_OK);
    char anchor2[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&gone, NULL, anchor2, sizeof(anchor2)));
    hyp_anchor_res_t res2;
    ASSERT_EQ(hyp_anchor_resolve(s, "acme", anchor2, &res2), HYP_ANCHOR_ORPHANED);
    hyp_anchor_res_free(&res2);

    hyp_store_close(s);
    th_cleanup(dir1_copy);
    th_cleanup(dir2);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * ERROR is not a state — "could not tell" never wears a state's clothes
 * ════════════════════════════════════════════════════════════════════ */

TEST(anchor_error_is_not_a_state) {
    char *dir = th_mktempdir("hyp-anchor");
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "src/a.c"), AN_FOO_FILE), 0);
    hyp_store_t *s = an_store_one("repo", dir);
    ASSERT_NOT_NULL(s);
    char *qn = an_put_node(s, "repo", "src/a.c", "foo", 1, 3);
    ASSERT_NOT_NULL(qn);

    /* A string that is not an anchor. */
    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, "not-an-anchor", &res), HYP_ANCHOR_ERROR);
    ASSERT_TRUE(res.reason[0] != '\0');
    hyp_anchor_res_free(&res);

    /* A NULL store. */
    ASSERT_EQ(hyp_anchor_resolve(NULL, NULL, "hyp1:repo/repo#repo.q", &res), HYP_ANCHOR_ERROR);
    hyp_anchor_res_free(&res);

    /* A recorded hash that cannot be compared because the span is unreadable:
     * the index has the node, the tree lost the file. Neither resolved nor
     * edited can honestly be claimed, and orphaned would be false too — the
     * QN is still indexed. Fail closed. */
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(AN_FOO_SPAN, hash);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(an_make(NULL, "repo", qn, hash, anchor, sizeof(anchor)));
    ASSERT_EQ(th_rmtree(TH_PATH(dir, "src")), 0);
    hyp_anchor_res_t res3;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &res3), HYP_ANCHOR_ERROR);
    ASSERT_TRUE(res3.reason[0] != '\0');
    hyp_anchor_res_free(&res3);

    free(qn);
    hyp_store_close(s);
    th_cleanup(dir);
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(anchor) {
    /* The anchor string */
    RUN_TEST(anchor_string_round_trips_with_and_without_hash);
    RUN_TEST(anchor_string_refuses_rather_than_guessing);
    RUN_TEST(anchor_qn_ending_in_at_hex_is_not_a_hash);

    /* Resolved, in both flavours, without ever reading a line number */
    RUN_TEST(anchor_resolves_when_symbol_and_content_match);
    RUN_TEST(anchor_without_recorded_hash_resolves_on_identity_alone);
    RUN_TEST(anchor_edit_in_place_is_resolved_edited_still_attached);
    RUN_TEST(anchor_moved_200_lines_stays_resolved);

    /* Orphaned: candidate-with-evidence, never automatic re-attach (I5) */
    RUN_TEST(anchor_orphaned_when_symbol_gone);
    RUN_TEST(anchor_cross_dir_move_yields_candidate_not_reattach);
    RUN_TEST(anchor_hash_match_with_different_qn_is_never_resolved);

    /* The two ways resolution refuses */
    RUN_TEST(anchor_unknown_workspace_is_a_visible_error_naming_it);
    RUN_TEST(anchor_error_is_not_a_state);

    /* The rendezvous namespace */
    RUN_TEST(anchor_rendezvous_resolves_across_repos);
}
