/*
 * pipeline.h — Indexing pipeline orchestrator.
 *
 * Orchestrates multi-pass indexing of a repository:
 *   1. Structure: Project/Folder/Package/File nodes
 *   2. Definitions: Extract + write nodes + build registry
 *   3. Imports: Resolve import edges
 *   4. Calls: Call resolution (registry + LSP)
 *   5. Usages: Usage/type_ref edges
 *   6. Semantic: Inherits/decorates/implements
 *   7. Post: Tests, communities, HTTP links, config, git history
 *
 * Depends on: foundation, extraction, lsp, store, graph_buffer, discover
 */
#ifndef HYP_PIPELINE_H
#define HYP_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "discover/discover.h"       /* hyp_ignored_file_t (#963) */
#include "foundation/constants.h"    /* HYP_SZ_512 */
#include "store/workspace_resolve.h" /* hyp_wsr_resolved_t */

/* Forward declarations */
typedef struct hyp_store hyp_store_t;
typedef struct hyp_gbuf hyp_gbuf_t;

/* ── Opaque handle ──────────────────────────────────────────────── */

typedef struct hyp_pipeline hyp_pipeline_t;

/* ── Index mode ─────────────────────────────────────────────────── */

#ifndef HYP_INDEX_MODE_T_DEFINED
#define HYP_INDEX_MODE_T_DEFINED
typedef enum {
    /* All modes run the LSP type-aware call/usage resolution (per-file +
     * cross-file). The mode only controls file discovery breadth and whether
     * SIMILAR_TO / SEMANTICALLY_RELATED edges are computed. */
    HYP_MODE_FULL = 0,     /* Full: everything including SIMILAR_TO + SEMANTICALLY_RELATED */
    HYP_MODE_MODERATE = 1, /* Moderate: fast discovery + SIMILAR_TO + SEMANTICALLY_RELATED */
    HYP_MODE_FAST = 2,     /* Fast: skip non-essential files, no similarity/semantic edges */
} hyp_index_mode_t;
#endif

/* ── Pipeline lifecycle ─────────────────────────────────────────── */

/* Create a new pipeline. Caller owns the result. */
hyp_pipeline_t *hyp_pipeline_new(const char *repo_path, const char *db_path, hyp_index_mode_t mode);

/* Enable persistent artifact export (.hyponoia/graph.db.zst).
 * When enabled, the pipeline writes a compressed artifact after indexing. */
void hyp_pipeline_set_persistence(hyp_pipeline_t *p, bool enabled);

/* Free a pipeline and all its internal state. NULL-safe. */
void hyp_pipeline_free(hyp_pipeline_t *p);

/* Run the full indexing pipeline. Discovers files, extracts, resolves, and
 * dumps to SQLite. Returns 0 on success and non-zero on failure.
 *
 * Treating any non-zero as "the run failed" is always correct. Callers that
 * need to know whether the PREVIOUS generation survived can distinguish the
 * failures by value: the run publishes by renaming a fully validated staging
 * database over the destination, so every abort before that rename leaves the
 * existing database in place. Those codes (HYP_PIPELINE_ABORT_PRESERVE_DB and
 * HYP_PIPELINE_PERSIST_FAILED) are defined in pipeline_internal.h alongside the
 * stages that raise them. */
int hyp_pipeline_run(hyp_pipeline_t *p);

