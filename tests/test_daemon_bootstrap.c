/* RED contract for early process-role classification. */
#include "test_framework.h"
#include "test_helpers.h"

#include "daemon/bootstrap.h"
#include "daemon/ipc.h"
#include "daemon/service.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/platform.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/un.h>
#endif

enum {
    BOOTSTRAP_TEST_PATH_CAP = 1024,
    BOOTSTRAP_TEST_TIMEOUT_MS = 2000,
    BOOTSTRAP_TEST_SHORT_TIMEOUT_MS = 20,
};

static const char BOOTSTRAP_BUILD_A[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char BOOTSTRAP_BUILD_B[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

typedef struct {
    char parent[BOOTSTRAP_TEST_PATH_CAP];
    char runtime_dir[BOOTSTRAP_TEST_PATH_CAP];
    hyp_daemon_ipc_endpoint_t *endpoint;
} bootstrap_endpoint_fixture_t;

typedef struct {
    atomic_int cohort_acquire_count;
    atomic_int cohort_release_count;
    atomic_int lock_held;
    atomic_int spawn_count;
    atomic_int probe_count;
    atomic_int lock_attempt_count;
    atomic_int handoff_count;
    atomic_int diagnostic_count;
    atomic_int reserved_probes_remaining;
    atomic_int terminal_probes_remaining;
    atomic_bool available;
    atomic_bool connect_after_reserved;
    atomic_bool connect_requires_unlocked;
    hyp_daemon_bootstrap_probe_status_t forced_probe;
    hyp_version_cohort_status_t forced_cohort;
    /* Holds the conflict text plus the escape line that follows it. */
    char diagnostic[HYP_DAEMON_CONFLICT_MESSAGE_SIZE * 2U];
} bootstrap_fake_ops_t;

typedef struct {
    const hyp_daemon_bootstrap_config_t *config;
    const hyp_daemon_bootstrap_ops_t *ops;
    atomic_int *ready;
    atomic_bool *go;
    hyp_daemon_bootstrap_result_t result;
    hyp_daemon_bootstrap_status_t status;
} bootstrap_thread_call_t;

static hyp_daemon_build_identity_t bootstrap_identity(const char *version, const char *build) {
    hyp_daemon_build_identity_t identity = {
        .semantic_version = version,
        .build_fingerprint = build,
        .cache_fingerprint = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        .protocol_abi = 3,
        .store_abi = 11,
        .feature_abi = 7,
    };
    return identity;
}

static bool bootstrap_endpoint_fixture_start(bootstrap_endpoint_fixture_t *fixture,
                                             const char *tag) {
    memset(fixture, 0, sizeof(*fixture));
    int written = snprintf(fixture->parent, sizeof(fixture->parent), "%s/hyp-bootstrap-%s-XXXXXX",
                           hyp_tmpdir(), tag);
    if (written <= 0 || written >= (int)sizeof(fixture->parent) || !hyp_mkdtemp(fixture->parent)) {
        return false;
    }
    fixture->endpoint = hyp_daemon_bootstrap_endpoint_new(fixture->parent);
    const char *runtime_dir =
        fixture->endpoint ? hyp_daemon_ipc_endpoint_runtime_dir(fixture->endpoint) : NULL;
    if (!runtime_dir) {
        return false;
    }
    written = snprintf(fixture->runtime_dir, sizeof(fixture->runtime_dir), "%s", runtime_dir);
    return written > 0 && written < (int)sizeof(fixture->runtime_dir);
}

static void bootstrap_endpoint_fixture_finish(bootstrap_endpoint_fixture_t *fixture) {
    hyp_daemon_ipc_endpoint_free(fixture->endpoint);
    if (fixture->runtime_dir[0] != '\0') {
        (void)hyp_rmdir(fixture->runtime_dir);
    }
    if (fixture->parent[0] != '\0') {
        (void)hyp_rmdir(fixture->parent);
    }
    memset(fixture, 0, sizeof(*fixture));
}

static hyp_daemon_bootstrap_probe_status_t bootstrap_fake_probe(
    void *opaque, const hyp_daemon_ipc_endpoint_t *endpoint,
    const hyp_daemon_build_identity_t *identity, uint32_t timeout_ms,
    hyp_daemon_runtime_client_t **client_out, hyp_daemon_runtime_connect_result_t *result_out) {
    (void)endpoint;
    (void)identity;
    (void)timeout_ms;
    bootstrap_fake_ops_t *fake = opaque;
    atomic_fetch_add(&fake->probe_count, 1);
    memset(result_out, 0, sizeof(*result_out));
    *client_out = NULL;
    int reserved_remaining = atomic_load(&fake->reserved_probes_remaining);
    while (reserved_remaining > 0 &&
           !atomic_compare_exchange_weak(&fake->reserved_probes_remaining, &reserved_remaining,
                                         reserved_remaining - 1)) {}
    if (reserved_remaining > 0) {
        if (reserved_remaining == 1 && atomic_load(&fake->connect_after_reserved)) {
            atomic_store(&fake->available, true);
        }
        return HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED;
    }
    int terminal_remaining = atomic_load(&fake->terminal_probes_remaining);
    while (terminal_remaining > 0 &&
           !atomic_compare_exchange_weak(&fake->terminal_probes_remaining, &terminal_remaining,
                                         terminal_remaining - 1)) {}
    if (terminal_remaining > 0) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL;
    }
    if (fake->forced_probe == HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED) {
        return fake->forced_probe;
    }
    if (fake->forced_probe == HYP_DAEMON_BOOTSTRAP_PROBE_CONFLICT) {
        result_out->status = HYP_DAEMON_RUNTIME_CONNECT_CONFLICT;
        result_out->hello_status = HYP_DAEMON_HELLO_BUILD_CONFLICT;
        snprintf(result_out->message, sizeof(result_out->message),
                 "HYP daemon could not start: conflicting versions");
        return fake->forced_probe;
    }
    if (fake->forced_probe == HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL) {
        return fake->forced_probe;
    }
    if (atomic_load(&fake->available) && atomic_load(&fake->connect_requires_unlocked) &&
        atomic_load(&fake->lock_held) != 0) {
        return HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED;
    }
    if (atomic_load(&fake->available)) {
        result_out->status = HYP_DAEMON_RUNTIME_CONNECT_ACCEPTED;
        result_out->hello_status = HYP_DAEMON_HELLO_COMPATIBLE;
        result_out->client_id = 1;
        *client_out = (hyp_daemon_runtime_client_t *)(uintptr_t)1;
        return HYP_DAEMON_BOOTSTRAP_PROBE_CONNECTED;
    }
    return HYP_DAEMON_BOOTSTRAP_PROBE_UNAVAILABLE;
}

static hyp_version_cohort_status_t bootstrap_fake_cohort_acquire(
    void *opaque, const hyp_daemon_ipc_endpoint_t *endpoint,
    const hyp_daemon_build_identity_t *identity, uint64_t deadline_ms,
    hyp_daemon_bootstrap_cohort_t *cohort_out, hyp_daemon_conflict_t *conflict_out) {
    (void)endpoint;
    (void)deadline_ms;
    bootstrap_fake_ops_t *fake = opaque;
    atomic_fetch_add(&fake->cohort_acquire_count, 1);
    *cohort_out = NULL;
    memset(conflict_out, 0, sizeof(*conflict_out));
    if (fake->forced_cohort == HYP_VERSION_COHORT_CONFLICT) {
        hyp_daemon_build_identity_t active = bootstrap_identity("2.3.0", BOOTSTRAP_BUILD_A);
        (void)hyp_daemon_hello_compare(&active, identity, conflict_out);
        return fake->forced_cohort;
    }
    if (fake->forced_cohort != HYP_VERSION_COHORT_OK) {
        return fake->forced_cohort;
    }
    *cohort_out = fake;
    return HYP_VERSION_COHORT_OK;
}

static void bootstrap_fake_cohort_release(void *opaque, hyp_daemon_bootstrap_cohort_t cohort) {
    bootstrap_fake_ops_t *fake = opaque;
    if (cohort == fake) {
        atomic_fetch_add(&fake->cohort_release_count, 1);
    }
}

static int bootstrap_fake_lock(void *opaque, const hyp_daemon_ipc_endpoint_t *endpoint,
                               hyp_daemon_bootstrap_lock_t *lock_out) {
    (void)endpoint;
    bootstrap_fake_ops_t *fake = opaque;
    atomic_fetch_add(&fake->lock_attempt_count, 1);
    int expected = 0;
    if (!atomic_compare_exchange_strong(&fake->lock_held, &expected, 1)) {
        return 0;
    }
    *lock_out = fake;
    return 1;
}

static bool bootstrap_fake_unlock(void *opaque, hyp_daemon_bootstrap_lock_t *lock_io) {
    bootstrap_fake_ops_t *fake = opaque;
    if (lock_io && *lock_io == fake) {
        atomic_store(&fake->lock_held, 0);
        *lock_io = NULL;
        return true;
    }
    return lock_io && !*lock_io;
}

static bool bootstrap_fake_handoff(void *opaque, hyp_daemon_bootstrap_lock_t lock) {
    bootstrap_fake_ops_t *fake = opaque;
    if (lock != fake || atomic_load(&fake->lock_held) != 1) {
        return false;
    }
    atomic_fetch_add(&fake->handoff_count, 1);
    return true;
}

static bool bootstrap_fake_spawn(void *opaque, const hyp_daemon_bootstrap_launch_spec_t *spec) {
    bootstrap_fake_ops_t *fake = opaque;
    /* Client bootstrap must only ever spawn the EPHEMERAL two-argument
     * shape; the permanent shape belongs exclusively to `daemon start`. */
    bool exact = spec && spec->argc == 2U && spec->argv[0] &&
                 spec->argv[1] && !spec->argv[2] &&
                 strcmp(spec->argv[1], HYP_DAEMON_INTERNAL_ARG) == 0 && spec->detached &&
                 !spec->inherit_standard_handles && !spec->use_shell &&
                 atomic_load(&fake->handoff_count) > 0 && atomic_load(&fake->lock_held) == 1;
    if (!exact) {
        return false;
    }
    atomic_fetch_add(&fake->spawn_count, 1);
    atomic_store(&fake->available, true);
    return true;
}

/* Accumulates rather than overwrites: a conflict is now reported as the
 * refusal followed by the supported escape, and asserting on only the last
 * line would hide whichever of the two regressed. */
static void bootstrap_fake_diagnostic(void *opaque, const char *message) {
    bootstrap_fake_ops_t *fake = opaque;
    atomic_fetch_add(&fake->diagnostic_count, 1);
    size_t used = strlen(fake->diagnostic);
    (void)snprintf(fake->diagnostic + used, sizeof(fake->diagnostic) - used, "%s%s",
                   used ? "\n" : "", message ? message : "");
}

static hyp_daemon_bootstrap_ops_t bootstrap_fake_callbacks(bootstrap_fake_ops_t *fake) {
    hyp_daemon_bootstrap_ops_t ops = {
        .context = fake,
        .cohort_acquire = bootstrap_fake_cohort_acquire,
        .cohort_release = bootstrap_fake_cohort_release,
        .probe = bootstrap_fake_probe,
        .startup_lock_try_acquire = bootstrap_fake_lock,
        .startup_lock_prepare_handoff = bootstrap_fake_handoff,
        .startup_lock_release = bootstrap_fake_unlock,
        .spawn_daemon = bootstrap_fake_spawn,
        .visible_diagnostic = bootstrap_fake_diagnostic,
    };
    return ops;
}

static void *bootstrap_thread_execute(void *opaque) {
    bootstrap_thread_call_t *call = opaque;
    atomic_fetch_add(call->ready, 1);
    while (!atomic_load(call->go)) {
        const struct timespec pause = {0, 1000000L};
        (void)hyp_nanosleep(&pause, NULL);
    }
    call->status = hyp_daemon_bootstrap_execute_with_ops(call->config, call->ops, &call->result);
    return NULL;
}

static hyp_daemon_process_role_t classify(int argc, char **argv) {
    return hyp_daemon_process_role(argc, argv);
}

TEST(daemon_bootstrap_classifies_default_and_ui_as_mcp_clients) {
    char *plain[] = {"hyponoia", NULL};
    char *ui[] = {"hyponoia", "--ui=true", "--port=9750", NULL};
    ASSERT_EQ(classify(1, plain), HYP_DAEMON_PROCESS_MCP_CLIENT);
    ASSERT_EQ(classify(3, ui), HYP_DAEMON_PROCESS_MCP_CLIENT);
    ASSERT_TRUE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_MCP_CLIENT));
    PASS();
}

