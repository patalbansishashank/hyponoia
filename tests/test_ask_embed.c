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

#include <sqlite3.h>

#include <math.h>
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
    hyp_ask_embed_report_free(&rep);
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
    hyp_ask_embed_report_free(&first);
    hyp_ask_embed_report_free(&second);
    hyp_ask_embed_report_free(&third);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

/* ── The pull, at file granularity (NEXT-STEPS §4 F2) ──────────── */

/* Twelve files, two declarations each. The clause under test is the
 * GRANULARITY of reuse: "a pull touching twelve files re-embeds twelve files'
 * worth of spans, not the corpus". Here the pull touches 3 of the 12 — every
 * span in a touched file changes, nothing else does — and between the two runs
 * every node id in the graph is re-minted the way a real re-index re-mints
 * them, so the same run also proves the reuse key is (qualified_name,
 * content_hash) and never the node id. */
enum { F2_FILES = 12, F2_SPANS_PER_FILE = 2, F2_TOUCHED = 3 };

/* The span text of declaration `a` or `b` in file `idx`, as the pass reads
 * it: three lines, joined with '\n', no trailing newline. One producer for
 * both the fixture and the expected hashes, so they cannot drift apart. */
static void f2_span_text(int idx, char which, bool touched, char *buf, size_t bufsz) {
    int value = which == 'a' ? (touched ? 900 + idx : idx) : (touched ? 800 + idx : idx + 1);
    snprintf(buf, bufsz,
             "int %c%02d(void) {\n"
             "    return %d;\n"
             "}",
             which, idx, value);
}

static int f2_write_source(const char *dir, int idx, bool touched) {
    char path[600];
    snprintf(path, sizeof(path), "%s/src/f%02d.c", dir, idx);
    char a[128];
    char b[128];
    f2_span_text(idx, 'a', touched, a, sizeof(a));
    f2_span_text(idx, 'b', touched, b, sizeof(b));
    char body[512];
    /* The touch edits BYTES, not geometry: both shapes are lines 1-3 and 4-6,
     * so the graph rows stay accurate without a re-extract. */
    snprintf(body, sizeof(body), "%s\n%s\n", a, b);
    return th_write_file(path, body);
}

static int f2_build_fixture(const char *dir, const char *graph_db, const char *project) {
    char src[512];
    snprintf(src, sizeof(src), "%s/src", dir);
    if (th_mkdir_p(src) != 0) {
        return -1;
    }
    for (int i = 0; i < F2_FILES; i++) {
        if (f2_write_source(dir, i, false) != 0) {
            return -1;
        }
    }
    hyp_store_t *s = hyp_store_open_path(graph_db);
    if (!s) {
        return -1;
    }
    if (hyp_store_upsert_project(s, project, dir) != HYP_STORE_OK) {
        hyp_store_close(s);
        return -1;
    }
    hyp_node_t nodes[F2_FILES * F2_SPANS_PER_FILE];
    char qn[F2_FILES * F2_SPANS_PER_FILE][32];
    char nm[F2_FILES * F2_SPANS_PER_FILE][16];
    char fp[F2_FILES][32];
    memset(nodes, 0, sizeof(nodes));
    for (int i = 0; i < F2_FILES; i++) {
        snprintf(fp[i], sizeof(fp[i]), "src/f%02d.c", i);
        for (int j = 0; j < F2_SPANS_PER_FILE; j++) {
            int k = (i * F2_SPANS_PER_FILE) + j;
            snprintf(nm[k], sizeof(nm[k]), "%c%02d", j == 0 ? 'a' : 'b', i);
            snprintf(qn[k], sizeof(qn[k]), "p.f%02d.%s", i, nm[k]);
            nodes[k] = (hyp_node_t){.project = project,
                                    .label = "Function",
                                    .name = nm[k],
                                    .qualified_name = qn[k],
                                    .file_path = fp[i],
                                    .start_line = j == 0 ? 1 : 4,
                                    .end_line = j == 0 ? 3 : 6};
        }
    }
    int rc = hyp_store_upsert_node_batch(s, nodes, F2_FILES * F2_SPANS_PER_FILE, NULL);
    hyp_store_close(s);
    return rc == HYP_STORE_OK ? 0 : -1;
}

