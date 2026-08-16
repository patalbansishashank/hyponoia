/*
 * main.c — Entry point for hyponoia.
 *
 * Modes:
 *   (default)       Run as MCP server on stdin/stdout (JSON-RPC 2.0)
 *   cli <tool> <json>  Run a single tool call and print result
 *   --version       Print version and exit
 *   --help          Print usage and exit
 *   --ui=true/false Enable/disable HTTP UI server (persisted)
 *   --port=N        Set HTTP UI port (persisted, default 9749)
 *   --tool-profile=analysis|scout  Expose a restricted agent tool surface
 *
 * Long-lived MCP and hook frontends are thin clients of one mandatory
 * per-account daemon. One-shot CLI tool calls run in an isolated local server
 * and never create or retain a daemon generation.
 */
#ifdef _WIN32
/* winsock2 must precede every project header that can transitively include
 * windows.h, otherwise the legacy winsock declarations conflict. */
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "hyp.h"
#include "store/store.h" // hyp_alloc_init — bind 3rd-party allocators to mimalloc before any sqlite/git init
#include "daemon/application.h"
#include "daemon/bootstrap.h"
#include "daemon/frontend.h"
#include "daemon/host.h"
#include "daemon/ipc.h"
#include "daemon/project_lock.h"
#include "daemon/version_cohort.h"
#include "mcp/mcp.h"
#include "mcp/index_supervisor.h"
#include "ask/ask_cmd.h"
#include "ask/ask_llama.h"
#include "ask/ask_provider.h" /* key custody: whose environment this process is */
#include "cli/cli.h"
#include "cli/model_fetch.h"
#include "cli/progress_sink.h"
#include "foundation/constants.h"

enum {
    MAIN_MIN_ARGC = 1,
    MAIN_CLI_ARGC = 2,
    MAIN_FLAG_OFF = 5, /* strlen("--ui=") */
    MAIN_PORT_OFF = 7, /* strlen("--port=") */
    MAIN_MAX_PORT = 65536,
    MAIN_PATH_CAP = 4096,
    MAIN_CONNECT_TIMEOUT_MS = 1000,
    MAIN_STARTUP_TIMEOUT_MS = 10000,
    MAIN_MCP_STARTUP_TIMEOUT_MS = 30000,
    MAIN_REQUEST_TIMEOUT_MS = 24 * 60 * 60 * 1000,
    MAIN_HOOK_CONNECT_TIMEOUT_MS = 250,
    MAIN_HOOK_REQUEST_TIMEOUT_MS = 1500,
    MAIN_HOOK_CLOSE_TIMEOUT_MS = 250,
    MAIN_HOOK_NOTICE_INTERVAL_SECONDS = 900,
    MAIN_CLOSE_TIMEOUT_MS = 5000,
    MAIN_COORDINATION_CLEANUP_MS = 500,
    PARENT_WATCHDOG_STACK_SIZE = 64 * HYP_SZ_1K, /* watchdog only polls — tiny stack suffices */
};
#define SLEN(s) (sizeof(s) - 1)
#include "foundation/log.h"
#include "foundation/diagnostics.h"
#include "foundation/platform.h"
#include "foundation/workspace.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/mem.h"
#include "foundation/profile.h"
#include "foundation/sha256.h"
#include "foundation/secure_random.h"
#include "foundation/win_utf8.h" /* hyp_wide_to_utf8 — Windows UTF-8 argv (#423/#20); no-op on POSIX */
#ifdef _WIN32
#include <shellapi.h> /* CommandLineToArgvW — not pulled in by windows.h under WIN32_LEAN_AND_MEAN */
#include <io.h>
#endif
#include "ui/http_server.h"
#include "ui/asset_pack.h"
#include "ui/config.h"
#include <yyjson/yyjson.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef HYP_VERSION
#define HYP_VERSION "dev"
#endif

/* Optional, injected by scripts/build.sh --build-sha. Deliberately not a git
 * call inside the Makefile: BUILD_CONFIG_SIG carries CFLAGS_EXTRA, so a SHA
 * that changed on every commit would force the whole one-shot compile+link on
 * every commit. See Makefile.hyp's note next to LLAMA_VERSION_DEFINES for the
 * same objection to build-time entropy in a compile line. */
/* Tested with #ifdef rather than against a defaulted "" — a runtime
 * `if (source_sha[0])` over a default empty string is a branch the compiler
 * (and cppcheck, which fails the lint gate on it) can prove is dead in every
 * build that did not inject one, which is most of them. */

/* Hex characters of the executable fingerprint shown to a human. Matches the
 * `%.12s` that `hyponoia daemon status` prints on its `build:` line, so a bug
 * report and a running daemon can be compared by eye. */
#define MAIN_VERSION_BUILD_HEX 12

/* `hyponoia --version` used to print "hyponoia dev" and nothing else, so a bug
 * report from any build that CI did not stamp could not be tied to a tree at
 * all. The daemon has always known exactly which bytes it is running -- the
 * SHA-256 of its own executable image -- and prints it in `daemon status`.
 * This prints the SAME identifier, so the two are comparable, and stays one
 * line because scripts/smoke-test.sh phase 4c requires exactly one.
 *
 * The hash costs one full read of the executable (~0.2 s for the shipped
 * ~340 MB image, warm). That is paid only here and only when the build did not
 * record a source SHA of its own; every other caller reuses the process-wide
 * capture. */
static void main_print_version(void) {
#ifdef HYP_BUILD_SHA
    /* The build stamped its own source SHA, so nothing has to be read. */
    printf("hyponoia %s (%s)\n", HYP_VERSION, HYP_BUILD_SHA);
#else
    const char *fingerprint = hyp_index_supervisor_capture_build_fingerprint()
                                  ? hyp_index_supervisor_build_fingerprint()
                                  : NULL;
    if (fingerprint && fingerprint[0]) {
        printf("hyponoia %s (%.*s)\n", HYP_VERSION, MAIN_VERSION_BUILD_HEX, fingerprint);
        return;
    }
    printf("hyponoia %s\n", HYP_VERSION);
#endif
}

/* ── Globals for signal handling ────────────────────────────────── */

static atomic_int g_shutdown = 0;
static hyp_daemon_runtime_client_t *g_daemon_client = NULL;

static uint64_t main_deadline_after(uint32_t timeout_ms);

/* This process's own id, for a human-readable disclosure. Same shape as
 * host.c's host_current_process_id — that one is static to the daemon host and
 * this is needed before the host is entered. */
static uint64_t main_current_process_id(void) {
#ifdef _WIN32
    return (uint64_t)GetCurrentProcessId();
#else
    return (uint64_t)getpid();
#endif
}

static bool main_session_context(const char *preferred_root, char root_out[MAIN_PATH_CAP],
                                 char allowed_out[MAIN_PATH_CAP], const char **allowed_out_ptr);

typedef struct main_local_cli_lease main_local_cli_lease_t;

struct main_local_cli_lease {
    char *project;
    hyp_project_lock_lease_t *lease;
    main_local_cli_lease_t *next;
};

typedef struct {
    hyp_project_lock_manager_t *manager;
    main_local_cli_lease_t *leases;
    FILE *feedback;
    bool index_worker;
    bool waiting_reported;
} main_local_cli_mutation_t;

typedef struct {
    hyp_mutex_t mutex;
    hyp_mcp_server_t *server;
    bool maintenance_cancelled;
} main_local_maintenance_context_t;

static void main_local_maintenance_context_init(main_local_maintenance_context_t *context) {
    memset(context, 0, sizeof(*context));
    hyp_mutex_init(&context->mutex);
}

static void main_local_maintenance_context_destroy(main_local_maintenance_context_t *context) {
    hyp_mutex_destroy(&context->mutex);
    memset(context, 0, sizeof(*context));
}

static void main_local_maintenance_server_bind(main_local_maintenance_context_t *context,
                                               hyp_mcp_server_t *server) {
    if (!context) {
        return;
    }
    hyp_mutex_lock(&context->mutex);
    context->server = server;
    hyp_mutex_unlock(&context->mutex);
}

static bool main_local_command_cancel(void *opaque) {
    main_local_maintenance_context_t *context = opaque;
    if (!context) {
        return false;
    }
    hyp_mutex_lock(&context->mutex);
    bool cancelled = context->server && hyp_mcp_server_cancel_active(context->server);
    context->maintenance_cancelled = context->maintenance_cancelled || cancelled;
    hyp_mutex_unlock(&context->mutex);
    return cancelled;
}

static bool main_local_maintenance_was_cancelled(main_local_maintenance_context_t *context) {
    if (!context) {
        return false;
    }
    hyp_mutex_lock(&context->mutex);
    bool cancelled = context->maintenance_cancelled;
    hyp_mutex_unlock(&context->mutex);
    return cancelled;
}

static void main_local_maintenance_finish(hyp_daemon_maintenance_monitor_t **monitor,
                                          main_local_maintenance_context_t *context,
                                          bool context_initialized, const char *participant) {
    if (monitor && *monitor && !hyp_daemon_maintenance_monitor_stop(monitor)) {
        /* The observer still borrows context (and may be inside cancellation).
         * Freeing command/server/manager memory would be a cross-thread UAF. */
        hyp_log_error("participant.maintenance_join_failed", "participant", participant, "action",
                      "process_exit");
        (void)fflush(stdout);
        (void)fflush(stderr);
        _Exit(EXIT_FAILURE);
    }
    if (context_initialized) {
        main_local_maintenance_context_destroy(context);
    }
}

static _Noreturn void main_coordination_cleanup_fail_stop(const char *component) {
    hyp_log_error("coordination.cleanup_timeout", "component", component, "action", "process_exit");
    (void)fprintf(stderr,
                  "hyponoia: coordination cleanup timed out (%s); "
                  "terminating so the OS releases retained claims\n",
                  component ? component : "unknown");
    (void)fflush(stdout);
    (void)fflush(stderr);
    _Exit(EXIT_FAILURE);
}

static void main_project_lock_release_fully(hyp_project_lock_lease_t **lease) {
    uint64_t deadline = main_deadline_after(MAIN_COORDINATION_CLEANUP_MS);
    while (lease && *lease) {
        (void)hyp_project_lock_lease_release(lease);
        if (!*lease) {
            return;
        }
        if (hyp_now_ms() >= deadline) {
            main_coordination_cleanup_fail_stop("project_lock_cleanup");
        }
        hyp_usleep(1000);
    }
}

/* Test-only ownership proof consumed by the POSIX worker-lease contract tests.
 * The environment variable is otherwise inert, and only a supervised physical
 * worker may publish it. Publication occurs after the native project lease is
 * acquired, so a marker from the worker also proves that its polling supervisor
 * did not retain the same exclusive lease.
 *
 * COMPILED OUT of ordinary builds alongside the watchdog probe above. This one
 * is benign in isolation (an O_EXCL|O_NOFOLLOW PID file), but it is still
 * test-only code reachable through a caller-supplied path in a shipped binary,
 * and its consumers all build with TEST_SEAMS=1. The crash/hang injectors in
 * internal/hyp/hyp.c are governed by the same boundary. The narrowly validated
 * Windows PATH smoke redirect remains in release artifacts so artifact tests
 * never mutate the runner's live user PATH. */
#ifdef HYP_ENABLE_TEST_SEAMS
static bool main_test_worker_project_lock_marker(const main_local_cli_mutation_t *mutation) {
#ifdef _WIN32
    (void)mutation;
    return true;
#else
    if (!mutation || !mutation->index_worker) {
        return true;
    }
    char marker_path[MAIN_PATH_CAP] = {0};
    if (!hyp_safe_getenv("HYP_TEST_WORKER_PROJECT_LOCK_PID_FILE", marker_path, sizeof(marker_path),
                         NULL) ||
        !marker_path[0]) {
        return true;
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int marker = open(marker_path, flags, 0600);
    if (marker < 0) {
        return false;
    }
    char identity[96];
    int length = snprintf(identity, sizeof(identity), "%ld %ld\n", (long)getpid(), (long)getpgrp());
    bool written = length > 0 && length < (int)sizeof(identity) &&
                   write(marker, identity, (size_t)length) == (ssize_t)length;
    return close(marker) == 0 && written;
#endif
}
#endif

static bool main_local_cli_mutation_begin_internal(void *context, const char *project, bool wait) {
    main_local_cli_mutation_t *mutation = context;
    if (!mutation || !mutation->manager || !project || !project[0]) {
        return false;
    }
    for (;;) {
        hyp_project_lock_lease_t *lease = NULL;
        hyp_private_file_lock_status_t status;
        if (wait) {
            uint64_t now = hyp_now_ms();
            uint64_t deadline = now > UINT64_MAX - 100U ? UINT64_MAX : now + 100U;
            status = hyp_project_lock_acquire(mutation->manager, project, deadline, NULL, &lease);
        } else {
            status = hyp_project_lock_try_acquire(mutation->manager, project, &lease);
        }
        if (status == HYP_PRIVATE_FILE_LOCK_OK && lease) {
            main_local_cli_lease_t *held = calloc(1, sizeof(*held));
            if (held) {
                held->project = hyp_strdup(project);
            }
            if (!held || !held->project) {
                free(held);
                main_project_lock_release_fully(&lease);
                return false;
            }
            held->lease = lease;
            held->next = mutation->leases;
            mutation->leases = held;
#ifdef HYP_ENABLE_TEST_SEAMS
            if (!main_test_worker_project_lock_marker(mutation)) {
                mutation->leases = held->next;
                main_project_lock_release_fully(&held->lease);
                free(held->project);
                free(held);
                return false;
            }
#endif
            return true;
        }
        main_project_lock_release_fully(&lease);
        if (status != HYP_PRIVATE_FILE_LOCK_BUSY) {
            hyp_log_error("cli.project_lock_failed", "project", project, "action",
                          "refuse_mutation");
            return false;
        }
        if (!wait) {
            return false;
        }
        if (mutation->feedback && !mutation->waiting_reported) {
            (void)fprintf(mutation->feedback, "Waiting for another HYP mutation of %s...\n",
                          project);
            (void)fflush(mutation->feedback);
            mutation->waiting_reported = true;
        }
    }
}

static bool main_local_cli_mutation_begin(void *context, const char *project) {
    return main_local_cli_mutation_begin_internal(context, project, true);
}

static bool main_local_cli_mutation_try_begin(void *context, const char *project) {
    return main_local_cli_mutation_begin_internal(context, project, false);
}

static void main_local_cli_mutation_end(void *context, const char *project) {
    main_local_cli_mutation_t *mutation = context;
    if (!mutation || !project) {
        return;
    }
    main_local_cli_lease_t **cursor = &mutation->leases;
    while (*cursor && strcmp((*cursor)->project, project) != 0) {
        cursor = &(*cursor)->next;
    }
    main_local_cli_lease_t *held = *cursor;
    if (held) {
        *cursor = held->next;
        main_project_lock_release_fully(&held->lease);
        free(held->project);
        free(held);
    }
}

static void main_local_cli_mutation_release_all(main_local_cli_mutation_t *mutation) {
    while (mutation && mutation->leases) {
        main_local_cli_lease_t *held = mutation->leases;
        mutation->leases = held->next;
        main_project_lock_release_fully(&held->lease);
        free(held->project);
        free(held);
    }
}

/* Signal handlers only publish intent and close stdin. The daemon host observes
 * the atomic; an MCP thin client unblocks its reader and closes its authenticated
 * daemon connection from normal thread context. */
static void request_shutdown(void) {
    if (atomic_exchange(&g_shutdown, 1)) {
        return; /* already shutting down */
    }
#ifdef _WIN32
    (void)_close(_fileno(stdin));
#else
    (void)close(STDIN_FILENO);
#endif
}

static void signal_handler(int sig) {
    (void)sig;
    request_shutdown();
}

/* ── Parent-process watchdog ────────────────────────────────────── */
/* parent-death watchdog — distilled from #407 (fixes #406, thanks @nvt-pankajsharma).
 *
 * When this stdio MCP server is launched by an agent that later dies without a
 * clean SIGTERM (e.g. the editor is force-killed), the orphaned server would
 * otherwise linger forever blocked on stdin. POSIX has no portable "notify on
 * parent death" primitive (PR_SET_PDEATHSIG is Linux-only), so we poll getppid:
 * once the parent dies the process is reparented (ppid changes, typically to 1)
 * and we shut down. Windows is unaffected (job objects handle this) — #ifndef. */

#ifndef _WIN32
typedef struct {
    pid_t initial_ppid;
    bool kill_worker_group;
    bool exit_on_parent_death;
} parent_watchdog_config_t;

static void *parent_watchdog_thread(void *arg) {
    parent_watchdog_config_t config = *(parent_watchdog_config_t *)arg;
    const unsigned int poll_interval_us = 500000; /* 500ms */

    while (!atomic_load(&g_shutdown)) {
        hyp_usleep(poll_interval_us);
        if (atomic_load(&g_shutdown)) {
            break;
        }
        /* initial_ppid > 1 guards against an already-orphaned start (ppid==1),
         * where a changing ppid carries no signal. */
        if (config.initial_ppid > 1 && getppid() != config.initial_ppid) {
            static const char msg[] = "level=warn msg=parent.exited reason=ppid_changed\n";
            (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
            if (config.kill_worker_group) {
                /* Valid workers establish pgid == pid before any stateful work.
                 * SIGKILL is deliberate: no non-escaped descendant may continue
                 * after the owning daemon disappears. */
                (void)kill(-getpid(), SIGKILL);
            }
            if (config.exit_on_parent_death) {
                /* Kernel EOF on every inherited daemon socket is the most
                 * reliable cancellation signal when an agent disappears. */
                _exit(0);
            }
            request_shutdown();
            break;
        }
    }
    return NULL;
}

static bool worker_prepare_process_group(void) {
    pid_t process_id = getpid();
    return (setpgid(0, 0) == 0 || getpgrp() == process_id) && getpgrp() == process_id;
}

/* A worker that cannot contain its own process tree must not index: on failure
 * it would keep running as an orphan with no supervisor able to reap it. Shared
 * by the ordered containment steps in the worker path so all of them fail
 * identically — write() and _exit() rather than fprintf/exit, because this runs
 * after fork-sensitive setup and must not touch stdio locks or atexit handlers.
 */
static void worker_containment_unavailable(void) {
    static const char message[] =
        "HYP index worker could not start: process-tree containment unavailable\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)kill(-getpid(), SIGKILL);
    _exit(EXIT_FAILURE);
}

/* Test-only crash-orphan probe used by tests/test_worker_watchdog.sh. It is
 * created before the watchdog thread so fork never occurs in a multithreaded
 * worker, and inherits the worker's isolated process group.
 *
 * COMPILED OUT of ordinary builds (see TEST_SEAMS in Makefile.hyp). "Fork a
 * child that ignores SIGTERM and loops forever, then write its PID to a path
 * the caller chose" is a fine test probe and an appalling thing to find in a
 * shipped executable — it is precisely the shape a generic malware classifier
 * is built to notice, and it has no production caller. Seams are OPT-IN so the
 * failure mode of forgetting the flag is a clean binary, not a leaky one; the
 * suites that need it build with TEST_SEAMS=1, and
 * scripts/ci/check-binary-composition.sh fails the release if the marker
 * string ever reappears in an artifact. */
#ifdef HYP_ENABLE_TEST_SEAMS
static bool worker_start_watchdog_test_descendant(void) {
    char pid_path[HYP_SZ_4K] = {0};
    if (!hyp_safe_getenv("HYP_TEST_WORKER_DESCENDANT_PID_FILE", pid_path, sizeof(pid_path), NULL) ||
        !pid_path[0]) {
        return true;
    }
    pid_t descendant = fork();
    if (descendant < 0) {
        return false;
    }
    if (descendant == 0) {
        (void)signal(SIGTERM, SIG_IGN);
        for (;;) {
            hyp_usleep(100000);
        }
    }
    int open_flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    open_flags |= O_NOFOLLOW;
#endif
    int pid_file = open(pid_path, open_flags, 0600);
    if (pid_file < 0) {
        (void)kill(descendant, SIGKILL);
        (void)waitpid(descendant, NULL, 0);
        return false;
    }
    char pid_text[32];
    int pid_length = snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)descendant);
    bool written = pid_length > 0 && pid_length < (int)sizeof(pid_text) &&
                   write(pid_file, pid_text, (size_t)pid_length) == (ssize_t)pid_length;
    written = close(pid_file) == 0 && written;
    if (!written) {
        (void)unlink(pid_path);
        (void)kill(descendant, SIGKILL);
        (void)waitpid(descendant, NULL, 0);
    }
    return written;
}
#endif

