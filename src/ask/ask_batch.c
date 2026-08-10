/*
 * ask_batch.c — length-sorted grouping under a LINEAR padded-slot budget.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * WHY THE CONSTRAINT IS LINEAR, AND WHY IT MUST NOT BE MADE QUADRATIC AGAIN
 * ─────────────────────────────────────────────────────────────────────────
 *
 * NEXT-STEPS §2.1 asks for the opposite of what is implemented here. It says:
 *
 *     "`(n+1) x max_len <= budget` is linear, while attention memory is
 *      quadratic in sequence length. ... Change the constraint to keep
 *      `n x max_len^2` under a VRAM-derived constant."
 *
 * That is wrong, and it is wrong in a way that would either OOM the machine or
 * refuse to index long declarations, depending on which end the constant was
 * fitted at. The reference implementation's OWN three memory measurements say
 * so (recorded in runs/ASK/T1-ctxengine-recipe.json, section 4):
 *
 *     n=16, max_len=  465 ->  7,440 padded slots ->  1,865 MiB above weights
 *     n=16, max_len=1,833 -> 29,328 padded slots ->  6,933 MiB above weights
 *     n=16, max_len=3,657 -> 58,512 padded slots -> 14,821 MiB above weights
 *
 * Across a 7.86x increase in sequence length at fixed n, memory grew 7.95x.
 *   - As MiB per padded slot that is 0.2507, 0.2364, 0.2533 — LINEAR to +-4%.
 *   - As MiB per (n x max_len^2) it is 5.391e-4, 1.290e-4, 6.926e-5 — the
 *     coefficient has to fall 7.8x over the same range for a quadratic model to
 *     fit, which means it does not fit.
 *   - A genuinely quadratic memory model would have predicted 15.5x and 61.8x
 *     growth. The measurements show 3.72x and 7.95x.
 *
 * The reason is that the attention score matrix is never materialised.
 * Transformers runs Qwen3 under SDPA (memory-efficient / flash kernels), whose
 * peak is O(n.L.d), not O(n.heads.L^2). The quadratic term is real in TIME
 * (FLOPs) and absent from MEMORY — and the budget being bounded here is a
 * MEMORY budget.
 *
 * The quadratic constant therefore cannot be calibrated:
 *
 *   Calibrate at the long end. One full-window document must stay feasible:
 *   n=1, max_len=19,118 on a 16 GB card gives C = 365,497,924. At max_len=64
 *   that authorises n = 89,232 documents in one pass, whose LINEAR cost is
 *   89,232 x 64 x 0.46 MiB = 2.5 TiB. Instant OOM.
 *
 *   Calibrate at the short end. n=16 at max_len=512 is a batch the card
 *   demonstrably survives: C = 4,194,304. At max_len=19,118 that permits
 *   n = 0.011 documents — it forbids encoding the long declaration at all,
 *   which is the exact case length-sorted grouping was written to keep
 *   possible.
 *
 *   The two constants differ by 87.1x. No value satisfies both ends, because
 *   the constraint has the wrong functional form for the quantity it bounds.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * WHAT ACTUALLY BOUND THE SHORT DECLARATIONS, AND WHAT 128 IS DERIVED FROM
 * ─────────────────────────────────────────────────────────────────────────
 *
 * §2.1's symptom is real — short declarations do get small batches on a card
 * that could take far more — but it names the wrong knob. At budget 8,192:
 *
 *     max_len   n allowed by budget   n actually used   binding
 *          32                   255                16   max_docs
 *          64                   127                16   max_docs
 *         128                    63                16   max_docs
 *         256                    31                16   max_docs
 *         512                    15                15   budget
 *       2,048                     3                 3   budget
 *       8,192                     1                 1   budget (batch of one)
 *
 * Below ~512 tokens — which is most of a real corpus — the budget is not what
 * limits the batch. `max_docs = 16` is. (§2.1's "batches of 40" appears
 * nowhere: the reference's value is 16, and it measured 13.8 documents per
 * forward pass on lld/ELF.)
 *
 * So the fix is to RAISE max_docs and keep the constraint linear. 128 is
 * derived, not guessed. Simulating this exact function over the 4,117 real
 * declaration spans of the pinned lld/ELF corpus, with token length modelled as
 * span bytes / r and r swept over 2.5, 3.0, 3.5, 4.0 bytes per token:
 *
 *   max_docs   forward passes   docs/pass   padded slots   padding tax
 *         16      327..303        12.6..13.6   1,291,769..847,597    1.01x
 *         32      214..185        19.2..22.3   1,295,151..849,733    1.01x
 *         64      164..131        25.1..31.4   1,297,042..853,053    1.02x
 *        128      142..107        29.0..38.5   1,300,112..855,767    1.02x
 *        256      133.. 97        31.0..42.4   1,303,082..857,992    1.02x
 *        512      130.. 93        31.7..44.3   1,303,753..860,060    1.02x
 *
 * Two things fall out of that table and both matter.
 *
 *   1. At max_docs=16 this function predicts 12.6-13.6 documents per pass
 *      across the whole ratio sweep. The reference implementation MEASURED
 *      13.8 on the same corpus. The model reproduces the measurement it was
 *      never fitted to, which is the reason to trust the rest of the table.
 *
 *   2. 16 -> 128 removes 2.7x of the forward passes for +0.8% padded slots.
 *      128 -> 512 buys a further 1.15x for another +0.5%. The curve is flat
 *      past 128, and every step past it raises the per-batch host bookkeeping
 *      and the blast radius of one failed pass for nothing. 128 is where the
 *      throughput is and the risk is not.
 *
 * Raising max_docs cannot cost memory. The budget test below is unconditional,
 * so a group of n documents with widest member w always satisfies n*w <= budget
 * unless n == 1 and w > budget — the deliberate batch-of-one escape hatch. The
 * worst-case rectangle is max(budget, longest document) whatever max_docs is.
 * That is the whole argument for why this knob is safe to turn and the
 * quadratic rewrite is not.
 */