TEST(ask_embed_touching_three_of_twelve_files_reembeds_their_spans_not_the_corpus) {
    char *dir = th_mktempdir("hyp-askpull");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (f2_build_fixture(dir, graph_db, "p") != 0) {
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
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &first), 0);
    ASSERT_EQ(first.declarations_seen, F2_FILES * F2_SPANS_PER_FILE);
    ASSERT_EQ(first.embedded, F2_FILES * F2_SPANS_PER_FILE);
    ASSERT_EQ(first.reused, 0);
    hyp_ask_embed_report_free(&first);

    /* The pull: three files change, nine do not. */
    for (int i = 0; i < F2_TOUCHED; i++) {
        ASSERT_EQ(f2_write_source(dir, i, true), 0);
    }
    /* The re-index that follows a pull re-mints EVERY node id, touched or
     * not. Reuse must not notice, because the key is the content hash under
     * the qualified name — a reuse path keyed on node id would re-embed the
     * whole corpus here and this test would count it. */
    hyp_store_t *s = hyp_store_open_path(graph_db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_exec(s, "UPDATE nodes SET id = id + 1000;"), HYP_STORE_OK);
    hyp_store_close(s);

    hyp_ask_embed_report_t second;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &second), 0);
    ASSERT_EQ(second.declarations_seen, F2_FILES * F2_SPANS_PER_FILE);
    /* THE CLAUSE: three files' worth of spans re-embed; the rest are reused. */
    ASSERT_EQ(second.embedded, F2_TOUCHED * F2_SPANS_PER_FILE);
    ASSERT_EQ(second.reused, (F2_FILES - F2_TOUCHED) * F2_SPANS_PER_FILE);
    ASSERT_EQ(second.pruned, 0);
    ASSERT_EQ(second.skipped_unreadable, 0);
    hyp_ask_embed_report_free(&second);

    /* The stored hashes agree: a touched span carries the NEW text's hash, an
     * untouched span still carries the original's. */
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_count(v), F2_FILES * F2_SPANS_PER_FILE);
    char text[128];
    char expect[HYP_ASK_VEC_HASH_LEN + 1];
    char stored[HYP_ASK_VEC_HASH_LEN + 1];
    f2_span_text(0, 'a', true, text, sizeof(text));
    hyp_ask_content_hash(text, expect);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.f00.a00", stored, NULL), HYP_ASK_VEC_OK);
    ASSERT_STR_EQ(stored, expect);
    f2_span_text(5, 'a', false, text, sizeof(text));
    hyp_ask_content_hash(text, expect);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.f05.a05", stored, NULL), HYP_ASK_VEC_OK);
    ASSERT_STR_EQ(stored, expect);
    hyp_ask_vectors_close(v);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_reuse_recites_a_span_that_moved_without_changing) {
    /* THE DRIFT RISK the F2 audit found, pinned. A pull that inserts lines
     * above a declaration moves its span and re-mints its node id while
     * leaving its bytes alone — the hash matches, so the vector is reused —
     * and the read path cites file and lines STRAIGHT OFF the vector row
     * (store/ask_index.c converts hits without touching the graph). A reused
     * row must therefore cite where the span IS, or every embed run after a
     * pull republishes citations to lines the checkout no longer has. */
    char *dir = th_mktempdir("hyp-askmove");
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
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &first), 0);
    ASSERT_EQ(first.embedded, 3);
    hyp_ask_embed_report_free(&first);

    /* Two new lines of preamble: every declaration slides down two lines,
     * byte-for-byte unchanged. The re-index records the new spans and
     * re-mints the ids; the source matches it. */
    char file[600];
    snprintf(file, sizeof(file), "%s/src/x.c", dir);
    ASSERT_EQ(th_write_file(file,
                            "/* two new lines\n"
                            " * of preamble */\n"
                            "int alpha(void) {\n"
                            "    return 1;\n"
                            "}\n"
                            "int beta(void) {\n"
                            "    return 2;\n"
                            "}\n"
                            "struct Gamma {\n"
                            "    int f;\n"
                            "};\n"),
              0);
    hyp_store_t *s = hyp_store_open_path(graph_db);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_exec(s, "UPDATE nodes SET id = id + 500,"
                                "  start_line = start_line + 2, end_line = end_line + 2"
                                " WHERE start_line > 0;"),
              HYP_STORE_OK);
    /* What the graph now says alpha's id is — the id the citation must carry. */
    sqlite3_stmt *st = NULL;
    int64_t alpha_id = 0;
    ASSERT_EQ(sqlite3_prepare_v2(hyp_store_get_db(s),
                                 "SELECT id FROM nodes WHERE qualified_name = 'p.alpha'", -1, &st,
                                 NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    alpha_id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    hyp_store_close(s);
    ASSERT_GT(alpha_id, 0);

    hyp_ask_embed_report_t second;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &second), 0);
    /* Nothing re-encodes — the bytes did not change... */
    ASSERT_EQ(second.embedded, 0);
    ASSERT_EQ(second.reused, 3);
    hyp_ask_embed_report_free(&second);

    /* ...and the CITATION is current anyway. Asserted through search — the
     * same rows the read path converts into an answer's file:line cite. */
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    float q[16] = {0};
    q[0] = 1.0F;
    hyp_ask_vec_hit_t *hits = NULL;
    int n = 0;
    ASSERT_EQ(hyp_ask_vectors_search(v, q, 16, 10, &hits, &n), HYP_ASK_VEC_OK);
    ASSERT_EQ(n, 3);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(hits[i].qualified_name, "p.alpha") != 0) {
            continue;
        }
        found = true;
        ASSERT_STR_EQ(hits[i].file_path, "src/x.c");
        ASSERT_EQ(hits[i].start_line, 3);
        ASSERT_EQ(hits[i].end_line, 5);
        ASSERT_EQ(hits[i].node_id, alpha_id);
    }
    ASSERT(found);
    hyp_ask_vec_hits_free(hits, n);
    hyp_ask_vectors_close(v);
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
    hyp_ask_embed_report_free(&rep);

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
    hyp_ask_embed_report_free(&rep);
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
    hyp_ask_embed_report_free(&full);
    hyp_ask_embed_report_free(&part);
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
    hyp_ask_embed_report_free(&r1);
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
    hyp_ask_embed_report_free(&r2);
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
    hyp_ask_embed_report_free(&rep);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

