#include "foundation/constants.h"
/*
 * pipeline_internal.h — Internal pipeline state shared between pass files.
 *
 * NOT a public header. Only included by pipeline.c and pass_*.c files.
 * Exposes the pipeline context struct for direct field access by passes.
 */
#ifndef HYP_PIPELINE_INTERNAL_H
#define HYP_PIPELINE_INTERNAL_H

#include "pipeline/pipeline.h"
#include "pipeline/path_alias.h"
#include "graph_buffer/graph_buffer.h"
#include "store/store.h"
#include "discover/discover.h"
#include "discover/userconfig.h"
#include "git/git_context.h"
#include "foundation/hash_table.h"
#include "hyp.h"
#include "lsp/go_lsp.h" /* HYPLSPDef for hyp_parallel_resolve cross-LSP inputs */
#include <stdatomic.h>
#include <string.h>

/* ── Shared pipeline constants ─────────────────────────────────── */

/* Per-file WALL-CLOCK ceiling on the tree-sitter parse, in MICROSECONDS --
 * the unit hyp_extract_file()'s timeout_micros parameter takes. It bounds
 * ts_parser_parse_with_options() and nothing else: not the extraction walks
 * that follow it, and not the second parse of preprocessed C/C++ source,
 * which passes pp_opts = {0} and has no ceiling at all. On a large C file
 * the parse this does bound is about a tenth of what the file costs.
 *
 * It is NOT a byte cap. hyp_max_file_bytes() is the byte cap; it reports
 * under its own "oversized" skip phase and has nothing to do with this.
 *
 * It is NOT the hang guard either. The index supervisor kills a worker that
 * emits no progress for 900 seconds and quarantines the file it was on. This
 * ceiling exists so that ONE pathological parse costs ONE skipped file
 * rather than a killed worker, which gives it an UPPER bound as well as a
 * lower one: progress is logged every PP_LOG_INTERVAL completed files and
 * MIN_WORKERS is 1, so a single-worker run -- a supported shape -- has to fit
 * PP_LOG_INTERVAL consecutive worst-case files inside that window.
 * 10 x ceiling < 900 s, so the ceiling stays under 90 seconds; 60 keeps 1.5x
 * under the derived bound.
 *
 * The lower bound is the slowest LEGITIMATE parse on the slowest supported
 * runner, because expiry returns an EMPTY result: no definitions for the
 * file, a skip that is not fatal, and a run that still reports success. A
 * ceiling an ordinary file can reach is a hole in the graph whose size
 * depends on how fast the machine is, and nothing in the answer says so.
 *
 * Measured with the ceiling lifted so every parse ran to completion, over
 * this repository's own history. On the slowest supported runner
 * (ubuntu-24.04-arm, 4 vCPU, the ASan+UBSan build the test venue uses) the
 * slowest raw parse is 28.6 s for a 447 KB file; src/mcp/mcp.c measures 22.0
 * s median over 18 observations with a 1.32x spread, and throughput sits
 * between 14 and 37 kB/s across 49 parses. The same 447 KB file parses in
 * 430 ms on an x86 runner of the same size and generation. That is 62x to
 * 66x median-to-median on files with 18 and 3 paired observations, so it is
 * a property of the machine rather than a spike.
 *
 * The bracket is therefore [28.6 s, 90 s]: a factor of 3.1 between the
 * slowest legitimate parse and the structural ceiling. Sixty seconds is 2.1x
 * over the first and 1.5x under the second, and NEITHER margin is
 * comfortable -- no value inside a 3.1x bracket is. At that runner's
 * throughput this admits files up to roughly 1.3 MB, and three files in this
 * tree are larger, the largest by two orders of magnitude. Without the
 * sanitizer the same ceiling admits several times more: instrumentation
 * costs 3.1x to 5.6x on identical input.
 *
 * Wall clock is the wrong unit and this value only buys room. Parse cost is
 * linear in bytes (r = 0.98 across the measured set), so the ceiling is a
 * file-size cap scaled by machine speed; adversarial C -- deep nesting,
 * whole-file error recovery, dense declaration ambiguity, huge single
 * expressions, stacked casts -- parses within a factor of 4 of ordinary
 * source, while the same file costs more than 11x more on a slow runner than
 * a fast one. The spread this cannot distinguish from sickness is larger than
 * any sickness measured, so what it still catches is a parse whose throughput
 * has collapsed by orders of magnitude, and nothing subtler. The guard that
 * would be right is the supervisor's own shape one level down: TSParseState
 * carries current_byte_offset, so the progress callback can abort a parse
 * that has stopped ADVANCING rather than one that is merely slow. */
#define HYP_PARSE_TIMEOUT_US 60000000

/* Route node QN buffer size (must fit __route__METHOD__/full/url/path) */
#define HYP_ROUTE_QN_SIZE 768

/* Incremental integrity failure: abort the run and preserve the existing DB.
 * Distinct from HYP_NOT_FOUND, which the orchestrator uses as the normal
 * "no incremental route; continue with a full index" sentinel. */
#define HYP_PIPELINE_ABORT_PRESERVE_DB (-2)
#define HYP_PIPELINE_FORCE_FULL_REINDEX (-3)
#define HYP_PIPELINE_PERSIST_FAILED (-4)

/* Canonicalize route-path parameter placeholders (":id", "{id}", "<id>",
 * "${...}") to a single "{}" token so that client call sites and server
 * handlers rendezvous on the same Route QN regardless of framework syntax.
 * Parameter names are intentionally discarded ("/u/{id}" and "/u/{slug}" both
 * canonicalize to "/u/{}"). The result never exceeds the input length, so
 * out_sz >= strlen(in) + 1 always suffices. Returns out. */
const char *hyp_route_canon_path(const char *in, char *out, size_t out_sz);

