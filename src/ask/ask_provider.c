/*
 * ask_provider.c — the table, and the two calls that use it.
 *
 * See ask_provider.h for why the table is the interesting part.
 */
#include "ask/ask_provider.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "ask/ask_encoder.h"
#include "semantic/ask_embed.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/secure_random.h"
#include "yyjson/yyjson.h"

/* ── The table ─────────────────────────────────────────────────────
 *
 * Every field here is a fact about a provider's wire format, checked against
 * its documentation AND against a live call where the row is marked
 * implemented. Voyage's row is the one §2.7 and §2.9 measured through.
 */
static const hyp_ask_provider_t PROVIDERS[] = {
    {
        .id = HYP_ASK_PROVIDER_VOYAGE,
        .name = "voyage",
        .endpoint = "https://api.voyageai.com/v1/embeddings",
        .asym_param = "input_type",
        .asym_document = "document",
        .asym_query = "query",
        .input_key = "input",
        .model_key = "model",
        .native_dim = 1024,
        .dim_param = "output_dimension",
        /* RE-EMBEDS. §2.7 ran voyage-code-3 at 1024 and 2048 as two separate
         * document passes for exactly this reason. */
        .dim_mode = HYP_ASK_DIM_REEMBEDS,
        .max_rows_per_request = 1000,
        .max_tokens_per_request = 120000,
        .implemented = true,
    },
    {
        .id = HYP_ASK_PROVIDER_JINA,
        .name = "jina",
        .endpoint = "https://api.jina.ai/v1/embeddings",
        /* Jina's asymmetry is a LoRA adapter selected by `task`, not a prompt
         * prefix. §2.6 measured it worth +0.077 RR (+31%, p = 0.012) over
         * sending no task at all — and measured our own instruct prefix HURTING
         * it by 14.3%. Both halves of that are why this field exists. */
        .asym_param = "task",
        .asym_document = "retrieval.passage",
        .asym_query = "retrieval.query",
        .input_key = "input",
        .model_key = "model",
        .native_dim = 1024,
        .dim_param = "dimensions",
        /* TRUNCATES — Matryoshka. §2.6 measured the cost of cutting to 512 at
         * -10.5% RR with a CI excluding zero, so "free" means free of a second
         * pass, not free of quality. */
        .dim_mode = HYP_ASK_DIM_TRUNCATES,
        .max_rows_per_request = 2048,
        .max_tokens_per_request = 8192,
        .implemented = true,
    },
    {
        .id = HYP_ASK_PROVIDER_GEMINI,
        .name = "gemini",
        .endpoint = "https://generativelanguage.googleapis.com/v1beta/models",
        .asym_param = "task_type",
        .asym_document = "RETRIEVAL_DOCUMENT",
        .asym_query = "RETRIEVAL_QUERY",
        .input_key = "requests",
        .model_key = "model",
        .native_dim = 3072,
        .dim_param = "outputDimensionality",
        .dim_mode = HYP_ASK_DIM_TRUNCATES,
        .max_rows_per_request = 100,
        .max_tokens_per_request = 2048,
        /* DECLARED, NOT WIRED. Gemini nests each input in its own request
         * object and authenticates on a query parameter rather than a bearer
         * header, so it is a different request builder, not a different row.
         * Left explicit: a provider that refuses is safer than one that guesses
         * a body shape and returns vectors from a request it built wrong. */
        .implemented = false,
    },
};

const hyp_ask_provider_t *hyp_ask_provider_table(size_t *count) {
    if (count) {
        *count = sizeof(PROVIDERS) / sizeof(PROVIDERS[0]);
    }
    return PROVIDERS;
}

