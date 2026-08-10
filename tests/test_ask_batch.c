/*
 * test_ask_batch.c — the grouping rule for the `ask` embed pass.
 *
 * These tests own nothing but lists of integers, which is the whole reason the
 * rule lives in a weight-free module: it is the arithmetic that decides whether
 * the embed pass survives a real corpus, and it must be checkable without a
 * model on disk.
 */

#include "test_framework.h"

#include "ask/ask_batch.h"

/* BOTH sides of the lane define the norm tolerance, in two headers written on
 * two branches, and they agreed only by luck until src/ask/ask_llama.c became
 * the first translation unit to include both. Capture each header's value and
 * compare them, rather than trusting that two literals stay equal. The #undef
 * is what makes the second header's guarded definition take effect here — it
 * is the mechanism under test, exercised. */
#include "ask/ask_encoder.h"
static const double ASK_ENCODER_SIDE_TOLERANCE = HYP_ASK_NORM_TOLERANCE;
#undef HYP_ASK_NORM_TOLERANCE
#include "semantic/ask_embed.h"
static const double ASK_TOOL_SIDE_TOLERANCE = HYP_ASK_NORM_TOLERANCE;

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Every original index appears exactly once. A permutation that escaped the
 * grouping would attach every citation in the lane to the wrong code. */
static int is_permutation(const hyp_ask_batches_t *b, int n) {
    if (b->doc_count != n) {
        return 0;
    }
    char *seen = calloc((size_t)n, 1);
    if (!seen) {
        return 0;
    }
    int ok = 1;
    int total = b->group_start[b->group_count];
    if (total != n) {
        ok = 0;
    }
    for (int i = 0; ok && i < n; i++) {
        int idx = b->order[i];
        if (idx < 0 || idx >= n || seen[idx]) {
            ok = 0;
            break;
        }
        seen[idx] = 1;
    }
    free(seen);
    return ok;
}

/* No group's padded rectangle may exceed the budget, EXCEPT a lone document
 * that is itself longer than the budget. */
static int rectangles_respect_budget(const hyp_ask_batches_t *b, const int *lengths, int budget) {
    for (int g = 0; g < b->group_count; g++) {
        int from = b->group_start[g];
        int to = b->group_start[g + 1];
        int widest = 0;
        for (int j = from; j < to; j++) {
            if (lengths[b->order[j]] > widest) {
                widest = lengths[b->order[j]];
            }
        }
        long long rect = (long long)(to - from) * widest;
        if (rect > budget && (to - from) != 1) {
            return 0;
        }
    }
    return 1;
}

TEST(ask_batch_empty_is_zero_groups) {
    hyp_ask_batches_t b;
    ASSERT_EQ(hyp_ask_group_by_token_budget(NULL, 0, 8192, 128, &b), 0);
    ASSERT_EQ(b.group_count, 0);
    ASSERT_EQ(b.doc_count, 0);
    hyp_ask_batches_free(&b);
    PASS();
}

TEST(ask_batch_returns_a_permutation_of_original_indices) {
    int lengths[] = {900, 12, 4000, 12, 300, 1, 77, 8000, 64, 64};
    int n = (int)(sizeof(lengths) / sizeof(lengths[0]));
    hyp_ask_batches_t b;
    ASSERT_EQ(hyp_ask_group_by_token_budget(lengths, n, 8192, 128, &b), 0);
    ASSERT(is_permutation(&b, n));
    hyp_ask_batches_free(&b);
    PASS();
}

TEST(ask_batch_is_length_ascending) {
    int lengths[] = {900, 12, 4000, 12, 300, 1, 77};
    int n = (int)(sizeof(lengths) / sizeof(lengths[0]));
    hyp_ask_batches_t b;
    ASSERT_EQ(hyp_ask_group_by_token_budget(lengths, n, 8192, 128, &b), 0);
    for (int i = 1; i < n; i++) {
        ASSERT_LTE(lengths[b.order[i - 1]], lengths[b.order[i]]);
    }
    hyp_ask_batches_free(&b);
    PASS();
}

TEST(ask_batch_is_deterministic_under_input_order) {
    /* The corpus arrives from a scan whose order is not guaranteed. Two
     * orderings of the same multiset must produce the same batch shapes, or a
     * throughput measurement is not repeatable. */
    int a[] = {5, 5, 5, 5, 5, 5};
    int c[] = {5, 5, 5, 5, 5, 5};
    hyp_ask_batches_t ba;
    hyp_ask_batches_t bc;
    ASSERT_EQ(hyp_ask_group_by_token_budget(a, 6, 8192, 2, &ba), 0);
    ASSERT_EQ(hyp_ask_group_by_token_budget(c, 6, 8192, 2, &bc), 0);
    ASSERT_EQ(ba.group_count, bc.group_count);
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(ba.order[i], bc.order[i]);
    }
    hyp_ask_batches_free(&ba);
    hyp_ask_batches_free(&bc);
    PASS();
}