/* True when a graph node is a structural directory container (Folder/Project)
 * rather than a code node. In a directory-based-module language (Java/Go, see
 * hyp_lang_module_is_dir) a file's module QN equals its directory QN, so an
 * enclosing-scope lookup for a CLASS-LEVEL usage/call (enclosing_func_qn ==
 * module_qn) resolves to the ONE Folder/Project node shared by every file in
 * that package. Sourcing an edge there conflates all same-package files into a
 * single source node with an arbitrary file_path (#787). Source-node finders
 * must treat such a hit as a miss and fall back to the per-file File node. */
static inline bool hyp_pipeline_node_is_dir_container(const hyp_gbuf_node_t *node) {
    return node && node->label &&
           (strcmp(node->label, "Folder") == 0 || strcmp(node->label, "Project") == 0);
}

/* Time unit conversions */
#define HYP_NS_PER_SEC 1000000000LL
#define HYP_US_PER_SEC 1000000LL
#define HYP_MS_PER_SEC 1000.0
#define HYP_US_PER_SEC_F 1e6

/* ── Pipeline context (internal) ─────────────────────────────────── */

/* Per-worker manifest collection entry. */
typedef struct {
    char *pkg_name;  /* heap: "@myorg/pkg", "github.com/foo/bar" */
    char *entry_rel; /* heap: "packages/pkg/src/index" (no extension) */
} hyp_pkg_entry_t;

/* Growable array of package entries (per-worker, no thread contention). */
typedef struct {
    hyp_pkg_entry_t *items;
    int count;
    int cap;
} hyp_pkg_entries_t;

void hyp_pkg_entries_init(hyp_pkg_entries_t *e);
void hyp_pkg_entries_free(hyp_pkg_entries_t *e);

/* Shared context passed to each pass function.
 * Derived from hyp_pipeline_t fields during run. */
typedef struct {
    const char *project_name; /* borrowed from pipeline */
    const char *repo_path;    /* borrowed from pipeline */
    hyp_gbuf_t *gbuf;         /* owned by pipeline */
    hyp_registry_t *registry; /* owned by pipeline */
    atomic_int *cancelled;    /* pointer to pipeline's cancelled flag */
    hyp_pipeline_t *pipeline; /* back-pointer for recording per-file skips
                               * (Stage 2 / Track B). May be NULL on paths that
                               * don't record; hyp_pipeline_add_file_error is
                               * NULL-safe. */
    int mode;                 /* hyp_index_mode_t (0=full, 1=moderate, 2=fast, 3=advanced) */

    /* Extraction result cache (sequential pipeline optimization).
     * When non-NULL, pass_definitions stores results here instead of freeing,
     * and pass_calls/usages/semantic reuse cached results instead of re-extracting.
     * Indexed by file position in the files[] array. Owned by pipeline.c. */
    HYPFileResult **result_cache;

    /* Build-tool path aliases (tsconfig/jsconfig today; webpack/vite-style
     * configs are an easy follow-on). NULL when no usable configs were found.
     * Owned by pipeline.c / pipeline_incremental.c. */
    const hyp_path_alias_collection_t *path_aliases;

    /* Directory subtrees excluded during discovery. Borrowed from pipeline.c. */
    char **excluded_dirs;
    int excluded_count;

    /* Sequential cross-LSP registry arena. The lsp_cross pass builds its
     * shared per-language registries here; resolved_calls entries may BORROW
     * strings owned by these registries, and the later calls pass still
     * reads them — so the arena is OWNED and destroyed by
     * run_sequential_pipeline AFTER all passes, never by the lsp_cross pass
     * itself (destroying at pass end was a use-after-free in pass_calls).
     * Mirrors the parallel path, where cross_lsp_arena outlives the fused
     * resolve. */
    HYPArena seq_cross_arena;
    bool seq_cross_arena_live;
    /* Sequential lsp_cross only: the per-file module-QN strings the collected
     * defs (and through them the shared cross registries in seq_cross_arena)
     * borrow. The registries outlive the pass so pass_calls can read borrowed
     * strings -- these must too. Ownership transfers here at the end of the
     * pass; released beside the arena. Freeing them at pass end was a
     * use-after-free first observable on the real-repo corpus tier. */
    char **seq_cross_def_modules;
    int seq_cross_def_module_count;

    /* ObjectScript $$$macro table built from .inc files in the repo (NULL if
     * no ObjectScript include files were found). Owned by pipeline.c. */
    const HYPMacroTable *macro_table;

    /* ObjectScript method-return-type table built from extracted definitions
     * (NULL until pass_calls builds it). Owned by pipeline.c. */
    const HYPReturnTypeTable *return_type_table;
} hyp_pipeline_ctx_t;

/* Transcode an ObjectScript Studio Export XML file and compose every generated
 * UDL class into one cacheable result. The returned result owns all child
 * extraction arenas and is released with the ordinary hyp_free_result(). */
HYPFileResult *hyp_pipeline_extract_objectscript_export(
    const char *source, int source_len, const char *project_name, const char *rel_path,
    const HYPMacroTable *macro_table, const HYPReturnTypeTable *return_type_table);

/* Materialize CONFIGURES edges from one extracted file's env-access carriers.
 * Shared by sequential definition processing and the parallel cache registry. */
int hyp_pipeline_create_env_configures_for_file(hyp_pipeline_ctx_t *ctx,
                                                const HYPFileResult *result, const char *rel);

static inline int hyp_pipeline_relpath_is_excluded(const char *rel_path, char *const *excluded_dirs,
                                                   int excluded_count) {
    if (!rel_path || rel_path[0] == '\0' || !excluded_dirs || excluded_count <= 0) {
        return 0;
    }
    for (int i = 0; i < excluded_count; i++) {
        const char *excluded = excluded_dirs[i];
        if (!excluded || excluded[0] == '\0') {
            continue;
        }
        size_t n = strlen(excluded);
        if (strncmp(rel_path, excluded, n) == 0 && (rel_path[n] == '\0' || rel_path[n] == '/')) {
            return SKIP_ONE;
        }
    }
    return 0;
}

/* Get the current pipeline's package map (NULL if none). */
HYPHashTable *hyp_pipeline_get_pkgmap(void);
void hyp_pipeline_set_pkgmap(HYPHashTable *map);

