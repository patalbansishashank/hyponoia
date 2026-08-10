/*
 * ask_rerank.h — the cross-encoder that re-reads `ask`'s top candidates.
 *
 * NEXT-STEPS.md §2.2 lever 2. The retrieval curve says every gold answer is
 * findable — recall@50 is 0.817 and recall@100 is 0.883 against recall@10's
 * 0.550 — so the work is not finding more, it is ordering what was found. A
 * bi-encoder cannot do that: it made the two vectors independently and never
 * saw the pair. A cross-encoder reads question and candidate IN THE SAME
 * FORWARD PASS, which is the whole of why it can beat the retrieval it reranks.
 *
 * ── The ceiling, which is not negotiable ──────────────────────────────
 *
 * RERANKING CANNOT BEAT THE RETRIEVAL IT RERANKS. Reranking the top 50 caps
 * recall@10 at 0.817 and the top 100 at 0.883, no matter how good the model is,
 * because a gold at dense rank 137 is not in the set being reordered. Every
 * number this file can move lives strictly inside that box. Anyone tempted to
 * tune N upward for recall should price it first: the cost is linear in N and
 * the ceiling gains 0.066 between 50 and 100.
 *
 * ── Why a causal LM and not llama.cpp's reranking path ────────────────
 *
 * llama.cpp HAS a reranking path — LLAMA_POOLING_TYPE_RANK — and the pinned
 * tree implements it for LLM_ARCH_QWEN3 specifically, last-token pooled, with a
 * softmax over the classifier head (llama-graph.cpp:3583..3626). It is
 * unreachable. That path needs a `cls.output` tensor holding two rows lifted
 * out of the lm_head, and the conversion that produces it landed upstream
 * AFTER every public Qwen3-Reranker GGUF was made: six were examined and not
 * one carries `cls.output`, a pooling type, or classifier labels. Converting
 * one ourselves would mean hosting weights nobody can verify against an
 * upstream digest.
 *
 * So this file computes the same function from the other side. llama.cpp's RANK
 * graph does softmax(cls_out @ h_last) where cls_out is [lm_head[yes],
 * lm_head[no]]; this runs the FULL lm_head and takes softmax over exactly those
 * two logits. Same two numbers, same order, from weights anyone can download.
 * It is also, exactly, what Qwen's own reference implementation does — the
 * reranker is a causal LM asked a yes/no question, not a regression head.
 *
 * ── What is fixed and must not drift ──────────────────────────────────
 *
 * The prompt is the model's, not ours. Qwen's reference wraps every pair in a
 * fixed system turn and a fixed assistant preamble; llama.cpp's own converter
 * bakes the identical string into the GGUF as a chat template. Reword it and
 * the model is being asked a question it was not tuned on, and no number here
 * describes the result. The ONE part that is ours is the <Instruct> line, which
 * is templated by language for the same reason the query encoder's prefix is —
 * and the display name matters there for the same reason too ("C++", never
 * "cpp").
 *
 * ── The device, which is not the query encoder's answer ───────────────
 *
 * The query encoder runs on the CPU because one 22-token prefix plus a sentence
 * costs 37 ms there and it must not compete with an embed pass for VRAM. That
 * argument does not survive contact with this lane: reranking N candidates is N
 * whole declarations through a 0.6B model, which is 100–200x the tokens. On the
 * CPU that is tens of seconds. So this path is AUTO — GPU when the free-VRAM
 * ceiling admits it, CPU with a warning otherwise — and every answer reports
 * which one ran, because the difference is two orders of magnitude and a user
 * who silently got the slow one deserves to know why.
 */
#ifndef HYP_ASK_RERANK_H
#define HYP_ASK_RERANK_H

#include <stdbool.h>
#include <stddef.h>

#include "semantic/ask_lang.h"

enum {
    /* Candidates reranked when a caller asks for reranking without naming a
     * depth. 50 rather than 100: §2.2's own curve puts 49 of 60 golds inside
     * the top 50, the ceiling gain from 50 to 100 is 0.066, and the cost is
     * linear. Measured both ways — see runs/ASK/L2-reranker.json. */
    HYP_ASK_RERANK_DEFAULT_N = 50,

    /* Hard cap. Not a resource limit — a truth-in-advertising one: past this
     * the latency stops being interactive and the ceiling has almost stopped
     * moving. */
    HYP_ASK_RERANK_MAX_N = 200,

    /* Tokens one (prompt + question + declaration) pair may occupy. A pair
     * longer than this is head-truncated IN THE DOCUMENT ONLY — the system
     * turn, the instruct, the question and the assistant preamble are never
     * cut, because a prompt missing its own closing turn does not ask the model
     * anything.
     *
     * 2048 and not the model's 40,960 because KV is charged at 112 KiB/token
     * for the whole rectangle: 16 pairs at 2,048 is 3.5 GiB and 16 pairs at
     * 8,192 is 14 GiB, which is the entire card. Whole-file `Module` spans are
     * the only rows on the measured corpus that reach it. */
    HYP_ASK_RERANK_PAIR_WINDOW = 2048,

    /* Pairs per forward pass, before the VRAM plan narrows it further. */
    HYP_ASK_RERANK_MAX_PAIRS = 16,
};

/* Why a rerank did not happen. NONE of these is "the results are worse than
 * they look": a rerank that could not run leaves the dense ordering, which is a
 * real answer, and the caller is told which one they got. */
typedef enum {
    HYP_ASK_RERANK_OK = 0,
    HYP_ASK_RERANK_OFF,         /* not requested */
    HYP_ASK_RERANK_NO_BACKEND,  /* no inference runtime in this build */
    HYP_ASK_RERANK_NO_WEIGHTS,  /* the reranker GGUF is not on this machine */
    HYP_ASK_RERANK_FAILED,      /* tried, and did not work; see the message */
} hyp_ask_rerank_status_t;

/* True when this build has the inference runtime compiled in. */
bool hyp_ask_rerank_compiled_in(void);

/* Compiled in AND the weights are on disk. Cheap: a stat, not a hash. */
bool hyp_ask_rerank_available(void);

/* The identity of the weights, in the shape the index's model id uses. Never
 * NULL; "" when nothing is loaded. */
const char *hyp_ask_rerank_model_id(void);

/* What device the loaded reranker is on, once it has been loaded. Never NULL;
 * "" before the first call. */
const char *hyp_ask_rerank_device_note(void);

/* Score `n` (question, document) pairs. `out[i]` receives P("yes") in [0,1] for
 * pair i — higher is more relevant.
 *
 * `docs` are the SAME verbatim source spans the index embedded, so that what
 * the reranker reads is what the retriever retrieved.
 *
 * Loads ~639 MB on first call and keeps it. Returns HYP_ASK_RERANK_OK only when
 * every pair scored; on any other outcome `out` is untouched and `err` carries
 * a caller-facing sentence, so a partial rerank can never be mistaken for a
 * complete one. */
hyp_ask_rerank_status_t hyp_ask_rerank_score(const char *question, const char *language_display,
                                             const char *const *docs, int n, float *out,
                                             char *err, size_t errlen);

/* How many of the last call's pairs had their document cut at the pair window,
 * and how long the last call took. Reported on the answer rather than logged,
 * for the reason the truncation counter exists: a caveat nobody can see is not
 * a caveat. */
int hyp_ask_rerank_last_truncated(void);
double hyp_ask_rerank_last_ms(void);

/* Release the lazily-loaded model. Idempotent. */
void hyp_ask_rerank_shutdown(void);

#endif /* HYP_ASK_RERANK_H */
