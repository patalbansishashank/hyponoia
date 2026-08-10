/*
 * test_ask_embed.c — the opt-in embed pass, end to end against a real graph
 * database and a real source tree, driven by the deterministic stub encoder.
 *
 * The two things worth nailing down here are what exactly gets embedded (a
 * different text unit invalidates every recorded number) and that a re-run
 * reuses byte-identical declarations instead of re-encoding them.
 */

#include "test_framework.h"
#include "test_helpers.h"

#include "ask/ask_embed.h"
#include "ask/ask_encoder.h"
#include "ask/ask_vectors.h"

#include <store/store.h>

#include <stdlib.h>
#include <string.h>

/* ── What exactly gets embedded ────────────────────────────────── */

TEST(ask_embed_span_is_verbatim_source_lines) {
    char *dir = th_mktempdir("hyp-askspan");
    ASSERT_NOT_NULL(dir);
    const char *path = TH_PATH(dir, "a.c");
    char kept[512];
    snprintf(kept, sizeof(kept), "%s", path);
    ASSERT_EQ(th_write_file(kept, "line1\nline2\nline3\nline4\n"), 0);

    /* Inclusive on both ends, 1-based, joined with '\n', NO trailing
     * newline — the reference implementation's exact unit. */
    char *s = hyp_ask_read_span(kept, 2, 3);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "line2\nline3");
    free(s);

    /* A single line. */
    s = hyp_ask_read_span(kept, 1, 1);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "line1");
    free(s);

    /* The last line, where the file's own trailing newline must not survive. */
    s = hyp_ask_read_span(kept, 4, 4);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "line4");
    free(s);

    /* A span that starts past the end of the file is unreadable, not empty. */
    ASSERT_NULL(hyp_ask_read_span(kept, 99, 100));
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_span_normalises_crlf_and_keeps_no_trailing_newline) {
    char *dir = th_mktempdir("hyp-askspan");
    ASSERT_NOT_NULL(dir);
    const char *path = TH_PATH(dir, "b.c");
    char kept[512];
    snprintf(kept, sizeof(kept), "%s", path);
    ASSERT_EQ(th_write_file(kept, "alpha\r\nbeta\r\ngamma\r\n"), 0);
    char *s = hyp_ask_read_span(kept, 1, 2);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "alpha\nbeta");
    free(s);
    s = hyp_ask_read_span(kept, 3, 3);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "gamma");
    free(s);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_content_hash_is_stable_and_32_hex) {
    char h1[HYP_ASK_VEC_HASH_LEN + 1];
    char h2[HYP_ASK_VEC_HASH_LEN + 1];
    hyp_ask_content_hash("void f(void) {}", h1);
    hyp_ask_content_hash("void f(void) {}", h2);
    ASSERT_STR_EQ(h1, h2);
    ASSERT_EQ((int)strlen(h1), HYP_ASK_VEC_HASH_LEN);
    hyp_ask_content_hash("void g(void) {}", h2);
    ASSERT_STR_NEQ(h1, h2);
    PASS();
}

/* ── End to end ───────────────────────────────────────────────── */

/* Build a graph database with three declarations over one source file, plus a
 * File node with no span (which must NOT be embedded — the exclusion is a span
 * test, never a kind test). */
