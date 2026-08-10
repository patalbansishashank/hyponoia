/*
 * ask_rerank.c — Qwen3-Reranker-0.6B, in-process, scoring pairs.
 *
 * The design argument is in ask_rerank.h. This file is the prompt and the
 * arithmetic. Read ask_batch.h's n_ctx note before changing any sizing: the KV
 * law is the same one, because it is the same model shape, and it is the law
 * that cost two desktop sessions.
 */

#include "ask/ask_rerank.h"

#include "ask/ask_batch.h"
#include "cli/model_fetch.h"
#include "foundation/constants.h"
#include "foundation/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HYP_ASK_LLAMA

#include "llama.h"

#include <math.h>
#include <time.h>

/* ── The prompt, which is the model's and not ours ───────────────────
 *
 * Byte-for-byte the string Qwen's reference implementation builds and the one
 * llama.cpp's own Qwen3-reranker converter bakes into the GGUF as a chat
 * template (conversion/qwen.py, set_gguf_parameters). The only substitution is
 * the instruct line. Changing anything else here silently invalidates every
 * number in runs/ASK/L2-reranker.json. */
#define RR_SYSTEM                                                                          \
    "<|im_start|>system\nJudge whether the Document meets the requirements based on the "  \
    "Query and the Instruct provided. Note that the answer can only be \"yes\" or "        \
    "\"no\".<|im_end|>\n<|im_start|>user\n"
#define RR_ASSISTANT "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"

/* The one templated part, and it takes the DISPLAY name — "C++", not "cpp" —
 * for exactly the reason §2.1 spells out for the query prefix: rendered with a
 * grammar id it still scores, still ranks, and is not the prompt anything was
 * measured with. */
#define RR_INSTRUCT_TEMPLATE \
    "<Instruct>: Given a natural-language description of %s code, retrieve the declaration it " \
    "describes\n<Query>: "

/* Threads for the CPU path, matching the encoder's. */
enum { RR_CPU_THREADS = 16 };
enum { RR_NGL_ALL = 99 };

/* Per-sequence context lengths a rerank context will be built at. Same reason
 * as the encoder's buckets: n_ctx is fixed at creation and rebuilding it per
 * chunk would re-allocate the KV cache for every group of 16. */
static const int RR_SEQ_BUCKETS[] = {512, 1024, HYP_ASK_RERANK_PAIR_WINDOW};
enum { RR_SEQ_BUCKET_COUNT = (int)(sizeof(RR_SEQ_BUCKETS) / sizeof(RR_SEQ_BUCKETS[0])) };

typedef struct {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;

    llama_token tok_yes;
    llama_token tok_no;

    int seq_len;   /* per-sequence limit of the LIVE context; 0 = none yet */
    int n_seq_max; /* sequences the LIVE context can hold */

    bool on_gpu;
    double ceiling_mib;
    char device_note[192];
    char model_id[128];

    llama_token *toks;
    int toks_cap;
} ask_rerank_t;

static ask_rerank_t *g_rr = NULL;
static bool g_rr_tried = false;
static int g_last_truncated = 0;
static double g_last_ms = 0.0;

/* ── Logging ─────────────────────────────────────────────────────── */

static void rr_log(enum ggml_log_level level, const char *text, void *user) {
    (void)user;
    if (!text || !text[0] || (text[0] == '\n' && !text[1])) {
        return;
    }
    /* GGML_LOG_LEVEL_CONT is 5 and means "continuation", not "worse than
     * ERROR" — see ask_llama.c, where treating it as severe promoted every
     * progress dot of a model load to an error. */
    switch (level) {
    case GGML_LOG_LEVEL_ERROR:
        hyp_log_error("ask.rerank.backend", "msg", text);
        break;
    case GGML_LOG_LEVEL_WARN:
        hyp_log_warn("ask.rerank.backend", "msg", text);
        break;
    default:
        hyp_log_debug("ask.rerank.backend", "msg", text);
        break;
    }
}

/* ── Tokens ──────────────────────────────────────────────────────── */