/* Unified module resolver: relative → pkgmap → fqn_module fallback.
 * Handles bare specifiers via pkgmap lookup with prefix matching.
 * Caller must free() the returned string. */
char *hyp_pipeline_resolve_module(const hyp_pipeline_ctx_t *ctx, const char *source_rel,
                                  const char *module_path);

/* Resolve an import to its in-graph target node, or NULL if unresolvable.
 *
 * Resolution order (first hit wins):
 *   1. Module-path resolution (relative / pkgmap / fqn_module) → existing node.
 *      This preserves the behavior for Python/TS/Go whose module path maps
 *      directly to a sibling Module/File QN.
 *   2. namespace_map[module_path-prefix] → File node QN (Java/Kotlin/C#/PHP
 *      `using`/`import` of a NAMESPACE that the path-based QN cannot express).
 *   3. Symbol-name fallback: the import's last path segment matched against an
 *      in-graph definition node of the same simple name in a different file
 *      (Rust `use crate::util::helper`, Java `import com.example.Util`, ...).
 *
 * `namespace_map` may be NULL (skips step 2).  `source_file_qn` is the importing
 * file's __file__ QN, used to avoid self-imports in step 3. */
const hyp_gbuf_node_t *hyp_pipeline_resolve_import_node(const hyp_pipeline_ctx_t *ctx,
                                                        const char *source_rel,
                                                        const char *source_file_qn,
                                                        const HYPImport *imp,
                                                        HYPHashTable *namespace_map);

/* Build a namespace → File-node-QN map from a set of extraction results.
 * Each result that declared a namespace/package contributes one entry keyed by
 * the namespace string (e.g. "App.Utils", "com.example").  Returns NULL when no
 * results declared a namespace.  Caller frees via hyp_pipeline_namespace_map_free. */
HYPHashTable *hyp_pipeline_namespace_map_build(const char *project_name,
                                               HYPFileResult *const *results,
                                               const char *const *rels, int count);
void hyp_pipeline_namespace_map_free(HYPHashTable *map);

/* Parse a manifest file and collect pkg entries. Returns true if basename matched. */
bool hyp_pkgmap_try_parse(const char *basename, const char *rel_path, const char *source,
                          int source_len, hyp_pkg_entries_t *entries);

/* Merge per-worker entries into a hash table. Returns NULL if no entries. */
HYPHashTable *hyp_pkgmap_build(hyp_pkg_entries_t *worker_entries, int worker_count,
                               const char *project_name);

/* Build pkgmap by reading manifest files from the files array (sequential path). */
int hyp_pkgmap_scan_repo(const char *repo_path, hyp_pkg_entries_t *entries, char **excluded_dirs,
                         int excluded_count);
HYPHashTable *hyp_pkgmap_build_from_repo(const char *repo_path, const hyp_file_info_t *files,
                                         int file_count, const char *project_name,
                                         char **excluded_dirs, int excluded_count);
HYPHashTable *hyp_pkgmap_build_from_files(const hyp_file_info_t *files, int file_count,
                                          const char *project_name);

/* Free pkgmap and all owned strings. */
void hyp_pkgmap_free(HYPHashTable *pkgmap);

/* Check cancellation. Returns non-zero if cancelled. */
static inline int hyp_pipeline_check_cancel(const hyp_pipeline_ctx_t *ctx) {
    return atomic_load(ctx->cancelled) ? HYP_NOT_FOUND : 0;
}

/* ── Testable helpers ────────────────────────────────────────────── */

/* Check if a file path is worth tracking for git history analysis. */
bool hyp_is_trackable_file(const char *path);

/* Check if a file path looks like a test file (language-agnostic). */
bool hyp_is_test_path(const char *path);

/* Check if a function name looks like a test function (language-agnostic). */
bool hyp_is_test_func_name(const char *name);

/* Coupling result from computeChangeCoupling */
typedef struct {
    char file_a[HYP_SZ_512];
    char file_b[HYP_SZ_512];
    int co_change_count;
    double coupling_score;
    /* Unix epoch of the most recent commit that touched both files together.
     * 0 when no timestamp was available (e.g. older callers / popen path
     * without %ct). */
    long long last_co_change;
} hyp_change_coupling_t;

/* Commit data for coupling analysis */
typedef struct {
    char **files;
    int count;
    /* Unix epoch of the commit. 0 means unknown — coupling computation
     * still works but last_co_change on the resulting edge will be 0. */
    long long timestamp;
} hyp_commit_files_t;

/* Per-file temporal metadata. Populated alongside change-coupling so File
 * nodes can carry change_count and last_modified for hotspot / risk
 * analysis queries. */
typedef struct {
    char file_path[HYP_SZ_512];
    int change_count;
    long long last_modified; /* unix epoch of most recent commit */
} hyp_file_temporal_t;

/* Compute change coupling from commit history.
 * Returns number of couplings written to out (up to max_out).
 * Caller owns out[]. */
int hyp_compute_change_coupling(const hyp_commit_files_t *commits, int commit_count,
                                hyp_change_coupling_t *out, int max_out);

/* Go-style implicit interface satisfaction on graph buffer.
 * Finds Interface nodes, matches method sets against Class nodes,
 * creates IMPLEMENTS + OVERRIDE edges. Returns edge count created. */
int hyp_pipeline_implements_go(hyp_pipeline_ctx_t *ctx);

/* Edge type for an explicit base-class relation, keyed off the resolved
 * TARGET node's label: Interface → IMPLEMENTS, anything else → INHERITS.
 * The single decision point for BOTH the sequential semantic pass and the
 * parallel per-file resolve — the two venues must never diverge. */
const char *hyp_semantic_base_edge_type(const hyp_gbuf_node_t *base_node);

