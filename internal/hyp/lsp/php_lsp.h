#ifndef HYP_LSP_PHP_LSP_H
#define HYP_LSP_PHP_LSP_H

#include "type_rep.h"
#include "scope.h"
#include "type_registry.h"
#include "../hyp.h"
#include "go_lsp.h"  /* HYPLSPDef reused across languages */

/* PHPLSPContext — per-file state for PHP type-aware call resolution.
 * Mirrors GoLSPContext / CLSPContext structure. */
typedef struct {
    HYPArena *arena;
    const char *source;
    int source_len;
    const HYPTypeRegistry *registry;
    HYPScope *current_scope;

    /* Namespace state. PHP files declare a single namespace
     * (or use the global namespace if none); empty string means global. */
    const char *current_namespace_qn;

    /* `use` clause map.
     * use_kinds[i] selects whether the local maps a class, function, or const. */
    const char **use_local_names;
    const char **use_target_qns;
    enum { HYP_PHP_USE_CLASS = 0, HYP_PHP_USE_FUNCTION, HYP_PHP_USE_CONST } *use_kinds;
    int use_count;
    int use_cap;

    /* Current function/method/class context. */
    const char *enclosing_func_qn;
    const char *enclosing_class_qn; /* NULL outside class body */
    const char *enclosing_parent_qn; /* parent class QN (for parent::), or NULL */
    const char *module_qn;

    /* Output: resolved calls accumulate here. */
    HYPResolvedCallArray *resolved_calls;

    /* @phpstan-type alias map (per-file, populated from class docblocks).
     * Used by resolve_phpdoc_type before generic name resolution so user
     * type aliases like `@phpstan-type UserId int|string` and references
     * to `UserId` in @var/@param/@return all resolve to the aliased type.
     */
    const char **phpstan_alias_names;  /* arena-allocated, NULL-terminated */
    const HYPType **phpstan_alias_types;
    int phpstan_alias_count;
    int phpstan_alias_cap;

    /* Recursion guard for php_eval_expr_type. */
    int eval_depth;

    /* AST-walk recursion depth for php_resolve_calls_in_node (guards stack
     * overflow on deeply-nested/cyclic files; see hyp_lsp_max_walk_depth).
     * Zero via memset. */
    int walk_depth;

    /* Debug mode (HYP_LSP_DEBUG env). */
    bool debug;
} PHPLSPContext;

/* Initialize a PHPLSPContext for processing one file. */
void php_lsp_init(PHPLSPContext *ctx, HYPArena *arena, const char *source, int source_len,
                  const HYPTypeRegistry *registry, const char *module_qn,
                  HYPResolvedCallArray *out);

/* Add a `use` mapping. */
void php_lsp_add_use(PHPLSPContext *ctx, const char *local_name, const char *target_qn,
                     int use_kind);

/* Process a file's AST: walk top-level decls, then function/method bodies. */
void php_lsp_process_file(PHPLSPContext *ctx, TSNode root);

/* Refine an already-populated registry from local class ASTs: declared and
 * PHPDoc returns, trait flattening, and typed fields. */
void hyp_php_refine_lsp_registry(PHPLSPContext *ctx, HYPTypeRegistry *reg, TSNode root);

/* Evaluate a PHP expression's type. May return NULL / HYP_TYPE_UNKNOWN. */
const HYPType *php_eval_expr_type(PHPLSPContext *ctx, TSNode node);

/* Parse a PHP type-AST node (named_type, primitive_type, union_type, ...) to HYPType. */
const HYPType *php_parse_type_node(PHPLSPContext *ctx, TSNode node);

/* Resolve a class name (bare or qualified) using current namespace + use map. */
const char *php_resolve_class_name(PHPLSPContext *ctx, const char *name);

/* Look up a method on a class, walking parent chain (registry-based). */
const HYPRegisteredFunc *php_lookup_method(PHPLSPContext *ctx, const char *class_qn,
                                            const char *method_name);

/* Entry point: build registry from file defs + stdlib + composer (if present),
 * then run resolution. Called from hyp_extract_file(). */
void hyp_run_php_lsp(HYPArena *arena, HYPFileResult *result, const char *source, int source_len,
                    TSNode root);

/* Register PHP stdlib + curated framework types into a registry. */
void hyp_php_stdlib_register(HYPTypeRegistry *reg, HYPArena *arena);

/* --- Cross-file LSP resolution ---
 *
 * Mirrors hyp_run_py_lsp_cross / hyp_run_ts_lsp_cross. Caller supplies the
 * combined HYPLSPDef[] (file-local + cross-file) and a resolved import map
 * (use → target QN). Imports are added as CLASS-kind uses; file-internal
 * `use` declarations from the AST are layered on top by process_file.
 *
 * Reuses go_lsp.h's HYPLSPDef so cross-language registration is uniform. */
void hyp_php_register_lsp_defs(HYPArena *arena, HYPArena *idx_arena, HYPTypeRegistry *reg,
                               const HYPLSPDef *defs, int def_count);

void hyp_run_php_lsp_cross(
    HYPArena *arena,
    const char *source, int source_len,
    const char *module_qn,
    HYPLSPDef *defs, int def_count,
    const char **import_names, const char **import_qns, int import_count,
    TSTree *cached_tree,           /* NULL = parse internally */
    HYPResolvedCallArray *out);

/* --- Batch cross-file LSP --- */

/* Per-file input for batch PHP LSP processing. */
typedef struct {
    const char *source;
    int source_len;
    const char *module_qn;
    TSTree *cached_tree;            /* NULL = parse internally */
    HYPLSPDef *defs;                /* combined file-local + cross-file defs */
    int def_count;
    const char **import_names;      /* parallel arrays, import_count long */
    const char **import_qns;
    int import_count;
} HYPBatchPHPLSPFile;

/* Process multiple PHP files' cross-file LSP in one call. out must point to
 * file_count pre-zeroed HYPResolvedCallArray structs. */
void hyp_batch_php_lsp_cross(
    HYPArena *arena,
    HYPBatchPHPLSPFile *files, int file_count,
    HYPResolvedCallArray *out);

#endif /* HYP_LSP_PHP_LSP_H */
