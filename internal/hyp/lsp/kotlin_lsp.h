/*
 * kotlin_lsp.h — Kotlin Light Semantic Pass.
 *
 * In-process type-aware call resolver for Kotlin. Mirrors go_lsp / php_lsp
 * shape: a per-file pass that walks the tree-sitter Kotlin AST, tracks
 * scope, infers expression types, and emits HYPResolvedCall entries for
 * call_expression / navigation_expression nodes whose target FQN can be
 * determined statically.
 *
 * Reverse-engineered from the fwcd kotlin-language-server reference
 * implementation (Kotlin compiler frontend) plus the official Kotlin
 * language specification — distilled into a pure-C resolver with no JVM
 * dependency. The goal is ≥ 90% quality parity with the LSP for the
 * call-edge and method-dispatch attribution problems HYP cares about.
 *
 * The Kotlin features handled:
 *   - Package declaration (`package a.b.c`)
 *   - Default Kotlin imports (kotlin.*, kotlin.collections.*, etc.)
 *   - Explicit imports with optional `as` aliases
 *   - Wildcard imports (`import a.b.*`)
 *   - Top-level functions and properties
 *   - Class / interface / object / data class / enum / sealed class
 *   - Companion objects (named and anonymous) with `Foo.bar()` static-style dispatch
 *   - Primary and secondary constructors (incl. `class Foo(val x: Int)`)
 *   - Inheritance: delegation_specifier list, super-method lookup
 *   - Extension functions and properties (`fun String.uppercaseFirst()`)
 *   - Infix and operator functions (`a foo b`, `a + b`)
 *   - Smart casts after `is` / `as` / null-checks
 *   - Generics (basic substitution; we track but don't fully unify)
 *   - Nullable types (T?), safe calls (?.) and not-null assertions (!!)
 *   - `lateinit var` and `by` delegation (best-effort)
 *   - Lambdas with implicit `it` parameter
 *   - `with`, `let`, `also`, `apply`, `run`, `takeIf` scope functions
 *   - `when` expressions with subject smart-cast
 *   - `object` declarations as singletons
 *
 * Out of scope (low value or impractical without the Kotlin compiler):
 *   - Reified generics across call boundaries
 *   - Full type unification with constraints
 *   - Decompilation of compiled Kotlin/Java bytecode
 *   - DSL-style type-safe builders beyond direct lambda receivers
 *   - Inline class boxing/unboxing
 *
 * The unresolved fallthrough goes to the registry's name-based matcher,
 * just like Go/Python/PHP. We never produce *worse* attribution than the
 * pre-LSP baseline; if the LSP can't decide, it emits nothing.
 */
#ifndef HYP_LSP_KOTLIN_LSP_H
#define HYP_LSP_KOTLIN_LSP_H

#include "type_rep.h"
#include "scope.h"
#include "type_registry.h"
#include "../hyp.h"
#include "go_lsp.h" /* HYPLSPDef, HYPResolvedCallArray reused across languages */

/* Use-kind for `import a.b.c.foo` — tracks whether the import refers to a
 * type, a function/extension, a property, or an object. Determines how
 * `foo` is resolved when used as a bare identifier. */
typedef enum {
    HYP_KT_USE_UNKNOWN = 0,
    HYP_KT_USE_TYPE,     /* class / interface / object / typealias */
    HYP_KT_USE_FUNCTION, /* top-level fun, extension fun */
    HYP_KT_USE_PROPERTY, /* top-level val/var */
    HYP_KT_USE_WILDCARD, /* import a.b.* — local_name is a.b prefix */
} HYPKotlinUseKind;

typedef struct {
    const HYPScope *scope; /* exact lexical declaration frame, NULL for members */
    const char *owner_qn;  /* class/object QN for a member, NULL for lexical values */
    const char *name;
    const char *getter_qn;
    const char *setter_qn;
} HYPKotlinDelegateBinding;

/* KotlinLSPContext — per-file state for Kotlin call resolution. */
typedef struct KotlinLSPContext {
    HYPArena *arena;
    const char *source;
    int source_len;
    const HYPTypeRegistry *registry;
    HYPScope *current_scope;

    /* Package context. Empty string for default package. */
    const char *package_qn;    /* dotted form, e.g. "com.example.foo" */
    const char *module_qn;     /* file-level QN, e.g. "<project>.com.example.foo.<File>" */
    const char *project_name;  /* project prefix (without trailing dot) */
    const char *file_class_qn; /* JVM file-class QN, "<package>.<File>Kt" */
    const char *rel_path;      /* for diagnostics */

    /* Import map (parallel arrays). Kotlin imports are flat — no grouping
     * — but each may have an `as` alias. Wildcard imports record the
     * package prefix in `import_targets[i]` with `import_kinds[i] = WILDCARD`
     * and `import_locals[i] = NULL`. */
    const char **import_locals;  /* alias name (or short name) used in code */
    const char **import_targets; /* full FQN being imported */
    HYPKotlinUseKind *import_kinds;
    int import_count;
    int import_cap;

    /* Current declaration context. */
    const char *enclosing_func_qn;      /* function we are resolving inside */
    const char *enclosing_class_qn;     /* innermost class/interface/object QN, or NULL */
    const char *enclosing_companion_qn; /* if inside a companion object body */
    const char *enclosing_super_qn;     /* primary super-class QN of current class, or NULL */

    /* Current `this` and `super` types. */
    const HYPType *this_type;
    const HYPType *super_type;

    /* `it` lambda parameter type, when inside a single-arg lambda.
     * Saved/restored across nested lambdas. */
    const HYPType *it_type;

    /* Output: resolved calls accumulate here. */
    HYPResolvedCallArray *resolved_calls;

    /* Access-level delegated-property semantics. The declaration records the
     * proven getValue/setValue targets; actual reads/writes inject exact call
     * carriers and resolutions at their source occurrences. */
    HYPCallArray *synthetic_calls;
    HYPKotlinDelegateBinding *delegate_bindings;
    int delegate_count;
    int delegate_cap;

    /* Recursion guard for kotlin_eval_expr_type. */
    int eval_depth;

    /* AST-walk recursion depth for kt_resolve_calls_in_node (guards stack
     * overflow on deeply-nested/cyclic files; see hyp_lsp_max_walk_depth).
     * Zero via memset. */
    int walk_depth;

    /* Debug mode (HYP_LSP_DEBUG env). */
    bool debug;
    /* Cross-file only (NULL per-file): unique short type name -> registered
     * QN, built from the project defs by hyp_run_kotlin_lsp_cross. Consulted
     * by kotlin_resolve_class_name before its blind same-package guess, so an
     * unimported cross-file receiver type ("Holder" defined in Holder.kt,
     * used in Use.kt) resolves to its real registered QN. An ambiguous short
     * name maps to a sentinel and resolves nothing (fail closed). Owned by
     * hyp_run_kotlin_lsp_cross; opaque pointer to avoid a foundation include
     * in this header. */
    const void *cross_type_short;
    /* Cross-file only (NULL per-file): receiver type QN -> KtCrossFieldList,
     * from hyp_kotlin_register_lsp_defs. Property-reference emission reads
     * each field's REAL def QN here (kotlin class properties are minted with
     * module-level QNs, so composing <class>.<member> would miss the node). */
    const void *cross_field_map;
} KotlinLSPContext;

