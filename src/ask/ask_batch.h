/*
 * ask_batch.h — how documents are grouped into forward passes for the `ask`
 * embed pass.
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
    /* The number is a property of the MODEL, not of this lane, so it is
     * re-measured whenever the model changes — a corpus-shaped number rather
     * than a law, and re-measuring is the only way to move it. Under
     * voyage-4-nano, on 4,072 declarations of lld/ELF:
     *
     *     max_docs   docs/s   forward passes
     *            8     29.3              520
     *           16     36.7              275     <- +25%
     *           24        —   ggml_can_mul_mat assert
     *
     * The Qwen3 optimum of 8 held because 16 cost it throughput (24.09 ->
     * 16.37). nano is 12 layers against 28 and half the KV per token, so a
     * wider batch fits where it does not for the larger model.
     *
     * THE "24 -> assert" ROW IS EASY TO MISREAD, and misreading it as a
     * ragged non-causal assert makes it look like a ceiling on this constant.
     * It is neither. It is n_batch (when fixed at 8,192) not
     * dividing by n_seq: 8192 % 16 = 0 runs, 8192 % 24 = 8 aborts, inside
     * llama.cpp's graph_reserve, before a single token — and it aborted just
     * the same at n_seq = 3 with three 2,408-token rows, which no value of
     * this constant could have prevented. n_batch is now the batch's own token
     * count and divisible by n_seq by construction (see the n_batch note in
     * ask_llama.c), so this number is once again ONLY a throughput knob and
     * "the last value that runs" is no longer a property it has. 16 stays
     * because it is the measured peak; re-measure before moving it. */
    HYP_ASK_MAX_DOCS = 16,

    /* voyage-4-nano's context window. The encoder reports its own; this is the
     * default the truncation counter is denominated in when it cannot. */
    HYP_ASK_MODEL_WINDOW = 32768,

    /* What a BIDIRECTIONAL model can actually be given in one go.
     *
     * Non-causal attention cannot be split across micro-batches — every token
     * attends to every other, so llama.cpp requires n_ubatch >= the whole
     * sequence. That turns the compute buffer from a tunable into a function of
     * the longest document: 8,192 tokens prices it at 5,284 MiB and fits inside
     * this card's 9,490 MiB ceiling; 32,768 asks for 21,135 MiB and aborts, on
     * the GPU and on CPU alike.
     *
     * So the weights support 32,768 and this lane can spend 8,192, and the
     * difference is DISCLOSED rather than hidden: model_window is clamped to
     * this, and the truncation counter is denominated in model_window. It is
     * also the window nano was measured at, so the shipped ceiling and the
     * measured one are the same number rather than two that happen to be
     * close. */
    HYP_ASK_NONCAUSAL_MAX_SEQ = 8192,

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
 * all. It was also dead code — called by nothing.
 *
 * NOTE: this is the PyTorch/SDPA activation model, kept because it is what the
 * recorded ctxengine numbers were derived under. For the llama.cpp backend the
 * binding quantity is the KV cache, not activations, and the KV arithmetic
 * below is exact rather than fitted. Prefer it. */
int hyp_ask_token_budget_for_device(double device_total_mib, bool is_gpu);

