/*
 * test_onboard.c — the probe (B1), the flow (B2), the TOML (B3)
 * and the zero-config path (B4).
 *
 * The two properties this file exists to hold:
 *
 *   1. The GPU answer is EVIDENCE. A verdict of `verified` requires the
 *      batching check to have RUN and PASSED on a GPU encoder — hardcoding the
 *      answer (the assumption the GPU retraction forbids) fails the tests
 *      below, because they inject encoders whose runs are known-bad or
 *      known-CPU and assert the verdict follows the run.
 *
 *   2. The TOML is NEVER a prerequisite. The zero-config test resolves,
 *      indexes and answers on a fixture repo that has no TOML, no Multica and
 *      no flags; making the TOML required fails it.
 *
 * The encoder is an in-process axis-vector double, same shape as test_ask.c's:
 * deterministic, weight-free, exact expected rankings. No model is downloaded,
 * no network is touched, no daemon is started.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <ask/ask_embed.h> /* hyp_ask_embed_run — the real embed pass */
#include <ask/ask_encoder.h>
#include <ask/ask_llama.h> /* hyp_ask_llama_compiled_in — what THIS build is */
#include <cli/cli.h>       /* hyp_config_*, HYP_CONFIG_ASK_ESC_KEY_ENV */
#include <cli/onboard.h>
#include <foundation/identity.h> /* HYP_ADDR_SLUG_MAX — the cap the plan must hold */
#include <mcp/mcp.h>
#include <pipeline/pipeline.h>  /* hyp_project_name_from_path */
#include <semantic/ask_embed.h> /* hyp_ask_backend_t — the query side */
#include <yyjson/yyjson.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── An axis-vector encoder double (document side) ───────────────── */

/* One deterministic hash shared by BOTH sides of the lane in this file, so a
 * question that is byte-identical to a document scores 1.0 against it and 0.0
 * against everything else — exact expected rankings, nothing approximate. */
static void ob_axis_vector(const char *text, float *out, int dim) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h = h * 33u + *p;
    }
    memset(out, 0, (size_t)dim * sizeof(float));
    out[h % (unsigned long)dim] = 1.0f;
}

#define OB_ENC_MODEL_ID "test-double/axis-1024"

typedef struct {
    bool claims_gpu;
    /* When set, a document encoded in a batch of more than one lands on a
     * DIFFERENT axis than the same document alone — the batch-contamination
     * class, reproduced on demand so the probe can be watched saying FAIL. */
    bool contaminate_batches;
} ob_enc_state_t;

static const char *ob_enc_model_id(void *self) {
    (void)self;
    return OB_ENC_MODEL_ID;
}
static const char *ob_enc_prefix_contract(void *self) {
    (void)self;
    return "test-double-contract";
}
static const char *ob_enc_device_note(void *self) {
    ob_enc_state_t *st = self;
    return st->claims_gpu ? "GPU (test double)" : "CPU (test double)";
}
static bool ob_enc_device_is_gpu(void *self) {
    return ((ob_enc_state_t *)self)->claims_gpu;
}
static int ob_enc_dim(void *self) {
    (void)self;
    return HYP_ASK_DIM;
}
static int ob_enc_window(void *self) {
    (void)self;
    return 32768;
}
static int ob_enc_token_length(void *self, const char *text) {
    (void)self;
    return (int)(strlen(text) / 4) + 1;
}
static int ob_enc_encode_documents(void *self, const char *const *texts, int count, float *out) {
    ob_enc_state_t *st = self;
    for (int i = 0; i < count; i++) {
        ob_axis_vector(texts[i], out + (size_t)i * HYP_ASK_DIM, HYP_ASK_DIM);
        if (st->contaminate_batches && count > 1) {
            /* Shift the axis: the vector now depends on what shared its
             * forward pass, which is exactly the retracted failure. */
            float *row = out + (size_t)i * HYP_ASK_DIM;
            for (int d = 0; d < HYP_ASK_DIM; d++) {
                if (row[d] == 1.0f) {
                    row[d] = 0.0f;
                    row[(d + 1) % HYP_ASK_DIM] = 1.0f;
                    break;
                }
            }
        }
    }
    return 0;
}
static int ob_enc_encode_query(void *self, const char *text, const char *lang, float *out) {
    (void)self;
    (void)lang;
    ob_axis_vector(text, out, HYP_ASK_DIM);
    return 0;
}
static void ob_enc_destroy(void *self) {
    (void)self;
}

static const hyp_ask_encoder_vt_t OB_ENC_VT = {
    .model_id = ob_enc_model_id,
    .prefix_contract = ob_enc_prefix_contract,
    .device_note = ob_enc_device_note,
    .device_is_gpu = ob_enc_device_is_gpu,
    .dim = ob_enc_dim,
    .window_tokens = ob_enc_window,
    .token_length = ob_enc_token_length,
    .full_token_length = ob_enc_token_length,
    .encode_documents = ob_enc_encode_documents,
    .encode_query = ob_enc_encode_query,
    .destroy = ob_enc_destroy,
};

/* ── The matching query backend (the `ask` tool's side) ──────────── */

static int ob_backend_encode_query(HYPLanguage lang, const char *text, float *out, char *err,
                                   size_t errlen) {
    (void)lang;
    (void)err;
    (void)errlen;
    ob_axis_vector(text, out, HYP_ASK_DIM);
    return 0;
}
static int ob_backend_encode_documents(const char *const *texts, int n, float *out, char *err,
                                       size_t errlen) {
    (void)err;
    (void)errlen;
    for (int i = 0; i < n; i++) {
        ob_axis_vector(texts[i], out + (size_t)i * HYP_ASK_DIM, HYP_ASK_DIM);
    }
    return 0;
}
static const hyp_ask_backend_t OB_BACKEND = {
    .model_id = OB_ENC_MODEL_ID,
    .dim = HYP_ASK_DIM,
    .window_tokens = 32768,
    .encode_query = ob_backend_encode_query,
    .encode_documents = ob_backend_encode_documents,
    .truncates = NULL,
};

/* ── Isolated cache (test_ask.c's esc_cache pattern) ─────────────── */

typedef struct {
    char *dir;
    char *saved_cache;
} ob_cache_t;