/* ── Whole-file spans (NEXT-STEPS §2.2 lever 3) ────────────────── */

TEST(ask_embed_span_reports_the_files_line_count) {
    char *dir = th_mktempdir("hyp-askwf");
    ASSERT_NOT_NULL(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/a.c", dir);
    ASSERT_EQ(th_write_file(path, "one\ntwo\nthree\n"), 0);
    int lines = -1;
    char *s = hyp_ask_read_span_lines(path, 2, 2, &lines);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "two");
    ASSERT_EQ(lines, 3);
    free(s);

    /* A last line with no terminator still counts, or the extractor's end row
     * would look like it overshot a file it exactly covers. */
    snprintf(path, sizeof(path), "%s/b.c", dir);
    ASSERT_EQ(th_write_file(path, "one\ntwo"), 0);
    s = hyp_ask_read_span_lines(path, 1, 2, &lines);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(lines, 2);
    free(s);

    ASSERT(hyp_ask_span_is_whole_file(1, 3, 3));
    ASSERT(hyp_ask_span_is_whole_file(1, 4, 3)); /* claims more than it needs */
    ASSERT(!hyp_ask_span_is_whole_file(2, 3, 3));
    ASSERT(!hyp_ask_span_is_whole_file(1, 2, 3));
    ASSERT(!hyp_ask_span_is_whole_file(1, 1, 0)); /* an empty file has none */
    th_cleanup(dir);
    PASS();
}

/* Three files, one of each shape the policy has to tell apart:
 *   whole.c   a whole-file Module span sitting over a real declaration
 *   only.txt  a whole-file Module span that is the file's ONLY row
 *   ns.ts     a Module span that is NOT the whole file (a TS namespace), over
 *             a file that has another row, so the sole-row exemption cannot be
 *             what keeps it. */