TEST(daemon_bootstrap_classifies_stateless_commands_without_client) {
    char *version[] = {"hyponoia", "--version", NULL};
    char *help[] = {"hyponoia", "--profile", "--help", NULL};
    char *install[] = {"hyponoia", "install", "--dry-run", NULL};
    char *uninstall[] = {"hyponoia", "uninstall", NULL};
    char *update[] = {"hyponoia", "update", "-n", NULL};
    char *verify_runtime_assets[] = {"hyponoia", "--verify-runtime-assets", NULL};
    /* fetch-model writes one file into the user's cache. Unlisted, it would
     * fall through to MCP_CLIENT and BLOCK serving the protocol on stdin
     * instead of downloading anything. */
    char *fetch_model[] = {"hyponoia", "fetch-model", "--path", NULL};
    ASSERT_EQ(classify(2, version), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(classify(3, help), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(classify(3, install), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(classify(2, uninstall), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(classify(3, update), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(classify(2, verify_runtime_assets), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(classify(3, fetch_model), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_FALSE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_STATELESS));
    PASS();
}

TEST(daemon_bootstrap_classifies_config_as_coordinated_local_cli) {
    char *list[] = {"hyponoia", "config", "list", NULL};
    char *set[] = {"hyponoia", "config", "set", "auto_watch", "false", NULL};
    char *help[] = {"hyponoia", "config", "--help", NULL};
    ASSERT_EQ(classify(3, list), HYP_DAEMON_PROCESS_LOCAL_CLI);
    ASSERT_EQ(classify(5, set), HYP_DAEMON_PROCESS_LOCAL_CLI);
    ASSERT_EQ(classify(3, help), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_FALSE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_LOCAL_CLI));
    PASS();
}

TEST(daemon_bootstrap_cli_help_is_stateless_but_tool_calls_are_local) {
    char *tool_help[] = {"hyponoia", "cli", "search_graph", "--help", NULL};
    char *tool_call[] = {"hyponoia", "cli", "search_graph", "{}", NULL};
    ASSERT_EQ(classify(4, tool_help), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(classify(4, tool_call), HYP_DAEMON_PROCESS_LOCAL_CLI);
    ASSERT_FALSE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_LOCAL_CLI));
    PASS();
}

TEST(daemon_bootstrap_cli_arguments_cannot_reclassify_the_process) {
    char *install_value[] = {
        "hyponoia", "cli", "search_code", "--query", "install", NULL};
    char *version_value[] = {"hyponoia", "cli", "search_code", "--query",
                             "--version",           NULL};
    ASSERT_EQ(classify(5, install_value), HYP_DAEMON_PROCESS_LOCAL_CLI);
    ASSERT_EQ(classify(5, version_value), HYP_DAEMON_PROCESS_LOCAL_CLI);
    PASS();
}

TEST(daemon_bootstrap_internal_roles_never_take_client_leases) {
    static char build[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char *daemon[] = {"hyponoia", HYP_DAEMON_INTERNAL_ARG, NULL};
    char *worker[] = {"hyponoia", "cli", "--index-worker", "--index-worker-build", build,
                      "index_repository",    "{}",  "--response-out", "/tmp/response",        NULL};
    char *malformed_worker[] = {"hyponoia", "cli", "--index-worker",
                                "index_repository",    "{}",  NULL};
    char *reserved_user_value[] = {"hyponoia", "cli", "search_code", "--query",
                                   "--index-worker",      NULL};
    char *hook[] = {"hyponoia", "hook-augment", NULL};
    ASSERT_EQ(classify(2, daemon), HYP_DAEMON_PROCESS_DAEMON);
    ASSERT_EQ(classify(9, worker), HYP_DAEMON_PROCESS_WORKER);
    ASSERT_EQ(classify(5, malformed_worker), HYP_DAEMON_PROCESS_INVALID);
    ASSERT_EQ(classify(5, reserved_user_value), HYP_DAEMON_PROCESS_INVALID);
    ASSERT_EQ(classify(2, hook), HYP_DAEMON_PROCESS_HOOK_CLIENT);
    ASSERT_FALSE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_DAEMON));
    ASSERT_FALSE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_WORKER));
    ASSERT_TRUE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_HOOK_CLIENT));
    PASS();
}

