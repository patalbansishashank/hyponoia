/*
 * bootstrap.c — Mandatory per-account daemon startup policy.
 */
#include "daemon/bootstrap.h"

#include "daemon/ipc.h"
#include "daemon/service.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "foundation/subprocess.h"
#include "foundation/win_utf8.h"
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread.h>
#include <spawn.h>
extern char **environ;
#endif
#endif

enum {
    BOOTSTRAP_RETRY_NS = 1000000,
    BOOTSTRAP_COORDINATION_CLEANUP_MS = 500,
    BOOTSTRAP_PATH_CAP = 4096,
    /* Hex characters of the cache digest that names an isolated rendezvous.
     * 48 bits over a handful of per-run cache roots; the rest of the budget
     * belongs to the caller's parent path, which sun_path bounds hard. */
    BOOTSTRAP_ISOLATION_SCOPE_HEX = 12,
};

static bool bootstrap_arg_is(const char *arg, const char *expected) {
    return arg && expected && strcmp(arg, expected) == 0;
}

static int bootstrap_find_arg(int argc, char *const argv[], const char *expected) {
    for (int i = 1; argv && i < argc; i++) {
        if (bootstrap_arg_is(argv[i], expected)) {
            return i;
        }
    }
    return -1;
}

static bool bootstrap_has_help_after(int argc, char *const argv[], int start) {
    for (int i = start; argv && i < argc; i++) {
        if (bootstrap_arg_is(argv[i], "--help") || bootstrap_arg_is(argv[i], "-h")) {
            return true;
        }
    }
    return false;
}

