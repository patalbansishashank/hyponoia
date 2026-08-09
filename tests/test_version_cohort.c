/* RED contract for exact-build admission shared by one-shot CLI and daemon. */
#include "test_framework.h"
#include "test_helpers.h"

#include "daemon/ipc.h"
#include "daemon/service.h"
#include "daemon/version_cohort.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/compat_thread.h"
#include "foundation/platform.h"
#include "foundation/subprocess.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

enum { VERSION_COHORT_TEST_PATH_CAP = 1024 };

static const char VERSION_COHORT_BUILD_A[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char VERSION_COHORT_BUILD_B[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char VERSION_COHORT_CACHE_A[] =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char VERSION_COHORT_CACHE_B[] =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

typedef struct {
    char parent[VERSION_COHORT_TEST_PATH_CAP];
    hyp_daemon_ipc_endpoint_t *endpoint;
} version_cohort_fixture_t;

typedef struct {
    hyp_version_cohort_manager_t *manager;
    uint64_t deadline_ms;
    atomic_int callback_count;
    atomic_bool callback_seen;
    atomic_bool finished;
    hyp_version_cohort_status_t status;
    hyp_version_cohort_quiesce_result_t quiesce_result;
    hyp_version_cohort_lease_t *lease;
} version_cohort_mutation_wait_t;

static hyp_daemon_build_identity_t version_cohort_identity(const char *version, const char *build) {
    hyp_daemon_build_identity_t identity = {
        .semantic_version = version,
        .build_fingerprint = build,
        .cache_fingerprint = VERSION_COHORT_CACHE_A,
        .protocol_abi = 3,
        .store_abi = 11,
        .feature_abi = 7,
    };
    return identity;
}

static bool version_cohort_fixture_start(version_cohort_fixture_t *fixture, const char *tag) {
    memset(fixture, 0, sizeof(*fixture));
    int written = snprintf(fixture->parent, sizeof(fixture->parent),
                           "%s/hyp-version-cohort-%s-XXXXXX", hyp_tmpdir(), tag);
    if (written <= 0 || written >= (int)sizeof(fixture->parent) || !hyp_mkdtemp(fixture->parent)) {
        return false;
    }
    fixture->endpoint = hyp_daemon_ipc_endpoint_new("0123456789abcdef", fixture->parent);
    return fixture->endpoint != NULL;
}

static void version_cohort_release(hyp_version_cohort_lease_t **lease) {
    while (lease && *lease && hyp_version_cohort_lease_release(lease) != HYP_PRIVATE_FILE_LOCK_OK) {
        hyp_usleep(1000);
    }
}

static void version_cohort_manager_close(hyp_version_cohort_manager_t **manager) {
    while (manager && *manager &&
           hyp_version_cohort_manager_free(manager) != HYP_PRIVATE_FILE_LOCK_OK) {
        hyp_usleep(1000);
    }
}

static void version_cohort_fixture_finish(version_cohort_fixture_t *fixture) {
    hyp_daemon_ipc_endpoint_free(fixture->endpoint);
    if (fixture->parent[0]) {
        (void)th_rmtree(fixture->parent);
    }
    memset(fixture, 0, sizeof(*fixture));
}

static void version_cohort_mutation_wait_init(version_cohort_mutation_wait_t *wait,
                                              hyp_version_cohort_manager_t *manager,
                                              uint64_t deadline_ms) {
    memset(wait, 0, sizeof(*wait));
    wait->manager = manager;
    wait->deadline_ms = deadline_ms;
    wait->status = HYP_VERSION_COHORT_IO;
    wait->quiesce_result = HYP_VERSION_COHORT_QUIESCE_NOT_NEEDED;
    atomic_init(&wait->callback_count, 0);
    atomic_init(&wait->callback_seen, false);
    atomic_init(&wait->finished, false);
}

static hyp_version_cohort_quiesce_result_t version_cohort_test_request_quiesce(void *context) {
    version_cohort_mutation_wait_t *wait = context;
    (void)atomic_fetch_add_explicit(&wait->callback_count, 1, memory_order_relaxed);
    atomic_store_explicit(&wait->callback_seen, true, memory_order_release);
    return HYP_VERSION_COHORT_QUIESCE_REQUESTED;
}

static void *version_cohort_mutation_wait_thread(void *context) {
    version_cohort_mutation_wait_t *wait = context;
    wait->status = hyp_version_cohort_reserve_for_mutation(
        wait->manager, wait->deadline_ms, version_cohort_test_request_quiesce, wait,
        &wait->quiesce_result, &wait->lease);
    atomic_store_explicit(&wait->finished, true, memory_order_release);
    return NULL;
}

static bool version_cohort_wait_for_atomic(atomic_bool *value, uint64_t deadline_ms) {
    while (!atomic_load_explicit(value, memory_order_acquire) && hyp_now_ms() < deadline_ms) {
        hyp_usleep(1000);
    }
    return atomic_load_explicit(value, memory_order_acquire);
}

TEST(version_cohort_shares_exact_build_rejects_conflict_and_turns_over) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "matrix"));
    hyp_version_cohort_manager_t *first = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *second = hyp_version_cohort_manager_new(fixture.endpoint);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    hyp_daemon_build_identity_t build_a = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    hyp_daemon_build_identity_t build_b = version_cohort_identity("2.5.0", VERSION_COHORT_BUILD_B);
    hyp_version_cohort_lease_t *a_first = NULL;
    hyp_version_cohort_lease_t *a_second = NULL;
    hyp_version_cohort_lease_t *b_lease = NULL;
    hyp_daemon_conflict_t conflict;

    ASSERT_EQ(hyp_version_cohort_acquire(first, &build_a, UINT64_MAX, &a_first, &conflict),
              HYP_VERSION_COHORT_OK);
    ASSERT_NOT_NULL(a_first);
    ASSERT_EQ(hyp_version_cohort_acquire(second, &build_a, UINT64_MAX, &a_second, &conflict),
              HYP_VERSION_COHORT_OK);
    ASSERT_NOT_NULL(a_second);
    ASSERT_EQ(hyp_version_cohort_acquire(second, &build_b, hyp_now_ms(), &b_lease, &conflict),
              HYP_VERSION_COHORT_CONFLICT);
    ASSERT_NULL(b_lease);
    ASSERT_EQ(conflict.status, HYP_DAEMON_HELLO_VERSION_CONFLICT);
    ASSERT_STR_EQ(conflict.active_version, "2.4.0");
    ASSERT_STR_EQ(conflict.requested_version, "2.5.0");

    version_cohort_release(&a_second);
    version_cohort_release(&a_first);
    ASSERT_EQ(hyp_version_cohort_acquire(second, &build_b, UINT64_MAX, &b_lease, &conflict),
              HYP_VERSION_COHORT_OK);
    ASSERT_NOT_NULL(b_lease);

    version_cohort_release(&b_lease);
    version_cohort_manager_close(&second);
    version_cohort_manager_close(&first);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

