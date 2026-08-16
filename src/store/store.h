/*
 * store.h — Opaque SQLite graph store for code knowledge graphs.
 *
 * All functions are prefixed hyp_store_*. The store handle is opaque —
 * callers never touch SQLite internals directly.
 *
 * Thread safety: a single store handle must not be used concurrently.
 * Use one store per thread or external synchronization.
 */
#ifndef HYP_STORE_H
#define HYP_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Opaque handle ──────────────────────────────────────────────── */

typedef struct hyp_store hyp_store_t;

/* ── Result codes ───────────────────────────────────────────────── */

#define HYP_STORE_OK 0
#define HYP_STORE_ERR (-1)
#define HYP_STORE_NOT_FOUND (-2)
/* More than one row answers a lookup that addresses exactly one thing. The
 * caller gets no row: an address that names two entities names neither, and
 * handing back whichever one the b-tree stepped first is the plausible wrong
 * answer this store refuses everywhere else. Distinct from NOT_FOUND, which
 * says "nothing is here" — the two are never the same claim. */
#define HYP_STORE_AMBIGUOUS (-3)

/* ── Data structures ────────────────────────────────────────────── */

typedef struct {
    int64_t id;
    const char *project;
    const char *label;          /* Function, Class, Method, Module, File, ... */
    const char *name;           /* short name */
    const char *qualified_name; /* full dotted path */
    const char *file_path;      /* relative file path */
    int start_line;
    int end_line;
    const char *properties_json; /* JSON string, NULL → "{}" */
} hyp_node_t;

typedef struct {
    int64_t id;
    const char *project;
    int64_t source_id;
    int64_t target_id;
    const char *type;            /* CALLS, HTTP_CALLS, IMPORTS, ... */
    const char *properties_json; /* JSON string, NULL → "{}" */
} hyp_edge_t;

typedef struct {
    const char *name;
    const char *indexed_at; /* ISO 8601 */
    const char *root_path;
} hyp_project_t;

typedef struct {
    const char *project;
    const char *rel_path;
    const char *sha256;
    int64_t mtime_ns;
    int64_t size;
} hyp_file_hash_t;

/* One file's persisted LSP surface: the serialized cross-file definition set
 * (exactly what pass_lsp_cross registration consumes) plus the metadata the
 * closure-repair incremental route needs to decide and bound its work. The
 * store treats defs_json/ref_bloom as opaque; the codec lives with
 * pass_lsp_cross, which is the only writer and reader of their contents. */
typedef struct {
    const char *project;
    const char *rel_path;
    const char *surface_sha; /* sha256 hex of defs_json (the early-cutoff key) */
    const char *defs_json;   /* canonical JSON array of the file's LSP defs */
    const void *ref_bloom;   /* referenced-identifier bloom blob (may be NULL) */
    int ref_bloom_len;
    const char *config_ctx; /* governing-config context hash ("" = none) */
} hyp_lsp_surface_row_t;

/* Find nodes overlapping a line range in a file (excludes Module/Package). */
int hyp_store_find_nodes_by_file_overlap(hyp_store_t *s, const char *project, const char *file_path,
                                         int start_line, int end_line, hyp_node_t **out,
                                         int *count);

/* Find nodes whose qualified_name ends with the given suffix (dot-boundary). */
int hyp_store_find_nodes_by_qn_suffix(hyp_store_t *s, const char *project, const char *suffix,
                                      hyp_node_t **out, int *count);

/* Get CALLS degree of a node (inbound and outbound). */
void hyp_store_node_degree(hyp_store_t *s, int64_t node_id, int *in_deg, int *out_deg);

/* Get distinct file paths for a project. Caller must free each out[i] and out itself.
 * Returns HYP_STORE_OK or HYP_STORE_ERR. */
int hyp_store_list_files(hyp_store_t *s, const char *project, char ***out, int *count);

/* Get caller/callee names for a node (CALLS/HTTP_CALLS/ASYNC_CALLS edges).
 * Returns 0 on success. Caller must free each out_callers[i]/out_callees[i]
 * and the arrays themselves. */
int hyp_store_node_neighbor_names(hyp_store_t *s, int64_t node_id, int limit, char ***out_callers,
                                  int *caller_count, char ***out_callees, int *callee_count);

/* Batch count in/out degree for multiple nodes.
 * edge_type: filter by edge type (e.g. "CALLS"), or NULL/"" for all types.
 * out_in[i] and out_out[i] receive the in/out degree for node_ids[i].
 * Returns HYP_STORE_OK or HYP_STORE_ERR. */
int hyp_store_batch_count_degrees(hyp_store_t *s, const int64_t *node_ids, int id_count,
                                  const char *edge_type, int *out_in, int *out_out);

/* Upsert file hashes in batch. */
int hyp_store_upsert_file_hash_batch(hyp_store_t *s, const hyp_file_hash_t *hashes, int count);

