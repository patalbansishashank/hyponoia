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
#include "ask/ask_view.h"

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
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM, 32768, "gen7", "CPU (test)", false),
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
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
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
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    float a[AV_TEST_DIM];
    unit_axis(a, AV_TEST_DIM, 0);
    hyp_ask_vec_row_t r = make_row("p.A", a, "aaaa1111aaaa1111aaaa1111aaaa1111", false);
    ASSERT_EQ(hyp_ask_vectors_put(v, &r, 1), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_finish_build(v, true), HYP_ASK_VEC_OK);

    /* Same dim, different model: two models' vectors are not comparable and
     * nothing downstream can detect the mix, so this refuses. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m2", "c1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_INCOMPATIBLE);
    ASSERT_EQ(hyp_ask_vectors_count(v), 1);

    /* Same model, different dim: also refused. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM + 1, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_INCOMPATIBLE);

    /* A different window changes what the truncation set is denominated in. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM, 4096, "g", "CPU (test)", false),
              HYP_ASK_VEC_INCOMPATIBLE);

    /* SAME MODEL, DIFFERENT PREFIX CONTRACT: refused. The weights are
     * identical, so model_id cannot catch this — but the text fed to them
     * differs, so the vectors are in a different space. §2.9 measured exactly
     * this case, one model wanting opposite contracts on two corpora. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c2", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_INCOMPATIBLE);
    ASSERT_EQ(hyp_ask_vectors_count(v), 1);

    /* An UNRECORDED contract is not a mismatch. An index written before the
     * field existed reads "", and refusing it would condemn a table that is
     * actually fine — "cannot say" and "differs" are different claims. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", NULL, AV_TEST_DIM, 32768, "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);

    /* Explicitly asked to replace: the old rows go, because keeping them would
     * make the matrix a mixture. */
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m2", "c1", AV_TEST_DIM, 32768, "g", "CPU (test)", true),
              HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_vectors_count(v), 0);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

/* The stamp has to survive a close and reopen, or the gate only works within
 * the process that wrote it. */
TEST(ask_vectors_prefix_contract_persists) {
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    char kept[512];
    snprintf(kept, sizeof(kept), "%s", TH_PATH(dir, "v.db"));

    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", kept);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "qwen3-instruct-v1-bare-doc", AV_TEST_DIM, 32768,
                                          "g", "CPU (test)", false),
              HYP_ASK_VEC_OK);
    hyp_ask_vectors_close(v);

    v = hyp_ask_vectors_open_path("p", kept);
    ASSERT_NOT_NULL(v);
    hyp_ask_vec_meta_t m;
    ASSERT_EQ(hyp_ask_vectors_get_meta(v, &m), HYP_ASK_VEC_OK);
    ASSERT_NOT_NULL(m.prefix_contract);
    ASSERT_STR_EQ(m.prefix_contract, "qwen3-instruct-v1-bare-doc");
    hyp_ask_vec_meta_free(&m);
    ASSERT_EQ(hyp_ask_vectors_check_compatible(v, "m1", "nano-prompted-v1", AV_TEST_DIM, 32768),
              HYP_ASK_VEC_INCOMPATIBLE);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

/* The two lanes are separate files. If they ever collided, an escalation build
 * would silently overwrite the local index with vectors from another model. */
TEST(ask_vectors_lanes_are_distinct_files) {
    char local[512];
    char esc[512];
    ASSERT_TRUE(hyp_ask_vectors_path_lane("proj", HYP_ASK_LANE_LOCAL, local, sizeof(local)));
    ASSERT_TRUE(hyp_ask_vectors_path_lane("proj", HYP_ASK_LANE_ESCALATION, esc, sizeof(esc)));
    ASSERT_TRUE(strcmp(local, esc) != 0);

    /* The local lane must keep the historical path, or every index built
     * before lanes existed becomes invisible rather than reusable. */
    char legacy[512];
    ASSERT_TRUE(hyp_ask_vectors_path("proj", legacy, sizeof(legacy)));
    ASSERT_STR_EQ(local, legacy);
    PASS();
}

