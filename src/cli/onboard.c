/*
 * onboard.c — NEXT-STEPS §4 Track B: probe (B1), flow (B2), TOML (B3), and
 * the zero-config resolution (B4). See onboard.h for the contract; comments
 * here are about the choices, not the plumbing.
 */
#include "cli/onboard.h"

#include "ask/ask_embed.h" /* HYP_ASK_CPU_DOCS_PER_SEC — the disclosed fallback */
#include "ask/ask_llama.h"
#include "ask/ask_vectors.h"
#include "cli/cli.h" /* hyp_config_*, HYP_CONFIG_ASK_ESC_KEY_ENV */
#include "cli/model_fetch.h"
#include "discover/discover.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/identity.h" /* HYP_ADDR_SLUG_MAX — the C1 cap, not a new one */
#include "foundation/platform.h"
#include "foundation/str_util.h" /* hyp_validate_project_name — THE slug validator */
#include "pipeline/pipeline.h"   /* hyp_project_name_from_path */

#include <yyjson/yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h> /* isatty */
#else
#include <io.h>
#define isatty _isatty
#endif

enum {
    ONBOARD_SAMPLE_DEFAULT = 8,  /* files measured for the cost estimate */
    ONBOARD_SAMPLE_TRUNC = 2048, /* bytes per sample document — declaration-sized */
    ONBOARD_VERIFY_GROUP = 3,    /* forms ragged groups over the fixture below */
    ONBOARD_ANSWER_LINE = 4096,  /* one answers-file line */
    ONBOARD_MS_PER_SEC = 1000,
};

/* The measured bar of `embed --verify-batching` (ask_cmd.c): an order of
 * magnitude above the alone-vs-alone arithmetic floor's worst deficit and two
 * below the retracted failure's best. Same check, same bar — a second constant
 * would be a second thing to drift. */
static const float ONBOARD_VERIFY_THRESHOLD = 0.999F;

/* Fixture for the batching check: few texts, deliberately DIFFERENT lengths,
 * so the groups are ragged the way a real length-sorted pass is — the exact
 * shape that produced §3's wrong batched vectors. */
static const char *const ONBOARD_VERIFY_TEXTS[] = {
    "int add(int a, int b) { return a + b; }",
    "x = 1",
    "static void clear(char *p, size_t n) {\n    memset(p, 0, n);\n}",
    "bool is_prime(unsigned n) {\n    if (n < 2) return false;\n    for (unsigned d = 2; "
    "d * d <= n; d++)\n        if (n % d == 0) return false;\n    return true;\n}",
    "#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))",
    "def parse(line):\n    return [int(x) for x in line.split(',') if x]",
};
enum {
    ONBOARD_VERIFY_COUNT = (int)(sizeof(ONBOARD_VERIFY_TEXTS) / sizeof(ONBOARD_VERIFY_TEXTS[0]))
};

/* ── Roles ──────────────────────────────────────────────────────── */

/* member and reference are declared; the other three are what A2-A4 derive,
 * listed so a TOML that records a derived role round-trips instead of being
 * rewritten to something weaker. */
static const char *const ONBOARD_ROLES[] = {
    HYP_ONBOARD_ROLE_MEMBER, HYP_ONBOARD_ROLE_REFERENCE, "vendored", "generated", "upstream",
};
enum { ONBOARD_ROLE_COUNT = (int)(sizeof(ONBOARD_ROLES) / sizeof(ONBOARD_ROLES[0])) };

bool hyp_onboard_role_valid(const char *role) {
    if (!role || !role[0]) {
        return false;
    }
    for (int i = 0; i < ONBOARD_ROLE_COUNT; i++) {
        if (strcmp(role, ONBOARD_ROLES[i]) == 0) {
            return true;
        }
    }
    return false;
}

const char *hyp_onboard_gpu_str(hyp_onboard_gpu_t g) {
    switch (g) {
    case HYP_ONBOARD_GPU_NOT_IN_BUILD:
        return "not_in_build";
    case HYP_ONBOARD_GPU_NO_MODEL:
        return "no_model";
    case HYP_ONBOARD_GPU_UNUSABLE:
        return "unusable";
    case HYP_ONBOARD_GPU_FAILED:
        return "failed";
    case HYP_ONBOARD_GPU_VERIFIED:
        return "verified";
    }
    return "unknown";
}

/* ── B1 · GPU: run the check, report what ran ───────────────────── */

/* Run the batching self-check on `enc` and write the verdict + evidence.
 * The verdict is DERIVED from (what the check said, what the encoder says it
 * runs on) and from nothing else — no field of the report can say VERIFIED
 * without this function having executed the check. */
static void onboard_gpu_run_check(hyp_ask_encoder_t *enc, hyp_onboard_probe_t *r) {
    hyp_ask_batch_check_t chk;
    memset(&chk, 0, sizeof(chk));
    const char *note = hyp_ask_encoder_device_note(enc);
    (void)snprintf(r->gpu_device, sizeof(r->gpu_device), "%s", note ? note : "unreported");
    int rc =
        hyp_ask_encoder_check_batching(enc, ONBOARD_VERIFY_TEXTS, ONBOARD_VERIFY_COUNT,
                                       ONBOARD_VERIFY_GROUP, ONBOARD_VERIFY_THRESHOLD, NULL, &chk);
    if (rc != 0) {
        /* The encoder failed outright: nothing was compared, so nothing is
         * verified. An encoder that cannot encode is not a working GPU. */
        r->gpu = HYP_ONBOARD_GPU_FAILED;
        r->gpu_check_ran = false;
        (void)snprintf(r->gpu_evidence, sizeof(r->gpu_evidence),
                       "the batching check could not run on %s: the encoder failed before "
                       "anything was compared",
                       r->gpu_device);
        return;
    }
    r->gpu_check_ran = true;
    r->gpu_check = chk;
    bool pass = chk.below == 0 && chk.misassigned == 0;
    bool on_gpu = hyp_ask_encoder_device_is_gpu(enc);
    if (!on_gpu) {
        /* A passing check on a CPU encoder verifies the CPU, which was never
         * in question. GPU capability was not demonstrated and is not
         * claimed. */
        r->gpu = HYP_ONBOARD_GPU_UNUSABLE;
        (void)snprintf(r->gpu_evidence, sizeof(r->gpu_evidence),
                       "the encoder that answered runs on %s, not a GPU; the batching check "
                       "%s there (min own-cosine %.6f), which says nothing about a GPU",
                       r->gpu_device, pass ? "passed" : "FAILED", (double)chk.min_own);
        return;
    }
    r->gpu = pass ? HYP_ONBOARD_GPU_VERIFIED : HYP_ONBOARD_GPU_FAILED;
    (void)snprintf(r->gpu_evidence, sizeof(r->gpu_evidence),
                   "ran the batching self-check on %s: %d texts alone then in groups of %d, "
                   "min own-cosine %.6f (bar %.3f), below-bar %d, misassigned %d — %s",
                   r->gpu_device, chk.n, ONBOARD_VERIFY_GROUP, (double)chk.min_own,
                   (double)ONBOARD_VERIFY_THRESHOLD, chk.below, chk.misassigned,
                   pass ? "PASS" : "FAIL");
}