/* ── LSP surface rows (closure-repair incremental) ───────────────
 * Upsert/get/delete are whole-row, keyed (project, rel_path). get returns
 * heap rows released with hyp_store_free_lsp_surfaces. A project with no
 * rows returns OK with *count == 0 — callers treat that as "no surface
 * data" and route to a full rebuild, which is also the upgrade path for
 * databases written before this table existed. */
int hyp_store_upsert_lsp_surface_batch(hyp_store_t *s, const hyp_lsp_surface_row_t *rows,
                                       int count);
int hyp_store_get_lsp_surfaces(hyp_store_t *s, const char *project, hyp_lsp_surface_row_t **out,
                               int *count);
int hyp_store_delete_lsp_surfaces(hyp_store_t *s, const char *project);
void hyp_store_free_lsp_surfaces(hyp_lsp_surface_row_t *rows, int count);

/* Reverse-dependency lookup for closure-repair routing: the DISTINCT
 * file_paths of nodes with at least one edge INTO a node of any file in
 * target_files, excluding the target files themselves. This is "who consumed
 * these files' definitions" as recorded by the previous generation — served
 * by idx_edges_target + idx_nodes_file, so cost tracks the result size, not
 * the graph size. out gets a malloc'd array of malloc'd strings; free with
 * hyp_store_free_dependent_files. */
int hyp_store_get_dependent_files(hyp_store_t *s, const char *project,
                                  const char *const *target_files, int target_count, char ***out,
                                  int *out_count);
void hyp_store_free_dependent_files(char **files, int count);

/* Find edges whose properties contain a url_path matching the keyword. */
int hyp_store_find_edges_by_url_path(hyp_store_t *s, const char *project, const char *keyword,
                                     hyp_edge_t **out, int *count);

/* Restore database from another store (backup API). */
int hyp_store_restore_from(hyp_store_t *dst, hyp_store_t *src);

/* Copy a transactionally-consistent snapshot, including committed WAL frames,
 * from an existing DB into a same-directory staging path. */
int hyp_store_backup_path(const char *source_path, const char *staging_path);

/* Seal a staging DB into one self-contained main file before atomic publish.
 * The store must have no concurrent users. */
int hyp_store_prepare_for_publish(hyp_store_t *s);

/* Checkpoint and detach sidecars from an existing destination immediately
 * before replacement. Fails closed while another process prevents sealing. */
int hyp_store_prepare_path_for_replace(const char *path);

/* ── Search ─────────────────────────────────────────────────────── */

typedef struct {
    const char *project;
    const char *label;        /* NULL = any label */
    const char *name_pattern; /* regex on name, NULL = any */
    const char *qn_pattern;   /* regex on qualified_name, NULL = any */
    const char *file_pattern; /* glob on file_path, NULL = any */
    const char *relationship; /* edge type filter, NULL = any */
    const char *direction;    /* "inbound" / "outbound" / "any", NULL = any */
    int min_degree;           /* -1 = no filter (default), 0+ = minimum */
    int max_degree;           /* -1 = no filter (default), 0+ = maximum */
    int limit;                /* 0 = default (10) */
    int offset;
    bool exclude_entry_points;
    bool include_connected;
    const char *sort_by; /* "relevance" / "name" / "degree", NULL = relevance */
    bool case_sensitive;
    const char **exclude_labels; /* NULL-terminated array, or NULL */
} hyp_search_params_t;

typedef struct {
    hyp_node_t node;
    int in_degree;
    int out_degree;
    /* connected_names: allocated array of strings, count in connected_count */
    const char **connected_names;
    int connected_count;
} hyp_search_result_t;

typedef struct {
    hyp_search_result_t *results;
    int count;
    int total; /* total before pagination */
} hyp_search_output_t;

/* ── Traversal ──────────────────────────────────────────────────── */

typedef struct {
    hyp_node_t node;
    int hop; /* BFS depth from root */
} hyp_node_hop_t;

typedef struct {
    const char *from_name;
    const char *to_name;
    const char *type;
    double confidence;
    int64_t source_id; /* edge endpoints — let callers match an edge to a hop node */
    int64_t target_id;
    const char *properties_json; /* raw edge properties (carries CALLS arg expressions) */
} hyp_edge_info_t;

typedef struct {
    hyp_node_t root;
    hyp_node_hop_t *visited;
    int visited_count;
    hyp_edge_info_t *edges;
    int edge_count;
} hyp_traverse_result_t;

/* ── Schema introspection ───────────────────────────────────────── */

typedef struct {
    const char *label;
    int count;
    char **properties; /* distinct property keys for this label (base + JSON) */
    int property_count;
} hyp_label_count_t;

typedef struct {
    const char *type;
    int count;
    char **properties; /* distinct property keys for this edge type (base + JSON) */
    int property_count;
} hyp_type_count_t;

