#ifndef HYP_LSP_TYPE_REGISTRY_H
#define HYP_LSP_TYPE_REGISTRY_H

#include "type_rep.h"
#include "../arena.h"
#include <stdbool.h>

// Language-specific function metadata. Added at struct tail so existing
// callers that memset to zero before populating other fields keep working.
typedef enum {
    HYP_FUNC_FLAG_NONE = 0,
    HYP_FUNC_FLAG_PROPERTY = 1 << 0,        // @property -> obj.attr returns getter return
    HYP_FUNC_FLAG_CLASSMETHOD = 1 << 1,     // @classmethod -> first arg is cls (the class)
    HYP_FUNC_FLAG_STATICMETHOD = 1 << 2,    // @staticmethod -> no implicit self/cls
    HYP_FUNC_FLAG_ABSTRACTMETHOD = 1 << 3,  // @abstractmethod -> still callable for resolution
    HYP_FUNC_FLAG_OVERLOAD = 1 << 4,        // @overload entry — non-implementation stub
    HYP_FUNC_FLAG_ASYNC = 1 << 5,           // async def — return is Coroutine[..., T]
    HYP_FUNC_FLAG_GENERATOR = 1 << 6,       // contains yield — return is Generator[T, ...]
    HYP_FUNC_FLAG_FINAL = 1 << 7,           // @final — overrides not allowed
    HYP_FUNC_FLAG_RUST_TRAIT_IMPL = 1 << 8, // exact method from impl Trait for Type
    HYP_FUNC_FLAG_RUST_ABSTRACT = 1 << 9,   // required trait method without a default body
    /* Python only: more than one registered definition has this exact QN.
     * Ordinary call resolution keeps its historical language-specific choice,
     * but a function value cannot name one materialized definition exactly. */
    HYP_FUNC_FLAG_AMBIGUOUS_BINDING = 1 << 10,
} HYPFuncFlags;

// Registered function/method with full type signature.
typedef struct {
    const char *qualified_name;    // e.g., "proj.pkg.TypeName.MethodName"
    const char *receiver_type;     // e.g., "proj.pkg.TypeName" (NULL for functions)
    const char *short_name;        // e.g., "MethodName"
    const HYPType *signature;      // FUNC type with param/return types
    const char **type_param_names; // NULL-terminated, e.g., ["T", "R", NULL] for generics
    int min_params;                // Minimum required params (excluding defaulted). -1 = unknown.
    int flags;                     // HYP_FUNC_FLAG_* bitfield
    const char **decorator_qns;    // NULL-terminated decorator QNs (Python only); used for
                                   // user-decorator return-type substitution.
    /* Rust only: canonical trait QN for a concrete trait-impl method. It may
     * remain NULL when raw cross-file provenance is ambiguous; the Rust trait
     * flag still prevents that method from being mistaken for inherent. */
    const char *impl_trait_qn;
} HYPRegisteredFunc;

// Registered type with fields and method names.
typedef struct {
    const char *qualified_name;    // e.g., "proj.pkg.TypeName"
    const char *short_name;        // e.g., "TypeName"
    const char **field_names;      // NULL-terminated
    const HYPType **field_types;   // NULL-terminated (parallel to field_names)
    const char **method_names;     // NULL-terminated (short names)
    const char **method_qns;       // NULL-terminated (qualified names, parallel)
    const char **embedded_types;   // NULL-terminated (embedded/anonymous field type QNs)
    const char *alias_of;          // QN of aliased type (type Foo = Bar), NULL if not alias
    const char **type_param_names; // NULL-terminated, e.g., ["T", "K", NULL] for template classes
    bool is_interface;
    bool is_object; // Kotlin `object`/`companion object` singleton (member calls are static)

    // --- TS-specific fields (NULL/empty for non-TS types — backward compatible) ---
    // TS interfaces / object types may be callable: `interface F { (x:number): string }`.
    const HYPType *call_signature; // FUNC type or NULL
    // TS objects can have an index signature: `{ [key:string]: V }` or `{ [i:number]: V }`.
    const HYPType *index_key_type;   // BUILTIN("string"|"number") or NULL
    const HYPType *index_value_type; // V or NULL
    // Generic constraints, parallel to type_param_names. NULL or shorter array means "any".
    const HYPType **type_param_constraints; // NULL-terminated, parallel to type_param_names
} HYPRegisteredType;

// Hash-table bucket entry. Chains collisions via next-index list for overload sets.
typedef struct {
    uint64_t hash;     // FNV-1a of key
    int payload_index; // index into reg->funcs[] or reg->types[]
    int next_index;    // -1 = end of chain; else index of next bucket entry in same chain
    int slot;          // bucket slot this entry sits in (for resize)
} HYPRegistryHashEntry;

