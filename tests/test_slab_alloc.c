/*
 * test_slab_alloc.c — Unit tests for slab_alloc.c.
 *
 * Covers: slab malloc/free/calloc/realloc for ≤64B and >64B paths,
 * realloc grow/shrink, zeroing, ownership, thread reset.
 */
#include "test_framework.h"
#include <foundation/slab_alloc.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants from slab_alloc.c ─────────────────────────────────── */
enum { SLAB_CHUNK_SIZE = 64 };

/* ── Alloc → free (small, slab path) ─────────────────────────────── */

TEST(slab_malloc_free_small) {
    void *p = hyp_slab_test_malloc(32);
    ASSERT_NOT_NULL(p);
    memset(p, 0xAA, 32);
    hyp_slab_test_free(p);
    PASS();
}

TEST(slab_malloc_free_large) {
    void *p = hyp_slab_test_malloc(128);
    ASSERT_NOT_NULL(p);
    memset(p, 0xBB, 128);
    hyp_slab_test_free(p);
    PASS();
}

TEST(slab_malloc_zero_size) {
    /* malloc(0) should return non-NULL on most platforms */
    void *p = hyp_slab_test_malloc(0);
    ASSERT_NOT_NULL(p);
    hyp_slab_test_free(p);
    PASS();
}

/* ── Calloc ──────────────────────────────────────────────────────── */

TEST(slab_calloc_zeros) {
    void *p = hyp_slab_test_calloc(10, 8);
    ASSERT_NOT_NULL(p);
    unsigned char *b = p;
    for (int i = 0; i < 80; i++) {
        ASSERT_EQ(b[i], 0);
    }
    hyp_slab_test_free(p);
    PASS();
}

TEST(slab_calloc_overflows) {
    void *p = hyp_slab_test_calloc(SIZE_MAX, 2);
    ASSERT_NULL(p);
    PASS();
}

/* ── Realloc ─────────────────────────────────────────────────────── */

TEST(slab_realloc_null_is_malloc) {
    void *p = hyp_slab_test_realloc(NULL, 64);
    ASSERT_NOT_NULL(p);
    hyp_slab_test_free(p);
    PASS();
}

TEST(slab_realloc_shrink_to_zero) {
    void *p = hyp_slab_test_malloc(32);
    ASSERT_NOT_NULL(p);
    void *q = hyp_slab_test_realloc(p, 0); /* frees and returns NULL */
    ASSERT_NULL(q);
    PASS();
}

TEST(slab_realloc_small_keep_same) {
    void *p = hyp_slab_test_malloc(16);
    ASSERT_NOT_NULL(p);
    memset(p, 0xCC, 16);
    /* Realloc within slab size: same pointer, content preserved */
    void *q = hyp_slab_test_realloc(p, 32);
    ASSERT_EQ(p, q);
    unsigned char *b = q;
    ASSERT_EQ(b[0], 0xCC);
    hyp_slab_test_free(q);
    PASS();
}

TEST(slab_realloc_promote_to_heap) {
    void *p = hyp_slab_test_malloc(32);
    ASSERT_NOT_NULL(p);
    memset(p, 0xDD, 32);
    /* Realloc above slab threshold: new ptr, content copied */
    void *q = hyp_slab_test_realloc(p, 128);
    ASSERT_NOT_NULL(q);
    unsigned char *b = q;
    ASSERT_EQ(b[0], 0xDD);
    ASSERT_EQ(b[31], 0xDD);
    hyp_slab_test_free(q);
    PASS();
}

/* ── Multiple allocations (free list cycling) ────────────────────── */

TEST(slab_many_small_allocs) {
    void *ptrs[64];
    for (int i = 0; i < 64; i++) {
        ptrs[i] = hyp_slab_test_malloc(SLAB_CHUNK_SIZE);
        ASSERT_NOT_NULL(ptrs[i]);
        memset(ptrs[i], (int)(i & 0xFF), SLAB_CHUNK_SIZE);
    }
    /* Free all: they go back to free list */
    for (int i = 0; i < 64; i++) {
        hyp_slab_test_free(ptrs[i]);
    }
    /* Re-allocate from free list: same pointer pool recycled */
    void *again = hyp_slab_test_malloc(SLAB_CHUNK_SIZE);
    ASSERT_NOT_NULL(again);
    hyp_slab_test_free(again);
    PASS();
}

/* ── Distinct pointer addresses ──────────────────────────────────── */

TEST(slab_distinct_allocations) {
    void *a = hyp_slab_test_malloc(SLAB_CHUNK_SIZE);
    void *b = hyp_slab_test_malloc(SLAB_CHUNK_SIZE);
    void *c = hyp_slab_test_malloc(SLAB_CHUNK_SIZE);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);
    ASSERT_NEQ(a, b);
    ASSERT_NEQ(b, c);
    ASSERT_NEQ(a, c);
    hyp_slab_test_free(a);
    hyp_slab_test_free(b);
    hyp_slab_test_free(c);
    PASS();
}

/* ── Large allocation (heap path, >64B) ──────────────────────────── */

TEST(slab_large_alloc_free) {
    void *p = hyp_slab_test_malloc(1024);
    ASSERT_NOT_NULL(p);
    hyp_slab_test_free(p);
    PASS();
}

TEST(slab_large_calloc_zero) {
    void *p = hyp_slab_test_calloc(64, 16);
    ASSERT_NOT_NULL(p);
    unsigned char *b = p;
    ASSERT_EQ(b[0], 0);
    ASSERT_EQ(b[1023], 0);
    hyp_slab_test_free(p);
    PASS();
}

/* ── Reset thread ────────────────────────────────────────────────── */

TEST(slab_reset_thread) {
    /* Allocate then reset: should not crash */
    void *p = hyp_slab_test_malloc(32);
    ASSERT_NOT_NULL(p);
    hyp_slab_test_free(p);
    hyp_slab_reset_thread();
    /* After reset, new allocs should still work */
    void *q = hyp_slab_test_malloc(32);
    ASSERT_NOT_NULL(q);
    hyp_slab_test_free(q);
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(slab_alloc) {
    RUN_TEST(slab_malloc_free_small);
    RUN_TEST(slab_malloc_free_large);
    RUN_TEST(slab_malloc_zero_size);
    RUN_TEST(slab_calloc_zeros);
    RUN_TEST(slab_calloc_overflows);
    RUN_TEST(slab_realloc_null_is_malloc);
    RUN_TEST(slab_realloc_shrink_to_zero);
    RUN_TEST(slab_realloc_small_keep_same);
    RUN_TEST(slab_realloc_promote_to_heap);
    RUN_TEST(slab_many_small_allocs);
    RUN_TEST(slab_distinct_allocations);
    RUN_TEST(slab_large_alloc_free);
    RUN_TEST(slab_large_calloc_zero);
    RUN_TEST(slab_reset_thread);
}