TEST(version_cohort_rejects_same_hash_with_different_abi) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "abi"));
    hyp_version_cohort_manager_t *first = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *second = hyp_version_cohort_manager_new(fixture.endpoint);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    hyp_daemon_build_identity_t active = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    hyp_daemon_build_identity_t requested = active;
    requested.feature_abi++;
    hyp_version_cohort_lease_t *active_lease = NULL;
    hyp_version_cohort_lease_t *requested_lease = NULL;
    hyp_daemon_conflict_t conflict;

    ASSERT_EQ(hyp_version_cohort_acquire(first, &active, UINT64_MAX, &active_lease, &conflict),
              HYP_VERSION_COHORT_OK);
    ASSERT_EQ(
        hyp_version_cohort_acquire(second, &requested, hyp_now_ms(), &requested_lease, &conflict),
        HYP_VERSION_COHORT_CONFLICT);
    ASSERT_NULL(requested_lease);
    ASSERT_EQ(conflict.status, HYP_DAEMON_HELLO_FEATURE_ABI_CONFLICT);

    version_cohort_release(&active_lease);
    version_cohort_manager_close(&second);
    version_cohort_manager_close(&first);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

/* A cohort identity without a canonical cache fingerprint would reintroduce
 * an unscoped namespace that can silently share with another cache-less
 * process. Cohort admission must fail closed even though the stable daemon
 * HELLO envelope intentionally remains cache-agnostic. */
TEST(version_cohort_rejects_missing_cache_fingerprint) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "missing-cache"));
    hyp_version_cohort_manager_t *manager = hyp_version_cohort_manager_new(fixture.endpoint);
    ASSERT_NOT_NULL(manager);
    hyp_daemon_build_identity_t identity = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    identity.cache_fingerprint = NULL;
    hyp_version_cohort_lease_t *lease = NULL;
    hyp_daemon_conflict_t conflict;

    ASSERT_EQ(hyp_version_cohort_acquire(manager, &identity, hyp_now_ms(), &lease, &conflict),
              HYP_VERSION_COHORT_UNSAFE);
    ASSERT_NULL(lease);

    version_cohort_manager_close(&manager);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

/* One account has one daemon and therefore one canonical cache generation.
 * A second exact-build process with another cache root must fail before it can
 * join lifetime ownership or request activation against the wrong storage. */
TEST(version_cohort_rejects_exact_build_with_different_cache_root) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "cache-root"));
    hyp_version_cohort_manager_t *first = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *second = hyp_version_cohort_manager_new(fixture.endpoint);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    hyp_daemon_build_identity_t active = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    hyp_daemon_build_identity_t requested = active;
    active.cache_fingerprint = VERSION_COHORT_CACHE_A;
    requested.cache_fingerprint = VERSION_COHORT_CACHE_B;
    hyp_version_cohort_lease_t *active_lease = NULL;
    hyp_version_cohort_lease_t *requested_lease = NULL;
    hyp_daemon_conflict_t conflict;

    ASSERT_EQ(hyp_version_cohort_acquire(first, &active, UINT64_MAX, &active_lease, &conflict),
              HYP_VERSION_COHORT_OK);
    ASSERT_EQ(
        hyp_version_cohort_acquire(second, &requested, hyp_now_ms(), &requested_lease, &conflict),
        HYP_VERSION_COHORT_CONFLICT);
    ASSERT_NULL(requested_lease);
    ASSERT_EQ(conflict.status, HYP_DAEMON_HELLO_CACHE_CONFLICT);
    ASSERT_STR_EQ(conflict.active_cache_fingerprint, VERSION_COHORT_CACHE_A);
    ASSERT_STR_EQ(conflict.requested_cache_fingerprint, VERSION_COHORT_CACHE_B);
    char message[HYP_DAEMON_CONFLICT_MESSAGE_SIZE];
    ASSERT_TRUE(hyp_daemon_conflict_format(&conflict, message, sizeof(message)));
    ASSERT_NOT_NULL(strstr(message, "cache"));

    version_cohort_release(&active_lease);
    version_cohort_manager_close(&second);
    version_cohort_manager_close(&first);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

