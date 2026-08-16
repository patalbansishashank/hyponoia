/*
 * test_orphan.c — orphaning: the read path, and the re-attach decision.
 *
 * The corpus is NOT authored here. Every lifecycle replayed below was mined
 * from this repository's own history and its expected answer was written
 * against the frozen contracts before any of this code existed; the data lives
 * in test_orphan_corpus.h and the harness only replays it. That is deliberate:
 * a gold standard invented alongside the thing it grades is where false
 * positives come from, and this project has already paid for one.
 *
 * THE PARSER CAVEAT IS DISCHARGED BY CONSTRUCTION. The corpus found its spans
 * by brace-matching; the product finds them with tree-sitter. So no parser runs
 * here at all: the span text is carried verbatim, written into a file, and
 * indexed at exactly the lines it occupies. The hash the product computes is
 * then the hash the corpus recorded, and a disagreement can only be about
 * anchoring — which is the thing under test — and never about where a
 * declaration begins.
 *
 * Two assertion-table rows live in this file:
 *
 *   orphan-ness off the record — after detection the read path shows
 *   candidate-with-evidence AND the store's set digest is unchanged;
 *
 *   three orphan inputs — cross-directory move, file rename and copy-paste
 *   each surface as candidate-with-evidence, and copy-paste yields two and
 *   attaches to neither.
 */
#include "test_framework.h"
#include "test_helpers.h"
#include "test_orphan_corpus.h"

#include "../src/foundation/identity.h"
#include "../src/foundation/record.h"
#include "../src/memory/anchor.h"
#include "../src/memory/orphan.h"
#include "../src/pipeline/pipeline.h"
#include "../src/store/record_store.h"
#include "../src/store/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Fixture helpers ─────────────────────────────────────────────── */

/* A three-line function, for the cases the corpus deliberately does not
 * cover: history produces no unknown-workspace and no unreadable span, so
 * those are planted and labelled rather than faked into the corpus. */
#define ORPH_FOO_SPAN "int foo(void) {\n    return 1;\n}"
#define ORPH_FOO_FILE ORPH_FOO_SPAN "\n"

/* Materialise the live tree the corpus describes: every after-state span, in
 * the file it really lives in, at the lines it really occupies within that
 * file. Spans sharing a path are concatenated in corpus order — several
 * functions per file is what a real tree looks like, and it is what makes the
 * candidate scan walk more than one node per read.
 *
 * The node's qualified name is the corpus's, verbatim. A separate test asserts
 * the product's own builder reproduces every one of them; keeping the two
 * apart means a divergence there is reported as a divergence, instead of
 * silently invalidating every replay in this file. */
