#ifndef HYP_HELPERS_H
#define HYP_HELPERS_H

#include "hyp.h"

// Portable memmem: find first occurrence of `needle` (needle_len bytes) within
// `haystack` (haystack_len bytes). Returns a pointer into haystack, or NULL.
// Hand-rolled so it compiles identically on all platforms (GNU/BSD-only
// memmem is unavailable under msys2-clang on Windows).
void *hyp_memmem(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len);

// Extract text of a node from source. Returns arena-allocated string.
char *hyp_node_text(HYPArena *a, TSNode node, const char *source);

// Check if a string is a language keyword (should be skipped as callee/usage).
bool hyp_is_keyword(const char *name, HYPLanguage lang);

// Check if a name is a builtin we mint a real graph node for, so a CALL to it
// must NOT be keyword-filtered out of call extraction (the LSP resolves it to
// the injected builtin node and forms a CALLS edge). Narrower than
// hyp_is_keyword: it only covers builtins with a target node, so un-filtering
// them cannot produce a node-less / Module-sourced edge. The Python set MUST
// stay in sync with kPyBuiltinNodes in internal/hyp/lsp/py_builtins.c.
bool hyp_is_resolvable_builtin(const char *name, HYPLanguage lang);

// Classify a string literal as URL, config, or neither.
// Returns HYP_STRREF_URL (0), HYP_STRREF_CONFIG (1), or -1 for neither.
int hyp_classify_string(const char *str, int len);

// Check if a name is exported per language convention.
bool hyp_is_exported(const char *name, HYPLanguage lang);

// Check if a file is a test file based on path and language.
bool hyp_is_test_file(const char *rel_path, HYPLanguage lang);

// Find the innermost enclosing function node by walking parent chain.
// Returns a null node if none found.
TSNode hyp_find_enclosing_func(TSNode node, HYPLanguage lang);

// Get the QN of an enclosing function, or module_qn if none.
const char *hyp_enclosing_func_qn(HYPArena *a, TSNode node, HYPLanguage lang, const char *source,
                                  const char *project, const char *rel_path, const char *module_qn);

// Cached version: uses ctx->ef_cache to avoid repeated parent-chain walks.
const char *hyp_enclosing_func_qn_cached(HYPExtractCtx *ctx, TSNode node);

// Max declarator-chain descent depth for C/C++/CUDA/GLSL function-name
// resolution. Single source of truth — extract_defs.c's DECLARATOR_DEPTH_LIMIT
// is derived from this so the three extractors cannot drift.
#define HYP_DECLARATOR_DEPTH_LIMIT 8

// Resolve the function-name node for a C/C++/CUDA/GLSL `function_definition`.
// Such nodes have no `name` field — the name is nested in the declarator chain
// (pointer/function/parenthesized/array declarators wrap it; out-of-line method
// definitions name it with a qualified_identifier). Descends the `declarator`
// field to the innermost name node and returns it, or a null node if none is
// found. Shared by the defs, calls, and unified extractors so all three agree on
// enclosing-function attribution — drift between private copies caused #438.
TSNode hyp_resolve_c_declarator_name_node(TSNode func_node);

// Convert a resolved function/method name node to its name string, normalizing a
// C++ conversion-operator's `operator_cast` node (which spans the full
// "operator bool() const") down to "operator bool". Shared by the defs and
// unified extractors so the def name and call-scope QN agree.
// Also strips the surrounding quotes from a Nix quoted attrpath segment, so
// `"kebab-case" = a: a;` is named kebab-case rather than "kebab-case". Takes the
// language for that reason; every caller must pass ctx->language.
char *hyp_func_name_node_text(HYPArena *a, TSNode name_node, const char *source, HYPLanguage lang);

// ── Nix attrpath helpers ──
// A Nix binding's name is a PATH (`a.b.c = …`) whose segments may be quoted or
// interpolated. Shared by the defs and unified (call-scope) extractors so both
// derive the same name and the same scope prefix — divergence makes a CALLS edge
// name a source node that does not exist, and it is dropped at write.

// Strip one matching pair of surrounding double quotes, in place.
void hyp_nix_strip_attr_quotes(char *text);

// True when an attrpath segment contains a `${...}` interpolation and therefore
// has no statically knowable name.
bool hyp_nix_attr_is_interpolated(TSNode attr);

// The leaf segment of an attrpath — the name. Null node for an empty attrpath.
TSNode hyp_nix_attrpath_last_attr(TSNode attrpath);

// The scope prefix of an attrpath: all segments but the leaf, quote-stripped and
// dot-joined, so `a.b.fn = …` qualifies identically to `a = { b = { fn = …; }; }`.
// NULL for a single-segment path, or when a leading segment is interpolated.
const char *hyp_nix_attrpath_scope(HYPArena *a, TSNode attrpath, const char *source);

// True when a Nix `binding`'s value is an attribute set — the binding names a
// scope rather than defining a value. Excludes let-bindings and lambda values.
bool hyp_nix_binding_is_attrset_scope(TSNode node);

// The scope QN contributed by a Nix `binding` whose value is an attribute set.
// Called by BOTH extract_defs.c and extract_unified.c, which carry separate
// compute_class_qn implementations — sharing this makes a def/call-scope QN
// mismatch (which silently drops the CALLS edge) structurally impossible.
const char *hyp_nix_binding_scope_qn(HYPExtractCtx *ctx, TSNode node, const char *saved_enclosing);