/* Returns the encoder used for the GPU verdict so the cost probe can reuse it
 * (one model load, two questions). *out_owned says whether to destroy it. */
static hyp_ask_encoder_t *onboard_gpu_probe(const hyp_onboard_probe_opts_t *opts,
                                            hyp_onboard_probe_t *r, bool *out_owned) {
    *out_owned = false;
    if (opts->encoder) {
        onboard_gpu_run_check(opts->encoder, r);
        return opts->encoder;
    }
    if (!hyp_ask_llama_compiled_in()) {
        r->gpu = HYP_ONBOARD_GPU_NOT_IN_BUILD;
        (void)snprintf(r->gpu_evidence, sizeof(r->gpu_evidence),
                       "this binary has no inference backend (%s); there is nothing to run "
                       "the check on",
                       hyp_ask_llama_build_note());
        return NULL;
    }
    if (!hyp_model_ask_present() || !hyp_model_spec_present(hyp_model_ask_proj_spec())) {
        /* Cannot run means UNKNOWN, and unknown is never yes. */
        r->gpu = HYP_ONBOARD_GPU_NO_MODEL;
        (void)snprintf(r->gpu_evidence, sizeof(r->gpu_evidence),
                       "the ask lane's model is not on this machine, so the check cannot RUN "
                       "and GPU capability is unknown — `%s` fetches it",
                       HYP_MODEL_ASK_COMMAND);
        return NULL;
    }
    char err[HYP_SZ_512];
    err[0] = '\0';
    /* DEVICE_GPU refuses rather than silently falling back, so a returned
     * encoder is a GPU encoder and a NULL is a named refusal — both are
     * evidence about THIS build on THIS card. */
    hyp_ask_encoder_t *enc = hyp_ask_llama_encoder_create(HYP_ASK_DEVICE_GPU, err, sizeof(err));
    if (!enc) {
        r->gpu = HYP_ONBOARD_GPU_UNUSABLE;
        (void)snprintf(r->gpu_evidence, sizeof(r->gpu_evidence), "%s",
                       err[0] ? err : "the GPU encoder could not be created");
        return NULL;
    }
    *out_owned = true;
    onboard_gpu_run_check(enc, r);
    return enc;
}

/* ── B1 · The key variable: a NAME and a NULL check ─────────────── */

static void onboard_key_probe(hyp_onboard_probe_t *r) {
    const char *cache = hyp_resolve_cache_dir();
    hyp_config_t *cfg = cache ? hyp_config_open(cache) : NULL;
    if (cfg) {
        const char *name = hyp_config_get(cfg, HYP_CONFIG_ASK_ESC_KEY_ENV, "");
        if (name && name[0]) {
            r->key_env_named = true;
            (void)snprintf(r->key_env_name, sizeof(r->key_env_name), "%s", name);
        }
        hyp_config_close(cfg);
    }
    if (r->key_env_named) {
        /* THE ONLY expression that touches the variable, and it extracts one
         * bit: present or absent. The value is never copied, compared,
         * logged or printed — that is the frozen rule, not a style choice. */
        r->key_env_set = getenv(r->key_env_name) != NULL;
    }
}

/* ── B1 · Corpus ────────────────────────────────────────────────── */

/* The discover result is handed back so the cost probe can sample real files
 * from this corpus instead of a synthetic fixture. Caller frees. */
static void onboard_corpus_probe(const char *repo_path, hyp_onboard_probe_t *r,
                                 hyp_file_info_t **out_files, int *out_count) {
    *out_files = NULL;
    *out_count = 0;
    hyp_discover_opts_t d;
    memset(&d, 0, sizeof(d));
    d.mode = HYP_MODE_FULL;
    uint64_t t0 = hyp_now_ms();
    hyp_file_info_t *fi = NULL;
    int n = 0;
    if (hyp_discover(repo_path, &d, &fi, &n) != 0) {
        return; /* corpus_walked stays false — reported, not invented */
    }
    r->corpus_walked = true;
    r->discover_ms = (double)(hyp_now_ms() - t0);
    r->files = n;
    for (int i = 0; i < n; i++) {
        r->bytes += fi[i].size > 0 ? fi[i].size : 0;
    }
    *out_files = fi;
    *out_count = n;

    /* Spans are counted, never estimated: the count is a property of an index
     * and exists exactly when one does. Reported ABSENT otherwise — an absent
     * count says "look after the first index"; a 0 would say "there is
     * nothing", and only one of those is ever true. */
    char *slug = hyp_project_name_from_path(repo_path);
    char vec_path[HYP_SZ_4K];
    if (slug && hyp_ask_vectors_path(slug, vec_path, sizeof(vec_path)) &&
        hyp_file_exists(vec_path)) {
        hyp_ask_vectors_t *v = hyp_ask_vectors_open_path(slug, vec_path);
        if (v) {
            int64_t count = hyp_ask_vectors_count(v);
            if (count > 0) {
                r->spans_known = true;
                r->spans = count;
                (void)snprintf(r->spans_method, sizeof(r->spans_method),
                               "counted from the existing ask index for '%s'", slug);
            }
            hyp_ask_vectors_close(v);
        }
    }
    if (!r->spans_known) {
        (void)snprintf(r->spans_method, sizeof(r->spans_method),
                       "absent: a span population exists only after the first index; the "
                       "file and byte counts above are exact");
    }
    free(slug);
}

/* ── B1 · Cost: measure a sample, extrapolate, say how ──────────── */

static char *onboard_slurp_capped(const char *path, size_t cap, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = malloc(cap + 1);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, cap, f);
    (void)fclose(f);
    buf[got] = '\0';
    if (out_len) {
        *out_len = got;
    }
    return buf;
}