TEST(daemon_bootstrap_rejects_ambiguous_internal_daemon_argv) {
    char *missing[] = {NULL};
    char *mixed[] = {"hyponoia", HYP_DAEMON_INTERNAL_ARG, "cli", NULL};
    ASSERT_EQ(classify(0, missing), HYP_DAEMON_PROCESS_INVALID);
    ASSERT_EQ(classify(3, mixed), HYP_DAEMON_PROCESS_INVALID);
    ASSERT_FALSE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_INVALID));
    PASS();
}

TEST(daemon_bootstrap_uses_one_stable_per_account_endpoint) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "stable-endpoint"));
    hyp_daemon_ipc_endpoint_t *second = hyp_daemon_bootstrap_endpoint_new(fixture.parent);
    ASSERT_NOT_NULL(second);
    ASSERT_STR_EQ(hyp_daemon_ipc_endpoint_address(fixture.endpoint),
                  hyp_daemon_ipc_endpoint_address(second));
    hyp_daemon_ipc_endpoint_free(second);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

TEST(daemon_bootstrap_launches_only_exact_detached_hidden_role) {
    hyp_daemon_bootstrap_launch_spec_t spec;
    ASSERT_TRUE(hyp_daemon_bootstrap_launch_spec_init("/tmp/hyp exact", &spec));
    ASSERT_EQ(spec.argc, 2U);
    ASSERT_STR_EQ(spec.executable_path, "/tmp/hyp exact");
    ASSERT_STR_EQ(spec.argv[0], "/tmp/hyp exact");
    ASSERT_STR_EQ(spec.argv[1], HYP_DAEMON_INTERNAL_ARG);
    ASSERT_NULL(spec.argv[2]);
    ASSERT_TRUE(spec.detached);
    ASSERT_FALSE(spec.inherit_standard_handles);
    ASSERT_FALSE(spec.use_shell);
    PASS();
}