/* ── The KV cache, which is what actually bounds a llama.cpp batch ──
 *
 * READ THIS BEFORE SIZING ANYTHING. It cost two desktop sessions.
 *
 * llama.cpp's `n_ctx` is NOT a per-sequence context length. It is the TOTAL
 * capacity of the unified KV cache, and llama.cpp DIVIDES it by `n_seq_max` to
 * get the per-sequence limit. So the correct sizing is
 *
 *     n_ctx = n_seq_max * seq_len          (seq_len = the LONGEST DOCUMENT
 *                                           the batch will accept)
 *
 * and the line that looks right and is not is `n_ctx = n_batch * n_seq_max`.
 * With n_batch 8,192 and n_seq_max 64 that asks for 524,288 tokens of KV —
 * about 57 GB — on a 16 GB card that is also driving the compositor. amdgpu
 * does not return an error for that; it RESETS THE DEVICE, which tears down
 * every context including the desktop's. Track 2 lost the session twice before
 * the cause was found, and it was this line, not the backend being unsafe:
 * after the fix, nine runs across six configurations completed cleanly at peaks
 * up to 6,110 MB.
 *
 * The consequence for THIS module is the useful part: `n_seq_max * seq_len` is
 * exactly the padded rectangle that `hyp_ask_group_by_token_budget` already
 * bounds. The budget IS n_ctx. A grouping that respects the budget cannot ask
 * for a KV cache larger than the context was created with, which is the whole
 * safety property — provided the budget was derived from the KV arithmetic
 * rather than guessed.
 *
 * And KV is charged for the WHOLE rectangle whether the documents fill it or
 * not, which is why a wide batch of short documents is not free even though it
 * looks free in padded-slot terms.
 */

/* KV bytes for ONE token of Qwen3-Embedding-0.6B under llama.cpp:
 * 28 layers x 2 (K and V) x 1024 dim x 2 bytes (f16) = 114,688 = 112 KiB.
 *
 * Verified exactly against all five of track 2's measured configurations:
 * n_ctx 8,192 -> 896 MB; 18,432 -> 2,016; 36,864 -> 4,032; 73,728 -> 8,064;
 * 131,072 -> 14,336. Not a fitted constant — an arithmetic one. */
#define HYP_ASK_KV_BYTES_PER_TOKEN (28 * 2 * 1024 * 2)
#define HYP_ASK_KV_MIB_PER_TOKEN (HYP_ASK_KV_BYTES_PER_TOKEN / 1048576.0)

/* What the backend occupies before a single KV byte: Q8_0 weights (~639 MB)
 * plus llama.cpp's compute buffers. Track 2's peaks minus their KV give 1,953
 * MB at n_ctx 8,192, 1,875 at 18,432 and 2,078 at 36,864 — flat, as it should
 * be, since only the KV scales with n_ctx. 2,100 is the top of that range. */
#define HYP_ASK_LLAMA_FIXED_MIB 2100.0

typedef struct {
    int n_seq_max;      /* sequences per forward pass */
    int seq_len;        /* per-sequence token limit = n_ctx / n_seq_max */
    int64_t n_ctx;      /* what llama.cpp is asked to allocate */
    double kv_mib;      /* KV implied by n_ctx */
    double total_mib;   /* kv_mib + the fixed backend cost */
    double ceiling_mib; /* what it was checked against */
    bool admissible;    /* false = REFUSE, do not attempt */
} hyp_ask_kv_plan_t;

/* ── n_ubatch: the SECOND allocation knob ───────────────────────────
 *
 * `n_ctx` bounds the KV cache. It does NOT bound the COMPUTE buffer, and the
 * compute buffer is the larger of the two at every batch shape this lane
 * actually uses. Track 2's record names n_ctx and stops.
 *
 * Track 8 found the edge: at n_ctx 32,768 with n_ubatch 2,048 a run reached
 * 5,374 MB attributable and the VRAM guard killed it; the SAME KV arithmetic
 * at n_ubatch 512 completed clean at 4,540 MB.
 *
 * Track 9 measured the law inside the binary, from llama.cpp's own reported
 * buffer sizes on the Vulkan backend (Qwen3-Embedding-0.6B, Q8_0):
 *
 *     n_ubatch    Vulkan0 compute buffer     MiB per ubatch token
 *          512              330.24 MiB               0.645
 *        2,048            1,321.03 MiB               0.645
 *        4,096            2,581.92 MiB               0.630
 *        8,192            4,005.72 MiB               0.489
 *
 * It scales with n_ubatch and is essentially INDEPENDENT of n_ctx and of
 * n_seq_max — 2,048 costs 1,321 MiB whether the context is 24,576 tokens or
 * 32,768. That is why bounding the rectangle n_ubatch x seq_len (the first
 * shape this track tried) is wrong: it admits n_ubatch 8,192 at seq_len 2,048
 * and buys a 4 GB allocation nothing in the KV pre-flight can see.
 *
 * The law also reproduces track 8's kill/clean boundary from first principles.
 * At n_ctx 32,768 (KV 3,584 MiB) with ~700 MiB of weights, against its 5,500 MB
 * guard: n_ubatch 2,048 predicts 5,605 MB — over, and it was killed; n_ubatch
 * 512 predicts 4,614 MB — under, and it ran.
 */