typedef struct {
    hyp_label_count_t *node_labels;
    int node_label_count;
    hyp_type_count_t *edge_types;
    int edge_type_count;
    /* relationship patterns like "(Function)-[CALLS]->(Function) [123x]" */
    const char **rel_patterns;
    int rel_pattern_count;
    const char **sample_func_names;
    int sample_func_count;
    const char **sample_class_names;
    int sample_class_count;
    const char **sample_qns;
    int sample_qn_count;
} hyp_schema_info_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/* Open an in-memory database (for testing). */
hyp_store_t *hyp_store_open_memory(void);

/* Open a file-backed database at the given path. Creates if needed. */
hyp_store_t *hyp_store_open_path(const char *db_path);

/* Open an existing file-backed database read-write without CREATE. Intended
 * for coordinated mutations where a missing/typo path must never materialize
 * a ghost database. Returns NULL when the file does not exist. */
hyp_store_t *hyp_store_open_path_existing(const char *db_path);

/* Open an existing file-backed database for querying only. Opened READ-ONLY
 * (no SQLITE_OPEN_CREATE, no write pragmas) so queries never mutate the DB and
 * work on a read-only file / filesystem. Returns NULL if the file does not
 * exist — never creates a new .db file. */
hyp_store_t *hyp_store_open_path_query(const char *db_path);

/* Validate and seal an existing DB for atomic replacement without creating or
 * migrating its schema. Returns OK when sealed, NOT_FOUND when the bytes are
 * definitely corrupt/incompatible and should be quarantined, or ERR when the
 * file is busy/unavailable and must be left in place. */
int hyp_store_seal_existing_path_for_replace(const char *db_path);

/* On-disk path of a file-backed store, or NULL for an in-memory (:memory:)
 * store. The returned pointer is owned by the store. */
const char *hyp_store_db_path(const hyp_store_t *s);

/* Check database integrity. Returns true if the DB passes basic sanity checks
 * (projects table has correct types, no corruption indicators).
 * Returns false if corruption is detected — caller should delete and re-index. */
bool hyp_store_check_integrity(hyp_store_t *s);
/* Shallow check + PRAGMA quick_check — catches page-level corruption.
 * O(db size); use on rare paths (artifact import), not hot opens. */
bool hyp_store_check_integrity_deep(hyp_store_t *s);

/* Open database for a named project in the default cache dir. */
hyp_store_t *hyp_store_open(const char *project);

/* Close the store and free all resources. NULL-safe. */
void hyp_store_close(hyp_store_t *s);

/* Get the underlying sqlite3 handle (for testing only). */
struct sqlite3 *hyp_store_get_db(hyp_store_t *s);

/* Get the last error message (static string, valid until next call). */
const char *hyp_store_error(hyp_store_t *s);

/* ── Transaction ────────────────────────────────────────────────── */

/* Begin a transaction. Returns HYP_STORE_OK on success. */
int hyp_store_begin(hyp_store_t *s);

/* Commit the current transaction. */
int hyp_store_commit(hyp_store_t *s);

/* Rollback the current transaction. */
int hyp_store_rollback(hyp_store_t *s);

/* ── Bulk write optimization ────────────────────────────────────── */

/* Tune pragmas for bulk write throughput (synchronous=OFF, large cache).
 * WAL journal mode is preserved throughout for crash safety. */
int hyp_store_begin_bulk(hyp_store_t *s);

/* Restore normal pragmas (synchronous=NORMAL, default cache) after bulk writes. */
int hyp_store_end_bulk(hyp_store_t *s);

/* Drop user indexes for faster bulk inserts. */
int hyp_store_drop_indexes(hyp_store_t *s);

/* Recreate user indexes after bulk inserts. */
int hyp_store_create_indexes(hyp_store_t *s);

/* ── WAL / Checkpoint ───────────────────────────────────────────── */

/* Force WAL checkpoint + PRAGMA optimize. */
int hyp_store_checkpoint(hyp_store_t *s);

/* #1083: the WAL size limit (journal_size_limit) applied to this write
 * connection, in bytes; -1 = unlimited (SQLite default / pre-fix). */
int64_t hyp_store_journal_size_limit(hyp_store_t *s);

/* Opaque store generation for pagination-cursor staleness detection:
 * "u<db_uid>g<mutation_gen>" — db_uid is minted per DB file, mutation_gen
 * bumps on every index run. "legacy" for DBs predating store_meta. */
int hyp_store_generation(hyp_store_t *s, char *buf, size_t bufsz);

/* Seal a fully-written staging database before atomic publication.
 * Raises synchronous to FULL, requires an exclusive TRUNCATE checkpoint to
 * complete, then leaves the database in verified DELETE journal mode so the
 * main file is self-contained (no required -wal/-shm sidecars). This is a
 * fail-closed operation: SQLITE_BUSY and an unconfirmed mode transition are
 * errors. The caller must own the staging database exclusively. */
int hyp_store_seal_for_atomic_publish(hyp_store_t *s);

/* Resolve the mmap_size pragma value applied to on-disk stores from the
 * HYP_SQLITE_MMAP_SIZE environment variable. Defaults to 67108864 (64 MB)
 * when the variable is unset, malformed, or partially numeric. Negative
 * values clamp to 0 (which disables mmap and reverts to read()/pread()
 * I/O — recoverable SQLITE_IOERR instead of SIGBUS when concurrent
 * processes truncate the DB file under live mappings). Exposed for
 * testability. */
