/*
 * http_server.h — Embedded HTTP server for the graph visualization UI.
 *
 * Binds to 127.0.0.1:<port> only (localhost).
 * Serves the verified external frontend pack and proxies /rpc to a dedicated
 * read-only hyp_mcp_server_t instance.
 *
 * Runs in a background pthread, same pattern as the watcher thread.
 */
#ifndef HYP_UI_HTTP_SERVER_H
#define HYP_UI_HTTP_SERVER_H

#include "ui/httpd.h"
#include "foundation/sha256.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct hyp_http_server hyp_http_server_t;
struct hyp_watcher;

/* Daemon-owned index boundary. The callback blocks until the coordinated
 * operation finishes; the HTTP server runs it on a tracked, joinable thread. */
typedef int (*hyp_http_index_executor_fn)(void *context, const char *root_path,
                                          const char *project_name);
typedef bool (*hyp_http_project_mutation_begin_fn)(void *context, const char *project);
typedef void (*hyp_http_project_mutation_end_fn)(void *context, const char *project);

/* Create an HTTP server on the given port.
 * Creates its own hyp_mcp_server_t with a separate read-only SQLite connection.
 * Returns NULL on failure (e.g. port in use). */
hyp_http_server_t *hyp_http_server_new(int port);

/* Free a quiescent HTTP server. Returns false without freeing if the run loop
 * or an index callback can still access it; callers must fail-stop rather than
 * continue cleanup in that state. */
bool hyp_http_server_free(hyp_http_server_t *srv);

/* Signal the HTTP server to stop (safe to call from any thread). */
void hyp_http_server_stop(hyp_http_server_t *srv);

/* Claim a future run before handing the server pointer to a new thread. This
 * closes the create-to-child-start lifetime gap. If thread creation fails,
 * cancel the still-scheduled run before freeing. */
bool hyp_http_server_schedule_run(hyp_http_server_t *srv);
bool hyp_http_server_cancel_scheduled_run(hyp_http_server_t *srv);

/* Run the scheduled HTTP server event loop from the background thread.
 * Blocks until hyp_http_server_stop() is called. */
void hyp_http_server_run(hyp_http_server_t *srv);

/* Observation-only phase seam for deterministic concurrency tests. */
hyp_httpd_activity_t hyp_http_server_activity_for_test(hyp_http_server_t *srv);

/* Check if the server started successfully (listener bound). */
bool hyp_http_server_is_running(const hyp_http_server_t *srv);

/* The actually-bound port (useful when constructed with port 0 in tests). */
int hyp_http_server_port(const hyp_http_server_t *srv);

/* Override the per-connection receive deadline (tests use short values). */
void hyp_http_server_set_recv_deadline_ms(hyp_http_server_t *srv, int ms);

/* Set external watcher reference for UI project lifecycle actions. Not owned. */
void hyp_http_server_set_watcher(hyp_http_server_t *srv, struct hyp_watcher *watcher);

/* Route UI indexing through the daemon's shared operation registry. */
void hyp_http_server_set_index_executor(hyp_http_server_t *srv, hyp_http_index_executor_fn executor,
                                        void *context);

/* Route direct UI mutations and /rpc mutation tools through the daemon's
 * per-project coordination gate. Direct UI calls are non-blocking: begin=false
 * is returned to the browser as a retryable locked response. */
void hyp_http_server_set_project_mutation_guard(hyp_http_server_t *srv,
                                                hyp_http_project_mutation_begin_fn begin,
                                                hyp_http_project_mutation_end_fn end,
                                                void *context);

/* Copy the daemon generation's private readiness secret into the server
 * before its run loop starts. GET /__hyp/ui-readiness returns only a
 * challenge-bound HMAC proof; the secret itself is never served. */
void hyp_http_server_set_readiness_secret(hyp_http_server_t *srv,
                                          const uint8_t secret[HYP_SHA256_DIGEST_LEN]);

/* Initialize the log ring buffer mutex. Must be called once before any threads. */
void hyp_ui_log_init(void);

/* Append a log line to the UI ring buffer (called from log hook). */
void hyp_ui_log_append(const char *line);

/* Set the binary path for subprocess spawning (call from main). */
void hyp_http_server_set_binary_path(const char *path);

/* Resolve argv[0] into an executable path suitable for subprocess spawning. */
bool hyp_http_server_resolve_binary_path(const char *argv0, char *out, size_t outsz);

/* Pure git-remote URL helpers used by GET /api/repo-info. Exposed for tests. */

/* Normalize a git remote (scp-style / ssh:// / https://) to a canonical
 * "https://host/org/repo" web base, with trailing ".git" and any embedded
 * credentials removed. malloc'd or NULL. Caller frees. */
char *hyp_ui_git_web_base(const char *url);

/* Copy `url` with any "user[:password]@" userinfo stripped from a
 * scheme://authority URL (scp-style is left unchanged). malloc'd, or NULL when
 * `url` is NULL. Caller frees. */
char *hyp_ui_git_strip_credentials(const char *url);

#endif /* HYP_UI_HTTP_SERVER_H */