static bool worker_start_parent_watchdog(pid_t initial_ppid) {
    static parent_watchdog_config_t worker_config;
    worker_config.initial_ppid = initial_ppid;
    worker_config.kill_worker_group = true;
    worker_config.exit_on_parent_death = true;
    hyp_thread_t worker_watchdog_tid;
    if (hyp_thread_create(&worker_watchdog_tid, PARENT_WATCHDOG_STACK_SIZE, parent_watchdog_thread,
                          &worker_config) != 0) {
        return false;
    }
    return hyp_thread_detach(&worker_watchdog_tid) == 0;
}

static bool client_start_parent_watchdog(pid_t initial_ppid) {
    if (initial_ppid <= 1) {
        return true;
    }
    static parent_watchdog_config_t client_config;
    client_config.initial_ppid = initial_ppid;
    client_config.kill_worker_group = false;
    client_config.exit_on_parent_death = true;
    hyp_thread_t watchdog;
    if (hyp_thread_create(&watchdog, PARENT_WATCHDOG_STACK_SIZE, parent_watchdog_thread,
                          &client_config) != 0) {
        return false;
    }
    if (hyp_thread_detach(&watchdog) != 0) {
        atomic_store(&g_shutdown, 1);
        (void)hyp_thread_join(&watchdog);
        return false;
    }
    return true;
}
#endif

/* ── CLI mode ───────────────────────────────────────────────────── */

#define CLI_USAGE "Usage: hyponoia cli [--progress] [--json] <tool_name> [json_args]\n"

/* Extract text content from MCP tool result envelope and print it.
 * MCP results: {"content":[{"type":"text","text":"..."}],"isError":...}
 * Returns 1 if the result was an error, 0 otherwise. */
static int cli_print_mcp_result(const char *result) {
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    if (!doc) {
        printf("%s\n", result);
        return 0;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *err_val = yyjson_obj_get(root, "isError");
    bool is_error = err_val && yyjson_get_bool(err_val);

    const char *text = NULL;
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (yyjson_is_arr(content) && yyjson_arr_size(content) > 0) {
        yyjson_val *tv = yyjson_obj_get(yyjson_arr_get_first(content), "text");
        text = tv ? yyjson_get_str(tv) : NULL;
    }

    if (text) {
        (void)fprintf(is_error ? stderr : stdout, "%s\n", text);
    } else {
        printf("%s\n", result);
    }

    yyjson_doc_free(doc);
    return is_error ? SKIP_ONE : 0;
}

/* Strip a flag from argv, returning true if found. */
static bool cli_strip_flag(int *argc, char **argv, const char *flag) {
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], flag) != 0) {
            continue;
        }
        for (int j = i; j < *argc - SKIP_ONE; j++) {
            argv[j] = argv[j + SKIP_ONE];
        }
        (*argc)--;
        return true;
    }
    return false;
}

/* Strip a flag AND its following value from argv, returning the value (a pointer
 * into the original argv strings, valid for the process lifetime) or NULL if the
 * flag is absent. */
static const char *cli_strip_flag_value(int *argc, char **argv, const char *flag) {
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], flag) != 0) {
            continue;
        }
        const char *value = (i + SKIP_ONE < *argc) ? argv[i + SKIP_ONE] : NULL;
        int remove_count = value ? 2 : 1;
        for (int j = i; j < *argc - remove_count; j++) {
            argv[j] = argv[j + remove_count];
        }
        *argc -= remove_count;
        return value;
    }
    return NULL;
}

/* Portable "is fd a terminal?" — _isatty on Windows, isatty on POSIX. */
#ifdef _WIN32
#define cli_isatty(fd) _isatty(fd)
#else
#define cli_isatty(fd) isatty(fd)
#endif

enum { CLI_SLURP_CHUNK = 4096 };

/* Read an open stream fully into a heap, NUL-terminated string. Caller frees.
 * Returns NULL on allocation failure. Reads binary-clean (UTF-8 JSON, no shell
 * quoting needed). */
static char *cli_slurp_stream(FILE *f) {
    size_t cap = CLI_SLURP_CHUNK;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    char tmp[CLI_SLURP_CHUNK];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) {
                cap *= 2;
            }
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    buf[len] = '\0';
    return buf;
}

/* Slurp a file path into a heap, NUL-terminated string. Caller frees. */
static char *cli_slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *s = cli_slurp_stream(f);
    (void)fclose(f);
    return s;
}

/* True if the first non-whitespace byte of s is '{' (raw-JSON detection). */
static bool cli_first_nonspace_is_brace(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    return *s == '{';
}

static char *main_local_cli_daemon_execute(const char *tool_name, const char *args_json);

static int run_cli(int argc, char **argv, hyp_project_lock_manager_t *project_locks,
                   main_local_maintenance_context_t *maintenance_context) {
    if (argc == 1 && argv && (strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0)) {
        (void)fputs(CLI_USAGE, stdout);
        return 0;
    }
    if (argc < MAIN_MIN_ARGC) {
        (void)fprintf(stderr, CLI_USAGE);
        return SKIP_ONE;
    }

    bool progress_requested = cli_strip_flag(&argc, argv, "--progress");
    bool raw_json = cli_strip_flag(&argc, argv, "--json");

    /* Supervisor worker role: when this process was spawned as a supervised index
     * worker, run indexing in-process (never re-supervise) and write the result to
     * the given file for the parent to read back. Stripped here so the tool
     * dispatch below sees only the tool name + its args. */
    bool index_worker = cli_strip_flag(&argc, argv, "--index-worker");
    (void)cli_strip_flag_value(&argc, argv, HYP_INDEX_WORKER_BUILD_ARG);
    const char *response_out = cli_strip_flag_value(&argc, argv, "--response-out");
    (void)cli_strip_flag_value(&argc, argv, HYP_INDEX_WORKER_MEMORY_BUDGET_ARG);
    bool worker_single_thread = cli_strip_flag(&argc, argv, HYP_INDEX_WORKER_SINGLE_THREAD_ARG);
    const char *worker_marker = cli_strip_flag_value(&argc, argv, HYP_INDEX_WORKER_MARKER_ARG);
    const char *worker_quarantine =
        cli_strip_flag_value(&argc, argv, HYP_INDEX_WORKER_QUARANTINE_ARG);
    hyp_index_set_worker_role_options(index_worker, response_out, worker_single_thread,
                                      worker_marker, worker_quarantine,
                                      hyp_index_worker_memory_budget_bytes());

    if (argc < MAIN_MIN_ARGC) {
        (void)fprintf(stderr, CLI_USAGE);
        return SKIP_ONE;
    }

    const char *tool_name = argv[0];
    int rem_argc = argc - SKIP_ONE; /* args following the tool name */
    char **rem_argv = argv + SKIP_ONE;

    /* --help / -h : print per-tool help (from the tool's input_schema) and exit
     * before any server work. */
    for (int i = 0; i < rem_argc; i++) {
        if (strcmp(rem_argv[i], "--help") == 0 || strcmp(rem_argv[i], "-h") == 0) {
            if (hyp_cli_print_tool_help(tool_name) != 0) {
                (void)fprintf(stderr, "error: unknown tool '%s'\n", tool_name);
                return SKIP_ONE;
            }
            return 0;
        }
    }

    /* Resolve the JSON arguments. Precedence: --args-file, then raw JSON
     * (back-compat), then --flags, then piped stdin, then empty {}. */
    char *heap_args = NULL; /* freed before return when set */
    const char *args_json = "{}";

    int args_file_idx = -1;
    for (int i = 0; i < rem_argc; i++) {
        if (strcmp(rem_argv[i], "--args-file") == 0) {
            args_file_idx = i;
            break;
        }
    }

    if (args_file_idx >= 0) {
        if (args_file_idx + SKIP_ONE >= rem_argc) {
            (void)fprintf(stderr, "error: --args-file requires a path argument\n");
            return SKIP_ONE;
        }
        const char *path = rem_argv[args_file_idx + SKIP_ONE];
        heap_args = cli_slurp_file(path);
        if (!heap_args) {
            (void)fprintf(stderr, "error: cannot read args file '%s'\n", path);
            return SKIP_ONE;
        }
        args_json = heap_args;
    } else if (rem_argc >= SKIP_ONE && cli_first_nonspace_is_brace(rem_argv[0])) {
        /* raw-JSON back-compat: cli <tool> '{"k":"v"}' (deprecated path). Warn on
         * STDERR only — stdout must stay clean JSON for piping. */
        (void)fprintf(stderr,
                      "warning: passing raw JSON to 'cli %s' is deprecated and "
                      "will be removed in a future release; use flags (run 'cli "
                      "%s --help'), --args-file <path>, or piped stdin.\n",
                      tool_name, tool_name);
        args_json = rem_argv[0];
    } else if (rem_argc >= SKIP_ONE && strncmp(rem_argv[0], "--", 2) == 0) {
        /* flag form: cli <tool> --flag value --bare-bool ... */
        char *err = NULL;
        heap_args = hyp_cli_build_args_json(tool_name, rem_argc, rem_argv, &err);
        if (!heap_args) {
            (void)fprintf(stderr, "error: %s\n", err ? err : "invalid arguments");
            free(err);
            return SKIP_ONE;
        }
        args_json = heap_args;
    } else if (!cli_isatty(0)) {
        /* piped stdin (UTF-8 clean, no shell quoting): cli <tool> < args.json */
        heap_args = cli_slurp_stream(stdin);
        if (heap_args && heap_args[0]) {
            args_json = heap_args;
        } else {
            free(heap_args);
            heap_args = NULL;
            args_json = "{}";
        }
    }

    bool progress =
        !index_worker && hyp_cli_progress_enabled(progress_requested, cli_isatty(2) != 0);
    uint64_t progress_started_ms = hyp_now_ms();
    if (progress) {
        hyp_progress_sink_init(stderr);
        hyp_cli_progress_start(stderr, tool_name);
    }

    /* Indexing always executes daemon-side now (the daemon's supervisor
     * spawns and budgets the worker); no local supervision prep remains for
     * one-shot commands. */
    hyp_mcp_server_t *srv = NULL;
    char *result = NULL;
    main_local_cli_mutation_t mutation = {
        .manager = project_locks,
        .feedback = progress ? stderr : NULL,
        .index_worker = index_worker,
    };
    bool maintenance_binding_failed = false;
    bool maintenance_cancelled = false;
    if (!index_worker) {
        result = main_local_cli_daemon_execute(tool_name, args_json);
    } else {
        srv = hyp_mcp_server_new(NULL);
        if (srv) {
            /* The in-process worker is a standalone instance: it may not
             * launch MCP-session background tasks. It receives project_locks
             * from its own process-level coordination setup and therefore
             * owns the mutation lease while it performs the physical write. */
            hyp_mcp_server_set_background_tasks(srv, false);
            if (project_locks) {
                hyp_mcp_server_set_project_mutation_guard(srv, main_local_cli_mutation_begin,
                                                          main_local_cli_mutation_end, &mutation);
                hyp_mcp_server_set_project_mutation_try_guard(srv,
                                                              main_local_cli_mutation_try_begin);
            }
        }
        maintenance_binding_failed = srv && !maintenance_context;
        if (srv && maintenance_context) {
            main_local_maintenance_server_bind(maintenance_context, srv);
            result = hyp_mcp_handle_tool(srv, tool_name, args_json);
            /* Unbind under the same mutex used by cancellation before any
             * server teardown. The process-level monitor remains active
             * across all parsing and cleanup, but can no longer race a freed
             * server. */
            main_local_maintenance_server_bind(maintenance_context, NULL);
            maintenance_cancelled = main_local_maintenance_was_cancelled(maintenance_context);
        }
    }
    if (!result) {
        if (maintenance_binding_failed) {
            (void)fprintf(stderr,
                          "error: local %s maintenance cancellation could not bind safely\n",
                          index_worker ? "worker" : "CLI");
        } else if (index_worker) {
            (void)fprintf(stderr, "error: failed to run local worker server\n");
        }
        hyp_mcp_server_free(srv);
        main_local_cli_mutation_release_all(&mutation);
        if (progress) {
            hyp_progress_sink_fini();
            hyp_cli_progress_finish(stderr, tool_name, false, hyp_now_ms() - progress_started_ms);
        }
        free(heap_args);
        return SKIP_ONE;
    }
    int exit_code = 0;

    {
        /* Supervised worker: hand the full result string to the parent via the
         * response file before printing (parent reads it back on a clean exit). */
        const char *ro = hyp_index_worker_response_out();
        bool worker_response_written = false;
        if (ro) {
            FILE *rf = hyp_fopen(ro, "wb");
            if (rf) {
                int write_rc = fputs(result, rf);
                int close_rc = fclose(rf);
                worker_response_written = write_rc >= 0 && close_rc == 0;
            }
        }
        if (raw_json) {
            printf("%s\n", result);
            /* Raw JSON changes presentation only. Preserve a failing process
             * status for MCP tool errors so scripts and activation-driven
             * cancellation cannot be reported as successful work. */
            exit_code = hyp_cli_mcp_result_is_error(result) ? SKIP_ONE : 0;
        } else {
            exit_code = cli_print_mcp_result(result);
        }
        exit_code = hyp_cli_exit_status_after_maintenance(exit_code, maintenance_cancelled);
        if (hyp_index_worker_active()) {
            /* The supervisor protocol classifies the PROCESS, not the tool
             * result: a valid MCP error response is a healthy worker outcome.
             * Propagating cli_print_mcp_result's isError exit code made the
             * parent discard that response and falsely report exit_nonzero as
             * "crashed on a file". Fail only when the response transport itself
             * failed. Skip multi-GB teardown; the OS reclaims it at exit. */
            hyp_log_info("index.worker.fast_exit", "action", "_Exit");
            fflush(NULL);
            _Exit(worker_response_written ? 0 : SKIP_ONE);
        }
        free(result);
    }

    hyp_mcp_server_free(srv);
    main_local_cli_mutation_release_all(&mutation);
    if (progress) {
        hyp_progress_sink_fini();
        hyp_cli_progress_finish(stderr, tool_name, exit_code == 0,
                                hyp_now_ms() - progress_started_ms);
    }
    free(heap_args);
    return exit_code;
}