static void onboard_cost_probe(const hyp_onboard_probe_opts_t *opts, hyp_onboard_probe_t *r,
                               hyp_ask_encoder_t *enc, const hyp_file_info_t *files, int count) {
    if (enc && r->corpus_walked && count > 0) {
        int want = opts->sample_files > 0 ? opts->sample_files : ONBOARD_SAMPLE_DEFAULT;
        if (want > count) {
            want = count;
        }
        int dim = hyp_ask_encoder_dim(enc);
        float *vec = dim > 0 ? malloc((size_t)dim * sizeof(float)) : NULL;
        int done = 0;
        uint64_t t0 = hyp_now_ms();
        for (int i = 0; i < want && vec; i++) {
            /* Strided, so the sample spans the corpus rather than one dir. */
            const hyp_file_info_t *fi = &files[(int64_t)i * count / want];
            char *text = onboard_slurp_capped(fi->path, ONBOARD_SAMPLE_TRUNC, NULL);
            if (!text || !text[0]) {
                free(text);
                continue;
            }
            const char *one[1] = {text};
            /* One at a time, deliberately: the zero-config first embed is the
             * cost being predicted, and per-document is its floor. */
            if (hyp_ask_encode_documents(enc, one, 1, vec) == 0) {
                done++;
            }
            free(text);
        }
        double ms = (double)(hyp_now_ms() - t0);
        free(vec);
        if (done > 0 && ms > 0) {
            r->encode_measured = true;
            r->encode_docs_per_sec = (double)done / (ms / ONBOARD_MS_PER_SEC);
            (void)snprintf(
                r->encode_method, sizeof(r->encode_method),
                "measured now: %d sample documents from this corpus (each capped at "
                "%d bytes), encoded one at a time by %s on %s in %.0f ms",
                done, ONBOARD_SAMPLE_TRUNC,
                hyp_ask_encoder_model_id(enc) ? hyp_ask_encoder_model_id(enc) : "unreported",
                hyp_ask_encoder_device_note(enc) ? hyp_ask_encoder_device_note(enc) : "unreported",
                ms);
        }
    }
    if (!r->encode_measured) {
        /* A constant is not a measurement, and the method says so out loud. */
        r->encode_docs_per_sec = HYP_ASK_CPU_DOCS_PER_SEC;
        (void)snprintf(r->encode_method, sizeof(r->encode_method),
                       "compiled-in constant (%.3f docs/s on CPU, measured on the reference "
                       "corpus) — this machine was NOT measured: no encoder is available to "
                       "sample",
                       HYP_ASK_CPU_DOCS_PER_SEC);
    }
    if (r->corpus_walked && r->files > 0 && r->encode_docs_per_sec > 0) {
        /* Denominator preference: the real span count when an index exists,
         * else one document per file — a FLOOR, and named as one, because
         * every count is a floor and the per-declaration population only
         * exists after the first index. */
        int64_t docs = r->spans_known ? r->spans : (int64_t)r->files;
        r->embed_estimate_known = true;
        r->embed_seconds = (double)docs / r->encode_docs_per_sec;
        (void)snprintf(r->cost_method, sizeof(r->cost_method),
                       "%lld documents (%s) / %.3f docs-per-sec (%s); plus the structural "
                       "index, whose file walk measured %.0f ms here",
                       (long long)docs,
                       r->spans_known ? "the indexed span count"
                                      : "one per file — a floor until the first index",
                       r->encode_docs_per_sec, r->encode_measured ? "measured" : "constant",
                       r->discover_ms);
    } else {
        (void)snprintf(r->cost_method, sizeof(r->cost_method),
                       "absent: the corpus walk did not complete, so there is no denominator "
                       "to extrapolate from");
    }
}

int hyp_onboard_probe(const hyp_onboard_probe_opts_t *opts, hyp_onboard_probe_t *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!opts || !opts->repo_path || !hyp_is_dir(opts->repo_path)) {
        return -1;
    }
    bool owned = false;
    hyp_ask_encoder_t *enc = onboard_gpu_probe(opts, out, &owned);
    onboard_key_probe(out);
    hyp_file_info_t *files = NULL;
    int count = 0;
    onboard_corpus_probe(opts->repo_path, out, &files, &count);
    onboard_cost_probe(opts, out, enc, files, count);
    hyp_discover_free(files, count);
    if (owned) {
        hyp_ask_encoder_destroy(enc);
    }
    return 0;
}

char *hyp_onboard_probe_json(const hyp_onboard_probe_t *p) {
    if (!p) {
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *gpu = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, gpu, "state", hyp_onboard_gpu_str(p->gpu));
    yyjson_mut_obj_add_bool(doc, gpu, "check_ran", p->gpu_check_ran);
    if (p->gpu_check_ran) {
        yyjson_mut_val *chk = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, chk, "texts", p->gpu_check.n);
        yyjson_mut_obj_add_int(doc, chk, "groups", p->gpu_check.groups);
        yyjson_mut_obj_add_real(doc, chk, "min_own_cosine", (double)p->gpu_check.min_own);
        yyjson_mut_obj_add_int(doc, chk, "below_bar", p->gpu_check.below);
        yyjson_mut_obj_add_int(doc, chk, "misassigned", p->gpu_check.misassigned);
        yyjson_mut_obj_add_val(doc, gpu, "check", chk);
    }
    if (p->gpu_device[0]) {
        yyjson_mut_obj_add_strcpy(doc, gpu, "device", p->gpu_device);
    }
    yyjson_mut_obj_add_strcpy(doc, gpu, "evidence", p->gpu_evidence);
    yyjson_mut_obj_add_val(doc, root, "gpu", gpu);

    yyjson_mut_val *key = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, key, "named", p->key_env_named);
    if (p->key_env_named) {
        /* The NAME. The value has no representation in this program. */
        yyjson_mut_obj_add_strcpy(doc, key, "name", p->key_env_name);
        yyjson_mut_obj_add_bool(doc, key, "set", p->key_env_set);
    }
    yyjson_mut_obj_add_val(doc, root, "key_variable", key);

    yyjson_mut_val *corpus = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, corpus, "walked", p->corpus_walked);
    if (p->corpus_walked) {
        yyjson_mut_obj_add_int(doc, corpus, "files", p->files);
        yyjson_mut_obj_add_int(doc, corpus, "bytes", p->bytes);
        yyjson_mut_obj_add_real(doc, corpus, "discover_ms", p->discover_ms);
    }
    if (p->spans_known) {
        /* Present exactly when counted. Never 0-for-unknown. */
        yyjson_mut_obj_add_int(doc, corpus, "spans", p->spans);
    }
    yyjson_mut_obj_add_strcpy(doc, corpus, "spans_method", p->spans_method);
    yyjson_mut_obj_add_val(doc, root, "corpus", corpus);

    yyjson_mut_val *cost = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, cost, "encode_measured", p->encode_measured);
    yyjson_mut_obj_add_real(doc, cost, "encode_docs_per_sec", p->encode_docs_per_sec);
    yyjson_mut_obj_add_strcpy(doc, cost, "encode_method", p->encode_method);
    if (p->embed_estimate_known) {
        yyjson_mut_obj_add_real(doc, cost, "embed_seconds", p->embed_seconds);
    }
    yyjson_mut_obj_add_strcpy(doc, cost, "method", p->cost_method);
    yyjson_mut_obj_add_val(doc, root, "first_index", cost);

    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

/* ── B3 · TOML ──────────────────────────────────────────────────── */