TEST(version_cohort_exclusive_activation_blocks_and_is_blocked_by_participants) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "activation"));
    hyp_version_cohort_manager_t *participant_manager =
        hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *activation_manager =
        hyp_version_cohort_manager_new(fixture.endpoint);
    ASSERT_NOT_NULL(participant_manager);
    ASSERT_NOT_NULL(activation_manager);
    hyp_daemon_build_identity_t build_a = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    hyp_version_cohort_lease_t *participant = NULL;
    hyp_version_cohort_lease_t *activation = NULL;
    hyp_daemon_conflict_t conflict;

    ASSERT_EQ(hyp_version_cohort_acquire(participant_manager, &build_a, UINT64_MAX, &participant,
                                         &conflict),
              HYP_VERSION_COHORT_OK);
    ASSERT_EQ(hyp_version_cohort_reserve_exclusive(activation_manager, hyp_now_ms(), &activation),
              HYP_VERSION_COHORT_BUSY);
    ASSERT_NULL(activation);
    version_cohort_release(&participant);

    ASSERT_EQ(hyp_version_cohort_reserve_exclusive(activation_manager, UINT64_MAX, &activation),
              HYP_VERSION_COHORT_OK);
    ASSERT_NOT_NULL(activation);
    ASSERT_EQ(hyp_version_cohort_acquire(participant_manager, &build_a, hyp_now_ms(), &participant,
                                         &conflict),
              HYP_VERSION_COHORT_BUSY);
    ASSERT_NULL(participant);
    version_cohort_release(&activation);

    ASSERT_EQ(hyp_version_cohort_acquire(participant_manager, &build_a, UINT64_MAX, &participant,
                                         &conflict),
              HYP_VERSION_COHORT_OK);
    version_cohort_release(&participant);
    version_cohort_manager_close(&activation_manager);
    version_cohort_manager_close(&participant_manager);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

TEST(version_cohort_mutation_intent_fails_new_admission_and_spans_lease) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "mutation-intent"));
    hyp_version_cohort_manager_t *participant_manager =
        hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *mutation_manager =
        hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *contender_manager =
        hyp_version_cohort_manager_new(fixture.endpoint);
    ASSERT_NOT_NULL(participant_manager);
    ASSERT_NOT_NULL(mutation_manager);
    ASSERT_NOT_NULL(contender_manager);

    hyp_daemon_build_identity_t build_a = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    hyp_daemon_conflict_t conflict;
    hyp_version_cohort_lease_t *participant = NULL;
    hyp_version_cohort_lease_t *contender = NULL;
    ASSERT_EQ(hyp_version_cohort_acquire(participant_manager, &build_a, UINT64_MAX, &participant,
                                         &conflict),
              HYP_VERSION_COHORT_OK);
    hyp_version_cohort_maintenance_presence_t before =
        hyp_version_cohort_maintenance_presence(contender_manager);

    version_cohort_mutation_wait_t wait;
    version_cohort_mutation_wait_init(&wait, mutation_manager, hyp_now_ms() + 5000U);
    hyp_thread_t thread;
    bool started = hyp_thread_create(&thread, 0, version_cohort_mutation_wait_thread, &wait) == 0;
    bool callback_seen =
        started && version_cohort_wait_for_atomic(&wait.callback_seen, hyp_now_ms() + 2000U);
    hyp_version_cohort_maintenance_presence_t during =
        callback_seen ? hyp_version_cohort_maintenance_presence(contender_manager)
                      : HYP_VERSION_COHORT_MAINTENANCE_IO;
    hyp_version_cohort_status_t racing_status =
        callback_seen ? hyp_version_cohort_acquire(contender_manager, &build_a, UINT64_MAX,
                                                   &contender, &conflict)
                      : HYP_VERSION_COHORT_IO;
    bool racing_lease_absent = contender == NULL;
    bool still_draining =
        callback_seen && !atomic_load_explicit(&wait.finished, memory_order_acquire);

    version_cohort_release(&contender);
    version_cohort_release(&participant);
    bool finished = started && version_cohort_wait_for_atomic(&wait.finished, hyp_now_ms() + 5500U);
    bool joined = started && hyp_thread_join(&thread) == 0;
    hyp_version_cohort_maintenance_presence_t retained =
        finished && wait.lease ? hyp_version_cohort_maintenance_presence(contender_manager)
                               : HYP_VERSION_COHORT_MAINTENANCE_IO;
    hyp_version_cohort_status_t retained_admission_status =
        finished && wait.lease ? hyp_version_cohort_acquire(contender_manager, &build_a, UINT64_MAX,
                                                            &contender, &conflict)
                               : HYP_VERSION_COHORT_IO;
    bool retained_admission_absent = contender == NULL;
    version_cohort_release(&contender);
    version_cohort_release(&wait.lease);
    hyp_version_cohort_maintenance_presence_t after =
        hyp_version_cohort_maintenance_presence(contender_manager);
    hyp_version_cohort_status_t post_status =
        hyp_version_cohort_acquire(contender_manager, &build_a, UINT64_MAX, &contender, &conflict);

    version_cohort_release(&contender);
    version_cohort_manager_close(&contender_manager);
    version_cohort_manager_close(&mutation_manager);
    version_cohort_manager_close(&participant_manager);
    version_cohort_fixture_finish(&fixture);

    ASSERT_EQ(before, HYP_VERSION_COHORT_MAINTENANCE_ABSENT);
    ASSERT_TRUE(started);
    ASSERT_TRUE(callback_seen);
    ASSERT_EQ(during, HYP_VERSION_COHORT_MAINTENANCE_REQUESTED);
    ASSERT_EQ(racing_status, HYP_VERSION_COHORT_BUSY);
    ASSERT_TRUE(racing_lease_absent);
    ASSERT_TRUE(still_draining);
    ASSERT_TRUE(finished);
    ASSERT_TRUE(joined);
    ASSERT_EQ(wait.status, HYP_VERSION_COHORT_OK);
    ASSERT_EQ(wait.quiesce_result, HYP_VERSION_COHORT_QUIESCE_REQUESTED);
    ASSERT_EQ(atomic_load_explicit(&wait.callback_count, memory_order_relaxed), 1);
    ASSERT_EQ(retained, HYP_VERSION_COHORT_MAINTENANCE_REQUESTED);
    ASSERT_EQ(retained_admission_status, HYP_VERSION_COHORT_BUSY);
    ASSERT_TRUE(retained_admission_absent);
    ASSERT_EQ(after, HYP_VERSION_COHORT_MAINTENANCE_ABSENT);
    ASSERT_EQ(post_status, HYP_VERSION_COHORT_OK);
    PASS();
}

