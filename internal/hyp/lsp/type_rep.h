#ifndef HYP_LSP_TYPE_REP_H
#define HYP_LSP_TYPE_REP_H

#include "../arena.h"
#include <stdbool.h>
#include <stdint.h>

// HYPTypeKind enumerates all type representations.
typedef enum {
    HYP_TYPE_UNKNOWN = 0,
    HYP_TYPE_NAMED,       // named type: "Database", "http.Request"
    HYP_TYPE_POINTER,     // *T
    HYP_TYPE_SLICE,       // []T
    HYP_TYPE_MAP,         // map[K]V
    HYP_TYPE_CHANNEL,     // chan T
    HYP_TYPE_FUNC,        // func(params) returns
    HYP_TYPE_INTERFACE,   // interface{...}
    HYP_TYPE_STRUCT,      // struct{...}
    HYP_TYPE_BUILTIN,     // int, string, bool, error, etc.
    HYP_TYPE_TUPLE,       // multi-return (T1, T2) / TS tuple [T,U]
    HYP_TYPE_TYPE_PARAM,  // generic type parameter: T, K, V
    HYP_TYPE_REFERENCE,   // T& (C++ lvalue reference)
    HYP_TYPE_RVALUE_REF,  // T&& (C++ rvalue reference)
    HYP_TYPE_TEMPLATE,    // Parameterized type: vector<T> — stores template name + args
    HYP_TYPE_ALIAS,       // Type alias: using/typedef — stores alias name + underlying type
    HYP_TYPE_UNION,       // Python: A | B; TS: A | B | C — sorted-canonical list (shared)
    HYP_TYPE_LITERAL,     // Python: Literal["foo", 3] — wraps a base type + literal value text
    HYP_TYPE_PROTOCOL,    // Python: typing.Protocol — like INTERFACE but matched structurally
    HYP_TYPE_MODULE,      // Python: import os; os is a module-typed binding
    HYP_TYPE_CALLABLE,    // Python: Callable[[A, B], R] — untyped-named callable variant of FUNC

    // --- TS-specific kinds (added in TS LSP integration) ---
    HYP_TYPE_INTERSECTION,  // TS: A & B — intersection type
    HYP_TYPE_TS_LITERAL,    // TS: "foo" / 42 / true literal types (tag+value layout, distinct
                            // from Python's HYP_TYPE_LITERAL which uses base+literal_text)
    HYP_TYPE_INDEXED,       // TS: T[K] — indexed access type
    HYP_TYPE_KEYOF,         // TS: keyof T
    HYP_TYPE_TYPEOF_QUERY,  // TS: typeof x in type position
    HYP_TYPE_CONDITIONAL,   // TS: T extends U ? X : Y
    HYP_TYPE_OBJECT_LIT,    // TS: { a: T1; b: T2 } anonymous object type
    HYP_TYPE_INFER,         // TS: `infer X` placeholder inside conditional
    HYP_TYPE_MAPPED,        // TS: {[K in keyof T]: ...} — v1 stub, members may be NULL
} HYPTypeKind;

// Forward declaration
typedef struct HYPType HYPType;

// Language-specific adapter used to parse one ordered signature type spelling.
typedef const HYPType *(*HYPTypeTextParser)(HYPArena *arena, const char *text, void *parser_ctx);

// HYPTypeParam represents a generic type parameter with optional constraint.
typedef struct {
    const char* name;        // "T", "K", "V"
    const HYPType* constraint; // interface constraint, or NULL for "any"
} HYPTypeParam;