/* Explicit-language override detection on the full graph (serial tail).
 * For every IMPLEMENTS/INHERITS edge whose source is a non-Go class, matches
 * the class's DEFINES_METHOD children by name against the base's and creates
 * Method→Method OVERRIDE edges (Java @Override, TS/C#/Kotlin override, PHP
 * redefinition). Go is excluded: implicit satisfaction already covers it.
 * Returns edge count created. */
int hyp_pipeline_override_explicit(hyp_pipeline_ctx_t *ctx);

/* ── Git diff helpers (pass_gitdiff.c) ───────────────────────────── */

typedef struct {
    char status[HYP_SZ_4]; /* M/A/D/R */ /* "M", "A", "D", "R" */
    char path[HYP_SZ_512];
    char old_path[HYP_SZ_512]; /* non-empty only for renames */
} hyp_changed_file_t;

/* hyp_changed_hunk_t + hyp_parse_hunks moved to pipeline.h (public — consumed
 * by src/mcp/mcp.c's detect_changes for line-scoped seed detection). Visible
 * here via the `#include "pipeline/pipeline.h"` above. */

/* Parse git diff --name-status output. Returns count written to out. */
int hyp_parse_name_status(const char *output, hyp_changed_file_t *out, int max_out);

/* Parse "start,count" or "start" → (start, count). */
void hyp_parse_range(const char *s, int *out_start, int *out_count);

/* ── Config helpers (pass_configures.c) ──────────────────────────── */

/* Check if a string looks like an environment variable name
 * (uppercase + underscore + digits, at least 2 chars with uppercase). */
bool hyp_is_env_var_name(const char *s);

/* Normalize a config key: split camelCase/snake/dots, lowercase.
 * Writes normalized form to norm_out (underscore-joined).
 * Returns token count. tokens_out[] receives borrowed pointers into norm_out. */
int hyp_normalize_config_key(const char *key, char *norm_out, size_t norm_sz);

/* Check if a file path has a config file extension (.toml, .yaml, .env, etc.) */
bool hyp_has_config_extension(const char *path);

/* ── Enrichment helpers (pass_enrichment.c) ──────────────────────── */

/* Split camelCase string on lowercase→uppercase transitions.
 * Writes substrings to out[]. Returns count. Caller must free each out[i]. */
int hyp_split_camel_case(const char *s, char **out, int max_out);

/* Tokenize a decorator into lowercase words, filtering stopwords.
 * E.g. "@login_required" → ["login", "required"].
 * Writes words to out[]. Returns count. Caller must free each out[i]. */
int hyp_tokenize_decorator(const char *dec, char **out, int max_out);

/* ── Compile commands helpers (pass_compile_commands.c) ──────────── */

typedef struct {
    char **include_paths;
    int include_count;
    char **defines;
    int define_count;
    char standard[HYP_SZ_32];
} hyp_compile_flags_t;

/* Split a shell command string into arguments (handles quoting).
 * Writes args to out[]. Returns count. Caller must free each out[i]. */
int hyp_split_command(const char *cmd, char **out, int max_out);

/* Extract -I, -isystem, -D, -std= flags from compiler arguments.
 * Caller must free result with hyp_compile_flags_free(). */
hyp_compile_flags_t *hyp_extract_flags(const char **args, int argc, const char *directory);

/* Free a compile_flags_t allocated by hyp_extract_flags(). */
void hyp_compile_flags_free(hyp_compile_flags_t *f);

/* Parse compile_commands.json content. Returns map as parallel arrays.
 * out_paths[i] is the relative file path, out_flags[i] is its flags.
 * Returns count. Caller must free out_paths[i] and hyp_compile_flags_free(out_flags[i]). */
int hyp_parse_compile_commands(const char *json_data, const char *repo_path, char ***out_paths,
                               hyp_compile_flags_t ***out_flags);

/* ── Infrascan helpers (pass_infrascan.c) ─────────────────────────── */

/* File identification helpers */
bool hyp_is_dockerfile(const char *name);
bool hyp_is_compose_file(const char *name);
bool hyp_is_cloudbuild_file(const char *name);
bool hyp_is_env_file(const char *name);
bool hyp_is_shell_script(const char *name, const char *ext);
bool hyp_is_kustomize_file(const char *name);
bool hyp_is_k8s_manifest(const char *name, const char *content);

/* Secret detection */
bool hyp_is_secret_binding(const char *key, const char *value);
bool hyp_is_secret_value(const char *value);

/* Clean JSON array brackets from CMD/ENTRYPOINT values.
 * E.g. ["./app", "--flag"] → ./app --flag
 * Writes result to out (up to out_sz). */
void hyp_clean_json_brackets(const char *s, char *out, size_t out_sz);

/* Key-value pair for environment variables / config entries */
typedef struct {
    char key[HYP_SZ_128];
    char value[HYP_SZ_512];
} hyp_env_kv_t;

/* Dockerfile parsing result */
typedef struct {
    char base_image[HYP_SZ_256];
    char stage_images[HYP_SZ_16][HYP_SZ_256];
    char stage_names[HYP_SZ_16][HYP_SZ_128];
    int stage_count;
    char exposed_ports[HYP_SZ_16][HYP_SZ_32];
    int port_count;
    hyp_env_kv_t env_vars[HYP_SZ_64];
    int env_count;
    char build_args[HYP_SZ_32][HYP_SZ_128];
    int build_arg_count;
    char workdir[HYP_SZ_256];
    char cmd[HYP_SZ_512];
    char entrypoint[HYP_SZ_512];
    char healthcheck[HYP_SZ_512];
    char user[HYP_SZ_64];
} hyp_dockerfile_result_t;

/* Dotenv parsing result */
typedef struct {
    hyp_env_kv_t env_vars[HYP_SZ_64];
    int env_count;
} hyp_dotenv_result_t;

/* Shell script parsing result */
typedef struct {
    char shebang[HYP_SZ_256];
    hyp_env_kv_t env_vars[HYP_SZ_64];
    int env_count;
    char sources[HYP_SZ_16][HYP_SZ_256];
    int source_count;
    char docker_cmds[HYP_SZ_16][HYP_SZ_256];
    int docker_cmd_count;
} hyp_shell_result_t;