TEST(version_cohort_mutation_waits_for_every_lifetime_participant) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "mutation-drain"));
    hyp_version_cohort_manager_t *first_manager = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *second_manager = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *mutation_manager =
        hyp_version_cohort_manager_new(fixture.endpoint);
    ASSERT_NOT_NULL(first_manager);
    ASSERT_NOT_NULL(second_manager);
    ASSERT_NOT_NULL(mutation_manager);

    hyp_daemon_build_identity_t build_a = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    hyp_daemon_conflict_t conflict;
    hyp_version_cohort_lease_t *first = NULL;
    hyp_version_cohort_lease_t *second = NULL;
    ASSERT_EQ(hyp_version_cohort_acquire(first_manager, &build_a, UINT64_MAX, &first, &conflict),
              HYP_VERSION_COHORT_OK);
    ASSERT_EQ(hyp_version_cohort_acquire(second_manager, &build_a, UINT64_MAX, &second, &conflict),
              HYP_VERSION_COHORT_OK);

    version_cohort_mutation_wait_t wait;
    version_cohort_mutation_wait_init(&wait, mutation_manager, hyp_now_ms() + 5000U);
    hyp_thread_t thread;
    bool started = hyp_thread_create(&thread, 0, version_cohort_mutation_wait_thread, &wait) == 0;
    bool callback_seen =
        started && version_cohort_wait_for_atomic(&wait.callback_seen, hyp_now_ms() + 2000U);
    version_cohort_release(&first);
    hyp_usleep(20000);
    bool finished_after_one = atomic_load_explicit(&wait.finished, memory_order_acquire);
    version_cohort_release(&second);
    bool finished = started && version_cohort_wait_for_atomic(&wait.finished, hyp_now_ms() + 5500U);
    bool joined = started && hyp_thread_join(&thread) == 0;

    version_cohort_release(&wait.lease);
    version_cohort_manager_close(&mutation_manager);
    version_cohort_manager_close(&second_manager);
    version_cohort_manager_close(&first_manager);
    version_cohort_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_TRUE(callback_seen);
    ASSERT_FALSE(finished_after_one);
    ASSERT_TRUE(finished);
    ASSERT_TRUE(joined);
    ASSERT_EQ(wait.status, HYP_VERSION_COHORT_OK);
    ASSERT_EQ(wait.quiesce_result, HYP_VERSION_COHORT_QUIESCE_REQUESTED);
    ASSERT_EQ(atomic_load_explicit(&wait.callback_count, memory_order_relaxed), 1);
    PASS();
}