/* Initialize a KotlinLSPContext for processing one file. */
void kotlin_lsp_init(KotlinLSPContext *ctx, HYPArena *arena, const char *source, int source_len,
                     const HYPTypeRegistry *registry, const char *package_qn, const char *module_qn,
                     const char *project_name, const char *rel_path, HYPResolvedCallArray *out);

/* Add an import mapping. local_name is the name used in code (alias or
 * short name); target_qn is the full dotted FQN. For wildcard imports,
 * pass the package prefix as target_qn and HYP_KT_USE_WILDCARD as kind. */
void kotlin_lsp_add_import(KotlinLSPContext *ctx, const char *local_name, const char *target_qn,
                           HYPKotlinUseKind kind);

/* Walk a file's AST: top-level decls, then function/method bodies. */
void kotlin_lsp_process_file(KotlinLSPContext *ctx, TSNode root);

/* Evaluate a Kotlin expression's type. Returns hyp_type_unknown() on
 * failure — never NULL. */
const HYPType *kotlin_eval_expr_type(KotlinLSPContext *ctx, TSNode node);

/* Parse a Kotlin type-AST node (user_type, nullable_type, function_type, …)
 * to HYPType. */
const HYPType *kotlin_parse_type_node(KotlinLSPContext *ctx, TSNode node);

/* Resolve a bare class name (possibly qualified like "Foo" or "a.Foo")
 * to its full QN using current package + import map. NULL if unresolved. */
const char *kotlin_resolve_class_name(KotlinLSPContext *ctx, const char *name);

/* Resolve a bare top-level function name to its target QN. */
const char *kotlin_resolve_function_name(KotlinLSPContext *ctx, const char *name);

/* Look up a method on a class, walking the super-chain (registry-based). */
const HYPRegisteredFunc *kotlin_lookup_method(KotlinLSPContext *ctx, const char *class_qn,
                                              const char *method_name);

/* Look up a property/field on a class, walking super-chain. */
const HYPType *kotlin_lookup_property_type(KotlinLSPContext *ctx, const char *class_qn,
                                           const char *prop_name);

/* Entry point: build registry from file defs + stdlib, then run resolution.
 * Called from hyp_extract_file() after definitions and imports have been
 * extracted. */
void hyp_run_kotlin_lsp(HYPArena *arena, HYPFileResult *result, const char *source, int source_len,
                        TSNode root);

/* Cross-file LSP: build a registry from project-wide defs (local + cross-file)
 * + stdlib, re-parse the source if no cached tree, walk and resolve calls.
 * Mirrors hyp_run_java_lsp_cross. `defs` carries the graph QNs of every
 * project definition so a bare top-level call in file B resolves to the
 * definition node living in file A. Output is appended to `out`. */
/* out_field_map (optional): receives ownership of the receiver-QN -> field
 * list map built during registration (opaque HYPHashTable; the caller frees
 * with hyp_ht_free). Pass NULL to discard it. The forward declaration lives at
 * file scope -- a struct tag first named inside a prototype would get
 * prototype scope and conflict with the foundation typedef. */
struct HYPHashTable;
void hyp_kotlin_register_lsp_defs(HYPArena *arena, HYPTypeRegistry *reg, const HYPLSPDef *defs,
                                  int def_count, struct HYPHashTable **out_field_map);

void hyp_run_kotlin_lsp_cross(HYPArena *arena, const char *source, int source_len,
                              const char *module_qn, HYPLSPDef *defs, int def_count,
                              const char **import_names, const char **import_qns, int import_count,
                              TSTree *cached_tree, HYPResolvedCallArray *out);

/* Register the curated Kotlin stdlib (kotlin.*, kotlin.collections.*, …)
 * into a registry. Implemented in lsp/generated/kotlin_stdlib_data.c. */
void hyp_kotlin_stdlib_register(HYPTypeRegistry *reg, HYPArena *arena);

/* Register Kotlin default-import targets — the prefixes auto-imported
 * by every Kotlin file. Used by the LSP context init. */
const char *const *hyp_kotlin_default_import_packages(int *count_out);

#endif /* HYP_LSP_KOTLIN_LSP_H */
