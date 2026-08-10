/*
 * ask_embed.h — the inference boundary for the `ask` lane (NEXT-STEPS.md §2.1).
 *
 * This header is the ONLY thing the `ask` tool knows about embedding. Whether
 * a transformer runs in-process (llama.cpp/GGUF statically linked), out of
 * process, or not at all is settled behind `hyp_ask_backend_t` and changes
 * nothing above this line. Nothing here includes a model, a tokenizer or a
 * BLAS.
 *
 * ── The asymmetry is the mechanism, so it is enforced structurally ──
 *
 * Qwen3-Embedding is asymmetric: a QUERY is encoded behind an instruct prefix
 * and a DOCUMENT is not. That asymmetry is what lets a question match code
 * sharing none of its words, and ctxengine's R@1 0.7833 is a number about it.
 * So there are exactly TWO entry points and no third one taking a flag:
 *
 *     encode_query(lang, text)  — renders the instruct prefix, then encodes
 *     encode_documents(texts)   — bare; takes NO language, so the prefix is
 *                                 not merely discouraged, it is UNSPELLABLE
 *
 * ctxengine's `semantic.py` says why: a single `encode()` "would make the
 * commonest way to lose recall the shortest thing to write, and it would lose
 * it silently, since both mistakes still return unit vectors of the right
 * shape". This port goes one step further than the reference — dropping the
 * language parameter from the document side means an implementation cannot
 * apply the prefix to documents even by accident, because it has nothing to
 * render one from.
 *
 * ── Geometry is fixed by the measurement, not by us ──
 *
 * float32, 1024 dimensions, NO Matryoshka truncation, L2-normalised. This
 * differs from §2's static token table (int8/768) on purpose: that table is a
 * different lane with a different constraint (`CBM_SEM_DIM`), and truncating
 * to 768 to share a struct with it would be an unmeasured change to the thing
 * being ported. Cosine == dot product only because both sides are unit
 * vectors, which is why `HYP_ASK_NORM_TOLERANCE` is a check and not a repair.
 *
 * Depends on: hyp.h (HYPLanguage). Deliberately NOT on store.h — an encoder
 * has no opinion about where vectors are kept.
 */
#ifndef HYP_ASK_EMBED_H
#define HYP_ASK_EMBED_H

#include <stdbool.h>
#include <stddef.h>

#include "hyp.h" /* HYPLanguage */

/* Vector geometry. Both are copied from the measured configuration
 * (runs/ASK/T1-ctxengine-recipe.json §3) and neither is a tuning knob. */
enum {
    HYP_ASK_DIM = 1024,        /* Qwen3-Embedding-0.6B, no truncation */
    HYP_ASK_MODEL_ID_MAX = 128 /* room for "Qwen3-Embedding-0.6B@<revision>" */
};

/* How far a stored row's L2 norm may sit from 1.0 before it is rejected.
 *
 * SET FROM MEASUREMENT, after 1e-3 was set from intuition and was wrong:
 * over 220 encodings the worst deviation was 0.003819 (p99 0.003529, median
 * 0.001058), so 1e-3 rejects about HALF of real model output. 1e-2 sits ~2.6x
 * above the worst measurement and ~50x below a genuine normalisation failure
 * (an unnormalised vector deviates by ~0.5 or ~1.0). Two orders of magnitude
 * separate the accepted band from the failure band, which is what makes this
 * a check rather than a coin flip. */
/* Also spelled in src/ask/ask_encoder.h — the same seam-level constant, on
 * the other side of the same lane. The two were written on separate branches
 * and no translation unit included both until src/ask/ask_llama.c did, which
 * is when -Werror found they differ in TYPE (0.01 here, 1e-2F there) while
 * agreeing in value. Both are guarded and tests/test_ask_batch.c asserts they
 * still agree: a constant living in two headers is a constant that will drift.
 */