TEST(version_cohort_mutation_timeout_releases_all_guards) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "mutation-timeout"));
    hyp_version_cohort_manager_t *participant_manager =
        hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *mutation_manager =
        hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_manager_t *probe_manager = hyp_version_cohort_manager_new(fixture.endpoint);
    ASSERT_NOT_NULL(participant_manager);
    ASSERT_NOT_NULL(mutation_manager);
    ASSERT_NOT_NULL(probe_manager);

    hyp_daemon_build_identity_t build_a = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    hyp_daemon_conflict_t conflict;
    hyp_version_cohort_lease_t *participant = NULL;
    hyp_version_cohort_lease_t *mutation = NULL;
    hyp_version_cohort_lease_t *probe = NULL;
    ASSERT_EQ(hyp_version_cohort_acquire(participant_manager, &build_a, UINT64_MAX, &participant,
                                         &conflict),
              HYP_VERSION_COHORT_OK);

    version_cohort_mutation_wait_t callback;
    version_cohort_mutation_wait_init(&callback, mutation_manager, hyp_now_ms() + 25U);
    hyp_version_cohort_quiesce_result_t quiesce_result = HYP_VERSION_COHORT_QUIESCE_NOT_NEEDED;
    hyp_version_cohort_status_t timeout_status = hyp_version_cohort_reserve_for_mutation(
        mutation_manager, callback.deadline_ms, version_cohort_test_request_quiesce, &callback,
        &quiesce_result, &mutation);
    bool no_mutation_authority = mutation == NULL;
    hyp_version_cohort_maintenance_presence_t after_timeout =
        hyp_version_cohort_maintenance_presence(probe_manager);
    hyp_version_cohort_status_t admission_status =
        hyp_version_cohort_acquire(probe_manager, &build_a, UINT64_MAX, &probe, &conflict);

    version_cohort_release(&probe);
    version_cohort_release(&participant);
    hyp_version_cohort_quiesce_result_t invalid_result = HYP_VERSION_COHORT_QUIESCE_REQUESTED;
    hyp_version_cohort_status_t unbounded_status = hyp_version_cohort_reserve_for_mutation(
        mutation_manager, UINT64_MAX, version_cohort_test_request_quiesce, &callback,
        &invalid_result, &mutation);
    bool unbounded_lease_absent = mutation == NULL;
    hyp_version_cohort_quiesce_result_t retry_result = HYP_VERSION_COHORT_QUIESCE_REQUESTED;
    hyp_version_cohort_status_t retry_status = hyp_version_cohort_reserve_for_mutation(
        mutation_manager, hyp_now_ms() + 250U, version_cohort_test_request_quiesce, &callback,
        &retry_result, &mutation);
    hyp_version_cohort_maintenance_presence_t during_retry =
        mutation ? hyp_version_cohort_maintenance_presence(probe_manager)
                 : HYP_VERSION_COHORT_MAINTENANCE_IO;
    int callback_count_after_retry =
        atomic_load_explicit(&callback.callback_count, memory_order_relaxed);

    version_cohort_release(&mutation);
    hyp_version_cohort_maintenance_presence_t after_retry =
        hyp_version_cohort_maintenance_presence(probe_manager);
    version_cohort_manager_close(&probe_manager);
    version_cohort_manager_close(&mutation_manager);
    version_cohort_manager_close(&participant_manager);
    version_cohort_fixture_finish(&fixture);

    ASSERT_EQ(timeout_status, HYP_VERSION_COHORT_BUSY);
    ASSERT_TRUE(no_mutation_authority);
    ASSERT_EQ(quiesce_result, HYP_VERSION_COHORT_QUIESCE_REQUESTED);
    ASSERT_EQ(atomic_load_explicit(&callback.callback_count, memory_order_relaxed), 1);
    ASSERT_EQ(after_timeout, HYP_VERSION_COHORT_MAINTENANCE_ABSENT);
    ASSERT_EQ(admission_status, HYP_VERSION_COHORT_OK);
    ASSERT_EQ(unbounded_status, HYP_VERSION_COHORT_UNSAFE);
    ASSERT_EQ(invalid_result, HYP_VERSION_COHORT_QUIESCE_NOT_NEEDED);
    ASSERT_TRUE(unbounded_lease_absent);
    ASSERT_EQ(retry_status, HYP_VERSION_COHORT_OK);
    ASSERT_EQ(retry_result, HYP_VERSION_COHORT_QUIESCE_NOT_NEEDED);
    ASSERT_EQ(during_retry, HYP_VERSION_COHORT_MAINTENANCE_REQUESTED);
    ASSERT_EQ(callback_count_after_retry, 1);
    ASSERT_EQ(after_retry, HYP_VERSION_COHORT_MAINTENANCE_ABSENT);
    PASS();
}

TEST(version_cohort_does_not_repurpose_daemon_startup_lock_for_lifetime) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "startup-independent"));
    hyp_daemon_ipc_startup_lock_t *startup = NULL;
    ASSERT_EQ(hyp_daemon_ipc_startup_lock_try_acquire(fixture.endpoint, &startup), 1);
    ASSERT_NOT_NULL(startup);
    hyp_version_cohort_manager_t *manager = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_daemon_build_identity_t build_a = version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
    hyp_version_cohort_lease_t *lease = NULL;
    hyp_daemon_conflict_t conflict;
    ASSERT_NOT_NULL(manager);
    ASSERT_EQ(hyp_version_cohort_acquire(manager, &build_a, UINT64_MAX, &lease, &conflict),
              HYP_VERSION_COHORT_OK);
    ASSERT_NOT_NULL(lease);

    hyp_daemon_ipc_startup_lock_release(&startup);
    version_cohort_release(&lease);
    version_cohort_manager_close(&manager);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

