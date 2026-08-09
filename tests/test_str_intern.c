/*
 * test_str_intern.c — RED phase tests for foundation/str_intern.
 */
#include "test_framework.h"
#include "../src/foundation/str_intern.h"

TEST(intern_create_free) {
    HYPInternPool *pool = hyp_intern_create();
    ASSERT_NOT_NULL(pool);
    ASSERT_EQ(hyp_intern_count(pool), 0);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_basic) {
    HYPInternPool *pool = hyp_intern_create();
    const char *s1 = hyp_intern(pool, "hello");
    ASSERT_NOT_NULL(s1);
    ASSERT_STR_EQ(s1, "hello");
    ASSERT_EQ(hyp_intern_count(pool), 1);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_dedup) {
    HYPInternPool *pool = hyp_intern_create();
    const char *s1 = hyp_intern(pool, "hello");
    const char *s2 = hyp_intern(pool, "hello");
    /* Must return same pointer */
    ASSERT_EQ((uintptr_t)s1, (uintptr_t)s2);
    ASSERT_EQ(hyp_intern_count(pool), 1); /* still 1 */
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_different_strings) {
    HYPInternPool *pool = hyp_intern_create();
    const char *s1 = hyp_intern(pool, "hello");
    const char *s2 = hyp_intern(pool, "world");
    ASSERT_NEQ((uintptr_t)s1, (uintptr_t)s2);
    ASSERT_STR_EQ(s1, "hello");
    ASSERT_STR_EQ(s2, "world");
    ASSERT_EQ(hyp_intern_count(pool), 2);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_n_with_length) {
    HYPInternPool *pool = hyp_intern_create();
    const char *s1 = hyp_intern_n(pool, "hello world", 5);
    ASSERT_STR_EQ(s1, "hello");
    /* Should dedup with full "hello" */
    const char *s2 = hyp_intern(pool, "hello");
    ASSERT_EQ((uintptr_t)s1, (uintptr_t)s2);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_empty_string) {
    HYPInternPool *pool = hyp_intern_create();
    const char *s = hyp_intern(pool, "");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "");
    ASSERT_EQ(hyp_intern_count(pool), 1);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_many_strings) {
    HYPInternPool *pool = hyp_intern_create();
    char buf[64];
    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "string_%04d", i);
        const char *s = hyp_intern(pool, buf);
        ASSERT_NOT_NULL(s);
    }
    ASSERT_EQ(hyp_intern_count(pool), 1000);

    /* Verify dedup — intern same strings again */
    for (int i = 0; i < 1000; i++) {
        snprintf(buf, sizeof(buf), "string_%04d", i);
        hyp_intern(pool, buf);
    }
    ASSERT_EQ(hyp_intern_count(pool), 1000); /* still 1000 */
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_bytes) {
    HYPInternPool *pool = hyp_intern_create();
    hyp_intern(pool, "abc");   /* 3 bytes + NUL */
    hyp_intern(pool, "defgh"); /* 5 bytes + NUL */
    /* bytes should be at least 8 (3+5 for the content) */
    ASSERT_GTE(hyp_intern_bytes(pool), 8);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_survives_stack_buffer) {
    HYPInternPool *pool = hyp_intern_create();
    const char *interned;
    {
        char buf[32];
        strcpy(buf, "stack_string");
        interned = hyp_intern(pool, buf);
    }
    /* buf is out of scope, but interned should still be valid */
    ASSERT_STR_EQ(interned, "stack_string");
    hyp_intern_free(pool);
    PASS();
}

/* ── Edge case tests ───────────────────────────────────────────── */