/*
 * Index every member of a resolved workspace into the workspace's ONE store.
 *
 * This is the assembly half of a workspace: the resolver says which repos
 * belong together and the store holds them, but nothing joined the two, so
 * "all members share one database" described no code path. Each member runs
 * the ordinary pipeline against the SAME database file — there is no second
 * mode and no second addressing scheme. A workspace of one is this function
 * with one member, over the byte-identical file that repo's index already
 * used.
 *
 * Opening the workspace store binds the registry first, so a member indexed
 * afterwards finds itself already registered; publication then carries that
 * registry, and every previously indexed member, across its own generation
 * boundary (generation_carry.h).
 *
 * Every member is indexed regardless of role. A role governs whether an agent
 * may EDIT a repo, not whether its code is worth finding — a vendored
 * dependency you must not touch is precisely one you need to be able to read.
 *
 * Returns 0 when every member published. On the first member that does not,
 * stops and returns that member's pipeline code, naming the member in err.
 * Members already published stay published: each publication is its own
 * atomic generation, and rolling them back would mean deleting good data in
 * order to report a failure. members_indexed (optional) receives how many
 * completed, so a caller can say which half of the workspace is current.
 *
 * NO PRODUCTION CALLER YET — the seam from a resolved workspace to the CLI,
 * the MCP server and the daemon belongs to the onboarding units, and this is
 * the function they call. Stated rather than silent.
 *
 * THIS IS ALSO THE DRIVER THE CROSS-MEMBER CALL PASS IS WAITING FOR, and that
 * pass says so in its own header: it needs the member count set on each
 * member's pipeline before the run, and one match pass over the assembled
 * store after the loop. Doing one without the other records crossings nobody
 * resolves, or resolves crossings nobody recorded. Both ends are written down;
 * whoever brings the two branches together owns wiring them here.
 */
int hyp_pipeline_index_workspace(const hyp_wsr_resolved_t *ws, hyp_index_mode_t mode,
                                 int *members_indexed, char *err, size_t err_sz);

/* Request cancellation of a running pipeline (thread-safe). */
void hyp_pipeline_cancel(hyp_pipeline_t *p);

/* Bind cancellation to a caller-owned atomic flag. The flag must outlive the
 * pipeline and should be initialized before binding. This lets a long-lived
 * daemon request cancellation without retaining/dereferencing a pipeline
 * pointer that its request thread may concurrently retire. */
void hyp_pipeline_bind_cancel_flag(hyp_pipeline_t *p, atomic_int *cancelled);

/* Get the project name derived from repo_path. Returned string is
 * owned by the pipeline. Valid until hyp_pipeline_free(). */
const char *hyp_pipeline_project_name(const hyp_pipeline_t *p);

/* Override the derived project name with a sanitized user-provided label. */
bool hyp_pipeline_set_project_name(hyp_pipeline_t *p, const char *name);

/* Get the index mode (HYP_MODE_FULL, HYP_MODE_MODERATE, HYP_MODE_FAST). */
int hyp_pipeline_get_mode(const hyp_pipeline_t *p);

/* ── Workspace membership ───────────────────────────────────────────
 *
 * Declare that this run indexes ONE member of a workspace containing
 * `member_count` repositories. THE CALLER SUPPLIES IT; the pipeline never reads
 * the workspace registry itself, and the reason is not layering taste — it is a
 * cycle. The registry lives in the destination database, and a full run
 * publishes by renaming a freshly built staging database OVER that destination,
 * so the pipeline would have to read the registry out of the file it is about
 * to replace. The caller has already run hyp_wsr_resolve() and therefore holds
 * the member list; it passes the count in.
 *
 * THE CONTRACT, for whoever builds the multi-member index driver:
 *   - Derive `member_count` from hyp_store_workspace_repos() (or the resolved
 *     hyp_wsr_resolved_t.member_count), never from a hand-written list.
 *   - Count the WHOLE workspace, including this member. A workspace of one
 *     passes 1, which is the same as never calling this at all.
 *   - Call it before hyp_pipeline_run().
 *
 * WHAT IT CHANGES: with a count above 1, the resolution passes record the two
 * facts they otherwise drop on the floor — a callee that resolved to nothing in
 * this member, and an import specifier that named no file in it — as
 * workspace-scoped rendezvous nodes for hyp_workspace_calls_match() to resolve
 * once every member is in one store. With 0 or 1 they record nothing, and the
 * graph of a lone repository is unchanged by any of this — which is the point:
 * otherwise every printf() in every repository would mint a node.
 *
 * Values below 1 are stored as 0. */