/* TOML basic-string escaping: backslash and double quote. Paths are the only
 * strings that can carry either. */
static void onboard_toml_escape(const char *s, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 2 < out_sz; i++) {
        if (s[i] == '\\' || s[i] == '"') {
            out[j++] = '\\';
        }
        out[j++] = s[i];
    }
    out[j] = '\0';
}

static bool onboard_toml_unescape(const char *s, size_t len, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\\') {
            if (i + 1 >= len) {
                return false;
            }
            c = s[++i];
            if (c != '\\' && c != '"') {
                return false; /* an escape this reader does not speak: refuse */
            }
        }
        if (j + 1 >= out_sz) {
            return false;
        }
        out[j++] = c;
    }
    out[j] = '\0';
    return true;
}

static int onboard_toml_render(const hyp_onboard_plan_t *plan, char *buf, size_t buf_sz) {
    char esc[HYP_PATH_MAX * 2];
    onboard_toml_escape(plan->name, esc, sizeof(esc));
    int off = snprintf(buf, buf_sz,
                       "# %s — the record of `hyponoia onboard`.\n"
                       "# Optional: hyp works in this repository without it.\n"
                       "name = \"%s\"\n",
                       HYP_ONBOARD_TOML_NAME, esc);
    for (int i = 0; i < plan->repo_count && off >= 0 && (size_t)off < buf_sz; i++) {
        onboard_toml_escape(plan->repos[i].path, esc, sizeof(esc));
        off += snprintf(buf + off, buf_sz - (size_t)off,
                        "\n[[repos]]\npath = \"%s\"\nrole = \"%s\"\n", esc, plan->repos[i].role);
    }
    return (off >= 0 && (size_t)off < buf_sz) ? off : -1;
}

int hyp_onboard_toml_write(const char *dir, const hyp_onboard_plan_t *plan, char *err,
                           size_t errlen) {
    if (!dir || !plan || plan->repo_count < 1 || plan->repo_count > HYP_ONBOARD_MAX_REPOS) {
        (void)snprintf(err, errlen, "nothing to record: the plan has no repositories");
        return -1;
    }
    if (!plan->name[0] || strlen(plan->name) > (size_t)HYP_ADDR_SLUG_MAX ||
        !hyp_validate_project_name(plan->name)) {
        (void)snprintf(err, errlen,
                       "'%s' is not a workspace name the address scheme accepts ([A-Za-z0-9._-])",
                       plan->name);
        return -1;
    }
    for (int i = 0; i < plan->repo_count; i++) {
        if (!hyp_onboard_role_valid(plan->repos[i].role)) {
            (void)snprintf(err, errlen, "'%s' is not a role this writer will record",
                           plan->repos[i].role);
            return -1;
        }
    }
    char rendered[HYP_SZ_32K];
    int len = onboard_toml_render(plan, rendered, sizeof(rendered));
    if (len < 0) {
        (void)snprintf(err, errlen, "the plan does not fit the %d-byte render buffer",
                       (int)sizeof(rendered));
        return -1;
    }
    char path[HYP_SZ_4K];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, HYP_ONBOARD_TOML_NAME);

    /* Idempotence is byte-level: a second run against an unchanged plan leaves
     * the file untouched (same bytes, same mtime), so the record never churns
     * under a watcher. */
    FILE *existing = fopen(path, "rb");
    if (existing) {
        char have[HYP_SZ_32K];
        size_t got = fread(have, 1, sizeof(have) - 1, existing);
        (void)fclose(existing);
        have[got] = '\0';
        if ((int)got == len && memcmp(have, rendered, (size_t)len) == 0) {
            return 0;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        (void)snprintf(err, errlen, "cannot write %s", path);
        return -1;
    }
    size_t wrote = fwrite(rendered, 1, (size_t)len, f);
    int close_rc = fclose(f);
    if (wrote != (size_t)len || close_rc != 0) {
        (void)snprintf(err, errlen, "short write to %s", path);
        return -1;
    }
    return 0;
}

/* One trimmed line: `key = "value"`. Returns true and fills value when the
 * line is a quoted assignment of `key`. */
static bool onboard_toml_kv(const char *line, const char *key, char *out, size_t out_sz) {
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) {
        return false;
    }
    const char *p = line + klen;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '=') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    const char *end = p;
    while (*end && !(*end == '"' && end[-1] != '\\')) {
        end++;
    }
    if (*end != '"') {
        return false;
    }
    return onboard_toml_unescape(p, (size_t)(end - p), out, out_sz);
}

int hyp_onboard_toml_read(const char *dir, hyp_onboard_plan_t *out, char *err, size_t errlen) {
    if (!dir || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char path[HYP_SZ_4K];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, HYP_ONBOARD_TOML_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0; /* absent is the zero-config path, not a failure */
    }
    char line[HYP_SZ_4K];
    int cur = -1; /* index into out->repos; -1 = top level */
    bool cur_has_path = false;
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
            s[--n] = '\0';
        }
        if (!s[0] || s[0] == '#') {
            continue;
        }
        if (strcmp(s, "[[repos]]") == 0) {
            if (cur >= 0 && !cur_has_path) {
                (void)fclose(f);
                (void)snprintf(err, errlen, "%s: a [[repos]] entry has no path", path);
                return -1;
            }
            if (out->repo_count >= HYP_ONBOARD_MAX_REPOS) {
                (void)fclose(f);
                (void)snprintf(err, errlen, "%s: more than %d repos", path, HYP_ONBOARD_MAX_REPOS);
                return -1;
            }
            cur = out->repo_count++;
            cur_has_path = false;
            /* A5: intent that was never declared reads as `reference` —
             * wrong-but-read-only is the recoverable wrong. */
            (void)snprintf(out->repos[cur].role, sizeof(out->repos[cur].role), "%s",
                           HYP_ONBOARD_ROLE_REFERENCE);
            continue;
        }
        char val[HYP_PATH_MAX];
        if (cur < 0) {
            if (onboard_toml_kv(s, "name", val, sizeof(val))) {
                (void)snprintf(out->name, sizeof(out->name), "%s", val);
            }
            /* Unknown top-level keys: ignored, so a future A6 field does not
             * break this binary. */
            continue;
        }
        if (onboard_toml_kv(s, "path", val, sizeof(val))) {
            (void)snprintf(out->repos[cur].path, sizeof(out->repos[cur].path), "%s", val);
            cur_has_path = true;
        } else if (onboard_toml_kv(s, "role", val, sizeof(val))) {
            /* A5 again: a role this binary does not recognise defaults to
             * reference rather than to editable. */
            (void)snprintf(out->repos[cur].role, sizeof(out->repos[cur].role), "%s",
                           hyp_onboard_role_valid(val) ? val : HYP_ONBOARD_ROLE_REFERENCE);
        }
    }
    (void)fclose(f);
    if (cur >= 0 && !cur_has_path) {
        (void)snprintf(err, errlen, "%s: a [[repos]] entry has no path", path);
        return -1;
    }
    return 1;
}