// HYPType is a tagged union representing Go types.
struct HYPType {
    HYPTypeKind kind;
    union {
        struct { const char* qualified_name; } named;      // NAMED
        struct { const HYPType* elem; } pointer;            // POINTER
        struct { const HYPType* elem; } slice;              // SLICE
        struct { const HYPType* key; const HYPType* value; } map;  // MAP
        struct { const HYPType* elem; int direction; } channel;    // CHANNEL (0=bidi, 1=send, 2=recv)
        struct {
            const char** param_names;  // NULL-terminated
            const HYPType** param_types; // NULL-terminated
            const HYPType** return_types; // NULL-terminated
        } func;                                             // FUNC
        struct {
            const char** method_names;  // NULL-terminated
            const HYPType** method_sigs; // NULL-terminated (each is FUNC)
        } interface_type;                                   // INTERFACE
        struct {
            const char** field_names;   // NULL-terminated
            const HYPType** field_types; // NULL-terminated
        } struct_type;                                      // STRUCT
        struct { const char* name; } builtin;               // BUILTIN
        struct {
            const HYPType** elems;      // NULL-terminated
            int count;
        } tuple;                                            // TUPLE
        struct { const char* name; } type_param;            // TYPE_PARAM
        struct { const HYPType* elem; } reference;            // REFERENCE / RVALUE_REF
        struct {
            const char* template_name;      // "std::vector", "std::map"
            const HYPType** template_args;  // NULL-terminated
            int arg_count;
        } template_type;                                      // TEMPLATE
        struct {
            const char* alias_qn;          // "proj.ns.MyAlias"
            const HYPType* underlying;     // the actual type it aliases
        } alias;                                              // ALIAS
        struct {
            const HYPType** members;       // NULL-terminated, deduplicated, sorted by kind/qn
            int count;
        } union_type;                                         // UNION / INTERSECTION (shared)
        struct {
            const HYPType* base;           // base type (e.g. BUILTIN("int"), BUILTIN("str"))
            const char* literal_text;      // canonical text: "3", "\"foo\"", "True"
        } literal;                                            // LITERAL (Python)
        struct {
            const char* qualified_name;    // e.g. "typing.Iterable"
            const char** method_names;     // NULL-terminated method names — structural matching
            const HYPType** method_sigs;   // NULL-terminated signatures (each is FUNC/CALLABLE)
        } protocol;                                           // PROTOCOL
        struct {
            const char* module_qn;         // module qualified name (matches HYPImport.module_path)
        } module;                                             // MODULE
        struct {
            const HYPType** param_types;   // NULL-terminated; NULL element means "Any" / unknown
            const HYPType* return_type;    // single return; for tuples wrap in HYP_TYPE_TUPLE
            int param_count;               // -1 = elliptic / Callable[..., R]
        } callable;                                           // CALLABLE

        // --- TS-specific data ---
        struct {
            // Tag distinguishes string / number / boolean / bigint / null / undefined literals.
            // For boolean literals, value points to "true" or "false".
            const char* tag;               // "string" | "number" | "boolean" | "bigint" | "null" | "undefined"
            const char* value;             // textual representation; arena-owned
        } literal_ts;                                         // TS_LITERAL
        struct {
            const HYPType* object;         // T in T[K]
            const HYPType* index;          // K in T[K]
        } indexed;                                            // INDEXED
        struct { const HYPType* operand; } keyof;             // KEYOF
        struct { const char* expr; } typeof_query;            // TYPEOF_QUERY (referenced expression text)
        struct {
            const HYPType* check;          // T
            const HYPType* extends;        // U
            const HYPType* true_branch;    // X
            const HYPType* false_branch;   // Y
        } conditional;                                        // CONDITIONAL
        struct {
            const char** prop_names;       // NULL-terminated
            const HYPType** prop_types;    // NULL-terminated, parallel to prop_names
            const HYPType* call_signature; // FUNC type or NULL
            const HYPType* index_value;    // type produced by string/number index, or NULL
        } object_lit;                                         // OBJECT_LIT
        struct { const char* name; } infer;                   // INFER (e.g., `infer R`)
        struct {
            const char* key_name;          // "K" in {[K in keyof T]: V}
            const HYPType* key_constraint; // `keyof T`
            const HYPType* value;          // V (may reference key_name as TYPE_PARAM)
        } mapped;                                             // MAPPED (v1 stub-friendly)
    } data;
};

// Constructors (arena-allocated)
const HYPType* hyp_type_unknown(void);
const HYPType* hyp_type_named(HYPArena* a, const char* qualified_name);
const HYPType* hyp_type_pointer(HYPArena* a, const HYPType* elem);
const HYPType* hyp_type_slice(HYPArena* a, const HYPType* elem);
const HYPType* hyp_type_map(HYPArena* a, const HYPType* key, const HYPType* value);
const HYPType* hyp_type_channel(HYPArena* a, const HYPType* elem, int direction);
const HYPType* hyp_type_func(HYPArena* a, const char** param_names, const HYPType** param_types, const HYPType** return_types);
// Materialize exactly count positional parameter slots. NULL, empty, exact "?",
// and parser failures become UNKNOWN; the returned vector is NULL-terminated.
const HYPType **hyp_type_materialize_signature_params(HYPArena *a, const char *const *type_texts,
                                                      int count, HYPTypeTextParser parser,
                                                      void *parser_ctx);
// Rebuild a FUNC with new returns while preserving its parameter names/types.
const HYPType *hyp_type_func_replace_returns(HYPArena *a, const HYPType *old_signature,
                                             const HYPType *const *new_return_types);
