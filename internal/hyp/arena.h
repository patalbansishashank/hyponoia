#ifndef HYP_ARENA_H
#define HYP_ARENA_H

#include <stddef.h>

// HYPArena is a simple bump allocator that allocates from fixed-size blocks.
// All memory is freed at once via hyp_arena_destroy(). Individual frees are not
// supported — this is by design for per-file extraction where all data has the
// same lifetime.
#define HYP_ARENA_MAX_BLOCKS 256
#define HYP_ARENA_DEFAULT_BLOCK_SIZE (64 * 1024) // 64KB initial

typedef struct {
    char *blocks[HYP_ARENA_MAX_BLOCKS];
    size_t block_sizes[HYP_ARENA_MAX_BLOCKS]; // per-block sizes (for stats)
    int nblocks;
    size_t block_size;
    size_t used;        // bytes used in current block
    size_t total_alloc; // cumulative bytes allocated (for stats)
} HYPArena;

// Initialize an arena with the default block size.
void hyp_arena_init(HYPArena *a);

// Allocate n bytes from the arena. Returns NULL on OOM or block exhaustion.
// All returned pointers are 8-byte aligned.
void *hyp_arena_alloc(HYPArena *a, size_t n);

// Duplicate a string into arena memory. Returns arena-owned copy.
char *hyp_arena_strdup(HYPArena *a, const char *s);

// Duplicate a string of known length into arena memory. NUL-terminates.
char *hyp_arena_strndup(HYPArena *a, const char *s, size_t len);

// sprintf into arena memory. Returns arena-owned string.
char *hyp_arena_sprintf(HYPArena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Free all blocks. Arena is invalid after this call.
void hyp_arena_destroy(HYPArena *a);

#endif // HYP_ARENA_H