#ifndef HYP_ASK_NORM_TOLERANCE
#define HYP_ASK_NORM_TOLERANCE 0.01
#endif

/* ── The backend vtable ──────────────────────────────────────────────
 *
 * One struct, installed once at startup, read-only afterwards. Three
 * implementations are expected and all three fit without a `#ifdef` reaching
 * above this header:
 *
 *   - none          — nothing installed. `ask` reports unavailable with the
 *                     NO_BACKEND remedy. This is today's shipped state.
 *   - llama.cpp     — T2's decision. Installs from its own init.
 *   - a test double — tests install a deterministic fake and exercise every
 *                     line of ranking and shaping with no model on disk.
 *
 * Every function pointer may be NULL except `encode_query`; a backend that
 * cannot encode a query is not a backend. */
typedef struct {
    /* Stable identity of the weights AND their configuration. Two models'
     * vectors are not comparable and nothing downstream can detect the mix,
     * so this string is what `ask` refuses on. Must not be NULL. */
    const char *model_id;

    /* Emitted dimension. Must equal HYP_ASK_DIM; `hyp_ask_backend_install`
     * rejects anything else rather than let a 768-dim table be searched with
     * a 1024-dim question. */
    int dim;

    /* The model's context window in tokens (32768 for Qwen3-Embedding-0.6B).
     * The truncation counter is denominated in THIS number, not in the model
     * card's `model_max_length`. A C port that sets n_ctx lower will make the
     * counter fire on corpora where ctxengine's reads zero — that is the
     * counter working, not a regression. */
    int window_tokens;

    /* Encode one natural-language question BEHIND the instruct prefix for
     * `lang`, batch of one. Writes HYP_ASK_DIM unit-normalised float32s to
     * `out`. Returns 0 on success, non-zero on failure (and must leave a
     * caller-facing sentence in `err`, which is never NULL).
     *
     * Rendering the prefix is the callee's job, not the caller's: that is how
     * "the prefix is applied to queries only" stops being a convention that
     * every call site has to remember. */
    int (*encode_query)(HYPLanguage lang, const char *text, float *out, char *err, size_t errlen);

    /* Encode `n` documents BARE — no prefix, no language, no assembly.
     * Writes n * HYP_ASK_DIM unit-normalised float32s to `out`, row i for
     * text i, in the ORDER GIVEN. An implementation is free to length-sort
     * internally for throughput, but must scatter back: `build` pairs row i
     * with span i positionally, and a permutation that escaped here would
     * attach every citation in the lane to the wrong code.
     *
     * NULL when the backend cannot index (query-only). Then `ask` can still
     * search an index someone else built. */
    int (*encode_documents)(const char *const *texts, int n, float *out, char *err, size_t errlen);

    /* Whether `text` exceeds `window_tokens` — the recall ceiling, reportable.
     * NULL means "this encoder cannot say", which is a FIRST-CLASS third
     * state (see hyp_ask_trunc_t) and must not collapse into "none".
     * Counted on the FULL token length, never on the truncated one. */
    int (*truncates)(const char *text, bool *out, char *err, size_t errlen);
} hyp_ask_backend_t;

/* Install the process-wide backend. Call once, from startup, before any
 * request is served; the pointer must outlive the process.
 *
 * Returns 0 on success, HYP_NOT_FOUND when `b` is malformed (NULL, no
 * model_id, no encode_query, or dim != HYP_ASK_DIM). Passing NULL uninstalls,
 * which is what the tests use to get back to the shipped unavailable state.
 *
 * NOT thread-safe against concurrent `hyp_ask_backend()`. It is an
 * initialisation call, not a runtime switch. */
int hyp_ask_backend_install(const hyp_ask_backend_t *b);

/* The installed backend, or NULL when none is. NULL is the whole of the
 * NO_BACKEND unavailable state — there is no other way for it to arise. */
const hyp_ask_backend_t *hyp_ask_backend(void);

#endif /* HYP_ASK_EMBED_H */