/* ── B4 · Zero config ───────────────────────────────────────────── */

/* Slug for one repo path. Returns false when it cannot be derived. */
static bool onboard_repo_slug(const char *path, char *out, size_t out_sz) {
    char *slug = hyp_project_name_from_path(path);
    if (!slug || !slug[0]) {
        free(slug);
        return false;
    }
    (void)snprintf(out, out_sz, "%s", slug);
    free(slug);
    return true;
}

/* Refuse a slug collision inside one workspace, naming BOTH paths (A6): /a/b
 * and /a-b are harmless in separate databases and merge two repos in one. */
static int onboard_check_collisions(const hyp_onboard_repo_t *repos, int count, char *err,
                                    size_t errlen) {
    char slugs[HYP_ONBOARD_MAX_REPOS][HYP_SZ_256];
    for (int i = 0; i < count; i++) {
        if (!onboard_repo_slug(repos[i].path, slugs[i], sizeof(slugs[i]))) {
            (void)snprintf(err, errlen, "no project slug can be derived from '%s'", repos[i].path);
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(slugs[i], slugs[j]) == 0 && strcmp(repos[i].path, repos[j].path) != 0) {
                (void)snprintf(err, errlen,
                               "REFUSED: '%s' and '%s' both slug to '%s' — inside one "
                               "workspace that would merge two repositories",
                               repos[j].path, repos[i].path, slugs[i]);
                return -1;
            }
        }
    }
    return 0;
}

/* A deterministic name from the member set: sorted slugs, joined. Never "the
 * first repo's slug" — two machines listing the same set in different orders
 * must derive the same workspace id, or C6u's union merges two address spaces
 * silently. */
static int onboard_name_from_members(const hyp_onboard_repo_t *repos, int count, char *out,
                                     size_t out_sz, char *err, size_t errlen) {
    char slugs[HYP_ONBOARD_MAX_REPOS][HYP_SZ_256];
    for (int i = 0; i < count; i++) {
        if (!onboard_repo_slug(repos[i].path, slugs[i], sizeof(slugs[i]))) {
            (void)snprintf(err, errlen, "no project slug can be derived from '%s'", repos[i].path);
            return -1;
        }
    }
    for (int i = 1; i < count; i++) { /* insertion sort: the set is tiny */
        char tmp[HYP_SZ_256];
        size_t len = strlen(slugs[i]) + 1;
        memcpy(tmp, slugs[i], len);
        int j = i - 1;
        while (j >= 0 && strcmp(slugs[j], tmp) > 0) {
            /* memmove, not snprintf: both sides are rows of ONE array, and a
             * `restrict`-qualified copy the compiler cannot prove disjoint is
             * an error here (-Werror=restrict), not a warning. */
            memmove(slugs[j + 1], slugs[j], strlen(slugs[j]) + 1);
            j--;
        }
        memcpy(slugs[j + 1], tmp, len);
    }
    size_t off = 0;
    out[0] = '\0';
    for (int i = 0; i < count; i++) {
        int wrote = snprintf(out + off, out_sz - off, "%s%s", i ? "-" : "", slugs[i]);
        if (wrote < 0 || off + (size_t)wrote >= out_sz ||
            off + (size_t)wrote > (size_t)HYP_ADDR_SLUG_MAX) {
            (void)snprintf(err, errlen,
                           "the derived workspace name is longer than a slug allows (%d); "
                           "name the workspace explicitly",
                           HYP_ADDR_SLUG_MAX);
            return -1;
        }
        off += (size_t)wrote;
    }
    return 0;
}

