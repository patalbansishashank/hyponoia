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
 * AND WHY THE DOCUMENT CAP GOES DOWN TO 8, NOT UP TO THE HUNDREDS
 * ─────────────────────────────────────────────────────────────────────────
 *
 * §2.1 is wrong about this twice over: wrong about the MECHANISM (memory is
 * linear, see above) and wrong about the DIRECTION. It asks for "batches of
 * hundreds"; the measurement says single digits, and that sixteen — the
 * reference implementation's own value — is already past the peak.
 *
 * The symptom §2.1 describes is real. At budget 8,192 the cap, not the budget,
 * is what closes a batch of short declarations:
 *
 *     max_len   n allowed by budget   n allowed by a cap of 16   binding
 *          32                   255                         16   cap
 *          64                   127                         16   cap
 *         128                    63                         16   cap
 *         256                    31                         16   cap
 *         512                    15                         15   budget
 *       2,048                     3                          3   budget
 *       8,192                     1                          1   budget (alone)
 *
 * (§2.1's "batches of 40" appears nowhere. The reference's cap is 16 and it
 * measured 13.8 documents per forward pass on lld/ELF. This function, run over
 * the 4,117 real declaration spans of that corpus at a cap of 16, produces 13.5
 * — it reproduces a measurement it was never fitted to, which is the reason to
 * trust the rest of its arithmetic.)
 *
 * But "the cap binds" does not imply "raise it". Track 2 measured the actual
 * backend — llama.cpp with Qwen3-Embedding-0.6B, encoding WHOLE lld/ELF
 * declarations on a Radeon RX 6900 XT:
 *
 *     configuration          docs/s      peak VRAM
 *     CPU, 16 threads          1.581      —
 *     GPU, n_seq =  1         17.18       2,849 MB
 *     GPU, n_seq =  8         24.09       3,891 MB     <- peak
 *     GPU, n_seq = 16         16.37       6,110 MB
 *
 * Sixteen sequences is SLOWER THAN ONE, at 2.1x the VRAM. Batching buys +40%
 * over a single sequence and then reverses. The ceiling is not what binds:
 * n_seq = 64 ran cleanly at 3,678 MB. Throughput binds.
 *
 * WHY, and this is the part that must not be lost, because someone will come
 * back with §2's numbers and "fix" it: §2's Track F found saturation at batch
 * 512 and it was measuring a DIFFERENT WORKLOAD. That pipeline encoded
 * SINGLE TOKENS — 40,856 one-token forward passes — where each pass is
 * launch-bound and you need hundreds of them to fill the GPU. A declaration is
 * hundreds of tokens. One declaration already fills a launch. Every sequence
 * added after that buys padding waste, not parallelism, and pays for it in
 * activation memory and in the widest member setting the rectangle for its
 * neighbours. The two numbers are not in conflict; they are about two
 * different things, and reading §2's 512 into this lane makes it slower while
 * tripling its memory.
 *
 * So: 8. Measured peak, on this corpus, on this card, at this model. It is a
 * corpus-shaped number rather than a law — a corpus of one-line declarations
 * would push it up, and re-measuring is the only way to move it.
 *
 * What the cap CANNOT do, in either direction, is change the memory ceiling.
 * The budget test below is unconditional, so a group of n documents with widest
 * member w always satisfies n*w <= budget unless n == 1 and w > budget — the
 * deliberate batch-of-one escape hatch. The worst-case rectangle is
 * max(budget, longest document) whatever the cap is. That is why turning this
 * knob is safe and why the quadratic rewrite is not.
 *
 * One more thing the measurement settles, for the pass that calls this: the
 * in-binary lane is CPU-only, and CPU is 1.581 docs/s. lld/ELF's 4,117
 * declarations are ~43 minutes there against ~3 minutes on the GPU. The
 * grouping rule is not what makes that difference and cannot fix it; the
 * command says so up front rather than letting a user discover it at minute
 * forty.
 */

#include "ask/ask_batch.h"

#include <stdio.h>
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

/* ── The KV cache ──────────────────────────────────────────────── */

/* ── The model the planner is sizing for ────────────────────────────
 *
 * Every memory constant in ask_batch.h was measured against
 * Qwen3-Embedding-0.6B — 28 layers, 639 MB of Q8_0 weights — across §2's
 * tracks 2, 4, 8 and 9. They are exact for THAT model and silently wrong for
 * any other, and being wrong here is not a crash: the planner simply believes
 * every batch costs more than it does, refuses shapes the card can hold, and
 * narrows n_seq. The lane then works perfectly and slowly.
 *
 * voyage-4-nano is 12 layers and ~355 MB, so the shipped constants over-reserve
 * KV by 2.33x and weights by 425 MiB. Measured cost of that: 1.46 documents per
 * llama_decode where the grouping intended up to 8.
 *
 * So the two terms that MOVE with the model are read from the model. The
 * defaults are the Qwen3 figures, so a caller that never sets them behaves
 * exactly as before. */
static double g_kv_mib_per_token = HYP_ASK_KV_MIB_PER_TOKEN;
static double g_weights_mib = HYP_ASK_WEIGHTS_RESIDENT_MIB;

void hyp_ask_kv_set_model_shape(int n_layer, double weights_mib) {
    /* Per-layer KV width is n_head_kv * head_dim * 2 (K and V) and is 1024 for
     * BOTH models measured so far — Qwen3-Embedding-0.6B and voyage-4-nano are
     * each 8 KV heads of 128. It is held constant here rather than derived,
     * because llama.cpp exposes n_head_kv but not head_dim, and n_embd/n_head
     * gives 64 for nano against its true 128 — a derivation that would
     * UNDER-reserve, which is the failure that reset the desktop twice in §2.
     *
     * CHECK IT when adding a model: the GGUF header's attention.head_count_kv
     * times attention.key_length. If it is not 1024, this needs a real
     * derivation and not a constant. */
    if (n_layer > 0) {
        g_kv_mib_per_token = (double)n_layer * 2.0 * 1024.0 * 2.0 / 1048576.0;
    }
    if (weights_mib > 0.0) {
        g_weights_mib = weights_mib;
    }
}