void hyp_pipeline_set_workspace_member_count(hyp_pipeline_t *p, int member_count);

/* Get the list of directory subtrees skipped during discovery (#411).
 * *out receives a borrowed array of rel-path strings (owned by the pipeline,
 * valid until hyp_pipeline_free()); *count receives its length. Both are set
 * to NULL/0 when p is NULL or nothing was excluded. Do not free. */
void hyp_pipeline_get_excluded(const hyp_pipeline_t *p, char ***out, int *count);

/* Committed node/edge counts captured at dump time (-1 when dump did not run).
 * Nodes are the #334 plausibility-gate axis; edges are informational only. */
void hyp_pipeline_get_committed_counts(const hyp_pipeline_t *p, int *nodes, int *edges);

/* ── Per-file indexing failures (Stage 2 / Track B) ─────────────── */

/* One source file that was skipped during indexing. All strings are owned by
 * the pipeline (copied on record, freed in hyp_pipeline_free). A skip is the
 * expected, handled outcome of a bad/oversized file — indexing continues and
 * the run still reports status "indexed"; these are surfaced (not errors that
 * fail the run) via MCP `skipped[]` / the CLI / a per-run logfile. */
typedef struct {
    char *path;   /* repo-relative path of the skipped file */
    char *reason; /* human-readable cause (e.g. "oversized (712 MB > 512 MB)",
                   * "parse timeout", "read failed"). For phase "parse_partial"
                   * this carries the 1-based line-range list ("12-40,88-90")
                   * of the unparseable regions. */
    char *phase;  /* "read" | "extract" | "oversized" | "parse_partial".
                   * "parse_partial" (#963) is NOT a skip: the file WAS indexed
                   * but contains tree-sitter ERROR/MISSING regions whose
                   * constructs are absent from the graph (best-effort signal —
                   * absence of the flag is NOT a completeness guarantee). The
                   * MCP layer reports it separately from skipped[]. "cross_lsp"
                   * is a RESERVED phase string for Track C's crash-attribution
                   * signal and is intentionally NOT emitted today (the
                   * cross-LSP passes are best-effort/void with no genuine
                   * per-file failure). */
} hyp_file_error_t;

/* Record a skipped file. path/reason/phase are copied. NULL-safe on p.
 *
 * NOT thread-safe: call it from the sequential extraction pass, or from the
 * parallel merge step (never from inside a parallel worker — workers collect
 * into per-worker lists and merge sequentially). */
void hyp_pipeline_add_file_error(hyp_pipeline_t *p, const char *path, const char *reason,
                                 const char *phase);

/* Borrowed accessor for the recorded skips (owned by the pipeline, valid until
 * hyp_pipeline_free()). out and count are set to NULL and 0 when p is NULL or
 * nothing was skipped. Do not free. */
void hyp_pipeline_get_file_errors(const hyp_pipeline_t *p, hyp_file_error_t **out, int *count);

/* Borrowed accessor for the individually-ignored files captured during
 * discovery (#963 "purposely not indexed" — by design, not failures). count
 * is the stored (capped) length, total the uncapped number seen. Do not
 * free. */
void hyp_pipeline_get_ignored(const hyp_pipeline_t *p, hyp_ignored_file_t **out, int *count,
                              int *total);

/* ── Index lock (prevents concurrent pipeline runs on same DB) ──── */

/* Try to acquire the global index lock. Returns true if acquired,
 * false if another pipeline is already running (non-blocking).
 * Use this in the watcher — skip reindex if busy. */
bool hyp_pipeline_try_lock(void);

/* Acquire the global index lock, blocking until available.
 * Use this in MCP handler and autoindex — wait for busy watcher to finish. */
void hyp_pipeline_lock(void);

/* Release the global index lock. */
void hyp_pipeline_unlock(void);

/* ── FQN helpers (used by passes and external callers) ──────────── */