TEST(version_cohort_distinguishes_coordinated_daemon_without_connecting) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "daemon-marker"));
    hyp_version_cohort_manager_t *manager = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_version_cohort_daemon_claim_t *claim = NULL;
    hyp_daemon_ipc_lifetime_reservation_t *lifetime = NULL;
    hyp_daemon_ipc_startup_lock_t *startup = NULL;
    ASSERT_NOT_NULL(manager);

    ASSERT_EQ(hyp_version_cohort_daemon_presence(manager, fixture.endpoint),
              HYP_VERSION_COHORT_DAEMON_ABSENT);

    /* Startup is also part of the migration boundary: on POSIX the legacy and
     * current startup locks are the same; on Windows current startup retains
     * the security-validated legacy mutex as an interlock. */
    ASSERT_EQ(hyp_daemon_ipc_startup_lock_try_acquire(fixture.endpoint, &startup), 1);
    ASSERT_NOT_NULL(startup);
    ASSERT_EQ(hyp_version_cohort_daemon_presence(manager, fixture.endpoint),
              HYP_VERSION_COHORT_DAEMON_UNCOORDINATED);
    hyp_daemon_ipc_startup_lock_release(&startup);
    startup = NULL;
    ASSERT_EQ(hyp_version_cohort_daemon_presence(manager, fixture.endpoint),
              HYP_VERSION_COHORT_DAEMON_ABSENT);

    /* A pre-cohort daemon owns the stable daemon lifetime reservation but
     * cannot own the new crash-released coordination marker. The local CLI
     * must fail closed without opening a protocol connection. */
    ASSERT_EQ(hyp_daemon_ipc_lifetime_reservation_try_acquire(fixture.endpoint, &lifetime), 1);
    ASSERT_NOT_NULL(lifetime);
    ASSERT_EQ(hyp_version_cohort_daemon_presence(manager, fixture.endpoint),
              HYP_VERSION_COHORT_DAEMON_UNCOORDINATED);

    ASSERT_EQ(hyp_version_cohort_daemon_claim_acquire(manager, &claim), HYP_VERSION_COHORT_OK);
    ASSERT_NOT_NULL(claim);
    ASSERT_EQ(hyp_version_cohort_daemon_presence(manager, fixture.endpoint),
              HYP_VERSION_COHORT_DAEMON_COORDINATED);

    /* RED for daemon turnover: listener/lifetime teardown precedes final
     * application/log cleanup. The still-held exact-generation marker is
     * authoritative during that window, so a new bootstrap waits instead of
     * starting a replacement against the old daemon's live state. */
    hyp_daemon_ipc_lifetime_reservation_release(lifetime);
    lifetime = NULL;
    ASSERT_EQ(hyp_version_cohort_daemon_presence(manager, fixture.endpoint),
              HYP_VERSION_COHORT_DAEMON_COORDINATED);

    ASSERT_EQ(hyp_version_cohort_daemon_claim_release(&claim), HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NULL(claim);
    ASSERT_EQ(hyp_version_cohort_daemon_presence(manager, fixture.endpoint),
              HYP_VERSION_COHORT_DAEMON_ABSENT);

    version_cohort_manager_close(&manager);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

TEST(version_cohort_transition_presence_is_authoritative_and_marker_checked) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "transition-presence"));
    hyp_version_cohort_manager_t *manager = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_daemon_ipc_local_transition_t *transition = NULL;
    hyp_daemon_ipc_lifetime_reservation_t *lifetime = NULL;
    hyp_version_cohort_daemon_claim_t *claim = NULL;
    ASSERT_NOT_NULL(manager);

    ASSERT_EQ(hyp_daemon_ipc_local_transition_try_acquire(fixture.endpoint, &transition), 1);
    ASSERT_NOT_NULL(transition);
    ASSERT_EQ(
        hyp_version_cohort_daemon_presence_under_transition(manager, fixture.endpoint, transition),
        HYP_VERSION_COHORT_DAEMON_UNSAFE);
    ASSERT_EQ(hyp_daemon_ipc_local_transition_seal_legacy(transition), 1);
    ASSERT_EQ(
        hyp_version_cohort_daemon_presence_under_transition(manager, fixture.endpoint, transition),
        HYP_VERSION_COHORT_DAEMON_ABSENT);
    ASSERT_TRUE(hyp_daemon_ipc_local_transition_release(&transition));
    ASSERT_NULL(transition);

    ASSERT_EQ(hyp_daemon_ipc_lifetime_reservation_try_acquire(fixture.endpoint, &lifetime), 1);
    ASSERT_NOT_NULL(lifetime);
    ASSERT_EQ(hyp_daemon_ipc_local_transition_try_acquire(fixture.endpoint, &transition), 1);
    ASSERT_NOT_NULL(transition);
    ASSERT_EQ(hyp_daemon_ipc_local_transition_seal_legacy(transition), 1);
    ASSERT_EQ(
        hyp_version_cohort_daemon_presence_under_transition(manager, fixture.endpoint, transition),
        HYP_VERSION_COHORT_DAEMON_UNCOORDINATED);

    ASSERT_EQ(hyp_version_cohort_daemon_claim_acquire(manager, &claim), HYP_VERSION_COHORT_OK);
    ASSERT_NOT_NULL(claim);
    ASSERT_EQ(
        hyp_version_cohort_daemon_presence_under_transition(manager, fixture.endpoint, transition),
        HYP_VERSION_COHORT_DAEMON_COORDINATED);

    ASSERT_EQ(hyp_version_cohort_daemon_claim_release(&claim), HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NULL(claim);
    ASSERT_TRUE(hyp_daemon_ipc_local_transition_release(&transition));
    ASSERT_NULL(transition);
    hyp_daemon_ipc_lifetime_reservation_release(lifetime);
    lifetime = NULL;
    version_cohort_manager_close(&manager);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

TEST(version_cohort_transition_shutdown_order_has_no_false_conflict) {
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "transition-shutdown"));
    hyp_version_cohort_manager_t *manager = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_daemon_ipc_lifetime_reservation_t *lifetime = NULL;
    hyp_version_cohort_daemon_claim_t *claim = NULL;
    hyp_daemon_ipc_local_transition_t *transition = NULL;
    ASSERT_NOT_NULL(manager);

    ASSERT_EQ(hyp_daemon_ipc_lifetime_reservation_try_acquire(fixture.endpoint, &lifetime), 1);
    ASSERT_EQ(hyp_version_cohort_daemon_claim_acquire(manager, &claim), HYP_VERSION_COHORT_OK);
    ASSERT_EQ(hyp_daemon_ipc_local_transition_try_acquire(fixture.endpoint, &transition), 1);
    ASSERT_EQ(hyp_daemon_ipc_local_transition_seal_legacy(transition), 1);
    ASSERT_EQ(
        hyp_version_cohort_daemon_presence_under_transition(manager, fixture.endpoint, transition),
        HYP_VERSION_COHORT_DAEMON_COORDINATED);

    /* Host teardown closes listener/lifetime before its daemon marker. The
     * overlap is still coordinated, never the pre-cohort conflict state. */
    hyp_daemon_ipc_lifetime_reservation_release(lifetime);
    lifetime = NULL;
    ASSERT_EQ(
        hyp_version_cohort_daemon_presence_under_transition(manager, fixture.endpoint, transition),
        HYP_VERSION_COHORT_DAEMON_COORDINATED);
    ASSERT_EQ(hyp_version_cohort_daemon_claim_release(&claim), HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(
        hyp_version_cohort_daemon_presence_under_transition(manager, fixture.endpoint, transition),
        HYP_VERSION_COHORT_DAEMON_ABSENT);

    ASSERT_TRUE(hyp_daemon_ipc_local_transition_release(&transition));
    version_cohort_manager_close(&manager);
    version_cohort_fixture_finish(&fixture);
    PASS();
}