int64_t hyp_store_resolve_mmap_size(void);

/* ── Dump / Restore ─────────────────────────────────────────────── */

/* Dump in-memory database to a file. */
int hyp_store_dump_to_file(hyp_store_t *s, const char *dest_path);

/* ── Project CRUD ───────────────────────────────────────────────── */

int hyp_store_upsert_project(hyp_store_t *s, const char *name, const char *root_path);
int hyp_store_get_project(hyp_store_t *s, const char *name, hyp_project_t *out);
int hyp_store_list_projects(hyp_store_t *s, hyp_project_t **out, int *count);
int hyp_store_delete_project(hyp_store_t *s, const char *name);

/* ── Workspace registry ─────────────────────────────────────────
 *
 * One workspace per database file, described IN the file: workspace_meta holds
 * the workspace's own id (one row), workspace_repos binds the member repos —
 * slug, root, role — under it. This is a REGISTRY inside the one existing
 * layout, not a second layout: a workspace of one lives in the byte-identical
 * <cache>/<slug>.db its per-repo index always did, and "migrating" an existing
 * per-repo database is exactly one bind call, which writes these rows and
 * touches nothing else — the identity function on every existing address.
 *
 * A store with NO registry rows is a pre-A1 per-repo store: ABSENT means
 * "derive: a workspace of one named by its single project", never "no
 * workspace". Reading it is fine; opening it through hyp_wsr_store_open()
 * (workspace_resolve.h) is what stamps the registry.
 *
 * The store treats `role` as opaque TEXT (NULL = not yet derived, A2-A5); the
 * vocabulary and its validation live with the resolver, which is the only
 * writer on this path. */

typedef struct {
    const char *slug;      /* project slug of the member repo */
    const char *root_path; /* canonical absolute root */
    const char *role;      /* NULL = not yet derived; never "" */
} hyp_workspace_repo_t;

/* Bind ws_id and its member repos into the registry, transactionally.
 *
 * Fail-closed rules, each refusing with HYP_STORE_ERR and a message in err
 * (when provided):
 *   - the store is already bound to a DIFFERENT id — a resolver that disagrees
 *     with the file it opened must not stamp over it;
 *   - two input repos share a slug (e.g. /a/b and /a-b) — the collision gate
 *     A1's migration inherits from A6; the message names BOTH paths;
 *   - an input slug already registered with a DIFFERENT root — the same
 *     collision, separated in time.
 *
 * Binding is additive and idempotent: existing members are verified, new ones
 * inserted, and a NULL incoming role never clobbers a derived one. It never
 * deletes members — removal is a distinct, visible operation (A7 reports the
 * contradiction in the meantime). */
int hyp_store_workspace_bind(hyp_store_t *s, const char *ws_id, const hyp_workspace_repo_t *repos,
                             int count, char *err, size_t err_sz);

/* The bound workspace id. HYP_STORE_OK with the id in out; HYP_STORE_NOT_FOUND
 * when the registry is absent (a pre-A1 store — see above); HYP_STORE_ERR on
 * failure. */
int hyp_store_workspace_id(hyp_store_t *s, char *out, size_t out_sz);

/* Member repos, ordered by slug. HYP_STORE_NOT_FOUND when the registry is
 * absent. Caller frees with hyp_store_free_workspace_repos. */
int hyp_store_workspace_repos(hyp_store_t *s, hyp_workspace_repo_t **out, int *count);
void hyp_store_free_workspace_repos(hyp_workspace_repo_t *rows, int count);

/* ── Node CRUD ──────────────────────────────────────────────────── */

/* Upsert a single node. Returns node ID (>0) or HYP_STORE_ERR. */
int64_t hyp_store_upsert_node(hyp_store_t *s, const hyp_node_t *n);

/* Upsert nodes in batch. out_ids must have room for count entries. */
int hyp_store_upsert_node_batch(hyp_store_t *s, const hyp_node_t *nodes, int count,
                                int64_t *out_ids);

/* Find node by primary key. Returns HYP_STORE_OK or HYP_STORE_NOT_FOUND. */
int hyp_store_find_node_by_id(hyp_store_t *s, int64_t id, hyp_node_t *out);

/* Find node by project + qualified_name. */
int hyp_store_find_node_by_qn(hyp_store_t *s, const char *project, const char *qn, hyp_node_t *out);

/* Find node by qualified_name only (no project filter — QNs are globally unique). */
int hyp_store_find_node_by_qn_any(hyp_store_t *s, const char *qn, hyp_node_t *out);

/* Find nodes by name (exact match). Returns allocated array, caller frees. */
int hyp_store_find_nodes_by_name(hyp_store_t *s, const char *project, const char *name,
                                 hyp_node_t **out, int *count);