int hyp_onboard_resolve(const char *repo_dir, hyp_onboard_resolved_t *out, char *err,
                        size_t errlen) {
    if (!repo_dir || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char canon[HYP_PATH_MAX];
    if (!hyp_canonical_path(repo_dir, canon, sizeof(canon)) || !hyp_is_dir(canon)) {
        (void)snprintf(err, errlen, "'%s' is not a directory", repo_dir);
        return -1;
    }
    /* These three never depend on configuration: the zero-config default is
     * local nano on CPU with no shared feeds, and a TOML can widen the
     * workspace but cannot switch on spending or a device nobody verified. */
    (void)snprintf(out->encoder, sizeof(out->encoder), "local-nano");
    (void)snprintf(out->device, sizeof(out->device), "cpu");
    out->shared_feeds = false;

    hyp_onboard_plan_t plan;
    char rerr[HYP_SZ_512];
    rerr[0] = '\0';
    int rc = hyp_onboard_toml_read(canon, &plan, rerr, sizeof(rerr));
    if (rc < 0) {
        (void)snprintf(err, errlen, "%s", rerr);
        return -1;
    }
    if (rc == 0) {
        /* NO TOML IS THE MAIN PATH, never an error: this repo, a workspace of
         * one, role member. Same code path as C1's workspace == NULL — the
         * workspace id of a workspace of one is the repo's own slug. */
        if (!onboard_repo_slug(canon, out->workspace, sizeof(out->workspace))) {
            (void)snprintf(err, errlen, "no project slug can be derived from '%s'", canon);
            return -1;
        }
        out->workspace_of_one = true;
        (void)snprintf(out->source, sizeof(out->source), "defaults");
        out->repo_count = 1;
        (void)snprintf(out->repos[0].path, sizeof(out->repos[0].path), "%s", canon);
        (void)snprintf(out->repos[0].role, sizeof(out->repos[0].role), "%s",
                       HYP_ONBOARD_ROLE_MEMBER);
        return 0;
    }

    (void)snprintf(out->source, sizeof(out->source), "toml");
    if (plan.repo_count == 0) {
        /* A TOML that names the workspace but lists no repos still sits in
         * one: the directory that holds it, as a member. */
        plan.repo_count = 1;
        (void)snprintf(plan.repos[0].path, sizeof(plan.repos[0].path), "%s", canon);
        (void)snprintf(plan.repos[0].role, sizeof(plan.repos[0].role), "%s",
                       HYP_ONBOARD_ROLE_MEMBER);
    }
    if (onboard_check_collisions(plan.repos, plan.repo_count, err, errlen) != 0) {
        return -1;
    }
    out->repo_count = plan.repo_count;
    for (int i = 0; i < plan.repo_count; i++) {
        out->repos[i] = plan.repos[i];
    }
    out->workspace_of_one = plan.repo_count == 1;
    if (plan.name[0]) {
        (void)snprintf(out->workspace, sizeof(out->workspace), "%s", plan.name);
    } else if (onboard_name_from_members(out->repos, out->repo_count, out->workspace,
                                         sizeof(out->workspace), err, errlen) != 0) {
        return -1;
    }
    return 0;
}

/* ── B2 · The flow ──────────────────────────────────────────────── */

/* The four questions. A row here is the ONLY way to ask; everything else is
 * shown. The table's size is asserted by a test, so a fifth question has to
 * argue with the suite, not slip past it. */
static const struct {
    const char *key;
    const char *prompt;
} ONBOARD_QUESTIONS[HYP_ONBOARD_Q_COUNT] = {
    [HYP_ONBOARD_Q_REPOS] = {"repos", "Which other repositories belong in this workspace? (paths, "
                                      "comma-separated; empty = just this one)"},
    [HYP_ONBOARD_Q_REFERENCE] = {"reference",
                                 "Which of them are read-only reference material? (paths, "
                                 "comma-separated; empty = none)"},
    [HYP_ONBOARD_Q_SPEND] = {"spend",
                             "May the escalation lane spend money against a hosted provider? "
                             "(yes/no; empty = no)"},
    [HYP_ONBOARD_Q_NAME] = {"name", "What is this workspace called? (empty = the derived name)"},
};

const char *hyp_onboard_question_key(hyp_onboard_q_t q) {
    int i = (int)q;
    return (i >= 0 && i < (int)HYP_ONBOARD_Q_COUNT) ? ONBOARD_QUESTIONS[i].key : NULL;
}

const char *hyp_onboard_question_prompt(hyp_onboard_q_t q) {
    int i = (int)q;
    return (i >= 0 && i < (int)HYP_ONBOARD_Q_COUNT) ? ONBOARD_QUESTIONS[i].prompt : NULL;
}

/* Split a comma-separated list, canonicalise each entry, call `fn`. */
static int onboard_each_path(const char *list, char *err, size_t errlen,
                             int (*fn)(const char *canon, void *ctx), void *ctx) {
    if (!list || !list[0]) {
        return 0;
    }
    char buf[HYP_SZ_4K];
    (void)snprintf(buf, sizeof(buf), "%s", list);
    char *tok = buf;
    while (tok) {
        char *comma = strchr(tok, ',');
        if (comma) {
            *comma = '\0';
        }
        while (*tok == ' ') {
            tok++;
        }
        size_t n = strlen(tok);
        while (n > 0 && tok[n - 1] == ' ') {
            tok[--n] = '\0';
        }
        if (tok[0]) {
            char canon[HYP_PATH_MAX];
            if (!hyp_canonical_path(tok, canon, sizeof(canon)) || !hyp_is_dir(canon)) {
                (void)snprintf(err, errlen, "'%s' is not a directory", tok);
                return -1;
            }
            if (fn(canon, ctx) != 0) {
                return -1;
            }
        }
        tok = comma ? comma + 1 : NULL;
    }
    return 0;
}

typedef struct {
    hyp_onboard_plan_t *plan;
    char *err;
    size_t errlen;
} onboard_path_ctx_t;

static int onboard_add_repo(const char *canon, void *opaque) {
    onboard_path_ctx_t *ctx = opaque;
    hyp_onboard_plan_t *plan = ctx->plan;
    for (int i = 0; i < plan->repo_count; i++) {
        if (strcmp(plan->repos[i].path, canon) == 0) {
            return 0; /* already in — repeating an answer is not an error */
        }
    }
    if (plan->repo_count >= HYP_ONBOARD_MAX_REPOS) {
        (void)snprintf(ctx->err, ctx->errlen, "more than %d repositories", HYP_ONBOARD_MAX_REPOS);
        return -1;
    }
    hyp_onboard_repo_t *r = &plan->repos[plan->repo_count++];
    (void)snprintf(r->path, sizeof(r->path), "%s", canon);
    /* Repos the human says BELONG TOGETHER are members; Q2 narrows. */
    (void)snprintf(r->role, sizeof(r->role), "%s", HYP_ONBOARD_ROLE_MEMBER);
    return 0;
}

static int onboard_mark_reference(const char *canon, void *opaque) {
    onboard_path_ctx_t *ctx = opaque;
    hyp_onboard_plan_t *plan = ctx->plan;
    for (int i = 0; i < plan->repo_count; i++) {
        if (strcmp(plan->repos[i].path, canon) == 0) {
            (void)snprintf(plan->repos[i].role, sizeof(plan->repos[i].role), "%s",
                           HYP_ONBOARD_ROLE_REFERENCE);
            return 0;
        }
    }
    (void)snprintf(ctx->err, ctx->errlen,
                   "'%s' was named as reference but is not in this workspace — answer the "
                   "repos question first",
                   canon);
    return -1;
}

/* An env-var NAME: [A-Za-z_][A-Za-z0-9_]*. Anything else — and anything that
 * smells like a value — is refused, because config stores names only. */
static bool onboard_env_name_ok(const char *s) {
    if (!s || !s[0]) {
        return false;
    }
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool onboard_yes_no(const char *s, bool dflt, bool *out) {
    if (!s || !s[0]) {
        *out = dflt;
        return true;
    }
    if (strcmp(s, "yes") == 0 || strcmp(s, "y") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "no") == 0 || strcmp(s, "n") == 0) {
        *out = false;
        return true;
    }
    return false;
}

int hyp_onboard_flow(const char *repo_dir, const hyp_onboard_probe_t *probe,
                     const hyp_onboard_answers_t *answers, hyp_onboard_plan_t *out_plan, char *err,
                     size_t errlen) {
    if (!repo_dir || !out_plan) {
        return -1;
    }
    static const hyp_onboard_answers_t none = {0};
    const hyp_onboard_answers_t *a = answers ? answers : &none;
    memset(out_plan, 0, sizeof(*out_plan));

    /* THE PROPOSAL IS THE RESOLVER'S ANSWER — the flow starts from what would
     * happen with nothing typed, so accepting every default is exactly the
     * zero-config outcome plus a recorded name. */
    hyp_onboard_resolved_t base;
    if (hyp_onboard_resolve(repo_dir, &base, err, errlen) != 0) {
        return -1;
    }
    (void)snprintf(out_plan->name, sizeof(out_plan->name), "%s", base.workspace);
    out_plan->repo_count = base.repo_count;
    for (int i = 0; i < base.repo_count; i++) {
        out_plan->repos[i] = base.repos[i];
    }
    /* Device is PROPOSED from evidence: gpu only when the probe RAN the check
     * on a GPU encoder and it passed. No probe, no gpu. */
    bool gpu_verified = probe && probe->gpu == HYP_ONBOARD_GPU_VERIFIED && probe->gpu_check_ran;
    (void)snprintf(out_plan->device, sizeof(out_plan->device), "%s", gpu_verified ? "gpu" : "cpu");
    out_plan->spend = false;
    if (probe && probe->key_env_named) {
        (void)snprintf(out_plan->key_env, sizeof(out_plan->key_env), "%s", probe->key_env_name);
    }
    out_plan->write_toml = true;

    /* Q1 · which repos belong together. */
    onboard_path_ctx_t ctx = {.plan = out_plan, .err = err, .errlen = errlen};
    if (onboard_each_path(a->repos, err, errlen, onboard_add_repo, &ctx) != 0) {
        return -1;
    }
    /* Q2 · which of them are reference. */
    if (onboard_each_path(a->reference, err, errlen, onboard_mark_reference, &ctx) != 0) {
        return -1;
    }
    /* Q3 · whether to spend money. Anything but yes/no is refused — this one
     * decides what leaves the machine. */
    if (!onboard_yes_no(a->spend, false, &out_plan->spend)) {
        (void)snprintf(err, errlen, "spend must be 'yes' or 'no' (got '%s')", a->spend);
        return -1;
    }
    /* Q4 · what to call it. Unanswered: recompute from the FINAL member set,
     * deterministically, so adding a repo in Q1 cannot leave a first-repo
     * name behind. */
    if (a->name && a->name[0]) {
        if (strlen(a->name) > (size_t)HYP_ADDR_SLUG_MAX || !hyp_validate_project_name(a->name)) {
            (void)snprintf(err, errlen,
                           "'%s' is not a workspace name the address scheme accepts "
                           "([A-Za-z0-9._-])",
                           a->name);
            return -1;
        }
        (void)snprintf(out_plan->name, sizeof(out_plan->name), "%s", a->name);
    } else if (out_plan->repo_count > 1 &&
               onboard_name_from_members(out_plan->repos, out_plan->repo_count, out_plan->name,
                                         sizeof(out_plan->name), err, errlen) != 0) {
        return -1;
    }

    /* Edits to SHOWN values. Not questions; still validated, still fail
     * closed. */
    if (a->device && a->device[0]) {
        if (strcmp(a->device, "cpu") == 0) {
            (void)snprintf(out_plan->device, sizeof(out_plan->device), "cpu");
        } else if (strcmp(a->device, "gpu") == 0) {
            if (!gpu_verified) {
                /* An edit cannot assert hardware. The probe's evidence is the
                 * only thing that can say gpu, and it did not. */
                (void)snprintf(err, errlen,
                               "device=gpu refused: the probe did not VERIFY a GPU on this "
                               "build (state: %s) — %s",
                               probe ? hyp_onboard_gpu_str(probe->gpu) : "no probe was run",
                               probe && probe->gpu_evidence[0] ? probe->gpu_evidence
                                                               : "run `hyponoia onboard` again");
                return -1;
            }
            (void)snprintf(out_plan->device, sizeof(out_plan->device), "gpu");
        } else {
            (void)snprintf(err, errlen, "device must be cpu or gpu (got '%s')", a->device);
            return -1;
        }
    }
    if (a->key_env && a->key_env[0]) {
        if (!onboard_env_name_ok(a->key_env)) {
            (void)snprintf(err, errlen,
                           "'%s' is not an environment-variable NAME — config stores the "
                           "name of a variable, never a key",
                           a->key_env);
            return -1;
        }
        (void)snprintf(out_plan->key_env, sizeof(out_plan->key_env), "%s", a->key_env);
    }
    if (a->write_toml && a->write_toml[0]) {
        if (!onboard_yes_no(a->write_toml, true, &out_plan->write_toml)) {
            (void)snprintf(err, errlen, "write_toml must be 'yes' or 'no' (got '%s')",
                           a->write_toml);
            return -1;
        }
    }

    /* The final member set is checked the same way the resolver checks a
     * TOML: a collision refused here is one that could never be recorded. */
    if (onboard_check_collisions(out_plan->repos, out_plan->repo_count, err, errlen) != 0) {
        return -1;
    }
    return 0;
}

/* ── The command ────────────────────────────────────────────────── */

typedef struct {
    char repos[HYP_SZ_4K];
    char reference[HYP_SZ_4K];
    char spend[HYP_SZ_16];
    char name[HYP_SZ_256];
    char device[HYP_SZ_16];
    char key_env[HYP_SZ_128];
    char write_toml[HYP_SZ_16];
} onboard_answer_store_t;

/* key=value lines. Unknown keys are refused — a typo'd answer silently
 * accepting a default would be the interrogation's quiet cousin. */
static int onboard_answers_load(const char *path, onboard_answer_store_t *st,
                                hyp_onboard_answers_t *a) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        (void)fprintf(stderr, "onboard: cannot read answers file '%s'\n", path);
        return -1;
    }
    char line[ONBOARD_ANSWER_LINE];
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            (void)fprintf(stderr, "onboard: answers line without '=': %s\n", line);
            rc = -1;
            break;
        }
        *eq = '\0';
        const char *val = eq + 1;
        if (strcmp(line, "repos") == 0) {
            (void)snprintf(st->repos, sizeof(st->repos), "%s", val);
            a->repos = st->repos;
        } else if (strcmp(line, "reference") == 0) {
            (void)snprintf(st->reference, sizeof(st->reference), "%s", val);
            a->reference = st->reference;
        } else if (strcmp(line, "spend") == 0) {
            (void)snprintf(st->spend, sizeof(st->spend), "%s", val);
            a->spend = st->spend;
        } else if (strcmp(line, "name") == 0) {
            (void)snprintf(st->name, sizeof(st->name), "%s", val);
            a->name = st->name;
        } else if (strcmp(line, "device") == 0) {
            (void)snprintf(st->device, sizeof(st->device), "%s", val);
            a->device = st->device;
        } else if (strcmp(line, "key_env") == 0) {
            (void)snprintf(st->key_env, sizeof(st->key_env), "%s", val);
            a->key_env = st->key_env;
        } else if (strcmp(line, "write_toml") == 0) {
            (void)snprintf(st->write_toml, sizeof(st->write_toml), "%s", val);
            a->write_toml = st->write_toml;
        } else {
            (void)fprintf(stderr, "onboard: unknown answer key '%s'\n", line);
            rc = -1;
            break;
        }
    }
    (void)fclose(f);
    return rc;
}