/* Compute a qualified name: project.dir.parts.name
 * Strips extension, converts / to ., drops __init__ and index.
 * Caller must free() the returned string. */
char *hyp_pipeline_fqn_compute(const char *project, const char *rel_path, const char *name);

/* Module QN: project.dir.parts (no name). Caller must free(). */
char *hyp_pipeline_fqn_module(const char *project, const char *rel_path);

/* Language-aware module QN. When `module_is_dir` is true (Java/Go package
 * semantics) the module is derived from the CONTAINING DIRECTORY (the filename
 * stem is dropped), so it agrees with the extraction-side def QNs; when false
 * it is exactly hyp_pipeline_fqn_module(). Caller must free(). */
char *hyp_pipeline_fqn_module_dir(const char *project, const char *rel_path, bool module_is_dir);

/* Folder QN: project.dir.parts. Caller must free(). */
char *hyp_pipeline_fqn_folder(const char *project, const char *rel_dir);

/* Resolve an import specifier that uses a relative path (./foo, ../bar, .foo,
 * or an unqualified local name like "foo.h") against the importing file's
 * path.  Returns a malloc'd normalized relative path without extension
 * (e.g. "src/api/helpers") suitable for passing to hyp_pipeline_fqn_module,
 * or NULL if the specifier is not a relative path (bare module names like
 * "lodash", "django", "github.com/foo/bar" return NULL — the caller should
 * treat those as external/unresolvable). Handles ".", "..", and leading
 * dot-only segments used by Python relative imports. */
char *hyp_pipeline_resolve_relative_import(const char *source_rel, const char *module_path);

/* Derive project name from an absolute path.
 * Replaces / and : with -, collapses --, trims leading -.
 * Caller must free() the returned string. */
char *hyp_project_name_from_path(const char *abs_path);

/* ── Function Registry ──────────────────────────────────────────── */

typedef struct hyp_registry hyp_registry_t;

typedef struct {
    const char *qualified_name; /* borrowed from registry */
    const char *strategy;       /* resolution strategy name */
    double confidence;          /* 0.0–1.0 */
    int candidate_count;
} hyp_resolution_t;

/* Create/free a function registry. */
hyp_registry_t *hyp_registry_new(void);
void hyp_registry_free(hyp_registry_t *r);

/* Register a function/method/class. All strings are copied. */
void hyp_registry_add(hyp_registry_t *r, const char *name, const char *qualified_name,
                      const char *label);

/* Resolve a callee name using prioritized strategies.
 * import_map: NULL-terminated array of {local_name, resolved_qn} pairs, or NULL.
 * Returns result with qualified_name="" if unresolved. */
hyp_resolution_t hyp_registry_resolve(const hyp_registry_t *r, const char *callee_name,
                                      const char *module_qn, const char **import_map_keys,
                                      const char **import_map_vals, int import_map_count);

/* Per-file memoization cache for is_import_reachable. Thread-local —
 * each resolve worker owns its own cache. Call _begin at the start
 * of resolve_file_calls (or any per-file resolve loop) and _end at
 * the end. The cache MUST be invalidated between files because
 * is_import_reachable's truth depends on the file's import_vals. */
void hyp_registry_reach_cache_begin(int estimated_capacity);
void hyp_registry_reach_cache_end(void);

/* Per-file import-map prefix → module-QN hash. Turns the linear
 * strcmp scan inside resolve_import_map into O(1). Keys/values are
 * BORROWED — caller must keep the import_map arrays alive for the
 * cache lifetime. Invalidate between files via _end. */
void hyp_registry_import_map_cache_begin(const char **keys, const char **vals, int count);
void hyp_registry_import_map_cache_end(void);

/* Per-file full-result cache for hyp_registry_resolve. The same
 * callee_name appears in many call sites within a file; module_qn
 * is constant per file so each name resolves identically. First
 * lookup does the full strategy chain; repeats are O(1) hash hits.
 * This eliminates ~75% of the resolve-chain work on K8s where the
 * same names ("Get", "Add", "New", etc) appear hundreds of times. */