/* Find nodes by name across all projects. Returns allocated array, caller frees. */
int hyp_store_find_nodes_by_name_any(hyp_store_t *s, const char *name, hyp_node_t **out,
                                     int *count);

/* Find nodes by label. */
int hyp_store_find_nodes_by_label(hyp_store_t *s, const char *project, const char *label,
                                  hyp_node_t **out, int *count);

/* Find nodes by file path. */
int hyp_store_find_nodes_by_file(hyp_store_t *s, const char *project, const char *file_path,
                                 hyp_node_t **out, int *count);

/* Batch lookup: map qualified names → node IDs.
 * qns[i] is resolved; out_ids[i] receives the ID or 0 if not found.
 * Returns number of QNs actually found, or HYP_STORE_ERR. */
int hyp_store_find_node_ids_by_qns(hyp_store_t *s, const char *project, const char **qns,
                                   int qn_count, int64_t *out_ids);

/* Count nodes in project. Returns count or HYP_STORE_ERR. */
int hyp_store_count_nodes(hyp_store_t *s, const char *project);

int hyp_store_count_nodes_scoped(hyp_store_t *s, const char *project, const char *path);

int hyp_store_count_edges_scoped(hyp_store_t *s, const char *project, const char *path);

/* True when path is a non-empty scope after normalization (issue #604). */
bool hyp_store_arch_path_scoped(const char *path);

/* When scoped, writes normalized directory prefix into norm_out. Returns false if unscoped. */
bool hyp_store_normalize_arch_path(const char *path, char *norm_out, size_t norm_sz);

/* True when architecture aspect `name` belongs to the "overview" subset:
 * every aspect EXCEPT the large per-file listing (file_tree). Shared by both
 * aspect gates — want_aspect (store.c) and aspect_wanted (mcp.c) — so the
 * two sites cannot drift. */
bool hyp_store_arch_aspect_in_overview(const char *name);

/* Delete all nodes for a project (cascade deletes edges). */
int hyp_store_delete_nodes_by_project(hyp_store_t *s, const char *project);

/* Delete nodes by file path. */
int hyp_store_delete_nodes_by_file(hyp_store_t *s, const char *project, const char *file_path);

/* Delete nodes by label. */
int hyp_store_delete_nodes_by_label(hyp_store_t *s, const char *project, const char *label);

/* ── Edge CRUD ──────────────────────────────────────────────────── */

/* Insert or update edge. Returns edge ID (>0) or HYP_STORE_ERR. */
int64_t hyp_store_insert_edge(hyp_store_t *s, const hyp_edge_t *e);

/* Insert edges in batch. */
int hyp_store_insert_edge_batch(hyp_store_t *s, const hyp_edge_t *edges, int count);

/* Fetch all CALLS edges among Function/Method nodes for a project as parallel
 * (source_id, target_id) arrays (caller frees both). For SCC / cycle analysis.
 * Stops at max_edges and sets *truncated — never a silent cap. Returns
 * HYP_STORE_OK (or _ERR); *count is the number returned. */
int hyp_store_fetch_call_edges(hyp_store_t *s, const char *project, int max_edges,
                               int64_t **out_src, int64_t **out_tgt, int *count, bool *truncated);

/* Find edges by source node. */
int hyp_store_find_edges_by_source(hyp_store_t *s, int64_t source_id, hyp_edge_t **out, int *count);

/* Find edges by target node. */
int hyp_store_find_edges_by_target(hyp_store_t *s, int64_t target_id, hyp_edge_t **out, int *count);

/* Find edges by source + type. */
int hyp_store_find_edges_by_source_type(hyp_store_t *s, int64_t source_id, const char *type,
                                        hyp_edge_t **out, int *count);

/* Find edges by target + type. */
int hyp_store_find_edges_by_target_type(hyp_store_t *s, int64_t target_id, const char *type,
                                        hyp_edge_t **out, int *count);

/* Find all edges of a type in project. */
int hyp_store_find_edges_by_type(hyp_store_t *s, const char *project, const char *type,
                                 hyp_edge_t **out, int *count);

/* Count all edges in project. */
int hyp_store_count_edges(hyp_store_t *s, const char *project);

/* Count edges of given type. */
int hyp_store_count_edges_by_type(hyp_store_t *s, const char *project, const char *type);

/* Delete all edges for a project. */
int hyp_store_delete_edges_by_project(hyp_store_t *s, const char *project);

/* Delete edges by type. */
int hyp_store_delete_edges_by_type(hyp_store_t *s, const char *project, const char *type);

/* ── File hash CRUD ─────────────────────────────────────────────── */

int hyp_store_upsert_file_hash(hyp_store_t *s, const char *project, const char *rel_path,
                               const char *sha256, int64_t mtime_ns, int64_t size);

int hyp_store_get_file_hashes(hyp_store_t *s, const char *project, hyp_file_hash_t **out,
                              int *count);

/* Fetch one exact file-hash record. The returned strings are heap-owned and
 * must be released with hyp_store_clear_file_hash(). */
