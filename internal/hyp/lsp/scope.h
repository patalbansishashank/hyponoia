#ifndef HYP_LSP_SCOPE_H
#define HYP_LSP_SCOPE_H

#include "type_rep.h"
#include "../arena.h"
#include <stdatomic.h> /* relaxed cache for hyp_lsp_max_walk_depth */
#include <stdlib.h>     /* getenv, atoi (hyp_lsp_max_walk_depth) */

typedef struct {
    const char* name;
    const HYPType* type;
    /* Exact callable value carried by this lexical binding, or NULL when the
     * binding is not proven to denote one callable.  This is deliberately
     * identity metadata rather than another HYPType kind: aliases need both
     * their ordinary type and the graph QN of the value they reference. */
    const char *callable_qn;
} HYPVarBinding;

#define HYP_SCOPE_CHUNK_BINDINGS 16

typedef struct HYPScopeChunk {
    HYPVarBinding bindings[HYP_SCOPE_CHUNK_BINDINGS];
    int used;
    struct HYPScopeChunk* next;
} HYPScopeChunk;

typedef struct HYPScope {
    struct HYPScope* parent;
    HYPScopeChunk* chunks;
    HYPArena* arena;        // owning arena, propagated to children at push time
} HYPScope;

// Bail-to-UNKNOWN depth for type-lookup chains: alias resolution, MRO walks,
// embedded-field/struct-traversal. Exceeding this collapses to hyp_type_unknown
// rather than recursing — guards against pathological hierarchies.
#define HYP_LSP_MAX_LOOKUP_DEPTH 16

// Recursion cap for the per-language "resolve calls in AST node" walkers. These
// recurse once per AST nesting level; a deeply-nested or cyclic file can drive
// them into a native stack overflow (SIGSEGV) that takes down the whole index.
// Past this cap the wrapper skips the subtree — those calls stay unresolved,
// which is graceful degradation, not a crash. 512 is far deeper than any
// hand-written source nests; override for pathological/generated repos via the
// HYP_LSP_MAX_WALK_DEPTH env var (positive integer).
#define HYP_LSP_MAX_WALK_DEPTH 512

// Resolved walk-depth cap: env override (HYP_LSP_MAX_WALK_DEPTH, if a positive
// integer) else HYP_LSP_MAX_WALK_DEPTH. Read once and cached — the walkers call
// this per node, so it must not hit getenv on the hot path. The cache is
// idempotent under multi-threaded indexing (every worker computes the same
// value), but a plain data race is undefined behavior even when the values
// agree, so the slot is a relaxed atomic: on the hot path this is a plain load
// with no fence, and a first-touch double-compute simply stores the same
// value. This keeps the parallel extractor TSan-clean.
static inline int hyp_lsp_max_walk_depth(void) {
    static _Atomic int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (value < 0) {
        const char* e = getenv("HYP_LSP_MAX_WALK_DEPTH");
        int v = (e && *e) ? atoi(e) : 0;
        value = (v > 0) ? v : HYP_LSP_MAX_WALK_DEPTH;
        atomic_store_explicit(&cached, value, memory_order_relaxed);
    }
    return value;
}

HYPScope* hyp_scope_push(HYPArena* a, HYPScope* current);
HYPScope* hyp_scope_pop(HYPScope* scope);
void hyp_scope_bind(HYPScope* scope, const char* name, const HYPType* type);
/* Checked forms: false when the binding could not be recorded in THIS frame
 * (arena exhaustion). The void forms above discard that and return silently,
 * which lets a caller that then does a scope-CHAIN lookup see a PARENT binding
 * of the same name and believe the child was bound -- fabricating callable
 * proof from a shadow that never took effect. Use these, and read the local
 * result, wherever a failed bind must not be mistaken for success. */
bool hyp_scope_bind_checked(HYPScope *scope, const char *name, const HYPType *type);
bool hyp_scope_bind_callable_checked(HYPScope *scope, const char *name, const HYPType *type,
                                     const char *callable_qn);
/* Bind a value whose identity is one exact callable.  A later ordinary
 * hyp_scope_bind of the same name clears this identity, so reassignment fails
 * closed instead of leaking a stale alias target. */
void hyp_scope_bind_callable(HYPScope *scope, const char *name, const HYPType *type,
                             const char *callable_qn);
const HYPType* hyp_scope_lookup(const HYPScope* scope, const char* name);
/* True when any lexical frame contains name, even when its type is UNKNOWN. */
bool hyp_scope_contains(const HYPScope *scope, const char *name);
/* Return the exact callable QN from the nearest binding.  A nearer ordinary
 * binding shadows a parent's callable and therefore returns NULL. */
const char *hyp_scope_lookup_callable(const HYPScope *scope, const char *name);
/* Replace (or clear with NULL) callable identity on the nearest existing
 * lexical binding. Returns false when name is unbound. This is for assignment;
 * declarations should continue to use hyp_scope_bind[_callable]. */
bool hyp_scope_update_callable(HYPScope *scope, const char *name, const char *callable_qn);

#endif // HYP_LSP_SCOPE_H