const HYPType* hyp_type_builtin(HYPArena* a, const char* name);
const HYPType* hyp_type_tuple(HYPArena* a, const HYPType** elems, int count);
const HYPType* hyp_type_type_param(HYPArena* a, const char* name);
const HYPType* hyp_type_reference(HYPArena* a, const HYPType* elem);
const HYPType* hyp_type_rvalue_ref(HYPArena* a, const HYPType* elem);
const HYPType* hyp_type_template(HYPArena* a, const char* name, const HYPType** args, int arg_count);
const HYPType* hyp_type_alias(HYPArena* a, const char* alias_qn, const HYPType* underlying);

// Python-flavored constructors. UNION normalizes input: nested unions are
// flattened, duplicates removed, single-member unions collapse to that
// member, and the empty union is UNKNOWN. Members must be arena-allocated.
// Shared with TS LSP — both call this same constructor for `A | B`.
const HYPType* hyp_type_union(HYPArena* a, const HYPType** members, int count);
const HYPType* hyp_type_optional(HYPArena* a, const HYPType* t);  // Optional[T] == Union[T, None]
const HYPType* hyp_type_literal(HYPArena* a, const HYPType* base, const char* literal_text);
const HYPType* hyp_type_protocol(HYPArena* a, const char* qualified_name,
    const char** method_names, const HYPType** method_sigs);
const HYPType* hyp_type_module(HYPArena* a, const char* module_qn);
const HYPType* hyp_type_callable(HYPArena* a, const HYPType** param_types, int param_count,
    const HYPType* return_type);

// --- TS-specific constructors ---
const HYPType* hyp_type_intersection(HYPArena* a, const HYPType** members, int count);
// tag is one of "string"|"number"|"boolean"|"bigint"|"null"|"undefined".
// Distinct from hyp_type_literal (Python) which uses base+literal_text.
const HYPType* hyp_type_ts_literal(HYPArena* a, const char* tag, const char* value);
const HYPType* hyp_type_indexed(HYPArena* a, const HYPType* object, const HYPType* index);
const HYPType* hyp_type_keyof(HYPArena* a, const HYPType* operand);
const HYPType* hyp_type_typeof_query(HYPArena* a, const char* expr);
const HYPType* hyp_type_conditional(HYPArena* a,
    const HYPType* check, const HYPType* extends,
    const HYPType* true_branch, const HYPType* false_branch);
// prop_names and prop_types are NULL-terminated parallel arrays; either may be NULL for empty.
const HYPType* hyp_type_object_lit(HYPArena* a,
    const char** prop_names, const HYPType** prop_types,
    const HYPType* call_signature, const HYPType* index_value);
const HYPType* hyp_type_infer(HYPArena* a, const char* name);
const HYPType* hyp_type_mapped(HYPArena* a,
    const char* key_name, const HYPType* key_constraint, const HYPType* value);

// Operations
const HYPType* hyp_type_deref(const HYPType* t);         // remove one pointer level
const HYPType* hyp_type_elem(const HYPType* t);           // get element type (slice/chan/pointer)
bool hyp_type_is_unknown(const HYPType* t);
bool hyp_type_is_interface(const HYPType* t);
bool hyp_type_is_pointer(const HYPType* t);
bool hyp_type_is_reference(const HYPType* t);
bool hyp_type_is_union(const HYPType* t);
bool hyp_type_is_protocol(const HYPType* t);
bool hyp_type_is_module(const HYPType* t);

// Structural equality on type representation (used by union dedup and
// protocol-method-set matching). Two types are equal if their kinds match
// and their structural members match recursively.
bool hyp_type_equal(const HYPType* a, const HYPType* b);

// Test whether `candidate` satisfies the structural protocol `proto`.
// Walks proto.method_names against candidate's method set (NAMED → registry
// lookup is the caller's job; this helper only matches existing method
// signatures stored on a PROTOCOL).
bool hyp_type_protocol_satisfied_by(const HYPType* proto, const HYPType* candidate);

// Follow alias chain with cycle detection (max 16 levels).
const HYPType* hyp_type_resolve_alias(const HYPType* t);

// Generic type substitution: replace type params in t with concrete types.
// type_params: NULL-terminated array of param names
// type_args: corresponding concrete types
const HYPType* hyp_type_substitute(HYPArena* a, const HYPType* t,
    const char** type_params, const HYPType** type_args);

#endif // HYP_LSP_TYPE_REP_H