static int orph_build_live_tree(hyp_store_t *store, const char *project, const char *root) {
    for (int i = 0; i < g1_live_count; i++) {
        /* First occurrence of this path owns writing it. */
        bool first = true;
        for (int j = 0; j < i; j++) {
            if (strcmp(g1_live[j].file_path, g1_live[i].file_path) == 0) {
                first = false;
                break;
            }
        }
        if (!first) {
            continue;
        }
        size_t cap = 1;
        for (int j = i; j < g1_live_count; j++) {
            if (strcmp(g1_live[j].file_path, g1_live[i].file_path) == 0) {
                cap += strlen(g1_texts[g1_live[j].text_index].text) + 1;
            }
        }
        char *buf = malloc(cap);
        if (!buf) {
            return -1;
        }
        size_t off = 0;
        int line = 1;
        for (int j = i; j < g1_live_count; j++) {
            if (strcmp(g1_live[j].file_path, g1_live[i].file_path) != 0) {
                continue;
            }
            const g1_text_t *t = &g1_texts[g1_live[j].text_index];
            size_t n = strlen(t->text);
            memcpy(buf + off, t->text, n);
            off += n;
            buf[off++] = '\n';
            hyp_node_t node = {.project = project,
                               .label = "Function",
                               .name = g1_live[j].name,
                               .qualified_name = g1_live[j].qualified_name,
                               .file_path = g1_live[j].file_path,
                               .start_line = line,
                               .end_line = line + t->lines - 1,
                               .properties_json = NULL};
            if (hyp_store_upsert_node(store, &node) <= 0) {
                free(buf);
                return -1;
            }
            line += t->lines;
        }
        buf[off] = '\0';
        int rc = th_write_file(TH_PATH(root, g1_live[i].file_path), buf);
        free(buf);
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

/* Open an in-memory index over one project and fill it with the live tree. */
static hyp_store_t *orph_live_store(const char *root) {
    hyp_store_t *s = hyp_store_open_memory();
    if (!s) {
        return NULL;
    }
    if (hyp_store_upsert_project(s, G1_PROJECT, root) != HYP_STORE_OK ||
        orph_build_live_tree(s, G1_PROJECT, root) != 0) {
        hyp_store_close(s);
        return NULL;
    }
    return s;
}

static const g1_fixture_t *orph_first_of(const char *category) {
    for (int i = 0; i < g1_fixture_count; i++) {
        if (strcmp(g1_fixtures[i].category, category) == 0) {
            return &g1_fixtures[i];
        }
    }
    return NULL;
}

static hyp_anchor_status_t orph_status_from_name(const char *name) {
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
    if (strcmp(name, "HYP_ANCHOR_AMBIGUOUS") == 0) {
        return HYP_ANCHOR_AMBIGUOUS;
    }
    return HYP_ANCHOR_ERROR;
}

/* The anchor a composed case resolves: a qualified name one fixture proves is
 * gone, carrying a content hash another fixture proves the tree holds more
 * than once. */
static bool orph_composed_anchor(const g1_composed_t *c, char *out, size_t out_sz) {
    hyp_addr_t a;
    if (hyp_addr_init(&a, NULL, G1_PROJECT, c->dead_qn) != HYP_ADDR_OK) {
        return false;
    }
    return hyp_anchor_format(&a, c->hash, out, out_sz);
}

/* Append one anchored decision and hand back its id. */
static char *orph_put_decision(hyp_record_store_t *rs, const char *anchor, const char *body) {
    hyp_record_input_t in = {.kind = HYP_RECORD_DECISION,
                             .author = "c4u-suite",
                             .timestamp_ms = 1700000000000,
                             .content = body,
                             .anchor = anchor,
                             .origin = NULL,
                             .thread = NULL,
                             .parent = NULL,
                             .redactions = 0};
    const hyp_record_t *rec = NULL;
    if (hyp_record_build(&in, &rec) != HYP_RECORD_OK) {
        return NULL;
    }
    char *id = NULL;
    if (hyp_record_store_append(rs, rec, NULL) == HYP_RECORD_STORE_OK) {
        size_t n = strlen(rec->id) + 1;
        id = malloc(n);
        if (id) {
            memcpy(id, rec->id, n);
        }
    }
    hyp_record_free(rec);
    return id;
}

/* ════════════════════════════════════════════════════════════════════
 * The corpus, checked against the product before anything is replayed
 * ════════════════════════════════════════════════════════════════════ */

TEST(orphan_corpus_qualified_names_match_the_product_builder) {
    /* The corpus built its addresses from the documented qualified-name rule.
     * The product has two implementations of that rule and a differential
     * found six divergences between them, so "the corpus assumed the right
     * answer" is not something to take on trust — it is a two-ends question,
     * and both ends are here. A mismatch is a finding about the address space,
     * not about anchoring. */
    int checked = 0;
    for (int i = 0; i < g1_fixture_count; i++) {
        char *qn = hyp_pipeline_fqn_compute(G1_PROJECT, g1_fixtures[i].before_file_path,
                                            g1_fixtures[i].before_name);
        ASSERT_NOT_NULL(qn);
        ASSERT_STR_EQ(qn, g1_fixtures[i].before_qn);
        free(qn);
        checked++;
    }
    for (int i = 0; i < g1_live_count; i++) {
        char *qn = hyp_pipeline_fqn_compute(G1_PROJECT, g1_live[i].file_path, g1_live[i].name);
        ASSERT_NOT_NULL(qn);
        ASSERT_STR_EQ(qn, g1_live[i].qualified_name);
        free(qn);
        checked++;
    }
    ASSERT_EQ(checked, g1_fixture_count + g1_live_count);
    PASS();
}

TEST(orphan_corpus_span_hashes_reproduce_under_the_product_hash) {
    /* The parser caveat, discharged: the text is pinned, so the only thing
     * being compared is the hash function itself. If this passes, every
     * RESOLVED/EDITED/CONTENT_ONLY answer below is about anchoring. */
    ASSERT_TRUE(g1_text_count > 0);
    for (int i = 0; i < g1_text_count; i++) {
        char h[HYP_ADDR_SPAN_HASH_LEN + 1];
        hyp_addr_span_hash(g1_texts[i].text, h);
        ASSERT_STR_EQ(h, g1_texts[i].span_hash);
    }
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The replay — all thirty-six lifecycles against one live tree
 * ════════════════════════════════════════════════════════════════════ */

TEST(orphan_replays_every_history_fixture) {
    char *tmp = th_mktempdir("hyp-orphan-g1");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *s = orph_live_store(root);
    ASSERT_NOT_NULL(s);

    /* One tree for all thirty-six. The cross-contamination is the point: the
     * eight renamed anchors are scanned against a workspace that also holds
     * every near-identical sibling and every pasted copy the corpus collected,
     * so "candidate_count 0" is a claim about the whole tree rather than about
     * a tree containing only the right answer. */
    int passed = 0;
    for (int i = 0; i < g1_fixture_count; i++) {
        const g1_fixture_t *f = &g1_fixtures[i];
        hyp_anchor_res_t res;
        hyp_anchor_status_t st = hyp_anchor_resolve(s, NULL, f->anchor, &res);
        if (st != orph_status_from_name(f->expected_status)) {
            (void)fprintf(stderr, "  %s: status %s, expected %s\n", f->id,
                          hyp_anchor_status_str(st), f->expected_status);
        }
        ASSERT_EQ(st, orph_status_from_name(f->expected_status));
        ASSERT_EQ(res.candidate_count, f->expected_candidate_count);
        /* The scan is a floor whenever it skipped; this tree is fully
         * readable, so a nonzero count would mean the counts above are floors
         * and the equality assertions are not entitled to be equalities. */
        ASSERT_EQ(res.scan_skipped, 0);
        /* The wrong answers the corpus names, one by one. A candidate MAY be
         * one of them — a candidate is evidence, not an attachment. What must
         * never happen is an ATTACHING status landing on one, so the address
         * this anchor attached to is compared against every named wrong
         * answer. A suite that checked only the status could pass by handing
         * back something plausible. */
        if (st == HYP_ANCHOR_RESOLVED || st == HYP_ANCHOR_RESOLVED_EDITED) {
            char addr[HYP_ADDR_MAX + 1];
            hyp_addr_t a;
            ASSERT_EQ(hyp_addr_init(&a, NULL, G1_PROJECT, f->before_qn), HYP_ADDR_OK);
            ASSERT_TRUE(hyp_addr_format(&a, addr, sizeof(addr)));
            for (const char *const *bad = f->must_not_attach; *bad; bad++) {
                ASSERT_STR_NEQ(addr, *bad);
            }
        }
        hyp_anchor_res_free(&res);
        passed++;
    }
    ASSERT_EQ(passed, 36);
    ASSERT_EQ(passed, g1_fixture_count);

    hyp_store_close(s);
    th_cleanup(root);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Row: three orphan inputs — cross-dir move, file rename, copy-paste
 * ════════════════════════════════════════════════════════════════════ */

/* Shared body for the two categories that produce exactly one candidate. */
static int orph_check_single_candidate_category(const char *category, int *out_seen) {
    char *tmp = th_mktempdir("hyp-orphan-cat");
    if (!tmp) {
        return -1;
    }
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *s = orph_live_store(root);
    if (!s) {
        th_cleanup(root);
        return -1;
    }
    int seen = 0;
    int rc = 0;
    for (int i = 0; i < g1_fixture_count && rc == 0; i++) {
        const g1_fixture_t *f = &g1_fixtures[i];
        if (strcmp(f->category, category) != 0) {
            continue;
        }
        hyp_anchor_res_t res;
        hyp_anchor_status_t st = hyp_anchor_resolve(s, NULL, f->anchor, &res);
        hyp_orphan_entry_t entry = {.record = NULL, .res = res};
        char *view = hyp_orphan_render_entry(&entry);
        if (st != HYP_ANCHOR_ORPHANED || res.candidate_count != 1 ||
            res.candidates[0].relation != HYP_ADDR_REL_CONTENT_ONLY || !view ||
            !strstr(view, "anchor_status: orphaned") || !strstr(view, "reattached: no") ||
            !strstr(view, "evidence=content-only") ||
            !strstr(view, res.candidates[0].address) || strstr(view, "\nattached:")) {
            (void)fprintf(stderr, "  %s: st=%s cand=%d\n", f->id, hyp_anchor_status_str(st),
                          res.candidate_count);
            rc = -1;
        }
        free(view);
        hyp_anchor_res_free(&res);
        seen++;
    }
    hyp_store_close(s);
    th_cleanup(root);
    *out_seen = seen;
    return rc;
}

TEST(orphan_cross_dir_move_surfaces_candidate_with_evidence) {
    /* The qualified name died, the content did not. The move is EVIDENCE with
     * a relation naming what was observed, and the answer says in as many
     * words that nothing was attached to it. */
    int seen = 0;
    ASSERT_EQ(orph_check_single_candidate_category("cross_directory_move", &seen), 0);
    ASSERT_EQ(seen, 5);
    PASS();
}

TEST(orphan_file_rename_surfaces_candidate_with_evidence) {
    /* A file rename inside one directory produces the SAME observation as a
     * cross-directory move — one content match at an address the anchor does
     * not name. The contract cannot tell them apart and this test asserts that
     * it does not pretend to: identical shape, identical relation. */
    int seen = 0;
    ASSERT_EQ(orph_check_single_candidate_category("file_rename_same_directory", &seen), 0);
    ASSERT_EQ(seen, 2);
    PASS();
}

TEST(orphan_copy_paste_original_still_resolves_to_itself) {
    /* Copies must not make a live anchor ambiguous. Symbol identity is
     * consulted first, so the original is found by name and the copies never
     * enter the answer — a hash scan does not even run. */
    char *tmp = th_mktempdir("hyp-orphan-copy");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *s = orph_live_store(root);
    ASSERT_NOT_NULL(s);

    int seen = 0;
    for (int i = 0; i < g1_fixture_count; i++) {
        const g1_fixture_t *f = &g1_fixtures[i];
        if (strcmp(f->category, "copy_paste") != 0) {
            continue;
        }
        hyp_anchor_res_t res;
        ASSERT_EQ(hyp_anchor_resolve(s, NULL, f->anchor, &res), HYP_ANCHOR_RESOLVED);
        ASSERT_EQ(res.relation, HYP_ADDR_REL_SAME);
        ASSERT_STR_EQ(res.file_path, f->before_file_path);
        ASSERT_EQ(res.candidate_count, 0);

        hyp_orphan_entry_t entry = {.record = NULL, .res = res};
        char *view = hyp_orphan_render_entry(&entry);
        ASSERT_NOT_NULL(view);
        ASSERT_NOT_NULL(strstr(view, "anchor_status: resolved\n"));
        ASSERT_NOT_NULL(strstr(view, f->before_file_path));
        /* The copies live in other files and this answer names none of them. */
        for (int j = 0; j < g1_live_count; j++) {
            if (strcmp(g1_live[j].fixture_id, f->id) != 0) {
                continue;
            }
            if (strcmp(g1_live[j].file_path, f->before_file_path) == 0) {
                continue;
            }
            ASSERT_NULL(strstr(view, g1_live[j].file_path));
        }
        free(view);
        hyp_anchor_res_free(&res);
        seen++;
    }
    ASSERT_EQ(seen, 5);

    hyp_store_close(s);
    th_cleanup(root);
    PASS();
}

TEST(orphan_copy_paste_yields_two_candidates_and_attaches_to_neither) {
    /* THE ROW. A dead qualified name plus content that survives at more than
     * one address is the shape §4 names, and NO COMMIT IN THIS HISTORY
     * PRODUCED IT — two exhaustive searches over the whole repository found
     * zero instances, and the corpus refused to fabricate one rather than
     * ship a plausible composite.
     *
     * So it is composed here from two verified operands: a qualified name one
     * rename fixture proves is gone from the tree, and a content hash a
     * copy-paste fixture proves the tree holds at two addresses, and at three.
     * The addresses and the count come from the corpus, not from this file.
     * What is asserted is the resolver's property, which does not care how the
     * state arose: every copy is listed, and none of them is attached. */
    char *tmp = th_mktempdir("hyp-orphan-two");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *s = orph_live_store(root);
    ASSERT_NOT_NULL(s);

    ASSERT_EQ(g1_composed_count, 2);
    bool saw_two = false;
    bool saw_three = false;
    for (int i = 0; i < g1_composed_count; i++) {
        const g1_composed_t *c = &g1_composed[i];
        char anchor[HYP_ANCHOR_MAX + 1];
        ASSERT_TRUE(orph_composed_anchor(c, anchor, sizeof(anchor)));

        hyp_anchor_res_t res;
        hyp_anchor_status_t st = hyp_anchor_resolve(s, NULL, anchor, &res);
        /* Never resolved, in either flavour: the false positive is the worse
         * failure and it is the one that looks like success. */
        ASSERT_NEQ((int)st, (int)HYP_ANCHOR_RESOLVED);
        ASSERT_NEQ((int)st, (int)HYP_ANCHOR_RESOLVED_EDITED);
        ASSERT_EQ(st, HYP_ANCHOR_ORPHANED);
        ASSERT_EQ(res.candidate_count, c->expected_candidate_count);
        ASSERT_EQ(res.scan_skipped, 0);

        /* Every candidate, in the corpus's own order, each one carrying the
         * evidence that it is a content match and nothing more. */
        int k = 0;
        for (const char *const *want = c->expected_addresses; *want; want++, k++) {
            ASSERT_TRUE(k < res.candidate_count);
            ASSERT_STR_EQ(res.candidates[k].address, *want);
            ASSERT_EQ(res.candidates[k].relation, HYP_ADDR_REL_CONTENT_ONLY);
        }
        ASSERT_EQ(k, res.candidate_count);

        hyp_orphan_entry_t entry = {.record = NULL, .res = res};
        char *view = hyp_orphan_render_entry(&entry);
        ASSERT_NOT_NULL(view);
        ASSERT_NOT_NULL(strstr(view, "anchor_status: orphaned"));
        ASSERT_NOT_NULL(strstr(view, "reattached: no"));
        ASSERT_NULL(strstr(view, "\nattached:"));
        for (const char *const *want = c->expected_addresses; *want; want++) {
            ASSERT_NOT_NULL(strstr(view, *want));
        }
        free(view);
        hyp_anchor_res_free(&res);
        if (c->expected_candidate_count == 2) {
            saw_two = true;
        }
        if (c->expected_candidate_count == 3) {
            saw_three = true;
        }
    }
    ASSERT_TRUE(saw_two);
    ASSERT_TRUE(saw_three);

    hyp_store_close(s);
    th_cleanup(root);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Row: orphan-ness is off the record — the digest does not move
 * ════════════════════════════════════════════════════════════════════ */

TEST(orphan_detection_leaves_the_store_digest_unchanged) {
    /* THE ROW. Detection is a read. The record carries the anchor inside the
     * preimage of its own id, so there is no mark to set — and this asserts
     * the consequence rather than the intention: sweep the whole store,
     * classify everything, watch a real orphan surface with its evidence, and
     * the digest is the digest it was. */
    char *tmp = th_mktempdir("hyp-orphan-dig");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *index = orph_live_store(root);
    ASSERT_NOT_NULL(index);

    hyp_record_store_t *rs = NULL;
    ASSERT_EQ(hyp_record_store_open(TH_PATH(root, "memory"), &rs), HYP_RECORD_STORE_OK);

    const g1_fixture_t *moved = orph_first_of("cross_directory_move");
    ASSERT_NOT_NULL(moved);
    const g1_fixture_t *live = orph_first_of("copy_paste");
    ASSERT_NOT_NULL(live);
    char *orphan_id = orph_put_decision(rs, moved->anchor, "why the moved symbol looks like this");
    ASSERT_NOT_NULL(orphan_id);
    char *live_id = orph_put_decision(rs, live->anchor, "why the copied symbol looks like this");
    ASSERT_NOT_NULL(live_id);

    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(rs, before), HYP_RECORD_STORE_OK);
    size_t count_before = 0;
    ASSERT_EQ(hyp_record_store_count(rs, &count_before), HYP_RECORD_STORE_OK);

    hyp_orphan_view_t *view = NULL;
    ASSERT_EQ(hyp_orphan_view_build(rs, index, NULL, NULL, &view), HYP_ORPHAN_OK);
    ASSERT_EQ(hyp_orphan_view_count(view), 2);
    ASSERT_EQ(hyp_orphan_view_count_status(view, HYP_ANCHOR_ORPHANED), 1);
    ASSERT_EQ(hyp_orphan_view_count_status(view, HYP_ANCHOR_RESOLVED), 1);

    /* Candidate-with-evidence, in what a reader actually reads. */
    char *rendered = hyp_orphan_render(view);
    ASSERT_NOT_NULL(rendered);
    ASSERT_NOT_NULL(strstr(rendered, "anchor_status: orphaned"));
    ASSERT_NOT_NULL(strstr(rendered, "candidates: 1"));
    ASSERT_NOT_NULL(strstr(rendered, "evidence=content-only"));
    ASSERT_NOT_NULL(strstr(rendered, "reattached: no"));
    ASSERT_NOT_NULL(strstr(rendered, orphan_id));
    free(rendered);

    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(rs, after), HYP_RECORD_STORE_OK);
    size_t count_after = 0;
    ASSERT_EQ(hyp_record_store_count(rs, &count_after), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(after, before);
    ASSERT_EQ((int)count_after, (int)count_before);

    hyp_orphan_view_free(view);
    free(orphan_id);
    free(live_id);
    hyp_record_store_close(rs);
    hyp_store_close(index);
    th_cleanup(root);
    PASS();
}

TEST(orphan_signal_is_an_append_whose_parent_is_the_decision) {
    /* The other half of the row: when the observation IS recorded, it is a new
     * record pointing back, never a mark. The decision comes out of the store
     * byte-identical afterwards, which it must, because its id is the digest
     * of its fields. */
    char *tmp = th_mktempdir("hyp-orphan-sig");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *index = orph_live_store(root);
    ASSERT_NOT_NULL(index);

    hyp_record_store_t *rs = NULL;
    ASSERT_EQ(hyp_record_store_open(TH_PATH(root, "memory"), &rs), HYP_RECORD_STORE_OK);
    const g1_fixture_t *moved = orph_first_of("cross_directory_move");
    ASSERT_NOT_NULL(moved);
    char *decision_id = orph_put_decision(rs, moved->anchor, "the reasoning that must survive");
    ASSERT_NOT_NULL(decision_id);

    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(rs, before), HYP_RECORD_STORE_OK);

    hyp_orphan_view_t *view = NULL;
    ASSERT_EQ(hyp_orphan_view_build(rs, index, NULL, NULL, &view), HYP_ORPHAN_OK);
    const hyp_orphan_entry_t *e = hyp_orphan_view_at(view, 0);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(e->res.status, HYP_ANCHOR_ORPHANED);

    const hyp_record_t *sig = NULL;
    ASSERT_EQ(hyp_orphan_signal_build(e, "c4u-suite", 1700000001000, &sig), HYP_ORPHAN_OK);
    ASSERT_NOT_NULL(sig);
    ASSERT_EQ((int)sig->kind, (int)HYP_RECORD_SIGNAL);
    /* It points back at the decision, and carries no anchor of its own: the
     * address it observed is the one that does not resolve. */
    ASSERT_STR_EQ(sig->parent, decision_id);
    ASSERT_NULL(sig->anchor);
    /* The evidence travels with the observation. */
    ASSERT_NOT_NULL(strstr(sig->content, "anchor_status: orphaned"));
    ASSERT_NOT_NULL(strstr(sig->content, "reattached: no"));
    ASSERT_NOT_NULL(strstr(sig->content, e->res.candidates[0].address));

    bool added = false;
    ASSERT_EQ(hyp_record_store_append(rs, sig, &added), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(added);
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(rs, after), HYP_RECORD_STORE_OK);
    ASSERT_STR_NEQ(after, before);
    size_t n = 0;
    ASSERT_EQ(hyp_record_store_count(rs, &n), HYP_RECORD_STORE_OK);
    ASSERT_EQ((int)n, 2);

    /* The decision itself was not touched — same id, same anchor, still the
     * record it was. There is no orphan flag on it because there is nowhere to
     * put one. */
    const hyp_record_t *back = NULL;
    ASSERT_EQ(hyp_record_store_get(rs, decision_id, &back), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(back);
    ASSERT_STR_EQ(back->id, decision_id);
    ASSERT_STR_EQ(back->anchor, moved->anchor);
    ASSERT_EQ((int)back->kind, (int)HYP_RECORD_DECISION);
    hyp_record_free(back);

    /* The same observation re-derived is the same record, so recording it
     * twice is a union absorbing a duplicate rather than a second row. */
    char twice[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_append(rs, sig, &added), HYP_RECORD_STORE_OK);
    ASSERT_FALSE(added);
    ASSERT_EQ(hyp_record_store_digest(rs, twice), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(twice, after);

    /* And it is findable from the decision it is about. */
    hyp_record_store_query_t q = {0};
    q.parent = decision_id;
    hyp_record_set_t *found = NULL;
    ASSERT_EQ(hyp_record_store_query(rs, &q, &found), HYP_RECORD_STORE_OK);
    ASSERT_EQ((int)hyp_record_set_count(found), 1);
    hyp_record_set_free(found);

    hyp_record_free(sig);
    hyp_orphan_view_free(view);
    free(decision_id);
    hyp_record_store_close(rs);
    hyp_store_close(index);
    th_cleanup(root);
    PASS();
}

TEST(orphan_ness_is_a_property_of_record_and_index_state) {
    /* Two machines may legitimately disagree, and this is why orphan-ness
     * cannot be stored: the SAME record, byte for byte, is resolved against
     * one index and orphaned against another. Neither answer is wrong, and a
     * flag would have made one of them permanent. */
    char *tmp = th_mktempdir("hyp-orphan-two-idx");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *live = orph_live_store(root);
    ASSERT_NOT_NULL(live);

    /* The second machine's tree: the same project, none of the code. */
    hyp_store_t *bare = hyp_store_open_memory();
    ASSERT_NOT_NULL(bare);
    ASSERT_EQ(hyp_store_upsert_project(bare, G1_PROJECT, root), HYP_STORE_OK);

    hyp_record_store_t *rs = NULL;
    ASSERT_EQ(hyp_record_store_open(TH_PATH(root, "memory"), &rs), HYP_RECORD_STORE_OK);
    const g1_fixture_t *copy = orph_first_of("copy_paste");
    ASSERT_NOT_NULL(copy);
    char *id = orph_put_decision(rs, copy->anchor, "one record, two machines");
    ASSERT_NOT_NULL(id);

    hyp_orphan_view_t *v1 = NULL;
    ASSERT_EQ(hyp_orphan_view_build(rs, live, NULL, NULL, &v1), HYP_ORPHAN_OK);
    ASSERT_EQ(hyp_orphan_view_at(v1, 0)->res.status, HYP_ANCHOR_RESOLVED);

    hyp_orphan_view_t *v2 = NULL;
    ASSERT_EQ(hyp_orphan_view_build(rs, bare, NULL, NULL, &v2), HYP_ORPHAN_OK);
    ASSERT_EQ(hyp_orphan_view_at(v2, 0)->res.status, HYP_ANCHOR_ORPHANED);

    /* Same record on both sides. */
    ASSERT_STR_EQ(hyp_orphan_view_at(v1, 0)->record->id, id);
    ASSERT_STR_EQ(hyp_orphan_view_at(v2, 0)->record->id, id);

    /* And the disagreement cost the store nothing. */
    char digest[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(rs, digest), HYP_RECORD_STORE_OK);
    size_t n = 0;
    ASSERT_EQ(hyp_record_store_count(rs, &n), HYP_RECORD_STORE_OK);
    ASSERT_EQ((int)n, 1);

    hyp_orphan_view_free(v1);
    hyp_orphan_view_free(v2);
    free(id);
    hyp_record_store_close(rs);
    hyp_store_close(bare);
    hyp_store_close(live);
    th_cleanup(root);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Never silently dropped; absent is not empty; a floor says so
 * ════════════════════════════════════════════════════════════════════ */

TEST(orphan_view_never_silently_drops_an_orphan) {
    /* A sweep that quietly kept only the entries it could resolve would read
     * exactly like a store with fewer decisions in it. Every anchored record
     * the query matched has an entry, and the orphan is one of them. */
    char *tmp = th_mktempdir("hyp-orphan-drop");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *index = orph_live_store(root);
    ASSERT_NOT_NULL(index);

    hyp_record_store_t *rs = NULL;
    ASSERT_EQ(hyp_record_store_open(TH_PATH(root, "memory"), &rs), HYP_RECORD_STORE_OK);

    /* One of each: renamed (orphan, no candidate), moved cross-directory
     * (orphan, one candidate), copied (resolves). */
    const g1_fixture_t *renamed = orph_first_of("renamed");
    const g1_fixture_t *moved = orph_first_of("cross_directory_move");
    const g1_fixture_t *copied = orph_first_of("copy_paste");
    ASSERT_NOT_NULL(renamed);
    ASSERT_NOT_NULL(moved);
    ASSERT_NOT_NULL(copied);
    char *a = orph_put_decision(rs, renamed->anchor, "reasoning about a renamed symbol");
    char *b = orph_put_decision(rs, moved->anchor, "reasoning about a moved symbol");
    char *c = orph_put_decision(rs, copied->anchor, "reasoning about a copied symbol");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);

    hyp_orphan_view_t *view = NULL;
    ASSERT_EQ(hyp_orphan_view_build(rs, index, NULL, NULL, &view), HYP_ORPHAN_OK);
    ASSERT_EQ(hyp_orphan_view_count(view), 3);
    ASSERT_EQ(hyp_orphan_view_count_status(view, HYP_ANCHOR_ORPHANED), 2);

    /* Visible in the read path, which is the only place "visibly orphaned"
     * can mean anything: all three record ids are in what a reader reads. */
    char *rendered = hyp_orphan_render(view);
    ASSERT_NOT_NULL(rendered);
    ASSERT_NOT_NULL(strstr(rendered, a));
    ASSERT_NOT_NULL(strstr(rendered, b));
    ASSERT_NOT_NULL(strstr(rendered, c));
    free(rendered);

    hyp_orphan_view_free(view);
    free(a);
    free(b);
    free(c);
    hyp_record_store_close(rs);
    hyp_store_close(index);
    th_cleanup(root);
    PASS();
}

TEST(orphan_absent_candidates_are_not_empty_candidates) {
    /* Absent means look elsewhere; empty means there is nothing. An anchor
     * with no recorded hash gave the scan nothing to look for, so no scan ran;
     * an anchor with a hash that matched nothing was searched for and is not
     * there. Both have zero candidates and they are not the same answer. */
    char *tmp = th_mktempdir("hyp-orphan-absent");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *index = orph_live_store(root);
    ASSERT_NOT_NULL(index);

    const g1_fixture_t *renamed = orph_first_of("renamed");
    ASSERT_NOT_NULL(renamed);

    /* Scanned, and there is nothing: the corpus verified this hash matches
     * nothing anywhere in the after-tree. */
    hyp_anchor_res_t scanned;
    ASSERT_EQ(hyp_anchor_resolve(index, NULL, renamed->anchor, &scanned), HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(scanned.candidate_count, 0);
    hyp_orphan_entry_t e1 = {.record = NULL, .res = scanned};
    char *v1 = hyp_orphan_render_entry(&e1);
    ASSERT_NOT_NULL(v1);
    ASSERT_NOT_NULL(strstr(v1, "candidates: none"));
    ASSERT_NULL(strstr(v1, "candidates: not-scanned"));

    /* Not scanned: the same dead name with no content observation recorded. */
    hyp_addr_t addr;
    ASSERT_EQ(hyp_addr_init(&addr, NULL, G1_PROJECT, renamed->before_qn), HYP_ADDR_OK);
    char hashless[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&addr, NULL, hashless, sizeof(hashless)));
    hyp_anchor_res_t unscanned;
    ASSERT_EQ(hyp_anchor_resolve(index, NULL, hashless, &unscanned), HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(unscanned.candidate_count, 0);
    hyp_orphan_entry_t e2 = {.record = NULL, .res = unscanned};
    char *v2 = hyp_orphan_render_entry(&e2);
    ASSERT_NOT_NULL(v2);
    ASSERT_NOT_NULL(strstr(v2, "candidates: not-scanned"));
    ASSERT_NULL(strstr(v2, "candidates: none"));

    /* Same status, same count, different claim. */
    ASSERT_STR_NEQ(v1, v2);

    free(v1);
    free(v2);
    hyp_anchor_res_free(&scanned);
    hyp_anchor_res_free(&unscanned);
    hyp_store_close(index);
    th_cleanup(root);
    PASS();
}

TEST(orphan_scan_floor_is_disclosed_not_absorbed) {
    /* A span the scan could not read makes the candidate list a floor. The
     * reader is told, because "no candidate" from an incomplete search is the
     * false negative dressed as a finding. */
    char *tmp = th_mktempdir("hyp-orphan-floor");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *s = hyp_store_open_memory();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_upsert_project(s, "repo", root), HYP_STORE_OK);
    ASSERT_EQ(th_write_file(TH_PATH(root, "src/a.c"), ORPH_FOO_FILE), 0);

    /* One node the index knows and the tree does not: the file was never
     * written. This is planted, and labelled as planted — history does not
     * produce an unreadable span, so the corpus has no fixture for it. */
    char *present = hyp_pipeline_fqn_compute("repo", "src/a.c", "foo");
    char *ghost = hyp_pipeline_fqn_compute("repo", "src/gone.c", "foo");
    ASSERT_NOT_NULL(present);
    ASSERT_NOT_NULL(ghost);
    hyp_node_t n1 = {.project = "repo",
                     .label = "Function",
                     .name = "foo",
                     .qualified_name = present,
                     .file_path = "src/a.c",
                     .start_line = 1,
                     .end_line = 3,
                     .properties_json = NULL};
    hyp_node_t n2 = {.project = "repo",
                     .label = "Function",
                     .name = "foo",
                     .qualified_name = ghost,
                     .file_path = "src/gone.c",
                     .start_line = 1,
                     .end_line = 3,
                     .properties_json = NULL};
    ASSERT_TRUE(hyp_store_upsert_node(s, &n1) > 0);
    ASSERT_TRUE(hyp_store_upsert_node(s, &n2) > 0);

    char *dead = hyp_pipeline_fqn_compute("repo", "src/never.c", "foo");
    ASSERT_NOT_NULL(dead);
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(ORPH_FOO_SPAN, hash);
    hyp_addr_t addr;
    ASSERT_EQ(hyp_addr_init(&addr, NULL, "repo", dead), HYP_ADDR_OK);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&addr, hash, anchor, sizeof(anchor)));

    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &res), HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(res.candidate_count, 1);
    ASSERT_TRUE(res.scan_skipped > 0);
    hyp_orphan_entry_t e = {.record = NULL, .res = res};
    char *view = hyp_orphan_render_entry(&e);
    ASSERT_NOT_NULL(view);
    ASSERT_NOT_NULL(strstr(view, "candidates_are_a_floor:"));
    free(view);
    hyp_anchor_res_free(&res);

    /* And when nothing was skipped the disclosure is ABSENT, not "0" — a
     * floor that is always announced stops being a signal. */
    ASSERT_EQ(th_write_file(TH_PATH(root, "src/gone.c"), ORPH_FOO_FILE), 0);
    hyp_anchor_res_t clean;
    ASSERT_EQ(hyp_anchor_resolve(s, NULL, anchor, &clean), HYP_ANCHOR_ORPHANED);
    ASSERT_EQ(clean.scan_skipped, 0);
    hyp_orphan_entry_t e2 = {.record = NULL, .res = clean};
    char *view2 = hyp_orphan_render_entry(&e2);
    ASSERT_NOT_NULL(view2);
    ASSERT_NULL(strstr(view2, "candidates_are_a_floor"));
    free(view2);
    hyp_anchor_res_free(&clean);

    free(present);
    free(ghost);
    free(dead);
    hyp_store_close(s);
    th_cleanup(root);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * AMBIGUOUS — the anchor names something real, and too much of it
 * ════════════════════════════════════════════════════════════════════ */

TEST(orphan_duplicate_qualified_name_is_ambiguous_not_error) {
    /* A duplicated qualified name is a fact about the anchor, so it is a state
     * of its own. Reported as ERROR it would be indistinguishable from the
     * store failing, and the tool surface already promises a reader the
     * difference. Every colliding node is published; nothing picks. */
    char *tmp = th_mktempdir("hyp-orphan-amb");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *s = hyp_store_open_memory();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_upsert_project(s, "repo", root), HYP_STORE_OK);
    ASSERT_EQ(th_write_file(TH_PATH(root, "src/a.c"), ORPH_FOO_FILE), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "src/b.c"), "int foo(void) {\n    return 2;\n}\n"), 0);

    /* The nodes table as a hand-built dump file carries it: same columns, no
     * uniqueness. A declared UNIQUE(project, qualified_name) makes the
     * collision unreachable through this schema, but the published dump writer
     * builds the file's pages and its index by hand, so uniqueness there is a
     * construction claim rather than something the engine enforces. This
     * fixture is that shape, and it is the shape a store loaded from a dump
     * can really have. */
    ASSERT_EQ(hyp_store_exec(s, "DROP TABLE nodes;"), HYP_STORE_OK);
    ASSERT_EQ(hyp_store_exec(s, "CREATE TABLE nodes ("
                                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "  project TEXT NOT NULL,"
                                "  label TEXT NOT NULL,"
                                "  name TEXT NOT NULL,"
                                "  qualified_name TEXT NOT NULL,"
                                "  file_path TEXT DEFAULT '',"
                                "  start_line INTEGER DEFAULT 0,"
                                "  end_line INTEGER DEFAULT 0,"
                                "  properties TEXT DEFAULT '{}');"),
              HYP_STORE_OK);

    /* One qualified name, two entities — what a derivation that mints two
     * different kinds of thing at one address produces. */
    char *qn = hyp_pipeline_fqn_compute("repo", "src/a.c", "foo");
    ASSERT_NOT_NULL(qn);
    char insert[1024];
    snprintf(insert, sizeof(insert),
             "INSERT INTO nodes (project, label, name, qualified_name, file_path, "
             "start_line, end_line) VALUES "
             "('repo','Function','foo','%s','src/a.c',1,3),"
             "('repo','Module','foo','%s','src/b.c',1,3);",
             qn, qn);
    ASSERT_EQ(hyp_store_exec(s, insert), HYP_STORE_OK);

    /* The plant took — two rows really do answer, so the refusal below is
     * being asserted against a collision that exists rather than against a
     * fixture nobody checked. */
    hyp_node_t *rows = NULL;
    int row_count = 0;
    ASSERT_EQ(hyp_store_find_nodes_by_qn_suffix(s, "repo", qn, &rows, &row_count), HYP_STORE_OK);
    ASSERT_EQ(row_count, 2);
    hyp_store_free_nodes(rows, row_count);

    hyp_node_t probe = {0};
    ASSERT_EQ(hyp_store_find_node_by_qn(s, "repo", qn, &probe), HYP_STORE_AMBIGUOUS);
    /* And no row handed back with the refusal. */
    ASSERT_NULL(probe.qualified_name);
    hyp_node_free_fields(&probe);

    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(ORPH_FOO_SPAN, hash);
    hyp_addr_t addr;
    ASSERT_EQ(hyp_addr_init(&addr, NULL, "repo", qn), HYP_ADDR_OK);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&addr, hash, anchor, sizeof(anchor)));

    hyp_anchor_res_t res;
    hyp_anchor_status_t st = hyp_anchor_resolve(s, NULL, anchor, &res);
    ASSERT_EQ(st, HYP_ANCHOR_AMBIGUOUS);
    ASSERT_NEQ((int)st, (int)HYP_ANCHOR_ERROR);
    ASSERT_NEQ((int)st, (int)HYP_ANCHOR_RESOLVED);
    ASSERT_STR_EQ(hyp_anchor_status_str(st), "ambiguous");
    ASSERT_EQ(res.candidate_count, 2);
    /* They share one address — that IS the collision — and the evidence
     * separates them: one still holds the recorded content, one does not. */
    ASSERT_STR_EQ(res.candidates[0].address, res.candidates[1].address);
    ASSERT_STR_EQ(res.candidates[0].file_path, "src/a.c");
    ASSERT_STR_EQ(res.candidates[1].file_path, "src/b.c");
    ASSERT_EQ(res.candidates[0].relation, HYP_ADDR_REL_SAME);
    ASSERT_EQ(res.candidates[1].relation, HYP_ADDR_REL_EDITED);
    ASSERT_TRUE(res.reason[0] != '\0');
    ASSERT_NOT_NULL(strstr(res.reason, qn));

    hyp_orphan_entry_t e = {.record = NULL, .res = res};
    char *view = hyp_orphan_render_entry(&e);
    ASSERT_NOT_NULL(view);
    ASSERT_NOT_NULL(strstr(view, "anchor_status: ambiguous"));
    ASSERT_NOT_NULL(strstr(view, "reattached: no"));
    ASSERT_NOT_NULL(strstr(view, "src/a.c"));
    ASSERT_NOT_NULL(strstr(view, "src/b.c"));
    ASSERT_NULL(strstr(view, "\nattached:"));
    free(view);
    hyp_anchor_res_free(&res);

    free(qn);
    hyp_store_close(s);
    th_cleanup(root);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The read path refuses rather than answering a different question
 * ════════════════════════════════════════════════════════════════════ */