TEST(ask_batch_never_exceeds_the_budget_rectangle) {
    int lengths[512];
    for (int i = 0; i < 512; i++) {
        lengths[i] = 1 + (i * 37) % 3000;
    }
    hyp_ask_batches_t b;
    ASSERT_EQ(hyp_ask_group_by_token_budget(lengths, 512, 8192, 128, &b), 0);
    ASSERT(rectangles_respect_budget(&b, lengths, 8192));
    hyp_ask_batches_free(&b);
    PASS();
}

TEST(ask_batch_max_docs_caps_the_group) {
    /* Sixteen one-token documents fit the budget a thousand times over, so
     * only max_docs can be what closes the group. */
    int lengths[16];
    for (int i = 0; i < 16; i++) {
        lengths[i] = 1;
    }
    hyp_ask_batches_t b;
    ASSERT_EQ(hyp_ask_group_by_token_budget(lengths, 16, 8192, 4, &b), 0);
    ASSERT_EQ(b.group_count, 4);
    for (int g = 0; g < b.group_count; g++) {
        ASSERT_EQ(b.group_start[g + 1] - b.group_start[g], 4);
    }
    hyp_ask_batches_free(&b);
    PASS();
}

TEST(ask_batch_over_budget_document_lands_alone_and_is_not_refused) {
    /* The escape hatch. A document longer than the whole budget must still be
     * grouped — refusing it would mean refusing to index the declaration. */
    int lengths[] = {10, 10, 40000, 10};
    hyp_ask_batches_t b;
    ASSERT_EQ(hyp_ask_group_by_token_budget(lengths, 4, 8192, 128, &b), 0);
    ASSERT_EQ(b.doc_count, 4);
    int found_alone = 0;
    for (int g = 0; g < b.group_count; g++) {
        int from = b.group_start[g];
        int to = b.group_start[g + 1];
        if (to - from == 1 && lengths[b.order[from]] == 40000) {
            found_alone = 1;
        }
    }
    ASSERT(found_alone);
    ASSERT(is_permutation(&b, 4));
    hyp_ask_batches_free(&b);
    PASS();
}

TEST(ask_batch_the_document_cap_is_the_measured_one) {
    /* PIN. The cap is 8 because the backend was measured at 17.18 docs/s with
     * one sequence, 24.09 with eight and 16.37 with sixteen — sixteen is slower
     * than one. §2.1 asks for "hundreds" and §2's Track F found saturation at
     * 512, but Track F was encoding SINGLE TOKENS, where a pass is launch-bound
     * and needs hundreds to fill the card. A declaration is hundreds of tokens
     * and already fills a launch.
     *
     * If this assertion is being changed, the thing that licenses the change is
     * a new docs/s measurement on the real backend, not an argument. */
    ASSERT_EQ(HYP_ASK_MAX_DOCS, 8);
    ASSERT_EQ(HYP_ASK_TOKEN_BUDGET, 8192);
    PASS();
}

TEST(ask_batch_changing_max_docs_cannot_enlarge_the_worst_rectangle) {
    /* THE safety argument for turning this knob in either direction. The budget
     * test is unconditional, so however high max_docs goes, no batch's
     * rectangle grows beyond max(budget, longest document). If this ever fails,
     * the quadratic rewrite argument is back on the table. */
    int lengths[1000];
    for (int i = 0; i < 1000; i++) {
        lengths[i] = 1 + (i * 13) % 5000;
    }
    int caps[] = {16, 32, 64, 128, 256, 512, 4096};
    long long baseline_max = 0;
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
        hyp_ask_batches_t b;
        ASSERT_EQ(hyp_ask_group_by_token_budget(lengths, 1000, 8192, caps[c], &b), 0);
        int64_t slots = 0;
        int64_t max_rect = 0;
        hyp_ask_batches_cost(&b, lengths, &slots, &max_rect);
        ASSERT_LTE(max_rect, 8192);
        if (c == 0) {
            baseline_max = max_rect;
        }
        ASSERT_LTE(max_rect, baseline_max > 8192 ? baseline_max : 8192);
        ASSERT(rectangles_respect_budget(&b, lengths, 8192));
        hyp_ask_batches_free(&b);
    }
    PASS();
}