static bool bootstrap_worker_fingerprint_valid(const char *fingerprint) {
    if (!fingerprint || strlen(fingerprint) != 64U) {
        return false;
    }
    for (size_t i = 0; i < 64U; i++) {
        char ch = fingerprint[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool bootstrap_worker_budget_valid(const char *text) {
    if (!text || !text[0]) {
        return false;
    }
    size_t value = 0;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        size_t digit = (size_t)(*cursor - '0');
        if (value > (SIZE_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    return value > 0;
}

/* Keep the bootstrap role boundary exact and fail closed before any client or
 * worker state is initialized. index_supervisor owns the matching builder and
 * performs the captured-build comparison after this syntax-only classification. */
static bool bootstrap_worker_argv_exact(int argc, char *const argv[]) {
    if (argc < 9 || !argv || !bootstrap_arg_is(argv[1], "cli") ||
        !bootstrap_arg_is(argv[2], "--index-worker") ||
        !bootstrap_arg_is(argv[3], "--index-worker-build") ||
        !bootstrap_worker_fingerprint_valid(argv[4]) ||
        !bootstrap_arg_is(argv[5], "index_repository") || !argv[6] || !argv[6][0] ||
        !bootstrap_arg_is(argv[7], "--response-out") || !argv[8] || !argv[8][0]) {
        return false;
    }
    int next = 9;
    if (next < argc && bootstrap_arg_is(argv[next], "--index-worker-memory-budget-bytes")) {
        if (next + 1 >= argc || !bootstrap_worker_budget_valid(argv[next + 1])) {
            return false;
        }
        next += 2;
    }
    if (next < argc && bootstrap_arg_is(argv[next], "--index-worker-single-thread")) {
        next++;
    }
    if (next < argc && bootstrap_arg_is(argv[next], "--index-worker-marker")) {
        if (next + 1 >= argc || !argv[next + 1] || !argv[next + 1][0]) {
            return false;
        }
        next += 2;
    }
    if (next < argc && bootstrap_arg_is(argv[next], "--index-worker-quarantine")) {
        if (next + 1 >= argc || !argv[next + 1] || !argv[next + 1][0]) {
            return false;
        }
        next += 2;
    }
    return next == argc;
}

hyp_daemon_process_role_t hyp_daemon_process_role(int argc, char *const argv[]) {
    if (argc <= 0 || !argv || !argv[0] || argv[0][0] == '\0') {
        return HYP_DAEMON_PROCESS_INVALID;
    }

    int daemon_arg = bootstrap_find_arg(argc, argv, HYP_DAEMON_INTERNAL_ARG);
    if (daemon_arg >= 0) {
        /* Byte-exact daemon-role grammar, deliberately unforgiving: the bare
         * internal marker, or the marker followed by exactly the permanent
         * flag. Every other shape — reordered, repeated, or extended — is
         * INVALID, so argv smuggling cannot reach the daemon role. */
        if (argc == 2 && daemon_arg == 1) {
            return HYP_DAEMON_PROCESS_DAEMON;
        }
        if (argc == 3 && daemon_arg == 1 && bootstrap_arg_is(argv[2], HYP_DAEMON_PERMANENT_ARG)) {
            return HYP_DAEMON_PROCESS_DAEMON;
        }
        return HYP_DAEMON_PROCESS_INVALID;
    }

    int worker_arg = bootstrap_find_arg(argc, argv, "--index-worker");
    if (worker_arg >= 0) {
        return bootstrap_worker_argv_exact(argc, argv) ? HYP_DAEMON_PROCESS_WORKER
                                                       : HYP_DAEMON_PROCESS_INVALID;
    }

    static const char *const stateless_commands[] = {
        "--verify-runtime-assets",
        "install",
        "uninstall",
        "update",
        /* allow-root writes one line of user-level config and reads nothing from a
         * project, so it needs no daemon. Listed here rather than routed through
         * the daemon so enrolling a root cannot depend on daemon state. */
        "allow-root",
        /* fetch-model writes ONE file into the user's cache and reads no
         * project, no index and no store. Omitting it here does not merely
         * cost a daemon: an unlisted top-level command falls through to
         * MCP_CLIENT, so `hyponoia fetch-model --path` would attach to a
         * daemon and then block serving MCP on stdin — a hang, not an error.
         * It also has to work when the daemon cannot start, which is exactly
         * when someone is trying to get the `ask` lane running. */
        "fetch-model",
        /* sync merges two record stores and touches no project, no index and
         * no daemon state. It carries the same hazard fetch-model documents
         * above: unlisted, a top-level command falls through to MCP_CLIENT,
         * so `hyponoia sync` would attach to a daemon and then serve MCP on
         * stdin — a hang where a merge was asked for. */
        "sync",
    };
    /* Stop at the first top-level mode token. Tool names, flag values, and JSON
     * following `cli` are opaque user input: a search query named "install"
     * or containing "--version" must never bypass the mandatory daemon. */
    for (int arg = 1; arg < argc; arg++) {
        if (bootstrap_arg_is(argv[arg], "cli")) {
            if (bootstrap_has_help_after(argc, argv, arg + 1)) {
                return HYP_DAEMON_PROCESS_STATELESS;
            }
            return HYP_DAEMON_PROCESS_LOCAL_CLI;
        }
        if (bootstrap_arg_is(argv[arg], "hook-augment")) {
            return HYP_DAEMON_PROCESS_HOOK_CLIENT;
        }
        if (bootstrap_arg_is(argv[arg], "config")) {
            return bootstrap_has_help_after(argc, argv, arg + 1) ? HYP_DAEMON_PROCESS_STATELESS
                                                                 : HYP_DAEMON_PROCESS_LOCAL_CLI;
        }
        /* `embed` builds the `ask` lane's vectors. It reads the graph database
         * READ-ONLY through its own connection and writes only to
         * <cache>/vectors/<project>.db, so it needs neither a daemon nor a
         * project mutation lease — it cannot mutate the graph at all. Stateless
         * for the same reason `allow-root` is: routing it through the daemon
         * would make an opt-in second pass depend on daemon state it never
         * touches. Placed after the `cli` check so an opaque tool argument
         * spelled "embed" cannot bypass the mandatory daemon. */
        if (bootstrap_arg_is(argv[arg], "embed")) {
            return HYP_DAEMON_PROCESS_STATELESS;
        }
        /* Placed after the `cli` check on purpose: `hyp cli search "daemon
         * start"` is opaque tool input and must stay LOCAL_CLI. */
        if (bootstrap_arg_is(argv[arg], "daemon")) {
            return bootstrap_has_help_after(argc, argv, arg + 1) ? HYP_DAEMON_PROCESS_STATELESS
                                                                 : HYP_DAEMON_PROCESS_DAEMON_CTL;
        }
        if (bootstrap_arg_is(argv[arg], "--version") || bootstrap_arg_is(argv[arg], "--help") ||
            bootstrap_arg_is(argv[arg], "-h")) {
            return HYP_DAEMON_PROCESS_STATELESS;
        }
        for (size_t command = 0;
             command < sizeof(stateless_commands) / sizeof(stateless_commands[0]); command++) {
            if (bootstrap_arg_is(argv[arg], stateless_commands[command])) {
                return HYP_DAEMON_PROCESS_STATELESS;
            }
        }
    }
    return HYP_DAEMON_PROCESS_MCP_CLIENT;
}

bool hyp_daemon_process_role_requires_client(hyp_daemon_process_role_t role) {
    return role == HYP_DAEMON_PROCESS_MCP_CLIENT || role == HYP_DAEMON_PROCESS_HOOK_CLIENT;
}

/* ── The supported isolation escape ───────────────────────────────────────
 *
 * The rendezvous is account-wide on purpose. Every HYP process for one uid
 * meets under ONE directory, and the version cohort inside it admits exactly
 * one build. That guard is not bureaucracy: the socket, the cohort record and
 * the project mutation leases all live in that directory, and they are what
 * stop two builds from interleaving writes into one SQLite cache.
 *
 * What it lacked was an exit. A second build could not run AT ALL while a
 * first one was alive, and an MCP server that cannot start does not fail
 * loudly — it leaves its client running tool-less and silent. The only escape
 * was HYP_TEST_DAEMON_RUNTIME_PARENT, compiled in only under
 * HYP_ENABLE_TEST_SEAMS, so "work around it" meant "build a different binary".
 *
 * HYP_DAEMON_RUNTIME_PARENT is the escape a normal build gets, and it keeps
 * the protection by making the rendezvous A FUNCTION OF THE CACHE ROOT: the
 * directory created under the caller's parent is named by a digest of the
 * canonical HYP_CACHE_DIR. Two processes on one cache still land on one
 * rendezvous and still meet the cohort guard; two processes on two caches
 * share no socket, no cohort record and no lease file.
 *
 * The build fingerprint is deliberately NOT a rendezvous input. Keying the
 * meeting place by build is exactly how two builds would stop meeting while
 * still writing to one store — it is the corruption the guard exists to
 * prevent, spelled as a feature. The fingerprint stays what the cohort
 * COMPARES once two processes have met.
 *
 * For the same reason isolation is refused outright while the cache still
 * resolves to the account default: a private socket over the shared store is
 * the one combination that loses data.
 */
static bool bootstrap_default_cache_dir(char *out, size_t out_size) {
    const char *home = hyp_get_home_dir();
    if (!home || !home[0]) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/.cache/hyponoia", home);
    if (written <= 0 || (size_t)written >= out_size) {
        return false;
    }
    hyp_normalize_path_sep(out);
    return true;
}

static bool bootstrap_isolation_cache_is_account_default(const char *canonical_cache) {
    char configured[BOOTSTRAP_PATH_CAP];
    char canonical_default[BOOTSTRAP_PATH_CAP];
    if (!bootstrap_default_cache_dir(configured, sizeof(configured))) {
        return false;
    }
    if (hyp_canonical_path(configured, canonical_default, sizeof(canonical_default))) {
        hyp_normalize_path_sep(canonical_default);
        if (strcmp(canonical_default, canonical_cache) == 0) {
            return true;
        }
    }
    return strcmp(configured, canonical_cache) == 0;
}

/* Resolve the cache the way main_build_identity does, so the digest names the
 * same root the cohort record will later carry. */
static bool bootstrap_isolation_canonical_cache(char *out, size_t out_size) {
    char configured[BOOTSTRAP_PATH_CAP];
    const char *cache = hyp_resolve_cache_dir();
    if (!cache || !cache[0]) {
        return false;
    }
    int written = snprintf(configured, sizeof(configured), "%s", cache);
    if (written <= 0 || (size_t)written >= sizeof(configured)) {
        return false;
    }
    bool ready = hyp_canonical_path(configured, out, out_size) != 0;
    if (!ready && hyp_mkdir_p(configured, 0700)) {
        ready = hyp_canonical_path(configured, out, out_size) != 0;
    }
    if (!ready || !hyp_is_dir(out)) {
        return false;
    }
    hyp_normalize_path_sep(out);
    return true;
}

#ifndef _WIN32
/* The endpoint will build <scope>/hyp-daemon-<euid>/hyp-<key>.sock and bind it
 * through a sockaddr_un, whose sun_path is 108 bytes on Linux and 104 on the
 * BSDs. Overrunning it fails deep inside the endpoint with nothing but
 * "secure daemon endpoint could not be created", so the bound is checked here,
 * where the caller's own parent can be named back to them. */
static bool bootstrap_isolation_socket_path_fits(const char *scope_parent, size_t *maximum_out) {
    struct sockaddr_un probe;
    size_t leaf = strlen("/hyp-daemon-") + strlen("/hyp-") + (HYP_DAEMON_KEY_SIZE - 1U) +
                  strlen(".sock") + 1U;
    char uid_text[32];
    int uid_written = snprintf(uid_text, sizeof(uid_text), "%lu", (unsigned long)geteuid());
    if (uid_written <= 0 || (size_t)uid_written >= sizeof(uid_text)) {
        return false;
    }
    leaf += (size_t)uid_written;
    if (leaf >= sizeof(probe.sun_path)) {
        return false;
    }
    size_t maximum = sizeof(probe.sun_path) - leaf;
    if (maximum_out) {
        *maximum_out = maximum;
    }
    return strlen(scope_parent) <= maximum;
}
#endif

/* Returns the directory to hand the endpoint as its parent, or false with a
 * named reason already written to stderr. */
static bool bootstrap_isolated_parent(const char *requested_parent, char *out, size_t out_size) {
    char canonical_cache[BOOTSTRAP_PATH_CAP];
    if (!bootstrap_isolation_canonical_cache(canonical_cache, sizeof(canonical_cache))) {
        (void)fprintf(stderr,
                      "hyponoia: HYP_DAEMON_RUNTIME_PARENT is set, but the cache directory could "
                      "not be resolved. Set HYP_CACHE_DIR to an absolute owner-writable path "
                      "and retry.\n");
        return false;
    }
    if (bootstrap_isolation_cache_is_account_default(canonical_cache)) {
        (void)fprintf(stderr,
                      "hyponoia: HYP_DAEMON_RUNTIME_PARENT is set, but HYP_CACHE_DIR still "
                      "resolves to the account default (%s). An isolated rendezvous over the "
                      "shared cache would run two daemons against one store, which is the "
                      "corruption the cohort guard exists to prevent. Point HYP_CACHE_DIR at a "
                      "directory this build owns, then retry.\n",
                      canonical_cache);
        return false;
    }

    char digest[HYP_SHA256_HEX_LEN + 1];
    hyp_sha256_hex(canonical_cache, strlen(canonical_cache), digest);

    size_t parent_length = strlen(requested_parent);
    bool has_separator = parent_length > 0 && (requested_parent[parent_length - 1] == '/' ||
                                               requested_parent[parent_length - 1] == '\\');
    int written = snprintf(out, out_size, "%s%shyp-scope-%.*s", requested_parent,
                           has_separator ? "" : "/", (int)BOOTSTRAP_ISOLATION_SCOPE_HEX, digest);
    if (written <= 0 || (size_t)written >= out_size) {
        (void)fprintf(stderr, "hyponoia: HYP_DAEMON_RUNTIME_PARENT is too long to hold an isolated "
                              "rendezvous directory.\n");
        return false;
    }
#ifndef _WIN32
    size_t maximum = 0;
    if (!bootstrap_isolation_socket_path_fits(out, &maximum)) {
        (void)fprintf(stderr,
                      "hyponoia: HYP_DAEMON_RUNTIME_PARENT is too long. The daemon socket is a "
                      "unix-domain path and the whole path must fit in %zu bytes, which leaves "
                      "at most %zu bytes for the parent; \"%s\" needs %zu. Choose a shorter "
                      "directory, for example /tmp/hyp-isolated.\n",
                      sizeof(((struct sockaddr_un *)0)->sun_path), maximum, requested_parent,
                      strlen(out));
        return false;
    }
#endif
    if (!hyp_daemon_ipc_private_directory_secure(out)) {
        (void)fprintf(stderr,
                      "hyponoia: HYP_DAEMON_RUNTIME_PARENT could not be prepared as an "
                      "owner-only rendezvous. \"%s\" must be an absolute path whose existing "
                      "ancestors are owned by this account and are not group- or "
                      "world-writable; the leaf is created mode 0700 when absent.\n",
                      out);
        return false;
    }
    return true;
}

hyp_daemon_ipc_endpoint_t *hyp_daemon_bootstrap_endpoint_new(const char *runtime_parent) {
    char key[HYP_DAEMON_KEY_SIZE];
    if (!hyp_daemon_rendezvous_key(key)) {
        return NULL;
    }
#ifdef HYP_ENABLE_TEST_SEAMS
    /* HYP_TEST_DAEMON_RUNTIME_PARENT exists so a run can have its own
     * rendezvous namespace instead of the account-wide one. It was read in
     * exactly ONE caller (main.c's daemon role), which left every other
     * daemon-coordinated path — including `hyponoia cli <tool>`, the shape
     * every measurement harness uses — attached to the real namespace with no
     * way to opt out. Found while measuring §2.2 lever 4: two other agents'
     * builds held the version cohort, and the seam whose whole purpose is
     * isolation did not reach the command being measured.
     *
     * Reading it HERE covers every caller by construction, and an explicit
     * argument still wins. With seams compiled out — every production build —
     * this block does not exist and the function is byte-for-byte what it
     * was. */
    char seam_parent[HYP_SZ_1K];
    if (!runtime_parent) {
        runtime_parent = hyp_safe_getenv("HYP_TEST_DAEMON_RUNTIME_PARENT", seam_parent,
                                         sizeof(seam_parent), NULL);
    }
#endif
    /* Read HERE, for the same reason the seam above is: this is the one
     * constructor every daemon-coordinated path goes through, so covering it
     * covers `hyponoia`, `hyponoia cli <tool>`, `daemon start|status|stop` and
     * the detached daemon child (which inherits the environment) by
     * construction, with no caller left attached to the account-wide
     * namespace. An explicit argument still wins. */
    char isolated_parent[BOOTSTRAP_PATH_CAP];
    if (!runtime_parent) {
        char requested[HYP_SZ_1K];
        const char *isolation =
            hyp_safe_getenv("HYP_DAEMON_RUNTIME_PARENT", requested, sizeof(requested), NULL);
        if (isolation && isolation[0]) {
            if (!bootstrap_isolated_parent(isolation, isolated_parent, sizeof(isolated_parent))) {
                return NULL;
            }
            runtime_parent = isolated_parent;
        }
    }
    return hyp_daemon_ipc_endpoint_new(key, runtime_parent);
}

bool hyp_daemon_bootstrap_launch_spec_init(const char *executable_path,
                                           hyp_daemon_bootstrap_launch_spec_t *spec_out) {
    if (!executable_path || executable_path[0] == '\0' || !spec_out) {
        return false;
    }
    memset(spec_out, 0, sizeof(*spec_out));
    spec_out->executable_path = executable_path;
    spec_out->argv[0] = executable_path;
    spec_out->argv[1] = HYP_DAEMON_INTERNAL_ARG;
    spec_out->argc = 2U;
    spec_out->detached = true;
    spec_out->inherit_standard_handles = false;
    spec_out->use_shell = false;
    return true;
}

bool hyp_daemon_bootstrap_launch_spec_init_permanent(const char *executable_path,
                                                     hyp_daemon_bootstrap_launch_spec_t *spec_out) {
    if (!hyp_daemon_bootstrap_launch_spec_init(executable_path, spec_out)) {
        return false;
    }
    spec_out->argv[2] = HYP_DAEMON_PERMANENT_ARG;
    spec_out->argc = 3U;
    return true;
}

static uint64_t bootstrap_deadline_after(uint32_t timeout_ms) {
    uint64_t now = hyp_now_ms();
    return UINT64_MAX - now < timeout_ms ? UINT64_MAX : now + (uint64_t)timeout_ms;
}

static _Noreturn void bootstrap_cleanup_fail_stop(const char *component) {
    (void)fprintf(stderr,
                  "hyponoia: coordination cleanup failed (%s); "
                  "terminating so the OS releases retained claims\n",
                  component ? component : "unknown");
    (void)fflush(stderr);
#ifdef _WIN32
    (void)TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
    abort();
#else
    _exit(EXIT_FAILURE);
#endif
}

static void bootstrap_pause(uint64_t deadline) {
    uint64_t now = hyp_now_ms();
    if (now >= deadline) {
        return;
    }
    uint64_t remaining_ms = deadline - now;
    struct timespec pause = {
        .tv_sec = 0,
        .tv_nsec = remaining_ms > 1 ? BOOTSTRAP_RETRY_NS : (long)(remaining_ms * 1000000ULL),
    };
    (void)hyp_nanosleep(&pause, NULL);
}

static void bootstrap_startup_lock_release_complete(const hyp_daemon_bootstrap_ops_t *ops,
                                                    hyp_daemon_bootstrap_lock_t *lock_io) {
    uint64_t deadline = bootstrap_deadline_after(BOOTSTRAP_COORDINATION_CLEANUP_MS);
    while (ops && ops->startup_lock_release && lock_io && *lock_io) {
        (void)ops->startup_lock_release(ops->context, lock_io);
        if (!*lock_io) {
            return;
        }
        if (hyp_now_ms() >= deadline) {
            bootstrap_cleanup_fail_stop("startup_lock_cleanup");
        }
        hyp_usleep(1000);
    }
}

static void bootstrap_result_reset(hyp_daemon_bootstrap_result_t *result,
                                   hyp_daemon_process_role_t role) {
    memset(result, 0, sizeof(*result));
    result->status = hyp_daemon_process_role_requires_client(role) ? HYP_DAEMON_BOOTSTRAP_FAILED
                                                                   : HYP_DAEMON_BOOTSTRAP_BYPASSED;
}

static hyp_daemon_bootstrap_status_t bootstrap_finish_probe(
    hyp_daemon_bootstrap_probe_status_t probe, hyp_daemon_runtime_client_t *client,
    const hyp_daemon_runtime_connect_result_t *connect_result,
    const hyp_daemon_bootstrap_ops_t *ops, hyp_daemon_bootstrap_result_t *result) {
    if (connect_result) {
        result->connect_result = *connect_result;
    }
    result->client = client;
    if (probe == HYP_DAEMON_BOOTSTRAP_PROBE_CONNECTED && client) {
        result->status = HYP_DAEMON_BOOTSTRAP_CONNECTED;
        return result->status;
    }
    result->client = NULL;
    if (probe == HYP_DAEMON_BOOTSTRAP_PROBE_CONFLICT) {
        result->status = HYP_DAEMON_BOOTSTRAP_CONFLICT;
        (void)snprintf(result->message, sizeof(result->message), "%s",
                       connect_result && connect_result->message[0]
                           ? connect_result->message
                           : "HYP could not start because a conflicting HYP process is active; "
                             "close all HYP sessions and commands, then retry. If a permanent "
                             "daemon from another build is running, `hyponoia daemon "
                             "stop` retires it");
        if (ops->visible_diagnostic) {
            ops->visible_diagnostic(ops->context, result->message);
            /* Second line, not appended: result->message is the rendezvous
             * wire field and is already sized for two fingerprints. */
            ops->visible_diagnostic(ops->context, hyp_daemon_conflict_escape_hint());
        }
        return result->status;
    }
    return HYP_DAEMON_BOOTSTRAP_FAILED;
}

static hyp_daemon_bootstrap_probe_status_t bootstrap_probe(
    const hyp_daemon_bootstrap_config_t *config, const hyp_daemon_bootstrap_ops_t *ops,
    hyp_daemon_runtime_client_t **client_out, hyp_daemon_runtime_connect_result_t *connect_result) {
    memset(connect_result, 0, sizeof(*connect_result));
    *client_out = NULL;
    return ops->probe(ops->context, config->endpoint, config->identity, config->connect_timeout_ms,
                      client_out, connect_result);
}

static bool bootstrap_probe_is_finishable(hyp_daemon_bootstrap_probe_status_t probe) {
    return probe == HYP_DAEMON_BOOTSTRAP_PROBE_CONNECTED ||
           probe == HYP_DAEMON_BOOTSTRAP_PROBE_CONFLICT;
}

static bool bootstrap_probe_is_waitable(hyp_daemon_bootstrap_probe_status_t probe) {
    return probe == HYP_DAEMON_BOOTSTRAP_PROBE_UNAVAILABLE ||
           probe == HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED ||
           probe == HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL;
}

static bool bootstrap_config_valid(const hyp_daemon_bootstrap_config_t *config,
                                   const hyp_daemon_bootstrap_ops_t *ops) {
    return config && ops && config->endpoint && config->identity && config->executable_path &&
           config->executable_path[0] && config->connect_timeout_ms > 0 &&
           config->startup_timeout_ms > 0 && ops->cohort_acquire && ops->cohort_release &&
           ops->probe && ops->startup_lock_try_acquire && ops->startup_lock_prepare_handoff &&
           ops->startup_lock_release && ops->spawn_daemon;
}

hyp_daemon_bootstrap_status_t hyp_daemon_bootstrap_execute_with_ops(
    const hyp_daemon_bootstrap_config_t *config, const hyp_daemon_bootstrap_ops_t *ops,
    hyp_daemon_bootstrap_result_t *result_out) {
    if (!result_out) {
        return HYP_DAEMON_BOOTSTRAP_FAILED;
    }
    hyp_daemon_process_role_t role = config ? config->role : HYP_DAEMON_PROCESS_INVALID;
    bootstrap_result_reset(result_out, role);
    if (!hyp_daemon_process_role_requires_client(role)) {
        return role == HYP_DAEMON_PROCESS_INVALID ? HYP_DAEMON_BOOTSTRAP_FAILED
                                                  : HYP_DAEMON_BOOTSTRAP_BYPASSED;
    }
    if (!bootstrap_config_valid(config, ops)) {
        return HYP_DAEMON_BOOTSTRAP_FAILED;
    }

    uint64_t deadline = bootstrap_deadline_after(config->startup_timeout_ms);
    hyp_daemon_bootstrap_cohort_t cohort = NULL;
    hyp_daemon_conflict_t cohort_conflict;
    hyp_version_cohort_status_t cohort_status = ops->cohort_acquire(
        ops->context, config->endpoint, config->identity, deadline, &cohort, &cohort_conflict);
    if (cohort_status != HYP_VERSION_COHORT_OK) {
        result_out->status = cohort_status == HYP_VERSION_COHORT_CONFLICT
                                 ? HYP_DAEMON_BOOTSTRAP_CONFLICT
                                 : HYP_DAEMON_BOOTSTRAP_FAILED;
        bool formatted = cohort_status == HYP_VERSION_COHORT_CONFLICT &&
                         hyp_daemon_conflict_format(&cohort_conflict, result_out->message,
                                                    sizeof(result_out->message));
        if (!formatted) {
            const char *reason = cohort_status == HYP_VERSION_COHORT_BUSY
                                     ? "another HYP activation is in progress"
                                     : "exact-build admission could not be verified";
            (void)snprintf(result_out->message, sizeof(result_out->message),
                           "HYP daemon could not start: %s", reason);
        }
        if (ops->visible_diagnostic) {
            ops->visible_diagnostic(ops->context, result_out->message);
            if (cohort_status == HYP_VERSION_COHORT_CONFLICT) {
                /* The cohort guard is the one that actually refuses a second
                 * build, so it is the one that most needs to name its own
                 * escape. Sent as a second line rather than appended:
                 * result_out->message is the rendezvous wire field and is
                 * already sized for two 64-character fingerprints. */
                ops->visible_diagnostic(ops->context, hyp_daemon_conflict_escape_hint());
            }
        }
        if (cohort) {
            ops->cohort_release(ops->context, cohort);
        }
        return result_out->status;
    }

    hyp_daemon_runtime_client_t *client = NULL;
    hyp_daemon_runtime_connect_result_t connect_result;
    hyp_daemon_bootstrap_probe_status_t probe =
        bootstrap_probe(config, ops, &client, &connect_result);
    if (bootstrap_probe_is_finishable(probe)) {
        hyp_daemon_bootstrap_status_t status =
            bootstrap_finish_probe(probe, client, &connect_result, ops, result_out);
        ops->cohort_release(ops->context, cohort);
        return status;
    }
    if (!bootstrap_probe_is_waitable(probe)) {
        probe = HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
    }

    hyp_daemon_bootstrap_lock_t startup_lock = NULL;
    bool lock_acquired = false;
    bool generation_observed = probe == HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED ||
                               probe == HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL;
    while (hyp_now_ms() < deadline) {
        if (!bootstrap_probe_is_waitable(probe)) {
            break;
        }
        if (probe == HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED ||
            probe == HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL) {
            /* A live or stopping generation owns the transition for now. Its
             * disappearance is not sticky: after observing true absence, the
             * same bootstrap attempt may serialize and become the next first
             * client. The startup lock and the re-probe below prevent two
             * replacements from being launched. */
            generation_observed = true;
            bootstrap_pause(deadline);
            probe = bootstrap_probe(config, ops, &client, &connect_result);
            continue;
        }

        int lock_status =
            ops->startup_lock_try_acquire(ops->context, config->endpoint, &startup_lock);
        if (lock_status < 0) {
            probe = HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
            break;
        }
        if (lock_status == 0) {
            bootstrap_pause(deadline);
            probe = bootstrap_probe(config, ops, &client, &connect_result);
            continue;
        }
        if (lock_status != 1 || !startup_lock) {
            probe = HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
            break;
        }

        lock_acquired = true;
        probe = bootstrap_probe(config, ops, &client, &connect_result);
        if (probe == HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED ||
            probe == HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL) {
            generation_observed = true;
            bootstrap_startup_lock_release_complete(ops, &startup_lock);
            lock_acquired = false;
            continue;
        }
        if (bootstrap_probe_is_finishable(probe) || probe == HYP_DAEMON_BOOTSTRAP_PROBE_ERROR) {
            break;
        }
        if (probe != HYP_DAEMON_BOOTSTRAP_PROBE_UNAVAILABLE) {
            probe = HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
            break;
        }

        hyp_daemon_bootstrap_launch_spec_t spec;
        bool spec_ready =
            config->spawn_permanent
                ? hyp_daemon_bootstrap_launch_spec_init_permanent(config->executable_path, &spec)
                : hyp_daemon_bootstrap_launch_spec_init(config->executable_path, &spec);
        if (!spec_ready || !ops->startup_lock_prepare_handoff(ops->context, startup_lock) ||
            !ops->spawn_daemon(ops->context, &spec)) {
            probe = HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
            break;
        }
        result_out->daemon_spawned = true;

        /* Keep startup ownership only until the child becomes observable.
         * Windows participant teardown reacquires the startup transition;
         * retaining it while probing an observable generation can deadlock the
         * bootstrap against a daemon that is trying to cleanly stand down. */
        do {
            bootstrap_pause(deadline);
            probe = bootstrap_probe(config, ops, &client, &connect_result);
            if (probe == HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED ||
                probe == HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL) {
                generation_observed = true;
                bootstrap_startup_lock_release_complete(ops, &startup_lock);
                lock_acquired = false;
                break;
            }
            if (!bootstrap_probe_is_waitable(probe)) {
                break;
            }
        } while (hyp_now_ms() < deadline);
        if (!lock_acquired) {
            continue;
        }
        break;
    }

    if (lock_acquired) {
        bootstrap_startup_lock_release_complete(ops, &startup_lock);
    }
    if (bootstrap_probe_is_finishable(probe)) {
        hyp_daemon_bootstrap_status_t status =
            bootstrap_finish_probe(probe, client, &connect_result, ops, result_out);
        ops->cohort_release(ops->context, cohort);
        return status;
    }

    result_out->status = HYP_DAEMON_BOOTSTRAP_FAILED;
    if (generation_observed) {
        (void)snprintf(result_out->message, sizeof(result_out->message),
                       "HYP daemon is active or starting but could not accept this client "
                       "within %u ms",
                       config->startup_timeout_ms);
    } else {
        (void)snprintf(result_out->message, sizeof(result_out->message),
                       "HYP daemon could not start within %u ms", config->startup_timeout_ms);
    }
    if (ops->visible_diagnostic) {
        ops->visible_diagnostic(ops->context, result_out->message);
    }
    ops->cohort_release(ops->context, cohort);
    return result_out->status;
}

hyp_daemon_bootstrap_probe_status_t hyp_daemon_bootstrap_classify_failed_connect(
    const hyp_daemon_runtime_connect_result_t *connect_result, int lifetime_status) {
    if (!connect_result) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
    }
    if (connect_result->status == HYP_DAEMON_RUNTIME_CONNECT_CONFLICT) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_CONFLICT;
    }
    if (connect_result->status == HYP_DAEMON_RUNTIME_CONNECT_REJECTED) {
        if (strstr(connect_result->message, "stopping") ||
            strstr(connect_result->message, "shutting down")) {
            return HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL;
        }
        /* Capacity, admission, and other protocol-level rejections prove an
         * existing generation answered. Never reinterpret them as absence. */
        return HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED;
    }
    if (lifetime_status == 1) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED;
    }
    if (lifetime_status != 0) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
    }
    return connect_result->status == HYP_DAEMON_RUNTIME_CONNECT_ERROR
               ? HYP_DAEMON_BOOTSTRAP_PROBE_UNAVAILABLE
               : HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
}

typedef struct bootstrap_production_cohort {
    hyp_version_cohort_manager_t *manager;
    hyp_version_cohort_lease_t *lease;
} bootstrap_production_cohort_t;

typedef struct {
    bootstrap_production_cohort_t *cohort;
#ifdef _WIN32
    DWORD spawn_error;
#elif defined(__APPLE__)
    int spawn_error;
#endif
} bootstrap_production_context_t;

static hyp_daemon_bootstrap_probe_status_t bootstrap_production_probe(
    void *context, const hyp_daemon_ipc_endpoint_t *endpoint,
    const hyp_daemon_build_identity_t *identity, uint32_t timeout_ms,
    hyp_daemon_runtime_client_t **client_out, hyp_daemon_runtime_connect_result_t *result_out) {
    bootstrap_production_context_t *production = context;
    if (!production || !production->cohort || !production->cohort->manager) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
    }
    hyp_version_cohort_daemon_presence_t claim =
        hyp_version_cohort_daemon_claim_presence(production->cohort->manager);
    if (claim == HYP_VERSION_COHORT_DAEMON_ABSENT) {
        int lifetime = hyp_daemon_ipc_lifetime_reservation_probe(endpoint);
        if (lifetime == 0) {
            /* Do not spend the per-connect timeout polling a generation that
             * both independent ownership signals prove absent. The startup
             * lock and its mandatory re-probe serialize a concurrent launch. */
            return HYP_DAEMON_BOOTSTRAP_PROBE_UNAVAILABLE;
        }
        if (lifetime != 1) {
            return HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
        }
    } else if (claim != HYP_VERSION_COHORT_DAEMON_COORDINATED) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
    }

    *client_out = hyp_daemon_runtime_client_connect(endpoint, identity, timeout_ms, result_out);
    if (*client_out) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_CONNECTED;
    }

    /* Ownership may turn over while the connection attempt is in flight.
     * Re-observe both signals so disappearance is not sticky and a live or
     * cleaning-up generation is never mistaken for absence. */
    claim = hyp_version_cohort_daemon_claim_presence(production->cohort->manager);
    if (claim == HYP_VERSION_COHORT_DAEMON_COORDINATED) {
        return hyp_daemon_bootstrap_classify_failed_connect(result_out, 1);
    }
    if (claim != HYP_VERSION_COHORT_DAEMON_ABSENT) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_ERROR;
    }
    int lifetime_status = hyp_daemon_ipc_lifetime_reservation_probe(endpoint);
    return hyp_daemon_bootstrap_classify_failed_connect(result_out, lifetime_status);
}