static bool ob_cache_begin(ob_cache_t *c) {
    const char *made = th_mktempdir("hyp-onboard-cache");
    c->dir = made ? hyp_strdup(made) : NULL;
    if (!c->dir) {
        return false;
    }
    const char *saved = getenv("HYP_CACHE_DIR");
    c->saved_cache = saved ? hyp_strdup(saved) : NULL;
    hyp_setenv("HYP_CACHE_DIR", c->dir, 1);
    return true;
}

static void ob_cache_end(ob_cache_t *c) {
    if (c->saved_cache) {
        hyp_setenv("HYP_CACHE_DIR", c->saved_cache, 1);
        free(c->saved_cache);
    } else {
        hyp_unsetenv("HYP_CACHE_DIR");
    }
    if (c->dir) {
        th_cleanup(c->dir);
        free(c->dir);
    }
    c->dir = NULL;
    c->saved_cache = NULL;
}

/* ── Fixture repo ────────────────────────────────────────────────── */

/* alpha's span, written ONCE and used to build the fixture file, so the text
 * the indexer sees and the text this file calls "alpha's span" cannot drift
 * apart. The B4 answer test asks a question that is the JSON escaping of this
 * — the one spelling that cannot be shared, since a JSON string literal is not
 * a C one — and it asserts a cosine above 0.99 on the top row, which is the
 * assertion that fails loudly if the two ever stop matching. */
#define OB_FIXTURE_ALPHA "int alpha(void) {\n    return 1;\n}"

static char *ob_fixture_repo(void) {
    const char *made = th_mktempdir("hyp-onboard-repo");
    char *dir = made ? hyp_strdup(made) : NULL;
    if (!dir) {
        return NULL;
    }
    if (th_write_file(TH_PATH(dir, "src/x.c"),
                      OB_FIXTURE_ALPHA "\n"
                                       "int beta(void) {\n"
                                       "    return 2;\n"
                                       "}\n") != 0 ||
        th_write_file(TH_PATH(dir, "src/y.c"), "int gamma(void) {\n    return 3;\n}\n") != 0) {
        th_cleanup(dir);
        free(dir);
        return NULL;
    }
    return dir;
}

/* result.content[0].text — same extraction test_ask.c uses, because asserting
 * the client's view is the house rule. hyp_mcp_handle_tool returns the tool
 * result object (not the JSON-RPC envelope), so the text sits at the top. */
static char *ob_tool_text(const char *response) {
    yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *content = root ? yyjson_obj_get(root, "content") : NULL;
    yyjson_val *first = content ? yyjson_arr_get(content, 0) : NULL;
    yyjson_val *text = first ? yyjson_obj_get(first, "text") : NULL;
    char *out = (text && yyjson_is_str(text)) ? hyp_strdup(yyjson_get_str(text)) : NULL;
    yyjson_doc_free(doc);
    return out;
}

/* ── The tables are frozen ───────────────────────────────────────── */

TEST(onboard_the_four_questions_are_exactly_four) {
    /* "Never interrogate" as a compile-visible property: the question table
     * has four rows, these four, and a fifth has to change this test. */
    ASSERT_EQ(HYP_ONBOARD_Q_COUNT, 4);
    ASSERT_STR_EQ(hyp_onboard_question_key(HYP_ONBOARD_Q_REPOS), "repos");
    ASSERT_STR_EQ(hyp_onboard_question_key(HYP_ONBOARD_Q_REFERENCE), "reference");
    ASSERT_STR_EQ(hyp_onboard_question_key(HYP_ONBOARD_Q_SPEND), "spend");
    ASSERT_STR_EQ(hyp_onboard_question_key(HYP_ONBOARD_Q_NAME), "name");
    for (int q = 0; q < HYP_ONBOARD_Q_COUNT; q++) {
        ASSERT_NOT_NULL(hyp_onboard_question_prompt((hyp_onboard_q_t)q));
    }
    ASSERT_NULL(hyp_onboard_question_key(HYP_ONBOARD_Q_COUNT));

    ASSERT_TRUE(hyp_onboard_role_valid("member"));
    ASSERT_TRUE(hyp_onboard_role_valid("reference"));
    ASSERT_TRUE(hyp_onboard_role_valid("vendored"));
    ASSERT_FALSE(hyp_onboard_role_valid("editable"));
    ASSERT_FALSE(hyp_onboard_role_valid(""));
    ASSERT_FALSE(hyp_onboard_role_valid(NULL));
    PASS();
}

/* ── B1 · GPU: the verdict follows the run ───────────────────────── */

TEST(onboard_probe_gpu_failed_when_the_run_fails) {
    /* The batch-contamination class, injected. A probe that ASSUMED the
     * answer — hardcoded `verified`, never ran the check — cannot pass this
     * test, which is the negative control's tripwire. */
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    ob_enc_state_t st = {.claims_gpu = true, .contaminate_batches = true};
    hyp_ask_encoder_t enc = {.vt = &OB_ENC_VT, .self = &st};

    hyp_onboard_probe_opts_t opts = {.repo_path = dir, .encoder = &enc};
    hyp_onboard_probe_t p;
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_EQ(p.gpu, HYP_ONBOARD_GPU_FAILED);
    ASSERT_TRUE(p.gpu_check_ran);
    ASSERT_GT(p.gpu_check.below, 0);
    ASSERT_NOT_NULL(strstr(p.gpu_evidence, "FAIL"));

    char *json = hyp_onboard_probe_json(&p);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"state\":\"failed\""));
    ASSERT_NOT_NULL(strstr(json, "\"check_ran\":true"));
    free(json);
    th_cleanup(dir);
    free(dir);
    PASS();
}