/* Terraform variable */
typedef struct {
    char name[HYP_SZ_128];
    char type[HYP_SZ_64];
    char default_val[HYP_SZ_256];
    char description[HYP_SZ_256];
} hyp_tf_variable_t;

/* Terraform resource / data source */
typedef struct {
    char type[HYP_SZ_128];
    char name[HYP_SZ_128];
} hyp_tf_resource_t;

/* Terraform module */
typedef struct {
    char tf_name[HYP_SZ_128];
    char source[HYP_SZ_256];
} hyp_tf_module_t;

/* Terraform parsing result */
typedef struct {
    hyp_tf_resource_t resources[HYP_SZ_32];
    int resource_count;
    hyp_tf_variable_t variables[HYP_SZ_32];
    int variable_count;
    char outputs[HYP_SZ_32][HYP_SZ_128];
    int output_count;
    char providers[HYP_SZ_16][HYP_SZ_128];
    int provider_count;
    hyp_tf_module_t modules[HYP_SZ_16];
    int module_count;
    hyp_tf_resource_t data_sources[HYP_SZ_16];
    int data_source_count;
    char backend[HYP_SZ_128];
    bool has_locals;
} hyp_terraform_result_t;

/* Parse a Dockerfile from source text. Returns 0 if parsed, -1 if empty/invalid. */
int hyp_parse_dockerfile_source(const char *source, hyp_dockerfile_result_t *out);

/* Parse a .env file from source text. Returns 0 if parsed, -1 if empty. */
int hyp_parse_dotenv_source(const char *source, hyp_dotenv_result_t *out);

/* Parse a shell script from source text. Returns 0 if parsed, -1 if empty. */
int hyp_parse_shell_source(const char *source, hyp_shell_result_t *out);

/* Parse a Terraform file from source text. Returns 0 if parsed, -1 if empty. */
int hyp_parse_terraform_source(const char *source, hyp_terraform_result_t *out);

/* Helm Chart.yaml parse result: chart name + dependency chart names (#338). */
enum { HYP_HELM_MAX_DEPS = 128, HYP_HELM_NAME_MAX = 128 };
typedef struct {
    char chart_name[HYP_HELM_NAME_MAX];
    char deps[HYP_HELM_MAX_DEPS][HYP_HELM_NAME_MAX];
    int dep_count;
} hyp_helm_chart_t;

/* Parse a Helm Chart.yaml: top-level `name:` and `dependencies:` list names.
 * Returns 0 if parsed (name or deps found), -1 otherwise. */
int hyp_parse_helm_chart(const char *source, hyp_helm_chart_t *out);

/* Build an infrastructure QN. Caller must free the returned string. */
char *hyp_infra_qn(const char *project_name, const char *rel_path, const char *infra_type,
                   const char *service_name);

/* ── Parallel pipeline prototypes (pass_parallel.c) ─────────────── */

/* Phase 3A: Parallel extract + create definition nodes.
 * Each worker creates nodes in a per-worker gbuf, then merges into ctx->gbuf.
 * Caches HYPFileResult* in result_cache[file_idx] for reuse in Phase 3B/4.
 * shared_ids provides globally unique node/edge IDs across workers. */

/* Source-retention tuning for hyp_parallel_extract_ex. Zero-valued byte caps
 * mean "use the derived default" (RAM-fraction total, clamped to an absolute
 * ceiling; modest per-file cap); HYP_RETAIN_TOTAL_MB / HYP_RETAIN_PER_FILE_MB
 * override those. retain_sources_set=false keeps the default retain policy. */
typedef struct {
    bool retain_sources;
    bool retain_sources_set; /* false keeps the default retain_sources policy */
    size_t retain_total_budget_bytes;
    size_t retain_per_file_max_bytes;
} hyp_parallel_extract_opts_t;

int hyp_parallel_extract_ex(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files, int file_count,
                            HYPFileResult **result_cache, _Atomic int64_t *shared_ids,
                            int worker_count, const hyp_parallel_extract_opts_t *opts);
int hyp_parallel_extract(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files, int file_count,
                         HYPFileResult **result_cache, _Atomic int64_t *shared_ids,
                         int worker_count);

/* Phase 3B: Serial registry build from cached extraction results.
 * Creates DEFINES, DEFINES_METHOD, and IMPORTS edges in ctx->gbuf.
 * Registers callable symbols (Function/Method/Class) in ctx->registry. */
int hyp_build_registry_from_cache(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files,
                                  int file_count, HYPFileResult **result_cache);

/* Phase 4: Parallel call/usage/semantic resolution.
 * Each worker resolves calls, usages, throws, rw, inherits, decorates,
 * and implements edges into per-worker edge bufs, then merges.
 * Runs Go-style implicit IMPLEMENTS as serial post-step. */
/* Opaque module-def index — defined in pass_lsp_cross.c. Forward-declared
 * here so we can include it in hyp_parallel_resolve's signature without
 * pulling the pass header into every consumer of pipeline_internal.h. */
struct HYPModuleDefIndex;

/* hyp_parallel_resolve's cross_registries param is typed `void*` to avoid
 * pulling lsp/go_lsp.h into every TU that includes pipeline_internal.h.
 * Callers cast a HYPCrossLspRegistries* (defined in pass_lsp_cross.h). */

int hyp_parallel_resolve(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files, int file_count,
                         HYPFileResult **result_cache, _Atomic int64_t *shared_ids,
                         int worker_count,
                         /* Cross-file LSP inputs — pre-built once by the caller and
                          * shared read-only across workers (typed non-const to match
                          * the existing hyp_run_X_lsp_cross signatures the resolve
                          * worker forwards them to). Pass NULL/0/NULL to skip. */
                         HYPLSPDef *all_defs, int def_count, char *const *def_modules,
                         /* Optional inverted index module_qn → defs[] — fallback
                          * path when there's no pre-built registry for this lang. */
                         struct HYPModuleDefIndex *module_def_index,
                         /* Optional Tier 2 full: pre-built per-language registries.
                          * For each language with a non-NULL entry, workers use the
                          * hyp_run_X_lsp_cross_with_registry fast path (skip per-
                          * file registry build entirely). Falls back to the filter
                          * + per-file build path when entry is NULL or struct is NULL.
                          * Typed as void* here to dodge the typedef/tag ordering
                          * problem — pass_parallel.c casts back to HYPCrossLspRegistries*. */
                         void *cross_registries);