static hyp_version_cohort_status_t bootstrap_production_cohort_acquire(
    void *context, const hyp_daemon_ipc_endpoint_t *endpoint,
    const hyp_daemon_build_identity_t *identity, uint64_t deadline_ms,
    hyp_daemon_bootstrap_cohort_t *cohort_out, hyp_daemon_conflict_t *conflict_out) {
    *cohort_out = NULL;
    bootstrap_production_cohort_t *cohort = calloc(1, sizeof(*cohort));
    if (cohort) {
        cohort->manager = hyp_version_cohort_manager_new(endpoint);
    }
    if (!cohort || !cohort->manager) {
        free(cohort);
        return HYP_VERSION_COHORT_IO;
    }
    hyp_version_cohort_status_t status = hyp_version_cohort_acquire(
        cohort->manager, identity, deadline_ms, &cohort->lease, conflict_out);
    if (status == HYP_VERSION_COHORT_CONFLICT) {
        (void)hyp_version_cohort_log_conflict(conflict_out);
    }
    if (status == HYP_VERSION_COHORT_OK || cohort->lease) {
        *cohort_out = cohort;
        if (context) {
            bootstrap_production_context_t *production = context;
            production->cohort = cohort;
        }
        return status;
    }
    uint64_t cleanup_deadline = bootstrap_deadline_after(BOOTSTRAP_COORDINATION_CLEANUP_MS);
    hyp_private_file_lock_status_t cleanup = HYP_PRIVATE_FILE_LOCK_OK;
    while (cohort->manager) {
        cleanup = hyp_version_cohort_manager_free(&cohort->manager);
        if (!cohort->manager) {
            break;
        }
        if (hyp_now_ms() >= cleanup_deadline) {
            bootstrap_cleanup_fail_stop("cohort_manager_cleanup");
        }
        hyp_usleep(1000);
    }
    free(cohort);
    return cleanup == HYP_PRIVATE_FILE_LOCK_OK ? status : HYP_VERSION_COHORT_IO;
}