TEST(version_cohort_presence_recovers_current_posix_listener_crash) {
#ifdef _WIN32
    PASS();
#else
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "daemon-crash"));
    int ready_pipe[2] = {-1, -1};
    ASSERT_EQ(pipe(ready_pipe), 0);
    pid_t child = fork();
    if (child == 0) {
        (void)close(ready_pipe[0]);
        hyp_daemon_ipc_listener_t *listener = hyp_daemon_ipc_listen(fixture.endpoint);
        char ready = listener ? 'R' : 'E';
        ssize_t reported = write(ready_pipe[1], &ready, 1);
        (void)close(ready_pipe[1]);
        /* Simulate a process crash: no listener_close and therefore no
         * userspace socket/identity cleanup. */
        _exit(listener && reported == 1 ? 0 : 1);
    }
    ASSERT_GT(child, 0);
    (void)close(ready_pipe[1]);
    char ready = 0;
    ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
    (void)close(ready_pipe[0]);
    ASSERT_EQ(ready, 'R');
    int child_status = 0;
    ASSERT_EQ(waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0);
    ASSERT_EQ(hyp_daemon_ipc_lifetime_reservation_probe(fixture.endpoint), 0);
    ASSERT_EQ(hyp_daemon_ipc_endpoint_probe(fixture.endpoint, 1), 1);

    hyp_version_cohort_manager_t *manager = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_daemon_ipc_local_transition_t *transition = NULL;
    ASSERT_NOT_NULL(manager);
    ASSERT_EQ(hyp_daemon_ipc_local_transition_try_acquire(fixture.endpoint, &transition), 1);
    ASSERT_NOT_NULL(transition);
    ASSERT_EQ(hyp_daemon_ipc_local_transition_seal_legacy(transition), 1);
    ASSERT_EQ(
        hyp_version_cohort_daemon_presence_under_transition(manager, fixture.endpoint, transition),
        HYP_VERSION_COHORT_DAEMON_ABSENT);
    ASSERT_EQ(hyp_daemon_ipc_endpoint_probe(fixture.endpoint, 1), 0);

    ASSERT_TRUE(hyp_daemon_ipc_local_transition_release(&transition));
    ASSERT_NULL(transition);
    version_cohort_manager_close(&manager);
    version_cohort_fixture_finish(&fixture);
    PASS();
#endif
}

TEST(version_cohort_crash_releases_process_lifetime_lease) {
#ifdef _WIN32
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "crash-win"));
    char ready_path[VERSION_COHORT_TEST_PATH_CAP];
    char self[MAX_PATH];
    DWORD self_length = GetModuleFileNameA(NULL, self, sizeof(self));
    int ready_length = snprintf(ready_path, sizeof(ready_path), "%s/ready", fixture.parent);
    bool launch_ready = self_length > 0 && self_length < sizeof(self) && ready_length > 0 &&
                        ready_length < (int)sizeof(ready_path);
    const char *const argv[] = {
        self, "__hyp_version_cohort_crash_holder", "0123456789abcdef", fixture.parent, ready_path,
        NULL,
    };
    hyp_proc_opts_t options = {
        .bin = self,
        .argv = argv,
        .quiet_timeout_ms = 2000,
        .cancel_grace_ms = 1,
    };
    hyp_subprocess_t *child = NULL;
    int spawn_status = launch_ready ? hyp_subprocess_spawn(&options, &child) : -1;
    bool ready = false;
    bool terminal = false;
    hyp_proc_result_t process_result = {0};
    uint64_t ready_deadline = hyp_now_ms() + 5000U;
    while (child && hyp_now_ms() < ready_deadline) {
        hyp_proc_poll_t poll = hyp_subprocess_poll(child, &process_result);
        if (poll == HYP_PROC_POLL_TERMINAL) {
            terminal = true;
            break;
        }
        if (poll == HYP_PROC_POLL_ERROR) {
            break;
        }
        FILE *marker = hyp_fopen(ready_path, "rb");
        if (marker) {
            ready = fgetc(marker) == 'R';
            (void)fclose(marker);
        }
        if (ready) {
            break;
        }
        hyp_usleep(1000);
    }

    hyp_version_cohort_manager_t *manager =
        ready ? hyp_version_cohort_manager_new(fixture.endpoint) : NULL;
    hyp_daemon_build_identity_t build_b = version_cohort_identity("2.5.0", VERSION_COHORT_BUILD_B);
    hyp_version_cohort_lease_t *lease = NULL;
    hyp_daemon_conflict_t conflict;
    hyp_version_cohort_status_t conflict_status =
        manager ? hyp_version_cohort_acquire(manager, &build_b, hyp_now_ms(), &lease, &conflict)
                : HYP_VERSION_COHORT_IO;
    bool conflict_lease_absent = lease == NULL;
    version_cohort_release(&lease);
    bool cancel_requested = child && !terminal && hyp_subprocess_request_cancel(child);
    uint64_t terminal_deadline = hyp_now_ms() + 5000U;
    while (child && !terminal && hyp_now_ms() < terminal_deadline) {
        hyp_proc_poll_t poll = hyp_subprocess_poll(child, &process_result);
        if (poll == HYP_PROC_POLL_TERMINAL) {
            terminal = true;
            break;
        }
        if (poll == HYP_PROC_POLL_ERROR) {
            break;
        }
        hyp_usleep(1000);
    }
    /* The turnover acquire gets its own conflict record: acquire zeroes its
     * conflict_out on entry, so reusing `conflict` here would erase the
     * probe's VERSION_CONFLICT detail before the assertions read it. */
    hyp_daemon_conflict_t turnover_conflict;
    hyp_version_cohort_status_t turnover_status =
        manager && terminal
            ? hyp_version_cohort_acquire(manager, &build_b, UINT64_MAX, &lease, &turnover_conflict)
            : HYP_VERSION_COHORT_IO;

    version_cohort_release(&lease);
    version_cohort_manager_close(&manager);
    if (child && terminal) {
        hyp_subprocess_destroy(child);
        child = NULL;
    }
    (void)hyp_unlink(ready_path);
    version_cohort_fixture_finish(&fixture);

    ASSERT_TRUE(launch_ready);
    ASSERT_EQ(spawn_status, 0);
    ASSERT_TRUE(ready);
    ASSERT_EQ(conflict_status, HYP_VERSION_COHORT_CONFLICT);
    ASSERT_TRUE(conflict_lease_absent);
    ASSERT_EQ(conflict.status, HYP_DAEMON_HELLO_VERSION_CONFLICT);
    ASSERT_TRUE(cancel_requested);
    ASSERT_TRUE(terminal);
    ASSERT_TRUE(process_result.tree_quiesced);
    ASSERT_FALSE(process_result.supervision_failed);
    ASSERT_EQ(process_result.outcome, HYP_PROC_KILLED);
    ASSERT_EQ(turnover_status, HYP_VERSION_COHORT_OK);
    PASS();