/* ── Help ───────────────────────────────────────────────────────── */

static void print_help(void) {
    printf("hyponoia %s\n\n", HYP_VERSION);
    printf("Usage:\n");
    printf("  hyponoia              Run MCP server on stdio\n");
    printf("  hyponoia cli [--progress] [--json] <tool> [args]\n");
    printf("                                      Run one tool locally, then exit\n");
    printf("  hyponoia install [-y|-n] [--force] [--dry-run] "
           "[--dir=<path>] [--skip-config]\n");
    printf("  hyponoia uninstall [-y|-n] [--dry-run]\n");
    printf("  hyponoia update [-y|-n]\n");
    printf("  hyponoia config <list|get|set|reset>\n");
    printf("  hyponoia embed --project <name> [--status]\n"
           "                                      Opt-in second pass: per-declaration\n"
           "                                      vectors for the `ask` lane\n");
    printf("  hyponoia fetch-model [-y] [--force] [--verify] [--path]\n");
    printf("                                      Download the `ask` lane's embedding model\n");
    printf("  hyponoia --version    Print version\n");
    printf("  hyponoia --help       Print this help\n");
    printf("\nUI options:\n");
    printf("  --ui=true    Enable HTTP graph visualization (persisted)\n");
    printf("  --ui=false   Disable HTTP graph visualization (persisted)\n");
    printf("  --port=N     Set UI port (default 9749, persisted)\n");
    printf("  --tool-profile=analysis|scout  Expose a restricted inspection surface\n");
    printf("\nSupported automatic/conditional client surfaces (43):\n");
    printf("  Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode,\n");
    printf("  Antigravity, Aider, KiloCode, VS Code, Cursor, Windsurf,\n");
    printf("  Augment / Auggie, OpenClaw, Kiro, Junie, Hermes, OpenHands,\n");
    printf("  Cline, Warp, Qwen Code, GitHub Copilot CLI, Factory Droid, Crush,\n");
    printf("  Goose, Mistral Vibe, Qoder CLI, Kimi Code CLI, GitLab Duo CLI,\n");
    printf("  Rovo Dev CLI, Amp, Devin CLI / Local, Tabnine, Continue / cn,\n");
    printf("  Visual Studio, TRAE, Roo Code, Amazon Q Developer IDE,\n");
    printf("  CodeBuddy Code CLI, IBM Bob IDE, IBM Bob Shell, Pochi, Pi,\n");
    printf("  Sourcegraph Cody\n");
    printf("  Conditional/explicit targets are changed only when their documented\n");
    printf("  platform, marker, or explicit existing config path is present.\n");
    printf("  Manual/UI MCP boundaries: Qodo, Warp, JetBrains AI/ACP, Replit,\n");
    printf("  Plandex, SWE-agent, BLACKBOX, GitHub cloud agents, Jules,\n");
    printf("  CodeRabbit.\n");
    /* Rendered from the MCP tool registry: a hand-maintained copy here
     * omitted check_index_coverage (#1361) and could silently drift again. */
    char *tools_help = hyp_mcp_tools_help_list();
    if (tools_help) {
        printf("\n%s", tools_help);
        free(tools_help);
    }
}

/* ── Main ───────────────────────────────────────────────────────── */

/* Try to handle a subcommand (cli/install/uninstall/update/config/--version/--help).
 * Returns -1 if no subcommand matched, otherwise the exit code. */
/* `allow-root [--approve-sensitive] <path>` — record an indexing root.
 *
 * Enrollment lives here, in a command a person types, and deliberately nowhere
 * else: the whole point of the grant store is that neither an indexed repository
 * nor a tool caller can widen its own boundary. A confirmation delivered through
 * the MCP surface would be answered by the same agent that may have been
 * influenced, so it would not be a human decision at all. */
static int main_run_allow_root(int argc, char **argv) {
    const char *path = NULL;
    bool approve_sensitive = false;
    bool list_only = false;
    bool approve_manifest = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--approve-sensitive") == 0) {
            approve_sensitive = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (strcmp(argv[i], "--approve-manifest") == 0) {
            approve_manifest = true;
        } else if (argv[i][0] == '-') {
            (void)fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            return EXIT_FAILURE;
        } else if (!path) {
            path = argv[i];
        } else {
            (void)fprintf(stderr, "error: only one path may be given\n");
            return EXIT_FAILURE;
        }
    }

    const char *cache_dir = hyp_workspace_cache_dir();
    if (!cache_dir || !cache_dir[0]) {
        (void)fprintf(stderr, "error: cache directory could not be resolved\n");
        return EXIT_FAILURE;
    }

    if (list_only || !path) {
        char listing[HYP_SZ_8K];
        if (hyp_workspace_grant_list(cache_dir, listing, sizeof(listing))) {
            printf("allowed roots:\n%s", listing);
        } else {
            printf("no allowed roots recorded — indexing is unconfined apart from the "
                   "always-refused roots (see docs/CONFIGURATION.md)\n");
        }
        if (!path && !list_only) {
            (void)fprintf(stderr, "usage: hyponoia allow-root [--approve-sensitive] <path>\n"
                                  "       hyponoia allow-root --approve-manifest <project>\n"
                                  "       hyponoia allow-root --list\n");
            return EXIT_FAILURE;
        }
        return 0;
    }

    if (approve_manifest) {
        /* Approve the manifest a project ships, keyed to its current content. The
         * file only ever requests; this is the human action that grants. */
        char canonical_project[HYP_PATH_MAX];
        if (!hyp_canonical_path(path, canonical_project, sizeof(canonical_project))) {
            (void)fprintf(stderr, "error: cannot resolve path: %s\n", path);
            return EXIT_FAILURE;
        }
        char merr[HYP_SZ_1K];
        if (!hyp_workspace_manifest_approve(cache_dir, hyp_workspace_home_dir(), canonical_project,
                                            merr, sizeof(merr))) {
            (void)fprintf(stderr, "refused: %s\n", merr[0] ? merr : "manifest not approvable");
            return EXIT_FAILURE;
        }
        printf("manifest approved for %s\n", canonical_project);
        printf("note: editing %s lapses this approval and it must be granted again.\n",
               HYP_WS_MANIFEST_NAME);
        return 0;
    }

    /* Canonicalize before recording: the policy is defined over resolved paths,
     * and a grant stored as a symlink would not match the resolved repo path the
     * indexer later presents. */
    char canonical[HYP_PATH_MAX];
    if (!hyp_canonical_path(path, canonical, sizeof(canonical))) {
        (void)fprintf(stderr, "error: cannot resolve path: %s\n", path);
        return EXIT_FAILURE;
    }
    if (!hyp_is_dir(canonical)) {
        (void)fprintf(stderr, "error: not a directory: %s\n", canonical);
        return EXIT_FAILURE;
    }

    char err[HYP_SZ_1K];
    if (!hyp_workspace_grant_add(cache_dir, hyp_workspace_home_dir(), canonical, approve_sensitive,
                                 err, sizeof(err))) {
        (void)fprintf(stderr, "refused: %s\n", err[0] ? err : "not an allowable root");
        return EXIT_FAILURE;
    }
    printf("allowed root recorded: %s\n", canonical);
    printf("note: with at least one root recorded, indexing is now confined to the "
           "recorded roots.\n");
    return 0;
}

