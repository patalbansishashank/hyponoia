/*
 * str_intern.h — String interning pool.
 *
 * Deduplicates strings: identical strings share a single allocation.
 * Returns stable pointers — safe to compare by pointer equality after interning.
 *
 * Uses an arena for string storage (bulk free) + hash table for dedup lookup.
 */
#ifndef HYP_STR_INTERN_H
#define HYP_STR_INTERN_H

#include <stddef.h>
#include <stdint.h>

typedef struct HYPInternPool HYPInternPool;

/* Create a new intern pool. */
HYPInternPool *hyp_intern_create(void);

/* Free the pool and all interned strings. */
void hyp_intern_free(HYPInternPool *pool);

/* Intern a NUL-terminated string. Returns a stable pointer.
 * The same input always returns the same pointer. */
const char *hyp_intern(HYPInternPool *pool, const char *s);

/* Intern a string of known length. */
const char *hyp_intern_n(HYPInternPool *pool, const char *s, size_t len);

/* Number of unique strings in the pool. */
uint32_t hyp_intern_count(const HYPInternPool *pool);

/* Total bytes stored (unique strings only). */
size_t hyp_intern_bytes(const HYPInternPool *pool);

#endif /* HYP_STR_INTERN_H */