TEST(daemon_bootstrap_permanent_daemon_argv_is_byte_exact) {
    char *permanent[] = {"hyponoia", HYP_DAEMON_INTERNAL_ARG, HYP_DAEMON_PERMANENT_ARG,
                         NULL};
    char *reordered[] = {"hyponoia", HYP_DAEMON_PERMANENT_ARG, HYP_DAEMON_INTERNAL_ARG,
                         NULL};
    char *repeated[] = {"hyponoia", HYP_DAEMON_INTERNAL_ARG, HYP_DAEMON_INTERNAL_ARG,
                        NULL};
    char *extended[] = {"hyponoia", HYP_DAEMON_INTERNAL_ARG, HYP_DAEMON_PERMANENT_ARG,
                        "extra", NULL};
    char *wrong_flag[] = {"hyponoia", HYP_DAEMON_INTERNAL_ARG, "--permanent", NULL};
    ASSERT_EQ(classify(3, permanent), HYP_DAEMON_PROCESS_DAEMON);
    ASSERT_EQ(classify(3, reordered), HYP_DAEMON_PROCESS_INVALID);
    ASSERT_EQ(classify(3, repeated), HYP_DAEMON_PROCESS_INVALID);
    ASSERT_EQ(classify(4, extended), HYP_DAEMON_PROCESS_INVALID);
    ASSERT_EQ(classify(3, wrong_flag), HYP_DAEMON_PROCESS_INVALID);
    PASS();
}

TEST(daemon_bootstrap_daemon_ctl_token_routes_after_cli) {
    char *start[] = {"hyponoia", "daemon", "start", NULL};
    char *stop[] = {"hyponoia", "daemon", "stop", NULL};
    char *status[] = {"hyponoia", "daemon", "status", NULL};
    char *help[] = {"hyponoia", "daemon", "--help", NULL};
    /* `daemon` after `cli` is opaque tool input, never a control command. */
    char *opaque[] = {"hyponoia", "cli", "search_code", "daemon", "start", NULL};
    ASSERT_EQ(classify(3, start), HYP_DAEMON_PROCESS_DAEMON_CTL);
    ASSERT_EQ(classify(3, stop), HYP_DAEMON_PROCESS_DAEMON_CTL);
    ASSERT_EQ(classify(3, status), HYP_DAEMON_PROCESS_DAEMON_CTL);
    ASSERT_EQ(classify(3, help), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(classify(5, opaque), HYP_DAEMON_PROCESS_LOCAL_CLI);
    ASSERT_FALSE(hyp_daemon_process_role_requires_client(HYP_DAEMON_PROCESS_DAEMON_CTL));
    PASS();
}

TEST(daemon_bootstrap_permanent_launch_spec_is_exact) {
    hyp_daemon_bootstrap_launch_spec_t spec;
    ASSERT_TRUE(hyp_daemon_bootstrap_launch_spec_init_permanent("/tmp/hyp exact", &spec));
    ASSERT_EQ(spec.argc, 3U);
    ASSERT_STR_EQ(spec.argv[0], "/tmp/hyp exact");
    ASSERT_STR_EQ(spec.argv[1], HYP_DAEMON_INTERNAL_ARG);
    ASSERT_STR_EQ(spec.argv[2], HYP_DAEMON_PERMANENT_ARG);
    ASSERT_NULL(spec.argv[3]);
    ASSERT_TRUE(spec.detached);
    ASSERT_FALSE(spec.inherit_standard_handles);
    ASSERT_FALSE(spec.use_shell);
    PASS();
}

TEST(daemon_bootstrap_stateless_roles_bypass_every_daemon_operation) {
    bootstrap_fake_ops_t fake = {0};
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_STATELESS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_BYPASSED);
    ASSERT_EQ(result.status, HYP_DAEMON_BOOTSTRAP_BYPASSED);
    ASSERT_EQ(atomic_load(&fake.cohort_acquire_count), 0);
    ASSERT_EQ(atomic_load(&fake.probe_count), 0);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 0);
    ASSERT_EQ(atomic_load(&fake.diagnostic_count), 0);
    PASS();
}