int hyp_store_get_file_hash(hyp_store_t *s, const char *project, const char *rel_path,
                            hyp_file_hash_t *out);

/* Free heap-owned fields in one exact file-hash record and zero it. */
void hyp_store_clear_file_hash(hyp_file_hash_t *hash);

int hyp_store_delete_file_hash(hyp_store_t *s, const char *project, const char *rel_path);

int hyp_store_delete_file_hashes(hyp_store_t *s, const char *project);

/* ── Index coverage (#963) ──────────────────────────────────────── */

/* One best-effort coverage row: a file the indexer could not fully cover.
 * kind "parse_partial" = indexed but the parse tree had ERROR/MISSING regions
 * (detail = 1-based line ranges "12-40,88-90"); skip kinds "read"/"extract"/
 * "oversized" = not indexed at all (detail = reason). Stored in the separate
 * index_coverage table — coverage is metadata ABOUT the graph, never mixed
 * into the graph itself. */
typedef struct {
    const char *rel_path;
    const char *kind;
    const char *detail;
} hyp_coverage_row_t;

/* Metadata describing how completely one index run recorded the best-effort
 * coverage signal. `recording_status` is "complete", "truncated", or
 * "unavailable"; it is deliberately separate from hash_records_complete.
 * Strings returned by hyp_store_coverage_meta_get are heap-owned. */
typedef struct {
    const char *project;
    const char *generation;
    const char *index_mode;
    const char *recorded_at;
    const char *recording_status;
    int ignored_files_stored;
    int ignored_files_total;
    int coverage_version;
    bool hash_records_complete;
} hyp_coverage_meta_t;

/* Replace the project's coverage rows in one transaction, then prune rows for
 * files absent from file_hashes (deleted from the repo). Call AFTER hashes
 * were persisted for the run. */
int hyp_store_coverage_replace(hyp_store_t *s, const char *project, const hyp_coverage_row_t *rows,
                               int count);

/* Replace coverage rows and their run metadata atomically. Passing NULL meta
 * clears any older metadata so it cannot be mistaken for the new row set. */
int hyp_store_coverage_replace_ex(hyp_store_t *s, const char *project,
                                  const hyp_coverage_row_t *rows, int count,
                                  const hyp_coverage_meta_t *meta);

/* Fetch all coverage rows (ordered by rel_path). Caller frees via
 * hyp_store_free_coverage. */
int hyp_store_coverage_get(hyp_store_t *s, const char *project, hyp_coverage_row_t **out,
                           int *count);

/* Fetch coverage rows for one path. Exact rows are returned together with any
 * not_indexed_dir ancestor that covers the path. */
int hyp_store_coverage_get_path(hyp_store_t *s, const char *project, const char *rel_path,
                                hyp_coverage_row_t **out, int *count);

/* Fetch coverage rows at/below a directory scope, plus a not_indexed_dir
 * ancestor that covers the scope. Prefix matching is segment-boundary safe. */
int hyp_store_coverage_get_scope(hyp_store_t *s, const char *project, const char *scope,
                                 hyp_coverage_row_t **out, int *count);

/* Fetch/free the metadata paired with the current coverage row set. */
int hyp_store_coverage_meta_get(hyp_store_t *s, const char *project, hyp_coverage_meta_t *out);
void hyp_store_coverage_meta_clear(hyp_coverage_meta_t *meta);

/* Name of the derived miss-graph shadow project ("<project>::missed").
 * hyp_store_coverage_replace materializes the coverage rows as a file-
 * structure graph (Project → Folder → File{kind, detail}) under this project
 * name — queryable via the normal cypher path without touching the real
 * project's graph. */
void hyp_store_coverage_shadow_project(char *dst, size_t dstsz, const char *project);

void hyp_store_free_coverage(hyp_coverage_row_t *rows, int count);

/* ── Search ─────────────────────────────────────────────────────── */

int hyp_store_search(hyp_store_t *s, const hyp_search_params_t *params, hyp_search_output_t *out);

/* Free a search output's allocated memory. */
void hyp_store_search_free(hyp_search_output_t *out);

/* ── Traversal ──────────────────────────────────────────────────── */

int hyp_store_bfs(hyp_store_t *s, int64_t start_id, const char *direction, const char **edge_types,
                  int edge_type_count, int max_depth, int max_results, hyp_traverse_result_t *out);

/* Multi-source BFS from ALL seed ids at once (one CTE, temp-table anchored).
 * Seeds are EXCLUDED from the result (impact semantics); MIN(hop) across the
 * seed set; canonical (hop,id) order; *truncated set when the max_results
 * memory-safety ceiling was hit (counting is otherwise uncapped). */
int hyp_store_bfs_multi(hyp_store_t *s, const int64_t *seed_ids, int seed_count,
                        const char *direction, const char **edge_types, int edge_type_count,
                        int max_depth, int max_results, hyp_traverse_result_t *out,
                        bool *truncated);