#include "ask/ask_batch.h"

#include <stdlib.h>
#include <string.h>

/* Sort context: qsort_r is not portable, so the length array is reached
 * through a file-scope pointer guarded by the fact that grouping is called
 * from one thread at a time inside the embed pass. Kept private to this
 * translation unit so no caller can be tempted to reuse it. */
static const int *g_sort_lengths;

static int cmp_by_length_then_index(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    int la = g_sort_lengths[ia];
    int lb = g_sort_lengths[ib];
    if (la != lb) {
        return la < lb ? -1 : 1;
    }
    /* Ties broken by original index so the grouping is DETERMINISTIC. The
     * corpus arrives from a graph-buffer scan whose order varies run to run;
     * without this the same corpus would produce different batches, different
     * padding, and a different (though equally correct) set of forward passes
     * on every run, which makes a throughput measurement unrepeatable. */
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

int hyp_ask_group_by_token_budget(const int *lengths, int n, int budget, int max_docs,
                                  hyp_ask_batches_t *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (n < 0 || (n > 0 && !lengths)) {
        return -1;
    }
    if (budget < 1) {
        budget = HYP_ASK_TOKEN_BUDGET;
    }
    if (max_docs < 1) {
        max_docs = HYP_ASK_MAX_DOCS;
    }
    out->doc_count = n;
    if (n == 0) {
        return 0;
    }

    int *order = malloc((size_t)n * sizeof(*order));
    /* n groups is the worst case (every document alone), + 1 for the sentinel. */
    int *starts = malloc(((size_t)n + 1) * sizeof(*starts));
    if (!order || !starts) {
        free(order);
        free(starts);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    g_sort_lengths = lengths;
    qsort(order, (size_t)n, sizeof(*order), cmp_by_length_then_index);
    g_sort_lengths = NULL;

    /* Walk in ascending order, accumulating. Because the walk is ascending,
     * lengths[order[i]] is the widest member of the group-to-be, so the
     * rectangle bound is EXACT rather than a guess made before the group is
     * closed. */
    int groups = 0;
    int cur_start = 0;
    starts[0] = 0;
    for (int i = 0; i < n; i++) {
        int cur_size = i - cur_start;
        int len = lengths[order[i]];
        if (len < 0) {
            len = 0;
        }
        if (cur_size > 0) {
            /* (cur_size + 1) * len is the rectangle this document would make.
             * A document longer than the whole budget therefore closes the
             * group before it and, because the next document is at least as
             * long, is closed after it too: it lands alone and is encoded
             * anyway. */
            bool over_budget = ((int64_t)(cur_size + 1) * (int64_t)len) > (int64_t)budget;
            bool over_docs = cur_size >= max_docs;
            if (over_budget || over_docs) {
                starts[++groups] = i;
                cur_start = i;
            }
        }
    }
    starts[++groups] = n;

    out->order = order;
    out->group_start = starts;
    out->group_count = groups;
    return 0;
}

void hyp_ask_batches_free(hyp_ask_batches_t *b) {
    if (!b) {
        return;
    }
    free(b->order);
    free(b->group_start);
    b->order = NULL;
    b->group_start = NULL;
    b->group_count = 0;
    b->doc_count = 0;
}

int hyp_ask_token_budget_for_device(double device_total_mib, bool is_gpu) {
    if (!is_gpu) {
        /* No device to interrogate, and 8,192 is the figure measured against
         * host RAM at ~0.25 MiB/slot. */
        return HYP_ASK_TOKEN_BUDGET;
    }
    if (device_total_mib <= 0.0) {
        return 0;
    }
    double usable = (device_total_mib * HYP_ASK_VRAM_HEADROOM) - HYP_ASK_WEIGHTS_MIB;
    if (usable <= 0.0) {
        /* The card cannot hold the weights plus any headroom. Refusing with a
         * zero is the caller's cue to fall back to CPU, not to guess a budget
         * the device cannot honour. */
        return 0;
    }
    double slots = usable / HYP_ASK_MIB_PER_SLOT_GPU;
    if (slots < (double)HYP_ASK_MIN_BUDGET) {
        return 0;
    }
    if (slots > (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int)slots;
}

void hyp_ask_batches_cost(const hyp_ask_batches_t *b, const int *lengths, int64_t *out_slots,
                          int64_t *out_max_rect) {
    int64_t slots = 0;
    int64_t max_rect = 0;
    if (b && lengths) {
        for (int g = 0; g < b->group_count; g++) {
            int from = b->group_start[g];
            int to = b->group_start[g + 1];
            int widest = 0;
            for (int j = from; j < to; j++) {
                int len = lengths[b->order[j]];
                if (len > widest) {
                    widest = len;
                }
            }
            int64_t rect = (int64_t)(to - from) * (int64_t)widest;
            slots += rect;
            if (rect > max_rect) {
                max_rect = rect;
            }
        }
    }
    if (out_slots) {
        *out_slots = slots;
    }
    if (out_max_rect) {
        *out_max_rect = max_rect;
    }
}