static void bootstrap_production_cohort_release(void *context,
                                                hyp_daemon_bootstrap_cohort_t opaque) {
    bootstrap_production_context_t *production = context;
    bootstrap_production_cohort_t *cohort = opaque;
    if (!cohort) {
        return;
    }
    uint64_t cleanup_deadline = bootstrap_deadline_after(BOOTSTRAP_COORDINATION_CLEANUP_MS);
    while (cohort->lease) {
        (void)hyp_version_cohort_lease_release(&cohort->lease);
        if (!cohort->lease) {
            break;
        }
        if (hyp_now_ms() >= cleanup_deadline) {
            bootstrap_cleanup_fail_stop("cohort_lease_cleanup");
        }
        hyp_usleep(1000);
    }
    cleanup_deadline = bootstrap_deadline_after(BOOTSTRAP_COORDINATION_CLEANUP_MS);
    while (cohort->manager) {
        (void)hyp_version_cohort_manager_free(&cohort->manager);
        if (!cohort->manager) {
            break;
        }
        if (hyp_now_ms() >= cleanup_deadline) {
            bootstrap_cleanup_fail_stop("cohort_manager_cleanup");
        }
        hyp_usleep(1000);
    }
    if (production && production->cohort == cohort) {
        production->cohort = NULL;
    }
    free(cohort);
}