static int build_fixture(const char *dir, const char *graph_db, const char *project) {
    char src[512];
    snprintf(src, sizeof(src), "%s/src", dir);
    if (th_mkdir_p(src) != 0) {
        return -1;
    }
    char file[600];
    snprintf(file, sizeof(file), "%s/x.c", src);
    if (th_write_file(file,
                      "int alpha(void) {\n"
                      "    return 1;\n"
                      "}\n"
                      "int beta(void) {\n"
                      "    return 2;\n"
                      "}\n"
                      "struct Gamma {\n"
                      "    int f;\n"
                      "};\n") != 0) {
        return -1;
    }
    hyp_store_t *s = hyp_store_open_path(graph_db);
    if (!s) {
        return -1;
    }
    if (hyp_store_upsert_project(s, project, dir) != HYP_STORE_OK) {
        hyp_store_close(s);
        return -1;
    }
    hyp_node_t nodes[4];
    memset(nodes, 0, sizeof(nodes));
    nodes[0] = (hyp_node_t){.project = project,
                            .label = "Function",
                            .name = "alpha",
                            .qualified_name = "p.alpha",
                            .file_path = "src/x.c",
                            .start_line = 1,
                            .end_line = 3};
    nodes[1] = (hyp_node_t){.project = project,
                            .label = "Function",
                            .name = "beta",
                            .qualified_name = "p.beta",
                            .file_path = "src/x.c",
                            .start_line = 4,
                            .end_line = 6};
    /* A struct. It has a span, so it is embedded — §2.1 forbids filtering by
     * node kind, and this is exactly the kind the static path could never
     * return. */
    nodes[2] = (hyp_node_t){.project = project,
                            .label = "Struct",
                            .name = "Gamma",
                            .qualified_name = "p.Gamma",
                            .file_path = "src/x.c",
                            .start_line = 7,
                            .end_line = 9};
    /* A File node: no span, so nothing to embed. */
    nodes[3] = (hyp_node_t){.project = project,
                            .label = "File",
                            .name = "x.c",
                            .qualified_name = "p.file.x_c",
                            .file_path = "src/x.c",
                            .start_line = 0,
                            .end_line = 0};
    int rc = hyp_store_upsert_node_batch(s, nodes, 4, NULL);
    hyp_store_close(s);
    return rc == HYP_STORE_OK ? 0 : -1;
}