/* Post-merge: create Route nodes for HTTP_CALLS/ASYNC_CALLS edges that
 * have url_path in properties but point to library functions instead of routes.
 * Re-targets these edges to Route nodes for cross-service traversal. */
void hyp_pipeline_create_route_nodes(hyp_gbuf_t *gb);

/* ── Pass function prototypes ────────────────────────────────────── */

int hyp_pipeline_pass_definitions(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files,
                                  int file_count);

int hyp_pipeline_pass_k8s(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files, int file_count);

int hyp_pipeline_pass_calls(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files, int file_count);

/* Cross-file LSP type-aware call resolution pass. Augments per-file
 * resolved_calls with cross-file resolutions before call edges are emitted.
 * Implementation: src/pipeline/pass_lsp_cross.c. */
int hyp_pipeline_pass_lsp_cross(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files,
                                int file_count, HYPFileResult **cache);

/* Sub-passes called from pass_calls: pattern-based edge extraction */
void hyp_pipeline_pass_fastapi_depends(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files,
                                       int file_count);

int hyp_pipeline_pass_usages(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files, int file_count);

int hyp_pipeline_pass_semantic(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files,
                               int file_count);

int hyp_pipeline_pass_tests(hyp_pipeline_ctx_t *ctx, const hyp_file_info_t *files, int file_count);

int hyp_pipeline_pass_githistory(hyp_pipeline_ctx_t *ctx);

/* Pre-computed git history result for fused post-pass parallelism. */
typedef struct {
    hyp_change_coupling_t *couplings;
    int count;
    int commit_count;
    /* Per-file temporal data (change_count + last_modified) for File nodes.
     * NULL when the history pass had no commits to analyse. */
    hyp_file_temporal_t *file_temporal;
    int file_temporal_count;
} hyp_githistory_result_t;

/* Compute change couplings without touching the graph buffer.
 * Can run on a separate thread while other passes use the gbuf. */
int hyp_pipeline_githistory_compute(const char *repo_path, hyp_githistory_result_t *result);

/* Apply pre-computed couplings to the graph buffer (main thread only). */
int hyp_pipeline_githistory_apply(hyp_pipeline_ctx_t *ctx, const hyp_githistory_result_t *result);

/* Pre-dump pass: decorator tags enrichment (operates on gbuf). */
int hyp_pipeline_pass_decorator_tags(hyp_gbuf_t *gbuf, const char *project);

/* Pre-dump pass: config ↔ code linking. */
int hyp_pipeline_pass_configlink(hyp_pipeline_ctx_t *ctx);

/* Pre-dump pass: SIMILAR_TO edges via MinHash fingerprinting. */
int hyp_pipeline_pass_similarity(hyp_pipeline_ctx_t *ctx);

/* Pre-dump pass: SEMANTICALLY_RELATED edges via algorithmic embeddings.
 * Opt-in: only runs when HYP_SEMANTIC_ENABLED=1. */
int hyp_pipeline_pass_semantic_edges(hyp_pipeline_ctx_t *ctx);

/* Pre-dump pass: interprocedural complexity propagation (Tier B).
 * Propagates per-function loop_depth along CALLS edges into a transitive
 * worst-case nested-loop estimate (transitive_loop_depth) and flags call-graph
 * cycles (recursive). Runs on the graph buffer before the dump. */
void hyp_pipeline_pass_complexity(hyp_pipeline_ctx_t *ctx);

/* ── Env URL scanner (pass_envscan.c) ────────────────────────────── */

typedef struct {
    char key[HYP_SZ_128];
    char value[HYP_SZ_512];
    char file_path[HYP_SZ_256];
} hyp_env_binding_t;

/* Scan a project directory for environment variable assignments with URL values.
 * Walks the filesystem, scans Dockerfiles, shell scripts, .env, YAML, TOML,
 * Terraform, and .properties files. Filters out secrets.
 * Returns number of bindings written to out (up to max_out).
 * NOTE: this walker currently has no production callers — it is exercised
 * only by tests. The _excluded variant honors discovery exclusions for
 * consistency with the pkgmap/path-alias walks (#792); the plain variant
 * scans unexcluded (NULL exclusion list). */
int hyp_scan_project_env_urls(const char *root_path, hyp_env_binding_t *out, int max_out);
int hyp_scan_project_env_urls_excluded(const char *root_path, hyp_env_binding_t *out, int max_out,
                                       char **excluded_dirs, int excluded_count);

/* ── Incremental pipeline (pipeline_incremental.c) ───────────────── */

/* Run incremental re-index on an existing disk DB.
 * Classifies files by mtime+size, deletes changed nodes, re-parses changed
 * files, merges into disk DB. Returns 0 on success. */
int hyp_pipeline_run_incremental(hyp_pipeline_t *p, const char *db_path, hyp_file_info_t *files,
                                 int file_count, const hyp_file_hash_t *baseline_manifest,
                                 int baseline_count);

/* Exact semantic inputs for no-op/forced-full routing. The manifest contains
 * every discovered source plus repository controls actually consumed by
 * discovery, package mapping, path aliases, and extension overrides. */
#define HYP_SEMANTIC_INPUT_PREFIX ".hyponoia/.semantic-input/"
#define HYP_SEMANTIC_INPUT_GIT_CONTEXT HYP_SEMANTIC_INPUT_PREFIX "git-context-v1"
#define HYP_SEMANTIC_INPUT_GLOBAL_CONFIG HYP_SEMANTIC_INPUT_PREFIX "global-extension-config-v1"
#define HYP_SEMANTIC_INPUT_PROJECT_CONFIG HYP_SEMANTIC_INPUT_PREFIX "project-extension-config-v1"