static int bootstrap_production_lock(void *context, const hyp_daemon_ipc_endpoint_t *endpoint,
                                     hyp_daemon_bootstrap_lock_t *lock_out) {
    (void)context;
    hyp_daemon_ipc_startup_lock_t *lock = NULL;
    int status = hyp_daemon_ipc_startup_lock_try_acquire(endpoint, &lock);
    *lock_out = lock;
    return status;
}

static bool bootstrap_production_unlock(void *context, hyp_daemon_bootstrap_lock_t *lock_io) {
    (void)context;
    if (!lock_io) {
        return false;
    }
    hyp_daemon_ipc_startup_lock_t *lock = *lock_io;
    bool released = hyp_daemon_ipc_startup_lock_release(&lock);
    *lock_io = lock;
    return released;
}

static bool bootstrap_production_handoff(void *context, hyp_daemon_bootstrap_lock_t lock) {
    (void)context;
    return hyp_daemon_ipc_startup_lock_prepare_handoff((hyp_daemon_ipc_startup_lock_t *)lock);
}

#ifdef _WIN32
static bool bootstrap_production_spawn(void *context,
                                       const hyp_daemon_bootstrap_launch_spec_t *spec) {
    bootstrap_production_context_t *production = context;
    if (production) {
        production->spawn_error = ERROR_SUCCESS;
    }
    if (!spec || !spec->detached || spec->inherit_standard_handles || spec->use_shell) {
        if (production) {
            production->spawn_error = ERROR_INVALID_PARAMETER;
        }
        return false;
    }
    char command_line[BOOTSTRAP_PATH_CAP * 2];
    if (!hyp_build_win_cmdline(command_line, sizeof(command_line), spec->argv)) {
        if (production) {
            production->spawn_error = ERROR_INVALID_PARAMETER;
        }
        return false;
    }
    wchar_t *application = hyp_utf8_to_wide(spec->executable_path);
    wchar_t *command = hyp_utf8_to_wide(command_line);
    if (!application || !command) {
        if (production) {
            production->spawn_error = ERROR_NOT_ENOUGH_MEMORY;
        }
        free(application);
        free(command);
        return false;
    }
    STARTUPINFOW startup;
    PROCESS_INFORMATION child;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&child, sizeof(child));
    startup.cb = sizeof(startup);
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
    /* A managed frontend payload is intentionally contained in the permanent
     * launcher's kill-on-close job. The account daemon outlives that one
     * frontend and therefore uses breakaway when the containing job explicitly
     * permits it. Do not request breakaway from an unrelated restrictive job:
     * CreateProcess would fail and regress portable/package-manager payloads. */
    BOOL in_job = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
    memset(&job_limits, 0, sizeof(job_limits));
    if (IsProcessInJob(GetCurrentProcess(), NULL, &in_job) && in_job &&
        QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &job_limits,
                                  sizeof(job_limits), NULL) &&
        (job_limits.BasicLimitInformation.LimitFlags &
         (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) != 0) {
        flags |= CREATE_BREAKAWAY_FROM_JOB;
    }
    BOOL created = CreateProcessW(application, command, NULL, NULL, FALSE, flags, NULL, NULL,
                                  &startup, &child);
    DWORD spawn_error = created ? ERROR_SUCCESS : GetLastError();
    free(application);
    free(command);
    if (production) {
        production->spawn_error = spawn_error;
    }
    if (!created) {
        return false;
    }
    (void)CloseHandle(child.hThread);
    (void)CloseHandle(child.hProcess);
    return true;
}
#elif defined(__APPLE__)
static bool bootstrap_darwin_spawn_state_init(posix_spawn_file_actions_t *actions,
                                              posix_spawnattr_t *attributes) {
    if (posix_spawn_file_actions_init(actions) != 0) {
        return false;
    }
    bool actions_ready =
        posix_spawn_file_actions_addopen(actions, STDIN_FILENO, "/dev/null", O_RDWR, 0) == 0 &&
        posix_spawn_file_actions_addopen(actions, STDOUT_FILENO, "/dev/null", O_RDWR, 0) == 0 &&
        posix_spawn_file_actions_addopen(actions, STDERR_FILENO, "/dev/null", O_RDWR, 0) == 0;
    if (!actions_ready) {
        (void)posix_spawn_file_actions_destroy(actions);
        return false;
    }
    if (posix_spawnattr_init(attributes) != 0) {
        (void)posix_spawn_file_actions_destroy(actions);
        return false;
    }
    sigset_t empty;
    (void)sigemptyset(&empty);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT;
    if (posix_spawnattr_setsigmask(attributes, &empty) != 0 ||
        posix_spawnattr_setflags(attributes, flags) != 0) {
        (void)posix_spawnattr_destroy(attributes);
        (void)posix_spawn_file_actions_destroy(actions);
        return false;
    }
    return true;
}

