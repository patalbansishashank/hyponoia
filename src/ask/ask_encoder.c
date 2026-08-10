/*
 * ask_encoder.c — forwarders for the encoder seam, the unit-norm check, and the
 * deterministic stub encoder that lets the rest of the `ask` build be tested
 * without an inference runtime.
 */

#include "ask/ask_encoder.h"
#include "foundation/sha256.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *hyp_ask_encoder_model_id(const hyp_ask_encoder_t *e) {
    return (e && e->vt && e->vt->model_id) ? e->vt->model_id(e->self) : NULL;
}

const char *hyp_ask_encoder_device_note(const hyp_ask_encoder_t *e) {
    if (e && e->vt && e->vt->device_note) {
        const char *n = e->vt->device_note(e->self);
        if (n && n[0]) {
            return n;
        }
    }
    /* An encoder that will not say which device it used is itself a finding.
     * The one thing this must never do is guess "CPU" or "GPU" — a wrong device
     * note is worse than an absent one, because it is believed. */
    return "UNKNOWN — this encoder does not report its device";
}

bool hyp_ask_encoder_device_is_gpu(const hyp_ask_encoder_t *e) {
    return (e && e->vt && e->vt->device_is_gpu) ? e->vt->device_is_gpu(e->self) : false;
}

const char *hyp_ask_device_pref_name(hyp_ask_device_pref_t p) {
    switch (p) {
    case HYP_ASK_DEVICE_GPU:
        return "gpu";
    case HYP_ASK_DEVICE_CPU:
        return "cpu";
    case HYP_ASK_DEVICE_AUTO:
    default:
        return "auto";
    }
}

int hyp_ask_encoder_dim(const hyp_ask_encoder_t *e) {
    return (e && e->vt && e->vt->dim) ? e->vt->dim(e->self) : 0;
}

int hyp_ask_encoder_window(const hyp_ask_encoder_t *e) {
    return (e && e->vt && e->vt->window_tokens) ? e->vt->window_tokens(e->self) : 0;
}

int hyp_ask_encoder_token_length(const hyp_ask_encoder_t *e, const char *text) {
    return (e && e->vt && e->vt->token_length) ? e->vt->token_length(e->self, text) : -1;
}

int hyp_ask_encoder_full_token_length(const hyp_ask_encoder_t *e, const char *text) {
    return (e && e->vt && e->vt->full_token_length) ? e->vt->full_token_length(e->self, text) : -1;
}

int hyp_ask_encode_documents(const hyp_ask_encoder_t *e, const char *const *texts, int count,
                             float *out) {
    if (!e || !e->vt || !e->vt->encode_documents) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (!texts || !out) {
        return -1;
    }
    return e->vt->encode_documents(e->self, texts, count, out);
}

int hyp_ask_encode_query(const hyp_ask_encoder_t *e, const char *text,
                         const char *language_display, float *out) {
    if (!e || !e->vt || !e->vt->encode_query || !text || !out) {
        return -1;
    }
    return e->vt->encode_query(e->self, text, language_display, out);
}

void hyp_ask_encoder_destroy(hyp_ask_encoder_t *e) {
    if (e && e->vt && e->vt->destroy) {
        e->vt->destroy(e->self);
    }
    free(e);
}

bool hyp_ask_vectors_are_unit(const float *vecs, int count, int dim, int *out_bad_row) {
    if (out_bad_row) {
        *out_bad_row = -1;
    }
    if (!vecs || dim <= 0) {
        return count == 0;
    }
    for (int r = 0; r < count; r++) {
        double sum = 0.0;
        const float *row = vecs + (size_t)r * (size_t)dim;
        for (int d = 0; d < dim; d++) {
            sum += (double)row[d] * (double)row[d];
        }
        double norm = sqrt(sum);
        if (fabs(norm - 1.0) > (double)HYP_ASK_NORM_TOLERANCE) {
            if (out_bad_row) {
                *out_bad_row = r;
            }
            return false;
        }
    }
    return true;
}

/* ── Deterministic stub encoder ─────────────────────────────────── */

typedef struct {
    int dim;
    int window;
    bool can_report_truncation;
} stub_state_t;

static const char *stub_model_id(void *self) {
    (void)self;
    return HYP_ASK_STUB_MODEL_ID;
}

static const char *stub_device_note(void *self) {
    (void)self;
    return HYP_ASK_STUB_DEVICE_NOTE;
}

static bool stub_device_is_gpu(void *self) {
    (void)self;
    return false;
}

static int stub_dim(void *self) {
    return ((const stub_state_t *)self)->dim;
}

static int stub_window(void *self) {
    return ((const stub_state_t *)self)->window;
}

/* Byte-based token estimate. Enough to exercise the grouping rule; it is not a
 * tokeniser and must never be presented as one. */
