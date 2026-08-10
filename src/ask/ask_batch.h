/*
 * ask_batch.h — how documents are grouped into forward passes for the `ask`
 * embed pass (NEXT-STEPS §2.1, track 6).
 *
 * Weight-free and store-free ON PURPOSE. This is the arithmetic that decides
 * whether the embed pass survives a real corpus, and it should be checkable by
 * a test that owns nothing but a list of integers. Behind an encoder it would
 * only be reachable through a downloaded model, which is how a rule this
 * load-bearing ends up asserted by a code-read instead of by a test.
 */
#ifndef HYP_ASK_BATCH_H
#define HYP_ASK_BATCH_H

#include <stdbool.h>
#include <stdint.h>

/* ── The two knobs the grouping rule reads ─────────────────────────
 *
 * BUDGET — the ceiling on ONE forward pass, counted in PADDED TOKEN SLOTS
 * (batch size x the longest text in that batch), because that rectangle, not
 * the number of tokens that are real, is what the transformer allocates
 * activations for. 8,192 is ctxengine's measured value: it buys ~2 GiB of
 * activation on top of the weights at ~0.25 MiB/slot on host RAM.
 *
 * A batch measured in DOCUMENTS has no ceiling at all, and that is what killed
 * the reference implementation's first embed run: one generated declaration
 * that filled the model's whole 32,768-token window dragged fifteen neighbours
 * up to 32,768 with it, asking for 524,288 slots (~130 GiB) on a 62 GB machine.
 * Length-sorted grouping plus a slot ceiling is what removes that failure mode,
 * and the failure grew with the WORST document in the tree rather than with the
 * corpus, which is why a per-document cap cannot fix it.
 *
 * MAX_DOCS — the number of sequences in one forward pass. It is NOT a memory
 * bound: the budget check below is unconditional, so the padded rectangle can
 * never exceed BUDGET however high MAX_DOCS goes, and the backend was measured
 * running 64 sequences cleanly at 3,678 MB. It is a THROUGHPUT knob, and the
 * measurement says the useful range is single digits — see the long note in
 * ask_batch.c before changing it upward.
 */
enum {
    /* Padded-slot ceiling for one forward pass on CPU. */
    HYP_ASK_TOKEN_BUDGET = 8192,

    /* Sequences per forward pass. LOWERED from the reference implementation's
     * 16, not raised: the backend was measured at 17.18 docs/s at one sequence,
     * 24.09 at eight, and 16.37 at sixteen. Sixteen is slower than one. The
     * arithmetic and the reason are in ask_batch.c and are worth reading before
     * anyone "optimises" this number. */
    HYP_ASK_MAX_DOCS = 8,

    /* Qwen3-Embedding-0.6B's context window. The encoder reports its own; this
     * is the default the truncation counter is denominated in when it cannot. */
    HYP_ASK_MODEL_WINDOW = 32768,

    /* Floor and ceiling for a device-derived budget. */
    HYP_ASK_MIN_BUDGET = 1024,
};

/* Activation cost of one padded token slot, in MiB — DIFFERENT ON EACH DEVICE,
 * which is the mistake this pair of constants exists to stop being made once.
 * CPU: the straight line through the reference's three measured points
 * (1,865 MiB at 16x465, 6,933 at 16x1,833, 14,821 at 16x3,657) gives ~0.25
 * MiB/slot of host RAM. GPU: 8,192 slots peaked at 7.9 GiB total on a 16 GB
 * card; minus ~4.2 GiB of fp32 weights that is ~0.46 MiB/slot, nearly twice the
 * host figure. Using the CPU number to plan a GPU budget is how a budget that
 * "fits" runs out of memory anyway. */
#define HYP_ASK_MIB_PER_SLOT_CPU 0.25
#define HYP_ASK_MIB_PER_SLOT_GPU 0.46

/* What the fp32 weights occupy before a single token is encoded. */
#define HYP_ASK_WEIGHTS_MIB 4300.0

/* Fraction of a device's memory this is willing to plan against. The allocator
 * fragments, the driver reserves, and a desktop is usually also drawing a
 * screen on the same card. The reference's own full-corpus run logged two
 * caching-allocator OOM warnings at 8,192 slots and completed only because the
 * allocator freed and retried — the high-water mark fitted, the transient
 * allocations did not. 0.70 rather than the reference's 0.80 for that reason. */
#define HYP_ASK_VRAM_HEADROOM 0.70

/* Groups of ORIGINAL document indices, length-ascending.
 *
 * `order` is a permutation of 0..doc_count-1. Group g occupies
 * order[group_start[g] .. group_start[g+1]), and group_start has
 * group_count + 1 entries so the last group needs no special case.
 *
 * The indices are ORIGINAL and are never applied to the caller's texts here:
 * the caller reads texts[order[j]] and writes the resulting vector back to row
 * order[j]. A permutation that escaped this function would attach every
 * citation in the lane to the wrong code — the one failure this engine exists
 * to remove, arriving through a performance fix. */
typedef struct {
    int *order;
    int *group_start;
    int group_count;
    int doc_count;
} hyp_ask_batches_t;

/* Group `n` documents whose padded token lengths are `lengths`.
 *
 * Returns 0 on success and fills `out` (caller frees with
 * hyp_ask_batches_free), non-zero on allocation failure or bad arguments.
 * n == 0 is a success that produces zero groups.
 *
 * A document longer than the whole budget lands in a group of ONE and is
 * grouped anyway — never refused. Alone is the smallest batch there is, and
 * refusing it would mean refusing to index the document. Whether the model then
 * sees all of it is the truncation counter's business, not this function's. */
int hyp_ask_group_by_token_budget(const int *lengths, int n, int budget, int max_docs,
                                  hyp_ask_batches_t *out);

void hyp_ask_batches_free(hyp_ask_batches_t *b);

/* Padded-slot budget derived from a device's memory.
 *
 *   usable_MiB  = total_MiB * HYP_ASK_VRAM_HEADROOM - HYP_ASK_WEIGHTS_MIB
 *   budget_slots = floor(usable_MiB / MiB_per_slot)
 *
 * Returns HYP_ASK_TOKEN_BUDGET for the CPU (there is no device to interrogate,
 * and 8,192 is the measured host-RAM figure), 0 when a GPU cannot hold the
 * weights plus a minimum batch at all. Unlike the reference implementation's
 * `token_budget_for`, there is NO `min(TOKEN_BUDGET, ...)` clamp on the derived
 * branch: that clamp made the function able only ever to LOWER the budget on a
 * small card and never to raise it on a large one, which is why the reference's
 * copy returned 8,192 unchanged on a 16 GB card and bought no throughput at
 * all. It was also dead code — called by nothing. */
int hyp_ask_token_budget_for_device(double device_total_mib, bool is_gpu);

/* Total padded slots the grouping will push through the model, and the largest
 * single rectangle among them. Diagnostics for the pass report: `slots` is what
 * the run costs, `max_rect` is what one forward pass must fit in memory. */
void hyp_ask_batches_cost(const hyp_ask_batches_t *b, const int *lengths, int64_t *out_slots,
                          int64_t *out_max_rect);

#endif /* HYP_ASK_BATCH_H */