TEST(onboard_probe_gpu_verified_only_by_a_passing_run_on_a_gpu) {
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);

    /* A clean run on a GPU encoder: the one path to `verified`, and it
     * carries the run's own numbers as evidence. */
    ob_enc_state_t gpu_ok = {.claims_gpu = true, .contaminate_batches = false};
    hyp_ask_encoder_t enc = {.vt = &OB_ENC_VT, .self = &gpu_ok};
    hyp_onboard_probe_opts_t opts = {.repo_path = dir, .encoder = &enc};
    hyp_onboard_probe_t p;
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_EQ(p.gpu, HYP_ONBOARD_GPU_VERIFIED);
    ASSERT_TRUE(p.gpu_check_ran);
    ASSERT_EQ(p.gpu_check.below, 0);
    ASSERT_EQ(p.gpu_check.misassigned, 0);
    ASSERT_NOT_NULL(strstr(p.gpu_evidence, "PASS"));

    /* The same clean run on a CPU encoder verifies nothing about a GPU and
     * must not be reported as though it did. */
    ob_enc_state_t cpu_ok = {.claims_gpu = false, .contaminate_batches = false};
    hyp_ask_encoder_t cenc = {.vt = &OB_ENC_VT, .self = &cpu_ok};
    opts.encoder = &cenc;
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_EQ(p.gpu, HYP_ONBOARD_GPU_UNUSABLE);
    ASSERT_TRUE(p.gpu_check_ran);
    ASSERT_NOT_NULL(strstr(p.gpu_evidence, "not a GPU"));
    th_cleanup(dir);
    free(dir);
    PASS();
}

TEST(onboard_probe_gpu_reports_what_this_build_can_do) {
    /* No injected encoder: the production path, asserted against what THIS
     * binary actually is. Whatever the build, `verified` without a run is
     * unreachable — that invariant holds on every machine this runs on. */
    ob_cache_t cache;
    ASSERT_TRUE(ob_cache_begin(&cache));
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    hyp_onboard_probe_opts_t opts = {.repo_path = dir};
    hyp_onboard_probe_t p;
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    if (!hyp_ask_llama_compiled_in()) {
        ASSERT_EQ(p.gpu, HYP_ONBOARD_GPU_NOT_IN_BUILD);
        ASSERT_FALSE(p.gpu_check_ran);
        /* The evidence names the build, because the remedy is a different
         * binary, not a different machine. */
        ASSERT_NOT_NULL(strstr(p.gpu_evidence, "backend"));
    } else {
        /* With the cache isolated the model is absent, so the check cannot
         * run — and cannot-run is never yes. */
        ASSERT_TRUE(p.gpu == HYP_ONBOARD_GPU_NO_MODEL || p.gpu == HYP_ONBOARD_GPU_UNUSABLE);
    }
    ASSERT_TRUE(p.gpu != HYP_ONBOARD_GPU_VERIFIED || p.gpu_check_ran);
    th_cleanup(dir);
    free(dir);
    ob_cache_end(&cache);
    PASS();
}

/* ── B1 · The key variable ───────────────────────────────────────── */

#define OB_KEY_NAME "HYP_ONBOARD_TEST_KEY"
#define OB_KEY_SENTINEL "hyp-test-secret-value-nobody-may-print"

TEST(onboard_probe_key_variable_names_and_never_values) {
    ob_cache_t cache;
    ASSERT_TRUE(ob_cache_begin(&cache));
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);

    /* No name configured: nothing to probe, said as such. */
    ob_enc_state_t st = {.claims_gpu = false, .contaminate_batches = false};
    hyp_ask_encoder_t enc = {.vt = &OB_ENC_VT, .self = &st};
    hyp_onboard_probe_opts_t opts = {.repo_path = dir, .encoder = &enc};
    hyp_onboard_probe_t p;
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_FALSE(p.key_env_named);

    /* Config names the variable; the environment carries a sentinel value.
     * The probe reports the NAME and one bit — and the sentinel must not
     * appear anywhere in the machine-readable report. */
    hyp_config_t *cfg = hyp_config_open(cache.dir);
    ASSERT_NOT_NULL(cfg);
    (void)hyp_config_set(cfg, HYP_CONFIG_ASK_ESC_KEY_ENV, OB_KEY_NAME);
    hyp_config_close(cfg);
    hyp_setenv(OB_KEY_NAME, OB_KEY_SENTINEL, 1);
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_TRUE(p.key_env_named);
    ASSERT_STR_EQ(p.key_env_name, OB_KEY_NAME);
    ASSERT_TRUE(p.key_env_set);
    char *json = hyp_onboard_probe_json(&p);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, OB_KEY_NAME));
    ASSERT_NULL(strstr(json, OB_KEY_SENTINEL));
    free(json);

    /* Unset: still named, reported not set. */
    hyp_unsetenv(OB_KEY_NAME);
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_TRUE(p.key_env_named);
    ASSERT_FALSE(p.key_env_set);

    th_cleanup(dir);
    free(dir);
    ob_cache_end(&cache);
    PASS();
}

/* ── B1 · Corpus and cost ────────────────────────────────────────── */

TEST(onboard_probe_corpus_is_exact_and_spans_absent_until_indexed) {
    ob_cache_t cache;
    ASSERT_TRUE(ob_cache_begin(&cache));
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    ob_enc_state_t st = {.claims_gpu = false, .contaminate_batches = false};
    hyp_ask_encoder_t enc = {.vt = &OB_ENC_VT, .self = &st};
    hyp_onboard_probe_opts_t opts = {.repo_path = dir, .encoder = &enc};
    hyp_onboard_probe_t p;
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_TRUE(p.corpus_walked);
    ASSERT_EQ(p.files, 2);
    /* Exact byte totals: the two fixture files, no estimate. */
    ASSERT_EQ(p.bytes, (int64_t)(strlen("int alpha(void) {\n    return 1;\n}\n"
                                        "int beta(void) {\n    return 2;\n}\n") +
                                 strlen("int gamma(void) {\n    return 3;\n}\n")));
    /* Never indexed: the span count is ABSENT — absent says "look after the
     * first index"; a zero would claim there is nothing. */
    ASSERT_FALSE(p.spans_known);
    ASSERT_NOT_NULL(strstr(p.spans_method, "after the first index"));
    char *json = hyp_onboard_probe_json(&p);
    ASSERT_NOT_NULL(json);
    ASSERT_NULL(strstr(json, "\"spans\":"));
    ASSERT_NOT_NULL(strstr(json, "\"spans_method\""));
    free(json);
    th_cleanup(dir);
    free(dir);
    ob_cache_end(&cache);
    PASS();
}

