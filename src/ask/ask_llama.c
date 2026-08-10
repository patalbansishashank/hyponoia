/*
 * ask_llama.c — Qwen3-Embedding-0.6B, in-process, behind the two `ask` seams.
 *
 * The design argument is in ask_llama.h. This file is the arithmetic, and the
 * arithmetic is the part that cost something: see the n_ctx note in
 * ask_batch.h before changing any sizing here.
 */

#include "ask/ask_llama.h"

#include "ask/ask_batch.h"
#include "cli/model_fetch.h"
#include "foundation/log.h"
#include "semantic/ask_embed.h"
#include "semantic/ask_lang.h"
#include "semantic/ask_prefix.h"

#include <stdio.h>
#include <string.h>

#if HYP_ASK_LLAMA

#include "llama.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

/* ── Configuration, all of it measured ──────────────────────────── */

/* Per-sequence context lengths the encoder will build a context at. A batch is
 * given the smallest bucket that holds its longest document.
 *
 * Buckets rather than an exact fit because a llama context's n_ctx is fixed at
 * creation, and the embed pass hands over length-sorted batches whose longest
 * member creeps upward one document at a time. Rebuilding the context per
 * batch would re-allocate the KV cache thousands of times; rebuilding it per
 * BUCKET does so at most four times per sort window. The top bucket is the
 * model's full window, which is what makes the truncation counter read the
 * same 2 rows on lld/ELF that the reference measured — a context sized from
 * free VRAM alone would land near 8,779 tokens and turn a 2-row disclosure
 * into a 30-row one that is an artifact of this machine's spare memory. */
static const int ASK_SEQ_BUCKETS[] = {512, 2048, 8192, HYP_ASK_MODEL_WINDOW};
enum { ASK_SEQ_BUCKET_COUNT = (int)(sizeof(ASK_SEQ_BUCKETS) / sizeof(ASK_SEQ_BUCKETS[0])) };

/* Threads for the CPU path. The prototype measured 1.581 docs/s at 16. */
enum { ASK_CPU_THREADS = 16 };

/* Offload everything when a GPU is in play: Qwen3-0.6B at Q8_0 is ~639 MB of
 * weights against a 16 GB card, and a partial offload pays the transfer on
 * every layer boundary. */
enum { ASK_NGL_ALL = 99 };

typedef struct {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;

    int n_embd;
    int model_window; /* what the weights support */
    int seq_len;      /* per-sequence limit of the LIVE context; 0 = none yet */
    int n_seq_max;    /* sequences the LIVE context can hold */

    bool on_gpu;
    double ceiling_mib; /* what a GPU plan is checked against; 0 on CPU */
    char device_note[192];
    char model_id[HYP_ASK_MODEL_ID_MAX];

    llama_token *toks;
    int toks_cap;
} ask_llama_t;

/* ── Logging: ggml is chatty, and its chatter is not ours ────────── */

static void ask_llama_log(enum ggml_log_level level, const char *text, void *user) {
    (void)user;
    if (!text || !text[0] || (text[0] == '\n' && !text[1])) {
        return;
    }
    /* GGML_LOG_LEVEL_CONT is 5 — HIGHER than ERROR (4) — because it means
     * "continuation of the previous line", not "more severe". A `level >=
     * ERROR` filter therefore promotes every progress dot of a 639 MB model
     * load to an error, which is what the first run of this backend did. */
    switch (level) {
    case GGML_LOG_LEVEL_ERROR:
        hyp_log_error("ask.llama.backend", "msg", text);
        break;
    case GGML_LOG_LEVEL_WARN:
        hyp_log_warn("ask.llama.backend", "msg", text);
        break;
    default:
        /* Buffer sizes, device names and the pooling type land here. Visible
         * at HYP_LOG_LEVEL=debug, which is where the VRAM accounting in the
         * run record came from. */
        hyp_log_debug("ask.llama.backend", "msg", text);
        break;
    }
}

static void ask_llama_backend_once(void) {
    static bool done = false;
    if (!done) {
        llama_log_set(ask_llama_log, NULL);
        llama_backend_init();
        done = true;
    }
}

/* ── Device selection: the request, then the outcome, never merged ── */

static bool ask_llama_gpu_present(char *name, size_t name_sz) {
    if (name && name_sz) {
        name[0] = '\0';
    }
    if (!llama_supports_gpu_offload()) {
        return false;
    }
    size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; i++) {
        ggml_backend_dev_t d = ggml_backend_dev_get(i);
        if (d && ggml_backend_dev_type(d) == GGML_BACKEND_DEVICE_TYPE_GPU) {
            const char *desc = ggml_backend_dev_description(d);
            if (name && name_sz) {
                (void)snprintf(name, name_sz, "%s", desc ? desc : "unnamed GPU");
            }
            return true;
        }
    }
    return false;
}

