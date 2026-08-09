#ifndef HYP_EXTRACT_UNIFIED_H
#define HYP_EXTRACT_UNIFIED_H

#include "hyp.h"
#include "lang_specs.h"

// Scope kinds for the walk state stack.
#define SCOPE_FUNC 1
#define SCOPE_CLASS 2
#define SCOPE_CALL 3
#define SCOPE_IMPORT 4
#define SCOPE_LOOP 5
#define SCOPE_BRANCH 6
#define SCOPE_LEXICAL 7
#define SCOPE_NAMESPACE 8

#define MAX_SCOPES 64
#define INLINE_LEXICAL_SCOPES 64
#define INLINE_LEXICAL_BINDINGS 64
#define INLINE_PYTHON_DIRECTIVES 16

// ObjectScript type map: variable name → class name (for instance_method_call
// resolution). Stack-allocated, per-method scope. Overflow is silent (no crash).
#define OS_TYPE_MAP_CAP 64
typedef struct {
    const char *var_name;
    const char *class_name;
} os_type_entry_t;

typedef struct {
    os_type_entry_t entries[OS_TYPE_MAP_CAP];
    int count;
    int class_base_count; // entries [0,class_base_count) survive method-scope resets
} os_type_map_t;

// A call consumes only the exact AST occurrence that denotes its callee.  The
// rest of the call subtree (receiver, computed key, arguments, callback body)
// remains ordinary expression input and is eligible for USAGE extraction.
typedef enum {
    HYP_INVOCATION_NONE = 0,
    HYP_INVOCATION_PRIMARY,
    HYP_INVOCATION_CALLABLE_REFERENCE,
} HYPInvocationKind;

typedef struct {
    HYPInvocationKind kind;
    TSNode site;
    TSNode callee_expr;
    TSNode callee_leaf;
    const char *callee_name;
    bool raw_call_emitted;
} HYPInvocationDescriptor;

typedef struct {
    const char *qn;
    uint32_t depth;
    uint32_t lexical_scope_id;
    uint8_t kind;
    HYPInvocationKind invocation_kind;
    TSNode callee_expr;
    TSNode callee_leaf;
    /* The complete walk-state tuple this frame displaced, restored verbatim on
     * pop. Saving the full tuple makes push and pop O(1) and kind-agnostic;
     * the previous design recomputed the state by iterating the WHOLE scope
     * stack on every code-bearing node, which is O(depth) per node and turned
     * the deep-nesting torture tests quadratic (0-1s on main, 39-119s here,
     * suite-budget kills on every non-M4 venue). */
    const char *prev_enclosing_func_qn;
    const char *prev_enclosing_class_qn;
    HYPInvocationKind prev_invocation_kind;
    TSNode prev_callee_expr;
    TSNode prev_callee_leaf;
    bool prev_inside_import;
    int prev_loop_depth;
    int prev_branch_depth;
} HYPWalkScope;

typedef enum {
    HYP_LEXICAL_SCOPE_MODULE = 0,
    HYP_LEXICAL_SCOPE_CLASS,
    HYP_LEXICAL_SCOPE_FUNCTION,
    HYP_LEXICAL_SCOPE_BLOCK,
    HYP_LEXICAL_SCOPE_COMPREHENSION,
} HYPLexicalScopeKind;

/* Concrete AST scope identity. QNs remain graph-attribution metadata only;
 * overloads, lambdas and sibling blocks therefore never share binding facts. */
typedef struct {
    uint32_t id;
    uint32_t parent_id;
    uint32_t lookup_parent_id;
    uint32_t start_byte;
    uint32_t end_byte;
    uint8_t kind;
} HYPLexicalScope;

/* Deferred binding event. Applying these after the walk represents hoisted
 * and whole-scope rules without depending on traversal order. */
typedef struct {
    uint32_t scope_id;
    uint32_t active_start;
    uint32_t active_end;
    const char *name;
} HYPLexicalBinding;

typedef enum {
    HYP_PYTHON_DIRECTIVE_GLOBAL = 1,
    HYP_PYTHON_DIRECTIVE_NONLOCAL,
} HYPPythonDirectiveKind;