TEST(onboard_probe_cost_states_its_method) {
    ob_cache_t cache;
    ASSERT_TRUE(ob_cache_begin(&cache));
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);

    /* With an encoder: MEASURED, on this corpus, and the method names the
     * encoder — a stub measurement is visibly a stub measurement. */
    ob_enc_state_t st = {.claims_gpu = false, .contaminate_batches = false};
    hyp_ask_encoder_t enc = {.vt = &OB_ENC_VT, .self = &st};
    hyp_onboard_probe_opts_t opts = {.repo_path = dir, .encoder = &enc};
    hyp_onboard_probe_t p;
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_TRUE(p.encode_measured);
    ASSERT_TRUE(p.encode_docs_per_sec > 0);
    ASSERT_NOT_NULL(strstr(p.encode_method, "measured now"));
    ASSERT_NOT_NULL(strstr(p.encode_method, OB_ENC_MODEL_ID));
    ASSERT_TRUE(p.embed_estimate_known);
    ASSERT_NOT_NULL(strstr(p.cost_method, "floor"));

    /* THE FLAG IS NOT THE MEASUREMENT. `encode_measured` says a sample ran;
     * these say WHAT ran, and they are the assertion that survives a change of
     * clock. The first version of this probe timed with a millisecond clock,
     * read elapsed 0 on a sample that takes microseconds, and fell back to the
     * compiled-in constant — a real measurement downgraded by its own
     * stopwatch. A test that only checked the flag would have watched that
     * happen and called it a pass, so the operands of the rate are asserted
     * here and the rate is recomputed from them. */
    ASSERT_EQ(p.encode_sampled, 2); /* the fixture's two files, both sampled */
    ASSERT_TRUE(p.encode_elapsed_ns > 0);
    double recomputed = (double)p.encode_sampled / (p.encode_elapsed_ns / 1e9);
    ASSERT_TRUE(recomputed > p.encode_docs_per_sec * 0.99 &&
                recomputed < p.encode_docs_per_sec * 1.01);
    char *mjson = hyp_onboard_probe_json(&p);
    ASSERT_NOT_NULL(mjson);
    ASSERT_NOT_NULL(strstr(mjson, "\"encode_sampled\":2"));
    ASSERT_NOT_NULL(strstr(mjson, "\"encode_elapsed_ns\""));
    free(mjson);

    /* Without one (isolated cache: no weights, or no backend at all): the
     * constant is disclosed AS a constant, never passed off as a number about
     * this machine. */
    opts.encoder = NULL;
    ASSERT_EQ(hyp_onboard_probe(&opts, &p), 0);
    ASSERT_FALSE(p.encode_measured);
    ASSERT_NOT_NULL(strstr(p.encode_method, "NOT measured"));
    ASSERT_TRUE(p.embed_estimate_known); /* still extrapolated, from the constant */
    /* And the operands are ABSENT, not zero: nothing was sampled, so there is
     * nothing to report — the two are different answers. */
    ASSERT_EQ(p.encode_sampled, 0);
    mjson = hyp_onboard_probe_json(&p);
    ASSERT_NOT_NULL(mjson);
    ASSERT_NULL(strstr(mjson, "\"encode_sampled\""));
    ASSERT_NULL(strstr(mjson, "\"encode_elapsed_ns\""));
    free(mjson);

    th_cleanup(dir);
    free(dir);
    ob_cache_end(&cache);
    PASS();
}

/* ── B3 · The TOML ───────────────────────────────────────────────── */

TEST(onboard_toml_round_trips_and_twice_is_byte_identical) {
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    char other_buf[512];
    snprintf(other_buf, sizeof(other_buf), "%s/other", dir);
    ASSERT_EQ(th_mkdir_p(other_buf), 0);

    hyp_onboard_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    snprintf(plan.name, sizeof(plan.name), "ws-under-test");
    plan.repo_count = 2;
    snprintf(plan.repos[0].path, sizeof(plan.repos[0].path), "%s", dir);
    snprintf(plan.repos[0].role, sizeof(plan.repos[0].role), "member");
    snprintf(plan.repos[1].path, sizeof(plan.repos[1].path), "%s", other_buf);
    snprintf(plan.repos[1].role, sizeof(plan.repos[1].role), "reference");

    char err[512];
    err[0] = '\0';
    ASSERT_EQ(hyp_onboard_toml_write(dir, &plan, err, sizeof(err)), 0);

    hyp_onboard_plan_t got;
    ASSERT_EQ(hyp_onboard_toml_read(dir, &got, err, sizeof(err)), 1);
    ASSERT_STR_EQ(got.name, "ws-under-test");
    ASSERT_EQ(got.repo_count, 2);
    ASSERT_STR_EQ(got.repos[0].path, dir);
    ASSERT_STR_EQ(got.repos[0].role, "member");
    ASSERT_STR_EQ(got.repos[1].path, other_buf);
    ASSERT_STR_EQ(got.repos[1].role, "reference");

    /* Idempotent at the byte level: a second write of the same plan changes
     * nothing a diff (or a watcher) could see. */
    char toml_path[512];
    snprintf(toml_path, sizeof(toml_path), "%s/%s", dir, HYP_ONBOARD_TOML_NAME);
    FILE *f = fopen(toml_path, "rb");
    ASSERT_NOT_NULL(f);
    char before[8192];
    size_t before_len = fread(before, 1, sizeof(before), f);
    fclose(f);
    ASSERT_EQ(hyp_onboard_toml_write(dir, &plan, err, sizeof(err)), 0);
    f = fopen(toml_path, "rb");
    ASSERT_NOT_NULL(f);
    char after[8192];
    size_t after_len = fread(after, 1, sizeof(after), f);
    fclose(f);
    ASSERT_EQ((long long)before_len, (long long)after_len);
    ASSERT_EQ(memcmp(before, after, before_len), 0);

    /* Fail closed on a role or a name the scheme cannot hold. */
    snprintf(plan.repos[1].role, sizeof(plan.repos[1].role), "editable");
    ASSERT_NEQ(hyp_onboard_toml_write(dir, &plan, err, sizeof(err)), 0);
    snprintf(plan.repos[1].role, sizeof(plan.repos[1].role), "reference");
    snprintf(plan.name, sizeof(plan.name), "bad name with spaces");
    ASSERT_NEQ(hyp_onboard_toml_write(dir, &plan, err, sizeof(err)), 0);

    th_cleanup(dir);
    free(dir);
    PASS();
}