double hyp_ask_kv_mib_per_token(void) {
    return g_kv_mib_per_token;
}

double hyp_ask_weights_mib(void) {
    return g_weights_mib;
}

bool hyp_ask_kv_plan(int n_seq_max, int seq_len, double ceiling_mib, hyp_ask_kv_plan_t *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->n_seq_max = n_seq_max;
    out->seq_len = seq_len;
    out->ceiling_mib = ceiling_mib;
    if (n_seq_max < 1 || seq_len < 1 || ceiling_mib <= 0.0) {
        return false;
    }
    /* n_ctx = n_seq_max * seq_len. NOT n_batch * n_seq_max — see the long note
     * in ask_batch.h. Computed in int64 because the wrong version of this
     * expression overflowed into a 57 GB request. */
    out->n_ctx = (int64_t)n_seq_max * (int64_t)seq_len;
    out->kv_mib = (double)out->n_ctx * g_kv_mib_per_token;
    /* The fixed term is weights + llama.cpp's structures. Taking the weights
     * from the loaded model rather than from a Qwen3-shaped constant is worth
     * 425 MiB of ceiling on nano, which is 425 MiB of batch. */
    out->total_mib = out->kv_mib + (HYP_ASK_LLAMA_FIXED_MIB - HYP_ASK_WEIGHTS_RESIDENT_MIB) +
                     g_weights_mib;
    out->admissible = out->total_mib <= ceiling_mib;
    return out->admissible;
}

double hyp_ask_compute_mib_for_ubatch(int n_ubatch) {
    return (double)n_ubatch * HYP_ASK_COMPUTE_MIB_PER_UBATCH;
}

int hyp_ask_ubatch_for(int n_batch, double compute_budget_mib) {
    if (n_batch < 1) {
        return 1;
    }
    int ub = 1;
    while (ub * 2 <= n_batch && ub * 2 <= HYP_ASK_UBATCH_MAX) {
        ub *= 2;
    }
    /* Walk down until the compute buffer fits. Stop at 1 rather than at 0:
     * refusing to encode is the caller's decision, made against the KV
     * pre-flight, not something the micro-batch sizer gets to take. */
    while (ub > 1 && hyp_ask_compute_mib_for_ubatch(ub) > compute_budget_mib) {
        ub /= 2;
    }
    if (ub > n_batch) {
        ub = n_batch;
    }
    return ub;
}

int hyp_ask_seq_len_for_kv(int n_seq_max, double ceiling_mib) {
    if (n_seq_max < 1 || ceiling_mib <= HYP_ASK_LLAMA_FIXED_MIB) {
        return 0;
    }
    double kv_budget = ceiling_mib - HYP_ASK_LLAMA_FIXED_MIB;
    double tokens = kv_budget / HYP_ASK_KV_MIB_PER_TOKEN;
    double per_seq = tokens / (double)n_seq_max;
    if (per_seq < 1.0) {
        return 0;
    }
    if (per_seq > (double)HYP_ASK_MODEL_WINDOW) {
        /* No point allocating KV for more context than the model has. */
        return HYP_ASK_MODEL_WINDOW;
    }
    return (int)per_seq;
}

bool hyp_ask_device_vram_mib(double *out_total_mib, double *out_used_mib) {
    if (out_total_mib) {
        *out_total_mib = 0.0;
    }
    if (out_used_mib) {
        *out_used_mib = 0.0;
    }
    /* amdgpu (and amdkfd) expose these; nothing else does. Absence is not an
     * error, it is "this platform cannot be interrogated", and the caller must
     * fall back to the CPU rather than guess a ceiling for a card it cannot
     * see. Cards are probed in order because the compute GPU is not reliably
     * card0 — on this machine it is card2. */
    for (int card = 0; card < 10; card++) {
        char path[128];
        double total = 0.0;
        double used = 0.0;
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/mem_info_vram_total", card);
        FILE *fp = fopen(path, "re");
        if (!fp) {
            continue;
        }
        unsigned long long raw = 0;
        int got = fscanf(fp, "%llu", &raw);
        (void)fclose(fp);
        if (got != 1 || raw == 0) {
            continue;
        }
        total = (double)raw / 1048576.0;
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/mem_info_vram_used", card);
        fp = fopen(path, "re");
        if (fp) {
            unsigned long long used_raw = 0;
            if (fscanf(fp, "%llu", &used_raw) == 1) {
                used = (double)used_raw / 1048576.0;
            }
            (void)fclose(fp);
        }
        if (out_total_mib) {
            *out_total_mib = total;
        }
        if (out_used_mib) {
            *out_used_mib = used;
        }
        return true;
    }
    return false;
}

double hyp_ask_gpu_ceiling_mib(double total_mib, double used_mib) {
    /* A fraction of what is FREE, not of what the card has. The measured
     * desktop baseline on this card is 2,442-2,586 MB of 16,368, and planning
     * against the nominal total plans against memory the compositor is already
     * holding — which is how an allocation that "fits" takes the session down
     * instead of returning an error. */
    double free_mib = total_mib - used_mib;
    if (free_mib <= 0.0) {
        return 0.0;
    }
    return free_mib * HYP_ASK_VRAM_HEADROOM;
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