static int handle_subcommand(int argc, char **argv, hyp_project_lock_manager_t *project_locks,
                             main_local_maintenance_context_t *maintenance_context) {
    /* First scan: global flags */
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0) {
            hyp_profile_enable();
        }
    }
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strcmp(argv[i], "--verify-runtime-assets") == 0) {
            if (i != SKIP_ONE || argc != MAIN_CLI_ARGC) {
                (void)fprintf(stderr, "hyponoia: --verify-runtime-assets accepts no arguments\n");
                return 2;
            }
            return hyp_cmd_verify_runtime_assets();
        }
        if (strcmp(argv[i], "--version") == 0) {
            main_print_version();
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "allow-root") == 0) {
            return main_run_allow_root(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "cli") == 0) {
            hyp_mem_init_with_cap(hyp_mem_ram_fraction_for_total(hyp_system_info().total_ram),
                                  hyp_index_worker_memory_budget_bytes());
            return run_cli(argc - i - SKIP_ONE, argv + i + SKIP_ONE, project_locks,
                           maintenance_context);
        }
        if (strcmp(argv[i], "hook-augment") == 0) {
            hyp_mem_init(hyp_mem_ram_fraction_for_total(hyp_system_info().total_ram));
            return hyp_cmd_hook_augment(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "install") == 0) {
            return hyp_cmd_install(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "uninstall") == 0) {
            return hyp_cmd_uninstall(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "update") == 0) {
            return hyp_cmd_update(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        if (strcmp(argv[i], "config") == 0) {
            return hyp_cmd_config(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
        /* The `ask` lane's opt-in second pass. Deliberately NOT a flag on
         * index_repository: keeping it a separate invocation is what makes
         * "the structural index is untouched" a property of the build rather
         * than a claim about a branch nobody took. */
        if (strcmp(argv[i], "embed") == 0) {
            hyp_mem_init(hyp_mem_ram_fraction_for_total(hyp_system_info().total_ram));
            return hyp_cmd_embed(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }

        /* The ONLY caller of the model fetcher, and it is a command a person
         * types. Nothing on the MCP or daemon path can reach it, which is what
         * keeps "no network request by default" a property of this binary
         * rather than a claim about it. See src/cli/model_fetch.h. */
        if (strcmp(argv[i], "fetch-model") == 0) {
            return hyp_cmd_fetch_model(argc - i - SKIP_ONE, argv + i + SKIP_ONE);
        }
    }
    return HYP_NOT_FOUND;
}

/* Parse --ui= and --port= into a per-field daemon mutation. */
static uint8_t parse_ui_flags(int argc, char **argv, bool *ui_enabled, int *ui_port,
                              bool *explicit_enable) {
    uint8_t update_mask = 0;
    for (int i = SKIP_ONE; i < argc; i++) {
        if (strncmp(argv[i], "--ui=", SLEN("--ui=")) == 0) {
            *ui_enabled = strcmp(argv[i] + MAIN_FLAG_OFF, "true") == 0;
            if (explicit_enable && *ui_enabled) {
                *explicit_enable = true;
            }
            update_mask |= HYP_DAEMON_APPLICATION_UI_CONFIG_ENABLED;
        }
        if (strncmp(argv[i], "--port=", SLEN("--port=")) == 0) {
            const char *value = argv[i] + MAIN_PORT_OFF;
            char *end = NULL;
            errno = 0;
            long port = strtol(value, &end, HYP_DECIMAL_BASE);
            if (errno == 0 && end != value && end && *end == '\0' && port > 0 &&
                port < MAIN_MAX_PORT) {
                *ui_port = (int)port;
                update_mask |= HYP_DAEMON_APPLICATION_UI_CONFIG_PORT;
            }
        }
    }
    return update_mask;
}

/* Install platform-specific signal handlers. */
static void setup_signal_handlers(void) {
#ifdef _WIN32
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
#else
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
#endif
}

#ifdef _WIN32
/* On Windows the CRT hands main() an argv encoded in the active ANSI code page, so a
 * non-ASCII CLI argument (e.g. a repo path like café_日本語_repo) is mangled before the
 * program ever sees it — the documented `cli index_repository "<json>"` then fails with
 * "repo_path is required" (#423/#20). Rebuild argv from the wide command line
 * (GetCommandLineW → CommandLineToArgvW) and convert each element to UTF-8 so the rest
 * of the program receives the same UTF-8 bytes it gets on POSIX. Returns a
 * NULL-terminated argv and sets *out_argc, or NULL on any failure (caller then keeps
 * the original narrow argv). The returned block lives for the whole process (argv must
 * stay valid until exit), so it is intentionally never freed. */
static char **hyp_win_utf8_argv(int *out_argc) {
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        return NULL;
    }
    if (wargc <= 0) {
        LocalFree(wargv);
        return NULL;
    }
    char **u8argv = (char **)calloc((size_t)wargc + 1, sizeof(char *));
    if (!u8argv) {
        LocalFree(wargv);
        return NULL;
    }
    for (int i = 0; i < wargc; i++) {
        u8argv[i] = hyp_wide_to_utf8(wargv[i]);
        if (!u8argv[i]) {
            for (int j = 0; j < i; j++) {
                free(u8argv[j]);
            }
            free(u8argv);
            LocalFree(wargv);
            return NULL;
        }
    }
    LocalFree(wargv);
    *out_argc = wargc;
    return u8argv; /* NULL-terminated (calloc'd wargc+1) */
}
#endif /* _WIN32 */

static bool main_resolve_executable(const char *argv0, char out[MAIN_PATH_CAP]) {
    char resolved[MAIN_PATH_CAP];
    return hyp_http_server_resolve_binary_path(argv0, resolved, sizeof(resolved)) &&
           hyp_canonical_path(resolved, out, MAIN_PATH_CAP);
}

typedef enum {
    MAIN_BUILD_IDENTITY_OK = 0,
    MAIN_BUILD_IDENTITY_INVALID_OUTPUT,
    MAIN_BUILD_IDENTITY_PROCESS_FINGERPRINT,
    MAIN_BUILD_IDENTITY_CACHE_RESOLVE,
    MAIN_BUILD_IDENTITY_CACHE_CANONICALIZE,
    MAIN_BUILD_IDENTITY_CACHE_PRIVATE,
    MAIN_BUILD_IDENTITY_CACHE_ENVIRONMENT,
} main_build_identity_status_t;

static const char *main_build_identity_status_name(main_build_identity_status_t status) {
    switch (status) {
    case MAIN_BUILD_IDENTITY_OK:
        return "ok";
    case MAIN_BUILD_IDENTITY_INVALID_OUTPUT:
        return "identity-output";
    case MAIN_BUILD_IDENTITY_PROCESS_FINGERPRINT:
        return "process-fingerprint";
    case MAIN_BUILD_IDENTITY_CACHE_RESOLVE:
        return "cache-resolve";
    case MAIN_BUILD_IDENTITY_CACHE_CANONICALIZE:
        return "cache-canonicalize";
    case MAIN_BUILD_IDENTITY_CACHE_PRIVATE:
        return "cache-private";
    case MAIN_BUILD_IDENTITY_CACHE_ENVIRONMENT:
        return "cache-environment";
    }
    return "identity-unknown";
}

static main_build_identity_status_t main_build_identity(hyp_daemon_build_identity_t *identity) {
    if (!identity) {
        return MAIN_BUILD_IDENTITY_INVALID_OUTPUT;
    }
    if (!hyp_index_supervisor_capture_build_fingerprint()) {
        return MAIN_BUILD_IDENTITY_PROCESS_FINGERPRINT;
    }
    const char *fingerprint = hyp_index_supervisor_build_fingerprint();
    if (!fingerprint) {
        return MAIN_BUILD_IDENTITY_PROCESS_FINGERPRINT;
    }
    const char *cache = hyp_resolve_cache_dir();
    char canonical_cache[MAIN_PATH_CAP];
    static char cache_fingerprint[HYP_SHA256_HEX_LEN + 1];
    if (!cache || !cache[0]) {
        return MAIN_BUILD_IDENTITY_CACHE_RESOLVE;
    }
    /* Preserve one intentional alias spelling at the process boundary: an
     * existing directory (including a symlink supplied by the user) is
     * resolved first. Only a genuinely absent root goes through mkdir_p's
     * component-by-component no-follow creation path. The process then uses
     * only the resulting canonical path, so retargeting the original alias
     * cannot move storage after cohort admission. */
    bool cache_ready = hyp_canonical_path(cache, canonical_cache, sizeof(canonical_cache));
    if (!cache_ready && hyp_mkdir_p(cache, 0700)) {
        cache_ready = hyp_canonical_path(cache, canonical_cache, sizeof(canonical_cache));
    }
    if (!cache_ready || !hyp_is_dir(canonical_cache)) {
        return MAIN_BUILD_IDENTITY_CACHE_CANONICALIZE;
    }
    hyp_normalize_path_sep(canonical_cache);
    /* Admission is account-scoped, so its storage authority must be too.
     * Harden the canonical object before hashing it. Replacement of this
     * owner-only path by the same already-compromised OS account is outside
     * the v1 threat boundary; cross-account and unsafe filesystem states fail
     * here before any daemon/cohort state is opened. */
    if (!hyp_daemon_ipc_private_directory_secure(canonical_cache)) {
        return MAIN_BUILD_IDENTITY_CACHE_PRIVATE;
    }
    /* Every cache consumer in this process must use the exact path whose
     * fingerprint joins the account-wide cohort. Keeping an original symlink
     * spelling in the environment would let a later retarget move storage
     * while the process still advertises the old canonical root. */
    if (hyp_setenv("HYP_CACHE_DIR", canonical_cache, 1) != 0) {
        return MAIN_BUILD_IDENTITY_CACHE_ENVIRONMENT;
    }
    hyp_sha256_hex(canonical_cache, strlen(canonical_cache), cache_fingerprint);
    *identity = (hyp_daemon_build_identity_t){
        .semantic_version = HYP_VERSION,
        .build_fingerprint = fingerprint,
        .cache_fingerprint = cache_fingerprint,
        .protocol_abi = HYP_DAEMON_RUNTIME_WIRE_ABI,
        .store_abi = 1,
        .feature_abi = 1,
    };
    return MAIN_BUILD_IDENTITY_OK;
}

static uint64_t main_deadline_after(uint32_t timeout_ms) {
    uint64_t now_ms = hyp_now_ms();
    return now_ms > UINT64_MAX - timeout_ms ? UINT64_MAX : now_ms + timeout_ms;
}

static hyp_daemon_ipc_endpoint_t *main_daemon_endpoint_new(void) {
    const char *runtime_parent = NULL;
#ifdef HYP_ENABLE_TEST_SEAMS
    /* Product daemon coordination is deliberately account-wide. Product-level
     * lifecycle guards need an isolated rendezvous namespace so they cannot
     * attach to or retire a developer's real daemon while exercising exact
     * start/open/stop behavior. The seam is opt-in at compile time and the
     * detached child inherits the same environment value. */
    char seam_runtime_parent[MAIN_PATH_CAP];
    runtime_parent = hyp_safe_getenv("HYP_TEST_DAEMON_RUNTIME_PARENT", seam_runtime_parent,
                                     sizeof(seam_runtime_parent), NULL);
#endif
    return hyp_daemon_bootstrap_endpoint_new(runtime_parent);
}

static bool main_local_cli_feedback_enabled(int argc, char **argv) {
    bool requested = false;
    for (int index = 1; index < argc; index++) {
        if (argv[index] && strcmp(argv[index], "--progress") == 0) {
            requested = true;
            break;
        }
    }
    return hyp_cli_progress_enabled(requested, cli_isatty(2) != 0);
}

static int main_local_transition_acquire(const hyp_daemon_ipc_endpoint_t *endpoint, FILE *feedback,
                                         hyp_daemon_ipc_local_transition_t **transition_out) {
    uint64_t deadline = main_deadline_after(MAIN_STARTUP_TIMEOUT_MS);
    bool waiting_reported = false;
    for (;;) {
        int status = hyp_daemon_ipc_local_transition_try_acquire(endpoint, transition_out);
        if (status != 0 || hyp_now_ms() >= deadline) {
            return status;
        }
        if (feedback && !waiting_reported) {
            (void)fputs("Waiting for HYP startup coordination...\n", feedback);
            (void)fflush(feedback);
            waiting_reported = true;
        }
        hyp_usleep(10000);
    }
}

static bool main_version_cohort_close(hyp_version_cohort_lease_t **lease,
                                      hyp_version_cohort_manager_t **manager) {
    bool ok = true;
    uint64_t deadline = main_deadline_after(MAIN_COORDINATION_CLEANUP_MS);
    while (lease && *lease) {
        hyp_private_file_lock_status_t status = hyp_version_cohort_lease_release(lease);
        if (status != HYP_PRIVATE_FILE_LOCK_OK) {
            ok = false;
        }
        if (*lease) {
            if (hyp_now_ms() >= deadline) {
                main_coordination_cleanup_fail_stop("cohort_lease_cleanup");
            }
            hyp_usleep(1000);
        }
    }
    deadline = main_deadline_after(MAIN_COORDINATION_CLEANUP_MS);
    while (manager && *manager) {
        hyp_private_file_lock_status_t status = hyp_version_cohort_manager_free(manager);
        if (status != HYP_PRIVATE_FILE_LOCK_OK) {
            ok = false;
        }
        if (*manager) {
            if (hyp_now_ms() >= deadline) {
                main_coordination_cleanup_fail_stop("cohort_manager_cleanup");
            }
            hyp_usleep(1000);
        }
    }
    return ok;
}

static bool main_project_lock_manager_close(hyp_project_lock_manager_t **manager) {
    bool ok = true;
    uint64_t deadline = main_deadline_after(MAIN_COORDINATION_CLEANUP_MS);
    while (manager && *manager) {
        hyp_private_file_lock_status_t status = hyp_project_lock_manager_free(manager);
        if (status != HYP_PRIVATE_FILE_LOCK_OK) {
            ok = false;
        }
        if (*manager) {
            if (hyp_now_ms() >= deadline) {
                main_coordination_cleanup_fail_stop("project_lock_manager_cleanup");
            }
            hyp_usleep(1000);
        }
    }
    return ok;
}

static bool main_local_transition_close(hyp_daemon_ipc_local_transition_t **transition) {
    uint64_t deadline = main_deadline_after(MAIN_COORDINATION_CLEANUP_MS);
    while (transition && *transition) {
        /* A failed release always RETAINS the transition and is retriable by
         * contract: the Windows release transition must briefly try-hold the
         * shared startup-v2/legacy gates, so concurrent one-shot teardowns
         * legitimately collide and succeed on retry. Success consumes the
         * transition; only never-finishing cleanup is a failure, and the
         * deadline escalation below owns that. */
        (void)hyp_daemon_ipc_local_transition_release(transition);
        if (*transition) {
            if (hyp_now_ms() >= deadline) {
                main_coordination_cleanup_fail_stop("local_transition_cleanup");
            }
            hyp_usleep(1000);
        }
    }
    return true;
}

static bool main_session_context(const char *preferred_root, char root_out[MAIN_PATH_CAP],
                                 char allowed_out[MAIN_PATH_CAP], const char **allowed_out_ptr) {
    const char *root = preferred_root && preferred_root[0] ? preferred_root : ".";
    if (!hyp_canonical_path(root, root_out, MAIN_PATH_CAP)) {
        return false;
    }
    char configured[MAIN_PATH_CAP];
    const char *allowed = hyp_safe_getenv("HYP_ALLOWED_ROOT", configured, sizeof(configured), NULL);
    if (allowed && allowed[0]) {
        if (!hyp_canonical_path(allowed, allowed_out, MAIN_PATH_CAP)) {
            return false;
        }
        *allowed_out_ptr = allowed_out;
    } else {
        allowed_out[0] = '\0';
        *allowed_out_ptr = NULL;
    }
    return true;
}

static bool main_set_client_context(hyp_daemon_runtime_client_t *client, const char *preferred_root,
                                    hyp_mcp_tool_profile_t tool_profile, const char *hook_event,
                                    const char *hook_dialect, uint32_t timeout_ms) {
    char root[MAIN_PATH_CAP];
    char allowed[MAIN_PATH_CAP];
    const char *allowed_ptr = NULL;
    if (!main_session_context(preferred_root, root, allowed, &allowed_ptr)) {
        return false;
    }
    return hyp_daemon_application_client_set_context(client, root, allowed_ptr, tool_profile,
                                                     hook_event, hook_dialect, timeout_ms) ==
           HYP_DAEMON_RUNTIME_APPLICATION_OK;
}

/* Parse a strict MAJOR.MINOR.PATCH triple; false for anything else (dev
 * builds and prereleases never participate in auto-drain decisions). */
static bool main_semver_triple(const char *text, long out[3]) {
    if (!text || !text[0]) {
        return false;
    }
    char *cursor = NULL;
    out[0] = strtol(text, &cursor, 10);
    if (!cursor || *cursor != '.') {
        return false;
    }
    out[1] = strtol(cursor + 1, &cursor, 10);
    if (!cursor || *cursor != '.') {
        return false;
    }
    out[2] = strtol(cursor + 1, &cursor, 10);
    return cursor && *cursor == '\0';
}

static bool main_semver_newer(const char *candidate, const char *active) {
    long candidate_triple[3];
    long active_triple[3];
    if (!main_semver_triple(candidate, candidate_triple) ||
        !main_semver_triple(active, active_triple)) {
        return false;
    }
    for (int part = 0; part < 3; part++) {
        if (candidate_triple[part] != active_triple[part]) {
            return candidate_triple[part] > active_triple[part];
        }
    }
    return false;
}

/* Client bootstrap with the upgrade policy: a CONFLICT against a PERMANENT
 * daemon of a strictly OLDER release is resolved by draining that daemon
 * (the same authenticated path install/update use) and retrying once. A
 * manual binary swap therefore self-heals exactly like the ephemeral
 * lifecycle used to, instead of deadlocking behind the pinned daemon. Same-
 * or newer-build daemons and dev builds are never auto-drained. */
static hyp_daemon_bootstrap_status_t main_client_bootstrap_with_upgrade(
    const hyp_daemon_bootstrap_config_t *config, hyp_daemon_bootstrap_result_t *result) {
    hyp_daemon_bootstrap_status_t status = hyp_daemon_bootstrap_execute(config, result);
    if (status != HYP_DAEMON_BOOTSTRAP_CONFLICT) {
        return status;
    }
    hyp_daemon_runtime_status_t active;
    if (!hyp_daemon_runtime_request_status(config->endpoint, config->identity,
                                           MAIN_CONNECT_TIMEOUT_MS, &active) ||
        !active.permanent ||
        !main_semver_newer(config->identity->semantic_version, active.semantic_version)) {
        return status;
    }
    (void)fprintf(stderr,
                  "hyponoia: retiring the active permanent daemon (%s, pid %lu) for "
                  "this newer build (%s)\n",
                  active.semantic_version, (unsigned long)active.daemon_pid,
                  config->identity->semantic_version);
    hyp_daemon_runtime_activation_result_t drain;
    if (!hyp_daemon_runtime_request_activation_shutdown(config->endpoint, config->identity,
                                                        HYP_DAEMON_RUNTIME_ACTIVATION_UPDATE,
                                                        MAIN_MCP_STARTUP_TIMEOUT_MS, &drain) ||
        !drain.accepted) {
        (void)fprintf(stderr, "hyponoia: the active daemon did not accept the "
                              "upgrade drain; run `hyponoia daemon stop`\n");
        return status;
    }
    return hyp_daemon_bootstrap_execute(config, result);
}

/* One-shot CLI commands execute through the shared daemon, exactly like MCP
 * sessions and hooks: an active daemon (any starter) is recycled, an absent
 * one is spawned for this command — with a hint that `daemon start` removes
 * that per-command cost. Only supervised index workers stay in-process. */
static char *main_local_cli_daemon_execute(const char *tool_name, const char *args_json) {
    hyp_daemon_ipc_endpoint_t *endpoint = hyp_daemon_bootstrap_endpoint_new(NULL);
    char executable_path[MAIN_PATH_CAP] = {0};
    hyp_daemon_build_identity_t identity;
    bool prepared =
        endpoint &&
        hyp_http_server_resolve_binary_path(NULL, executable_path, sizeof(executable_path)) &&
        main_build_identity(&identity) == MAIN_BUILD_IDENTITY_OK;
    if (!prepared) {
        (void)fprintf(stderr, "error: daemon-backed CLI coordination could not be prepared\n");
        hyp_daemon_ipc_endpoint_free(endpoint);
        return NULL;
    }
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = endpoint,
        .identity = &identity,
        .executable_path = executable_path,
        .connect_timeout_ms = MAIN_CONNECT_TIMEOUT_MS,
        .startup_timeout_ms = MAIN_MCP_STARTUP_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t bootstrap;
    hyp_daemon_bootstrap_status_t status = main_client_bootstrap_with_upgrade(&config, &bootstrap);
    hyp_daemon_ipc_endpoint_free(endpoint);
    if (status != HYP_DAEMON_BOOTSTRAP_CONNECTED || !bootstrap.client) {
        (void)fprintf(stderr, "error: %s\n",
                      bootstrap.message[0] ? bootstrap.message
                                           : "no HYP daemon connection for CLI execution");
        return NULL;
    }
    if (bootstrap.daemon_spawned) {
        (void)fprintf(stderr, "hint: this command started a temporary HYP daemon. "
                              "`hyponoia daemon start` keeps one warm and removes this "
                              "startup cost from every CLI command.\n");
    }
    char session_root[MAIN_PATH_CAP];
    char allowed_root[MAIN_PATH_CAP];
    const char *allowed_root_ptr = NULL;
    char *result = NULL;
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    bool context_ok =
        main_session_context(NULL, session_root, allowed_root, &allowed_root_ptr) &&
        main_set_client_context(bootstrap.client, session_root, HYP_MCP_TOOL_PROFILE_ALL, NULL,
                                NULL, MAIN_CONNECT_TIMEOUT_MS);
    if (context_ok &&
        hyp_daemon_application_client_tool(bootstrap.client, tool_name, args_json, &response,
                                           &response_length, MAIN_REQUEST_TIMEOUT_MS) ==
            HYP_DAEMON_RUNTIME_APPLICATION_OK &&
        response && response_length > 0) {
        result = malloc((size_t)response_length + 1U);
        if (result) {
            memcpy(result, response, response_length);
            result[response_length] = '\0';
        }
    }
    free(response);
    if (!result) {
        (void)fprintf(stderr, "error: daemon-backed CLI execution failed\n");
    }
    (void)hyp_daemon_runtime_client_close(bootstrap.client, MAIN_CLOSE_TIMEOUT_MS);
    return result;
}

static char *main_hook_cwd(const char *input_json) {
    if (!input_json) {
        return NULL;
    }
    yyjson_doc *document = yyjson_read(input_json, strlen(input_json), 0);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    yyjson_val *cwd_value = yyjson_is_obj(root) ? yyjson_obj_get(root, "cwd") : NULL;
    const char *cwd = yyjson_is_str(cwd_value) ? yyjson_get_str(cwd_value) : NULL;
    char *copy = NULL;
    if (cwd && hyp_hook_path_is_abs(cwd)) {
        size_t length = strlen(cwd);
        copy = malloc(length + 1U);
        if (copy) {
            memcpy(copy, cwd, length + 1U);
        }
    }
    if (document) {
        yyjson_doc_free(document);
    }
    return copy;
}

/* Hooks never spawn a daemon (a cold spawn livelocks against the fail-open
 * budget), so augmentation is absent until an MCP session or `daemon start`
 * brings one up. That state must be VISIBLE, not silent — but a notice per
 * tool call would nag, so a cache-scoped marker rate-limits it. */
static bool main_hook_absent_notice_due(void) {
    const char *cache_dir = hyp_resolve_cache_dir();
    if (!cache_dir) {
        return false;
    }
    char marker[MAIN_PATH_CAP];
    int written = snprintf(marker, sizeof(marker), "%s/.hook-daemon-absent-notice", cache_dir);
    if (written <= 0 || (size_t)written >= sizeof(marker)) {
        return false;
    }
    uint64_t now_seconds = hyp_now_ms() / 1000U;
    uint64_t stamp_seconds = 0;
    FILE *stamp = hyp_fopen(marker, "r");
    if (stamp) {
        char text[32] = {0};
        if (fgets(text, sizeof(text), stamp)) {
            stamp_seconds = strtoull(text, NULL, 10);
        }
        (void)fclose(stamp);
    }
    if (stamp_seconds != 0 && now_seconds >= stamp_seconds &&
        now_seconds - stamp_seconds < MAIN_HOOK_NOTICE_INTERVAL_SECONDS) {
        return false;
    }
    FILE *update = hyp_fopen(marker, "w");
    if (update) {
        (void)fprintf(update, "%llu\n", (unsigned long long)now_seconds);
        (void)fclose(update);
    }
    return true;
}

static void main_hook_report_absent_daemon(const char *hook_dialect) {
    if (!main_hook_absent_notice_due()) {
        return;
    }
    (void)fprintf(stderr, "hyponoia: no HYP daemon is running, so graph "
                          "augmentation is skipped. Start an MCP session or run "
                          "`hyponoia daemon start` to enable it.\n");
    const char *notice = hyp_hook_admission_notice(HYP_HOOK_ADMISSION_DAEMON_ABSENT, hook_dialect);
    if (notice) {
        (void)fputs(notice, stdout);
        (void)fflush(stdout);
    }
}

/* #1388: a version-conflicted daemon is an actionable broken state, unlike a
 * merely absent one - and stdout is the only hook channel Claude Code
 * surfaces, so stderr-only reporting reads as eternal silence in-session.
 * Emit a throttled systemMessage with restart guidance (the misleading
 * "no daemon is running" text pointed at `daemon start`, which cannot heal a
 * build conflict). */
static void main_hook_report_conflicted_daemon(const char *hook_dialect) {
    if (hook_dialect || !main_hook_absent_notice_due()) {
        return;
    }
    const char *notice = hyp_hook_admission_notice(HYP_HOOK_ADMISSION_BUILD_CONFLICT, hook_dialect);
    if (notice) {
        (void)fputs(notice, stdout);
        (void)fflush(stdout);
    }
}

static int main_run_hook_frontend(hyp_daemon_runtime_client_t *client, const char *hook_event,
                                  const char *hook_dialect) {
    char *input = hyp_hook_augment_read_stdin();
    if (!input) {
        return 0;
    }
    char *hook_cwd = main_hook_cwd(input);
    bool context_set =
        main_set_client_context(client, hook_cwd, HYP_MCP_TOOL_PROFILE_ALL, hook_event,
                                hook_dialect, MAIN_HOOK_CONNECT_TIMEOUT_MS);
    free(hook_cwd);
    if (!context_set) {
        free(input);
        return 0;
    }
    uint8_t *response = NULL;
    uint32_t response_length = 0;
    hyp_daemon_runtime_application_status_t status = hyp_daemon_application_client_hook_augment(
        client, input, &response, &response_length, MAIN_HOOK_REQUEST_TIMEOUT_MS);
    free(input);
    if (status == HYP_DAEMON_RUNTIME_APPLICATION_OK && response && response_length > 0) {
        (void)fwrite(response, 1, response_length, stdout);
        (void)fflush(stdout);
    }
    free(response);
    return 0; /* hooks always fail open */
}

static bool main_hook_options(int argc, char **argv, const char **event_out,
                              const char **dialect_out) {
    if (!argv || !event_out || !dialect_out) {
        return false;
    }
    *event_out = NULL;
    *dialect_out = NULL;
    int hook_index = -1;
    for (int index = 1; index < argc; index++) {
        if (argv[index] && strcmp(argv[index], "hook-augment") == 0) {
            hook_index = index;
            break;
        }
    }
    if (hook_index < 0) {
        return false;
    }
    for (int index = hook_index + 1; index < argc; index++) {
        if (strcmp(argv[index], "--event") == 0 && index + 1 < argc) {
            *event_out = argv[++index];
        } else if (strcmp(argv[index], "--dialect") == 0 && index + 1 < argc) {
            *dialect_out = argv[++index];
        } else {
            return false;
        }
    }
    return hyp_hook_augment_invocation_supported(*event_out, *dialect_out);
}

enum {
    MAIN_DAEMON_CTL_PROBE_TIMEOUT_MS = 3000,
    MAIN_DAEMON_CTL_STOP_TIMEOUT_MS = 10000,
    MAIN_DAEMON_CTL_START_TIMEOUT_MS = 30000,
    MAIN_DAEMON_CTL_UI_READY_TIMEOUT_MS = 30000,
    MAIN_DAEMON_CTL_UI_PROBE_TIMEOUT_MS = 250,
    MAIN_DAEMON_CTL_UI_PROBE_INTERVAL_US = 50000,
    MAIN_DAEMON_CTL_UI_RESPONSE_CAP = 4096,
};

static void main_daemon_ctl_print_clients(const uint32_t *pids, uint8_t count, uint16_t committed) {
    printf("  committed clients: %u\n", (unsigned)committed);
    for (uint8_t index = 0; index < count; index++) {
        printf("    - pid %lu\n", (unsigned long)pids[index]);
    }
    if (committed > count) {
        printf("    - (%u more not listed)\n", (unsigned)(committed - count));
    }
}

#ifdef _WIN32
typedef SOCKET main_daemon_ctl_socket_t;
#define MAIN_DAEMON_CTL_BAD_SOCKET INVALID_SOCKET
#define main_daemon_ctl_socket_close closesocket
#else
typedef int main_daemon_ctl_socket_t;
#define MAIN_DAEMON_CTL_BAD_SOCKET (-1)
#define main_daemon_ctl_socket_close close
#endif

#ifdef _WIN32
static bool main_daemon_ctl_socket_runtime_ready(void) {
    /* main is single-threaded along this path. As in the HTTP listener, keep
     * Winsock initialized for the short remaining lifetime of this process. */
    static bool attempted = false;
    static bool ready = false;
    if (!attempted) {
        attempted = true;
        WSADATA winsock;
        ready = WSAStartup(MAKEWORD(2, 2), &winsock) == 0;
    }
    return ready;
}
#endif

static bool main_daemon_ctl_socket_set_nonblocking(main_daemon_ctl_socket_t socket_handle) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket_handle, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl(socket_handle, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_handle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static bool main_daemon_ctl_socket_would_block(void) {
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY ||
           error == WSAEINTR;
#else
    return errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/* 1 = requested readiness, 0 = timeout/interruption, -1 = socket error. */
static int main_daemon_ctl_socket_wait(main_daemon_ctl_socket_t socket_handle, bool writing,
                                       int timeout_ms) {
#ifdef _WIN32
    fd_set ready;
    fd_set errors;
    FD_ZERO(&ready);
    FD_ZERO(&errors);
    FD_SET(socket_handle, &ready);
    FD_SET(socket_handle, &errors);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int result = select(0, writing ? NULL : &ready, writing ? &ready : NULL, &errors, &timeout);
    if (result <= 0) {
        return result < 0 ? -1 : 0;
    }
    return FD_ISSET(socket_handle, &errors) ? -1 : 1;
#else
    struct pollfd descriptor = {
        .fd = socket_handle,
        .events = writing ? POLLOUT : POLLIN,
        .revents = 0,
    };
    int result = poll(&descriptor, 1, timeout_ms);
    if (result < 0) {
        return errno == EINTR ? 0 : -1;
    }
    if (result == 0) {
        return 0;
    }
    return (descriptor.revents & descriptor.events) != 0 ? 1 : -1;
#endif
}

static int main_daemon_ctl_socket_remaining_ms(uint64_t deadline_ms) {
    uint64_t now_ms = hyp_now_ms();
    if (now_ms >= deadline_ms) {
        return 0;
    }
    uint64_t remaining = deadline_ms - now_ms;
    return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

static bool main_daemon_ctl_socket_connected(main_daemon_ctl_socket_t socket_handle, int port,
                                             uint64_t deadline_ms) {
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(0x7F000001U);
    if (connect(socket_handle, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        return true;
    }
    if (!main_daemon_ctl_socket_would_block()) {
        return false;
    }
    int remaining = main_daemon_ctl_socket_remaining_ms(deadline_ms);
    if (remaining <= 0 || main_daemon_ctl_socket_wait(socket_handle, true, remaining) != 1) {
        return false;
    }
    int socket_error = 0;
#ifdef _WIN32
    int socket_error_size = (int)sizeof(socket_error);
    return getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, (char *)&socket_error,
                      &socket_error_size) == 0 &&
           socket_error == 0;
#else
    socklen_t socket_error_size = sizeof(socket_error);
    return getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) ==
               0 &&
           socket_error == 0;
#endif
}

static int main_daemon_ctl_socket_send(main_daemon_ctl_socket_t socket_handle, const char *data,
                                       size_t length) {
#ifdef _WIN32
    return send(socket_handle, data, (int)length, 0);
#else
#ifdef MSG_NOSIGNAL
    return (int)send(socket_handle, data, length, MSG_NOSIGNAL);
#else
    return (int)send(socket_handle, data, length, 0);
#endif
#endif
}

static int main_daemon_ctl_socket_receive(main_daemon_ctl_socket_t socket_handle, char *data,
                                          size_t capacity) {
#ifdef _WIN32
    return recv(socket_handle, data, (int)capacity, 0);
#else
    return (int)recv(socket_handle, data, capacity, 0);
#endif
}

typedef struct {
    char challenge_hex[HYP_SHA256_HEX_LEN + 1U];
    char proof_hex[HYP_SHA256_HEX_LEN + 1U];
} main_daemon_ctl_ui_readiness_t;

static void main_daemon_ctl_hex_encode(const uint8_t bytes[HYP_SHA256_DIGEST_LEN],
                                       char out[HYP_SHA256_HEX_LEN + 1U]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < HYP_SHA256_DIGEST_LEN; i++) {
        out[i * 2U] = hex[bytes[i] >> 4];
        out[i * 2U + 1U] = hex[bytes[i] & 0x0fU];
    }
    out[HYP_SHA256_HEX_LEN] = '\0';
}

static bool main_daemon_ctl_ui_readiness_prepare(hyp_daemon_runtime_client_t *client,
                                                 main_daemon_ctl_ui_readiness_t *readiness) {
    uint8_t challenge[HYP_SHA256_DIGEST_LEN] = {0};
    uint8_t proof[HYP_SHA256_DIGEST_LEN] = {0};
    if (!client || !readiness) {
        return false;
    }
    hyp_secure_zero(readiness, sizeof(*readiness));
    bool prepared = hyp_secure_random(challenge, sizeof(challenge)) &&
                    hyp_daemon_application_client_ui_readiness_proof(client, challenge, proof,
                                                                     MAIN_CONNECT_TIMEOUT_MS) ==
                        HYP_DAEMON_RUNTIME_APPLICATION_OK;
    if (prepared) {
        main_daemon_ctl_hex_encode(challenge, readiness->challenge_hex);
        main_daemon_ctl_hex_encode(proof, readiness->proof_hex);
    }
    hyp_secure_zero(challenge, sizeof(challenge));
    hyp_secure_zero(proof, sizeof(proof));
    return prepared;
}

static bool main_daemon_ctl_constant_time_equal(const char *left, const char *right,
                                                size_t length) {
    unsigned char difference = 0;
    for (size_t index = 0; index < length; index++) {
        difference |= (unsigned char)left[index] ^ (unsigned char)right[index];
    }
    return difference == 0;
}

/* Bind HTTP readiness to the exact daemon generation already authenticated by
 * local IPC. The public challenge is safe to expose; a foreign listener cannot
 * compute the expected HMAC because the generation secret never leaves the
 * daemon's application and HTTP server instances. */
static bool main_daemon_ctl_ui_endpoint_ready(int port,
                                              const main_daemon_ctl_ui_readiness_t *readiness,
                                              uint32_t timeout_ms) {
    if (port <= 0 || port >= MAIN_MAX_PORT || !readiness || timeout_ms == 0) {
        return false;
    }
#ifdef _WIN32
    if (!main_daemon_ctl_socket_runtime_ready()) {
        return false;
    }
#endif
    main_daemon_ctl_socket_t socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle == MAIN_DAEMON_CTL_BAD_SOCKET ||
        !main_daemon_ctl_socket_set_nonblocking(socket_handle)) {
        if (socket_handle != MAIN_DAEMON_CTL_BAD_SOCKET) {
            (void)main_daemon_ctl_socket_close(socket_handle);
        }
        return false;
    }
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
    int no_sigpipe = 1;
    (void)setsockopt(socket_handle, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif

    uint64_t deadline_ms = main_deadline_after(timeout_ms);
    bool ready = main_daemon_ctl_socket_connected(socket_handle, port, deadline_ms);
    char request[384];
    int request_length = snprintf(request, sizeof(request),
                                  "GET /__hyp/ui-readiness?challenge=%s HTTP/1.1\r\n"
                                  "Host: 127.0.0.1:%d\r\nAccept: text/plain\r\n"
                                  "Connection: close\r\n\r\n",
                                  readiness->challenge_hex, port);
    if (request_length <= 0 || request_length >= (int)sizeof(request)) {
        ready = false;
    }
    size_t sent = 0;
    while (ready && sent < (size_t)request_length) {
        int remaining = main_daemon_ctl_socket_remaining_ms(deadline_ms);
        if (remaining <= 0 || main_daemon_ctl_socket_wait(socket_handle, true, remaining) != 1) {
            ready = false;
            break;
        }
        int count = main_daemon_ctl_socket_send(socket_handle, request + sent,
                                                (size_t)request_length - sent);
        if (count > 0) {
            sent += (size_t)count;
        } else if (count == 0 || !main_daemon_ctl_socket_would_block()) {
            ready = false;
        }
    }

    char response[MAIN_DAEMON_CTL_UI_RESPONSE_CAP] = {0};
    size_t received = 0;
    while (ready && received + 1U < sizeof(response)) {
        int remaining = main_daemon_ctl_socket_remaining_ms(deadline_ms);
        if (remaining <= 0 || main_daemon_ctl_socket_wait(socket_handle, false, remaining) != 1) {
            ready = false;
            break;
        }
        int count = main_daemon_ctl_socket_receive(socket_handle, response + received,
                                                   sizeof(response) - received - 1U);
        if (count > 0) {
            received += (size_t)count;
            response[received] = '\0';
        } else if (count == 0 || !main_daemon_ctl_socket_would_block()) {
            break;
        }
    }
    if (received + 1U >= sizeof(response)) {
        response[sizeof(response) - 1U] = '\0';
    }
    char *body = strstr(response, "\r\n\r\n");
    body = body ? body + 4 : NULL;
    ready = ready && received > 0U && strncmp(response, "HTTP/1.1 200 ", 13) == 0 &&
            strstr(response, "\r\nContent-Type: text/plain; charset=utf-8\r\n") != NULL &&
            strstr(response, "\r\nCache-Control: no-store\r\n") != NULL && body &&
            strlen(body) == HYP_SHA256_HEX_LEN &&
            main_daemon_ctl_constant_time_equal(body, readiness->proof_hex, HYP_SHA256_HEX_LEN);
    (void)main_daemon_ctl_socket_close(socket_handle);
    return ready;
}

static uint32_t main_daemon_ctl_ui_ready_timeout_ms(void) {
#ifdef HYP_ENABLE_TEST_SEAMS
    char timeout_text[32];
    const char *configured = hyp_safe_getenv("HYP_TEST_DAEMON_UI_READY_TIMEOUT_MS", timeout_text,
                                             sizeof(timeout_text), NULL);
    if (configured) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(configured, &end, 10);
        if (errno == 0 && end && *end == '\0' && parsed >= 50UL &&
            parsed <= MAIN_DAEMON_CTL_UI_READY_TIMEOUT_MS) {
            return (uint32_t)parsed;
        }
    }
#endif
    return MAIN_DAEMON_CTL_UI_READY_TIMEOUT_MS;
}

static bool main_daemon_ctl_wait_for_ui(int port, const main_daemon_ctl_ui_readiness_t *readiness,
                                        uint32_t timeout_ms) {
    uint64_t deadline_ms = main_deadline_after(timeout_ms);
    for (;;) {
        uint64_t now_ms = hyp_now_ms();
        if (now_ms >= deadline_ms) {
            return false;
        }
        uint64_t remaining = deadline_ms - now_ms;
        uint32_t attempt_ms = remaining > MAIN_DAEMON_CTL_UI_PROBE_TIMEOUT_MS
                                  ? MAIN_DAEMON_CTL_UI_PROBE_TIMEOUT_MS
                                  : (uint32_t)remaining;
        if (main_daemon_ctl_ui_endpoint_ready(port, readiness, attempt_ms)) {
            return true;
        }
        if (hyp_now_ms() >= deadline_ms) {
            return false;
        }
        hyp_usleep(MAIN_DAEMON_CTL_UI_PROBE_INTERVAL_US);
    }
}

static int main_daemon_ctl_finish_ui_open(hyp_daemon_runtime_client_t **client_io, int port,
                                          bool open_browser);

static void main_daemon_ctl_print_ui_configuration(void) {
    if (!hyp_ui_assets_supported()) {
        return;
    }
    hyp_ui_config_t ui_config;
    hyp_ui_config_load(&ui_config);
    if (ui_config.ui_enabled) {
        printf("  ui: configured at http://127.0.0.1:%d (readiness not checked; "
               "`daemon start --open` verifies it)\n",
               ui_config.ui_port);
    } else {
        printf("  ui: disabled (enable with `daemon start` in a UI build)\n");
    }
}

/* THE SPENDING SURFACE, WHERE SOMEONE WOULD GO LOOKING FOR IT
 * (NEXT-STEPS §3.2 step 5). A daemon reads the escalation key out of the
 * environment it was started with, so it can spend on behalf of clients that
 * never held the key. Whether it MAY is `ask.escalation.daemon_key`, and that
 * lives in the config database — machine-global, readable from here — so this
 * line needs nothing from the daemon's own status wire, which stays the
 * bounded 121-byte control probe it was designed to be.
 *
 * Printed only when a key variable is configured at all. Escalation is opt-in
 * and most installs never touch it; a permanent line about a key that does not
 * exist is noise, and noise in a status block is how the interesting line gets
 * skipped. */
static void main_daemon_ctl_print_escalation_key_custody(void) {
    const char *cache_dir = hyp_resolve_cache_dir();
    hyp_config_t *cfg = cache_dir ? hyp_config_open(cache_dir) : NULL;
    if (!cfg) {
        return;
    }
    char key_env[128];
    char policy[HYP_SZ_32];
    (void)snprintf(key_env, sizeof(key_env), "%s",
                   hyp_config_get(cfg, HYP_CONFIG_ASK_ESC_KEY_ENV, ""));
    (void)snprintf(
        policy, sizeof(policy), "%s",
        hyp_config_get(cfg, HYP_CONFIG_ASK_ESC_DAEMON_KEY, HYP_CONFIG_ASK_ESC_DAEMON_KEY_DEFAULT));
    hyp_config_close(cfg);
    if (!key_env[0]) {
        return;
    }
    if (strcmp(policy, HYP_CONFIG_ASK_ESC_DAEMON_KEY_ALLOW) == 0) {
        printf("  escalation key: %s=%s — this daemon reads $%s from ITS OWN environment when a "
               "client asks an escalated question, so any local process of this user account "
               "that can reach it can spend against that account without holding the key. Stop "
               "it with `hyponoia config set %s %s`.\n",
               HYP_CONFIG_ASK_ESC_DAEMON_KEY, HYP_CONFIG_ASK_ESC_DAEMON_KEY_ALLOW, key_env,
               HYP_CONFIG_ASK_ESC_DAEMON_KEY, HYP_CONFIG_ASK_ESC_DAEMON_KEY_REFUSE);
    } else {
        printf("  escalation key: %s=%s — this daemon will not read $%s on a client's behalf; "
               "ask(escalate=true) through it is refused. `hyponoia embed --escalation` and any "
               "process holding the key itself are unaffected.\n",
               HYP_CONFIG_ASK_ESC_DAEMON_KEY, HYP_CONFIG_ASK_ESC_DAEMON_KEY_REFUSE, key_env);
    }
}

static void main_daemon_ctl_open_browser(int port) {
    char url[64];
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
#ifdef HYP_ENABLE_TEST_SEAMS
    char marker_path[MAIN_PATH_CAP];
    if (hyp_safe_getenv("HYP_TEST_DAEMON_OPEN_MARKER", marker_path, sizeof(marker_path), NULL)) {
        FILE *marker = hyp_fopen(marker_path, "wb");
        bool opened = marker && fwrite(url, 1, strlen(url), marker) == strlen(url);
        if (marker && fclose(marker) != 0) {
            opened = false;
        }
        if (!opened) {
            (void)fprintf(stderr, "hint: could not record the test browser request\n");
        }
        return;
    }
#endif
#if defined(_WIN32)
    /* ShellExecuteW resolves the http protocol association directly — no
     * command shell interprets the argument. Values > 32 signal success. */
    wchar_t *wide_url = hyp_utf8_to_wide(url);
    bool opened =
        wide_url && (INT_PTR)ShellExecuteW(NULL, L"open", wide_url, NULL, NULL, SW_SHOWNORMAL) > 32;
    free(wide_url);
#elif defined(__APPLE__)
    const char *open_argv[] = {"open", url, NULL};
    bool opened = hyp_exec_no_shell(open_argv) == 0;
#else
    const char *open_argv[] = {"xdg-open", url, NULL};
    bool opened = hyp_exec_no_shell(open_argv) == 0;
#endif
    if (!opened) {
        (void)fprintf(stderr, "hint: could not open a browser automatically; visit %s\n", url);
    }
}

static int main_daemon_ctl_finish_ui_open(hyp_daemon_runtime_client_t **client_io, int port,
                                          bool open_browser) {
    if (!open_browser) {
        printf("  ui: warming asynchronously on configured port %d\n", port);
        return EXIT_SUCCESS;
    }
    main_daemon_ctl_ui_readiness_t readiness = {0};
    hyp_daemon_runtime_client_t *client = client_io ? *client_io : NULL;
    if (!main_daemon_ctl_ui_readiness_prepare(client, &readiness)) {
        (void)fprintf(stderr,
                      "error: the active daemon generation could not provide an authenticated "
                      "UI readiness proof; browser was not opened\n");
        return EXIT_FAILURE;
    }
    /* The generation-bound proof is self-contained.  Do not retain the
     * application client while polling HTTP: the daemon deliberately serves
     * one application connection at a time, so an idle retained client would
     * make `daemon status` and other control probes wait for the whole UI
     * deadline.  EOF/disconnect cannot weaken the HMAC proof we already hold. */
    if (client_io && *client_io) {
        hyp_daemon_runtime_client_t *owned_client = *client_io;
        *client_io = NULL;
        (void)hyp_daemon_runtime_client_close(owned_client, MAIN_CLOSE_TIMEOUT_MS);
    }
    uint32_t timeout_ms = main_daemon_ctl_ui_ready_timeout_ms();
    bool ready = main_daemon_ctl_wait_for_ui(port, &readiness, timeout_ms);
    hyp_secure_zero(&readiness, sizeof(readiness));
    if (!ready) {
        (void)fprintf(stderr,
                      "error: UI endpoint did not become ready within %u ms; browser was not "
                      "opened\n",
                      timeout_ms);
        (void)fprintf(stderr,
                      "hint: verify matching runtime assets with `hyponoia "
                      "--verify-runtime-assets`, check the daemon log, and if port %d is in use "
                      "retry with --port=N\n",
                      port);
        return EXIT_FAILURE;
    }
    printf("  ui: http://127.0.0.1:%d\n", port);
    main_daemon_ctl_open_browser(port);
    return EXIT_SUCCESS;
}

static int main_run_daemon_ctl(int argc, char **argv, const hyp_daemon_ipc_endpoint_t *endpoint,
                               const hyp_daemon_build_identity_t *identity,
                               const char *executable_path) {
    const char *subcommand = NULL;
    bool open_browser = false;
    int requested_port = 0;
    bool arguments_valid = true;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "daemon") == 0) {
            continue;
        }
        if (strcmp(argv[index], "start") == 0 || strcmp(argv[index], "stop") == 0 ||
            strcmp(argv[index], "status") == 0) {
            subcommand = argv[index];
        } else if (strcmp(argv[index], "--open") == 0) {
            open_browser = true;
        } else if (strncmp(argv[index], "--port=", 7) == 0) {
            requested_port = atoi(argv[index] + 7);
            if (requested_port <= 0 || requested_port >= MAIN_MAX_PORT) {
                (void)fprintf(stderr, "error: --port requires a value between 1 and 65535\n");
                return EXIT_FAILURE;
            }
        } else {
            (void)fprintf(stderr, "error: unknown daemon option: %s\n", argv[index]);
            arguments_valid = false;
            break;
        }
    }
    if (!arguments_valid || !subcommand) {
        (void)fprintf(stderr, "usage: hyponoia daemon <start|stop|status> "
                              "[--open] [--port=N]\n");
        return EXIT_FAILURE;
    }

    hyp_daemon_runtime_status_t status;
    bool active = hyp_daemon_runtime_request_status(endpoint, identity,
                                                    MAIN_DAEMON_CTL_PROBE_TIMEOUT_MS, &status);

    if (strcmp(subcommand, "status") == 0) {
        if (!active) {
            printf("daemon: not running\n");
            printf("hint: `hyponoia daemon start` keeps a daemon warm so CLI "
                   "commands and hooks skip the per-command startup cost.\n");
            return EXIT_FAILURE;
        }
        printf("daemon: active (%s)\n", status.permanent ? "permanent" : "session-managed");
        printf("  pid: %lu\n", (unsigned long)status.daemon_pid);
        printf("  build: %s (%.12s...)\n", status.semantic_version, status.build_fingerprint);
        if (status.stopping) {
            printf("  state: stopping\n");
        }
        main_daemon_ctl_print_clients(status.client_pids, status.client_count,
                                      status.committed_clients);
        main_daemon_ctl_print_ui_configuration();
        main_daemon_ctl_print_escalation_key_custody();
        return EXIT_SUCCESS;
    }

    if (strcmp(subcommand, "stop") == 0) {
        if (!active) {
            printf("daemon: not running (nothing to stop)\n");
            return EXIT_SUCCESS;
        }
        hyp_daemon_runtime_stop_result_t stop_result;
        if (!hyp_daemon_runtime_request_stop(endpoint, identity, MAIN_DAEMON_CTL_STOP_TIMEOUT_MS,
                                             &stop_result)) {
            (void)fprintf(stderr,
                          "error: the active daemon did not answer the stop request; "
                          "if it is stuck, terminate pid %lu directly\n",
                          (unsigned long)status.daemon_pid);
            return EXIT_FAILURE;
        }
        if (stop_result.busy) {
            printf("daemon: NOT stopped — %u committed client(s) still use it.\n",
                   (unsigned)stop_result.committed_clients);
            main_daemon_ctl_print_clients(stop_result.client_pids, stop_result.client_count,
                                          stop_result.committed_clients);
            printf("Close these sessions/commands first, then retry `daemon stop`.\n");
            return EXIT_FAILURE;
        }
        if (!stop_result.accepted) {
            printf("daemon: already stopping\n");
            return EXIT_SUCCESS;
        }
        printf("daemon: stopping (pid %lu)\n", (unsigned long)status.daemon_pid);
        return EXIT_SUCCESS;
    }

    /* start */
    if (active) {
        if (status.permanent) {
            printf("daemon: already active (permanent, pid %lu)\n",
                   (unsigned long)status.daemon_pid);
        } else {
            printf("daemon: already active (session-managed, pid %lu) — it stops with its "
                   "last session; run `daemon stop` first if you want a permanent one\n",
                   (unsigned long)status.daemon_pid);
        }
        if (!hyp_ui_assets_supported()) {
            if (requested_port > 0 || open_browser) {
                (void)fprintf(stderr, "warning: this binary was built without UI support; "
                                      "--port/--open have no effect\n");
            }
            return EXIT_SUCCESS;
        }
        hyp_ui_config_t ui_config;
        hyp_ui_config_load(&ui_config);
        if (!ui_config.ui_enabled) {
            (void)fprintf(stderr, "error: UI is disabled for the active daemon; browser was not "
                                  "opened\n");
            return open_browser ? EXIT_FAILURE : EXIT_SUCCESS;
        }
        if (requested_port > 0 && requested_port != ui_config.ui_port) {
            (void)fprintf(stderr,
                          "warning: the active daemon remains configured on port %d; stop it "
                          "before starting on --port=%d\n",
                          ui_config.ui_port, requested_port);
        }
        hyp_daemon_runtime_client_t *ui_client = NULL;
        hyp_daemon_runtime_connect_result_t connect_result;
        if (open_browser) {
            ui_client = hyp_daemon_runtime_client_connect(endpoint, identity,
                                                          MAIN_CONNECT_TIMEOUT_MS, &connect_result);
            if (!ui_client) {
                (void)fprintf(stderr,
                              "error: the active daemon generation could not be authenticated; "
                              "browser was not opened\n");
                return EXIT_FAILURE;
            }
        }
        int ui_result = main_daemon_ctl_finish_ui_open(&ui_client, ui_config.ui_port, open_browser);
        if (ui_client) {
            (void)hyp_daemon_runtime_client_close(ui_client, MAIN_CLOSE_TIMEOUT_MS);
        }
        return ui_result;
    }

    hyp_daemon_bootstrap_config_t start_config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = endpoint,
        .identity = identity,
        .executable_path = executable_path,
        .connect_timeout_ms = MAIN_CONNECT_TIMEOUT_MS,
        .startup_timeout_ms = MAIN_DAEMON_CTL_START_TIMEOUT_MS,
        .spawn_permanent = true,
    };
    hyp_daemon_bootstrap_result_t start_result;
    hyp_daemon_bootstrap_status_t start_status =
        main_client_bootstrap_with_upgrade(&start_config, &start_result);
    if (start_status != HYP_DAEMON_BOOTSTRAP_CONNECTED || !start_result.client) {
        (void)fprintf(stderr, "error: %s\n",
                      start_result.message[0] ? start_result.message
                                              : "the permanent daemon could not be started");
        if (start_status == HYP_DAEMON_BOOTSTRAP_CONFLICT) {
            (void)fprintf(stderr, "hint: a daemon of a different build is active; "
                                  "`hyponoia daemon stop` retires it.\n");
        }
        return EXIT_FAILURE;
    }

    /* The committed control connection satisfied the daemon's no-client
     * startup window; configure the UI before departing. */
    int ui_port = 0;
    if (hyp_ui_assets_supported()) {
        hyp_ui_config_t ui_config;
        hyp_ui_config_load(&ui_config);
        ui_port = requested_port > 0 ? requested_port : ui_config.ui_port;
        uint8_t update_mask = 0x03U; /* enabled + port */
        bool context_set =
            main_set_client_context(start_result.client, ".", HYP_MCP_TOOL_PROFILE_ALL, NULL, NULL,
                                    MAIN_CONNECT_TIMEOUT_MS);
        if (!context_set || hyp_daemon_application_client_set_ui_config(
                                start_result.client, update_mask, true, ui_port,
                                MAIN_CONNECT_TIMEOUT_MS) != HYP_DAEMON_RUNTIME_APPLICATION_OK) {
            (void)fprintf(stderr,
                          "error: the daemon did not accept the UI configuration; browser was "
                          "not opened\n");
            (void)hyp_daemon_runtime_client_close(start_result.client, MAIN_CLOSE_TIMEOUT_MS);
            return EXIT_FAILURE;
        }
    } else if (requested_port > 0 || open_browser) {
        (void)fprintf(stderr, "warning: this binary was built without UI support; "
                              "--port/--open have no effect\n");
    }

    hyp_daemon_runtime_status_t started;
    bool started_ok = hyp_daemon_runtime_request_status(endpoint, identity,
                                                        MAIN_DAEMON_CTL_PROBE_TIMEOUT_MS, &started);
    if (started_ok) {
        printf("daemon: started (permanent, pid %lu)\n", (unsigned long)started.daemon_pid);
    } else {
        printf("daemon: started (permanent)\n");
    }
    printf("It survives idle periods and session ends; `hyponoia daemon stop` "
           "retires it.\n");
    int ui_result =
        hyp_ui_assets_supported()
            ? main_daemon_ctl_finish_ui_open(&start_result.client, ui_port, open_browser)
            : EXIT_SUCCESS;
    if (start_result.client) {
        (void)hyp_daemon_runtime_client_close(start_result.client, MAIN_CLOSE_TIMEOUT_MS);
    }
    return ui_result;
}