TEST(ask_batch_the_cap_governs_only_the_short_end) {
    /* The cap decides batch size only where the budget has slack. At 64 tokens
     * the budget would allow 127 documents, so the cap of 8 is what closes the
     * group; at 2,048 the budget closes it at 3 and the cap is irrelevant. That
     * split is why lowering the cap costs nothing on long declarations — the
     * ones that were already expensive — and why raising it would have changed
     * only the cheap end. */
    int shortdocs[800];
    for (int i = 0; i < 800; i++) {
        shortdocs[i] = 64;
    }
    hyp_ask_batches_t b;
    ASSERT_EQ(hyp_ask_group_by_token_budget(shortdocs, 800, 8192, HYP_ASK_MAX_DOCS, &b), 0);
    ASSERT_EQ(b.group_count, 100); /* 800 / 8 — the cap, not the budget */
    for (int g = 0; g < b.group_count; g++) {
        ASSERT_EQ(b.group_start[g + 1] - b.group_start[g], HYP_ASK_MAX_DOCS);
    }
    hyp_ask_batches_free(&b);

    int longdocs[9];
    for (int i = 0; i < 9; i++) {
        longdocs[i] = 2048;
    }
    ASSERT_EQ(hyp_ask_group_by_token_budget(longdocs, 9, 8192, HYP_ASK_MAX_DOCS, &b), 0);
    /* 3 x 2048 = 6,144 fits; a fourth would be 8,192... which is exactly the
     * budget, so 4 x 2048 <= 8192 holds and the group closes at 4. */
    ASSERT_EQ(b.group_count, 3);
    for (int g = 0; g < b.group_count; g++) {
        int size = b.group_start[g + 1] - b.group_start[g];
        ASSERT_LTE(size, HYP_ASK_MAX_DOCS); /* the budget bound first */
        ASSERT_LTE((long long)size * 2048, 8192);
    }
    hyp_ask_batches_free(&b);
    PASS();
}

TEST(ask_batch_padded_slots_do_not_move_with_the_cap) {
    /* Changing the cap redistributes the SAME documents across a different
     * number of rectangles. On uniform lengths the padded-slot total — which is
     * what memory and FLOPs scale with — is identical, so the cap is purely a
     * launch-count knob. That is what makes the docs/s measurement the only
     * thing that can decide it. */
    int lengths[2000];
    for (int i = 0; i < 2000; i++) {
        lengths[i] = 64;
    }
    int caps[] = {1, 8, 16, 64};
    int64_t first_slots = -1;
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
        hyp_ask_batches_t b;
        ASSERT_EQ(hyp_ask_group_by_token_budget(lengths, 2000, 8192, caps[c], &b), 0);
        int64_t slots = 0;
        int64_t rect = 0;
        hyp_ask_batches_cost(&b, lengths, &slots, &rect);
        if (first_slots < 0) {
            first_slots = slots;
        }
        ASSERT_EQ(slots, first_slots);
        ASSERT_LTE(rect, 8192);
        hyp_ask_batches_free(&b);
    }
    PASS();
}

TEST(ask_batch_device_budget_has_no_clamp_that_forbids_growth) {
    /* The reference implementation's own helper clamped the derived value with
     * min(TOKEN_BUDGET, ...), so it could only ever LOWER the budget on a small
     * card and never raise it on a large one — which is why it returned 8,192
     * unchanged on the 16 GB card it was written for, and bought nothing. */
    int cpu = hyp_ask_token_budget_for_device(0.0, false);
    ASSERT_EQ(cpu, HYP_ASK_TOKEN_BUDGET);

    /* 16,368 MiB as the device reports it, not a nominal 16 GB — torch reports
     * 16368 on this card and planning against 16384 plans against memory that
     * is not there. (16368 * 0.70 - 4300) / 0.46 = 15,559.99..., and this
     * FLOORS rather than rounds: a budget must never be larger than the
     * arithmetic that justified it. */
    int gpu16 = hyp_ask_token_budget_for_device(16368.0, true);
    ASSERT_EQ(gpu16, 15559);
    ASSERT_GT(gpu16, HYP_ASK_TOKEN_BUDGET);

    /* A bigger card must get a bigger budget — the property the clamp broke. */
    int gpu48 = hyp_ask_token_budget_for_device(49152.0, true);
    ASSERT_GT(gpu48, gpu16);

    /* A card that cannot hold the weights gets a refusal, not a guess. */
    ASSERT_EQ(hyp_ask_token_budget_for_device(4096.0, true), 0);
    PASS();
}