#define HYP_ASK_COMPUTE_MIB_PER_UBATCH 0.645

/* What the weights and llama.cpp's fixed structures occupy before any KV or
 * compute buffer: 603.87 MiB of Q8_0 tensors on the device plus 157.37 MiB
 * mapped on the host, measured from load_tensors' own report. Rounded up.
 *
 * DELIBERATELY SEPARATE from HYP_ASK_LLAMA_FIXED_MIB below, which track 4
 * fitted by subtracting KV from track 2's peaks and therefore has the compute
 * buffer folded into it. That conflation is exactly what this pair separates:
 * one term that does not move, and one that moves with a knob. */
#define HYP_ASK_WEIGHTS_RESIDENT_MIB 780.0

/* Hard ceiling on n_ubatch regardless of how much memory is free. Above 2,048
 * the compute buffer passes 1.3 GiB for no measured throughput gain on
 * declaration-length text, and a batch of whole declarations already fills a
 * launch — the same reason n_seq saturates at 8 rather than at hundreds. */
enum { HYP_ASK_UBATCH_MAX = 2048 };

/* The largest admissible n_ubatch.
 *
 * `n_batch` is the tokens one decode may carry; `compute_budget_mib` is what is
 * left of the ceiling after the KV cache and the resident weights. Powers of
 * two only — llama.cpp splits a batch into ubatch-sized chunks and a ragged
 * final chunk buys nothing. Never returns more than n_batch and never less
 * than 1: a single token must always be encodable, whatever shape it arrives
 * in, because the one thing this lane must not do is drop a row. */
int hyp_ask_ubatch_for(int n_batch, double compute_budget_mib);

/* MiB the compute buffer will occupy at `n_ubatch`. */
double hyp_ask_compute_mib_for_ubatch(int n_ubatch);

/* ── The ONE tensor that decides where a pass runs ─────────────────
 *
 * The compute buffer above is a TOTAL, and the total is not what the Vulkan
 * backend checks. It checks every op's tensors one at a time
 * (ggml-vulkan.cpp ggml_backend_vk_device_supports_op, "reject any tensors
 * larger than the max buffer size") and any op with a single tensor above the
 * device's max buffer size is REFUSED by the device, so ggml_backend_sched
 * assigns it to the CPU backend — whose compute buffer llama-context.cpp
 * deliberately places in the device's HOST buffer type ("Vulkan_Host"). The
 * graph then runs its attention on the CPU with the rest of the graph on the
 * GPU and the activations crossing PCIe. Measured on an RX 6900 XT: 2 docs,
 * 5,266 tokens, 4,946 tok/s with a 3,685 MiB Vulkan0 compute buffer; 3 docs,
 * 7,904 tokens, 734 tok/s with 5,278 MiB in Vulkan_Host. Same graph, one
 * tensor over the line.
 *
 * With flash attention off — the encoder path materialises the scores — that
 * tensor is KQ = [n_kv, n_tokens, n_head] in f32, where n_tokens is the whole
 * ubatch (non-causal cannot split) and n_kv is n_ctx rounded up to llama.cpp's
 * cache granularity. It is QUADRATIC in the pass length, which is why the
 * linear compute law above cannot see the cliff. This is that tensor's size. */

/* llama.cpp rounds n_ctx up to a multiple of this before sizing the cache
 * (llama-context.cpp: `cparams.n_ctx = GGML_PAD(cparams.n_ctx, 256)`), and the
 * reserved KQ has n_kv = that padded n_ctx. Its constant, quoted, not ours. */
enum { HYP_ASK_LLAMA_CTX_PAD = 256 };