/* Free a traverse result's allocated memory. */
void hyp_store_traverse_free(hyp_traverse_result_t *out);

/* ── Impact analysis ────────────────────────────────────────────── */

typedef enum {
    HYP_RISK_CRITICAL = 0,
    HYP_RISK_HIGH = 1,
    HYP_RISK_MEDIUM = 2,
    HYP_RISK_LOW = 3,
} hyp_risk_level_t;

/* Map BFS hop depth to risk level. */
hyp_risk_level_t hyp_hop_to_risk(int hop);

/* String representation of risk level. */
const char *hyp_risk_label(hyp_risk_level_t level);

typedef struct {
    int critical;
    int high;
    int medium;
    int low;
    int total;
    bool has_cross_service;
} hyp_impact_summary_t;

/* Build impact summary from visited hops and edges. */
hyp_impact_summary_t hyp_build_impact_summary(const hyp_node_hop_t *hops, int hop_count,
                                              const hyp_edge_info_t *edges, int edge_count);

/* Deduplicate BFS hops, keeping minimum hop per node ID.
 * Returns allocated array and count via out params. Caller frees result. */
int hyp_deduplicate_hops(const hyp_node_hop_t *hops, int hop_count, hyp_node_hop_t **out,
                         int *out_count);

/* ── Schema ─────────────────────────────────────────────────────── */

int hyp_store_get_schema(hyp_store_t *s, const char *project, hyp_schema_info_t *out);

/* Like hyp_store_get_schema but skips per-label/per-type JSON property-key
 * discovery (json_each scans over every row) — for callers that only need
 * label/type counts, e.g. get_architecture. */
int hyp_store_get_schema_counts(hyp_store_t *s, const char *project, hyp_schema_info_t *out);

int hyp_store_get_schema_counts_scoped(hyp_store_t *s, const char *project, const char *path,
                                       hyp_schema_info_t *out);

/* Free a schema info's allocated memory. */
void hyp_store_schema_free(hyp_schema_info_t *out);

/* ── Architecture ───────────────────────────────────────────────── */

typedef struct {
    const char *language;
    int file_count;
} hyp_language_count_t;

typedef struct {
    const char *name;
    int node_count;
    int fan_in;
    int fan_out;
} hyp_package_summary_t;

typedef struct {
    const char *name;
    const char *qualified_name;
    const char *file;
} hyp_entry_point_t;

typedef struct {
    const char *method;
    const char *path;
    const char *handler;
} hyp_route_info_t;

typedef struct {
    const char *name;
    const char *qualified_name;
    int fan_in;
} hyp_hotspot_t;

typedef struct {
    const char *from;
    const char *to;
    int call_count;
} hyp_cross_pkg_boundary_t;

typedef struct {
    const char *from;
    const char *to;
    const char *type;
    int count;
} hyp_service_link_t;

typedef struct {
    const char *name;
    const char *layer;
    const char *reason;
} hyp_package_layer_t;

typedef struct {
    int id;
    const char *label;
    int members;
    double cohesion;
    const char **top_nodes;
    int top_node_count;
    const char **packages;
    int package_count;
    const char **edge_types;
    int edge_type_count;
} hyp_cluster_info_t;

typedef struct {
    const char *path;
    const char *type; /* "dir" or "file" */
    int children;
} hyp_file_tree_entry_t;

typedef struct {
    /* Pointers first to minimize padding */
    hyp_language_count_t *languages;
    hyp_package_summary_t *packages;
    hyp_entry_point_t *entry_points;
    hyp_route_info_t *routes;
    hyp_hotspot_t *hotspots;
    hyp_cross_pkg_boundary_t *boundaries;
    hyp_service_link_t *services;
    hyp_package_layer_t *layers;
    hyp_cluster_info_t *clusters;
    hyp_file_tree_entry_t *file_tree;
    /* Counts after pointers */
    int language_count;
    int package_count;
    int entry_point_count;
    int route_count;
    int hotspot_count;
    int boundary_count;
    int service_count;
    int layer_count;
    int cluster_count;
    int file_tree_count;
} hyp_architecture_info_t;

int hyp_store_get_architecture(hyp_store_t *s, const char *project, const char *path,
                               const char **aspects, int aspect_count,
                               hyp_architecture_info_t *out);
void hyp_store_architecture_free(hyp_architecture_info_t *out);

/* ── ADR (Architecture Decision Record) ────────────────────────── */

#define HYP_ADR_MAX_LENGTH 8000

typedef struct {
    const char *project;
    const char *content;
    const char *created_at;
    const char *updated_at;
} hyp_adr_t;

int hyp_store_adr_store(hyp_store_t *s, const char *project, const char *content);
int hyp_store_adr_get(hyp_store_t *s, const char *project, hyp_adr_t *out);
int hyp_store_adr_delete(hyp_store_t *s, const char *project);
int hyp_store_adr_update_sections(hyp_store_t *s, const char *project, const char **keys,
                                  const char **values, int count, hyp_adr_t *out);
void hyp_store_adr_free(hyp_adr_t *adr);