// The QN-relative name of a Nix binding — its attrpath scope joined to `name`.
// Callers prepend the enclosing attrset scope (or the module QN), so a dotted
// attrpath and an enclosing attrset compose into one qualified name.
const char *hyp_nix_qn_name(HYPArena *a, TSNode func_node, const char *source, const char *name);

// Resolve a function/method definition node's NAME node across all ~130 grammars
// (generic `name` field, arrow→declarator, C/C++ declarator chain, plus the many
// per-language quirks: Fortran subroutine, SCSS mixin, SQL create_function, R,
// PowerShell, Ada, the Lisp/FP family, etc.). Defined in extract_defs.c. Shared by
// the defs, calls, and unified extractors so all three agree on enclosing-function
// naming — drift between private copies caused the Module-mis-attribution of
// gap #3 (and #438 for the C-declarator case).
TSNode hyp_resolve_func_name(TSNode node, HYPLanguage lang);

// C++/CUDA out-of-line method definition (`void Foo::bar() {...}`): return the
// immediate enclosing class name ("Foo") from the qualified declarator, or NULL
// for a plain free function. Defined in extract_defs.c. Shared so the unified
// (call-scope) extractor computes the SAME class-qualified enclosing QN as the
// def extractor — drift dropped the class qualifier from in-body calls (#554/#621).
char *hyp_cpp_out_of_line_parent_class(HYPArena *a, TSNode node, const char *source);

// Find a child node by kind string.
TSNode hyp_find_child_by_kind(TSNode parent, const char *kind);

// Check if node kind matches a set of types (NULL-terminated array of strings).
bool hyp_kind_in_set(TSNode node, const char **types);

/* Namespace/module declarations that extend a qualified-name scope without
 * turning their children into class methods. Shared by definition and unified
 * walks so TS/TSX scope attribution cannot drift. */
bool hyp_is_namespace_scope_kind(HYPLanguage lang, const char *kind);

// Free the calling thread's hyp_kind_in_set bitset cache (call at thread/process
// teardown so the thread-local cache is not reported as a leak).
void hyp_kind_in_set_free_cache(void);

// Check if node has an ancestor of the given kind, within max_depth levels.
bool hyp_has_ancestor_kind(TSNode node, const char *kind, int max_depth);

// Count nodes of given kinds in subtree (for complexity metric).
int hyp_count_branching(TSNode node, const char **branching_types);

// Per-function structural complexity, computed in a single AST walk.
typedef struct {
    int cyclomatic;       // branching-node count (matches def.complexity)
    int cognitive;        // nesting-weighted flow-break count (Campbell-style approximation)
    int loop_count;       // total loop constructs in the body
    int loop_depth;       // maximum nested-loop depth — structural bottleneck proxy
    int max_access_depth; // deepest chained member/subscript access (a.b.c.d → 4) — structure smell
} hyp_complexity_t;

// Compute the metrics above in one traversal of `node`'s subtree.
// `branching_types` is the language's branching node-type set.
void hyp_compute_complexity(TSNode node, const char **branching_types, hyp_complexity_t *out);

// Is `kind` a loop construct node type? Language-agnostic curated set (for/while/
// do/foreach/repeat/loop variants). Exposed so the unified walk can track loop
// nesting at call sites without re-deriving the set.
bool hyp_is_loop_node_type(const char *kind);

// Is this a module-level node? (not nested inside function/class body)
bool hyp_is_module_level(TSNode node, HYPLanguage lang);

// Same check, but the node's PARENT is supplied directly — avoids the
// O(n) ts_node_parent rescan. Use at call sites iterating a known
// parent's children (the common case). `parent` is the parent of the
// node being classified.
bool hyp_is_module_level_p(TSNode parent, HYPLanguage lang);

// --- FQN computation ---

// Compute qualified name: project.rel_path_parts.name
char *hyp_fqn_compute(HYPArena *a, const char *project, const char *rel_path, const char *name);

// Module QN (file without name): project.rel_path_parts
char *hyp_fqn_module(HYPArena *a, const char *project, const char *rel_path);

// Language-aware module QN. For directory-module languages (Java package, Go
// package) the module is derived from the CONTAINING DIRECTORY (the filename
// stem is NOT baked in): `Outer.java` at root -> "proj", `myapp/db/conn.go` ->
// "proj.myapp.db". For every OTHER language this returns exactly what
// hyp_fqn_module returns (no behavior change).
char *hyp_fqn_module_source_lang(HYPArena *a, const char *project, const char *rel_path,
                                 HYPLanguage lang);

// Language-aware symbol QN. For directory-module languages this is the
// directory-based module + "." + name (so a top-level class `Outer` in
// `Outer.java` is "proj.Outer", not "proj.Outer.Outer"). For every other
// language this is exactly hyp_fqn_compute (no behavior change).
char *hyp_fqn_compute_source_lang(HYPArena *a, const char *project, const char *rel_path,
                                  const char *name, HYPLanguage lang);

// Folder QN: project.dir_parts
char *hyp_fqn_folder(HYPArena *a, const char *project, const char *rel_dir);

/* Flatten a JS/TS `template_string` node into plain text: string fragments are
 * kept verbatim and each ${...} substitution becomes the "{}" placeholder, so
 * client-side URLs built from template literals share the canonical parameter
 * shape that server-side route paths already use. NULL when empty/oversized. */
const char *hyp_template_string_text(HYPArena *a, TSNode node, const char *source);

#endif // HYP_HELPERS_H