typedef struct {
    uint32_t function_scope_id;
    const char *name;
    uint8_t kind;
} HYPPythonDirective;

// WalkState tracks scope context during the unified cursor walk.
// Replaces parent-chain walks for enclosing_func_qn, import context, etc.
typedef struct {
    const char *enclosing_func_qn;      // current function QN (module_qn at top level)
    const char *enclosing_class_qn;     // current class QN (NULL outside class)
    const TSTreeCursor *current_cursor; // unified walk cursor at the current node
    TSTreeCursor *occurrence_cursor;    // reusable parent-preserving classifier cursor
    HYPInvocationKind invocation_kind;  // exact active invocation/reference role
    TSNode callee_expr;                 // exact active callee expression, if any
    TSNode callee_leaf;                 // exact active terminal callee, if any
    bool inside_import;                 // within an import_node_types subtree
    int loop_depth;                     // count of enclosing loop scopes (for bottleneck metrics)
    int branch_depth;                   // count of enclosing branch scopes

    HYPArena *arena;
    HYPWalkScope *scopes;
    HYPWalkScope inline_scopes[MAX_SCOPES];
    int scope_capacity;
    int scope_top;

    HYPLexicalScope *lexical_scopes;
    HYPLexicalScope inline_lexical_scopes[INLINE_LEXICAL_SCOPES];
    int lexical_scope_capacity;
    int lexical_scope_count;
    uint32_t root_lexical_scope_id;
    uint32_t split_function_scope_id;
    uint32_t split_signature_start_byte;
    uint32_t split_signature_end_byte;
    const char *split_function_qn;
    uint32_t flat_function_scope_id;
    uint32_t flat_anchor_start_byte;
    uint32_t flat_anchor_end_byte;
    const char *flat_function_qn;

    HYPLexicalBinding *lexical_bindings;
    HYPLexicalBinding inline_lexical_bindings[INLINE_LEXICAL_BINDINGS];
    int lexical_binding_capacity;
    int lexical_binding_count;
    int usage_start_index;
    bool lexical_binding_tracking_failed;
    HYPPythonDirective *python_directives;
    HYPPythonDirective inline_python_directives[INLINE_PYTHON_DIRECTIVES];
    int python_directive_capacity;
    int python_directive_count;
    HYPLanguage language;

    os_type_map_t os_type_map; // ObjectScript variable → type mapping
} WalkState;

// Per-node handler prototypes. Each is called once per node during the
// unified cursor walk, replacing the old recursive walk_* functions.
HYPInvocationDescriptor handle_calls(HYPExtractCtx *ctx, TSNode node, const HYPLangSpec *spec,
                                     WalkState *state);
void handle_usages(HYPExtractCtx *ctx, TSNode node, const HYPLangSpec *spec, WalkState *state);
void hyp_finalize_lexical_usages(HYPExtractCtx *ctx, WalkState *state);
void handle_throws(HYPExtractCtx *ctx, TSNode node, const HYPLangSpec *spec, WalkState *state);
void handle_readwrites(HYPExtractCtx *ctx, TSNode node, const HYPLangSpec *spec, WalkState *state);
void handle_type_refs(HYPExtractCtx *ctx, TSNode node, const HYPLangSpec *spec, WalkState *state);
void handle_env_accesses(HYPExtractCtx *ctx, TSNode node, const HYPLangSpec *spec,
                         WalkState *state);
void handle_type_assigns(HYPExtractCtx *ctx, TSNode node, const HYPLangSpec *spec,
                         WalkState *state);

// Single-pass extraction using TSTreeCursor. Visits every node once,
// dispatching to all handlers per node. Replaces the 7 separate walk_*
// functions for calls/usages/throws/readwrites/type_refs/env_accesses/type_assigns.
// Definitions and imports stay as separate passes (different recursion patterns).
void hyp_extract_unified(HYPExtractCtx *ctx);

#endif // HYP_EXTRACT_UNIFIED_H