#else
    version_cohort_fixture_t fixture;
    ASSERT_TRUE(version_cohort_fixture_start(&fixture, "crash"));
    int ready_pipe[2] = {-1, -1};
    int command_pipe[2] = {-1, -1};
    ASSERT_EQ(pipe(ready_pipe), 0);
    ASSERT_EQ(pipe(command_pipe), 0);
    pid_t child = fork();
    if (child == 0) {
        (void)close(ready_pipe[0]);
        (void)close(command_pipe[1]);
        hyp_version_cohort_manager_t *manager = hyp_version_cohort_manager_new(fixture.endpoint);
        hyp_daemon_build_identity_t build_a =
            version_cohort_identity("2.4.0", VERSION_COHORT_BUILD_A);
        hyp_version_cohort_lease_t *lease = NULL;
        hyp_daemon_conflict_t conflict;
        bool acquired = manager && hyp_version_cohort_acquire(manager, &build_a, UINT64_MAX, &lease,
                                                              &conflict) == HYP_VERSION_COHORT_OK;
        char ready = acquired ? 'R' : 'E';
        ssize_t ignored = write(ready_pipe[1], &ready, 1);
        (void)ignored;
        (void)close(ready_pipe[1]);
        char command = 0;
        ssize_t commanded = read(command_pipe[0], &command, 1);
        (void)close(command_pipe[0]);
        _exit(acquired && commanded == 1 && command == 'X' ? 0 : 1);
        /* no release: kernel must drop the lease */
    }
    (void)close(ready_pipe[1]);
    (void)close(command_pipe[0]);
    char ready = 0;
    ssize_t received = read(ready_pipe[0], &ready, 1);
    (void)close(ready_pipe[0]);
    ASSERT_EQ(received, 1);
    ASSERT_EQ(ready, 'R');
    ASSERT_GT(child, 0);

    hyp_version_cohort_manager_t *manager = hyp_version_cohort_manager_new(fixture.endpoint);
    hyp_daemon_build_identity_t build_b = version_cohort_identity("2.5.0", VERSION_COHORT_BUILD_B);
    hyp_version_cohort_lease_t *lease = NULL;
    hyp_daemon_conflict_t conflict;
    ASSERT_NOT_NULL(manager);
    ASSERT_EQ(hyp_version_cohort_acquire(manager, &build_b, hyp_now_ms(), &lease, &conflict),
              HYP_VERSION_COHORT_CONFLICT);
    ASSERT_NULL(lease);
    ASSERT_EQ(conflict.status, HYP_DAEMON_HELLO_VERSION_CONFLICT);

    char exit_command = 'X';
    ASSERT_EQ(write(command_pipe[1], &exit_command, 1), 1);
    (void)close(command_pipe[1]);
    int child_status = 0;
    ASSERT_EQ(waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0);

    ASSERT_EQ(hyp_version_cohort_acquire(manager, &build_b, UINT64_MAX, &lease, &conflict),
              HYP_VERSION_COHORT_OK);
    version_cohort_release(&lease);
    version_cohort_manager_close(&manager);
    version_cohort_fixture_finish(&fixture);
    PASS();
#endif
}

SUITE(version_cohort) {
    RUN_TEST(version_cohort_shares_exact_build_rejects_conflict_and_turns_over);
    RUN_TEST(version_cohort_rejects_same_hash_with_different_abi);
    RUN_TEST(version_cohort_rejects_missing_cache_fingerprint);
    RUN_TEST(version_cohort_rejects_exact_build_with_different_cache_root);
    RUN_TEST(version_cohort_exclusive_activation_blocks_and_is_blocked_by_participants);
    RUN_TEST(version_cohort_mutation_intent_fails_new_admission_and_spans_lease);
    RUN_TEST(version_cohort_mutation_waits_for_every_lifetime_participant);
    RUN_TEST(version_cohort_mutation_timeout_releases_all_guards);
    RUN_TEST(version_cohort_does_not_repurpose_daemon_startup_lock_for_lifetime);
    RUN_TEST(version_cohort_distinguishes_coordinated_daemon_without_connecting);
    RUN_TEST(version_cohort_transition_presence_is_authoritative_and_marker_checked);
    RUN_TEST(version_cohort_transition_shutdown_order_has_no_false_conflict);
    RUN_TEST(version_cohort_presence_recovers_current_posix_listener_crash);
    RUN_TEST(version_cohort_crash_releases_process_lifetime_lease);
}