/* ── Context lifecycle ──────────────────────────────────────────── */

static int ask_bucket_for(int longest) {
    for (int i = 0; i < ASK_SEQ_BUCKET_COUNT; i++) {
        if (longest <= ASK_SEQ_BUCKETS[i]) {
            return ASK_SEQ_BUCKETS[i];
        }
    }
    return ASK_SEQ_BUCKETS[ASK_SEQ_BUCKET_COUNT - 1];
}

static void ask_llama_drop_ctx(ask_llama_t *s) {
    if (s->ctx) {
        llama_free(s->ctx);
        s->ctx = NULL;
    }
    s->seq_len = 0;
    s->n_seq_max = 0;
}

/* Build a context for `n_seq` sequences of at most `seq_len` tokens.
 *
 * REFUSES rather than attempts when the implied KV exceeds the ceiling. On a
 * machine where the compute GPU is also the display GPU — the common case for
 * the developers this tool targets — an over-allocation is not an OOM return,
 * it is the user losing their session. */
static bool ask_llama_make_ctx(ask_llama_t *s, int n_seq, int seq_len, char *err, size_t errlen) {
    hyp_ask_kv_plan_t plan;
    if (s->on_gpu) {
        if (!hyp_ask_kv_plan(n_seq, seq_len, s->ceiling_mib, &plan)) {
            (void)snprintf(err, errlen,
                           "REFUSED before allocating: n_seq_max=%d x seq_len=%d is n_ctx=%lld, "
                           "%.0f MiB of KV and %.0f MiB with the backend's fixed cost, over the "
                           "%.0f MiB ceiling taken from FREE VRAM",
                           n_seq, seq_len, (long long)plan.n_ctx, plan.kv_mib, plan.total_mib,
                           plan.ceiling_mib);
            return false;
        }
    } else {
        (void)hyp_ask_kv_plan(n_seq, seq_len, 1e12, &plan);
    }

    struct llama_context_params cp = llama_context_default_params();
    cp.embeddings = true;
    /* FORCED, not inherited. The official GGUF carries pooling_type and
     * llama.cpp resolves it to LAST without being asked — but the fallback
     * when the key is absent is POOLING_TYPE_NONE, which returns no pooled
     * output at all, and mean pooling (§2's expensive mistake) measures
     * 0.5384-0.8484 against correct. Asking is free; assuming is not. */
    cp.pooling_type = LLAMA_POOLING_TYPE_LAST;

    /* n_ctx is TOTAL KV capacity and llama.cpp DIVIDES it by n_seq_max.
     * n_ctx = n_batch * n_seq_max is the line that looks right and asked for
     * ~57 GB. This is the other one. */
    cp.n_ctx = (uint32_t)plan.n_ctx;
    cp.n_seq_max = (uint32_t)n_seq;

    /* n_batch is the tokens ONE decode may carry, and the grouping rule already
     * bounds that: a group never exceeds HYP_ASK_TOKEN_BUDGET padded slots
     * except for the single over-budget document that lands alone, which needs
     * seq_len. Sizing it at n_ctx instead — the obvious upper bound — makes
     * llama.cpp reserve host-pinned staging buffers for tokens no batch will
     * ever carry, and the Vulkan driver answers with
     * `Failed to allocate pinned memory` and silently falls back to unpinned
     * transfers. */
    int n_batch = seq_len > HYP_ASK_TOKEN_BUDGET ? seq_len : HYP_ASK_TOKEN_BUDGET;
    if ((int64_t)n_batch > plan.n_ctx) {
        n_batch = (int)plan.n_ctx;
    }
    cp.n_batch = (uint32_t)n_batch;

    /* The compute buffer is the OTHER allocation, it scales with n_ubatch, and
     * nothing in the KV plan can see it. Give it what is left of the ceiling
     * after the KV cache and the resident weights. */
    double compute_budget = s->on_gpu
                                ? s->ceiling_mib - plan.kv_mib - HYP_ASK_WEIGHTS_RESIDENT_MIB
                                : 1e9;
    cp.n_ubatch = (uint32_t)hyp_ask_ubatch_for(n_batch, compute_budget);
    cp.n_threads = ASK_CPU_THREADS;
    cp.n_threads_batch = ASK_CPU_THREADS;

    /* THE OUTGOING CONTEXT IS FREED FIRST. Allocating the new one while the
     * old one still holds its KV cache and compute buffer doubles the peak for
     * the duration of the switch — the first GPU run of this backend peaked at
     * 8,888 MiB attributable doing exactly that, against a working set that
     * needs about half of it. Two contexts are never alive at once here.
     *
     * The cost of getting it this way round is that a failed init leaves no
     * context at all, which is why the caller treats that as fatal for the
     * batch rather than retrying with the previous shape. */
    ask_llama_drop_ctx(s);

    struct llama_context *ctx = llama_init_from_model(s->model, cp);
    if (!ctx) {
        (void)snprintf(err, errlen,
                       "llama context init failed at n_ctx=%lld (n_seq_max=%d x seq_len=%d, "
                       "n_ubatch=%u)",
                       (long long)plan.n_ctx, n_seq, seq_len, cp.n_ubatch);
        return false;
    }
    if (llama_pooling_type(ctx) != LLAMA_POOLING_TYPE_LAST) {
        /* A model that will not last-token pool is not the model any number
         * here was measured with, and mean-pooled vectors are the right shape,
         * unit length, and wrong. Refuse rather than index. */
        (void)snprintf(err, errlen,
                       "the model resolved pooling type %d, not LAST (%d). Qwen3-Embedding is "
                       "last-token pooled and mean pooling measures 0.54-0.85 against correct",
                       (int)llama_pooling_type(ctx), (int)LLAMA_POOLING_TYPE_LAST);
        llama_free(ctx);
        return false;
    }

    s->ctx = ctx;
    s->seq_len = seq_len;
    s->n_seq_max = n_seq;
    char shape[224];
    (void)snprintf(shape, sizeof(shape),
                   "n_ctx=%lld n_seq_max=%d seq_len=%d n_batch=%u n_ubatch=%u kv_mib=%.0f "
                   "compute_mib=%.0f weights_mib=%.0f total_mib=%.0f ceiling_mib=%.0f",
                   (long long)plan.n_ctx, n_seq, seq_len, cp.n_batch, cp.n_ubatch, plan.kv_mib,
                   hyp_ask_compute_mib_for_ubatch((int)cp.n_ubatch),
                   HYP_ASK_WEIGHTS_RESIDENT_MIB,
                   plan.kv_mib + hyp_ask_compute_mib_for_ubatch((int)cp.n_ubatch) +
                       HYP_ASK_WEIGHTS_RESIDENT_MIB,
                   s->on_gpu ? s->ceiling_mib : 0.0);
    hyp_log_info("ask.llama.ctx", "plan", shape);
    return true;
}