TEST(daemon_bootstrap_cohort_conflict_is_visible_before_probe_or_spawn) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "cohort-conflict"));
    bootstrap_fake_ops_t fake = {0};
    fake.forced_cohort = HYP_VERSION_COHORT_CONFLICT;
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_B);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 50,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_CONFLICT);
    ASSERT_EQ(result.status, HYP_DAEMON_BOOTSTRAP_CONFLICT);
    ASSERT_EQ(atomic_load(&fake.cohort_acquire_count), 1);
    ASSERT_EQ(atomic_load(&fake.cohort_release_count), 0);
    ASSERT_EQ(atomic_load(&fake.probe_count), 0);
    ASSERT_EQ(atomic_load(&fake.lock_attempt_count), 0);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 0);
    ASSERT_EQ(atomic_load(&fake.diagnostic_count), 2);
    ASSERT_NOT_NULL(strstr(fake.diagnostic, "conflicting HYP process is active"));
    ASSERT_NOT_NULL(strstr(fake.diagnostic, "Close all HYP sessions and commands"));
    /* A refusal that does not name its own escape is what made six agents
     * rebuild with test seams; the escape is part of the diagnostic now. */
    ASSERT_NOT_NULL(strstr(fake.diagnostic, "HYP_DAEMON_RUNTIME_PARENT"));
    ASSERT_NOT_NULL(strstr(fake.diagnostic, "HYP_CACHE_DIR"));
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

TEST(daemon_bootstrap_existing_exact_daemon_connects_without_spawn) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "existing"));
    bootstrap_fake_ops_t fake = {0};
    atomic_store(&fake.available, true);
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_A);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 50,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_EQ(result.status, HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_NOT_NULL(result.client);
    ASSERT_FALSE(result.daemon_spawned);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 0);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

TEST(daemon_bootstrap_conflict_is_visible_and_never_spawns) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "conflict"));
    bootstrap_fake_ops_t fake = {0};
    fake.forced_probe = HYP_DAEMON_BOOTSTRAP_PROBE_CONFLICT;
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_B);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 50,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_CONFLICT);
    ASSERT_EQ(result.status, HYP_DAEMON_BOOTSTRAP_CONFLICT);
    ASSERT_NULL(result.client);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 0);
    ASSERT_EQ(atomic_load(&fake.diagnostic_count), 2);
    ASSERT_NOT_NULL(strstr(fake.diagnostic, "conflicting versions"));
    ASSERT_NOT_NULL(strstr(fake.diagnostic, "HYP_DAEMON_RUNTIME_PARENT"));
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

TEST(daemon_bootstrap_terminal_generation_that_never_exits_is_not_replaced) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "terminal"));
    bootstrap_fake_ops_t fake = {0};
    fake.forced_probe = HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL;
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_A);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_HOOK_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 1,
        .startup_timeout_ms = BOOTSTRAP_TEST_SHORT_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_FAILED);
    ASSERT_EQ(atomic_load(&fake.lock_held), 0);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 0);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

/* RED for final-session/new-session overlap: STOPPING is a temporary state of
 * the previous same-build generation. Once that generation disappears, the
 * already-running bootstrap attempt must serialize and become the new first
 * client instead of forcing the coding agent to restart its MCP process. */
TEST(daemon_bootstrap_terminal_then_absent_spawns_replacement) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "terminal-absent"));
    bootstrap_fake_ops_t fake = {0};
    atomic_store(&fake.terminal_probes_remaining, 1);
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_A);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 1,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_TRUE(result.daemon_spawned);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 1);
    ASSERT(atomic_load(&fake.lock_attempt_count) >= 1);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

TEST(daemon_bootstrap_reserved_generation_becomes_connectable_without_spawn) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "reserved-connect"));
    bootstrap_fake_ops_t fake = {0};
    atomic_store(&fake.reserved_probes_remaining, 2);
    atomic_store(&fake.connect_after_reserved, true);
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_A);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 1,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_EQ(result.status, HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_FALSE(result.daemon_spawned);
    ASSERT_EQ(atomic_load(&fake.lock_attempt_count), 0);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 0);
    ASSERT(atomic_load(&fake.probe_count) >= 3);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

/* RED for the equivalent race when the old listener vanishes without first
 * returning the explicit STOPPING response. Startup serialization decides
 * whether absence is now safe; a historical RESERVED sample is not sticky. */
TEST(daemon_bootstrap_reserved_then_absent_spawns_replacement) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "reserved-absent"));
    bootstrap_fake_ops_t fake = {0};
    atomic_store(&fake.reserved_probes_remaining, 1);
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_A);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 1,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_EQ(result.status, HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_TRUE(result.daemon_spawned);
    ASSERT(atomic_load(&fake.lock_attempt_count) >= 1);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 1);
    ASSERT(atomic_load(&fake.probe_count) > 1);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