typedef struct {
    pid_t pid;
} bootstrap_darwin_reaper_t;

static void *bootstrap_darwin_reap(void *opaque) {
    bootstrap_darwin_reaper_t *reaper = opaque;
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(reaper->pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    free(reaper);
    return NULL;
}

static bool bootstrap_darwin_reaper_start(pid_t daemon) {
    bootstrap_darwin_reaper_t *reaper = malloc(sizeof(*reaper));
    if (!reaper) {
        return false;
    }
    reaper->pid = daemon;
    pthread_attr_t attributes;
    bool attributes_ready = pthread_attr_init(&attributes) == 0;
    bool detached =
        attributes_ready && pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0;
    pthread_t thread;
    int thread_status =
        detached ? pthread_create(&thread, &attributes, bootstrap_darwin_reap, reaper) : -1;
    if (attributes_ready) {
        (void)pthread_attr_destroy(&attributes);
    }
    if (thread_status != 0) {
        free(reaper);
        return false;
    }
    return true;
}

static bool bootstrap_production_spawn(void *context,
                                       const hyp_daemon_bootstrap_launch_spec_t *spec) {
    bootstrap_production_context_t *production = context;
    if (production) {
        production->spawn_error = 0;
    }
    if (!spec || !spec->detached || spec->inherit_standard_handles || spec->use_shell) {
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (!bootstrap_darwin_spawn_state_init(&actions, &attributes)) {
        return false;
    }
    /* Calling library-heavy process creation between fork() and exec is not a
     * safe boundary once another thread may own libc state. Darwin's spawn
     * primitive can create the detached session and close inherited FDs in
     * one operation, so launch it from the original process and retain only a
     * tiny detached waiter to prevent a crashed daemon from becoming a
     * long-lived zombie owned by the first frontend. */
    pid_t daemon = 0;
    int spawn_status = posix_spawn(&daemon, spec->executable_path, &actions, &attributes,
                                   (char *const *)spec->argv, environ);
    (void)posix_spawnattr_destroy(&attributes);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (production) {
        production->spawn_error = spawn_status;
    }
    if (spawn_status != 0) {
        return false;
    }
    if (!bootstrap_darwin_reaper_start(daemon)) {
        (void)kill(daemon, SIGKILL);
        int status = 0;
        while (waitpid(daemon, &status, 0) < 0 && errno == EINTR) {}
        if (production) {
            production->spawn_error = EAGAIN;
        }
        return false;
    }
    return true;
}
#else
static void bootstrap_child_close_fds(void) {
    long open_max = sysconf(_SC_OPEN_MAX);
    if (open_max < 0 || open_max > 1048576L) {
        open_max = 65536L;
    }
    for (int fd = 3; fd < open_max; fd++) {
        (void)close(fd);
    }
}

static void bootstrap_daemon_grandchild(const hyp_daemon_bootstrap_launch_spec_t *spec) {
    (void)umask(077);
    sigset_t empty;
    (void)sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0) {
        _exit(127);
    }
    if (null_fd > STDERR_FILENO) {
        (void)close(null_fd);
    }
    bootstrap_child_close_fds();
    execv(spec->executable_path, (char *const *)spec->argv);
    _exit(127);
}

static bool bootstrap_production_spawn(void *context,
                                       const hyp_daemon_bootstrap_launch_spec_t *spec) {
    (void)context;
    if (!spec || !spec->detached || spec->inherit_standard_handles || spec->use_shell) {
        return false;
    }
    pid_t first = fork();
    if (first < 0) {
        return false;
    }
    if (first == 0) {
        if (setsid() < 0) {
            _exit(127);
        }
        pid_t daemon = fork();
        if (daemon < 0) {
            _exit(127);
        }
        if (daemon > 0) {
            _exit(0);
        }
        bootstrap_daemon_grandchild(spec);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(first, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == first && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

static void bootstrap_production_diagnostic(void *context, const char *message) {
    bootstrap_production_context_t *production = context;
#ifdef _WIN32
    if (production && production->spawn_error != ERROR_SUCCESS) {
        (void)fprintf(stderr, "hyponoia: %s (daemon launch error %lu)\n",
                      message ? message : "daemon startup failed",
                      (unsigned long)production->spawn_error);
        (void)fflush(stderr);
        return;
    }
#elif defined(__APPLE__)
    if (production && production->spawn_error != 0) {
        (void)fprintf(stderr, "hyponoia: %s (daemon launch: %s)\n",
                      message ? message : "daemon startup failed",
                      strerror(production->spawn_error));
        (void)fflush(stderr);
        return;
    }
#else
    (void)production;
#endif
    (void)fprintf(stderr, "hyponoia: %s\n", message ? message : "daemon startup failed");
    (void)fflush(stderr);
}

hyp_daemon_bootstrap_status_t hyp_daemon_bootstrap_execute(
    const hyp_daemon_bootstrap_config_t *config, hyp_daemon_bootstrap_result_t *result_out) {
    bootstrap_production_context_t context = {0};
    const hyp_daemon_bootstrap_ops_t ops = {
        .context = &context,
        .cohort_acquire = bootstrap_production_cohort_acquire,
        .cohort_release = bootstrap_production_cohort_release,
        .probe = bootstrap_production_probe,
        .startup_lock_try_acquire = bootstrap_production_lock,
        .startup_lock_prepare_handoff = bootstrap_production_handoff,
        .startup_lock_release = bootstrap_production_unlock,
        .spawn_daemon = bootstrap_production_spawn,
        .visible_diagnostic = bootstrap_production_diagnostic,
    };
    return hyp_daemon_bootstrap_execute_with_ops(config, &ops, result_out);
}