TEST(ask_vectors_search_is_exact_top_k) {
    char *dir = th_mktempdir("hyp-askvec");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", TH_PATH(dir, "v.db"));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
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
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
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
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
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
    ASSERT_EQ(hyp_ask_vectors_begin_build(v, "m1", "c1", AV_TEST_DIM, 32768, "g", "CPU (test)", false),
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

/* ── The 3-D view (ask_view.h) ───────────────────────────────────── */

/* A small store whose vectors have a KNOWN principal structure: points on the
 * unit sphere in AV_TEST_DIM dims, spread widely along axes 0 and 1, barely
 * along axis 2, and not at all along the rest. PCA must find axes 0/1/2 as its
 * three directions, in that order. Unit-normalised so put() accepts them. */
static hyp_ask_vectors_t *view_fixture(const char *path, int rows) {
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_path("p", path);
    if (!v) {
        return NULL;
    }
    if (hyp_ask_vectors_begin_build(v, "m", "", AV_TEST_DIM, 64, "g1", "test", false) !=
        HYP_ASK_VEC_OK) {
        hyp_ask_vectors_close(v);
        return NULL;
    }
    static char names[64][16];
    static char hashes[64][8];
    static float vecs[64][AV_TEST_DIM];
    static hyp_ask_vec_row_t rowbuf[64];
    if (rows > 64) {
        rows = 64;
    }
    for (int i = 0; i < rows; i++) {
        double t = (double)i / (double)rows * 6.283185307179586;
        float *x = vecs[i];
        for (int k = 0; k < AV_TEST_DIM; k++) {
            x[k] = 0.0F;
        }
        x[0] = (float)(0.8 * cos(t));
        x[1] = (float)(0.5 * sin(t));
        x[2] = (float)(0.2 * ((i % 3) - 1));
        double n2 = 0.0;
        for (int k = 0; k < 3; k++) {
            n2 += (double)x[k] * (double)x[k];
        }
        /* Axis 3 completes the unit norm. It varies with the others, but by
         * less than any planted axis, so it does not compete for the top-3. */
        x[3] = (float)sqrt(1.0 - n2);
        snprintf(names[i], sizeof(names[i]), "p.row%02d", i);
        snprintf(hashes[i], sizeof(hashes[i]), "h%d", i);
        rowbuf[i] = make_row(names[i], x, hashes[i], false);
        rowbuf[i].node_id = 100 + i;
    }
    if (hyp_ask_vectors_put(v, rowbuf, rows) != HYP_ASK_VEC_OK ||
        hyp_ask_vectors_finish_build(v, true) != HYP_ASK_VEC_OK) {
        hyp_ask_vectors_close(v);
        return NULL;
    }
    return v;
}

TEST(ask_view_unfitted_is_not_found) {
    char *dir = th_mktempdir("hyp-askview");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = view_fixture(TH_PATH(dir, "v.db"), 12);
    ASSERT_NOT_NULL(v);
    hyp_ask_view_t view;
    /* An index without a view says so; it does not hand back zeros. */
    ASSERT_EQ(hyp_ask_view_load(v, &view), HYP_ASK_VEC_NOT_FOUND);
    hyp_ask_view_point_t *pts = NULL;
    int n = -1;
    int64_t gaps = -1;
    ASSERT_EQ(hyp_ask_view_points(v, &pts, &n, &gaps), HYP_ASK_VEC_OK);
    ASSERT_EQ(n, 0);
    ASSERT_EQ(gaps, 12);
    hyp_ask_view_points_free(pts, n);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_view_fit_finds_the_planted_axes_and_round_trips) {
    char *dir = th_mktempdir("hyp-askview");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = view_fixture(TH_PATH(dir, "v.db"), 24);
    ASSERT_NOT_NULL(v);
    hyp_ask_view_t view;
    ASSERT_EQ(hyp_ask_view_fit(v, &view), HYP_ASK_VEC_OK);
    ASSERT_STR_EQ(view.method, HYP_ASK_VIEW_METHOD);
    ASSERT_EQ(view.dim, AV_TEST_DIM);
    ASSERT_EQ(view.rows, 24);
    /* Descending variance, and the planted order: 0.8-cos, 0.5-sin, 0.2-step.
     * (ASSERT_GT truncates to integers; these are doubles.) */
    ASSERT_TRUE(view.eigen[0] > view.eigen[1]);
    ASSERT_TRUE(view.eigen[1] > view.eigen[2]);
    ASSERT_TRUE(view.eigen[2] > 0.0);
    ASSERT_TRUE(fabsf(view.basis[0 * AV_TEST_DIM + 0]) > 0.99F);
    ASSERT_TRUE(fabsf(view.basis[1 * AV_TEST_DIM + 1]) > 0.99F);
    ASSERT_TRUE(fabsf(view.basis[2 * AV_TEST_DIM + 2]) > 0.9F);
    /* Sign convention: the dominant component is positive. */
    ASSERT_TRUE(view.basis[0 * AV_TEST_DIM + 0] > 0.0F);
    ASSERT_TRUE(view.basis[1 * AV_TEST_DIM + 1] > 0.0F);
    ASSERT_TRUE(view.basis[2 * AV_TEST_DIM + 2] > 0.0F);
    /* Nearly all variance is on the three planted axes; the norm-completing
     * axis carries the little that is left. */
    double kept = (view.eigen[0] + view.eigen[1] + view.eigen[2]) / view.total_var;
    ASSERT_TRUE(kept > 0.95);
    /* Every row has coordinates, and they ARE the projection of its vector —
     * what the store returns for a row and what a query would get for the
     * same vector are one computation. */
    hyp_ask_view_point_t *pts = NULL;
    int n = 0;
    int64_t gaps = -1;
    ASSERT_EQ(hyp_ask_view_points(v, &pts, &n, &gaps), HYP_ASK_VEC_OK);
    ASSERT_EQ(n, 24);
    ASSERT_EQ(gaps, 0);
    ASSERT_STR_EQ(pts[0].qualified_name, "p.row00");
    ASSERT_EQ(pts[0].node_id, 100);
    float row0[AV_TEST_DIM] = {0.8F, 0.0F, -0.2F, 0.0F, 0, 0, 0, 0};
    row0[3] = (float)sqrt(1.0 - (0.64 + 0.04));
    float p3[3];
    hyp_ask_view_project(&view, row0, p3);
    ASSERT_FLOAT_EQ(pts[0].x, p3[0], 1e-5F);
    ASSERT_FLOAT_EQ(pts[0].y, p3[1], 1e-5F);
    ASSERT_FLOAT_EQ(pts[0].z, p3[2], 1e-5F);
    float lk[3];
    int64_t nid = 0;
    bool present = false;
    ASSERT_EQ(hyp_ask_view_lookup(v, "p.row00", lk, &nid, &present), HYP_ASK_VEC_OK);
    ASSERT_TRUE(present);
    ASSERT_EQ(nid, 100);
    ASSERT_FLOAT_EQ(lk[0], pts[0].x, 0.0F);
    ASSERT_EQ(hyp_ask_view_lookup(v, "p.nope", lk, &nid, &present), HYP_ASK_VEC_NOT_FOUND);
    ASSERT_FALSE(present);
    /* Fresh: fitted against the seal that exists. */
    ASSERT_FALSE(hyp_ask_view_is_stale(v, &view));
    hyp_ask_view_points_free(pts, n);
    hyp_ask_view_free(&view);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_view_fit_is_deterministic_bit_for_bit) {
    /* The plan's requirement — pin the seed — is met by having no seed. Two
     * fits over the same rows must agree to the bit: same basis, same
     * coordinates, same eigenvalues. Anything less and a re-render that moved
     * would be indistinguishable from an index that changed. */
    char *dir = th_mktempdir("hyp-askview");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = view_fixture(TH_PATH(dir, "v.db"), 40);
    ASSERT_NOT_NULL(v);
    hyp_ask_view_t a;
    hyp_ask_view_t b;
    ASSERT_EQ(hyp_ask_view_fit(v, &a), HYP_ASK_VEC_OK);
    hyp_ask_view_point_t *pa = NULL;
    int na = 0;
    ASSERT_EQ(hyp_ask_view_points(v, &pa, &na, NULL), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_view_fit(v, &b), HYP_ASK_VEC_OK);
    hyp_ask_view_point_t *pb = NULL;
    int nb = 0;
    ASSERT_EQ(hyp_ask_view_points(v, &pb, &nb, NULL), HYP_ASK_VEC_OK);
    ASSERT_EQ(na, nb);
    ASSERT_EQ(na, 40);
    ASSERT_MEM_EQ(a.basis, b.basis, sizeof(float) * 3 * AV_TEST_DIM);
    ASSERT_MEM_EQ(a.mean, b.mean, sizeof(float) * AV_TEST_DIM);
    ASSERT_MEM_EQ(a.eigen, b.eigen, sizeof(a.eigen));
    ASSERT_EQ(a.iterations, b.iterations);
    for (int i = 0; i < na; i++) {
        ASSERT_STR_EQ(pa[i].qualified_name, pb[i].qualified_name);
        ASSERT_MEM_EQ(&pa[i].x, &pb[i].x, sizeof(float));
        ASSERT_MEM_EQ(&pa[i].y, &pb[i].y, sizeof(float));
        ASSERT_MEM_EQ(&pa[i].z, &pb[i].z, sizeof(float));
    }
    /* And what was persisted reads back as what was fitted. */
    hyp_ask_view_t loaded;
    ASSERT_EQ(hyp_ask_view_load(v, &loaded), HYP_ASK_VEC_OK);
    ASSERT_MEM_EQ(loaded.basis, b.basis, sizeof(float) * 3 * AV_TEST_DIM);
    ASSERT_MEM_EQ(loaded.mean, b.mean, sizeof(float) * AV_TEST_DIM);
    ASSERT_EQ(loaded.rows, 40);
    hyp_ask_view_free(&loaded);
    hyp_ask_view_points_free(pa, na);
    hyp_ask_view_points_free(pb, nb);
    hyp_ask_view_free(&a);
    hyp_ask_view_free(&b);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

TEST(ask_view_rewritten_vector_loses_its_coordinates) {
    char *dir = th_mktempdir("hyp-askview");
    ASSERT_NOT_NULL(dir);
    hyp_ask_vectors_t *v = view_fixture(TH_PATH(dir, "v.db"), 12);
    ASSERT_NOT_NULL(v);
    hyp_ask_view_t view;
    ASSERT_EQ(hyp_ask_view_fit(v, &view), HYP_ASK_VEC_OK);
    ASSERT_FALSE(hyp_ask_view_is_stale(v, &view));
    /* Re-put one row with a different vector: its dot must vanish, not stay
     * where the old vector was. */
    float moved[AV_TEST_DIM];
    unit_axis(moved, AV_TEST_DIM, 5);
    hyp_ask_vec_row_t r = make_row("p.row03", moved, "changed", false);
    ASSERT_EQ(hyp_ask_vectors_put(v, &r, 1), HYP_ASK_VEC_OK);
    float xyz[3];
    bool present = false;
    ASSERT_EQ(hyp_ask_view_lookup(v, "p.row03", xyz, NULL, &present), HYP_ASK_VEC_NOT_FOUND);
    ASSERT_TRUE(present); /* the row exists; only its picture is gone */
    hyp_ask_view_point_t *pts = NULL;
    int n = 0;
    int64_t gaps = 0;
    ASSERT_EQ(hyp_ask_view_points(v, &pts, &n, &gaps), HYP_ASK_VEC_OK);
    ASSERT_EQ(n, 11);
    ASSERT_EQ(gaps, 1);
    hyp_ask_view_points_free(pts, n);
    /* Untouched rows keep theirs. */
    ASSERT_EQ(hyp_ask_view_lookup(v, "p.row04", xyz, NULL, &present), HYP_ASK_VEC_OK);
    /* A re-fit fills the gap back in — over the whole table. */
    hyp_ask_view_free(&view);
    ASSERT_EQ(hyp_ask_view_fit(v, &view), HYP_ASK_VEC_OK);
    ASSERT_EQ(hyp_ask_view_lookup(v, "p.row03", xyz, NULL, &present), HYP_ASK_VEC_OK);
    hyp_ask_view_free(&view);
    hyp_ask_vectors_close(v);
    th_cleanup(dir);
    PASS();
}

SUITE(ask_vectors) {
    RUN_TEST(ask_view_unfitted_is_not_found);
    RUN_TEST(ask_view_fit_finds_the_planted_axes_and_round_trips);
    RUN_TEST(ask_view_fit_is_deterministic_bit_for_bit);
    RUN_TEST(ask_view_rewritten_vector_loses_its_coordinates);
    RUN_TEST(ask_vectors_unbuilt_index_is_not_found_not_empty);
    RUN_TEST(ask_vectors_roundtrip_and_meta);
    RUN_TEST(ask_vectors_refuses_a_row_that_is_not_unit_normalised);
    RUN_TEST(ask_vectors_refuses_a_different_model_unless_told_to_wipe);
    RUN_TEST(ask_vectors_prefix_contract_persists);
    RUN_TEST(ask_vectors_lanes_are_distinct_files);
    RUN_TEST(ask_vectors_search_is_exact_top_k);
    RUN_TEST(ask_vectors_truncation_has_three_states);
    RUN_TEST(ask_vectors_prune_removes_only_what_is_gone);
    RUN_TEST(ask_vectors_stored_hash_is_the_reuse_key);
    RUN_TEST(ask_vectors_stub_encoder_returns_unit_rows);
}