/* RED for the native Windows lock order: after spawn, the generation claim or
 * lifetime reservation becomes visible before the client can connect. Daemon
 * participant teardown needs startup ownership, so bootstrap must release its
 * handoff once that generation is observable. */
TEST(daemon_bootstrap_releases_handoff_when_spawned_generation_is_reserved) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "spawn-admission"));
    bootstrap_fake_ops_t fake = {0};
    atomic_store(&fake.connect_requires_unlocked, true);
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_A);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 1,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    ASSERT_EQ(hyp_daemon_bootstrap_execute_with_ops(&config, &ops, &result),
              HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_EQ(result.status, HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_TRUE(result.daemon_spawned);
    ASSERT_NOT_NULL(result.client);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 1);
    ASSERT_EQ(atomic_load(&fake.lock_held), 0);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

TEST(daemon_bootstrap_rejected_connect_is_reserved_and_never_unavailable) {
    hyp_daemon_runtime_connect_result_t capacity = {0};
    capacity.status = HYP_DAEMON_RUNTIME_CONNECT_REJECTED;
    snprintf(capacity.message, sizeof(capacity.message), "HYP daemon connection capacity reached");
    ASSERT_EQ(hyp_daemon_bootstrap_classify_failed_connect(&capacity, 1),
              HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED);
    ASSERT_EQ(hyp_daemon_bootstrap_classify_failed_connect(&capacity, 0),
              HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED);
    ASSERT_EQ(hyp_daemon_bootstrap_classify_failed_connect(&capacity, -1),
              HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED);

    hyp_daemon_runtime_connect_result_t stopping = capacity;
    snprintf(stopping.message, sizeof(stopping.message), "HYP daemon is stopping");
    ASSERT_EQ(hyp_daemon_bootstrap_classify_failed_connect(&stopping, 1),
              HYP_DAEMON_BOOTSTRAP_PROBE_TERMINAL);

    hyp_daemon_runtime_connect_result_t absent = {0};
    absent.status = HYP_DAEMON_RUNTIME_CONNECT_ERROR;
    ASSERT_EQ(hyp_daemon_bootstrap_classify_failed_connect(&absent, 1),
              HYP_DAEMON_BOOTSTRAP_PROBE_RESERVED);
    ASSERT_EQ(hyp_daemon_bootstrap_classify_failed_connect(&absent, 0),
              HYP_DAEMON_BOOTSTRAP_PROBE_UNAVAILABLE);
    ASSERT_EQ(hyp_daemon_bootstrap_classify_failed_connect(&absent, -1),
              HYP_DAEMON_BOOTSTRAP_PROBE_ERROR);
    PASS();
}

TEST(daemon_bootstrap_concurrent_first_clients_spawn_one_daemon) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "startup-race"));
    bootstrap_fake_ops_t fake = {0};
    hyp_daemon_bootstrap_ops_t ops = bootstrap_fake_callbacks(&fake);
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_A);
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = "/tmp/hyp",
        .connect_timeout_ms = 10,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    atomic_int ready = 0;
    atomic_bool go = false;
    bootstrap_thread_call_t calls[2] = {
        {.config = &config, .ops = &ops, .ready = &ready, .go = &go},
        {.config = &config, .ops = &ops, .ready = &ready, .go = &go},
    };
    hyp_thread_t threads[2];
    ASSERT_EQ(hyp_thread_create(&threads[0], 0, bootstrap_thread_execute, &calls[0]), 0);
    ASSERT_EQ(hyp_thread_create(&threads[1], 0, bootstrap_thread_execute, &calls[1]), 0);
    uint64_t ready_deadline = hyp_now_ms() + BOOTSTRAP_TEST_TIMEOUT_MS;
    while (atomic_load(&ready) != 2 && hyp_now_ms() < ready_deadline) {
        const struct timespec pause = {0, 1000000L};
        (void)hyp_nanosleep(&pause, NULL);
    }
    atomic_store(&go, true);
    ASSERT_EQ(hyp_thread_join(&threads[0]), 0);
    ASSERT_EQ(hyp_thread_join(&threads[1]), 0);
    ASSERT_EQ(calls[0].status, HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_EQ(calls[1].status, HYP_DAEMON_BOOTSTRAP_CONNECTED);
    ASSERT_EQ(atomic_load(&fake.spawn_count), 1);
    ASSERT_TRUE(calls[0].result.daemon_spawned != calls[1].result.daemon_spawned);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}

#ifdef __APPLE__
/* RED: the old double-fork returned success before its grandchild attempted
 * posix_spawn, hiding an immediate launch error behind the full timeout. */