static void onboard_print_probe(const hyp_onboard_probe_t *p) {
    printf("probe:\n");
    printf("  gpu           %s — %s\n", hyp_onboard_gpu_str(p->gpu), p->gpu_evidence);
    if (p->key_env_named) {
        printf("  key variable  $%s is %s (the name comes from config; the value was not "
               "read)\n",
               p->key_env_name, p->key_env_set ? "SET" : "NOT set");
    } else {
        printf("  key variable  none configured — the local lane needs none\n");
    }
    if (p->corpus_walked) {
        printf("  corpus        %d files, %lld bytes (walked in %.0f ms)\n", p->files,
               (long long)p->bytes, p->discover_ms);
    } else {
        printf("  corpus        the walk did not complete\n");
    }
    if (p->spans_known) {
        printf("  spans         %lld (%s)\n", (long long)p->spans, p->spans_method);
    } else {
        printf("  spans         %s\n", p->spans_method);
    }
    if (p->embed_estimate_known) {
        printf("  first index   ~%.0f s for the embed pass — %s\n", p->embed_seconds,
               p->cost_method);
    } else {
        printf("  first index   %s\n", p->cost_method);
    }
}

static void onboard_print_plan(const hyp_onboard_plan_t *plan) {
    printf("plan:\n");
    printf("  workspace     %s\n", plan->name);
    for (int i = 0; i < plan->repo_count; i++) {
        printf("  repo          %s (%s)\n", plan->repos[i].path, plan->repos[i].role);
    }
    printf("  device        %s\n", plan->device);
    printf("  spend         %s\n", plan->spend ? "yes" : "no");
    if (plan->key_env[0]) {
        printf("  key variable  $%s (a NAME; the key itself stays in your environment)\n",
               plan->key_env);
    }
    printf("  record        %s\n",
           plan->write_toml ? HYP_ONBOARD_TOML_NAME : "none (nothing will be written)");
}

