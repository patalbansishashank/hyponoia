/*
 * ask_encoder.h — the narrow seam between the `ask` lane and whatever runs the
 * transformer (NEXT-STEPS §2.1).
 *
 * TWO ENTRY POINTS, NO FLAG. Qwen3-Embedding is ASYMMETRIC: a query is encoded
 * behind an instruct prefix and a document is not. The measured retrieval
 * numbers are numbers about that asymmetry. A single `encode(text, bool
 * is_query)` would make the commonest way to lose recall the shortest thing to
 * write, and it would lose it SILENTLY, because both mistakes still return unit
 * vectors of the right shape. The reference implementation enforces the split
 * structurally for exactly this reason and says so in a comment; this header
 * keeps that property. Do not add a third entry point that takes a flag.
 *
 * Everything below is an INTERFACE. The inference runtime behind it (llama.cpp
 * / GGUF, track 2) and the `ask` tool in front of it (track 3) are other
 * people's work. This header exists so the storage and the embed pass can be
 * built and tested end to end today against `hyp_ask_encoder_stub_create`,
 * and so the day the real backend lands nothing above this line changes.
 *
 * The contract the storage relies on:
 *   - vectors are float32, `dim` wide, L2-NORMALISED, and dim is 1024 for the
 *     model §2.1 names. NOT the int8/768 static-table format of §2 — that is a
 *     different store with a different model and the two must never mix.
 *   - a document is encoded from its VERBATIM SOURCE LINES, bare.
 *   - cosine == dot product, and only because both sides are unit vectors.
 */
#ifndef HYP_ASK_ENCODER_H
#define HYP_ASK_ENCODER_H

#include <stdbool.h>
#include <stddef.h>

/* Deviation from 1.0 tolerated in a stored row's L2 norm.
 *
 * SET FROM MEASUREMENT, after 1e-3 was set from intuition and was wrong.
 * Qwen3-Embedding-0.6B under normalisation does not return exactly-unit rows:
 * over 220 encodings of varied C++ declarations, comments, unicode and
 * near-empty strings the reference implementation measured worst deviation
 * 0.003819, p99 0.003529, median 0.001058. 1e-3 rejects about HALF of real
 * model output. 1e-2 sits ~2.6x above the worst measured deviation and ~50x
 * below a genuine normalisation failure (an unnormalised vector deviates by
 * ~0.5 or ~1.0), so two orders of magnitude separate the accepted band from the
 * failure band. That gap is what makes this a check rather than a coin flip.
 *
 * Assert it; never silently re-normalise on load. A row that fails this was
 * produced by a caller with a different convention, and quietly fixing it would
 * turn wrong scores into plausible ones. */
#define HYP_ASK_NORM_TOLERANCE 1e-2F

/* The dimension §2.1's model emits. Recorded per index rather than compiled in
 * as a hard assumption — a store whose dim does not match the live encoder is
 * REFUSED, not truncated. */
#define HYP_ASK_DIM_DEFAULT 1024

typedef struct hyp_ask_encoder hyp_ask_encoder_t;