/* ADR section parsing/rendering (pure functions, no store needed) */

enum { PROPS_MAX = 16 };

typedef struct {
    char *keys[PROPS_MAX];
    char *values[PROPS_MAX];
    int count;
} hyp_adr_sections_t;

hyp_adr_sections_t hyp_adr_parse_sections(const char *content);
char *hyp_adr_render(const hyp_adr_sections_t *sections);
int hyp_adr_validate_content(const char *content, char *errbuf, int errbuf_size);
int hyp_adr_validate_section_keys(const char **keys, int count, char *errbuf, int errbuf_size);
void hyp_adr_sections_free(hyp_adr_sections_t *s);

/* ── Search helpers (exposed for testing) ───────────────────────── */

/* Convert a glob pattern to SQL LIKE pattern. Caller must free result. */
char *hyp_glob_to_like(const char *pattern);

/* Extract literal substrings (>= 3 chars) from a regex pattern for LIKE pre-filtering.
 * Bails on alternation (|). Returns count of hints written to out[].
 * Each out[i] is malloc'd — caller must free each string. */
int hyp_extract_like_hints(const char *pattern, char **out, int max_out);

/* Prepend (?i) to a regex pattern if not already present.
 * Returns a static buffer — do NOT free. */
const char *hyp_ensure_case_insensitive(const char *pattern);

/* Strip leading (?i) from a regex pattern.
 * Returns a static buffer — do NOT free. */
const char *hyp_strip_case_flag(const char *pattern);

/* ── Architecture helpers (exposed for testing) ────────────────── */

const char *hyp_qn_to_package(const char *qn);
const char *hyp_qn_to_top_package(const char *qn);
bool hyp_is_test_file_path(const char *fp);
int hyp_store_find_architecture_docs(hyp_store_t *s, const char *project, char ***out, int *count);

/* ── Community detection (Leiden) ──────────────────────────────── */

typedef struct {
    int64_t src;
    int64_t dst;
} hyp_louvain_edge_t;

typedef struct {
    int64_t node_id;
    int community;
} hyp_louvain_result_t;

/* Multi-level Leiden community detection (Traag, Waltman & van Eck 2019,
 * arXiv:1810.08473): local moving + refinement + aggregation, repeated until
 * the partition can no longer be coarsened. Refinement guarantees every
 * reported community is internally connected. The resolution parameter
 * controls granularity (higher -> more, smaller communities); 1.0 is standard.
 * Allocates *out (length *out_count == node_count); the caller frees it. */
int hyp_leiden(const int64_t *nodes, int node_count, const hyp_louvain_edge_t *edges,
               int edge_count, double resolution, hyp_louvain_result_t **out, int *out_count);

/* Convenience wrapper: hyp_leiden with resolution 1.0. */
int hyp_louvain(const int64_t *nodes, int node_count, const hyp_louvain_edge_t *edges,
                int edge_count, hyp_louvain_result_t **out, int *out_count);

/* ── Memory management helpers ──────────────────────────────────── */

/* Free heap-allocated strings in a stack-allocated node (does NOT free the node itself). */
void hyp_node_free_fields(hyp_node_t *n);

/* Free heap-allocated strings in a stack-allocated project (does NOT free the project itself). */
void hyp_project_free_fields(hyp_project_t *p);

/* Free an array of nodes returned by find_nodes_by_* functions. */
void hyp_store_free_nodes(hyp_node_t *nodes, int count);

/* Free an array of edges returned by find_edges_by_* functions. */
void hyp_store_free_edges(hyp_edge_t *edges, int count);

/* Free an array of projects. */
void hyp_store_free_projects(hyp_project_t *projects, int count);

/* Free an array of file hashes. */
void hyp_store_free_file_hashes(hyp_file_hash_t *hashes, int count);

/* ── Vector search ───────────────────────────────────────────────── */

/* Result from vector similarity search. */
typedef struct {
    int64_t node_id;
    char *name;
    char *qualified_name;
    char *file_path;
    char *label;
    double score;
} hyp_vector_result_t;

/* Search for nodes similar to the given query keywords using stored RI vectors.
 * Builds a merged query vector from the keywords, then does cosine scan via
 * the hyp_cosine_i8 SQL function joined with the nodes table.
 * Returns results sorted by score DESC. Caller must free with hyp_store_free_vector_results. */
int hyp_store_vector_search(hyp_store_t *s, const char *project, const char **keywords,
                            int keyword_count, int limit, hyp_vector_result_t **out,
                            int *out_count);

/* Free vector search results. */
void hyp_store_free_vector_results(hyp_vector_result_t *results, int count);

/* Count vectors for a project. */
int hyp_store_count_vectors(hyp_store_t *s, const char *project);

/* Execute an arbitrary SQL statement (pragmas, FTS5 maintenance, etc).
 * Returns HYP_STORE_OK on success. */
int hyp_store_exec(hyp_store_t *s, const char *sql);

#endif /* HYP_STORE_H */