static int build_wholefile_fixture(const char *dir, const char *graph_db, const char *project) {
    char src[512];
    snprintf(src, sizeof(src), "%s/src", dir);
    if (th_mkdir_p(src) != 0) {
        return -1;
    }
    char file[600];
    snprintf(file, sizeof(file), "%s/whole.c", src);
    if (th_write_file(file, "int alpha(void) {\n"
                            "    return 1;\n"
                            "}\n"
                            "static int spare = 2;\n") != 0) {
        return -1;
    }
    snprintf(file, sizeof(file), "%s/only.txt", src);
    if (th_write_file(file, "set(SOURCES a.c)\nadd_library(x)\n") != 0) {
        return -1;
    }
    snprintf(file, sizeof(file), "%s/ns.ts", src);
    if (th_write_file(file, "import x from 'y';\n"
                            "namespace N {\n"
                            "  export const k = 1;\n"
                            "}\n"
                            "function tail() {}\n") != 0) {
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
    hyp_node_t nodes[6];
    memset(nodes, 0, sizeof(nodes));
    nodes[0] = (hyp_node_t){.project = project,
                            .label = "Module",
                            .name = "whole.c",
                            .qualified_name = "p.whole",
                            .file_path = "src/whole.c",
                            .start_line = 1,
                            .end_line = 4};
    nodes[1] = (hyp_node_t){.project = project,
                            .label = "Function",
                            .name = "alpha",
                            .qualified_name = "p.whole.alpha",
                            .file_path = "src/whole.c",
                            .start_line = 1,
                            .end_line = 3};
    nodes[2] = (hyp_node_t){.project = project,
                            .label = "Module",
                            .name = "only.txt",
                            .qualified_name = "p.only",
                            .file_path = "src/only.txt",
                            .start_line = 1,
                            .end_line = 2};
    nodes[3] = (hyp_node_t){.project = project,
                            .label = "Module",
                            .name = "N",
                            .qualified_name = "p.ns.N",
                            .file_path = "src/ns.ts",
                            .start_line = 2,
                            .end_line = 4};
    nodes[4] = (hyp_node_t){.project = project,
                            .label = "Function",
                            .name = "tail",
                            .qualified_name = "p.ns.tail",
                            .file_path = "src/ns.ts",
                            .start_line = 5,
                            .end_line = 5};
    /* The per-file Module of ns.ts, so the file is not a special case. */
    nodes[5] = (hyp_node_t){.project = project,
                            .label = "Module",
                            .name = "ns.ts",
                            .qualified_name = "p.ns",
                            .file_path = "src/ns.ts",
                            .start_line = 1,
                            .end_line = 5};
    int rc = hyp_store_upsert_node_batch(s, nodes, 6, NULL);
    hyp_store_close(s);
    return rc == HYP_STORE_OK ? 0 : -1;
}

TEST(ask_embed_drops_whole_file_spans_by_default) {
    char *dir = th_mktempdir("hyp-askwf");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_wholefile_fixture(dir, graph_db, "p") != 0) {
        th_cleanup(dir);
        FAIL("could not build the graph fixture");
    }
    hyp_ask_encoder_t *enc = hyp_ask_encoder_stub_create(16, 32768, true);
    ASSERT_NOT_NULL(enc);
    hyp_ask_embed_opts_t opts;
    memset(&opts, 0, sizeof(opts)); /* zeroed options select DROP */
    opts.project = "p";
    opts.graph_db_path = graph_db;
    opts.vectors_db_path = vec_db;
    hyp_ask_embed_report_t rep;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &rep), 0);

    ASSERT_EQ(rep.declarations_seen, 6);
    /* whole.c's and ns.ts's per-file rows go; only.txt's stays because it is
     * the file's only row. */
    ASSERT_EQ(rep.skipped_whole_file, 2);
    ASSERT_EQ(rep.whole_file_kept_sole, 1);
    ASSERT_EQ(rep.embedded, 4);
    ASSERT_EQ(rep.skipped_unreadable, 0);
    hyp_ask_embed_report_free(&rep);

    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    char stored[HYP_ASK_VEC_HASH_LEN + 1];
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.whole", stored, NULL), HYP_ASK_VEC_NOT_FOUND);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.ns", stored, NULL), HYP_ASK_VEC_NOT_FOUND);
    /* Kept: the declaration inside the dropped span, the file with nothing
     * else, and the namespace that is a Module but not a file. */
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.whole.alpha", stored, NULL), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.only", stored, NULL), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.ns.N", stored, NULL), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.ns.tail", stored, NULL), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_count(v), 4);

    /* The index says which population it holds — two indexes over one corpus
     * under the two policies rank different candidate sets. */
    hyp_ask_vec_meta_t m;
    ASSERT_EQ(hyp_ask_vectors_get_meta(v, &m), HYP_ASK_VEC_OK);
    ASSERT_NOT_NULL(m.whole_file_spans);
    ASSERT_STR_EQ(m.whole_file_spans, "drop");
    hyp_ask_vec_meta_free(&m);
    hyp_ask_vectors_close(v);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