typedef struct {
    /* Identifies the model AND its configuration. Two models' vectors are not
     * comparable and nothing downstream can detect the mix, so this string is
     * the gate that stops a mixed index from existing. */
    const char *(*model_id)(void *self);

    int (*dim)(void *self);

    /* The model's context window in tokens. The truncation counter is
     * denominated in THIS number, not in a constant — if a backend sets its
     * context below 32,768, the counter must compare against what the backend
     * actually did. A smaller window making the counter fire on a corpus where
     * a larger one read zero is the counter working, not a regression. */
    int (*window_tokens)(void *self);

    /* How many padded slots this document will occupy AFTER the window
     * truncates it. Budgeting against 89,985 tokens for a document the
     * transformer will see 32,768 of would reserve memory for work that never
     * happens. Returns < 0 if the encoder cannot say, in which case the caller
     * falls back to a byte-length estimate for grouping only. */
    int (*token_length)(void *self, const char *text);

    /* The document's FULL token length, untruncated — this is what detects
     * overflow, and it is deliberately a DIFFERENT question from
     * `token_length` above. Collapsing the two is a subtle off-by-
     * special-tokens error exactly at the boundary that matters.
     *
     * Returns < 0 for "cannot say". That is a first-class answer, not an
     * error: it is what produces the UNKNOWN truncation state, which is
     * disclosed as its own claim rather than folded into "none". */
    int (*full_token_length)(void *self, const char *text);

    /* Encode `count` documents BARE — no instruct prefix, nothing prepended,
     * appended, lowercased or trimmed. `out` is count*dim float32, row-major,
     * caller-allocated, each row L2-normalised. Returns 0 on success. */
    int (*encode_documents)(void *self, const char *const *texts, int count, float *out);

    /* Encode one query BEHIND the instruct prefix, batch of one. `out` is dim
     * float32, L2-normalised.
     *
     * `language_display` is the DISPLAY name, not the grammar id — `C++`, not
     * `cpp`. Rendered with the grammar id the prefix stops being byte-identical
     * to the one the recall was measured with and the number quietly no longer
     * applies. Building that map is track 5's job; this parameter is the seam
     * it plugs into. NULL means the backend uses its own default. */
    int (*encode_query)(void *self, const char *text, const char *language_display, float *out);

    void (*destroy)(void *self);
} hyp_ask_encoder_vt_t;

struct hyp_ask_encoder {
    const hyp_ask_encoder_vt_t *vt;
    void *self;
};

/* ── Thin forwarders, so call sites read as the two-entry-point API ── */

const char *hyp_ask_encoder_model_id(const hyp_ask_encoder_t *e);
int hyp_ask_encoder_dim(const hyp_ask_encoder_t *e);
int hyp_ask_encoder_window(const hyp_ask_encoder_t *e);
int hyp_ask_encoder_token_length(const hyp_ask_encoder_t *e, const char *text);
int hyp_ask_encoder_full_token_length(const hyp_ask_encoder_t *e, const char *text);
int hyp_ask_encode_documents(const hyp_ask_encoder_t *e, const char *const *texts, int count,
                             float *out);
int hyp_ask_encode_query(const hyp_ask_encoder_t *e, const char *text,
                         const char *language_display, float *out);
void hyp_ask_encoder_destroy(hyp_ask_encoder_t *e);

/* True when every row of `vecs` (count x dim, float32) is unit-normalised to
 * within HYP_ASK_NORM_TOLERANCE. `*out_bad_row` receives the first offending
 * row index when the check fails. */
bool hyp_ask_vectors_are_unit(const float *vecs, int count, int dim, int *out_bad_row);

/* ── The stub ──────────────────────────────────────────────────────
 *
 * A deterministic, weight-free encoder: it hashes the text and expands the hash
 * into a unit vector. It exists so the storage, the batching and the truncation
 * accounting can be built and tested end to end before the inference runtime
 * lands, and so the test suite never depends on a downloaded model.
 *
 * Its model_id begins with "stub/" ON PURPOSE. Provenance is a hard gate in the
 * vector store: an index built by the stub can never be silently reused by a
 * real model, and the CLI refuses to build a stub index unless it is asked for
 * explicitly. It produces vectors of the right shape and NO retrieval quality
 * whatsoever; any recall measured against it is a measurement of the harness.
 *
 * `window_tokens` and the token-length estimator are byte-based
 * (bytes / HYP_ASK_STUB_BYTES_PER_TOKEN), which is enough to exercise the
 * grouping rule and the truncation counter's three states. */
#define HYP_ASK_STUB_MODEL_ID "stub/deterministic-hash-v1"
#define HYP_ASK_STUB_BYTES_PER_TOKEN 4

/* `can_report_truncation` false makes full_token_length return "cannot say",
 * which is how the UNKNOWN truncation state is reachable from a test. */
hyp_ask_encoder_t *hyp_ask_encoder_stub_create(int dim, int window_tokens,
                                               bool can_report_truncation);

#endif /* HYP_ASK_ENCODER_H */