// Cross-file type/function registry.
typedef struct HYPTypeRegistry {
    HYPRegisteredFunc *funcs;
    int func_count;
    int func_cap;

    HYPRegisteredType *types;
    int type_count;
    int type_cap;

    HYPArena *arena; // owns all string data

    /* Optional fallback registry (Tier 2 two-level lookup). When a
     * lookup misses in this registry, it chains to `fallback`. Used by
     * TS/PHP cross-LSP: a small per-file registry (the file's own
     * AST-refined types) chains to a shared, immutable base registry
     * (stdlib + all project defs) built once. NULL = no chaining. */
    const struct HYPTypeRegistry *fallback;

    // Hash indexes (built lazily by hyp_registry_finalize, NULL until then).
    // Lookups fall back to linear scan when these are NULL.
    int *func_qn_buckets; // bucket → first entry index in func_qn_entries; -1 = empty
    HYPRegistryHashEntry *func_qn_entries; // entries indexed by linear order
    int func_qn_bucket_count;
    int func_qn_entry_count;

    int *type_qn_buckets;
    HYPRegistryHashEntry *type_qn_entries;
    int type_qn_bucket_count;
    int type_qn_entry_count;

    // Methods indexed by (receiver_type, short_name) — chain holds overloads.
    int *method_buckets;
    HYPRegistryHashEntry *method_entries;
    int method_bucket_count;
    int method_entry_count;

    // Auxiliary short-name / embedded-type indexes (built by finalize alongside the
    // QN buckets). Turn the Rust trait- and free-function fallback scans from
    // O(type_count)/O(func_count) into O(chain). Read-only after finalize.
    // Embedded-type index: fnv1a(bare last-'.'-segment of each embedded_type) -> chain
    // of TYPE indices declaring it. payload_index = type index (a type may appear once
    // per embedded entry; consumers dedup adjacent same-type via the iterator).
    int *type_embed_buckets;
    HYPRegistryHashEntry *type_embed_entries;
    int type_embed_bucket_count;
    int type_embed_entry_count;
    // Free-function short-name index: fnv1a(short_name) -> chain of FREE-function
    // (receiver_type==NULL) indices. payload_index = func index.
    int *ffunc_short_buckets;
    HYPRegistryHashEntry *ffunc_short_entries;
    int ffunc_short_bucket_count;
    int ffunc_short_entry_count;

    /* Sealed / read-only. Set true by the hyp_X_build_cross_registry builders
     * (c/cpp, python, c#, ts, go) right after finalize: a Tier-2 cross-registry
     * is built ONCE and shared READ-ONLY across the parallel resolve workers.
     * hyp_registry_add_func/_type no-op on a sealed registry, so a per-file
     * resolver can never mutate the shared, finalized registry. Without this,
     * post-finalize adds accumulate in a tail the hash index does not cover ->
     * every lookup linear-scans it -> O(files*defs) (the Linux-kernel full-index
     * hang) plus a heap data race across workers. */
    bool read_only;
} HYPTypeRegistry;

// Initialize a registry.
void hyp_registry_init(HYPTypeRegistry *reg, HYPArena *arena);

// Build the hash indexes after all funcs/types have been added. Subsequent lookups
// use O(1) hashed dispatch instead of linear scans. Calling this is OPTIONAL — the
// linear-scan path remains correct. Single-file resolvers (small registries) skip
// finalize and stay linear; project-wide registries (many thousands of entries) call
// it once after pass-1.5 def-collection.
void hyp_registry_finalize(HYPTypeRegistry *reg);

// Like hyp_registry_finalize, but the hash-index allocations (buckets/entries)
// come from idx_arena instead of reg->arena. Per-file cross resolvers MUST use
// this with a scratch arena destroyed after the walk: their reg->arena is the
// pipeline-lifetime result arena, and per-file index allocations accumulated
// there add GBs across a large repo (FastAPI incremental test: +1.1 GB RSS).
void hyp_registry_finalize_into(HYPTypeRegistry *reg, HYPArena *idx_arena);

// Register a function/method.
void hyp_registry_add_func(HYPTypeRegistry *reg, HYPRegisteredFunc func);

// Register a type.
void hyp_registry_add_type(HYPTypeRegistry *reg, HYPRegisteredType type);

// Look up a method by receiver type QN + method name.
const HYPRegisteredFunc *hyp_registry_lookup_method(const HYPTypeRegistry *reg,
                                                    const char *receiver_qn,
                                                    const char *method_name);

// Look up a type by qualified name.
const HYPRegisteredType *hyp_registry_lookup_type(const HYPTypeRegistry *reg,
                                                  const char *qualified_name);

// Look up a function by qualified name.
const HYPRegisteredFunc *hyp_registry_lookup_func(const HYPTypeRegistry *reg,
                                                  const char *qualified_name);

// Look up a symbol (type or function) in a package by short name.
// package_qn is the package prefix (e.g., "proj.pkg").
const HYPRegisteredFunc *hyp_registry_lookup_symbol(const HYPTypeRegistry *reg,
                                                    const char *package_qn, const char *name);

// Resolve type alias chain: follow alias_of until concrete type found (max 16 levels).
const HYPRegisteredType *hyp_registry_resolve_alias(const HYPTypeRegistry *reg,
                                                    const char *type_qn);