static int stub_estimate_tokens(const char *text) {
    if (!text) {
        return 0;
    }
    size_t bytes = strlen(text);
    size_t toks = (bytes + (HYP_ASK_STUB_BYTES_PER_TOKEN - 1)) / HYP_ASK_STUB_BYTES_PER_TOKEN;
    if (toks > (size_t)INT32_MAX) {
        return INT32_MAX;
    }
    return (int)toks;
}

static int stub_token_length(void *self, const char *text) {
    const stub_state_t *s = (const stub_state_t *)self;
    int t = stub_estimate_tokens(text);
    return t > s->window ? s->window : t;
}

static int stub_full_token_length(void *self, const char *text) {
    const stub_state_t *s = (const stub_state_t *)self;
    if (!s->can_report_truncation) {
        return -1; /* the UNKNOWN state, reachable on purpose */
    }
    return stub_estimate_tokens(text);
}

/* Expand a sha256 of the text into `dim` floats, then L2-normalise. Identical
 * text gives an identical vector, which is what makes the reuse-on-content-hash
 * path testable; unrelated text gives an unrelated vector, which is all the
 * storage layer needs. */
static void stub_embed_one(const char *text, int dim, float *out) {
    uint8_t seed[HYP_SHA256_DIGEST_LEN];
    hyp_sha256_ctx c;
    hyp_sha256_init(&c);
    hyp_sha256_update(&c, text ? text : "", text ? strlen(text) : 0);
    hyp_sha256_final(&c, seed);

    uint64_t state = 0;
    for (int i = 0; i < 8; i++) {
        state = (state << 8) | seed[i];
    }
    if (state == 0) {
        state = 0x9E3779B97F4A7C15ULL;
    }
    double sum = 0.0;
    for (int d = 0; d < dim; d++) {
        /* splitmix64 — deterministic, portable, no libc RNG state. */
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        /* Map to (-1, 1). */
        double v = ((double)(z >> 11) / 9007199254740992.0) * 2.0 - 1.0;
        out[d] = (float)v;
        sum += v * v;
    }
    double norm = sqrt(sum);
    if (norm < 1e-12) {
        /* Degenerate draw: fall back to a basis vector rather than emit a row
         * that would fail the unit-norm check downstream. */
        for (int d = 0; d < dim; d++) {
            out[d] = 0.0F;
        }
        out[0] = 1.0F;
        return;
    }
    for (int d = 0; d < dim; d++) {
        out[d] = (float)((double)out[d] / norm);
    }
}

static int stub_encode_documents(void *self, const char *const *texts, int count, float *out) {
    const stub_state_t *s = (const stub_state_t *)self;
    for (int i = 0; i < count; i++) {
        stub_embed_one(texts[i], s->dim, out + (size_t)i * (size_t)s->dim);
    }
    return 0;
}

static int stub_encode_query(void *self, const char *text, const char *language_display,
                             float *out) {
    const stub_state_t *s = (const stub_state_t *)self;
    /* The stub still models the asymmetry: the query side hashes a different
     * string from the document side. It does NOT reproduce the measured prefix
     * — that is track 5's constant and track 2's runtime. */
    size_t need = strlen("stub-query:") + (language_display ? strlen(language_display) : 0) + 2 +
                  strlen(text) + 1;
    char *buf = malloc(need);
    if (!buf) {
        return -1;
    }
    snprintf(buf, need, "stub-query:%s:%s", language_display ? language_display : "", text);
    stub_embed_one(buf, s->dim, out);
    free(buf);
    return 0;
}

static void stub_destroy(void *self) {
    free(self);
}

static const hyp_ask_encoder_vt_t STUB_VT = {
    .model_id = stub_model_id,
    .device_note = stub_device_note,
    .device_is_gpu = stub_device_is_gpu,
    .dim = stub_dim,
    .window_tokens = stub_window,
    .token_length = stub_token_length,
    .full_token_length = stub_full_token_length,
    .encode_documents = stub_encode_documents,
    .encode_query = stub_encode_query,
    .destroy = stub_destroy,
};

hyp_ask_encoder_t *hyp_ask_encoder_stub_create(int dim, int window_tokens,
                                               bool can_report_truncation) {
    if (dim <= 0) {
        dim = HYP_ASK_DIM_DEFAULT;
    }
    if (window_tokens <= 0) {
        window_tokens = 32768;
    }
    stub_state_t *st = calloc(1, sizeof(*st));
    hyp_ask_encoder_t *e = calloc(1, sizeof(*e));
    if (!st || !e) {
        free(st);
        free(e);
        return NULL;
    }
    st->dim = dim;
    st->window = window_tokens;
    st->can_report_truncation = can_report_truncation;
    e->vt = &STUB_VT;
    e->self = st;
    return e;
}