const hyp_ask_provider_t *hyp_ask_provider_by_name(const char *name) {
    if (!name || !name[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(PROVIDERS) / sizeof(PROVIDERS[0]); i++) {
        if (strcmp(PROVIDERS[i].name, name) == 0) {
            return &PROVIDERS[i];
        }
    }
    return NULL;
}

const hyp_ask_provider_t *hyp_ask_provider_by_id(hyp_ask_provider_id_t id) {
    for (size_t i = 0; i < sizeof(PROVIDERS) / sizeof(PROVIDERS[0]); i++) {
        if (PROVIDERS[i].id == id) {
            return &PROVIDERS[i];
        }
    }
    return NULL;
}

bool hyp_ask_provider_contract(const hyp_ask_provider_t *p, char *buf, size_t buflen) {
    if (!p || !buf || buflen == 0) {
        return false;
    }
    int n = snprintf(buf, buflen, "%s/%s=%s|%s", p->name, p->asym_param, p->asym_document,
                     p->asym_query);
    return n > 0 && (size_t)n < buflen;
}

const char *hyp_ask_provider_key(const char *key_env, char *err, size_t errlen) {
    if (err && errlen) {
        err[0] = '\0';
    }
    if (!key_env || !key_env[0]) {
        snprintf(err, errlen,
                 "no key variable configured: set ask.escalation.key_env to the NAME of an "
                 "environment variable holding the API key");
        return NULL;
    }
    const char *v = getenv(key_env);
    if (!v || !v[0]) {
        /* Names the VARIABLE, never a value, and never hints at a length. */
        snprintf(err, errlen,
                 "environment variable %s is not set (or is empty) — the escalation lane reads "
                 "the key from it at the moment of use and never stores it",
                 key_env);
        return NULL;
    }
    return v;
}

/* ── Request plumbing ──────────────────────────────────────────────
 *
 * curl through hyp_exec_no_shell, the shape model_fetch.c already uses. Two
 * files are written beside each other in the cache, both 0600:
 *
 *   the CONFIG file holds the Authorization header and the URL, because argv is
 *   world-readable in /proc and a key in a `ps` listing is a leaked key;
 *   the BODY file holds the JSON, because a request can be megabytes and argv
 *   has a length limit that a large batch would silently hit.
 *
 * Both are unlinked before the function returns, on every path.
 */
typedef struct {
    char cfg[1024];
    char body[1024];
    char resp[1024];
} ap_paths_t;

static bool ap_tempdir(char *buf, size_t buflen) {
    const char *base = hyp_resolve_cache_dir();
    if (!base) {
        base = hyp_tmpdir();
    }
    if (!base) {
        return false;
    }
    int n = snprintf(buf, buflen, "%s/escalation", base);
    if (n <= 0 || (size_t)n >= buflen) {
        return false;
    }
    return hyp_mkdir_p(buf, 0700);
}

static bool ap_write_private(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    /* Narrow the mode BEFORE the secret is written, not after: a reader racing
     * between the open and the chmod would otherwise win. fchmod on the open
     * descriptor rather than chmod on the path, so the narrowing cannot be
     * redirected by a symlink swapped in underneath. */
    if (fchmod(hyp_fileno(f), 0600) != 0) {
        (void)fclose(f);
        (void)remove(path);
        return false;
    }
    size_t wrote = len ? fwrite(data, 1, len, f) : 0;
    bool ok = (wrote == len) && (fflush(f) == 0);
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)remove(path);
    }
    return ok;
}

static char *ap_read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    size_t cap = 1 << 16;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    for (;;) {
        if (len + 4096 > cap) {
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                (void)fclose(f);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) {
            break;
        }
    }
    (void)fclose(f);
    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

/* Build the JSON body. yyjson's mutable API, so the escaping is the vendored
 * library's problem and not a hand-rolled quoting bug in a code path that
 * carries user source text. */
static char *ap_build_body(const hyp_ask_provider_t *p, const char *model,
                           const char *const *texts, int count, int dim, const char *asym_value,
                           size_t *out_len) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, p->model_key, model);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_str(doc, arr, texts[i] ? texts[i] : "");
    }
    yyjson_mut_obj_add_val(doc, root, p->input_key, arr);
    if (asym_value && asym_value[0]) {
        yyjson_mut_obj_add_str(doc, root, p->asym_param, asym_value);
    }
    if (dim > 0 && p->dim_param) {
        yyjson_mut_obj_add_int(doc, root, p->dim_param, dim);
    }
    char *json = yyjson_mut_write(doc, 0, out_len);
    yyjson_mut_doc_free(doc);
    return json;
}

/* Pull `expected` rows of `dim` floats out of the response, IN THE ORDER THE
 * PROVIDER LABELLED THEM. Voyage and Jina both return an `index` per row and
 * both are documented as possibly reordering; trusting array position instead
 * would attach every citation in the lane to the wrong declaration, and nothing
 * downstream could detect it. */