static bool rr_toks_reserve(ask_rerank_t *s, int n) {
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

/* Tokenise into the scratch buffer, growing it once if needed. Negative on
 * failure. */
static int rr_tokenise(ask_rerank_t *s, const char *text, bool special) {
    if (!text) {
        return 0;
    }
    int32_t n = llama_tokenize(s->vocab, text, (int32_t)strlen(text), s->toks, s->toks_cap,
                               /*add_special=*/false, /*parse_special=*/special);
    if (n >= 0) {
        return (int)n;
    }
    if (!rr_toks_reserve(s, -(int)n)) {
        return -1;
    }
    n = llama_tokenize(s->vocab, text, (int32_t)strlen(text), s->toks, s->toks_cap, false, special);
    return n >= 0 ? (int)n : -1;
}

/* The two tokens the whole method reduces to.
 *
 * Resolved by EXACT VOCAB TEXT rather than by tokenising the string "yes",
 * which is the same thing the reference's convert_tokens_to_ids does and is not
 * the same thing as tokenising: a BPE tokeniser is free to split, to prepend a
 * word boundary, or to prefer a longer merge, and any of those would silently
 * score a different pair of logits. Refusing when either is missing or
 * ambiguous is the only safe answer — every number this file returns is a
 * softmax over exactly these two. */
static bool rr_resolve_yes_no(ask_rerank_t *s, char *err, size_t errlen) {
    s->tok_yes = -1;
    s->tok_no = -1;
    int n = llama_vocab_n_tokens(s->vocab);
    for (int i = 0; i < n; i++) {
        const char *t = llama_vocab_get_text(s->vocab, i);
        if (!t) {
            continue;
        }
        if (strcmp(t, "yes") == 0) {
            if (s->tok_yes >= 0) {
                (void)snprintf(err, errlen, "the vocabulary holds two tokens spelled \"yes\"");
                return false;
            }
            s->tok_yes = i;
        } else if (strcmp(t, "no") == 0) {
            if (s->tok_no >= 0) {
                (void)snprintf(err, errlen, "the vocabulary holds two tokens spelled \"no\"");
                return false;
            }
            s->tok_no = i;
        }
    }
    if (s->tok_yes < 0 || s->tok_no < 0) {
        (void)snprintf(err, errlen,
                       "this GGUF's vocabulary has no single token spelled \"yes\" (%d) or \"no\" "
                       "(%d). A Qwen3 reranker scores a pair by the softmax over exactly those "
                       "two next-token logits, so without them there is nothing to score",
                       s->tok_yes, s->tok_no);
        return false;
    }
    return true;
}

/* ── Context ─────────────────────────────────────────────────────── */

static int rr_bucket_for(int longest) {
    for (int i = 0; i < RR_SEQ_BUCKET_COUNT; i++) {
        if (longest <= RR_SEQ_BUCKETS[i]) {
            return RR_SEQ_BUCKETS[i];
        }
    }
    return RR_SEQ_BUCKETS[RR_SEQ_BUCKET_COUNT - 1];
}

static void rr_drop_ctx(ask_rerank_t *s) {
    if (s->ctx) {
        llama_free(s->ctx);
        s->ctx = NULL;
    }
    s->seq_len = 0;
    s->n_seq_max = 0;
}

static bool rr_make_ctx(ask_rerank_t *s, int n_seq, int seq_len, char *err, size_t errlen) {
    hyp_ask_kv_plan_t plan;
    if (s->on_gpu) {
        if (!hyp_ask_kv_plan(n_seq, seq_len, s->ceiling_mib, &plan)) {
            (void)snprintf(err, errlen,
                           "REFUSED before allocating: %d pairs x %d tokens is n_ctx=%lld, "
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
    /* NOT an embedding context. This lane wants the language-model head, which
     * is the whole mechanism: the score is a next-token distribution restricted
     * to two tokens. Asking for embeddings here would return a pooled vector
     * and no logits at all. */
    cp.embeddings = false;
    cp.pooling_type = LLAMA_POOLING_TYPE_NONE;

    /* n_ctx is TOTAL KV capacity and llama.cpp DIVIDES it by n_seq_max. */
    cp.n_ctx = (uint32_t)plan.n_ctx;
    cp.n_seq_max = (uint32_t)n_seq;

    int n_batch = n_seq * seq_len;
    if ((int64_t)n_batch > plan.n_ctx) {
        n_batch = (int)plan.n_ctx;
    }
    cp.n_batch = (uint32_t)n_batch;

    double compute_budget =
        s->on_gpu ? s->ceiling_mib - plan.kv_mib - HYP_ASK_WEIGHTS_RESIDENT_MIB : 1e9;
    cp.n_ubatch = (uint32_t)hyp_ask_ubatch_for(n_batch, compute_budget);
    cp.n_threads = RR_CPU_THREADS;
    cp.n_threads_batch = RR_CPU_THREADS;

    /* Freed FIRST, for the reason ask_llama.c records: two contexts alive at
     * once doubles the peak for the duration of the switch. */
    rr_drop_ctx(s);

    struct llama_context *ctx = llama_init_from_model(s->model, cp);
    if (!ctx) {
        (void)snprintf(err, errlen,
                       "llama context init failed at n_ctx=%lld (%d pairs x %d tokens, "
                       "n_ubatch=%u)",
                       (long long)plan.n_ctx, n_seq, seq_len, cp.n_ubatch);
        return false;
    }
    s->ctx = ctx;
    s->seq_len = seq_len;
    s->n_seq_max = n_seq;
    char shape[224];
    (void)snprintf(shape, sizeof(shape),
                   "pairs=%d seq_len=%d n_ctx=%lld n_batch=%u n_ubatch=%u kv_mib=%.0f "
                   "compute_mib=%.0f ceiling_mib=%.0f",
                   n_seq, seq_len, (long long)plan.n_ctx, cp.n_batch, cp.n_ubatch, plan.kv_mib,
                   hyp_ask_compute_mib_for_ubatch((int)cp.n_ubatch),
                   s->on_gpu ? s->ceiling_mib : 0.0);
    hyp_log_info("ask.rerank.ctx", "plan", shape);
    return true;
}

/* ── Open ────────────────────────────────────────────────────────── */

static bool rr_gpu_present(char *name, size_t name_sz) {
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

static void rr_close(ask_rerank_t *s) {
    if (!s) {
        return;
    }
    rr_drop_ctx(s);
    if (s->model) {
        llama_model_free(s->model);
    }
    free(s->toks);
    free(s);
}

static ask_rerank_t *rr_open(char *err, size_t errlen) {
    char path[HYP_MODEL_PATH_MAX];
    if (!hyp_model_rerank_path(path, sizeof(path)) || path[0] == '\0') {
        (void)snprintf(err, errlen, "the model cache directory cannot be resolved");
        return NULL;
    }
    if (!hyp_model_rerank_present()) {
        (void)snprintf(err, errlen, "%s are not on this machine (%s). Run `%s`.",
                       hyp_model_rerank_spec()->what, path, HYP_MODEL_RERANK_COMMAND);
        return NULL;
    }

    /* llama_backend_init is idempotent and ask_llama.c may already have called
     * it; the log hook is set unconditionally so ggml chatter from either model
     * lands in the same place. */
    llama_log_set(rr_log, NULL);
    llama_backend_init();

    ask_rerank_t *s = (ask_rerank_t *)calloc(1, sizeof(*s));
    if (!s) {
        (void)snprintf(err, errlen, "out of memory");
        return NULL;
    }

    /* AUTO, and only AUTO. See the device note in ask_rerank.h: unlike the
     * query encoder this lane is 100-200x the tokens, so the CPU is a
     * last resort rather than the default — but it is never a refusal, because
     * a slow rerank still beats no rerank and the alternative would be a lane
     * that vanishes when the display is busy. */
    char gpu_name[128];
    bool gpu_available = rr_gpu_present(gpu_name, sizeof(gpu_name));
    double total = 0.0;
    double used = 0.0;
    bool vram_readable = hyp_ask_device_vram_mib(&total, &used);
    bool use_gpu = false;
    if (gpu_available && vram_readable) {
        s->ceiling_mib = hyp_ask_gpu_ceiling_mib(total, used);
        hyp_ask_kv_plan_t probe;
        /* One pair at the smallest bucket, or there is no GPU pass to be had. */
        use_gpu = hyp_ask_kv_plan(1, RR_SEQ_BUCKETS[0], s->ceiling_mib, &probe);
    }

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = use_gpu ? RR_NGL_ALL : 0;
    s->model = llama_model_load_from_file(path, mp);
    if (!s->model) {
        (void)snprintf(err, errlen, "llama could not load the reranker GGUF at %s", path);
        free(s);
        return NULL;
    }
    s->vocab = llama_model_get_vocab(s->model);
    s->on_gpu = use_gpu;

    if (!rr_resolve_yes_no(s, err, errlen)) {
        llama_model_free(s->model);
        free(s);
        return NULL;
    }

    if (use_gpu) {
        (void)snprintf(s->device_note, sizeof(s->device_note),
                       "GPU (%s, Vulkan) — %.0f MiB free of %.0f MiB, ceiling %.0f MiB", gpu_name,
                       total - used, total, s->ceiling_mib);
    } else if (gpu_available) {
        (void)snprintf(s->device_note, sizeof(s->device_note),
                       "CPU (%d threads) — DOWNGRADED from the GPU that is present (%s): not "
                       "enough free VRAM for a safe allocation. Reranking is 100-200x the tokens "
                       "of a query encode, so this is seconds rather than milliseconds",
                       RR_CPU_THREADS, gpu_name);
    } else {
        (void)snprintf(s->device_note, sizeof(s->device_note), "CPU (%d threads)", RR_CPU_THREADS);
    }

    (void)snprintf(s->model_id, sizeof(s->model_id), "%s-%s@%.12s", HYP_MODEL_RERANK_MODEL,
                   HYP_MODEL_RERANK_QUANT, HYP_MODEL_RERANK_REVISION);

    (void)rr_toks_reserve(s, 4096);
    return s;
}

static ask_rerank_t *rr_engine(char *err, size_t errlen) {
    if (g_rr) {
        return g_rr;
    }
    if (g_rr_tried) {
        (void)snprintf(err, errlen, "the reranker could not be loaded earlier in this process; "
                                    "see the log for the reason");
        return NULL;
    }
    g_rr_tried = true;
    g_rr = rr_open(err, errlen);
    return g_rr;
}

/* ── Building one pair ───────────────────────────────────────────── */

/* Tokenise a pair into `dst`, head-truncating THE DOCUMENT ONLY so the closing
 * turn always survives. Returns the token count, or -1.
 *
 * The three fixed parts are tokenised separately from the document precisely so
 * that this is possible: concatenate first and a truncation at the window cuts
 * off `<|im_end|><|im_start|>assistant`, leaving a prompt that asks the model
 * nothing and whose next token is not a verdict at all. That failure is silent
 * — it still produces two logits and a plausible number. */
static int rr_build_pair(ask_rerank_t *s, const llama_token *head, int head_n,
                         const llama_token *tail, int tail_n, const char *doc, llama_token **dst,
                         bool *truncated) {
    *truncated = false;
    int room = HYP_ASK_RERANK_PAIR_WINDOW - head_n - tail_n;
    if (room < 1) {
        return -1; /* the question alone does not fit; caller reports it */
    }
    int doc_n = rr_tokenise(s, doc ? doc : "", /*special=*/false);
    if (doc_n < 0) {
        return -1;
    }
    int keep = doc_n > room ? room : doc_n;
    if (keep < doc_n) {
        *truncated = true;
    }
    int total = head_n + keep + tail_n;
    llama_token *row = (llama_token *)malloc(sizeof(llama_token) * (size_t)(total > 0 ? total : 1));
    if (!row) {
        return -1;
    }
    memcpy(row, head, sizeof(llama_token) * (size_t)head_n);
    memcpy(row + head_n, s->toks, sizeof(llama_token) * (size_t)keep);
    memcpy(row + head_n + keep, tail, sizeof(llama_token) * (size_t)tail_n);
    *dst = row;
    return total;
}

/* ── Scoring one chunk ───────────────────────────────────────────── */

static bool rr_score_chunk(ask_rerank_t *s, llama_token **rows, const int *lens, int n, float *out,
                           char *err, size_t errlen) {
    int longest = 1;
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += lens[i];
        if (lens[i] > longest) {
            longest = lens[i];
        }
    }
    int seq_len = rr_bucket_for(longest);

    if (!s->ctx || s->seq_len != seq_len || s->n_seq_max < n) {
        if (!rr_make_ctx(s, n, seq_len, err, errlen)) {
            return false;
        }
    }

    struct llama_batch b = llama_batch_init(total, 0, n);
    int *last = (int *)calloc((size_t)n, sizeof(int));
    if (!last) {
        llama_batch_free(b);
        (void)snprintf(err, errlen, "out of memory");
        return false;
    }
    int p = 0;
    for (int i = 0; i < n; i++) {
        for (int t = 0; t < lens[i]; t++) {
            b.token[p] = rows[i][t];
            b.pos[p] = t;
            b.n_seq_id[p] = 1;
            b.seq_id[p][0] = i;
            /* Logits at the LAST position only. A cross-encoder needs one
             * distribution per pair, and asking for all of them would
             * materialise n x seq_len x 151,669 floats — 4.7 GiB for a single
             * chunk of 16 at the pair window. */
            b.logits[p] = (t == lens[i] - 1);
            if (t == lens[i] - 1) {
                last[i] = p;
            }
            p++;
        }
    }
    b.n_tokens = total;

    llama_memory_clear(llama_get_memory(s->ctx), true);
    struct timespec t0;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    bool ok = llama_decode(s->ctx, b) == 0;
    struct timespec t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!ok) {
        (void)snprintf(err, errlen, "llama_decode failed (%d pair(s), %d token(s))", n, total);
    } else {
        double ms =
            (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        char pass[128];
        (void)snprintf(pass, sizeof(pass), "pairs=%d tokens=%d seq_len=%d ms=%.1f", n, total,
                       seq_len, ms);
        hyp_log_debug("ask.rerank.decode", "pass", pass);
        for (int i = 0; i < n; i++) {
            const float *lg = llama_get_logits_ith(s->ctx, last[i]);
            if (!lg) {
                (void)snprintf(err, errlen, "no logits for pair %d", i);
                ok = false;
                break;
            }
            /* THE SCORE. softmax over exactly two logits, computed in the
             * numerically stable order — the raw logits sit around 8..18 and
             * exp(18) is fine, but the shift costs nothing and the failure it
             * prevents is a silent inf/inf = nan that would sort as neither
             * greater nor less than anything. */
            double ly = (double)lg[s->tok_yes];
            double ln = (double)lg[s->tok_no];
            double mx = ly > ln ? ly : ln;
            double ey = exp(ly - mx);
            double en = exp(ln - mx);
            out[i] = (float)(ey / (ey + en));
        }
    }
    free(last);
    llama_batch_free(b);
    return ok;
}

/* ── The entry point ─────────────────────────────────────────────── */

hyp_ask_rerank_status_t hyp_ask_rerank_score(const char *question, const char *language_display,
                                             const char *const *docs, int n, float *out, char *err,
                                             size_t errlen) {
    if (err && errlen) {
        err[0] = '\0';
    }
    g_last_truncated = 0;
    g_last_ms = 0.0;
    if (n <= 0) {
        return HYP_ASK_RERANK_OK;
    }
    if (!hyp_model_rerank_present()) {
        return HYP_ASK_RERANK_NO_WEIGHTS;
    }
    ask_rerank_t *s = rr_engine(err, errlen);
    if (!s) {
        return HYP_ASK_RERANK_FAILED;
    }
    if (!language_display || !language_display[0]) {
        (void)snprintf(err, errlen,
                       "no display name for this language, so the instruct line cannot be "
                       "rendered — refusing rather than sending a prompt no number was measured "
                       "against");
        return HYP_ASK_RERANK_FAILED;
    }

    struct timespec w0;
    (void)clock_gettime(CLOCK_MONOTONIC, &w0);

    /* The fixed head (system turn + instruct + question) and the fixed tail
     * (closing turn + assistant preamble) are tokenised ONCE per call. They are
     * identical across every pair, which is most of what makes N=100 tractable
     * at all. */
    char head_text[HYP_SZ_4K];
    int hn = snprintf(head_text, sizeof(head_text), RR_SYSTEM RR_INSTRUCT_TEMPLATE "%s\n<Document>: ",
                      language_display, question ? question : "");
    if (hn < 0 || (size_t)hn >= sizeof(head_text)) {
        (void)snprintf(err, errlen, "the rerank prompt would not fit; the question is too long");
        return HYP_ASK_RERANK_FAILED;
    }
    int head_n = rr_tokenise(s, head_text, /*special=*/true);
    if (head_n < 0) {
        (void)snprintf(err, errlen, "tokenising the rerank prompt failed");
        return HYP_ASK_RERANK_FAILED;
    }
    llama_token *head = (llama_token *)malloc(sizeof(llama_token) * (size_t)head_n);
    if (!head) {
        (void)snprintf(err, errlen, "out of memory");
        return HYP_ASK_RERANK_FAILED;
    }
    memcpy(head, s->toks, sizeof(llama_token) * (size_t)head_n);

    int tail_n = rr_tokenise(s, RR_ASSISTANT, /*special=*/true);
    if (tail_n < 0) {
        free(head);
        (void)snprintf(err, errlen, "tokenising the rerank prompt failed");
        return HYP_ASK_RERANK_FAILED;
    }
    llama_token *tail = (llama_token *)malloc(sizeof(llama_token) * (size_t)tail_n);
    if (!tail) {
        free(head);
        (void)snprintf(err, errlen, "out of memory");
        return HYP_ASK_RERANK_FAILED;
    }
    memcpy(tail, s->toks, sizeof(llama_token) * (size_t)tail_n);

    llama_token **rows = (llama_token **)calloc((size_t)n, sizeof(*rows));
    int *lens = (int *)calloc((size_t)n, sizeof(int));
    float *scores = (float *)calloc((size_t)n, sizeof(float));
    if (!rows || !lens || !scores) {
        free(head);
        free(tail);
        free(rows);
        free(lens);
        free(scores);
        (void)snprintf(err, errlen, "out of memory");
        return HYP_ASK_RERANK_FAILED;
    }

    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        bool cut = false;
        int len = rr_build_pair(s, head, head_n, tail, tail_n, docs[i], &rows[i], &cut);
        if (len < 0) {
            (void)snprintf(err, errlen, "building the rerank prompt for candidate %d failed", i);
            ok = false;
            break;
        }
        lens[i] = len;
        if (cut) {
            g_last_truncated++;
        }
    }

    /* Chunked so the KV rectangle stays inside the ceiling. Grouping is by
     * POSITION rather than by length on purpose: a length sort would reorder
     * the candidates and the mapping back is one more thing to get wrong, for a
     * padding saving that does not exist here — llama.cpp charges for tokens
     * decoded, not for a padded rectangle. Only the KV allocation is
     * rectangular, and the bucket already absorbs that. */
    for (int off = 0; off < n && ok; off += HYP_ASK_RERANK_MAX_PAIRS) {
        int chunk = n - off < HYP_ASK_RERANK_MAX_PAIRS ? n - off : HYP_ASK_RERANK_MAX_PAIRS;
        /* Narrow further if the ceiling says so, one pair at a time down. */
        if (s->on_gpu) {
            int longest = 1;
            for (int i = off; i < off + chunk; i++) {
                if (lens[i] > longest) {
                    longest = lens[i];
                }
            }
            int bucket = rr_bucket_for(longest);
            int fit = 0;
            for (int m = chunk; m >= 1; m--) {
                hyp_ask_kv_plan_t plan;
                if (hyp_ask_kv_plan(m, bucket, s->ceiling_mib, &plan)) {
                    fit = m;
                    break;
                }
            }
            if (fit == 0) {
                (void)snprintf(err, errlen,
                               "the GPU ceiling is %.0f MiB, which cannot hold even one %d-token "
                               "rerank pair",
                               s->ceiling_mib, bucket);
                ok = false;
                break;
            }
            if (fit < chunk) {
                chunk = fit;
            }
        }
        ok = rr_score_chunk(s, rows + off, lens + off, chunk, scores + off, err, errlen);
        if (ok && chunk < HYP_ASK_RERANK_MAX_PAIRS) {
            /* The loop step is the constant, so a narrowed chunk has to walk
             * back the difference or candidates would be skipped unscored —
             * which would leave a zero in `scores` and sort a real answer to
             * the bottom. */
            off -= (HYP_ASK_RERANK_MAX_PAIRS - chunk);
        }
    }

    struct timespec w1;
    (void)clock_gettime(CLOCK_MONOTONIC, &w1);
    g_last_ms = (double)(w1.tv_sec - w0.tv_sec) * 1e3 + (double)(w1.tv_nsec - w0.tv_nsec) / 1e6;

    if (ok) {
        memcpy(out, scores, sizeof(float) * (size_t)n);
    }
    for (int i = 0; i < n; i++) {
        free(rows[i]);
    }
    free(rows);
    free(lens);
    free(scores);
    free(head);
    free(tail);
    if (!ok) {
        g_last_truncated = 0;
        if (err && errlen && !err[0]) {
            (void)snprintf(err, errlen, "the reranker failed without a reason");
        }
        return HYP_ASK_RERANK_FAILED;
    }
    return HYP_ASK_RERANK_OK;
}

bool hyp_ask_rerank_compiled_in(void) {
    return true;
}

const char *hyp_ask_rerank_model_id(void) {
    return g_rr ? g_rr->model_id : "";
}

const char *hyp_ask_rerank_device_note(void) {
    return g_rr ? g_rr->device_note : "";
}

void hyp_ask_rerank_shutdown(void) {
    if (g_rr) {
        rr_close(g_rr);
        g_rr = NULL;
    }
    g_rr_tried = false;
}

#else /* !HYP_ASK_LLAMA */

hyp_ask_rerank_status_t hyp_ask_rerank_score(const char *question, const char *language_display,
                                             const char *const *docs, int n, float *out, char *err,
                                             size_t errlen) {
    (void)question;
    (void)language_display;
    (void)docs;
    (void)n;
    (void)out;
    if (err && errlen) {
        (void)snprintf(err, errlen, "this build has no inference runtime, so no cross-encoder");
    }
    return HYP_ASK_RERANK_NO_BACKEND;
}

bool hyp_ask_rerank_compiled_in(void) {
    return false;
}

const char *hyp_ask_rerank_model_id(void) {
    return "";
}

const char *hyp_ask_rerank_device_note(void) {
    return "";
}

void hyp_ask_rerank_shutdown(void) {
}

#endif /* HYP_ASK_LLAMA */

/* Compiled in BOTH configurations: "are the weights on disk" is a question
 * about the filesystem, and a build with no runtime must still answer it the
 * same way, or the two unavailable causes get told apart by the wrong test. */
bool hyp_ask_rerank_available(void) {
    return hyp_ask_rerank_compiled_in() && hyp_model_rerank_present();
}

int hyp_ask_rerank_last_truncated(void) {
#if HYP_ASK_LLAMA
    return g_last_truncated;
#else
    return 0;
#endif
}

double hyp_ask_rerank_last_ms(void) {
#if HYP_ASK_LLAMA
    return g_last_ms;
#else
    return 0.0;
#endif
}