TEST(intern_null_returns_null) {
    HYPInternPool *pool = hyp_intern_create();
    const char *s = hyp_intern(pool, NULL);
    ASSERT_NULL(s);
    ASSERT_EQ(hyp_intern_count(pool), 0);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_n_zero_len) {
    /* intern_n with len=0 should produce an empty string */
    HYPInternPool *pool = hyp_intern_create();
    const char *s = hyp_intern_n(pool, "anything", 0);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "");
    ASSERT_EQ(hyp_intern_count(pool), 1);
    /* Should dedup with intern("") */
    const char *s2 = hyp_intern(pool, "");
    ASSERT_EQ((uintptr_t)s, (uintptr_t)s2);
    ASSERT_EQ(hyp_intern_count(pool), 1);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_very_long_string) {
    HYPInternPool *pool = hyp_intern_create();
    /* 2000 char string */
    char buf[2001];
    memset(buf, 'A', 2000);
    buf[2000] = '\0';
    const char *s1 = hyp_intern(pool, buf);
    ASSERT_NOT_NULL(s1);
    ASSERT_EQ(strlen(s1), 2000);
    ASSERT_STR_EQ(s1, buf);
    /* Dedup works for long strings too */
    const char *s2 = hyp_intern(pool, buf);
    ASSERT_EQ((uintptr_t)s1, (uintptr_t)s2);
    ASSERT_EQ(hyp_intern_count(pool), 1);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_same_prefix_different_lengths) {
    /* intern_n of same buffer with different lengths produces different strings */
    HYPInternPool *pool = hyp_intern_create();
    const char *src = "abcdefgh";
    const char *s3 = hyp_intern_n(pool, src, 3); /* "abc" */
    const char *s5 = hyp_intern_n(pool, src, 5); /* "abcde" */
    const char *s8 = hyp_intern_n(pool, src, 8); /* "abcdefgh" */
    ASSERT_STR_EQ(s3, "abc");
    ASSERT_STR_EQ(s5, "abcde");
    ASSERT_STR_EQ(s8, "abcdefgh");
    /* All different pointers */
    ASSERT_NEQ((uintptr_t)s3, (uintptr_t)s5);
    ASSERT_NEQ((uintptr_t)s5, (uintptr_t)s8);
    ASSERT_NEQ((uintptr_t)s3, (uintptr_t)s8);
    ASSERT_EQ(hyp_intern_count(pool), 3);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_pointer_stability) {
    /* Intern a string, then intern 10000 more, original pointer must survive */
    HYPInternPool *pool = hyp_intern_create();
    const char *original = hyp_intern(pool, "sentinel_value");
    ASSERT_NOT_NULL(original);

    /* Force many hash table resizes */
    char buf[64];
    for (int i = 0; i < 10000; i++) {
        snprintf(buf, sizeof(buf), "filler_%06d", i);
        hyp_intern(pool, buf);
    }
    ASSERT_EQ(hyp_intern_count(pool), 10001);

    /* Original pointer must still be valid and unchanged */
    ASSERT_STR_EQ(original, "sentinel_value");
    /* Re-interning must return the exact same pointer */
    const char *again = hyp_intern(pool, "sentinel_value");
    ASSERT_EQ((uintptr_t)original, (uintptr_t)again);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_two_pools_independent) {
    /* Two pools must return different pointers for the same string */
    HYPInternPool *p1 = hyp_intern_create();
    HYPInternPool *p2 = hyp_intern_create();
    const char *s1 = hyp_intern(p1, "shared");
    const char *s2 = hyp_intern(p2, "shared");
    ASSERT_STR_EQ(s1, "shared");
    ASSERT_STR_EQ(s2, "shared");
    /* Different pools own different copies — different pointers */
    ASSERT_NEQ((uintptr_t)s1, (uintptr_t)s2);
    hyp_intern_free(p1);
    hyp_intern_free(p2);
    PASS();
}

TEST(intern_free_empty_pool) {
    /* Free a pool with no strings interned — should not crash */
    HYPInternPool *pool = hyp_intern_create();
    ASSERT_EQ(hyp_intern_count(pool), 0);
    ASSERT_EQ(hyp_intern_bytes(pool), 0);
    hyp_intern_free(pool);
    PASS();
}

TEST(intern_bytes_tracking) {
    HYPInternPool *pool = hyp_intern_create();
    ASSERT_EQ(hyp_intern_bytes(pool), 0);

    hyp_intern(pool, "abc");   /* 3 bytes */
    ASSERT_EQ(hyp_intern_bytes(pool), 3);

    hyp_intern(pool, "defgh"); /* 5 bytes */
    ASSERT_EQ(hyp_intern_bytes(pool), 8);

    /* Duplicate should not add bytes */
    hyp_intern(pool, "abc");
    ASSERT_EQ(hyp_intern_bytes(pool), 8);

    hyp_intern(pool, "");      /* 0 bytes */
    ASSERT_EQ(hyp_intern_bytes(pool), 8);

    hyp_intern(pool, "x");     /* 1 byte */
    ASSERT_EQ(hyp_intern_bytes(pool), 9);

    hyp_intern_free(pool);
    PASS();
}

TEST(intern_free_null_pool) {
    /* Free NULL pool — should not crash */
    hyp_intern_free(NULL);
    PASS();
}

TEST(intern_count_null_pool) {
    ASSERT_EQ(hyp_intern_count(NULL), 0);
    PASS();
}

SUITE(str_intern) {
    RUN_TEST(intern_create_free);
    RUN_TEST(intern_basic);
    RUN_TEST(intern_dedup);
    RUN_TEST(intern_different_strings);
    RUN_TEST(intern_n_with_length);
    RUN_TEST(intern_empty_string);
    RUN_TEST(intern_many_strings);
    RUN_TEST(intern_bytes);
    RUN_TEST(intern_survives_stack_buffer);
    /* Edge cases */
    RUN_TEST(intern_null_returns_null);
    RUN_TEST(intern_n_zero_len);
    RUN_TEST(intern_very_long_string);
    RUN_TEST(intern_same_prefix_different_lengths);
    RUN_TEST(intern_pointer_stability);
    RUN_TEST(intern_two_pools_independent);
    RUN_TEST(intern_free_empty_pool);
    RUN_TEST(intern_bytes_tracking);
    RUN_TEST(intern_free_null_pool);
    RUN_TEST(intern_count_null_pool);
}
