/*
 * arena.h — Bump allocator with block-based growth.
 *
 * All memory is freed at once via hyp_arena_destroy(). Individual frees are
 * not supported — this is by design for per-file extraction where all data
 * has the same lifetime.
 *
 * Restructured from internal/hyp/arena.h for the pure C rewrite.
 * New additions: hyp_arena_reset() for reuse without realloc.
 */
#ifndef HYP_ARENA_H
#define HYP_ARENA_H

#include <stddef.h>
#include <stdarg.h>

#define HYP_ARENA_MAX_BLOCKS 256
#define HYP_ARENA_DEFAULT_BLOCK_SIZE ((size_t)64 * 1024) /* 64KB */

typedef struct {
    char *blocks[HYP_ARENA_MAX_BLOCKS];
    size_t block_sizes[HYP_ARENA_MAX_BLOCKS]; /* per-block sizes (for stats) */
    int nblocks;
    size_t block_size;  /* current block capacity */
    size_t used;        /* bytes used in current block */
    size_t total_alloc; /* cumulative bytes allocated (for stats) */
} HYPArena;

/* Initialize arena with default block size. */
void hyp_arena_init(HYPArena *a);

/* Initialize arena with a custom initial block size. */
void hyp_arena_init_sized(HYPArena *a, size_t block_size);

/* Allocate n bytes (8-byte aligned). Returns NULL on OOM. */
void *hyp_arena_alloc(HYPArena *a, size_t n);

/* Allocate n bytes, zero-initialized. */
void *hyp_arena_calloc(HYPArena *a, size_t n);

/* Duplicate a NUL-terminated string. */
char *hyp_arena_strdup(HYPArena *a, const char *s);

/* Duplicate a string of known length, NUL-terminate. */
char *hyp_arena_strndup(HYPArena *a, const char *s, size_t len);

/* sprintf into arena memory. */
char *hyp_arena_sprintf(HYPArena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Reset arena for reuse: keeps first block, frees the rest. */
void hyp_arena_reset(HYPArena *a);

/* Free all blocks. Arena is zeroed after this. */
void hyp_arena_destroy(HYPArena *a);

/* Return total bytes allocated (for diagnostics). */
size_t hyp_arena_total(const HYPArena *a);

#endif /* HYP_ARENA_H */