int main(int argc, char **argv) {
    /* Must remain the first statement: see allocator binding contract above. */
    hyp_alloc_init();
#ifndef _WIN32
    pid_t process_initial_ppid = getppid();
#endif
#ifdef _WIN32
    {
        int win_argc = 0;
        char **win_argv = hyp_win_utf8_argv(&win_argc);
        if (win_argv) {
            argc = win_argc;
            argv = win_argv;
        }
    }
#endif
    hyp_daemon_process_role_t role = hyp_daemon_process_role(argc, argv);
    if (role == HYP_DAEMON_PROCESS_INVALID) {
        (void)fprintf(stderr, "hyponoia: invalid internal process arguments\n");
        return EXIT_FAILURE;
    }
#ifndef _WIN32
    if (role == HYP_DAEMON_PROCESS_DAEMON) {
        (void)umask(077);
    }
#endif

    hyp_cli_set_version(HYP_VERSION);
    hyp_profile_init();
    hyp_log_init_from_env();

    /* Register the `ask` lane's query encoder. This is a VTABLE INSTALL, not a
     * model load: the 639 MB of weights are opened on the first encode_query
     * and never by a process that does not ask one. Installing unconditionally
     * is what makes hyp_ask_backend() non-NULL, which is what moves the lane's
     * unavailable cause off `no_encoder` — a claim about the BUILD — and onto
     * the causes that are about this machine and this project.
     *
     * In a build without the runtime this is a no-op and hyp_ask_backend()
     * stays NULL, which is the correct answer for that binary. */
    (void)hyp_ask_llama_backend_install();

    hyp_mcp_tool_profile_t tool_profile = HYP_MCP_TOOL_PROFILE_ALL;
    if (role == HYP_DAEMON_PROCESS_MCP_CLIENT &&
        hyp_mcp_parse_tool_profile_args(argc, (const char *const *)argv, &tool_profile) != 0) {
        (void)fprintf(stderr, "hyponoia: --tool-profile requires the supported value "
                              "'analysis' or 'scout'\n");
        return 2;
    }
    const char *hook_event = NULL;
    const char *hook_dialect = NULL;
    if (role == HYP_DAEMON_PROCESS_HOOK_CLIENT &&
        !main_hook_options(argc, argv, &hook_event, &hook_dialect)) {
        return EXIT_SUCCESS; /* hook adapters are contractually fail-open */
    }

    /* Hook augmentation is contractually fail-open and time-bounded. It is
     * daemon-backed but CONNECT-ONLY: a hook never spawns a daemon (a cold
     * spawn cannot fit the fail-open budget and livelocks against the
     * last-client-exit teardown), it recycles whichever daemon an MCP
     * session or `daemon start` already brought up. Arm the deadline before
     * hashing and IPC. */
    if (role == HYP_DAEMON_PROCESS_HOOK_CLIENT) {
#ifndef _WIN32
        hyp_hook_augment_arm_deadline();
#endif
    }

    if (role == HYP_DAEMON_PROCESS_STATELESS) {
        int result = handle_subcommand(argc, argv, NULL, NULL);
        return result >= 0 ? result : EXIT_FAILURE;
    }

    if (role == HYP_DAEMON_PROCESS_LOCAL_CLI) {
        bool feedback_enabled = main_local_cli_feedback_enabled(argc, argv);
        FILE *feedback = feedback_enabled ? stderr : NULL;
        if (feedback) {
            (void)fputs("Preparing one-shot local HYP command...\n", feedback);
            (void)fflush(feedback);
        }
        /* main_daemon_endpoint_new(), not bootstrap_endpoint_new(NULL): the
         * one-shot local CLI is the ONLY daemon-coordinated path that ignored
         * the HYP_TEST_DAEMON_RUNTIME_PARENT seam, so `hyponoia cli <tool>`
         * could not be isolated from the developer's real account-wide
         * rendezvous even in a seam build. Found while measuring §2.2 lever 4:
         * two other agents' binaries were holding the version cohort, and the
         * seam that exists precisely to give a measurement its own namespace
         * did not reach the command being measured. With seams compiled OUT —
         * every production build — main_daemon_endpoint_new() IS
         * bootstrap_endpoint_new(NULL), so this is a literal no-op there. */
        hyp_daemon_ipc_endpoint_t *local_endpoint = main_daemon_endpoint_new();
        char local_executable[MAIN_PATH_CAP];
        hyp_daemon_build_identity_t local_identity;
        hyp_project_lock_manager_t *project_locks =
            local_endpoint ? hyp_project_lock_manager_new(local_endpoint) : NULL;
        hyp_version_cohort_manager_t *cohort_manager =
            local_endpoint ? hyp_version_cohort_manager_new(local_endpoint) : NULL;
        hyp_version_cohort_lease_t *cohort_lease = NULL;
        hyp_daemon_ipc_local_transition_t *local_transition = NULL;
        main_local_maintenance_context_t maintenance_context;
        bool maintenance_context_initialized = false;
        hyp_daemon_maintenance_monitor_t *maintenance_monitor = NULL;
        hyp_daemon_conflict_t cohort_conflict;
        hyp_version_cohort_status_t cohort_status = HYP_VERSION_COHORT_IO;
        main_build_identity_status_t local_identity_status = MAIN_BUILD_IDENTITY_OK;
        int result = HYP_NOT_FOUND;
        int exit_code = EXIT_FAILURE;
        bool cleanup_ok = true;
        const char *coordination_failure = NULL;
        if (!local_endpoint) {
            coordination_failure = "endpoint";
        } else if (!project_locks) {
            coordination_failure = "project-locks";
        } else if (!cohort_manager) {
            coordination_failure = "version-cohort";
        } else if (!main_resolve_executable(argv[0], local_executable)) {
            coordination_failure = "executable-path";
        } else if ((local_identity_status = main_build_identity(&local_identity)) !=
                   MAIN_BUILD_IDENTITY_OK) {
            coordination_failure = main_build_identity_status_name(local_identity_status);
        }
        if (coordination_failure) {
            (void)fprintf(stderr, "hyponoia: secure CLI coordination could not be created (%s)\n",
                          coordination_failure);
            goto local_cli_cleanup;
        }
        hyp_http_server_set_binary_path(local_executable);

        cohort_status = hyp_version_cohort_acquire(cohort_manager, &local_identity,
                                                   main_deadline_after(MAIN_STARTUP_TIMEOUT_MS),
                                                   &cohort_lease, &cohort_conflict);
        if (cohort_status != HYP_VERSION_COHORT_OK) {
            char message[HYP_DAEMON_CONFLICT_MESSAGE_SIZE];
            bool formatted = cohort_status == HYP_VERSION_COHORT_CONFLICT &&
                             hyp_daemon_conflict_format(&cohort_conflict, message, sizeof(message));
            if (cohort_status == HYP_VERSION_COHORT_CONFLICT) {
                (void)hyp_version_cohort_log_conflict(&cohort_conflict);
            }
            (void)fprintf(stderr, "hyponoia: %s\n",
                          formatted ? message
                                    : "CLI exact-build admission could not be verified; retry "
                                      "after active HYP operations exit");
            goto local_cli_cleanup;
        }
        main_local_maintenance_context_init(&maintenance_context);
        maintenance_context_initialized = true;
        maintenance_monitor =
            hyp_daemon_maintenance_monitor_start(cohort_manager, main_local_command_cancel,
                                                 &maintenance_context, EXIT_FAILURE, "CLI command");
        if (!maintenance_monitor) {
            (void)fprintf(stderr, "hyponoia: CLI maintenance observer could not start safely\n");
            goto local_cli_cleanup;
        }

        int transition_status =
            main_local_transition_acquire(local_endpoint, feedback, &local_transition);
        if (transition_status != 1 || !local_transition) {
            (void)fprintf(stderr,
                          "hyponoia: CLI startup coordination %s; retry after the "
                          "active HYP transition exits\n",
                          transition_status == 0 ? "remained busy"
                                                 : "could not be verified safely");
            goto local_cli_cleanup;
        }
        int seal_status = hyp_daemon_ipc_local_transition_seal_legacy(local_transition);
        if (seal_status != 1) {
            if (seal_status == 0) {
                (void)hyp_version_cohort_log_uncoordinated_daemon(&local_identity);
            }
            (void)fprintf(stderr, "hyponoia: HYP CLI could not start because a "
                                  "pre-coordination or unverified HYP generation is active; close "
                                  "all HYP sessions and commands, then retry\n");
            goto local_cli_cleanup;
        }

        hyp_version_cohort_daemon_presence_t daemon_presence =
            hyp_version_cohort_daemon_presence_under_transition(cohort_manager, local_endpoint,
                                                                local_transition);
        if (daemon_presence != HYP_VERSION_COHORT_DAEMON_ABSENT &&
            daemon_presence != HYP_VERSION_COHORT_DAEMON_COORDINATED) {
            if (daemon_presence == HYP_VERSION_COHORT_DAEMON_UNCOORDINATED) {
                (void)hyp_version_cohort_log_uncoordinated_daemon(&local_identity);
                (void)fprintf(stderr, "hyponoia: HYP CLI could not start because "
                                      "an active pre-coordination or unverified HYP daemon is "
                                      "running. Close all HYP sessions and commands, then "
                                      "retry.\n");
            } else {
                (void)fprintf(stderr, "hyponoia: active daemon coordination could "
                                      "not be verified safely; retry after active HYP sessions "
                                      "exit\n");
            }
            goto local_cli_cleanup;
        }
        if (!hyp_daemon_ipc_local_transition_begin_work(local_transition)) {
            (void)fprintf(stderr, "hyponoia: CLI startup coordination could not enter "
                                  "local work safely\n");
            goto local_cli_cleanup;
        }

        result = handle_subcommand(argc, argv, project_locks, &maintenance_context);
        exit_code = result >= 0 ? result : EXIT_FAILURE;

    local_cli_cleanup:
        main_local_maintenance_finish(&maintenance_monitor, &maintenance_context,
                                      maintenance_context_initialized, "CLI command");
        cleanup_ok = main_project_lock_manager_close(&project_locks) && cleanup_ok;
        cleanup_ok = main_local_transition_close(&local_transition) && cleanup_ok;
        /* Lifetime is the final coordination token released. The mutation
         * barrier must not prove every old participant gone while this process
         * still owns a local transition or project mutation lease. */
        cleanup_ok = main_version_cohort_close(&cohort_lease, &cohort_manager) && cleanup_ok;
        hyp_daemon_ipc_endpoint_free(local_endpoint);
        if (!cleanup_ok) {
            (void)fprintf(stderr, "hyponoia: CLI coordination cleanup failed\n");
            return EXIT_FAILURE;
        }
        return exit_code;
    }

    char executable_path[MAIN_PATH_CAP];
    hyp_daemon_build_identity_t identity;
    if (!main_resolve_executable(argv[0], executable_path)) {
        (void)fprintf(stderr, "hyponoia: exact executable identity could not be verified "
                              "(executable-path)\n");
        return role == HYP_DAEMON_PROCESS_HOOK_CLIENT ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    main_build_identity_status_t identity_status = main_build_identity(&identity);
    if (identity_status != MAIN_BUILD_IDENTITY_OK) {
        const char *validation_detail = hyp_daemon_ipc_validation_detail();
        (void)fprintf(stderr,
                      "hyponoia: exact executable identity could not be verified "
                      "(%s)%s%s\n",
                      main_build_identity_status_name(identity_status),
                      validation_detail[0] ? " - " : "", validation_detail);
        return role == HYP_DAEMON_PROCESS_HOOK_CLIENT ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    hyp_http_server_set_binary_path(executable_path);

    if (role == HYP_DAEMON_PROCESS_WORKER) {
        /* A supervised worker was forked by the daemon's supervisor, so its
         * environment is the DAEMON's, not the asking client's — the same
         * classification, reached a different way. No worker path reads an
         * escalation key today (a worker only ever runs the index tool;
         * `embed --escalation` is a direct CLI command), so this changes
         * nothing now. It is here because the RULE is "did this process get
         * its environment from the party that is asking", and a rule applied
         * to one of its two cases is an enumeration waiting to be wrong. */
        char worker_key_holder[128];
        (void)snprintf(worker_key_holder, sizeof(worker_key_holder),
                       "a hyponoia index worker (pid %llu) started by the daemon, with the "
                       "daemon's environment",
                       (unsigned long long)main_current_process_id());
        hyp_ask_provider_declare_shared_key_custody(worker_key_holder);
        hyp_index_worker_invocation_t invocation;
        hyp_index_worker_argv_status_t worker_status =
            hyp_index_worker_parse_process_argv(argc, argv, &invocation);
        if (worker_status != HYP_INDEX_WORKER_ARGV_VALID) {
            (void)fprintf(stderr, "HYP index worker could not start: %s\n",
                          hyp_index_worker_argv_status_message(worker_status));
            return EXIT_FAILURE;
        }
        hyp_daemon_ipc_endpoint_t *worker_endpoint = hyp_daemon_bootstrap_endpoint_new(NULL);
        hyp_project_lock_manager_t *worker_project_locks =
            worker_endpoint ? hyp_project_lock_manager_new(worker_endpoint) : NULL;
        hyp_version_cohort_manager_t *worker_cohort_manager =
            worker_endpoint ? hyp_version_cohort_manager_new(worker_endpoint) : NULL;
        hyp_version_cohort_lease_t *worker_cohort_lease = NULL;
        hyp_daemon_ipc_local_transition_t *worker_transition = NULL;
        main_local_maintenance_context_t worker_maintenance_context;
        bool worker_maintenance_context_initialized = false;
        hyp_daemon_maintenance_monitor_t *worker_maintenance_monitor = NULL;
        hyp_daemon_conflict_t worker_conflict;
        int result = HYP_NOT_FOUND;
        bool worker_cleanup_ok = true;
        hyp_version_cohort_status_t worker_cohort_status =
            worker_project_locks && worker_cohort_manager
                ? hyp_version_cohort_acquire(worker_cohort_manager, &identity,
                                             main_deadline_after(MAIN_STARTUP_TIMEOUT_MS),
                                             &worker_cohort_lease, &worker_conflict)
                : HYP_VERSION_COHORT_IO;
        if (worker_cohort_status != HYP_VERSION_COHORT_OK) {
            char message[HYP_DAEMON_CONFLICT_MESSAGE_SIZE];
            bool formatted = worker_cohort_status == HYP_VERSION_COHORT_CONFLICT &&
                             hyp_daemon_conflict_format(&worker_conflict, message, sizeof(message));
            if (worker_cohort_status == HYP_VERSION_COHORT_CONFLICT) {
                (void)hyp_version_cohort_log_conflict(&worker_conflict);
            }
            (void)fprintf(stderr, "HYP index worker could not start: %s\n",
                          formatted ? message : "exact-build admission failed");
            if (worker_cohort_status == HYP_VERSION_COHORT_CONFLICT) {
                (void)fprintf(stderr, "hyponoia: %s\n", hyp_daemon_conflict_escape_hint());
            }
            goto worker_cleanup;
        }

        main_local_maintenance_context_init(&worker_maintenance_context);
        worker_maintenance_context_initialized = true;
        worker_maintenance_monitor = hyp_daemon_maintenance_monitor_start(
            worker_cohort_manager, main_local_command_cancel, &worker_maintenance_context,
            EXIT_FAILURE, "index worker");
        if (!worker_maintenance_monitor) {
            (void)fprintf(stderr,
                          "HYP index worker could not start: maintenance observer unavailable\n");
            goto worker_cleanup;
        }

        int worker_transition_status =
            main_local_transition_acquire(worker_endpoint, NULL, &worker_transition);
        if (worker_transition_status != 1 || !worker_transition) {
            (void)fprintf(stderr, "HYP index worker could not start: local coordination %s\n",
                          worker_transition_status == 0 ? "remained busy"
                                                        : "could not be verified safely");
            goto worker_cleanup;
        }
        int worker_seal_status = hyp_daemon_ipc_local_transition_seal_legacy(worker_transition);
        if (worker_seal_status != 1) {
            if (worker_seal_status == 0) {
                (void)hyp_version_cohort_log_uncoordinated_daemon(&identity);
            }
            (void)fprintf(stderr, "HYP index worker could not start: a pre-coordination or "
                                  "unverified HYP generation is active\n");
            goto worker_cleanup;
        }
        hyp_version_cohort_daemon_presence_t worker_daemon_presence =
            hyp_version_cohort_daemon_presence_under_transition(worker_cohort_manager,
                                                                worker_endpoint, worker_transition);
        if (worker_daemon_presence != HYP_VERSION_COHORT_DAEMON_ABSENT &&
            worker_daemon_presence != HYP_VERSION_COHORT_DAEMON_COORDINATED) {
            if (worker_daemon_presence == HYP_VERSION_COHORT_DAEMON_UNCOORDINATED) {
                (void)hyp_version_cohort_log_uncoordinated_daemon(&identity);
            }
            (void)fprintf(stderr, "HYP index worker could not start: active daemon coordination "
                                  "could not be verified safely\n");
            goto worker_cleanup;
        }
        if (!hyp_daemon_ipc_local_transition_begin_work(worker_transition)) {
            (void)fprintf(stderr, "HYP index worker could not start: local coordination could not "
                                  "enter worker execution\n");
            goto worker_cleanup;
        }
        hyp_index_set_worker_role_options(true, invocation.response_out, invocation.single_thread,
                                          invocation.marker_file, invocation.quarantine_file,
                                          invocation.memory_budget_bytes);
#ifndef _WIN32
        /* Split into three ordered steps rather than one condition, because the
         * ORDER is load-bearing and the middle step only exists in test builds:
         *   1. establish the isolated process group,
         *   2. (test builds) start the crash-orphan probe, which must inherit
         *      that group and must fork BEFORE the watchdog thread exists —
         *      forking a multithreaded process is the bug this ordering avoids,
         *   3. start the parent-death watchdog thread.
         * Keeping the probe inside a single `||` chain made its call
         * unconditional in the source, so with the seam compiled out cppcheck
         * correctly reported `!probe()` as always false. Guarding the STEP, not
         * stubbing the function, means release builds simply do not have it. */
        if (!worker_prepare_process_group() || process_initial_ppid <= 1 ||
            getppid() != process_initial_ppid) {
            worker_containment_unavailable();
        }
#ifdef HYP_ENABLE_TEST_SEAMS
        if (!worker_start_watchdog_test_descendant()) {
            worker_containment_unavailable();
        }
#endif
        if (!worker_start_parent_watchdog(process_initial_ppid)) {
            worker_containment_unavailable();
        }
#endif
        hyp_index_supervisor_mark_host();
        result = handle_subcommand(argc, argv, worker_project_locks, &worker_maintenance_context);

    worker_cleanup:
        main_local_maintenance_finish(&worker_maintenance_monitor, &worker_maintenance_context,
                                      worker_maintenance_context_initialized, "index worker");
        worker_cleanup_ok =
            main_project_lock_manager_close(&worker_project_locks) && worker_cleanup_ok;
        worker_cleanup_ok = main_local_transition_close(&worker_transition) && worker_cleanup_ok;
        /* As in the parent CLI, release cohort lifetime last so activation
         * cannot overtake physical-worker coordination cleanup. */
        worker_cleanup_ok =
            main_version_cohort_close(&worker_cohort_lease, &worker_cohort_manager) &&
            worker_cleanup_ok;
        hyp_daemon_ipc_endpoint_free(worker_endpoint);
        if (!worker_cleanup_ok || worker_cohort_status != HYP_VERSION_COHORT_OK || result < 0) {
            return EXIT_FAILURE;
        }
        return result;
    }

    hyp_daemon_ipc_endpoint_t *endpoint = main_daemon_endpoint_new();
    if (!endpoint) {
        (void)fprintf(stderr, "hyponoia: secure daemon endpoint could not be created\n");
        return EXIT_FAILURE;
    }

    if (role == HYP_DAEMON_PROCESS_DAEMON_CTL) {
        int ctl_result = main_run_daemon_ctl(argc, argv, endpoint, &identity, executable_path);
        hyp_daemon_ipc_endpoint_free(endpoint);
        return ctl_result;
    }

    if (role == HYP_DAEMON_PROCESS_DAEMON) {
        setup_signal_handlers();
        /* THIS PROCESS'S ENVIRONMENT IS NOT ITS CALLERS' (NEXT-STEPS §3.2
         * step 5). Declared here, at the one place a daemon is born, rather
         * than per MCP session: `environ` was fixed at exec and belongs to the
         * shell that started this daemon, while the sessions, the CLI tool
         * invocations and the graph UI's HTTP routes it is about to serve all
         * belong to other processes. Everything downstream that would read an
         * escalation key out of the environment now knows it would be
         * spending someone else's, and refuses unless
         * ask.escalation.daemon_key says otherwise.
         *
         * Before any thread starts, which is what makes the state safe to read
         * from a request thread without a lock. */
        char key_holder[128];
        char started[32];
        time_t started_at = time(NULL);
        struct tm started_tm;
        if (hyp_gmtime_r(&started_at, &started_tm) == NULL ||
            strftime(started, sizeof(started), "%Y-%m-%dT%H:%M:%SZ", &started_tm) == 0) {
            (void)snprintf(started, sizeof(started), "an unrecorded time");
        }
        (void)snprintf(key_holder, sizeof(key_holder),
                       "the hyponoia daemon (pid %llu, holding it since %s)",
                       (unsigned long long)main_current_process_id(), started);
        hyp_ask_provider_declare_shared_key_custody(key_holder);
        hyp_daemon_host_config_t host_config = {
            .endpoint = endpoint,
            .identity = identity,
            .executable_path = executable_path,
            .stop_requested = &g_shutdown,
            /* The role classifier already enforced the byte-exact grammar:
             * argc==3 can only be the permanent spawn shape. */
            .permanent = argc == 3,
        };
        int result = hyp_daemon_host_run(&host_config);
        hyp_daemon_ipc_endpoint_free(endpoint);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

#ifdef HYP_ENABLE_TEST_SEAMS
    /* #1388 test seam: let one binary present a foreign build fingerprint as a
     * hook client, so the daemon-conflict reporting path is testable without
     * building a second binary. */
    static char seam_hook_build[HYP_DAEMON_BUILD_FINGERPRINT_SIZE];
    const char *seam_forced_build = hyp_safe_getenv("HYP_TEST_HOOK_CLIENT_BUILD", seam_hook_build,
                                                    sizeof(seam_hook_build), NULL);
    if (role == HYP_DAEMON_PROCESS_HOOK_CLIENT && seam_forced_build && seam_forced_build[0]) {
        identity.build_fingerprint = seam_hook_build;
    }
#endif

    hyp_version_cohort_manager_t *client_cohort_manager = hyp_version_cohort_manager_new(endpoint);
    hyp_version_cohort_lease_t *client_cohort_lease = NULL;
    hyp_daemon_conflict_t client_cohort_conflict;
    hyp_version_cohort_status_t client_cohort_status =
        client_cohort_manager
            ? hyp_version_cohort_acquire(client_cohort_manager, &identity,
                                         main_deadline_after(role == HYP_DAEMON_PROCESS_HOOK_CLIENT
                                                                 ? MAIN_HOOK_REQUEST_TIMEOUT_MS
                                                                 : MAIN_MCP_STARTUP_TIMEOUT_MS),
                                         &client_cohort_lease, &client_cohort_conflict)
            : HYP_VERSION_COHORT_IO;
    if (client_cohort_status != HYP_VERSION_COHORT_OK) {
        char message[HYP_DAEMON_CONFLICT_MESSAGE_SIZE];
        bool formatted =
            client_cohort_status == HYP_VERSION_COHORT_CONFLICT &&
            hyp_daemon_conflict_format(&client_cohort_conflict, message, sizeof(message));
        if (client_cohort_status == HYP_VERSION_COHORT_CONFLICT) {
            (void)hyp_version_cohort_log_conflict(&client_cohort_conflict);
        }
        (void)fprintf(stderr, "hyponoia: %s\n",
                      formatted ? message : "client exact-build admission failed");
        if (client_cohort_status == HYP_VERSION_COHORT_CONFLICT) {
            (void)fprintf(stderr, "hyponoia: %s\n", hyp_daemon_conflict_escape_hint());
        }
        if (role == HYP_DAEMON_PROCESS_HOOK_CLIENT &&
            client_cohort_status == HYP_VERSION_COHORT_CONFLICT) {
            main_hook_report_conflicted_daemon(hook_dialect);
        }
        (void)main_version_cohort_close(&client_cohort_lease, &client_cohort_manager);
        hyp_daemon_ipc_endpoint_free(endpoint);
        return role == HYP_DAEMON_PROCESS_HOOK_CLIENT ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (role == HYP_DAEMON_PROCESS_HOOK_CLIENT) {
        /* Connect-only: recycle an active daemon or fail open fast. */
        hyp_daemon_runtime_connect_result_t hook_connect;
        hyp_daemon_runtime_client_t *hook_client = hyp_daemon_runtime_client_connect(
            endpoint, &identity, MAIN_HOOK_CONNECT_TIMEOUT_MS, &hook_connect);
        hyp_daemon_ipc_endpoint_free(endpoint);
        if (!hook_client) {
            if (hook_connect.status == HYP_DAEMON_RUNTIME_CONNECT_CONFLICT) {
                char conflict_detail[HYP_DAEMON_CONFLICT_MESSAGE_SIZE];
                if (hyp_daemon_conflict_format(&hook_connect.conflict, conflict_detail,
                                               sizeof(conflict_detail))) {
                    (void)fprintf(stderr, "hyponoia: %s\n", conflict_detail);
                }
                main_hook_report_conflicted_daemon(hook_dialect);
            } else {
                main_hook_report_absent_daemon(hook_dialect);
            }
            (void)main_version_cohort_close(&client_cohort_lease, &client_cohort_manager);
            return EXIT_SUCCESS;
        }
#ifdef _WIN32
        /* Windows keeps the upstream fixed augmentation budget, armed only
         * after the authenticated connection. */
        hyp_hook_augment_arm_deadline();
#endif
        /* Fail-open: a hook must never block the caller's tool use, so the
         * exit code is EXIT_SUCCESS even when augmentation failed — the
         * frontend already emitted any visible notice. */
        (void)main_run_hook_frontend(hook_client, hook_event, hook_dialect);
        (void)hyp_daemon_runtime_client_close(hook_client, MAIN_HOOK_CLOSE_TIMEOUT_MS);
        (void)main_version_cohort_close(&client_cohort_lease, &client_cohort_manager);
        return EXIT_SUCCESS;
    }

    hyp_daemon_bootstrap_config_t bootstrap_config = {
        .role = role,
        .endpoint = endpoint,
        .identity = &identity,
        .executable_path = executable_path,
        .connect_timeout_ms = MAIN_CONNECT_TIMEOUT_MS,
        .startup_timeout_ms = MAIN_MCP_STARTUP_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t bootstrap_result;
    hyp_daemon_bootstrap_status_t bootstrap_status =
        main_client_bootstrap_with_upgrade(&bootstrap_config, &bootstrap_result);
    hyp_daemon_ipc_endpoint_free(endpoint);
    if (bootstrap_status != HYP_DAEMON_BOOTSTRAP_CONNECTED || !bootstrap_result.client) {
        (void)main_version_cohort_close(&client_cohort_lease, &client_cohort_manager);
        return EXIT_FAILURE;
    }

    g_daemon_client = bootstrap_result.client;

    if (role == HYP_DAEMON_PROCESS_MCP_CLIENT &&
        !main_set_client_context(g_daemon_client, NULL, tool_profile, NULL, NULL,
                                 MAIN_CONNECT_TIMEOUT_MS)) {
        (void)fprintf(stderr, "hyponoia: daemon session context was rejected\n");
        (void)hyp_daemon_runtime_client_close(g_daemon_client, MAIN_CLOSE_TIMEOUT_MS);
        g_daemon_client = NULL;
        (void)main_version_cohort_close(&client_cohort_lease, &client_cohort_manager);
        return EXIT_FAILURE;
    }

    /* Persist UI mutations only after the exact-build HELLO succeeds. A
     * conflicting binary must be observationally read-only: applying its
     * flags before bootstrap could reconfigure the already-running daemon
     * even though that client was then rejected. */
    if (role == HYP_DAEMON_PROCESS_MCP_CLIENT && hyp_mcp_tool_profile_allows_http(tool_profile)) {
        bool ui_enabled = false;
        int ui_port = 0;
        bool explicitly_enabled = false;
        uint8_t update_mask =
            parse_ui_flags(argc, argv, &ui_enabled, &ui_port, &explicitly_enabled);
        if (update_mask != 0 && hyp_daemon_application_client_set_ui_config(
                                    g_daemon_client, update_mask, ui_enabled, ui_port,
                                    MAIN_CONNECT_TIMEOUT_MS) != HYP_DAEMON_RUNTIME_APPLICATION_OK) {
            (void)fprintf(stderr, "hyponoia: daemon UI configuration update failed\n");
            (void)hyp_daemon_runtime_client_close(g_daemon_client, MAIN_CLOSE_TIMEOUT_MS);
            g_daemon_client = NULL;
            (void)main_version_cohort_close(&client_cohort_lease, &client_cohort_manager);
            return EXIT_FAILURE;
        }
        if (explicitly_enabled && !hyp_ui_assets_supported()) {
            (void)fprintf(stderr, "hyponoia: --ui requested, but this binary was built "
                                  "without UI support; rebuild with `make -f Makefile.hyp "
                                  "hyp-with-ui`.\n");
        }
    }
#ifndef _WIN32
    if (!client_start_parent_watchdog(process_initial_ppid)) {
        (void)fprintf(stderr, "hyponoia: parent-death watchdog could not start\n");
        (void)hyp_daemon_runtime_client_close(g_daemon_client, MAIN_CLOSE_TIMEOUT_MS);
        g_daemon_client = NULL;
        (void)main_version_cohort_close(&client_cohort_lease, &client_cohort_manager);
        return EXIT_FAILURE;
    }
#endif

    setup_signal_handlers();
    int result = hyp_daemon_frontend_mcp_run(g_daemon_client, client_cohort_manager, stdin, stdout);
    g_daemon_client = NULL; /* frontend consumed the handle */
    bool client_cohort_cleanup =
        main_version_cohort_close(&client_cohort_lease, &client_cohort_manager);
    atomic_store(&g_shutdown, 1);
    if (!client_cohort_cleanup) {
        return EXIT_FAILURE;
    }
    return result < 0 ? EXIT_FAILURE : result;
}