int hyp_pipeline_build_semantic_manifest(const char *project, const char *repo_path,
                                         const hyp_file_info_t *files, int file_count,
                                         char **excluded_dirs, int excluded_count,
                                         const hyp_git_context_t *git_ctx,
                                         const hyp_userconfig_t *userconfig, hyp_file_hash_t **out,
                                         int *out_count);
void hyp_pipeline_free_semantic_manifest(hyp_file_hash_t *manifest, int count);
bool hyp_pipeline_semantic_manifests_equal(const hyp_file_hash_t *left, int left_count,
                                           const hyp_file_hash_t *right, int right_count);
/* Re-run discovery and hash its exact semantic inputs. Used at the publication
 * boundary so late additions/deletions cannot escape a frozen file list. */
int hyp_pipeline_build_fresh_semantic_manifest(const char *project, const char *repo_path, int mode,
                                               hyp_file_hash_t **out, int *out_count);

/* Compatibility contract persisted in coverage metadata. Increment when a
 * graph/manifest semantic change makes prior exact-input indexes unsafe. */
enum { HYP_SEMANTIC_INDEX_VERSION = 3 };

typedef struct {
    hyp_gbuf_t *gbuf;
    const char *final_db_path;
    /* Where the durable half of the store still lives while this generation is
     * being built: the LIVE database, not the staging copy of it. The two
     * differ exactly when they matter — a forced full rebuild unlinks the
     * staging copy before indexing, and a failed backup never makes one — so
     * reading the previous generation from final_db_path would find nothing
     * and publish an empty registry. NULL falls back to final_db_path, which
     * is right for a caller whose destination IS the live file. */
    const char *previous_db_path;
    const char *project;
    atomic_int *cancelled;
    const hyp_file_hash_t *manifest;
    int manifest_count;
    const hyp_coverage_row_t *coverage;
    int coverage_count;
    hyp_coverage_meta_t coverage_meta;
    /* Per-file LSP surfaces for the generation being published (may be NULL:
     * cross-LSP disabled, or a caller that has none). Written into the
     * staging store alongside the manifest so surface data and graph always
     * belong to the same generation. */
    const hyp_lsp_surface_row_t *surface_rows;
    int surface_row_count;
    /* True when the caller already wrote this generation's surface rows
     * into the staging store (delta patch); publish then skips the
     * wholesale delete+rewrite. */
    bool surfaces_in_place;
} hyp_pipeline_generation_t;

/* Serialize and fully populate a sibling staging database, then atomically
 * replace final_db_path. The old generation is untouched on every failure or
 * cancellation observed before the rename commit point. */
int hyp_pipeline_publish_generation(const hyp_pipeline_generation_t *generation);
/* Final leg shared by the dump-built and delta-patched publication paths:
 * sidecar removal, previous-generation quarantine, atomic rename. The stage
 * must be complete and sealed with its store handle closed. Discards the
 * stage on every failure. Does NOT free stage_path. */
int hyp_pipeline_finalize_staged_generation(char *stage_path, const char *final_db_path,
                                            atomic_int *cancelled, bool destination_known_healthy);
/* Metadata writes + FTS policy + integrity + seal + finalize for an
 * already-materialized staging DB. Takes ownership of stage_path. The dump
 * path passes fts_wholesale=true; the delta path passes false (its patch
 * wrote row-level FTS inserts). generation->gbuf is not read here. */
int hyp_pipeline_publish_staged(char *stage_path, const hyp_pipeline_generation_t *generation,
                                bool fts_wholesale, bool destination_known_healthy);

/* mkstemp-minted staging sibling of final_path (exported for the delta
 * executor; the dump path uses it internally). malloc'd, caller frees. */
char *hyp_pipeline_create_staging_path(const char *final_path);

/* ── Delta-repair staging primitives (pipeline_delta.c) ──────────
 * Closure-route-only subsystem: clone the live generation, patch exactly
 * the repaired node/edge set, publish through the shared finalize leg. */
typedef struct {
    char *source_qn;
    char *target_qn;
    char *type;
    char *props;
} hyp_delta_saved_edge_t;

int hyp_delta_stage_clone(const char *final_db_path, char **out_stage_path);
int hyp_delta_snapshot_inbound(hyp_store_t *store, const char *project, const char *const *paths,
                               int path_count, hyp_delta_saved_edge_t **out, int *out_count);
void hyp_delta_free_snapshot(hyp_delta_saved_edge_t *items, int count);
int hyp_delta_purge(hyp_store_t *store, const char *project, const char *const *paths,
                    int path_count);
/* Pre-seed proxy nodes with their REAL database ids and move the gbuf id
 * watermark above MAX(id); returns that max id, or -1 on failure. */
int64_t hyp_delta_preseed(hyp_store_t *store, const char *project, hyp_gbuf_t *gbuf);
int hyp_delta_patch(hyp_store_t *store, const char *project, hyp_gbuf_t *gbuf, int64_t max_db_id,
                    const hyp_delta_saved_edge_t *snapshot, int snapshot_count);
/* discard helper shared with the delta executor (unlink stage + sidecars). */
void hyp_pipeline_discard_stage(const char *stage_path);
/* The SQLite generation is authoritative. An explicitly requested artifact is
 * part of the caller-visible operation and its export error is returned;
 * automatic refresh of an already-existing artifact remains best-effort. */
int hyp_pipeline_refresh_artifact(hyp_pipeline_t *p, const char *db_path);

/* Hand the pipeline the per-file LSP-surface rows serialized at the
 * collect_all_defs seam (the only moment the result cache is alive).
 * Takes ownership; dump_and_persist_hashes writes them into the staging
 * store and hyp_pipeline_free releases them. Passing NULL/0 clears. */
void hyp_pipeline_set_lsp_surfaces(hyp_pipeline_t *p, hyp_lsp_surface_row_t *rows, int count);