TEST(daemon_bootstrap_darwin_launch_failure_is_synchronous) {
    bootstrap_endpoint_fixture_t fixture;
    ASSERT_TRUE(bootstrap_endpoint_fixture_start(&fixture, "darwin-missing"));
    hyp_daemon_build_identity_t identity = bootstrap_identity("2.4.0", BOOTSTRAP_BUILD_A);
    char missing[BOOTSTRAP_TEST_PATH_CAP];
    int written = snprintf(missing, sizeof(missing), "%s/definitely-missing-hyp", fixture.parent);
    ASSERT(written > 0 && written < (int)sizeof(missing));
    hyp_daemon_bootstrap_config_t config = {
        .role = HYP_DAEMON_PROCESS_MCP_CLIENT,
        .endpoint = fixture.endpoint,
        .identity = &identity,
        .executable_path = missing,
        .connect_timeout_ms = 1,
        .startup_timeout_ms = BOOTSTRAP_TEST_TIMEOUT_MS,
    };
    hyp_daemon_bootstrap_result_t result;
    uint64_t started = hyp_now_ms();
    ASSERT_EQ(hyp_daemon_bootstrap_execute(&config, &result), HYP_DAEMON_BOOTSTRAP_FAILED);
    uint64_t elapsed = hyp_now_ms() - started;
    ASSERT_FALSE(result.daemon_spawned);
    ASSERT(elapsed < BOOTSTRAP_TEST_TIMEOUT_MS / 2U);
    bootstrap_endpoint_fixture_finish(&fixture);
    PASS();
}
#endif

#ifndef _WIN32
static char *bootstrap_env_save(const char *name) {
    const char *value = getenv(name);
    return value ? strdup(value) : NULL;
}

static void bootstrap_env_restore(const char *name, char *saved) {
    if (saved) {
        hyp_setenv(name, saved, 1);
        free(saved);
    } else {
        hyp_unsetenv(name);
    }
}
#endif

/* §3.2 defect 1. The account-wide rendezvous had no exit that a normal build
 * could take: HYP_TEST_DAEMON_RUNTIME_PARENT is compiled in only under
 * HYP_ENABLE_TEST_SEAMS, so during §3.1 six parallel agents each had to build a
 * different binary to work at all -- and an MCP server that cannot start does
 * not fail loudly, it leaves its client answering with no tools.
 *
 * The escape must satisfy three things at once, and this asserts all three:
 * it must EXIST for a normal build; it must be a function of the CACHE, so the
 * guard it escapes still protects the store it exists to protect; and it must
 * REFUSE the one combination that corrupts -- a private socket over the shared
 * account cache. */
TEST(daemon_bootstrap_isolated_rendezvous_is_scoped_by_cache_and_refuses_the_default) {
#ifdef _WIN32
    SKIP_PLATFORM("windows: the rendezvous is a named pipe with no cache-scoped directory");
#else
    char tmpdir[BOOTSTRAP_TEST_PATH_CAP];
    (void)snprintf(tmpdir, sizeof(tmpdir), "/tmp/hyp-isolation-XXXXXX");
    ASSERT_NOT_NULL(hyp_mkdtemp(tmpdir));

    char *saved_home = bootstrap_env_save("HOME");
    char *saved_cache = bootstrap_env_save("HYP_CACHE_DIR");
    char *saved_parent = bootstrap_env_save("HYP_DAEMON_RUNTIME_PARENT");
    char *saved_seam = bootstrap_env_save("HYP_TEST_DAEMON_RUNTIME_PARENT");

    char parent[BOOTSTRAP_TEST_PATH_CAP];
    char default_cache[BOOTSTRAP_TEST_PATH_CAP];
    char cache_a[BOOTSTRAP_TEST_PATH_CAP];
    char cache_b[BOOTSTRAP_TEST_PATH_CAP];
    (void)snprintf(parent, sizeof(parent), "%s/rt", tmpdir);
    (void)snprintf(default_cache, sizeof(default_cache), "%s/.cache/hyponoia", tmpdir);
    (void)snprintf(cache_a, sizeof(cache_a), "%s/cache-a", tmpdir);
    (void)snprintf(cache_b, sizeof(cache_b), "%s/cache-b", tmpdir);

    /* HOME decides what "the account default cache" means; the seam must be
     * out of the way so the production variable is the one under test. */
    hyp_setenv("HOME", tmpdir, 1);
    hyp_unsetenv("HYP_TEST_DAEMON_RUNTIME_PARENT");
    hyp_setenv("HYP_DAEMON_RUNTIME_PARENT", parent, 1);

    bool made = hyp_mkdir_p(default_cache, 0700) && hyp_mkdir_p(cache_a, 0700) &&
                hyp_mkdir_p(cache_b, 0700);

    /* 1. Refused: isolating the socket while sharing the account's store is
     *    the corruption the cohort guard exists to prevent. */
    hyp_setenv("HYP_CACHE_DIR", default_cache, 1);
    hyp_daemon_ipc_endpoint_t *shared_cache = hyp_daemon_bootstrap_endpoint_new(NULL);

    /* 2. Accepted with its own cache, under the caller's own parent. */
    hyp_setenv("HYP_CACHE_DIR", cache_a, 1);
    hyp_daemon_ipc_endpoint_t *first = hyp_daemon_bootstrap_endpoint_new(NULL);
    char first_dir[BOOTSTRAP_TEST_PATH_CAP] = {0};
    char first_address[BOOTSTRAP_TEST_PATH_CAP] = {0};
    if (first) {
        (void)snprintf(first_dir, sizeof(first_dir), "%s",
                       hyp_daemon_ipc_endpoint_runtime_dir(first));
        (void)snprintf(first_address, sizeof(first_address), "%s",
                       hyp_daemon_ipc_endpoint_address(first));
    }

    /* 3. A different cache is a different rendezvous: nothing is shared. */
    hyp_setenv("HYP_CACHE_DIR", cache_b, 1);
    hyp_daemon_ipc_endpoint_t *other = hyp_daemon_bootstrap_endpoint_new(NULL);
    char other_dir[BOOTSTRAP_TEST_PATH_CAP] = {0};
    if (other) {
        (void)snprintf(other_dir, sizeof(other_dir), "%s",
                       hyp_daemon_ipc_endpoint_runtime_dir(other));
    }

    /* 4. The SAME cache is the same rendezvous, so two processes pointed at
     *    one store still meet -- and still meet the guard. */
    hyp_setenv("HYP_CACHE_DIR", cache_a, 1);
    hyp_daemon_ipc_endpoint_t *again = hyp_daemon_bootstrap_endpoint_new(NULL);
    char again_dir[BOOTSTRAP_TEST_PATH_CAP] = {0};
    if (again) {
        (void)snprintf(again_dir, sizeof(again_dir), "%s",
                       hyp_daemon_ipc_endpoint_runtime_dir(again));
    }

    /* 5. Unset, and the account-wide rendezvous is exactly what it always
     *    was -- the escape is opt-in and changes nothing by default. */
    hyp_unsetenv("HYP_DAEMON_RUNTIME_PARENT");
    hyp_daemon_ipc_endpoint_t *account = hyp_daemon_bootstrap_endpoint_new(NULL);
    char account_dir[BOOTSTRAP_TEST_PATH_CAP] = {0};
    if (account) {
        (void)snprintf(account_dir, sizeof(account_dir), "%s",
                       hyp_daemon_ipc_endpoint_runtime_dir(account));
    }

    hyp_daemon_ipc_endpoint_free(shared_cache);
    hyp_daemon_ipc_endpoint_free(first);
    hyp_daemon_ipc_endpoint_free(other);
    hyp_daemon_ipc_endpoint_free(again);
    hyp_daemon_ipc_endpoint_free(account);
    bootstrap_env_restore("HYP_TEST_DAEMON_RUNTIME_PARENT", saved_seam);
    bootstrap_env_restore("HYP_DAEMON_RUNTIME_PARENT", saved_parent);
    bootstrap_env_restore("HYP_CACHE_DIR", saved_cache);
    bootstrap_env_restore("HOME", saved_home);
    th_rmtree(tmpdir);

    ASSERT_TRUE(made);
    ASSERT_NULL(shared_cache);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(other);
    ASSERT_NOT_NULL(again);
    ASSERT_NOT_NULL(account);
    /* Under the caller's parent, in a directory named from the cache. */
    ASSERT_EQ(strncmp(first_dir, parent, strlen(parent)), 0);
    ASSERT_NOT_NULL(strstr(first_dir, "/hyp-scope-"));
    ASSERT_STR_NEQ(first_dir, other_dir);
    ASSERT_STR_EQ(first_dir, again_dir);
    /* And the account-wide default is untouched by any of it. */
    ASSERT_NULL(strstr(account_dir, "hyp-scope-"));
    ASSERT_NULL(strstr(account_dir, tmpdir));
    /* The socket that will be bound inside it must fit sun_path. */
    ASSERT_TRUE(first_address[0] != '\0');
    ASSERT_TRUE(strlen(first_address) < sizeof(((struct sockaddr_un *)0)->sun_path));
    PASS();
#endif
}