TEST(onboard_toml_unknown_role_reads_as_reference) {
    /* A5: intent that was never declared — or that this binary does not
     * recognise — reads as `reference`. Wrong-but-read-only is recoverable;
     * wrong-but-editable means an agent rewrites a dependency. */
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    char toml[1024];
    snprintf(toml, sizeof(toml),
             "name = \"ws\"\n"
             "\n"
             "[[repos]]\n"
             "path = \"%s\"\n"
             "role = \"editable-someday\"\n"
             "\n"
             "[[repos]]\n"
             "path = \"%s\"\n",
             dir, dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, HYP_ONBOARD_TOML_NAME), toml), 0);
    hyp_onboard_plan_t got;
    char err[512];
    ASSERT_EQ(hyp_onboard_toml_read(dir, &got, err, sizeof(err)), 1);
    ASSERT_EQ(got.repo_count, 2);
    ASSERT_STR_EQ(got.repos[0].role, "reference");
    ASSERT_STR_EQ(got.repos[1].role, "reference");
    th_cleanup(dir);
    free(dir);
    PASS();
}

TEST(onboard_a_legal_workspace_name_survives_the_plan_intact) {
    /* A derived slug runs to HYP_ADDR_SLUG_MAX bytes: fqn.c caps it there and
     * appends an FNV-1a hash of the FULL path, precisely so two deeply nested
     * repositories that share a long prefix keep distinct names. A plan field
     * too small to hold one truncates the hash away and both become the same
     * workspace — the A6 collision, reintroduced downstream of
     * onboard_check_collisions, which compares the untruncated slugs and so
     * cannot see it. The name is carried whole or this fails. */
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    char longname[HYP_ADDR_SLUG_MAX + 1];
    memset(longname, 'w', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    ASSERT_EQ((int)strlen(longname), HYP_ADDR_SLUG_MAX);

    hyp_onboard_answers_t a;
    memset(&a, 0, sizeof(a));
    a.name = longname;
    hyp_onboard_plan_t plan;
    char err[512];
    err[0] = '\0';
    ASSERT_EQ(hyp_onboard_flow(dir, NULL, &a, &plan, err, sizeof(err)), 0);
    /* Byte for byte, not a prefix of it. */
    ASSERT_STR_EQ(plan.name, longname);

    /* And it survives the record, so the file agrees with the plan. */
    ASSERT_EQ(hyp_onboard_toml_write(dir, &plan, err, sizeof(err)), 0);
    hyp_onboard_plan_t got;
    ASSERT_EQ(hyp_onboard_toml_read(dir, &got, err, sizeof(err)), 1);
    ASSERT_STR_EQ(got.name, longname);

    /* One byte over the cap is still refused — widening the field must not
     * have widened what the address scheme accepts. */
    char toolong[HYP_ADDR_SLUG_MAX + 2];
    memset(toolong, 'w', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    a.name = toolong;
    ASSERT_NEQ(hyp_onboard_flow(dir, NULL, &a, &plan, err, sizeof(err)), 0);

    th_cleanup(dir);
    free(dir);
    PASS();
}

/* ── B4 · Zero config ────────────────────────────────────────────── */

TEST(onboard_resolve_zero_config_is_the_workspace_of_one) {
    /* THE PATH DECISION. No TOML, no Multica, no flags: this repo, workspace
     * of one, role member, local nano, CPU, no shared feeds. The TOML's
     * absence is the MAIN path — making it a prerequisite fails here, which
     * is the second negative control's tripwire. */
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    hyp_onboard_resolved_t r;
    char err[512];
    err[0] = '\0';
    ASSERT_EQ(hyp_onboard_resolve(dir, &r, err, sizeof(err)), 0);
    ASSERT_STR_EQ(r.source, "defaults");
    ASSERT_TRUE(r.workspace_of_one);
    ASSERT_EQ(r.repo_count, 1);
    ASSERT_STR_EQ(r.repos[0].role, "member");
    ASSERT_STR_EQ(r.encoder, "local-nano");
    ASSERT_STR_EQ(r.device, "cpu");
    ASSERT_FALSE(r.shared_feeds);
    /* C1: the workspace id of a workspace of one is the repo's own slug.
     *
     * Compared against hyp_project_name_from_path, which is the function the
     * resolver itself calls — so on its own this line only says the two agree,
     * and a deriver returning "" for everything satisfies it. Two absolute
     * facts about the id go with it: it is non-empty, and it DISTINGUISHES.
     * A workspace id that is the same string for two different repositories is
     * A6's disjoint-address-space failure with the sign flipped — one address
     * space where there should be two — and the union merges that silently. */
    char *slug = hyp_project_name_from_path(dir);
    ASSERT_NOT_NULL(slug);
    ASSERT_GT((long long)strlen(slug), 0LL);
    ASSERT_STR_EQ(r.workspace, slug);

    char *other = ob_fixture_repo();
    ASSERT_NOT_NULL(other);
    hyp_onboard_resolved_t r2;
    ASSERT_EQ(hyp_onboard_resolve(other, &r2, err, sizeof(err)), 0);
    ASSERT_STR_NEQ(r2.workspace, r.workspace);
    th_cleanup(other);
    free(other);

    free(slug);
    th_cleanup(dir);
    free(dir);
    PASS();
}

TEST(onboard_resolve_reads_the_toml_and_refuses_a_slug_collision) {
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    /* <dir>/a/b and <dir>/a-b flatten to the same slug — A6's own example.
     * In separate databases that is harmless; inside ONE workspace it would
     * merge two repositories, so it is refused naming both paths. */
    char a_b[512];
    char a_dash_b[512];
    snprintf(a_b, sizeof(a_b), "%s/a/b", dir);
    snprintf(a_dash_b, sizeof(a_dash_b), "%s/a-b", dir);
    ASSERT_EQ(th_mkdir_p(a_b), 0);
    ASSERT_EQ(th_mkdir_p(a_dash_b), 0);

    char toml[2048];
    snprintf(toml, sizeof(toml),
             "name = \"named-in-toml\"\n"
             "[[repos]]\npath = \"%s\"\nrole = \"member\"\n",
             a_b);
    ASSERT_EQ(th_write_file(TH_PATH(dir, HYP_ONBOARD_TOML_NAME), toml), 0);
    hyp_onboard_resolved_t r;
    char err[512];
    err[0] = '\0';
    ASSERT_EQ(hyp_onboard_resolve(dir, &r, err, sizeof(err)), 0);
    ASSERT_STR_EQ(r.source, "toml");
    ASSERT_STR_EQ(r.workspace, "named-in-toml");

    snprintf(toml, sizeof(toml),
             "name = \"collides\"\n"
             "[[repos]]\npath = \"%s\"\nrole = \"member\"\n"
             "[[repos]]\npath = \"%s\"\nrole = \"member\"\n",
             a_b, a_dash_b);
    ASSERT_EQ(th_write_file(TH_PATH(dir, HYP_ONBOARD_TOML_NAME), toml), 0);
    ASSERT_NEQ(hyp_onboard_resolve(dir, &r, err, sizeof(err)), 0);
    /* The refusal NAMES BOTH PATHS — a collision one cannot locate is a
     * collision one cannot fix. */
    ASSERT_NOT_NULL(strstr(err, a_b));
    ASSERT_NOT_NULL(strstr(err, a_dash_b));
    th_cleanup(dir);
    free(dir);
    PASS();
}

/* ── B2 · The flow ───────────────────────────────────────────────── */

TEST(onboard_flow_defaults_are_the_proposal_and_answers_edit_it) {
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    char other[512];
    snprintf(other, sizeof(other), "%s/vendor-tree", dir);
    ASSERT_EQ(th_mkdir_p(other), 0);

    ob_enc_state_t st = {.claims_gpu = true, .contaminate_batches = false};
    hyp_ask_encoder_t enc = {.vt = &OB_ENC_VT, .self = &st};
    hyp_onboard_probe_opts_t popts = {.repo_path = dir, .encoder = &enc};
    hyp_onboard_probe_t probe;
    ASSERT_EQ(hyp_onboard_probe(&popts, &probe), 0);
    ASSERT_EQ(probe.gpu, HYP_ONBOARD_GPU_VERIFIED);

    /* Every answer empty: the plan IS the zero-config resolution, plus a
     * device the probe VERIFIED. Nothing was asked; nothing had to be. */
    hyp_onboard_plan_t plan;
    char err[512];
    err[0] = '\0';
    ASSERT_EQ(hyp_onboard_flow(dir, &probe, NULL, &plan, err, sizeof(err)), 0);
    ASSERT_EQ(plan.repo_count, 1);
    ASSERT_STR_EQ(plan.repos[0].role, "member");
    ASSERT_STR_EQ(plan.device, "gpu"); /* proposed FROM the verified run */
    ASSERT_FALSE(plan.spend);
    ASSERT_TRUE(plan.write_toml);

    /* The four answers, answered: each edits exactly its own thing. */
    hyp_onboard_answers_t a;
    memset(&a, 0, sizeof(a));
    a.repos = other;
    a.reference = other;
    a.spend = "yes";
    a.name = "renamed-ws";
    ASSERT_EQ(hyp_onboard_flow(dir, &probe, &a, &plan, err, sizeof(err)), 0);
    ASSERT_EQ(plan.repo_count, 2);
    ASSERT_STR_EQ(plan.repos[0].role, "member");
    ASSERT_STR_EQ(plan.repos[1].role, "reference");
    ASSERT_TRUE(plan.spend);
    ASSERT_STR_EQ(plan.name, "renamed-ws");

    /* Refusals, not silent corrections. */
    memset(&a, 0, sizeof(a));
    a.spend = "maybe";
    ASSERT_NEQ(hyp_onboard_flow(dir, &probe, &a, &plan, err, sizeof(err)), 0);
    memset(&a, 0, sizeof(a));
    a.name = "bad name";
    ASSERT_NEQ(hyp_onboard_flow(dir, &probe, &a, &plan, err, sizeof(err)), 0);
    memset(&a, 0, sizeof(a));
    a.reference = dir; /* not first added — but dir IS in the workspace */
    ASSERT_EQ(hyp_onboard_flow(dir, &probe, &a, &plan, err, sizeof(err)), 0);
    ASSERT_STR_EQ(plan.repos[0].role, "reference");

    th_cleanup(dir);
    free(dir);
    PASS();
}

TEST(onboard_flow_an_edit_cannot_assert_hardware) {
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    /* The probe's run said the GPU was not demonstrated; an EDIT saying
     * device=gpu is an assumption wearing a flag, and it is refused. */
    ob_enc_state_t st = {.claims_gpu = false, .contaminate_batches = false};
    hyp_ask_encoder_t enc = {.vt = &OB_ENC_VT, .self = &st};
    hyp_onboard_probe_opts_t popts = {.repo_path = dir, .encoder = &enc};
    hyp_onboard_probe_t probe;
    ASSERT_EQ(hyp_onboard_probe(&popts, &probe), 0);
    ASSERT_NEQ(probe.gpu, HYP_ONBOARD_GPU_VERIFIED);

    hyp_onboard_answers_t a;
    memset(&a, 0, sizeof(a));
    a.device = "gpu";
    hyp_onboard_plan_t plan;
    char err[512];
    err[0] = '\0';
    ASSERT_NEQ(hyp_onboard_flow(dir, &probe, &a, &plan, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, "refused"));

    /* Downgrading to cpu over a VERIFIED probe is always allowed — the human
     * may not want the display card busy. */
    st.claims_gpu = true;
    ASSERT_EQ(hyp_onboard_probe(&popts, &probe), 0);
    a.device = "cpu";
    ASSERT_EQ(hyp_onboard_flow(dir, &probe, &a, &plan, err, sizeof(err)), 0);
    ASSERT_STR_EQ(plan.device, "cpu");

    /* And the key-variable edit takes a NAME, never something key-shaped. */
    memset(&a, 0, sizeof(a));
    a.key_env = "MY_PROVIDER_KEY_VAR";
    ASSERT_EQ(hyp_onboard_flow(dir, &probe, &a, &plan, err, sizeof(err)), 0);
    ASSERT_STR_EQ(plan.key_env, "MY_PROVIDER_KEY_VAR");
    a.key_env = "sk-abc123-not-a-name";
    ASSERT_NEQ(hyp_onboard_flow(dir, &probe, &a, &plan, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, "never a key"));

    th_cleanup(dir);
    free(dir);
    PASS();
}

TEST(onboard_flow_toml_is_optional_never_a_prerequisite) {
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    hyp_onboard_answers_t a;
    memset(&a, 0, sizeof(a));
    a.write_toml = "no";
    hyp_onboard_plan_t plan;
    char err[512];
    err[0] = '\0';
    ASSERT_EQ(hyp_onboard_flow(dir, NULL, &a, &plan, err, sizeof(err)), 0);
    ASSERT_FALSE(plan.write_toml);
    /* Declining the record changes NOTHING downstream: the resolver still
     * answers, from defaults, on the repo with no TOML. */
    char toml_path[512];
    snprintf(toml_path, sizeof(toml_path), "%s/%s", dir, HYP_ONBOARD_TOML_NAME);
    ASSERT_FALSE(hyp_file_exists(toml_path));
    hyp_onboard_resolved_t r;
    ASSERT_EQ(hyp_onboard_resolve(dir, &r, err, sizeof(err)), 0);
    ASSERT_STR_EQ(r.source, "defaults");
    th_cleanup(dir);
    free(dir);
    PASS();
}

TEST(onboard_cmd_answers_file_runs_non_interactively) {
    ob_cache_t cache;
    ASSERT_TRUE(ob_cache_begin(&cache));
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);
    ASSERT_EQ(th_write_file(TH_PATH(dir, "answers.txt"),
                            "name=answered-ws\n"
                            "spend=no\n"
                            "write_toml=yes\n"),
              0);
    char answers_path[512];
    snprintf(answers_path, sizeof(answers_path), "%s/answers.txt", dir);

    char *argv1[] = {"--answers", answers_path, dir};
    ASSERT_EQ(hyp_cmd_onboard(3, argv1), 0);
    hyp_onboard_plan_t got;
    char err[512];
    ASSERT_EQ(hyp_onboard_toml_read(dir, &got, err, sizeof(err)), 1);
    ASSERT_STR_EQ(got.name, "answered-ws");

    /* Run it again: the record is byte-stable, the command is re-runnable. */
    ASSERT_EQ(hyp_cmd_onboard(3, argv1), 0);
    ASSERT_EQ(hyp_onboard_toml_read(dir, &got, err, sizeof(err)), 1);
    ASSERT_STR_EQ(got.name, "answered-ws");

    /* An unknown answer key is refused — a typo silently becoming "accept the
     * default" would be the quiet cousin of an interrogation. */
    ASSERT_EQ(th_write_file(TH_PATH(dir, "bad.txt"), "nmae=typo\n"), 0);
    char bad_path[512];
    snprintf(bad_path, sizeof(bad_path), "%s/bad.txt", dir);
    char *argv2[] = {"--answers", bad_path, dir};
    ASSERT_NEQ(hyp_cmd_onboard(3, argv2), 0);

    th_cleanup(dir);
    free(dir);
    ob_cache_end(&cache);
    PASS();
}

/* ── B4 · The G5 rehearsal: index and answer with nothing typed ──── */

TEST(onboard_zero_config_indexes_and_answers) {
    ob_cache_t cache;
    ASSERT_TRUE(ob_cache_begin(&cache));
    char *dir = ob_fixture_repo();
    ASSERT_NOT_NULL(dir);

    /* 1 · The path decision, asserted before anything runs: with no TOML the
     * resolver commits to local nano on CPU with no shared feeds. */
    hyp_onboard_resolved_t r;
    char err[512];
    err[0] = '\0';
    ASSERT_EQ(hyp_onboard_resolve(dir, &r, err, sizeof(err)), 0);
    ASSERT_STR_EQ(r.source, "defaults");
    ASSERT_STR_EQ(r.encoder, "local-nano");
    ASSERT_STR_EQ(r.device, "cpu");
    ASSERT_FALSE(r.shared_feeds);

    /* 2 · The index step: the PRODUCTION index_repository over the fixture,
     * no flags beyond the path. */
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", r.repos[0].path);
    char *resp = hyp_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    /* 3 · The embed pass — the real one — with the deterministic encoder
     * standing in for local nano, exactly as the existing ask tests stub it.
     * Default paths throughout: the graph and vector stores land where the
     * zero-config chain puts them, in the isolated cache. */
    ob_enc_state_t st = {.claims_gpu = false, .contaminate_batches = false};
    hyp_ask_encoder_t enc = {.vt = &OB_ENC_VT, .self = &st};
    hyp_ask_embed_opts_t eopts;
    memset(&eopts, 0, sizeof(eopts));
    eopts.project = r.workspace; /* the workspace of one IS the project */
    hyp_ask_embed_report_t rep;
    ASSERT_EQ(hyp_ask_embed_run(&enc, &eopts, &rep), 0);
    ASSERT_GT(rep.embedded, 0);
    hyp_ask_embed_report_free(&rep);

    /*
     * 4 · The answer, and "answer" is a claim about RANK.
     *
     * The fixture holds three declarations, `limit` is 10, and `ask` applies no
     * score threshold anywhere (mcp.c: "there is no score threshold in this
     * tool"). So every span comes back on every call, in every ordering, and
     * `strstr(text, "alpha")` is true of a reversed ranking, of a random score,
     * and of a zero query vector alike. Presence proves the index was searched;
     * it proves nothing about the answer, and this step is the one that claims
     * `hyp` on a fresh repo can ANSWER.
     *
     * What makes the rank exact rather than a tendency: the document side and
     * the query side share one axis hash, and hyp_ask_read_span_lines stops
     * before the newline that ends the span, so alpha's indexed text is
     * byte-identical to the question below. Cosine 1.0 for alpha, 0.0 for beta
     * and gamma — the same construction test_ask.c uses to pin an order.
     */
    ASSERT_EQ(hyp_ask_backend_install(&OB_BACKEND), 0);
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"limit\":10,"
             "\"question\":\"int alpha(void) {\\n    return 1;\\n}\"}",
             r.workspace);
    resp = hyp_mcp_handle_tool(srv, "ask", args);
    ASSERT_NOT_NULL(resp);
    char *text = ob_tool_text(resp);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "available: true"));
    ASSERT_NOT_NULL(strstr(text, "alpha"));
    ASSERT_NOT_NULL(strstr(text, "src/x.c"));
    /* Position, in the text an agent actually reads: the results table is
     * emitted in rank order, so the first mention of each name follows the
     * ranking. beta and gamma have to BE there for this to mean anything —
     * an ordering over one row is not an ordering. */
    const char *at_alpha = strstr(text, "alpha");
    const char *at_beta = strstr(text, "beta");
    const char *at_gamma = strstr(text, "gamma");
    ASSERT_NOT_NULL(at_beta);
    ASSERT_NOT_NULL(at_gamma);
    ASSERT_LT((long long)(at_alpha - text), (long long)(at_beta - text));
    ASSERT_LT((long long)(at_alpha - text), (long long)(at_gamma - text));
    free(text);
    free(resp);

    /* And the same question through the structured encoding, where rank is an
     * ARRAY INDEX rather than a byte offset, and the score separation that
     * produced it is readable. Column positions come from `cols`: the rows are
     * column-ordered arrays, so an added column would silently shift an index
     * written here by hand. */
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"limit\":10,\"format\":\"json\","
             "\"include_source\":false,"
             "\"question\":\"int alpha(void) {\\n    return 1;\\n}\"}",
             r.workspace);
    resp = hyp_mcp_handle_tool(srv, "ask", args);
    ASSERT_NOT_NULL(resp);
    char *jtext = ob_tool_text(resp);
    ASSERT_NOT_NULL(jtext);
    yyjson_doc *jdoc = yyjson_read(jtext, strlen(jtext), 0);
    ASSERT_NOT_NULL(jdoc);
    yyjson_val *jroot = yyjson_doc_get_root(jdoc);
    ASSERT_NOT_NULL(jroot);
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(jroot, "available")));

    yyjson_val *cols = yyjson_obj_get(jroot, "cols");
    ASSERT_NOT_NULL(cols);
    int qn_col = -1;
    int score_col = -1;
    size_t ci = 0;
    size_t cmax = 0;
    yyjson_val *cv = NULL;
    yyjson_arr_foreach(cols, ci, cmax, cv) {
        const char *cname = yyjson_get_str(cv);
        if (cname && strcmp(cname, "qn") == 0) {
            qn_col = (int)ci;
        } else if (cname && strcmp(cname, "score") == 0) {
            score_col = (int)ci;
        }
    }
    ASSERT_GTE(qn_col, 0);
    ASSERT_GTE(score_col, 0);

    yyjson_val *rows = yyjson_obj_get(jroot, "rows");
    ASSERT_NOT_NULL(rows);
    /* Three declarations were indexed and nothing filters them, so three rows
     * is what a working lane returns. Pinned absolutely: a single-row answer
     * would make every ordering assertion below vacuously true. */
    ASSERT_EQ((long long)yyjson_arr_size(rows), 3);

    yyjson_val *row0 = yyjson_arr_get(rows, 0);
    ASSERT_NOT_NULL(row0);
    const char *qn0 = yyjson_get_str(yyjson_arr_get(row0, (size_t)qn_col));
    ASSERT_NOT_NULL(qn0);
    ASSERT_NOT_NULL(strstr(qn0, "alpha"));
    double s0 = yyjson_get_num(yyjson_arr_get(row0, (size_t)score_col));
    /* The question IS alpha's span, so the top score is the exact cosine and
     * not merely the largest of three numbers that happen to be equal. */
    ASSERT_GT((long long)(s0 * 1000000.0), 990000LL);
    for (size_t ri = 1; ri < yyjson_arr_size(rows); ri++) {
        yyjson_val *row = yyjson_arr_get(rows, ri);
        ASSERT_NOT_NULL(row);
        const char *qn = yyjson_get_str(yyjson_arr_get(row, (size_t)qn_col));
        ASSERT_NOT_NULL(qn);
        ASSERT_NULL(strstr(qn, "alpha"));
        double s = yyjson_get_num(yyjson_arr_get(row, (size_t)score_col));
        /* Strictly below, so the order is a separation and not a tie the sort
         * happened to break in our favour. */
        ASSERT_LT((long long)(s * 1000000.0), (long long)(s0 * 1000000.0));
    }
    yyjson_doc_free(jdoc);
    free(jtext);
    free(resp);

    hyp_ask_backend_install(NULL);
    hyp_mcp_server_free(srv);
    th_cleanup(dir);
    free(dir);
    ob_cache_end(&cache);
    PASS();
}