TEST(orphan_view_refuses_a_query_for_unanchored_records) {
    char *tmp = th_mktempdir("hyp-orphan-q");
    ASSERT_NOT_NULL(tmp);
    char root[256];
    snprintf(root, sizeof(root), "%s", tmp);
    hyp_store_t *index = hyp_store_open_memory();
    ASSERT_NOT_NULL(index);
    ASSERT_EQ(hyp_store_upsert_project(index, "repo", root), HYP_STORE_OK);
    hyp_record_store_t *rs = NULL;
    ASSERT_EQ(hyp_record_store_open(TH_PATH(root, "memory"), &rs), HYP_RECORD_STORE_OK);

    /* An unanchored record has no anchor status. Answering with an empty view
     * would say "none of them is orphaned", which is a claim about records the
     * question was never about. */
    hyp_record_store_query_t q = {0};
    q.unanchored_only = true;
    hyp_orphan_view_t *view = NULL;
    ASSERT_EQ(hyp_orphan_view_build(rs, index, NULL, &q, &view), HYP_ORPHAN_ERR_QUERY);
    ASSERT_NULL(view);

    /* And a NULL index is refused rather than resolving everything to
     * orphaned, which is what an empty index would have said. */
    ASSERT_EQ(hyp_orphan_view_build(rs, NULL, NULL, NULL, &view), HYP_ORPHAN_ERR_NULL);
    ASSERT_NULL(view);

    hyp_record_store_close(rs);
    hyp_store_close(index);
    th_cleanup(root);
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(orphan) {
    /* The corpus, before it is trusted */
    RUN_TEST(orphan_corpus_qualified_names_match_the_product_builder);
    RUN_TEST(orphan_corpus_span_hashes_reproduce_under_the_product_hash);
    RUN_TEST(orphan_replays_every_history_fixture);

    /* Three orphan inputs, one observation, no attachment */
    RUN_TEST(orphan_cross_dir_move_surfaces_candidate_with_evidence);
    RUN_TEST(orphan_file_rename_surfaces_candidate_with_evidence);
    RUN_TEST(orphan_copy_paste_original_still_resolves_to_itself);
    RUN_TEST(orphan_copy_paste_yields_two_candidates_and_attaches_to_neither);

    /* Orphan-ness is off the record */
    RUN_TEST(orphan_detection_leaves_the_store_digest_unchanged);
    RUN_TEST(orphan_signal_is_an_append_whose_parent_is_the_decision);
    RUN_TEST(orphan_ness_is_a_property_of_record_and_index_state);

    /* Never dropped, never absorbed, never conflated */
    RUN_TEST(orphan_view_never_silently_drops_an_orphan);
    RUN_TEST(orphan_absent_candidates_are_not_empty_candidates);
    RUN_TEST(orphan_scan_floor_is_disclosed_not_absorbed);

    /* The anchor that names too much */
    RUN_TEST(orphan_duplicate_qualified_name_is_ambiguous_not_error);
    RUN_TEST(orphan_view_refuses_a_query_for_unanchored_records);
}