TEST(ask_batch_kv_arithmetic_matches_every_measured_configuration) {
    /* The KV cost of a token is arithmetic, not a fit: 28 layers x 2 (K,V) x
     * 1024 dim x 2 bytes = 112 KiB. Track 2 measured five configurations and
     * this reproduces all five exactly. If it ever stops doing so, either the
     * model changed shape or the cache dtype did, and the budget derived from
     * it is no longer safe. */
    struct {
        int n_seq_max;
        int seq_len;
        int64_t n_ctx;
        double kv_mib;
    } cases[] = {
        {1, 8192, 8192, 896.0},      {8, 2304, 18432, 2016.0}, {16, 2304, 36864, 4032.0},
        {32, 2304, 73728, 8064.0},   {16, 8192, 131072, 14336.0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hyp_ask_kv_plan_t p;
        /* A ceiling big enough that only the arithmetic is under test. */
        (void)hyp_ask_kv_plan(cases[i].n_seq_max, cases[i].seq_len, 1e9, &p);
        ASSERT_EQ((long long)p.n_ctx, (long long)cases[i].n_ctx);
        ASSERT(fabs(p.kv_mib - cases[i].kv_mib) < 0.5);
    }
    PASS();
}

TEST(ask_batch_kv_preflight_refuses_before_allocating) {
    /* The two configurations track 2's pre-flight REFUSED must still be
     * refused, and the one it ran must still be admitted. On a display GPU an
     * over-allocation is not an OOM error — amdgpu resets the device and the
     * user loses their session. This check is the difference and it costs one
     * multiplication. */
    hyp_ask_kv_plan_t p;
    const double ceiling = 5500.0; /* track 2's guardrail */

    ASSERT(hyp_ask_kv_plan(8, 2304, ceiling, &p));
    ASSERT(p.admissible);
    ASSERT_EQ((long long)p.n_ctx, 18432);

    /* 32 x 2304 -> 8,064 MiB of KV over a 5,500 MiB ceiling. */
    ASSERT(!hyp_ask_kv_plan(32, 2304, ceiling, &p));
    ASSERT(!p.admissible);
    ASSERT_EQ((long long)p.n_ctx, 73728);

    /* 16 x 8192 -> 14,336 MiB. */
    ASSERT(!hyp_ask_kv_plan(16, 8192, ceiling, &p));
    ASSERT(!p.admissible);

    /* THE LINE THAT COST TWO DESKTOP SESSIONS. n_ctx = n_batch * n_seq_max
     * with n_batch 8,192 and n_seq_max 64 is 524,288 tokens — ~57 GB. Whatever
     * else changes, this must never be admissible on a 16 GB card. */
    ASSERT(!hyp_ask_kv_plan(64, 8192, ceiling, &p));
    ASSERT_EQ((long long)p.n_ctx, 524288);
    ASSERT_GT((long long)p.kv_mib, 57000);
    PASS();
}

TEST(ask_batch_gpu_ceiling_is_taken_from_free_not_total) {
    /* The card also drives the display. Its measured baseline is ~2.5 GB of
     * 16,368, and planning against the nominal total plans against memory the
     * compositor already holds. */
    double busy = hyp_ask_gpu_ceiling_mib(16368.0, 2586.0);
    double idle = hyp_ask_gpu_ceiling_mib(16368.0, 0.0);
    ASSERT_LT(busy, idle);
    ASSERT(fabs(busy - (16368.0 - 2586.0) * HYP_ASK_VRAM_HEADROOM) < 0.5);
    /* A fully occupied card yields no ceiling at all, which is a refusal and
     * not a small number to squeeze into. */
    ASSERT(hyp_ask_gpu_ceiling_mib(16368.0, 16368.0) <= 0.0);

    /* And the per-sequence length that fits follows from it. */
    int seq = hyp_ask_seq_len_for_kv(HYP_ASK_MAX_DOCS, busy);
    ASSERT_GT(seq, 0);
    hyp_ask_kv_plan_t p;
    ASSERT(hyp_ask_kv_plan(HYP_ASK_MAX_DOCS, seq, busy, &p));
    ASSERT(p.admissible);
    /* One more token per sequence must NOT fit — the helper returns the
     * largest admissible length, not a comfortable one. */
    ASSERT(!hyp_ask_kv_plan(HYP_ASK_MAX_DOCS, seq + 64, busy, &p) || seq >= HYP_ASK_MODEL_WINDOW);

    /* A card too small for the backend's fixed cost refuses rather than
     * returning a tiny sequence length nobody can use. */
    ASSERT_EQ(hyp_ask_seq_len_for_kv(HYP_ASK_MAX_DOCS, 1000.0), 0);
    PASS();
}

/* ── n_ubatch, the second allocation knob ───────────────────────────
 *
 * Track 2's record names n_ctx and stops. Track 8 found the other edge: at
 * n_ctx 32,768 the SAME KV arithmetic ran at 4,540 MB with n_ubatch 512 and
 * was killed by the VRAM guard at 5,374 MB with n_ubatch 2,048. A pre-flight
 * that bounds only n_ctx says ADMISSIBLE right up until the driver disagrees.
 */
TEST(ask_batch_ubatch_reproduces_every_measured_outcome) {
    /* Track 8's three passes, in its own order. */
    ASSERT_EQ(hyp_ask_ubatch_for(8192, 2304), 4096);  /* P1: T8 ran 8192, clean */
    ASSERT_EQ(hyp_ask_ubatch_for(8192, 8192), 2048);  /* P2: T8 ran 2048, clean */
    ASSERT_EQ(hyp_ask_ubatch_for(32768, 32768), 512); /* P3: 2048 was KILLED */

    /* The rectangle, not the micro-batch alone, is what is bounded. */
    ASSERT(hyp_ask_ubatch_for(32768, 32768) * 32768 <= HYP_ASK_UBATCH_SLOT_LIMIT);
    ASSERT(hyp_ask_ubatch_for(8192, 8192) * 8192 <= HYP_ASK_UBATCH_SLOT_LIMIT);

    /* Powers of two only: llama.cpp chunks the batch and a ragged tail buys
     * nothing. */
    for (int seq = 128; seq <= 32768; seq *= 2) {
        int ub = hyp_ask_ubatch_for(8192, seq);
        ASSERT((ub & (ub - 1)) == 0);
        ASSERT(ub >= 1);
    }
    PASS();
}

TEST(ask_batch_ubatch_never_exceeds_the_batch_and_never_reaches_zero) {
    /* A single token must always be encodable, whatever shape it arrives in —
     * the alternative is a document that cannot be indexed at all, and the one
     * thing this lane must never do is drop a row. */
    ASSERT_EQ(hyp_ask_ubatch_for(1, HYP_ASK_MODEL_WINDOW), 1);
    ASSERT_EQ(hyp_ask_ubatch_for(0, 512), 1);
    ASSERT_EQ(hyp_ask_ubatch_for(300, 512), 256);
    ASSERT(hyp_ask_ubatch_for(64, 32768) <= 64);
    PASS();
}

/* Both sides of the lane carry the norm tolerance, in two headers written on
 * two branches. They agreed by luck until src/ask/ask_llama.c included both
 * and -Werror reported a redefinition. This asserts the agreement instead. */
TEST(ask_batch_norm_tolerance_agrees_across_both_seams) {
    ASSERT(fabs(ASK_ENCODER_SIDE_TOLERANCE - ASK_TOOL_SIDE_TOLERANCE) < 1e-12);
    /* And that the value is the MEASURED one: 1e-3 was set from intuition and
     * rejects about half of real model output (worst deviation over 220
     * encodings was 0.003819). */
    ASSERT(fabs(ASK_TOOL_SIDE_TOLERANCE - 0.01) < 1e-12);
    PASS();
}

SUITE(ask_batch) {
    RUN_TEST(ask_batch_empty_is_zero_groups);
    RUN_TEST(ask_batch_returns_a_permutation_of_original_indices);
    RUN_TEST(ask_batch_is_length_ascending);
    RUN_TEST(ask_batch_is_deterministic_under_input_order);
    RUN_TEST(ask_batch_never_exceeds_the_budget_rectangle);
    RUN_TEST(ask_batch_max_docs_caps_the_group);
    RUN_TEST(ask_batch_over_budget_document_lands_alone_and_is_not_refused);
    RUN_TEST(ask_batch_the_document_cap_is_the_measured_one);
    RUN_TEST(ask_batch_changing_max_docs_cannot_enlarge_the_worst_rectangle);
    RUN_TEST(ask_batch_the_cap_governs_only_the_short_end);
    RUN_TEST(ask_batch_padded_slots_do_not_move_with_the_cap);
    RUN_TEST(ask_batch_device_budget_has_no_clamp_that_forbids_growth);
    RUN_TEST(ask_batch_kv_arithmetic_matches_every_measured_configuration);
    RUN_TEST(ask_batch_kv_preflight_refuses_before_allocating);
    RUN_TEST(ask_batch_gpu_ceiling_is_taken_from_free_not_total);
    RUN_TEST(ask_batch_ubatch_reproduces_every_measured_outcome);
    RUN_TEST(ask_batch_ubatch_never_exceeds_the_batch_and_never_reaches_zero);
    RUN_TEST(ask_batch_norm_tolerance_agrees_across_both_seams);
}