/* ── Tokenisation ───────────────────────────────────────────────── */

static bool ask_toks_reserve(ask_llama_t *s, int n) {
    if (n <= s->toks_cap) {
        return true;
    }
    int cap = s->toks_cap ? s->toks_cap : 4096;
    while (cap < n) {
        cap *= 2;
    }
    llama_token *p = (llama_token *)realloc(s->toks, sizeof(llama_token) * (size_t)cap);
    if (!p) {
        return false;
    }
    s->toks = p;
    s->toks_cap = cap;
    return true;
}

/* Full, untruncated token count. Negative on failure.
 *
 * This is a DIFFERENT question from "how many slots will this occupy", and the
 * two are kept apart on purpose: deriving one from the other cannot tell a
 * document of exactly the window length from one cut off at it, which is the
 * only boundary the truncation counter cares about. */
static int ask_llama_full_tokens(ask_llama_t *s, const char *text) {
    if (!text) {
        return 0;
    }
    int32_t n = llama_tokenize(s->vocab, text, (int32_t)strlen(text), s->toks, s->toks_cap,
                               /*add_special=*/true, /*parse_special=*/true);
    if (n >= 0) {
        return (int)n;
    }
    /* llama_tokenize returns -(required) when the buffer is too small. */
    int need = -(int)n;
    if (!ask_toks_reserve(s, need)) {
        return -1;
    }
    n = llama_tokenize(s->vocab, text, (int32_t)strlen(text), s->toks, s->toks_cap, true, true);
    return n >= 0 ? (int)n : -1;
}

/* ── The one encode(), which the two entry points route through ──── */

static void ask_l2_normalise(float *v, int dim) {
    double sum = 0.0;
    for (int i = 0; i < dim; i++) {
        sum += (double)v[i] * (double)v[i];
    }
    double norm = sqrt(sum);
    if (norm < 1e-12) {
        return;
    }
    for (int i = 0; i < dim; i++) {
        v[i] = (float)((double)v[i] / norm);
    }
}