TEST(ask_embed_keeps_whole_file_spans_when_asked) {
    char *dir = th_mktempdir("hyp-askwf");
    ASSERT_NOT_NULL(dir);
    char graph_db[512];
    char vec_db[512];
    snprintf(graph_db, sizeof(graph_db), "%s/graph.db", dir);
    snprintf(vec_db, sizeof(vec_db), "%s/vec.db", dir);
    if (build_wholefile_fixture(dir, graph_db, "p") != 0) {
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
    opts.whole_file_spans = HYP_ASK_WHOLE_FILE_KEEP;
    hyp_ask_embed_report_t rep;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &rep), 0);
    ASSERT_EQ(rep.embedded, 6);
    ASSERT_EQ(rep.skipped_whole_file, 0);
    ASSERT_EQ(rep.whole_file_kept_sole, 0);
    hyp_ask_embed_report_free(&rep);

    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    char stored[HYP_ASK_VEC_HASH_LEN + 1];
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.whole", stored, NULL), HYP_ASK_VEC_OK);
    hyp_ask_vec_meta_t m;
    ASSERT_EQ(hyp_ask_vectors_get_meta(v, &m), HYP_ASK_VEC_OK);
    ASSERT_STR_EQ(m.whole_file_spans, "keep");
    hyp_ask_vec_meta_free(&m);
    hyp_ask_vectors_close(v);

    /* FLIPPING THE KNOB DOES NOT NEED A REBUILD. The pass prunes what it did
     * not see, so a second run under DROP removes the rows the first wrote and
     * re-encodes nothing else. This is what makes the decision revisitable. */
    opts.whole_file_spans = HYP_ASK_WHOLE_FILE_DROP;
    hyp_ask_embed_report_t second;
    ASSERT_EQ(hyp_ask_embed_run(enc, &opts, &second), 0);
    ASSERT_EQ(second.embedded, 0);
    ASSERT_EQ(second.reused, 4);
    ASSERT_EQ(second.skipped_whole_file, 2);
    ASSERT_EQ(second.pruned, 2);
    hyp_ask_embed_report_free(&second);

    v = hyp_ask_vectors_open_path("p", vec_db);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_count(v), 4);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.whole", stored, NULL), HYP_ASK_VEC_NOT_FOUND);
    ASSERT_EQ(hyp_ask_vectors_get_meta(v, &m), HYP_ASK_VEC_OK);
    ASSERT_STR_EQ(m.whole_file_spans, "drop");
    hyp_ask_vec_meta_free(&m);
    hyp_ask_vectors_close(v);
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
    hyp_ask_embed_report_free(&rep);
    hyp_ask_encoder_destroy(enc);
    th_cleanup(dir);
    PASS();
}

/* ── The batched-versus-alone self-check ───────────────────────── */

/* Sixteen texts of different byte lengths, so the stub's byte-based token
 * counts make the group ragged the way a real length-sorted group is. */
static const char *const CHECK_TEXTS[] = {
    "a",
    "bb",
    "ccc",
    "dddd",
    "eeeee",
    "ffffff",
    "ggggggg",
    "hhhhhhhh",
    "iiiiiiiii",
    "jjjjjjjjjj",
    "kkkkkkkkkkk",
    "llllllllllll",
    "mmmmmmmmmmmmm",
    "nnnnnnnnnnnnnn",
    "ooooooooooooooo",
    "pppppppppppppppp",
};
enum { CHECK_TEXTS_N = (int)(sizeof(CHECK_TEXTS) / sizeof(CHECK_TEXTS[0])) };

