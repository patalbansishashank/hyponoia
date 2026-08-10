/*
 * test_ask_vectors.c — the per-declaration vector store and its truncation
 * counter.
 *
 * The properties under test are the ones that make a stored vector safe to
 * serve: the provenance gate, the unit-norm gate, exactness of the top-k, and
 * the three truncation states staying three.
 */

#include "test_framework.h"
#include "test_helpers.h"

#include "ask/ask_encoder.h"
#include "ask/ask_vectors.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AV_TEST_DIM 8

static void unit_axis(float *v, int dim, int axis) {
    for (int i = 0; i < dim; i++) {
        v[i] = 0.0F;
    }
    v[axis % dim] = 1.0F;
}

static hyp_ask_vec_row_t make_row(const char *qn, const float *vec, const char *hash,
                                  bool truncated) {
    hyp_ask_vec_row_t r;
    memset(&r, 0, sizeof(r));
    r.qualified_name = qn;
    r.node_id = 7;
    r.label = "Function";
    r.file_path = "src/x.c";
    r.start_line = 10;
    r.end_line = 20;
    r.lang = "C";
    r.content_hash = hash;
    r.truncated = truncated;
    r.vector = vec;
    return r;
}

TEST(ask_vectors_unbuilt_index_is_not_found_not_empty) {
    /* "No index has been built" and "an index with no rows" are different
     * answers, and the lane's availability depends on telling them apart. */
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", TH_PATH(dir, "v.db"));
    ASSERT_NOT_NULL(v);
    hyp_ask_vec_meta_t m;
    ASSERT_EQ(hyp_ask_vectors_get_meta(v, &m), HYP_ASK_VEC_NOT_FOUND);
    ASSERT_EQ(hyp_ask_vectors_count(v), 0);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_vectors_roundtrip_and_meta) {
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", TH_PATH(dir, "v.db"));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM, 32768, "gen7", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    float a[AV_TEST_DIM];
    float b[AV_TEST_DIM];
    unit_axis(a, AV_TEST_DIM, 0);
    unit_axis(b, AV_TEST_DIM, 1);
    hyp_ask_vec_row_t rows[2] = {make_row("p.A", a, "aaaa1111aaaa1111aaaa1111aaaa1111", false),
                                 make_row("p.B", b, "bbbb2222bbbb2222bbbb2222bbbb2222", false)};
    ASSERT_EQ(hyp_ask_vectors_put(v, rows, 2), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_finish_build(v, true), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_count(v), 2);

    hyp_ask_vec_meta_t m;
    ASSERT_EQ(hyp_ask_vectors_get_meta(v, &m), HYP_ASK_VEC_OK);
    ASSERT_EQ(m.format, HYP_ASK_VEC_FORMAT);
    ASSERT_STR_EQ(m.model_id, "m1");
    ASSERT_EQ(m.dim, AV_TEST_DIM);
    ASSERT_EQ(m.window_tokens, 32768);
    ASSERT_STR_EQ(m.graph_generation, "gen7");
    /* Which device built the index is recorded. A 45-minute CPU build and a
     * 3-minute GPU build produce indistinguishable files otherwise. */
    ASSERT_STR_EQ(m.device_note, "CPU (test)");
    ASSERT_EQ(m.row_count, 2);
    ASSERT(m.truncation_known);
    ASSERT_EQ(m.truncated_count, 0);
    hyp_ask_vec_meta_free(&m);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_vectors_refuses_a_row_that_is_not_unit_normalised) {
    /* Cosine is a dot product only when both sides are unit vectors. A row that
     * is not would score silently wrong rather than loudly absent, so it must
     * never reach disk. */
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", TH_PATH(dir, "v.db"));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    float bad[AV_TEST_DIM];
    for (int i = 0; i < AV_TEST_DIM; i++) {
        bad[i] = 1.0F; /* norm sqrt(8) ~= 2.83 */
    }
    hyp_ask_vec_row_t r = make_row("p.bad", bad, "cccc3333cccc3333cccc3333cccc3333", false);
    ASSERT_EQ(hyp_ask_vectors_put(v, &r, 1), HYP_ASK_VEC_ERR);
    ASSERT_EQ(hyp_ask_vectors_count(v), 0);

    /* The tolerance is 1e-2, not 1e-3: the real model's worst measured
     * deviation is 0.003819, and 1e-3 would reject about half of its output. */
    float nearly[AV_TEST_DIM];
    unit_axis(nearly, AV_TEST_DIM, 0);
    nearly[0] = 1.0F + 0.004F;
    hyp_ask_vec_row_t ok = make_row("p.ok", nearly, "dddd4444dddd4444dddd4444dddd4444", false);
    ASSERT_EQ(hyp_ask_vectors_put(v, &ok, 1), HYP_ASK_VEC_OK);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_vectors_refuses_a_different_model_unless_told_to_wipe) {
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    const char *path = TH_PATH(dir, "v.db");
    char kept[512];
    snprintf(kept, sizeof(kept), "%s", path);

    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", kept);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    float a[AV_TEST_DIM];
    unit_axis(a, AV_TEST_DIM, 0);
    hyp_ask_vec_row_t r = make_row("p.A", a, "aaaa1111aaaa1111aaaa1111aaaa1111", false);
    ASSERT_EQ(hyp_ask_vectors_put(v, &r, 1), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_finish_build(v, true), HYP_ASK_VEC_OK);

    /* Same dim, different model: two models' vectors are not comparable and
     * nothing downstream can detect the mix, so this refuses. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m2", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_INCOMPATIBLE);
    ASSERT_EQ(hyp_ask_vectors_count(v), 1);

    /* Same model, different dim: also refused. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM + 1, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_INCOMPATIBLE);

    /* A different window changes what the truncation set is denominated in. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM, 4096, "g", "CPU (test)", false),
              HYP_ASK_VEC_INCOMPATIBLE);

    /* Explicitly asked to replace: the old rows go, because keeping them would
     * make the matrix a mixture. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m2", AV_TEST_DIM, 32768, "g", "CPU (test)", true),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_count(v), 0);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_vectors_search_is_exact_top_k) {
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", TH_PATH(dir, "v.db"));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    /* One basis vector per axis: the top-k for axis j is exactly row j. */
    float vecs[AV_TEST_DIM][AV_TEST_DIM];
    hyp_ask_vec_row_t rows[AV_TEST_DIM];
    char names[AV_TEST_DIM][16];
    char hashes[AV_TEST_DIM][HYP_ASK_VEC_HASH_LEN + 1];
    for (int i = 0; i < AV_TEST_DIM; i++) {
        unit_axis(vecs[i], AV_TEST_DIM, i);
        snprintf(names[i], sizeof(names[i]), "p.n%d", i);
        snprintf(hashes[i], sizeof(hashes[i]), "%032d", i);
        rows[i] = make_row(names[i], vecs[i], hashes[i], false);
    }
    ASSERT_EQ(hyp_ask_vectors_put(v, rows, AV_TEST_DIM), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_finish_build(v, true), HYP_ASK_VEC_OK);

    float q[AV_TEST_DIM];
    unit_axis(q, AV_TEST_DIM, 3);
    hyp_ask_vec_hit_t *hits = NULL;
    int n = 0;
    ASSERT_EQ(hyp_ask_vectors_search(v, q, AV_TEST_DIM, 3, &hits, &n), HYP_ASK_VEC_OK);
    ASSERT_EQ(n, 3);
    ASSERT_STR_EQ(hits[0].qualified_name, "p.n3");
    ASSERT(fabs((double)hits[0].score - 1.0) < 1e-5);
    /* Descending. */
    ASSERT_LTE((double)hits[1].score, (double)hits[0].score);
    ASSERT_LTE((double)hits[2].score, (double)hits[1].score);
    /* The span travels with the hit, so an answer can cite it without
     * consulting the graph. */
    ASSERT_STR_EQ(hits[0].file_path, "src/x.c");
    ASSERT_EQ(hits[0].start_line, 10);
    ASSERT_EQ(hits[0].end_line, 20);
    hyp_ask_vec_hits_free(hits, n);

    /* A question of the wrong width is two different models, and is refused. */
    float wrong[AV_TEST_DIM + 1];
    for (int i = 0; i <= AV_TEST_DIM; i++) {
        wrong[i] = 0.0F;
    }
    wrong[0] = 1.0F;
    ASSERT_EQ(hyp_ask_vectors_search(v, wrong, AV_TEST_DIM + 1, 3, &hits, &n),
              HYP_ASK_VEC_INCOMPATIBLE);

    /* An unnormalised question is refused rather than scored. */
    float unnorm[AV_TEST_DIM];
    unit_axis(unnorm, AV_TEST_DIM, 0);
    unnorm[0] = 2.0F;
    ASSERT_EQ(hyp_ask_vectors_search(v, unnorm, AV_TEST_DIM, 3, &hits, &n), HYP_ASK_VEC_ERR);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_vectors_truncation_has_three_states) {
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", TH_PATH(dir, "v.db"));
    ASSERT_NOT_NULL(v);
    float a[AV_TEST_DIM];
    float b[AV_TEST_DIM];
    unit_axis(a, AV_TEST_DIM, 0);
    unit_axis(b, AV_TEST_DIM, 1);

    /* 1. UNKNOWN — the encoder could not say. Not the same claim as "none". */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    hyp_ask_vec_row_t rows[2] = {make_row("p.A", a, "aaaa1111aaaa1111aaaa1111aaaa1111", true),
                                 make_row("p.B", b, "bbbb2222bbbb2222bbbb2222bbbb2222", false)};
    ASSERT_EQ(hyp_ask_vectors_put(v, rows, 2), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_finish_build(v, false), HYP_ASK_VEC_OK);
    hyp_ask_trunc_t t;
    ASSERT_EQ(hyp_ask_vectors_truncation(v, &t), HYP_ASK_VEC_OK);
    ASSERT_EQ(t.state, HYP_ASK_TRUNC_UNKNOWN);
    ASSERT_EQ(t.count, 0);
    char line[512];
    hyp_ask_trunc_describe(&t, line, sizeof(line));
    ASSERT(strstr(line, "UNKNOWN") != NULL);
    /* And it must not read as a claim that nothing was truncated. */
    ASSERT(strstr(line, "not a claim that none") != NULL);
    hyp_ask_trunc_free(&t);
    /* The stale `true` flag on p.A must be gone: an unattested index may not
     * leave an attested-looking row behind. */
    char hash[HYP_ASK_VEC_HASH_LEN + 1];
    bool stored_trunc = true;
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.A", hash, &stored_trunc), HYP_ASK_VEC_OK);
    ASSERT_EQ(stored_trunc, false);

    /* 2. ATTESTED ZERO. */
    ASSERT_EQ(hyp_ask_vectors_finish_build(v, true), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_truncation(v, &t), HYP_ASK_VEC_OK);
    ASSERT_EQ(t.state, HYP_ASK_TRUNC_NONE);
    ASSERT_EQ(t.count, 0);
    hyp_ask_trunc_describe(&t, line, sizeof(line));
    ASSERT(strstr(line, "none") != NULL);
    hyp_ask_trunc_free(&t);

    /* 3. THE SET, with the largest offender named. */
    const char *qns[1] = {"p.A"};
    bool flags[1] = {true};
    ASSERT_EQ(hyp_ask_vectors_set_truncated_batch(v, qns, flags, 1), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_finish_build(v, true), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_truncation(v, &t), HYP_ASK_VEC_OK);
    ASSERT_EQ(t.state, HYP_ASK_TRUNC_SOME);
    ASSERT_EQ(t.count, 1);
    ASSERT_STR_EQ(t.worst_qualified_name, "p.A");
    hyp_ask_trunc_describe(&t, line, sizeof(line));
    ASSERT(strstr(line, "FROM THEIR FIRST TOKENS ONLY") != NULL);
    hyp_ask_trunc_free(&t);

    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_vectors_prune_removes_only_what_is_gone) {
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", TH_PATH(dir, "v.db"));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    float a[AV_TEST_DIM];
    float b[AV_TEST_DIM];
    unit_axis(a, AV_TEST_DIM, 0);
    unit_axis(b, AV_TEST_DIM, 1);
    hyp_ask_vec_row_t rows[2] = {make_row("p.A", a, "aaaa1111aaaa1111aaaa1111aaaa1111", false),
                                 make_row("p.B", b, "bbbb2222bbbb2222bbbb2222bbbb2222", false)};
    ASSERT_EQ(hyp_ask_vectors_put(v, rows, 2), HYP_ASK_VEC_OK);
    const char *keep[1] = {"p.A"};
    int64_t removed = 0;
    ASSERT_EQ(hyp_ask_vectors_prune(v, keep, 1, &removed), HYP_ASK_VEC_OK);
    ASSERT_EQ(removed, 1);
    ASSERT_EQ(hyp_ask_vectors_count(v), 1);
    char hash[HYP_ASK_VEC_HASH_LEN + 1];
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.A", hash, NULL), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.B", hash, NULL), HYP_ASK_VEC_NOT_FOUND);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_vectors_stored_hash_is_the_reuse_key) {
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", TH_PATH(dir, "v.db"));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    float a[AV_TEST_DIM];
    unit_axis(a, AV_TEST_DIM, 0);
    hyp_ask_vec_row_t r = make_row("p.A", a, "aaaa1111aaaa1111aaaa1111aaaa1111", true);
    ASSERT_EQ(hyp_ask_vectors_put(v, &r, 1), HYP_ASK_VEC_OK);
    char hash[HYP_ASK_VEC_HASH_LEN + 1];
    bool trunc = false;
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.A", hash, &trunc), HYP_ASK_VEC_OK);
    ASSERT_STR_EQ(hash, "aaaa1111aaaa1111aaaa1111aaaa1111");
    ASSERT_EQ(trunc, true);
    ASSERT_EQ(hyp_ask_vectors_stored_hash(v, "p.missing", hash, &trunc), HYP_ASK_VEC_NOT_FOUND);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_vectors_stub_encoder_returns_unit_rows) {
    hyp_ask_encoder_t *e = hyp_ask_encoder_stub_create(64, 32768, true);
    ASSERT_NOT_NULL(e);
    /* The encoder must say which device it ran on, and the stub must not
     * pretend to be a GPU. An encoder that will not say returns the UNKNOWN
     * sentence rather than a guess — a wrong device note is worse than an
     * absent one because it is believed. */
    ASSERT_STR_EQ(hyp_ask_encoder_device_note(e), HYP_ASK_STUB_DEVICE_NOTE);
    ASSERT_EQ(hyp_ask_encoder_device_is_gpu(e), false);
    ASSERT_EQ(hyp_ask_encoder_dim(e), 64);
    ASSERT_EQ(hyp_ask_encoder_window(e), 32768);
    ASSERT_STR_EQ(hyp_ask_encoder_model_id(e), HYP_ASK_STUB_MODEL_ID);
    const char *texts[3] = {"int f(void) { return 1; }", "", "class C { int x; };"};
    float *out = malloc(3 * 64 * sizeof(float));
    ASSERT_NOT_NULL(out);
    ASSERT_EQ(hyp_ask_encode_documents(e, texts, 3, out), 0);
    int bad = -1;
    ASSERT(hyp_ask_vectors_are_unit(out, 3, 64, &bad));
    /* Deterministic: the same text must give the same vector, or reuse by
     * content hash is not testable. */
    float again[64];
    const char *one[1] = {"int f(void) { return 1; }"};
    ASSERT_EQ(hyp_ask_encode_documents(e, one, 1, again), 0);
    for (int i = 0; i < 64; i++) {
        ASSERT(fabsf(again[i] - out[i]) < 1e-6F);
    }
    /* An encoder that cannot report gives the UNKNOWN answer, not zero. */
    free(out);
    hyp_ask_encoder_destroy(e);
    hyp_ask_encoder_t *quiet = hyp_ask_encoder_stub_create(64, 32768, false);
    ASSERT_NOT_NULL(quiet);
    ASSERT_LT(hyp_ask_encoder_full_token_length(quiet, "anything"), 0);
    ASSERT_GT(hyp_ask_encoder_token_length(quiet, "anything"), 0);
    hyp_ask_encoder_destroy(quiet);
    PASS();
}

SUITE(ask_vectors) {
    RUN_TEST(ask_vectors_unbuilt_index_is_not_found_not_empty);
    RUN_TEST(ask_vectors_roundtrip_and_meta);
    RUN_TEST(ask_vectors_refuses_a_row_that_is_not_unit_normalised);
    RUN_TEST(ask_vectors_refuses_a_different_model_unless_told_to_wipe);
    RUN_TEST(ask_vectors_search_is_exact_top_k);
    RUN_TEST(ask_vectors_truncation_has_three_states);
    RUN_TEST(ask_vectors_prune_removes_only_what_is_gone);
    RUN_TEST(ask_vectors_stored_hash_is_the_reuse_key);
    RUN_TEST(ask_vectors_stub_encoder_returns_unit_rows);
}