SUITE(daemon_bootstrap) {
    RUN_TEST(daemon_bootstrap_classifies_default_and_ui_as_mcp_clients);
    RUN_TEST(daemon_bootstrap_classifies_stateless_commands_without_client);
    RUN_TEST(daemon_bootstrap_classifies_config_as_coordinated_local_cli);
    RUN_TEST(daemon_bootstrap_cli_help_is_stateless_but_tool_calls_are_local);
    RUN_TEST(daemon_bootstrap_cli_arguments_cannot_reclassify_the_process);
    RUN_TEST(daemon_bootstrap_internal_roles_never_take_client_leases);
    RUN_TEST(daemon_bootstrap_rejects_ambiguous_internal_daemon_argv);
    RUN_TEST(daemon_bootstrap_uses_one_stable_per_account_endpoint);
    RUN_TEST(daemon_bootstrap_isolated_rendezvous_is_scoped_by_cache_and_refuses_the_default);
    RUN_TEST(daemon_bootstrap_launches_only_exact_detached_hidden_role);
    RUN_TEST(daemon_bootstrap_permanent_daemon_argv_is_byte_exact);
    RUN_TEST(daemon_bootstrap_daemon_ctl_token_routes_after_cli);
    RUN_TEST(daemon_bootstrap_permanent_launch_spec_is_exact);
    RUN_TEST(daemon_bootstrap_stateless_roles_bypass_every_daemon_operation);
    RUN_TEST(daemon_bootstrap_cohort_conflict_is_visible_before_probe_or_spawn);
    RUN_TEST(daemon_bootstrap_existing_exact_daemon_connects_without_spawn);
    RUN_TEST(daemon_bootstrap_conflict_is_visible_and_never_spawns);
    RUN_TEST(daemon_bootstrap_terminal_generation_that_never_exits_is_not_replaced);
    RUN_TEST(daemon_bootstrap_terminal_then_absent_spawns_replacement);
    RUN_TEST(daemon_bootstrap_reserved_generation_becomes_connectable_without_spawn);
    RUN_TEST(daemon_bootstrap_reserved_then_absent_spawns_replacement);
    RUN_TEST(daemon_bootstrap_releases_handoff_when_spawned_generation_is_reserved);
    RUN_TEST(daemon_bootstrap_rejected_connect_is_reserved_and_never_unavailable);
    RUN_TEST(daemon_bootstrap_concurrent_first_clients_spawn_one_daemon);
#ifdef __APPLE__
    RUN_TEST(daemon_bootstrap_darwin_launch_failure_is_synchronous);
#endif
}