TEST(ask_encoder_check_batching_passes_a_batch_independent_encoder) {
    /* The stub hashes each text on its own: what shares its forward pass
     * cannot reach it, so alone and batched are bit-identical, and the check
     * must say so — every row at exactly 1.0, nothing misassigned. */
    hyp_ask_encoder_t *enc = hyp_ask_encoder_stub_create(16, 32768, true);
    ASSERT_NOT_NULL(enc);
    hyp_ask_batch_row_t rows[CHECK_TEXTS_N];
    hyp_ask_batch_check_t chk;
    ASSERT_EQ(
        hyp_ask_encoder_check_batching(enc, CHECK_TEXTS, CHECK_TEXTS_N, 16, 0.999F, rows, &chk), 0);
    ASSERT_EQ(chk.n, CHECK_TEXTS_N);
    ASSERT_EQ(chk.groups, 1);
    ASSERT_EQ(chk.below, 0);
    ASSERT_EQ(chk.misassigned, 0);
    ASSERT_FLOAT_EQ((double)chk.min_own, 1.0, 1e-6);
    for (int i = 0; i < CHECK_TEXTS_N; i++) {
        ASSERT_FLOAT_EQ((double)rows[i].own, 1.0, 1e-6);
        ASSERT_TRUE(rows[i].best_other < rows[i].own);
    }
    /* Groups of 5 over 16 texts: 4 forward passes, last one of a single row
     * whose best_other is then -1 (nobody shared its pass). */
    ASSERT_EQ(
        hyp_ask_encoder_check_batching(enc, CHECK_TEXTS, CHECK_TEXTS_N, 5, 0.999F, rows, &chk), 0);
    ASSERT_EQ(chk.groups, 4);
    ASSERT_EQ(chk.below, 0);
    ASSERT_EQ(rows[CHECK_TEXTS_N - 1].best_other_idx, -1);
    hyp_ask_encoder_destroy(enc);
    PASS();
}

/* An encoder double that is WRONG in the two ways the check tells apart.
 *
 * mode 1 (contaminated): in a group of more than one, every row but the
 *   shortest text is blended with the group — the exact shape of the
 *   split_equal failure the check was written for (ask_llama.c, cp.kv_unified).
 * mode 2 (misassigned): in a group of more than one, the first two rows are
 *   swapped — a seq_id/slot mapping bug. Alone, both modes are correct. */
typedef struct {
    int mode;
} wrong_enc_t;

static void wrong_axis(const char *text, float *out) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h = h * 33u + *p;
    }
    memset(out, 0, 16 * sizeof(float));
    out[h % 16u] = 1.0f;
}

static int wrong_encode_documents(void *self, const char *const *texts, int count, float *out) {
    wrong_enc_t *w = (wrong_enc_t *)self;
    for (int i = 0; i < count; i++) {
        wrong_axis(texts[i], out + (size_t)i * 16);
    }
    if (count < 2) {
        return 0;
    }
    if (w->mode == 1) {
        int shortest = 0;
        for (int i = 1; i < count; i++) {
            if (strlen(texts[i]) < strlen(texts[shortest])) {
                shortest = i;
            }
        }
        for (int i = 0; i < count; i++) {
            if (i == shortest) {
                continue;
            }
            float *row = out + (size_t)i * 16;
            /* Half its own axis, half the neighbour's: cosine to itself
             * drops to ~0.71, to nothing else above that. */
            float other[16];
            wrong_axis(texts[(i + 1) % count], other);
            double norm = 0.0;
            for (int d = 0; d < 16; d++) {
                row[d] = 0.5f * row[d] + 0.5f * other[d];
                norm += (double)row[d] * (double)row[d];
            }
            norm = sqrt(norm);
            for (int d = 0; d < 16; d++) {
                row[d] = (float)((double)row[d] / norm);
            }
        }
    } else {
        float tmp[16];
        memcpy(tmp, out, sizeof(tmp));
        memcpy(out, out + 16, sizeof(tmp));
        memcpy(out + 16, tmp, sizeof(tmp));
    }
    return 0;
}