TEST(ask_embed_runs_end_to_end_and_embeds_every_spanned_node) {
    char *dir = th_mktempdir("hyp-askembed");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_fixture(dir, graph_db, "p") != 0) {
        th_cleanup(dir);
        FAIL("could not build the graph fixture");
    }

    hyp_ask_encoder_t *enc = hyp_ask_encoder_stub_create(16, 32768, true);
    ASSERT_NOT_NULL(enc);
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.project = "p";
    opts.graph_db_path = graph_db;
    opts.vectors_db_path = vec_db;
    hyp_ask_embed_report_t rep;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &rep), 0);

    /* Three spanned declarations; the File node has no span and is not one. */
    ASSERT_EQ(rep.declarations_seen, 3);
    ASSERT_EQ(rep.embedded, 3);
    ASSERT_EQ(rep.reused, 0);
    ASSERT_EQ(rep.skipped_unreadable, 0);
    ASSERT_GT(rep.forward_passes, 0);
    ASSERT(rep.truncation_known);
    ASSERT_EQ(rep.truncated, 0);

    /* The stored hash must be the hash of the VERBATIM SPAN, not of anything
     * assembled from the name, the path or the kind. */
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_count(v), 3);
    char expect[HYP_ASK_VEC_HASH_LEN + 1];
    hyp_ask_content_hash("int alpha(void) {\n    return 1;\n}", expect);
    char stored[HYP_ASK_VEC_HASH_LEN + 1];
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.alpha", stored, NULL), HYP_ASK_VEC_OK);
    ASSERT_STR_EQ(stored, expect);
    /* The struct is there — the population this pass newly makes retrievable. */
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.Gamma", stored, NULL), HYP_ASK_VEC_OK);
    /* The File node is not. */
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.file.x_c", stored, NULL),
              HYP_ASK_VEC_NOT_FOUND);
    hyp_ask_vectors_close(v);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_reuses_byte_identical_declarations_on_a_rerun) {
    char *dir = th_mktempdir("hyp-askembed");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_fixture(dir, graph_db, "p") != 0) {
        th_cleanup(dir);
        FAIL("could not build the graph fixture");
    }
    hyp_ask_encoder_t *enc = hyp_ask_encoder_stub_create(16, 32768, true);
    ASSERT_NOT_NULL(enc);
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.project = "p";
    opts.graph_db_path = graph_db;
    opts.vectors_db_path = vec_db;
    hyp_ask_embed_report_t first;
    hyp_ask_embed_report_t second;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &first), 0);
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &second), 0);
    ASSERT_EQ(second.embedded, 0);
    ASSERT_EQ(second.reused, 3);
    ASSERT_EQ(second.forward_passes, 0);

    /* --force re-encodes anyway, which is how the reuse path is proved to be
     * an optimisation rather than the only path. */
    opts.force = true;
    hyp_ask_embed_report_t third;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &third), 0);
    ASSERT_EQ(third.embedded, 3);
    ASSERT_EQ(third.reused, 0);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_prunes_declarations_that_no_longer_exist) {
    char *dir = th_mktempdir("hyp-askembed");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_fixture(dir, graph_db, "p") != 0) {
        th_cleanup(dir);
        FAIL("could not build the graph fixture");
    }
    hyp_ask_encoder_t *enc = hyp_ask_encoder_stub_create(16, 32768, true);
    ASSERT_NOT_NULL(enc);
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.project = "p";
    opts.graph_db_path = graph_db;
    opts.vectors_db_path = vec_db;
    hyp_ask_embed_report_t rep;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &rep), 0);
    ASSERT_EQ(rep.embedded, 3);

    /* Remove one declaration from the graph and re-run. */
    hyp_store_t *s = hyp_store_open_path(graph_db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_exec(s, "DELETE FROM nodes WHERE qualified_name = 'p.beta';"),
              HYP_STORE_OK);
    hyp_store_close(s);

    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &rep), 0);
    ASSERT_EQ(rep.declarations_seen, 2);
    ASSERT_EQ(rep.pruned, 1);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_count(v), 2);
    hyp_ask_vectors_close(v);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_limited_run_is_marked_partial_and_never_prunes) {
    /* A run that stopped early has no opinion about the declarations it never
     * reached, so it must not delete their rows. */
    char *dir = th_mktempdir("hyp-askembed");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_fixture(dir, graph_db, "p") != 0) {
        th_cleanup(dir);
        FAIL("could not build the graph fixture");
    }
    hyp_ask_encoder_t *enc = hyp_ask_encoder_stub_create(16, 32768, true);
    ASSERT_NOT_NULL(enc);
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.project = "p";
    opts.graph_db_path = graph_db;
    opts.vectors_db_path = vec_db;
    hyp_ask_embed_report_t full;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &full), 0);
    ASSERT_EQ(full.embedded, 3);

    opts.limit = 1;
    opts.force = true;
    hyp_ask_embed_report_t part;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &part), 0);
    ASSERT(part.partial);
    ASSERT_EQ(part.pruned, 0);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_count(v), 3);
    hyp_ask_vectors_close(v);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_truncation_unknown_then_reprobed_to_attested) {
    /* The three states, exercised through the pass rather than the store.
     * An encoder that cannot report token counts leaves the index UNKNOWN; a
     * later run with one that can must RE-PROBE the reused rows rather than
     * republishing an unattested zero as an attested one. */
    char *dir = th_mktempdir("hyp-asktrunc");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_fixture(dir, graph_db, "p") != 0) {
        th_cleanup(dir);
        FAIL("could not build the graph fixture");
    }
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.project = "p";
    opts.graph_db_path = graph_db;
    opts.vectors_db_path = vec_db;

    /* A tiny window makes every declaration overflow, so the counter has
     * something real to count. The encoder cannot report, so the state is
     * UNKNOWN even though rows would qualify. */
    hyp_ask_encoder_t *quiet = hyp_ask_encoder_stub_create(16, 4, false);
    ASSERT_NOT_NULL(quiet);
    hyp_ask_embed_report_t r1;
    ASSERT_EQ(hyp_ask_embed_run(quiet, &opts, &r1), 0);
    ASSERT_EQ(r1.truncation_known, false);
    ASSERT_EQ(r1.truncated, 0);
    hyp_ask_encoder_destroy(quiet);

    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    hyp_ask_trunc_t t;
    ASSERT_EQ(hyp_ask_vectors_truncation(v, &t), HYP_ASK_VEC_OK);
    ASSERT_EQ(t.state, HYP_ASK_TRUNC_UNKNOWN);
    hyp_ask_trunc_free(&t);
    hyp_ask_vectors_close(v);

    /* Same model id, same dim, same window — so the vectors are reusable — but
     * this encoder CAN report. Nothing is re-encoded; the disclosure is
     * repaired. */
    hyp_ask_encoder_t *loud = hyp_ask_encoder_stub_create(16, 4, true);
    ASSERT_NOT_NULL(loud);
    hyp_ask_embed_report_t r2;
    ASSERT_EQ(hyp_ask_embed_run(loud, &opts, &r2), 0);
    ASSERT_EQ(r2.embedded, 0);
    ASSERT_EQ(r2.reused, 3);
    ASSERT(r2.truncation_known);
    ASSERT_EQ(r2.truncated, 3);
    hyp_ask_encoder_destroy(loud);

    v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_truncation(v, &t), HYP_ASK_VEC_OK);
    ASSERT_EQ(t.state, HYP_ASK_TRUNC_SOME);
    ASSERT_EQ(t.count, 3);
    char line[512];
    hyp_ask_trunc_describe(&t, line, sizeof(line));
    ASSERT(strstr(line, "FROM THEIR FIRST TOKENS ONLY") != NULL);
    hyp_ask_trunc_free(&t);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_counts_unreadable_spans_instead_of_hiding_them) {
    /* The reference implementation skips unreadable files silently. The
     * difference between "this corpus has no declaration for X" and "the file
     * holding X could not be read" is exactly the distinction the rest of this
     * engine spends its effort preserving, so it is counted. */
    char *dir = th_mktempdir("hyp-askembed");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_fixture(dir, graph_db, "p") != 0) {
        th_cleanup(dir);
        FAIL("could not build the graph fixture");
    }
    /* A declaration whose file is not there at all. */
    hyp_store_t *s = hyp_store_open_path(graph_db);
    ASSERT_NOT_NULL(s);
    hyp_node_t ghost = {.project = "p",
                        .label = "Function",
                        .name = "ghost",
                        .qualified_name = "p.ghost",
                        .file_path = "src/missing.c",
                        .start_line = 1,
                        .end_line = 5};
    ASSERT_NEQ(hyp_store_upsert_node(s, &ghost), HYP_STORE_ERR);
    hyp_store_close(s);

    hyp_ask_encoder_t *enc = hyp_ask_encoder_stub_create(16, 32768, true);
    ASSERT_NOT_NULL(enc);
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.project = "p";
    opts.graph_db_path = graph_db;
    opts.vectors_db_path = vec_db;
    hyp_ask_embed_report_t rep;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &rep), 0);
    ASSERT_EQ(rep.declarations_seen, 4);
    ASSERT_EQ(rep.embedded, 3);
    ASSERT_EQ(rep.skipped_unreadable, 1);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_zero_work_is_not_reported_as_success) {
    /* A run that found no declaration completed everything it attempted and
     * indexed nothing. If that returned 0 like any other run, "measured
     * nothing" and "measured fine" would be the same answer — which is exactly
     * how a silent no-op survives a green run. It gets its own code. */
    char *dir = th_mktempdir("hyp-asknowork");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_fixture(dir, graph_db, "p") != 0) {
        th_cleanup(dir);
        FAIL("could not build the graph fixture");
    }
    hyp_ask_encoder_t *enc = hyp_ask_encoder_stub_create(16, 32768, true);
    ASSERT_NOT_NULL(enc);
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    /* A project the graph does not contain: every query returns nothing and
     * every step succeeds. */
    opts.project = "not-a-project";
    opts.repo_path = dir;
    opts.graph_db_path = graph_db;
    opts.vectors_db_path = vec_db;
    hyp_ask_embed_report_t rep;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &rep), HYP_ASK_EMBED_NO_WORK);
    ASSERT_EQ(rep.declarations_seen, 0);
    ASSERT_EQ(rep.embedded, 0);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

SUITE(ask_embed) {
    RUN_TEST(ask_embed_span_is_verbatim_source_lines);
    RUN_TEST(ask_embed_span_normalises_crlf_and_keeps_no_trailing_newline);
    RUN_TEST(ask_embed_content_hash_is_stable_and_32_hex);
    RUN_TEST(ask_embed_runs_end_to_end_and_embeds_every_spanned_node);
    RUN_TEST(ask_embed_reuses_byte_identical_declarations_on_a_rerun);
    RUN_TEST(ask_embed_prunes_declarations_that_no_longer_exist);
    RUN_TEST(ask_embed_limited_run_is_marked_partial_and_never_prunes);
    RUN_TEST(ask_embed_truncation_unknown_then_reprobed_to_attested);
    RUN_TEST(ask_embed_counts_unreadable_spans_instead_of_hiding_them);
    RUN_TEST(ask_embed_zero_work_is_not_reported_as_success);
}