SUITE(onboard) {
    RUN_TEST(onboard_the_four_questions_are_exactly_four);
    RUN_TEST(onboard_probe_gpu_failed_when_the_run_fails);
    RUN_TEST(onboard_probe_gpu_verified_only_by_a_passing_run_on_a_gpu);
    RUN_TEST(onboard_probe_gpu_reports_what_this_build_can_do);
    RUN_TEST(onboard_probe_key_variable_names_and_never_values);
    RUN_TEST(onboard_probe_corpus_is_exact_and_spans_absent_until_indexed);
    RUN_TEST(onboard_probe_cost_states_its_method);
    RUN_TEST(onboard_toml_round_trips_and_twice_is_byte_identical);
    RUN_TEST(onboard_toml_unknown_role_reads_as_reference);
    RUN_TEST(onboard_a_legal_workspace_name_survives_the_plan_intact);
    RUN_TEST(onboard_resolve_zero_config_is_the_workspace_of_one);
    RUN_TEST(onboard_resolve_reads_the_toml_and_refuses_a_slug_collision);
    RUN_TEST(onboard_flow_defaults_are_the_proposal_and_answers_edit_it);
    RUN_TEST(onboard_flow_an_edit_cannot_assert_hardware);
    RUN_TEST(onboard_flow_toml_is_optional_never_a_prerequisite);
    RUN_TEST(onboard_cmd_answers_file_runs_non_interactively);
    RUN_TEST(onboard_zero_config_indexes_and_answers);
}