/* `prefix` is already rendered. The document path passes "" and has no way to
 * obtain anything else — that is the asymmetry, enforced by the fact that
 * neither public entry point lets a caller choose. */
static int ask_llama_encode(ask_llama_t *s, const char *const *texts, const char *prefix, int count,
                            float *out, char *err, size_t errlen) {
    if (count <= 0) {
        return 0;
    }
    size_t plen = prefix ? strlen(prefix) : 0;

    /* Pass 1: tokenise everything, find the longest, and TRUNCATE rather than
     * skip. The prototype skips a document longer than the batch's seq_len and
     * writes no output line for it, which shifts every later vector onto the
     * wrong declaration — a citation-to-the-wrong-code failure arriving through
     * a batching guard (runs/ASK T8-D1). Every input gets exactly one row. */
    int *lens = (int *)calloc((size_t)count, sizeof(int));
    llama_token **rows = (llama_token **)calloc((size_t)count, sizeof(llama_token *));
    if (!lens || !rows) {
        free(lens);
        free(rows);
        (void)snprintf(err, errlen, "out of memory tokenising %d document(s)", count);
        return -1;
    }

    int longest = 1;
    int rc = 0;
    for (int i = 0; i < count; i++) {
        const char *body = texts[i] ? texts[i] : "";
        char *joined = NULL;
        const char *full = body;
        if (plen) {
            size_t need = plen + strlen(body) + 1;
            joined = (char *)malloc(need);
            if (!joined) {
                (void)snprintf(err, errlen, "out of memory building the prefixed query");
                rc = -1;
                break;
            }
            (void)snprintf(joined, need, "%s%s", prefix, body);
            full = joined;
        }
        int n = ask_llama_full_tokens(s, full);
        free(joined);
        if (n < 0) {
            (void)snprintf(err, errlen, "tokenisation failed for document %d", i);
            rc = -1;
            break;
        }
        if (n == 0) {
            /* An empty span still needs a row, or the caller's positional
             * pairing breaks. One BOS-equivalent slot carries it. */
            n = 1;
            if (!ask_toks_reserve(s, 1)) {
                rc = -1;
                break;
            }
            s->toks[0] = llama_vocab_bos(s->vocab);
            if (s->toks[0] < 0) {
                s->toks[0] = 0;
            }
        }
        int keep = n;
        if (keep > HYP_ASK_MODEL_WINDOW) {
            keep = HYP_ASK_MODEL_WINDOW;
        }
        rows[i] = (llama_token *)malloc(sizeof(llama_token) * (size_t)keep);
        if (!rows[i]) {
            (void)snprintf(err, errlen, "out of memory holding %d tokens", keep);
            rc = -1;
            break;
        }
        memcpy(rows[i], s->toks, sizeof(llama_token) * (size_t)keep);
        lens[i] = keep;
        if (keep > longest) {
            longest = keep;
        }
    }

    if (rc == 0) {
        /* THE RECTANGLE. KV is charged for n_seq_max * seq_len whether the
         * documents fill it or not, so both edges have to be decided here and
         * both have to be checked before anything is allocated. */
        int seq_len = ask_bucket_for(longest);
        int n_seq = count;

        if (s->on_gpu) {
            /* How many sequences of this length the free-VRAM ceiling allows.
             * count is at most HYP_ASK_MAX_DOCS, so counting down is cheaper
             * to read than solving for it. */
            int max_seq = 0;
            for (int m = n_seq; m >= 1; m--) {
                hyp_ask_kv_plan_t p;
                if (hyp_ask_kv_plan(m, seq_len, s->ceiling_mib, &p)) {
                    max_seq = m;
                    break;
                }
            }
            if (max_seq == 0) {
                /* Not even one sequence at this bucket fits. Take the largest
                 * per-sequence length that DOES fit alone. That lowers the
                 * effective window for this batch, and a document longer than
                 * it is truncated — which the counter is denominated in and
                 * which is reported, rather than being discovered as a missing
                 * row later. */
                int fit = hyp_ask_seq_len_for_kv(1, s->ceiling_mib);
                if (fit < 1) {
                    (void)snprintf(err, errlen,
                                   "the GPU ceiling is %.0f MiB, which cannot hold the backend's "
                                   "%.0f MiB fixed cost plus one token of KV cache",
                                   s->ceiling_mib, HYP_ASK_LLAMA_FIXED_MIB);
                    rc = -1;
                } else {
                    hyp_log_warn("ask.llama.ceiling", "detail",
                                 "free VRAM cannot hold the model window; this batch is encoded "
                                 "at a smaller effective window and long rows are truncated");
                    seq_len = fit;
                    max_seq = 1;
                }
            }
            if (rc == 0 && max_seq < n_seq) {
                /* Narrow the batch rather than refuse one the grouping rule
                 * considered legal, or widen a rectangle the card cannot hold.
                 * Track 8 did exactly this by hand, in three passes. */
                for (int off = 0; off < count && rc == 0; off += max_seq) {
                    int chunk = count - off < max_seq ? count - off : max_seq;
                    rc = ask_llama_encode(s, texts + off, prefix, chunk,
                                          out + (size_t)off * (size_t)s->n_embd, err, errlen);
                }
                for (int i = 0; i < count; i++) {
                    free(rows[i]);
                }
                free(rows);
                free(lens);
                return rc;
            }
        }

        if (rc == 0 && (!s->ctx || s->seq_len != seq_len || s->n_seq_max < n_seq)) {
            if (!ask_llama_make_ctx(s, n_seq, seq_len, err, errlen)) {
                rc = -1;
            }
        }

        if (rc == 0) {
            int total = 0;
            for (int i = 0; i < count; i++) {
                if (lens[i] > seq_len) {
                    lens[i] = seq_len; /* head-truncated; counted by the caller */
                }
                total += lens[i];
            }
            struct llama_batch batch = llama_batch_init(total, 0, n_seq);
            int k = 0;
            for (int i = 0; i < count; i++) {
                for (int t = 0; t < lens[i]; t++) {
                    batch.token[k] = rows[i][t];
                    batch.pos[k] = t;
                    batch.n_seq_id[k] = 1;
                    batch.seq_id[k][0] = i;
                    batch.logits[k] = 1;
                    k++;
                }
            }
            batch.n_tokens = total;
            llama_memory_clear(llama_get_memory(s->ctx), true);
            struct timespec t0;
            (void)clock_gettime(CLOCK_MONOTONIC, &t0);
            if (llama_decode(s->ctx, batch) != 0) {
                (void)snprintf(err, errlen, "llama_decode failed (%d sequence(s), %d token(s))",
                               count, total);
                rc = -1;
            } else {
                struct timespec t1;
                (void)clock_gettime(CLOCK_MONOTONIC, &t1);
                double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                            (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
                char pass[128];
                (void)snprintf(pass, sizeof(pass), "docs=%d tokens=%d seq_len=%d ms=%.1f", count,
                               total, seq_len, ms);
                hyp_log_debug("ask.llama.decode", "pass", pass);
                for (int i = 0; i < count; i++) {
                    const float *emb = llama_get_embeddings_seq(s->ctx, i);
                    if (!emb) {
                        (void)snprintf(err, errlen, "no pooled embedding for sequence %d", i);
                        rc = -1;
                        break;
                    }
                    float *dst = out + (size_t)i * (size_t)s->n_embd;
                    memcpy(dst, emb, sizeof(float) * (size_t)s->n_embd);
                    ask_l2_normalise(dst, s->n_embd);
                }
            }
            llama_batch_free(batch);
        }
    }

    for (int i = 0; i < count; i++) {
        free(rows[i]);
    }
    free(rows);
    free(lens);
    return rc;
}

/* ── Construction ───────────────────────────────────────────────── */

static ask_llama_t *ask_llama_open(hyp_ask_device_pref_t pref, char *err, size_t errlen) {
    char path[HYP_MODEL_PATH_MAX];
    if (!hyp_model_ask_path(path, sizeof(path)) || path[0] == '\0') {
        (void)snprintf(err, errlen, "the model cache directory cannot be resolved");
        return NULL;
    }
    if (!hyp_model_ask_present()) {
        (void)snprintf(err, errlen,
                       "the ask lane's embedding weights are not on this machine (%s). Run `%s`.",
                       path, HYP_MODEL_ASK_COMMAND);
        return NULL;
    }

    ask_llama_backend_once();

    ask_llama_t *s = (ask_llama_t *)calloc(1, sizeof(*s));
    if (!s) {
        (void)snprintf(err, errlen, "out of memory");
        return NULL;
    }

    /* WHICH DEVICE. The request is an input; what follows is the outcome, and
     * they are recorded separately so a run that asked for a GPU and got a CPU
     * is visible rather than merely fifteen times slower. */
    char gpu_name[128];
    bool gpu_available = (pref != HYP_ASK_DEVICE_CPU) && ask_llama_gpu_present(gpu_name,
                                                                               sizeof(gpu_name));
    double total = 0.0;
    double used = 0.0;
    bool vram_readable = hyp_ask_device_vram_mib(&total, &used);
    bool use_gpu = false;

    if (gpu_available) {
        if (!vram_readable) {
            /* A ceiling that cannot be CHECKED must never be ASSUMED — see
             * the two lost desktop sessions in ask_batch.h. */
            if (pref == HYP_ASK_DEVICE_GPU) {
                (void)snprintf(err, errlen,
                               "--device gpu: a GPU is present (%s) but no device exposes its VRAM "
                               "to this process, so the allocation ceiling cannot be checked. "
                               "REFUSING rather than guessing; use --device auto or cpu",
                               gpu_name);
                free(s);
                return NULL;
            }
        } else {
            s->ceiling_mib = hyp_ask_gpu_ceiling_mib(total, used);
            hyp_ask_kv_plan_t probe;
            /* The smallest bucket has to fit at one sequence, or there is no
             * GPU pass to be had at all. */
            use_gpu = hyp_ask_kv_plan(1, ASK_SEQ_BUCKETS[0], s->ceiling_mib, &probe);
            if (!use_gpu && pref == HYP_ASK_DEVICE_GPU) {
                (void)snprintf(err, errlen,
                               "--device gpu: %.0f MiB of %.0f MiB VRAM is free (this card also "
                               "drives the display), giving a %.0f MiB ceiling — not enough for "
                               "the backend's %.0f MiB fixed cost plus a minimum KV cache. "
                               "REFUSING rather than attempting",
                               total - used, total, s->ceiling_mib, HYP_ASK_LLAMA_FIXED_MIB);
                free(s);
                return NULL;
            }
        }
    } else if (pref == HYP_ASK_DEVICE_GPU) {
        (void)snprintf(err, errlen,
                       "--device gpu: this build has no GPU backend it can reach. %s",
                       llama_supports_gpu_offload()
                           ? "A GPU backend is compiled in but no device answered."
                           : "This binary was built with HYP_ASK_GPU=none.");
        free(s);
        return NULL;
    }

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = use_gpu ? ASK_NGL_ALL : 0;
    s->model = llama_model_load_from_file(path, mp);
    if (!s->model) {
        (void)snprintf(err, errlen, "llama could not load the GGUF at %s", path);
        free(s);
        return NULL;
    }
    s->vocab = llama_model_get_vocab(s->model);
    s->n_embd = llama_model_n_embd(s->model);
    s->model_window = HYP_ASK_MODEL_WINDOW;
    {
        int trained = llama_model_n_ctx_train(s->model);
        if (trained > 0 && trained < s->model_window) {
            s->model_window = trained;
        }
    }
    s->on_gpu = use_gpu;

    if (s->n_embd != HYP_ASK_DIM) {
        (void)snprintf(err, errlen,
                       "the GGUF emits %d dimensions, not the %d every measured number here is "
                       "about",
                       s->n_embd, HYP_ASK_DIM);
        llama_model_free(s->model);
        free(s);
        return NULL;
    }

    if (use_gpu) {
        (void)snprintf(s->device_note, sizeof(s->device_note),
                       "GPU (%s, Vulkan) — %.0f MiB free of %.0f MiB, ceiling %.0f MiB", gpu_name,
                       total - used, total, s->ceiling_mib);
    } else if (pref == HYP_ASK_DEVICE_AUTO && gpu_available) {
        (void)snprintf(s->device_note, sizeof(s->device_note),
                       "CPU (%d threads) — DOWNGRADED from the GPU that is present (%s): not "
                       "enough free VRAM for a safe allocation",
                       ASK_CPU_THREADS, gpu_name);
    } else {
        (void)snprintf(s->device_note, sizeof(s->device_note), "CPU (%d threads)",
                       ASK_CPU_THREADS);
    }

    /* The model id is the identity of the WEIGHTS AND their configuration —
     * it is what `ask` refuses a mixed index on, so the quantisation and the
     * pinned revision both belong in it. */
    (void)snprintf(s->model_id, sizeof(s->model_id), "%s-%s@%.12s", HYP_MODEL_ASK_MODEL,
                   HYP_MODEL_ASK_QUANT, HYP_MODEL_ASK_REVISION);

    (void)ask_toks_reserve(s, 4096);
    return s;
}

static void ask_llama_close(ask_llama_t *s) {
    if (!s) {
        return;
    }
    ask_llama_drop_ctx(s);
    if (s->model) {
        llama_model_free(s->model);
    }
    free(s->toks);
    free(s);
}

/* ── hyp_ask_encoder_t: the embed pass's seam ───────────────────── */

static const char *enc_model_id(void *self) {
    return ((ask_llama_t *)self)->model_id;
}
static const char *enc_device_note(void *self) {
    return ((ask_llama_t *)self)->device_note;
}
static bool enc_device_is_gpu(void *self) {
    return ((ask_llama_t *)self)->on_gpu;
}
static int enc_dim(void *self) {
    return ((ask_llama_t *)self)->n_embd;
}

/* The window the counter is denominated in is the model's, not the live
 * context's: the context grows to the top bucket for any document that needs
 * it, so a declaration is only ever cut at HYP_ASK_MODEL_WINDOW. Reporting the
 * momentary seq_len here would make the disclosure depend on which batch
 * happened to be in flight. */
static int enc_window(void *self) {
    return ((ask_llama_t *)self)->model_window;
}

static int enc_token_length(void *self, const char *text) {
    ask_llama_t *s = (ask_llama_t *)self;
    int n = ask_llama_full_tokens(s, text);
    if (n < 0) {
        return -1;
    }
    return n > s->model_window ? s->model_window : n;
}

static int enc_full_token_length(void *self, const char *text) {
    return ask_llama_full_tokens((ask_llama_t *)self, text);
}

static int enc_encode_documents(void *self, const char *const *texts, int count, float *out) {
    char err[256];
    err[0] = '\0';
    int rc = ask_llama_encode((ask_llama_t *)self, texts, "", count, out, err, sizeof(err));
    if (rc != 0 && err[0]) {
        hyp_log_error("ask.llama.encode_documents", "err", err);
    }
    return rc;
}

static int enc_encode_query(void *self, const char *text, const char *language_display,
                            float *out) {
    char prefix[HYP_ASK_PREFIX_MAX];
    /* Rendering is the CALLEE's job, which is how "the prefix is applied to
     * queries only" stops being a convention every call site has to remember.
     * A NULL display name is a refusal, not a default: a prefix built with a
     * grammar id still encodes, still ranks and is not the prefix any number
     * was measured against. */
    if (!language_display || !language_display[0]) {
        /* ask_encoder.h documents NULL as "the backend uses its own default".
         * The default is the ONE language the recall numbers were measured on,
         * and it is logged rather than assumed silently. */
        language_display = hyp_ask_language_display(HYP_LANG_CPP);
        hyp_log_debug("ask.llama.encode_query", "language", "defaulted to C++");
        if (!language_display) {
            return -1;
        }
    }
    int n = snprintf(prefix, sizeof(prefix), HYP_ASK_INSTRUCT_TEMPLATE, language_display);
    if (n < 0 || (size_t)n >= sizeof(prefix)) {
        hyp_log_error("ask.llama.encode_query", "err", "the instruct prefix would not fit");
        return -1;
    }
    char err[256];
    err[0] = '\0';
    const char *one[1];
    one[0] = text;
    int rc = ask_llama_encode((ask_llama_t *)self, one, prefix, 1, out, err, sizeof(err));
    if (rc != 0 && err[0]) {
        hyp_log_error("ask.llama.encode_query", "err", err);
    }
    return rc;
}

static void enc_destroy(void *self) {
    ask_llama_close((ask_llama_t *)self);
}

static const hyp_ask_encoder_vt_t ASK_LLAMA_VT = {
    .model_id = enc_model_id,
    .device_note = enc_device_note,
    .device_is_gpu = enc_device_is_gpu,
    .dim = enc_dim,
    .window_tokens = enc_window,
    .token_length = enc_token_length,
    .full_token_length = enc_full_token_length,
    .encode_documents = enc_encode_documents,
    .encode_query = enc_encode_query,
    .destroy = enc_destroy,
};

hyp_ask_encoder_t *hyp_ask_llama_encoder_create(hyp_ask_device_pref_t pref, char *err,
                                                size_t errlen) {
    if (err && errlen) {
        err[0] = '\0';
    }
    ask_llama_t *s = ask_llama_open(pref, err, errlen);
    if (!s) {
        return NULL;
    }
    hyp_ask_encoder_t *e = (hyp_ask_encoder_t *)calloc(1, sizeof(*e));
    if (!e) {
        ask_llama_close(s);
        (void)snprintf(err, errlen, "out of memory");
        return NULL;
    }
    e->vt = &ASK_LLAMA_VT;
    e->self = s;
    return e;
}

/* ── hyp_ask_backend_t: the `ask` TOOL's query seam ─────────────── */

/* Lazily loaded. Registering the vtable costs nothing; loading 639 MB would
 * cost every `hyponoia mcp` startup, including the ones that never ask. */
static ask_llama_t *g_query_engine = NULL;
static bool g_query_tried = false;

static ask_llama_t *ask_llama_query_engine(char *err, size_t errlen) {
    if (g_query_engine) {
        return g_query_engine;
    }
    if (g_query_tried) {
        (void)snprintf(err, errlen, "the embedding model could not be loaded earlier in this "
                                    "process; see the log for the reason");
        return NULL;
    }
    g_query_tried = true;
    /* A query is one 22-token prefix plus a sentence: it does not need a GPU
     * (12 ms there against 37 ms on CPU, both far under the 248 ms budget) and
     * it must not compete for VRAM with an embed pass that may be running. */
    g_query_engine = ask_llama_open(HYP_ASK_DEVICE_CPU, err, errlen);
    return g_query_engine;
}

static int backend_encode_query(HYPLanguage lang, const char *text, float *out, char *err,
                                size_t errlen) {
    ask_llama_t *s = ask_llama_query_engine(err, errlen);
    if (!s) {
        return -1;
    }
    char prefix[HYP_ASK_PREFIX_MAX];
    int n = hyp_ask_render_instruct_prefix(lang, prefix, sizeof(prefix));
    if (n <= 0) {
        (void)snprintf(err, errlen,
                       "no display name for this language, so the instruct prefix cannot be "
                       "rendered — refusing rather than sending a prefix no number was measured "
                       "against");
        return -1;
    }
    const char *one[1];
    one[0] = text;
    return ask_llama_encode(s, one, prefix, 1, out, err, errlen);
}

static int backend_encode_documents(const char *const *texts, int n, float *out, char *err,
                                    size_t errlen) {
    ask_llama_t *s = ask_llama_query_engine(err, errlen);
    if (!s) {
        return -1;
    }
    return ask_llama_encode(s, texts, "", n, out, err, errlen);
}

static int backend_truncates(const char *text, bool *outv, char *err, size_t errlen) {
    ask_llama_t *s = ask_llama_query_engine(err, errlen);
    if (!s) {
        return -1;
    }
    int full = ask_llama_full_tokens(s, text);
    if (full < 0) {
        (void)snprintf(err, errlen, "tokenisation failed");
        return -1;
    }
    *outv = full > s->model_window;
    return 0;
}

static const hyp_ask_backend_t ASK_LLAMA_BACKEND = {
    .model_id = HYP_MODEL_ASK_MODEL "-" HYP_MODEL_ASK_QUANT "@370f27d7550e",
    .dim = HYP_ASK_DIM,
    .window_tokens = HYP_ASK_MODEL_WINDOW,
    .encode_query = backend_encode_query,
    .encode_documents = backend_encode_documents,
    .truncates = backend_truncates,
};

int hyp_ask_llama_backend_install(void) {
    return hyp_ask_backend_install(&ASK_LLAMA_BACKEND);
}

void hyp_ask_llama_backend_shutdown(void) {
    if (g_query_engine) {
        ask_llama_close(g_query_engine);
        g_query_engine = NULL;
    }
    g_query_tried = false;
}

bool hyp_ask_llama_compiled_in(void) {
    return true;
}

const char *hyp_ask_llama_build_note(void) {
    /* The pin is spelled here rather than taken from ggml's GGML_COMMIT: that
     * macro exists only inside the vendored flags bucket, and a build note
     * that silently became empty when the buckets drifted would be worse than
     * no build note. vendored/llama.cpp/NOTICE carries the same value. */
#if HYP_ASK_GPU_VULKAN
    return "llama.cpp 74ce157 — CPU + Vulkan (this binary needs libvulkan.so.1 "
           "at runtime and is therefore NOT statically linked)";
#else
    return "llama.cpp 74ce157 — CPU only (statically linkable)";
#endif
}

#else /* !HYP_ASK_LLAMA */

/* No inference runtime in this build. Every entry point still exists and every
 * one of them says so, because `no_encoder` is a first-class outcome with its
 * own remedy — a different build — and not an error. */

bool hyp_ask_llama_compiled_in(void) {
    return false;
}

const char *hyp_ask_llama_build_note(void) {
    return "no inference runtime compiled in";
}

hyp_ask_encoder_t *hyp_ask_llama_encoder_create(hyp_ask_device_pref_t pref, char *err,
                                                size_t errlen) {
    (void)pref;
    if (err && errlen) {
        (void)snprintf(err, errlen, "this build has no embedding backend linked in");
    }
    return NULL;
}

int hyp_ask_llama_backend_install(void) {
    return 0; /* nothing to install; hyp_ask_backend() stays NULL */
}

void hyp_ask_llama_backend_shutdown(void) {
}

#endif /* HYP_ASK_LLAMA */
