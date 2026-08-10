/*
 * ask_index.h — the per-declaration vector index the `ask` lane searches.
 *
 * A SECOND, OPT-IN store, sharing nothing with the static token vectors in
 * `nodes.properties`. §2.1's seam: "only `search_graph`'s optional parameter
 * and `SEMANTICALLY_RELATED` read the token vectors. The other fourteen tools
 * have no opinion about how they are made." This header does not touch them,
 * and `ask` cannot perturb them because it never reads them.
 *
 * ── The three unavailable states, and why they are not one ──
 *
 * `ask` can be unusable for three different reasons with three different
 * remedies, and a caller who is told the wrong one wastes a round trip:
 *
 *   NO_BACKEND       no encoder is linked into this binary. Nothing to do at
 *                    the project level; a build or a model is missing.
 *   NO_INDEX         the encoder is here, the project has never been embedded.
 *                    Fixed by one command, on this project.
 *   MODEL_MISMATCH   vectors exist but were made by a different model, a
 *                    different dimension, or a different extractor. Fixed by
 *                    rebuilding — and REFUSED meanwhile, because two models'
 *                    vectors are not comparable and nothing downstream can
 *                    detect the mix.
 *
 * None of them is "zero results". Zero results reads as "nothing in your
 * codebase matches", which is the more damaging lie: it is a claim about the
 * code rather than about the tool, and an agent will act on it.
 *
 * ── On-disk shape (T4 owns the write half) ──
 *
 *   ask_vectors(project, node_id, vec BLOB, content_hash TEXT, truncated INT)
 *       vec is HYP_ASK_DIM little-endian float32s, unit-normalised on write.
 *       content_hash is sha256 of the embedded span text, first 32 hex chars:
 *       the incremental-rebuild key. truncated is per-row, not a total.
 *   ask_index_meta(project, model_id, dim, window_tokens, extraction,
 *                  watermark, trunc_state, trunc_count, n_rows, built_at)
 *       One row per project. `trunc_state` carries THREE values and the third
 *       one is load-bearing — see hyp_ask_trunc_t.
 *
 * Row i is a node_id, not a position: the positional pairing ctxengine has to
 * defend in a docstring is a foreign key here, and a row whose node is gone
 * cannot be returned at all rather than returned uncitable.
 *
 * Reads never create these tables. An absent table is the NO_INDEX answer,
 * and a read path that created it would turn "never built" into "built and
 * empty" — the same collapse the three states exist to prevent.
 */
#ifndef HYP_ASK_INDEX_H
#define HYP_ASK_INDEX_H

#include <stdbool.h>
#include <stdint.h>

#include "foundation/constants.h"
#include "semantic/ask_embed.h" /* HYP_ASK_DIM, HYP_ASK_MODEL_ID_MAX */
#include "store/store.h"

/* Table names, so the write half (T4) and the read half agree by symbol
 * rather than by string literal. */
#define HYP_ASK_VECTORS_TABLE "ask_vectors"
#define HYP_ASK_META_TABLE "ask_index_meta"

typedef enum {
    HYP_ASK_AVAILABLE = 0,  /* vectors present, model matches, searchable */
    HYP_ASK_NO_BACKEND,     /* no encoder linked into this binary */
    HYP_ASK_NO_INDEX,       /* this project has never been embedded */
    HYP_ASK_MODEL_MISMATCH, /* vectors exist, made by a different model/dim */
    /* The encoder IS linked but its weights are not on this machine. Fetched
     * on first use of this lane, 639 MB at Q8_0, SHA-256 verified against a
     * constant compiled into the binary (src/cli/model_fetch.h).
     *
     * Genuinely distinct from NO_BACKEND, which is about the BUILD and whose
     * remedy is a different binary; this one is one command away. And it is
     * checked BEFORE NO_INDEX on purpose: building the semantic index is what
     * needs the model, so telling someone with neither to build an index
     * first sends them round a loop they cannot leave. */
    HYP_ASK_NO_WEIGHTS,
} hyp_ask_avail_t;

/* Truncation has THREE states and collapsing the first two is how a graph
 * nobody built answers confidently. `UNKNOWN` is "the encoder could not say",
 * `NONE` is "attested that none were cut". They are disclosed differently on
 * every answer, not only when a truncated row ranks — a caveat that appeared
 * only when a truncated span ranked would be silent in exactly the case that
 * hurts, the one where truncation is WHY it did not. */
/* The three states are defined once, in semantic/ask_embed.h, which this
 * header already includes. They used to be declared here AND in
 * src/ask/ask_vectors.h — the latter as a STRUCT of the same name — and the
 * collision was invisible until one file needed both. */

typedef struct {
    hyp_ask_avail_t avail;
    hyp_ask_trunc_state_t trunc;
    int trunc_count;                       /* meaningful only when trunc == HYP_ASK_TRUNC_SOME */
    int n_vectors;                         /* rows for this project; 0 unless AVAILABLE */
    int dim;                               /* dimension recorded in the index, 0 when none */
    char model_id[HYP_ASK_MODEL_ID_MAX];   /* the index's model, "" when none */
    char backend_id[HYP_ASK_MODEL_ID_MAX]; /* the linked encoder's, "" when none */
} hyp_ask_status_t;

/* One ranked answer. Carries the SPAN, so `ask` can hand back
 * `file:start-end` in the shape `search_graph` uses — an agent that can read
 * one can read the other. `hyp_vector_result_t`, the static lane's row, has
 * no line numbers at all and structurally cannot. */
typedef struct {
    int64_t node_id;
    char *qualified_name;
    char *name;
    char *label;
    char *file_path;
    int start_line;
    int end_line;
    double score;   /* cosine == dot product; both sides are unit vectors */
    bool truncated; /* this row's span exceeded the model window when embedded */
} hyp_ask_hit_t;

/* Report whether `ask` can answer for `project`, and why not when it cannot.
 * Never fails: an unreadable store is NO_INDEX, because "cannot see an index"
 * and "there is no index" are the same claim from a caller's chair, and the
 * remedy is identical. `out` is fully populated in every case. */
void hyp_ask_index_status(hyp_store_t *s, const char *project, hyp_ask_status_t *out);

/* Exact top-`limit` by cosine against `qvec` (HYP_ASK_DIM unit-normalised
 * float32s). Brute force, no approximation: measured at 40.4 ms over 300k
 * vectors and 0.09 ms over 3k, and every ANN index would trade recall — the
 * one thing this lane exists to buy — for a speedup nothing needs yet.
 *
 * Returns HYP_STORE_OK with *out_count possibly 0 (a real "nothing scored"),
 * or HYP_STORE_ERR. Rows whose node has been deleted are dropped rather than
 * returned uncitable. Caller frees with hyp_ask_free_hits. */
int hyp_ask_index_search(hyp_store_t *s, const char *project, const float *qvec, int limit,
                         hyp_ask_hit_t **out, int *out_count);

void hyp_ask_free_hits(hyp_ask_hit_t *hits, int count);

#endif /* HYP_ASK_INDEX_H */