// Look up a method by receiver type QN + method name, following alias chains.
const HYPRegisteredFunc *hyp_registry_lookup_method_aliased(const HYPTypeRegistry *reg,
                                                            const char *receiver_qn,
                                                            const char *method_name);

// Look up a method by receiver type + name, preferring the overload with matching arg count.
// Falls back to any match if no exact arg count match found.
const HYPRegisteredFunc *hyp_registry_lookup_method_by_args(const HYPTypeRegistry *reg,
                                                            const char *receiver_qn,
                                                            const char *method_name, int arg_count);

// Look up a free function by package + name, preferring matching arg count.
const HYPRegisteredFunc *hyp_registry_lookup_symbol_by_args(const HYPTypeRegistry *reg,
                                                            const char *package_qn,
                                                            const char *name, int arg_count);

// Look up a method by receiver type + name, scoring overloads by parameter type match.
// arg_types may contain NULL entries for unknown types. Falls back to arg-count matching.
const HYPRegisteredFunc *hyp_registry_lookup_method_by_types(const HYPTypeRegistry *reg,
                                                             const char *receiver_qn,
                                                             const char *method_name,
                                                             const HYPType **arg_types,
                                                             int arg_count);

// Look up a free function by package + name, scoring overloads by parameter type match.
const HYPRegisteredFunc *hyp_registry_lookup_symbol_by_types(const HYPTypeRegistry *reg,
                                                             const char *package_qn,
                                                             const char *name,
                                                             const HYPType **arg_types,
                                                             int arg_count);

// --- Auxiliary index iterators (Rust trait / free-function fallback fast paths) ---
//
// Iterate registry TYPE indices whose embedded_types contain an entry whose BARE
// name (last '.'-segment) equals `bare`. On a finalized registry this walks the
// embedded-type index plus any post-finalize tail; on an unfinalized registry it
// degrades to a full linear scan over all types (identical candidate set). Each
// matching type index is yielded at most once, in ascending registry order. The
// index is a bare-name PREFILTER — the caller MUST still apply its own exact
// predicate on each yielded type. Read-only, allocation-free. Usage:
//   HYPTypeEmbedIter it; hyp_registry_types_by_embedded_bare(reg, bare, &it);
//   int ti; while ((ti = hyp_type_embed_iter_next(&it)) >= 0) { ... reg->types[ti] ... }
typedef struct {
    const HYPTypeRegistry *reg;
    uint64_t hash;
    int chain_idx; // next entry in the embed chain, or -1
    int tail_i;    // next tail/linear type index
    int tail_end;  // reg->type_count snapshot
    int prev_type; // last yielded type index (adjacent-dedup); -1 = none
} HYPTypeEmbedIter;
void hyp_registry_types_by_embedded_bare(const HYPTypeRegistry *reg, const char *bare,
                                         HYPTypeEmbedIter *out);
int hyp_type_embed_iter_next(HYPTypeEmbedIter *it);

// Iterate FREE-function (receiver_type==NULL) indices whose short_name equals
// `short_name`. Same finalized/unfinalized behavior as above; caller re-checks its
// own predicate. Read-only, allocation-free.
typedef struct {
    const HYPTypeRegistry *reg;
    uint64_t hash;
    int chain_idx;
    int tail_i;
    int tail_end;
} HYPFreeFuncIter;
void hyp_registry_free_funcs_by_short_name(const HYPTypeRegistry *reg, const char *short_name,
                                           HYPFreeFuncIter *out);
int hyp_free_func_iter_next(HYPFreeFuncIter *it);

// Iterate function indices for one exact (receiver QN, method name) key.  This
// exposes the existing finalized method bucket without making Rust scan the
// project-wide func array merely to distinguish inherent and trait-impl
// entries that intentionally share the same source-level QN.  The caller may
// filter on language-specific flags. Read-only and allocation-free.
typedef struct {
    const HYPTypeRegistry *reg;
    const char *receiver_qn;
    const char *method_name;
    uint64_t hash;
    int chain_idx;
    int tail_i;
    int tail_end;
} HYPMethodIter;
void hyp_registry_methods(const HYPTypeRegistry *reg, const char *receiver_qn,
                          const char *method_name, HYPMethodIter *out);
int hyp_method_iter_next(HYPMethodIter *it);

// --- TS-specific helpers (return NULL for types without these signatures) ---

// If the type has a call signature (e.g., `interface F { (x:number): string }`), return
// a synthesised HYPRegisteredFunc whose qualified_name is "<type_qn>.__call" and
// short_name is "__call". Returns NULL if no call signature is present, the type is
// missing, or the receiver type was not registered. Caller must NOT free.
const HYPRegisteredFunc *hyp_registry_lookup_callable(const HYPTypeRegistry *reg, HYPArena *arena,
                                                      const char *type_qn);

// If the type has an index signature, return the value type produced by indexing with
// the given key type (string vs number). Returns NULL if no matching index signature.
const HYPType *hyp_registry_lookup_index_signature(const HYPTypeRegistry *reg, const char *type_qn,
                                                   const HYPType *key_type);

#endif // HYP_LSP_TYPE_REGISTRY_H