/* One interactive prompt. Empty input accepts the proposal. */
static const char *onboard_ask_tty(hyp_onboard_q_t q, char *buf, size_t buf_sz) {
    printf("%s\n> ", hyp_onboard_question_prompt(q));
    (void)fflush(stdout);
    if (!fgets(buf, (int)buf_sz, stdin)) {
        return NULL;
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    return buf[0] ? buf : NULL;
}

int hyp_cmd_onboard(int argc, char **argv) {
    const char *dir = ".";
    const char *answers_path = NULL;
    bool json = false;
    bool yes = false;
    bool no_toml = false;
    bool propose_only = false;
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--json") == 0) {
            json = true;
        } else if (strcmp(arg, "--yes") == 0 || strcmp(arg, "-y") == 0) {
            yes = true;
        } else if (strcmp(arg, "--no-toml") == 0) {
            no_toml = true;
        } else if (strcmp(arg, "--propose-only") == 0) {
            propose_only = true;
        } else if (strcmp(arg, "--answers") == 0 && i + 1 < argc) {
            answers_path = argv[++i];
        } else if (arg[0] == '-') {
            (void)fprintf(stderr,
                          "onboard: unknown option '%s'\nusage: hyponoia onboard [dir] [--json] "
                          "[--answers <file>] [--yes] [--no-toml] [--propose-only]\n",
                          arg);
            return 2;
        } else {
            dir = arg;
        }
    }

    hyp_onboard_probe_opts_t popts;
    memset(&popts, 0, sizeof(popts));
    popts.repo_path = dir;
    hyp_onboard_probe_t probe;
    if (hyp_onboard_probe(&popts, &probe) != 0) {
        (void)fprintf(stderr, "onboard: '%s' is not a directory\n", dir);
        return 1;
    }

    onboard_answer_store_t store;
    memset(&store, 0, sizeof(store));
    hyp_onboard_answers_t answers;
    memset(&answers, 0, sizeof(answers));
    if (answers_path && onboard_answers_load(answers_path, &store, &answers) != 0) {
        return 2;
    }

    bool interactive = !answers_path && !yes && !propose_only && isatty(0) != 0;
    if (!json) {
        onboard_print_probe(&probe);
    }
    if (interactive) {
        /* The four questions — and nothing else. The table IS the list. */
        static char bufs[HYP_ONBOARD_Q_COUNT][HYP_SZ_4K];
        const char *got = onboard_ask_tty(HYP_ONBOARD_Q_REPOS, bufs[0], sizeof(bufs[0]));
        answers.repos = got;
        got = onboard_ask_tty(HYP_ONBOARD_Q_REFERENCE, bufs[1], sizeof(bufs[1]));
        answers.reference = got;
        got = onboard_ask_tty(HYP_ONBOARD_Q_SPEND, bufs[2], sizeof(bufs[2]));
        answers.spend = got;
        got = onboard_ask_tty(HYP_ONBOARD_Q_NAME, bufs[3], sizeof(bufs[3]));
        answers.name = got;
    }
    if (no_toml) {
        answers.write_toml = "no";
    }

    hyp_onboard_plan_t plan;
    char err[HYP_SZ_512];
    err[0] = '\0';
    if (hyp_onboard_flow(dir, &probe, &answers, &plan, err, sizeof(err)) != 0) {
        (void)fprintf(stderr, "onboard: %s\n", err);
        return 1;
    }

    if (json) {
        char *pj = hyp_onboard_probe_json(&probe);
        printf("{\"probe\":%s,\"plan\":{\"name\":\"%s\",\"device\":\"%s\",\"spend\":%s,"
               "\"repos\":%d,\"write_toml\":%s}}\n",
               pj ? pj : "null", plan.name, plan.device, plan.spend ? "true" : "false",
               plan.repo_count, plan.write_toml ? "true" : "false");
        free(pj);
    } else {
        onboard_print_plan(&plan);
    }

    if (propose_only) {
        return 0;
    }
    if (!interactive && !yes && !answers_path) {
        /* No human, no answers: propose and stop. A confident refusal to
         * write beats a prompt nobody is there to answer. */
        printf("nothing written — re-run with --yes, or --answers <file>, to confirm\n");
        return 0;
    }
    if (interactive) {
        char confirm[HYP_SZ_16];
        printf("proceed? [Y/n] ");
        (void)fflush(stdout);
        if (fgets(confirm, sizeof(confirm), stdin) && (confirm[0] == 'n' || confirm[0] == 'N')) {
            printf("nothing written\n");
            return 0;
        }
    }

    if (plan.write_toml) {
        if (hyp_onboard_toml_write(dir, &plan, err, sizeof(err)) != 0) {
            (void)fprintf(stderr, "onboard: %s\n", err);
            return 1;
        }
        printf("recorded %s/%s\n", dir, HYP_ONBOARD_TOML_NAME);
    }

    /* Next steps are PRINTED, not executed: onboarding decides, it does not
     * start a forty-minute pass under someone's enter key. */
    printf("next:\n");
    printf("  hyponoia cli index_repository --repo_path \"%s\"\n", plan.repos[0].path);
    printf("  hyponoia embed --project %s --device %s\n", plan.name, plan.device);
    if (plan.spend) {
        if (plan.key_env[0]) {
            printf("  hyponoia config set %s %s   # the NAME of the variable, never the key\n",
                   HYP_CONFIG_ASK_ESC_KEY_ENV, plan.key_env);
        } else {
            printf("  hyponoia config set %s <ENV_VAR_NAME>   # spending is on; name the "
                   "variable that holds the key\n",
                   HYP_CONFIG_ASK_ESC_KEY_ENV);
        }
    }
    return 0;
}
