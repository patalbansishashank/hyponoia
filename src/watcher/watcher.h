/*
 * watcher.h — File change watcher for auto-reindexing.
 *
 * Polls indexed projects for git changes (HEAD movement or dirty working tree)
 * and triggers re-indexing via a callback. Uses adaptive polling intervals
 * based on project size (5s base + 1s per 500 files, capped at 60s).
 *
 * Depends on: foundation, store (for project metadata)
 */
#ifndef HYP_WATCHER_H
#define HYP_WATCHER_H

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
typedef struct hyp_store hyp_store_t;

/* ── Opaque handle ──────────────────────────────────────────────── */

typedef struct hyp_watcher hyp_watcher_t;

/* ── Index callback ─────────────────────────────────────────────── */

/* Called when file changes are detected. Return 0 on success, a POSITIVE
 * value when the reindex was skipped and should be retried on the next poll
 * (e.g. another pipeline holds the lock), negative on error. Only a 0 return
 * commits the watcher's change baselines — a skipped or failed reindex keeps
 * the change pending so it is retried, never silently lost (#937).
 * project_name: project identifier
 * root_path: absolute path to the repository root */
typedef int (*hyp_index_fn)(const char *project_name, const char *root_path, void *user_data);

/* ── Memory sync trigger ────────────────────────────────────────── */

/* Called in the SAME poll pass that detected a local change, before the
 * reindex callback. The trigger for pulling shared memory is the code index,
 * not an agent and not a clock: a changed tree is exactly the moment an agent
 * asks "what happened here", so the records about that work have to be present
 * already. The watcher stays free of record vocabulary — it reports the
 * change, and whoever installed this decides what a pull means.
 *
 * It must not affect the reindex: its return value is not consulted, and a
 * failure here neither skips a reindex nor commits a change baseline. */
typedef void (*hyp_watcher_memory_sync_fn)(const char *project_name, const char *root_path,
                                           void *context);

/* Optional daemon coordination for destructive stale-root pruning. begin is
 * non-blocking: a false result preserves the watch and retries on a later
 * poll. A successful begin is paired with end. pruned is called after the
 * physical watch and cached DB have been removed so the daemon can invalidate
 * its logical subscriptions. All callbacks use the same borrowed context. */
typedef bool (*hyp_watcher_project_mutation_begin_fn)(void *context, const char *project);
typedef void (*hyp_watcher_project_mutation_end_fn)(void *context, const char *project);
typedef void (*hyp_watcher_project_pruned_fn)(void *context, const char *project);

/* ── Lifecycle ──────────────────────────────────────────────────── */

/* Create a new watcher. store is used for project metadata lookups.
 * index_fn is called when file changes are detected.
 * user_data is passed to index_fn. */
hyp_watcher_t *hyp_watcher_new(hyp_store_t *store, hyp_index_fn index_fn, void *user_data);

/* Free the watcher and all per-project state. NULL-safe.
 * Precondition: hyp_watcher_stop() + thread join must have completed. */
void hyp_watcher_free(hyp_watcher_t *w);

/* Install or clear daemon-owned stale-root coordination. Passing NULL for
 * begin/end clears all callbacks. The setter waits for an in-flight prune
 * callback to finish before returning. */
void hyp_watcher_set_project_mutation_guard(hyp_watcher_t *w,
                                            hyp_watcher_project_mutation_begin_fn begin,
                                            hyp_watcher_project_mutation_end_fn end,
                                            hyp_watcher_project_pruned_fn pruned, void *context);

/* Install or clear the memory sync trigger. Passing NULL for fn clears it.
 * Safe to call while the poll loop runs. */
void hyp_watcher_set_memory_sync(hyp_watcher_t *w, hyp_watcher_memory_sync_fn fn, void *context);

/* ── Watch list management ──────────────────────────────────────── */

/* Add a project to the watch list. root_path is copied. Returns true only when
 * the physical registration exists (including an identical existing watch).
 * A stopped watcher rejects new registrations. */
bool hyp_watcher_watch(hyp_watcher_t *w, const char *project_name, const char *root_path);

/* Remove a project from the watch list. Any not-yet-admitted callback in the
 * current poll snapshot is invalidated before this function returns. */
void hyp_watcher_unwatch(hyp_watcher_t *w, const char *project_name);

/* Refresh a project's timestamp (resets adaptive backoff). */
void hyp_watcher_touch(hyp_watcher_t *w, const char *project_name);

/* ── Polling ────────────────────────────────────────────────────── */

/* Run a single poll cycle — check each watched project for changes.
 * Returns the number of projects that were reindexed. */
int hyp_watcher_poll_once(hyp_watcher_t *w);

/* Run the blocking poll loop. Polls every base_interval_ms until
 * hyp_watcher_stop() is called. Returns 0 on clean shutdown. */
int hyp_watcher_run(hyp_watcher_t *w, int base_interval_ms);

/* Request the run loop to stop (thread-safe). */
void hyp_watcher_stop(hyp_watcher_t *w);

/* ── Introspection (for testing) ────────────────────────────────── */

/* Return the number of projects in the watch list. */
int hyp_watcher_watch_count(hyp_watcher_t *w);

/* Return the adaptive poll interval (ms) for a given file count. */
int hyp_watcher_poll_interval_ms(int file_count);

/* Classify a stat() errno observed on a watched project root: returns true
 * only for values that mean the root itself is gone (ENOENT, ENOTDIR) and
 * may count toward stale-root pruning (#286). Any other failure (EACCES,
 * EIO, transient mounts, macOS TCC revocation) must NOT count — the cached
 * DB holds user-authored data and is unrecoverable once pruned. Exposed
 * for direct unit testing with injected errno values. */
bool hyp_watcher_root_missing_errno(int err);

#endif /* HYP_WATCHER_H */