static int ap_parse(const char *json, size_t len, int expected, int dim, float *out, char *err,
                    size_t errlen) {
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (!doc) {
        snprintf(err, errlen, "provider returned a body that is not JSON");
        return -1;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *data = yyjson_obj_get(root, "data");
    if (!data || !yyjson_is_arr(data)) {
        /* An error body is the common case here — surface the provider's own
         * message rather than "malformed", which sends the reader to the wrong
         * place. */
        yyjson_val *detail = yyjson_obj_get(root, "detail");
        yyjson_val *e = yyjson_obj_get(root, "error");
        const char *msg = NULL;
        if (detail && yyjson_is_str(detail)) {
            msg = yyjson_get_str(detail);
        } else if (e && yyjson_is_obj(e)) {
            yyjson_val *m = yyjson_obj_get(e, "message");
            msg = (m && yyjson_is_str(m)) ? yyjson_get_str(m) : NULL;
        } else if (e && yyjson_is_str(e)) {
            msg = yyjson_get_str(e);
        }
        snprintf(err, errlen, "provider refused the request: %s", msg ? msg : "no data array");
        yyjson_doc_free(doc);
        return -1;
    }
    size_t got = yyjson_arr_size(data);
    if (got != (size_t)expected) {
        snprintf(err, errlen, "asked for %d embeddings, got %zu — refusing a partial batch",
                 expected, got);
        yyjson_doc_free(doc);
        return -1;
    }
    bool *seen = (bool *)calloc((size_t)expected, sizeof(bool));
    if (!seen) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int rc = 0;
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *row = NULL;
    yyjson_arr_foreach(data, idx, max, row) {
        yyjson_val *iv = yyjson_obj_get(row, "index");
        int slot = (iv && yyjson_is_int(iv)) ? (int)yyjson_get_int(iv) : (int)idx;
        if (slot < 0 || slot >= expected || seen[slot]) {
            snprintf(err, errlen, "provider returned index %d twice or out of range", slot);
            rc = -1;
            break;
        }
        yyjson_val *emb = yyjson_obj_get(row, "embedding");
        if (!emb || !yyjson_is_arr(emb) || yyjson_arr_size(emb) != (size_t)dim) {
            snprintf(err, errlen, "row %d is %zu floats, expected %d", slot,
                     emb ? yyjson_arr_size(emb) : 0, dim);
            rc = -1;
            break;
        }
        float *dst = out + (size_t)slot * (size_t)dim;
        size_t j = 0;
        size_t jmax = 0;
        yyjson_val *fv = NULL;
        yyjson_arr_foreach(emb, j, jmax, fv) {
            dst[j] = (float)yyjson_get_num(fv);
        }
        seen[slot] = true;
    }
    free(seen);
    yyjson_doc_free(doc);
    if (rc != 0) {
        return rc;
    }
    /* Unit-normalise. The store refuses a row that is not normalised, and
     * providers differ on whether they pre-normalise — doing it here means one
     * rule holds for every lane rather than each provider's habit leaking into
     * the index. */
    for (int i = 0; i < expected; i++) {
        float *v = out + (size_t)i * (size_t)dim;
        double ss = 0.0;
        for (int j = 0; j < dim; j++) {
            ss += (double)v[j] * (double)v[j];
        }
        if (ss <= 0.0) {
            snprintf(err, errlen, "row %d came back as all zeros", i);
            return -1;
        }
        float inv = (float)(1.0 / sqrt(ss));
        for (int j = 0; j < dim; j++) {
            v[j] *= inv;
        }
    }
    return 0;
}

static int ap_post(const hyp_ask_provider_t *p, const char *key, const char *body, size_t body_len,
                   char **out_resp, size_t *out_len, char *err, size_t errlen) {
    char dir[900];
    if (!ap_tempdir(dir, sizeof(dir))) {
        snprintf(err, errlen, "could not create a private directory for the request");
        return -1;
    }
    /* Unpredictable names, not pid-derived ones: the config file holds a
     * bearer token for the instant it exists, and a guessable path in a shared
     * cache directory is the difference between 0600 protecting it and 0600
     * protecting a file an attacker already created. */
    unsigned char rnd[8];
    if (!hyp_secure_random(rnd, sizeof(rnd))) {
        snprintf(err, errlen, "no secure randomness available for a private request file");
        return -1;
    }
    char tag[17];
    for (size_t i = 0; i < sizeof(rnd); i++) {
        (void)snprintf(tag + i * 2, 3, "%02x", rnd[i]);
    }
    ap_paths_t pth;
    (void)snprintf(pth.cfg, sizeof(pth.cfg), "%s/req-%s.conf", dir, tag);
    (void)snprintf(pth.body, sizeof(pth.body), "%s/req-%s.json", dir, tag);
    (void)snprintf(pth.resp, sizeof(pth.resp), "%s/resp-%s.json", dir, tag);

    /* The header and the URL travel in the config file. curl's own
     * documentation recommends -K for exactly this reason. */
    char *cfg = NULL;
    int cfg_len = 0;
    {
        size_t need = strlen(key) + strlen(p->endpoint) + 256;
        cfg = (char *)malloc(need);
        if (!cfg) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        cfg_len = snprintf(cfg, need,
                           "url = \"%s\"\n"
                           "header = \"Authorization: Bearer %s\"\n"
                           "header = \"Content-Type: application/json\"\n",
                           p->endpoint, key);
    }
    bool ok = cfg_len > 0 && ap_write_private(pth.cfg, cfg, (size_t)cfg_len);
    /* Scrub before free: the key sat in this buffer, and a plain memset here is
     * exactly the store the optimiser is entitled to delete. */
    if (cfg) {
        hyp_secure_zero(cfg, (size_t)(cfg_len > 0 ? cfg_len : 0));
        free(cfg);
    }
    if (!ok) {
        snprintf(err, errlen, "could not write the request config privately");
        return -1;
    }
    if (!ap_write_private(pth.body, body, body_len)) {
        (void)remove(pth.cfg);
        snprintf(err, errlen, "could not write the request body");
        return -1;
    }

    const char *argv[] = {"curl",     "--silent", "--show-error", "--fail-with-body",
                          "--config", pth.cfg,    "--data-binary",
                          NULL, /* @body, filled below */
                          "--output", pth.resp,   "--max-time",    "300",
                          NULL};
    char data_arg[1024];
    (void)snprintf(data_arg, sizeof(data_arg), "@%s", pth.body);
    argv[7] = data_arg;

    int rc = hyp_exec_no_shell(argv);
    (void)remove(pth.cfg);
    (void)remove(pth.body);
    if (rc != 0) {
        /* --fail-with-body keeps the provider's error text, which is where the
         * useful sentence is (quota, rate limit, unknown model). */
        size_t rl = 0;
        char *r = ap_read_all(pth.resp, &rl);
        snprintf(err, errlen, "curl exited %d%s%s", rc, (r && rl) ? ": " : "",
                 (r && rl) ? r : "");
        free(r);
        (void)remove(pth.resp);
        return -1;
    }
    *out_resp = ap_read_all(pth.resp, out_len);
    (void)remove(pth.resp);
    if (!*out_resp) {
        snprintf(err, errlen, "could not read the provider's response");
        return -1;
    }
    return 0;
}

static int ap_embed(const hyp_ask_provider_t *p, const char *model, const char *key,
                    const char *const *texts, int count, int dim, const char *asym, float *out,
                    char *err, size_t errlen) {
    if (err && errlen) {
        err[0] = '\0';
    }
    if (!p || !model || !model[0] || !key || !texts || count <= 0 || !out) {
        snprintf(err, errlen, "bad arguments");
        return -1;
    }
    if (!p->implemented) {
        snprintf(err, errlen,
                 "provider '%s' is declared but not wired in this build — its request body is a "
                 "different shape, and guessing it would return vectors from a request built wrong",
                 p->name);
        return -1;
    }
    if (count > p->max_rows_per_request) {
        snprintf(err, errlen, "%d rows exceeds %s's per-request cap of %d — batch upstream",
                 count, p->name, p->max_rows_per_request);
        return -1;
    }
    int want = dim > 0 ? dim : p->native_dim;
    size_t body_len = 0;
    char *body = ap_build_body(p, model, texts, count, dim, asym, &body_len);
    if (!body) {
        snprintf(err, errlen, "could not build the request body");
        return -1;
    }
    char *resp = NULL;
    size_t resp_len = 0;
    int rc = ap_post(p, key, body, body_len, &resp, &resp_len, err, errlen);
    free(body);
    if (rc != 0) {
        return rc;
    }
    rc = ap_parse(resp, resp_len, count, want, out, err, errlen);
    free(resp);
    return rc;
}

int hyp_ask_provider_embed_documents(const hyp_ask_provider_t *p, const char *model,
                                     const char *key, const char *const *texts, int count, int dim,
                                     float *out, char *err, size_t errlen) {
    return ap_embed(p, model, key, texts, count, dim, p ? p->asym_document : NULL, out, err,
                    errlen);
}

int hyp_ask_provider_embed_query(const hyp_ask_provider_t *p, const char *model, const char *key,
                                 const char *text, int dim, float *out, char *err, size_t errlen) {
    const char *one[1] = {text};
    return ap_embed(p, model, key, one, 1, dim, p ? p->asym_query : NULL, out, err, errlen);
}

/* ── The encoder adapter ───────────────────────────────────────────
 *
 * The escalation index is built by the ordinary embed pass. That is deliberate:
 * batching, reuse-by-content-hash, truncation disclosure and provenance
 * stamping are all things the escalation lane needs exactly as much as the
 * local one, and a parallel implementation would drift from them.
 */
typedef struct {
    const hyp_ask_provider_t *p;
    char model[128];
    char model_id[HYP_ASK_MODEL_ID_MAX];
    char contract[128];
    const char *key; /* points into the environment; never freed, never logged */
} ap_encoder_t;

static const char *ape_model_id(void *self) {
    return ((ap_encoder_t *)self)->model_id;
}
static const char *ape_prefix_contract(void *self) {
    return ((ap_encoder_t *)self)->contract;
}
static const char *ape_device_note(void *self) {
    (void)self;
    /* Not a device at all, and saying so is the honest answer — "GPU" or "CPU"
     * would both be lies, and this string is what the index records about who
     * built it. */
    return "remote API (no local inference)";
}
static bool ape_device_is_gpu(void *self) {
    (void)self;
    return false;
}
static int ape_dim(void *self) {
    (void)self;
    return HYP_ASK_DIM;
}
static int ape_window(void *self) {
    /* The provider's per-request token cap is the effective window: a document
     * over it cannot be sent at all, which is a harder limit than truncation. */
    return ((ap_encoder_t *)self)->p->max_tokens_per_request;
}

static int ape_encode_documents(void *self, const char *const *texts, int count, float *out) {
    ap_encoder_t *e = (ap_encoder_t *)self;
    char err[512];
    err[0] = '\0';
    int rc = hyp_ask_provider_embed_documents(e->p, e->model, e->key, texts, count, HYP_ASK_DIM,
                                              out, err, sizeof(err));
    if (rc != 0 && err[0]) {
        hyp_log_error("ask.provider.encode_documents", "err", err);
    }
    return rc;
}

static int ape_encode_query(void *self, const char *text, const char *language_display,
                            float *out) {
    ap_encoder_t *e = (ap_encoder_t *)self;
    /* The provider's asymmetry is its input_type/task, not a language-bearing
     * prefix, so the display name plays no part. */
    (void)language_display;
    char err[512];
    err[0] = '\0';
    int rc = hyp_ask_provider_embed_query(e->p, e->model, e->key, text, HYP_ASK_DIM, out, err,
                                         sizeof(err));
    if (rc != 0 && err[0]) {
        hyp_log_error("ask.provider.encode_query", "err", err);
    }
    return rc;
}

static void ape_destroy(void *self) {
    free(self);
}

static const hyp_ask_encoder_vt_t AP_ENCODER_VT = {
    .model_id = ape_model_id,
    .prefix_contract = ape_prefix_contract,
    .device_note = ape_device_note,
    .device_is_gpu = ape_device_is_gpu,
    .dim = ape_dim,
    .window_tokens = ape_window,
    .encode_documents = ape_encode_documents,
    .encode_query = ape_encode_query,
    .destroy = ape_destroy,
};

struct hyp_ask_encoder *hyp_ask_provider_encoder_create(const char *provider_name,
                                                        const char *model, const char *key_env,
                                                        char *err, size_t errlen) {
    if (err && errlen) {
        err[0] = '\0';
    }
    const hyp_ask_provider_t *p = hyp_ask_provider_by_name(provider_name);
    if (!p) {
        snprintf(err, errlen,
                 "no escalation provider configured, or '%s' is not one this build knows — set "
                 "ask.escalation.provider",
                 provider_name ? provider_name : "");
        return NULL;
    }
    if (!p->implemented) {
        snprintf(err, errlen, "provider '%s' is declared but not wired in this build", p->name);
        return NULL;
    }
    if (!model || !model[0]) {
        snprintf(err, errlen, "no escalation model configured — set ask.escalation.model");
        return NULL;
    }
    const char *key = hyp_ask_provider_key(key_env, err, errlen);
    if (!key) {
        return NULL;
    }
    ap_encoder_t *e = (ap_encoder_t *)calloc(1, sizeof(*e));
    hyp_ask_encoder_t *enc = (hyp_ask_encoder_t *)calloc(1, sizeof(*enc));
    if (!e || !enc) {
        free(e);
        free(enc);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    e->p = p;
    e->key = key;
    (void)snprintf(e->model, sizeof(e->model), "%s", model);
    /* The provider is part of the identity: two providers could plausibly serve
     * the same model name and would not be the same vectors. */
    (void)snprintf(e->model_id, sizeof(e->model_id), "%s/%s", p->name, model);
    (void)hyp_ask_provider_contract(p, e->contract, sizeof(e->contract));
    enc->vt = &AP_ENCODER_VT;
    enc->self = e;
    return enc;
}