void hyp_registry_resolve_cache_begin(int estimated_capacity);
void hyp_registry_resolve_cache_end(void);

/* Check if a qualified name exists in the registry. */
bool hyp_registry_exists(const hyp_registry_t *r, const char *qn);

/* True if `name` is one of the curated Perl core builtins (perlfunc). Used by
 * the call-resolution passes to suppress generic-resolver CALLS edges from Perl
 * builtin invocations (push/shift/keys/...) to project subs that merely share
 * the name. Perl-scoped: callers gate on the file language. */
bool hyp_perl_is_builtin(const char *name);

/* Decide whether a resolved Perl call edge is generic-resolver noise to drop
 * (#476): true only for Perl, only for a builtin/method call, and only when the
 * match used a weak short-name strategy — high-confidence same_module/import_map
 * matches are kept. Pure; unit-tested in test_registry.c. */
bool hyp_perl_suppress_generic_match(bool is_perl, bool is_method, const char *callee_name,
                                     const char *strategy);

/* Decide whether a resolved TS/JS/TSX member-call edge is weak-strategy noise to
 * drop (#592/#606): true only for TS/JS, only for a member call with a
 * non-this/super receiver (is_method), and only when the match used a weak
 * short-name strategy (suffix_match / unique_name / field_type_hint / fuzzy).
 * Explicit drop-list keeps every lsp_* / import / same-module / qualified match.
 * Pure; unit-tested in test_registry.c. */
bool hyp_tsjs_suppress_weak_method_match(bool is_tsjs, bool is_method, const char *strategy);

/* Get the label of a qualified name, or NULL if not found. */
const char *hyp_registry_label_of(const hyp_registry_t *r, const char *qn);

/* Find all QNs with a given simple name. Sets *out and *count.
 * Caller does NOT free the array (owned by registry). */
int hyp_registry_find_by_name(const hyp_registry_t *r, const char *name, const char ***out,
                              int *count);

/* Return total number of entries. */
int hyp_registry_size(const hyp_registry_t *r);

/* Find all qualified names ending with ".suffix".
 * Sets *out to heap-allocated array of borrowed string pointers.
 * Caller must free(*out) but NOT the individual strings.
 * Returns count of matches. */
int hyp_registry_find_ending_with(const hyp_registry_t *r, const char *suffix, const char ***out);

/* Check if candidate QN's module prefix is reachable via any import value. */
bool hyp_registry_is_import_reachable(const char *candidate_qn, const char **import_vals,
                                      int import_count);

/* Fuzzy resolve: match callee by bare function name (last segment after dots).
 * Returns result with ok=true if found, ok=false if not.
 * Lower confidence than Resolve (0.40 single, 0.30 multiple). */
typedef struct {
    hyp_resolution_t result;
    bool ok;
} hyp_fuzzy_result_t;

hyp_fuzzy_result_t hyp_registry_fuzzy_resolve(const hyp_registry_t *r, const char *callee_name,
                                              const char *module_qn, const char **import_map_keys,
                                              const char **import_map_vals, int import_map_count);

const char *hyp_confidence_band(double score);

/* ── Git diff hunks (pass_gitdiff.c) ──────────────────────────────
 * Public (unlike the rest of pipeline_internal.h) because detect_changes
 * (src/mcp/mcp.c) scopes seed detection to changed line ranges, not just
 * changed files. */

typedef struct {
    char path[HYP_SZ_512];
    int start_line;
    int end_line;
} hyp_changed_hunk_t;

/* Parse `git diff --unified=0` output into per-hunk (path, start_line,
 * end_line) entries — end_line is the last new-side line the hunk touches.
 * Returns count written to out (capped at max_out). */
int hyp_parse_hunks(const char *output, hyp_changed_hunk_t *out, int max_out);

#endif /* HYP_PIPELINE_H */