static int wrong_dim(void *self) {
    (void)self;
    return 16;
}

TEST(ask_encoder_check_batching_catches_contamination_and_misassignment) {
    hyp_ask_encoder_vt_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.dim = wrong_dim;
    vt.encode_documents = wrong_encode_documents;
    wrong_enc_t w;
    hyp_ask_encoder_t enc;
    enc.vt = &vt;
    enc.self = &w;
    hyp_ask_batch_row_t rows[CHECK_TEXTS_N];
    hyp_ask_batch_check_t chk;

    /* Contamination: every row but the shortest is below the bar, none is
     * closer to another text than to itself. This is the fingerprint the
     * ragged split left on real code — the shortest row alone was right. */
    w.mode = 1;
    ASSERT_EQ(
        hyp_ask_encoder_check_batching(&enc, CHECK_TEXTS, CHECK_TEXTS_N, 16, 0.999F, rows, &chk),
        0);
    ASSERT_EQ(chk.below, CHECK_TEXTS_N - 1);
    ASSERT_EQ(chk.misassigned, 0);
    ASSERT_TRUE(chk.min_own < 0.999F);
    ASSERT_FLOAT_EQ((double)rows[0].own, 1.0, 1e-6); /* "a" is the shortest */
    ASSERT_TRUE(rows[1].own < 0.999F);

    /* Misassignment: two rows carry each other's vector — own cosine 0,
     * best_other 1.0 pointing at the swapped partner. */
    w.mode = 2;
    ASSERT_EQ(
        hyp_ask_encoder_check_batching(&enc, CHECK_TEXTS, CHECK_TEXTS_N, 16, 0.999F, rows, &chk),
        0);
    ASSERT_EQ(chk.misassigned, 2);
    ASSERT_EQ(chk.below, 2);
    ASSERT_EQ(rows[0].best_other_idx, 1);
    ASSERT_EQ(rows[1].best_other_idx, 0);
    ASSERT_FLOAT_EQ((double)rows[0].best_other, 1.0, 1e-6);

    /* Alone, the same double is correct — which is precisely why the pass's
     * own output could not reveal the bug, and this comparison can. */
    ASSERT_EQ(
        hyp_ask_encoder_check_batching(&enc, CHECK_TEXTS, CHECK_TEXTS_N, 1, 0.999F, rows, &chk), 0);
    ASSERT_EQ(chk.below, 0);
    ASSERT_EQ(chk.misassigned, 0);
    PASS();
}

SUITE(ask_embed) {
    RUN_TEST(ask_encoder_check_batching_passes_a_batch_independent_encoder);
    RUN_TEST(ask_encoder_check_batching_catches_contamination_and_misassignment);
    RUN_TEST(ask_embed_span_is_verbatim_source_lines);
    RUN_TEST(ask_embed_span_normalises_crlf_and_keeps_no_trailing_newline);
    RUN_TEST(ask_embed_content_hash_is_stable_and_32_hex);
    RUN_TEST(ask_embed_runs_end_to_end_and_embeds_every_spanned_node);
    RUN_TEST(ask_embed_reuses_byte_identical_declarations_on_a_rerun);
    RUN_TEST(ask_embed_touching_three_of_twelve_files_reembeds_their_spans_not_the_corpus);
    RUN_TEST(ask_embed_reuse_recites_a_span_that_moved_without_changing);
    RUN_TEST(ask_embed_prunes_declarations_that_no_longer_exist);
    RUN_TEST(ask_embed_limited_run_is_marked_partial_and_never_prunes);
    RUN_TEST(ask_embed_truncation_unknown_then_reprobed_to_attested);
    RUN_TEST(ask_embed_counts_unreadable_spans_instead_of_hiding_them);
    RUN_TEST(ask_embed_span_reports_the_files_line_count);
    RUN_TEST(ask_embed_drops_whole_file_spans_by_default);
    RUN_TEST(ask_embed_keeps_whole_file_spans_when_asked);
    RUN_TEST(ask_embed_zero_work_is_not_reported_as_success);
}