/* Pipeline accessors for incremental use */
const char *hyp_pipeline_repo_path(const hyp_pipeline_t *p);
/* The live database this run replaces, or NULL when the destination IS it.
 * Publication reads the previous generation's durable rows from here. */
const char *hyp_pipeline_live_db_path(const hyp_pipeline_t *p);
atomic_int *hyp_pipeline_cancelled_ptr(hyp_pipeline_t *p);
int hyp_pipeline_workspace_member_count(const hyp_pipeline_t *p);

/* The workspace-evidence gate, as ONE predicate. True only when this run
 * indexes one member of
 * a workspace that has somewhere else to look; every site that records
 * cross-member evidence asks this and nothing else, so the sequential and the
 * parallel resolution paths cannot disagree about it. Derived from
 * ctx->pipeline, which all three ctx initialisers already set — a field on the
 * ctx would be a third place to forget. */
bool hyp_pipeline_ctx_records_workspace_evidence(const hyp_pipeline_ctx_t *ctx);
/* Record committed graph size (#334 gate axis) from the incremental path,
 * which cannot see the opaque hyp_pipeline struct. Call before the dump. */
void hyp_pipeline_set_committed_counts(hyp_pipeline_t *p, int nodes, int edges);

/* Test seam: invoked after a complete staging DB is sealed and immediately
 * before the cancellation check + atomic replace. Not part of the public API. */
void hyp_pipeline_set_before_publish_hook_for_tests(
    hyp_pipeline_t *p, void (*hook)(hyp_pipeline_t *, const char *, void *), void *ctx);
void hyp_pipeline_set_rename_hook_for_tests(hyp_pipeline_t *p,
                                            int (*hook)(const char *, const char *, void *),
                                            void *ctx);

/* Synchronous thread-local seam for deterministic cross-repo cancellation
 * tests. The callback runs immediately after a CROSS_* edge is committed and
 * is never retained; it must not re-enter cross-repo matching. */
typedef void (*hyp_cross_repo_after_insert_test_hook_t)(const char *project, const char *edge_type,
                                                        void *context);
void hyp_cross_repo_set_after_insert_hook_for_tests(hyp_cross_repo_after_insert_test_hook_t hook,
                                                    void *context);

/* Parse a gRPC stub call "<service-stub>.<method>" into the canonical proto
 * service name + method. Returns true ONLY when a recognized gRPC stub/client
 * suffix is present (the stub-type signal that gates Route emission, #294).
 * Exposed for testing. */
bool extract_grpc_service_method(const char *callee, char *service, size_t srv_sz, char *method,
                                 size_t meth_sz);

/* Extraction back-pressure observability (pass_parallel.c): nap-cycle counter
 * for the over-budget collect+nap gate. Test hook — asserts the gate stops
 * re-paying the nap tax once a full cycle failed to reclaim under budget
 * (futile: the resident floor, not transients, holds the memory). */
long hyp_pp_bp_nap_cycles(void);
void hyp_pp_bp_nap_cycles_reset(void);

/* Number of resolved-call rows handed to the parallel resolver's linear LSP
 * fallback since the last reset. Test observability for occurrence-index
 * coverage; deterministic and independent of wall-clock timing. */
uint64_t hyp_pp_lsp_linear_fallback_rows(void);
void hyp_pp_lsp_linear_fallback_rows_reset(void);

#if defined(HYP_CALL_REFERENCE_LOOKUP_TEST_API) && HYP_CALL_REFERENCE_LOOKUP_TEST_API
/* Deterministic test-only operation count for the shared semantic-reference
 * matcher used by both sequential and fused-parallel usage materialization. */
void hyp_pipeline_lsp_reference_lookup_test_reset(void);
uint64_t hyp_pipeline_lsp_reference_lookup_test_rows_examined(void);
#endif

#if defined(HYP_INCREMENTAL_TEST_API) && HYP_INCREMENTAL_TEST_API
typedef enum {
    HYP_INCREMENTAL_ROUTE_NONE = 0,
    HYP_INCREMENTAL_ROUTE_NOOP,
    HYP_INCREMENTAL_ROUTE_FORCED_FULL,
    HYP_INCREMENTAL_ROUTE_LEGACY_PARTIAL,
    HYP_INCREMENTAL_ROUTE_CLOSURE_REPAIR,
} hyp_incremental_route_t;

/* Deterministic one-shot fault injection for the incremental-parallel result
 * cache allocation. Reset explicitly so one test cannot affect another. */
void hyp_pipeline_incremental_test_fail_result_cache_alloc_once(void);
void hyp_pipeline_incremental_test_force_legacy_partial_once(void);
void hyp_pipeline_incremental_test_fail_after_stage_dump_once(void);
void hyp_pipeline_incremental_test_cancel_after_predump_once(void);
void hyp_pipeline_incremental_test_cancel_after_destination_prepare_once(void);
void hyp_pipeline_incremental_test_fail_carry_forward_once(void);
typedef void (*hyp_pipeline_test_hook_fn)(void *userdata);
void hyp_pipeline_incremental_test_before_final_manifest_once(hyp_pipeline_test_hook_fn hook,
                                                              void *userdata);
hyp_incremental_route_t hyp_pipeline_incremental_test_last_route(void);
void hyp_pipeline_incremental_test_reset_faults(void);

/* Shared persistence-hook plumbing. Tests use the incremental facade above so
 * one reset covers route, extraction, and publication faults. */
bool hyp_pipeline_persist_test_take_failure_after_stage_dump(void);
bool hyp_pipeline_persist_test_take_cancel_after_predump(void);
bool hyp_pipeline_persist_test_take_cancel_after_destination_prepare(void);
bool hyp_pipeline_persist_test_take_failure_carry_forward(void);
void hyp_pipeline_persist_test_run_before_final_manifest(void);
void hyp_pipeline_persist_test_reset_faults(void);
#endif

#endif /* HYP_PIPELINE_INTERNAL_H */
