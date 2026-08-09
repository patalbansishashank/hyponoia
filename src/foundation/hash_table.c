/*
 * hash_table.c — HYPHashTable backed by Verstable.
 *
 * Public API in hash_table.h is unchanged. Internals are a Verstable
 * template instantiation (const char* → void*). Verstable is a 2024
 * open-addressing hash table using quadratic probing with metadata
 * stored separately from buckets (4-bit hash fragment + 11-bit
 * displacement + 1-bit in-home-bucket flag per uint16_t). Documented
 * in vendored/verstable/verstable.h.
 *
 * Why swap the prior Robin Hood implementation: cumulative profiling
 * showed hyp_ht_get is a hot path in resolve_file_calls's per-call
 * registry resolution. Verstable's 4-bit hash-fragment metadata
 * sidesteps most key comparisons during chain walks, which the prior
 * implementation could not.
 *
 * Lifetime: keys are BORROWED pointers (caller owns the strings).
 * Verstable's KEY_TY is const char*; the templated comparison +
 * hash use the standard vt_cmpr_string / vt_hash_string helpers.
 */
#include "foundation/constants.h"
#include "hash_table.h"
#include <stdlib.h>
#include <string.h>

/* Instantiate a Verstable map of (const char* → void*). The single
 * include below generates static inline functions named hyp_vt_init,
 * hyp_vt_cleanup, hyp_vt_get, hyp_vt_insert, etc., plus the hyp_vt
 * struct itself. */
#define NAME hyp_vt
#define KEY_TY const char *
#define VAL_TY void *
#define HASH_FN vt_hash_string
#define CMPR_FN vt_cmpr_string
#include "../../internal/hyp/vendored/verstable/verstable.h"

/* The opaque HYPHashTable struct holds the Verstable instance + a
 * count cache (Verstable's _size traversal is O(buckets) so we keep
 * our own atomic-free counter). */
struct HYPHashTable {
    hyp_vt vt;
};

HYPHashTable *hyp_ht_create(uint32_t initial_capacity) {
    HYPHashTable *ht = (HYPHashTable *)calloc(HYP_ALLOC_ONE, sizeof(*ht));
    if (!ht)
        return NULL;
    hyp_vt_init(&ht->vt);
    if (initial_capacity > 0) {
        /* Reserve enough buckets for the requested entries. Verstable
         * computes the minimum bucket count internally. */
        if (!hyp_vt_reserve(&ht->vt, (size_t)initial_capacity)) {
            hyp_vt_cleanup(&ht->vt);
            free(ht);
            return NULL;
        }
    }
    return ht;
}

void hyp_ht_free(HYPHashTable *ht) {
    if (!ht)
        return;
    hyp_vt_cleanup(&ht->vt);
    free(ht);
}

void *hyp_ht_set(HYPHashTable *ht, const char *key, void *value) {
    if (!ht || !key)
        return NULL;
    /* Capture previous value (if any) before overwriting.
     * Verstable's _insert overwrites silently and returns an iterator
     * to the (now updated) entry — we have to peek first to surface
     * the prior value to the caller (back-compat with our API). */
    void *prev = NULL;
    hyp_vt_itr itr = hyp_vt_get(&ht->vt, key);
    if (!hyp_vt_is_end(itr)) {
        prev = itr.data->val;
    }
    (void)hyp_vt_insert(&ht->vt, key, value);
    return prev;
}

void *hyp_ht_get(const HYPHashTable *ht, const char *key) {
    if (!ht || !key)
        return NULL;
    hyp_vt_itr itr = hyp_vt_get(&ht->vt, key);
    if (hyp_vt_is_end(itr))
        return NULL;
    return itr.data->val;
}

bool hyp_ht_has(const HYPHashTable *ht, const char *key) {
    if (!ht || !key)
        return false;
    hyp_vt_itr itr = hyp_vt_get(&ht->vt, key);
    return !hyp_vt_is_end(itr);
}

const char *hyp_ht_get_key(const HYPHashTable *ht, const char *key) {
    if (!ht || !key)
        return NULL;
    hyp_vt_itr itr = hyp_vt_get(&ht->vt, key);
    if (hyp_vt_is_end(itr))
        return NULL;
    return itr.data->key;
}

void *hyp_ht_delete(HYPHashTable *ht, const char *key) {
    if (!ht || !key)
        return NULL;
    hyp_vt_itr itr = hyp_vt_get(&ht->vt, key);
    if (hyp_vt_is_end(itr))
        return NULL;
    void *prev = itr.data->val;
    (void)hyp_vt_erase(&ht->vt, key);
    return prev;
}

uint32_t hyp_ht_count(const HYPHashTable *ht) {
    if (!ht)
        return 0;
    return (uint32_t)hyp_vt_size(&ht->vt);
}

void hyp_ht_foreach(const HYPHashTable *ht, hyp_ht_iter_fn fn, void *userdata) {
    if (!ht || !fn)
        return;
    for (hyp_vt_itr itr = hyp_vt_first(&ht->vt); !hyp_vt_is_end(itr); itr = hyp_vt_next(itr)) {
        fn(itr.data->key, itr.data->val, userdata);
    }
}

void hyp_ht_clear(HYPHashTable *ht) {
    if (!ht)
        return;
    hyp_vt_clear(&ht->vt);
}