/* Bytes of the f32 attention-scores tensor a pass of `n_seq` sequences at
 * `seq_len` reserves under a model with `n_head` query heads, when the context
 * is built as this backend builds it (n_ubatch = n_batch = n_ctx = n_seq x
 * seq_len). Zero for a degenerate shape. */
uint64_t hyp_ask_attn_scores_bytes(int n_seq, int seq_len, int n_head);

/* The most sequences of `seq_len` (never more than `n_seq`, never fewer than
 * 1) whose scores tensor stays within `max_tensor_bytes` — the device's own
 * per-buffer ceiling as ggml reports it, not a constant. A cap of 0 means
 * "unknown, do not narrow" and returns n_seq unchanged. One sequence is always
 * admitted: alone is the smallest pass there is, and a document that is over
 * the line by itself is still encoded, just slowly. */
int hyp_ask_max_seq_for_tensor_cap(int n_seq, int seq_len, int n_head, uint64_t max_tensor_bytes);

/* Price a (n_seq_max, seq_len) pair against a VRAM ceiling.
 *
 * REFUSES rather than attempts. On a machine where the compute GPU is also the
 * display GPU — the common case for the developers this tool targets — an
 * over-allocation is not an OOM error, it is the user losing their session.
 * A pre-flight check that costs one multiplication is the difference. */
bool hyp_ask_kv_plan(int n_seq_max, int seq_len, double ceiling_mib, hyp_ask_kv_plan_t *out);

/* Tell the planner which model it is sizing for.
 *
 * THE CONSTANTS ABOVE ARE QWEN3-EMBEDDING-0.6B'S. They were measured exactly,
 * for that model, and they are silently wrong for any other — not as a crash
 * but as a slowdown: the planner over-charges every batch, refuses shapes the
 * device can hold, and narrows n_seq until documents stop sharing a forward
 * pass. On voyage-4-nano (12 layers against 28, ~355 MB against 639) the
 * shipped defaults over-reserve KV by 2.33x and weights by 425 MiB, and cost
 * 1.46 documents per llama_decode where the grouping intended up to 8.
 *
 * Call it once, after the model loads. Not calling it leaves the Qwen3
 * defaults, which is exactly the previous behaviour. */
void hyp_ask_kv_set_model_shape(int n_layer, double weights_mib);
double hyp_ask_kv_mib_per_token(void);
double hyp_ask_weights_mib(void);

/* The largest per-sequence length that fits `ceiling_mib` at `n_seq_max`.
 * 0 when even one token does not fit, which is the caller's cue to fall back to
 * the CPU rather than to try a smaller batch. */
int hyp_ask_seq_len_for_kv(int n_seq_max, double ceiling_mib);

/* Read the device's real VRAM, in MiB, from sysfs — the files
 * mem_info_vram_total and mem_info_vram_used under
 * /sys/class/drm/cardN/device.
 *
 * CHECKED, NOT ASSUMED, and `used` matters as much as `total`: this card also
 * drives the display and its baseline is ~2.5 GB of the 16 GB. Planning against
 * the nominal total plans against memory somebody else is already holding.
 * Returns false when no amdgpu-style device exposes the files — every other
 * platform, in which case the caller must fall back to the CPU rather than
 * guess a ceiling. */
bool hyp_ask_device_vram_mib(double *out_total_mib, double *out_used_mib);

/* Ceiling to plan a GPU pass against, from live VRAM:
 *     (total - used) * HYP_ASK_VRAM_HEADROOM
 * i.e. a fraction of what is FREE right now, not of what the card has. */
double hyp_ask_gpu_ceiling_mib(double total_mib, double used_mib);

/* Total padded slots the grouping will push through the model, and the largest
 * single rectangle among them. Diagnostics for the pass report: `slots` is what
 * the run costs, `max_rect` is what one forward pass must fit in memory. */
void hyp_ask_batches_cost(const hyp_ask_batches_t *b, const int *lengths, int64_t *out_slots,
                          int64_t *out_max_rect);

#endif /* HYP_ASK_BATCH_H */
