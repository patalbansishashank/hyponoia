/* RED contract for the generic writer-preference lock registry. */
#include "test_framework.h"

#include "foundation/compat.h"
#include "foundation/compat_thread.h"
#include "foundation/lock_registry.h"
#include "foundation/lock_registry_internal.h"
#include "foundation/platform.h"
#include "foundation/private_file_lock_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

enum {
    LOCK_REGISTRY_TEST_PATH_CAP = 1024,
    LOCK_REGISTRY_TEST_TIMEOUT_MS = 5000,
    LOCK_REGISTRY_STRESS_THREADS = 8,
    LOCK_REGISTRY_STRESS_ITERATIONS = 160,
    LOCK_REGISTRY_PARKING_WAITERS = 64,
};

typedef struct {
    char parent[LOCK_REGISTRY_TEST_PATH_CAP];
    char root[LOCK_REGISTRY_TEST_PATH_CAP];
    hyp_private_lock_directory_t *directory;
    hyp_lock_registry_t *registry;
} lock_registry_fixture_t;

/* The stress fixtures that use this helper are POSIX-only. */
#ifndef _WIN32
static void lock_registry_test_yield(void) {
    (void)sched_yield();
}
#endif

#ifndef _WIN32
static hyp_private_lock_directory_t *lock_registry_test_directory_open(const char *root) {
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    hyp_private_lock_directory_t *directory = NULL;
    if (fd < 0 ||
        hyp_private_lock_directory_adopt_posix(fd, root, &directory) != HYP_PRIVATE_FILE_LOCK_OK) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return NULL;
    }
    return directory;
}
#endif

#ifndef _WIN32
static bool lock_registry_fixture_start(lock_registry_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
#ifdef _WIN32
    return false;
#else
    int written = snprintf(fixture->parent, sizeof(fixture->parent), "%s/hyp-lock-registry-XXXXXX",
                           hyp_tmpdir());
    if (written <= 0 || written >= (int)sizeof(fixture->parent) || !hyp_mkdtemp(fixture->parent)) {
        return false;
    }
    written = snprintf(fixture->root, sizeof(fixture->root), "%s/root", fixture->parent);
    if (written <= 0 || written >= (int)sizeof(fixture->root) || mkdir(fixture->root, 0700) != 0) {
        return false;
    }
    fixture->directory = lock_registry_test_directory_open(fixture->root);
    fixture->registry = hyp_lock_registry_new(fixture->directory);
    return fixture->directory != NULL && fixture->registry != NULL;
#endif
}

static void lock_registry_fixture_finish(lock_registry_fixture_t *fixture) {
    (void)hyp_lock_registry_free(&fixture->registry);
    hyp_private_lock_directory_close(fixture->directory);
#ifndef _WIN32
    DIR *directory = opendir(fixture->root);
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char path[LOCK_REGISTRY_TEST_PATH_CAP];
            int written = snprintf(path, sizeof(path), "%s/%s", fixture->root, entry->d_name);
            if (written > 0 && written < (int)sizeof(path)) {
                (void)unlink(path);
            }
        }
        (void)closedir(directory);
    }
    (void)rmdir(fixture->root);
    (void)rmdir(fixture->parent);
#endif
    memset(fixture, 0, sizeof(*fixture));
}
#endif

typedef struct {
    hyp_lock_registry_t *registry;
    const char *resource_key;
    hyp_private_file_lock_mode_t mode;
    hyp_lock_cancel_token_t cancel_token;
    atomic_bool finished;
    hyp_private_file_lock_status_t status;
    hyp_lock_lease_t *lease;
} lock_registry_waiter_t;

#ifndef _WIN32
static void *lock_registry_waiter_run(void *opaque) {
    lock_registry_waiter_t *waiter = opaque;
    waiter->status = hyp_lock_registry_acquire(waiter->registry, waiter->resource_key, waiter->mode,
                                               hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS,
                                               &waiter->cancel_token, &waiter->lease);
    atomic_store_explicit(&waiter->finished, true, memory_order_release);
    return NULL;
}

typedef struct {
    hyp_lock_cancel_token_t cancel_token;
    atomic_bool native_ready;
} lock_registry_rollback_fault_t;

static void lock_registry_cancel_at_native_ready(void *opaque, hyp_private_file_lock_mode_t mode,
                                                 hyp_lock_registry_stage_t stage) {
    lock_registry_rollback_fault_t *fault = opaque;
    if (mode == HYP_PRIVATE_FILE_LOCK_EX && stage == HYP_LOCK_REGISTRY_STAGE_NATIVE_READY) {
        atomic_store_explicit(&fault->native_ready, true, memory_order_release);
        atomic_store_explicit(&fault->cancel_token, true, memory_order_release);
    }
}
#endif

TEST(lock_registry_cancelled_wait_rolls_back_and_does_not_barge) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry RED runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *holder = NULL;
    hyp_private_file_lock_status_t holder_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "cancelled-waiter",
                                        HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &holder)
            : HYP_PRIVATE_FILE_LOCK_IO;

    lock_registry_waiter_t waiter = {.registry = fixture.registry,
                                     .resource_key = "cancelled-waiter",
                                     .mode = HYP_PRIVATE_FILE_LOCK_EX,
                                     .status = HYP_PRIVATE_FILE_LOCK_IO};
    atomic_init(&waiter.cancel_token, false);
    atomic_init(&waiter.finished, false);
    hyp_thread_t thread;
    bool thread_started = holder_status == HYP_PRIVATE_FILE_LOCK_OK &&
                          hyp_thread_create(&thread, 0, lock_registry_waiter_run, &waiter) == 0;
    bool queued = false;
    bool writer_has_turn = false;
    char turn_name[HYP_LOCK_REGISTRY_NAME_CAP];
    char rw_name[HYP_LOCK_REGISTRY_NAME_CAP];
    bool names_ok = hyp_lock_registry_resource_names("cancelled-waiter", turn_name, rw_name);
    uint64_t observe_deadline = hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS;
    while (thread_started && hyp_now_ms() < observe_deadline) {
        if (hyp_lock_registry_waiter_count(fixture.registry) == 1) {
            queued = true;
            hyp_private_file_lock_t *probe = NULL;
            hyp_private_file_lock_status_t probe_status =
                names_ok ? hyp_private_file_lock_try_acquire(fixture.directory, turn_name,
                                                             HYP_PRIVATE_FILE_LOCK_EX, &probe)
                         : HYP_PRIVATE_FILE_LOCK_IO;
            if (probe_status == HYP_PRIVATE_FILE_LOCK_BUSY) {
                writer_has_turn = true;
                break;
            }
            if (probe) {
                (void)hyp_private_file_lock_release(&probe);
            }
        }
        if (atomic_load_explicit(&waiter.finished, memory_order_acquire)) {
            break;
        }
        lock_registry_test_yield();
    }
    if (thread_started) {
        (void)hyp_lock_registry_request_cancel(fixture.registry, &waiter.cancel_token);
        (void)hyp_thread_join(&thread);
    }
    hyp_private_file_lock_t *after_turn = NULL;
    hyp_private_file_lock_status_t after_turn_status =
        names_ok ? hyp_private_file_lock_try_acquire(fixture.directory, turn_name,
                                                     HYP_PRIVATE_FILE_LOCK_EX, &after_turn)
                 : HYP_PRIVATE_FILE_LOCK_IO;
    if (after_turn) {
        (void)hyp_private_file_lock_release(&after_turn);
    }
    hyp_private_file_lock_status_t holder_release =
        holder ? hyp_lock_lease_release(&holder) : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_lock_lease_t *after = NULL;
    hyp_private_file_lock_status_t after_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "cancelled-waiter",
                                        HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &after)
            : HYP_PRIVATE_FILE_LOCK_IO;
    if (after) {
        (void)hyp_lock_lease_release(&after);
    }
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(holder_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(thread_started);
    ASSERT_TRUE(queued);
    ASSERT_TRUE(names_ok);
    ASSERT_TRUE(writer_has_turn);
    ASSERT_EQ(waiter.status, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_NULL(waiter.lease);
    ASSERT_EQ(after_turn_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(holder_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(after_status, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

TEST(lock_registry_failed_rollback_returns_cleanup_only_lease) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry rollback runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    lock_registry_rollback_fault_t fault;
    atomic_init(&fault.cancel_token, false);
    atomic_init(&fault.native_ready, false);
    bool hook_set = started && hyp_lock_registry_set_stage_hook_for_test(
                                   fixture.registry, lock_registry_cancel_at_native_ready, &fault);
    bool fault_set = hook_set && hyp_lock_registry_fail_next_native_release_step_for_test(
                                     fixture.registry, HYP_LOCK_REGISTRY_RELEASE_RW,
                                     HYP_PRIVATE_FILE_LOCK_RELEASE_UNLOCK);
    hyp_lock_lease_t *cleanup = NULL;
    hyp_private_file_lock_status_t status =
        fault_set ? hyp_lock_registry_acquire(
                        fixture.registry, "rollback-cleanup", HYP_PRIVATE_FILE_LOCK_EX,
                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, &fault.cancel_token, &cleanup)
                  : HYP_PRIVATE_FILE_LOCK_IO;
    bool reached_native_ready = atomic_load_explicit(&fault.native_ready, memory_order_acquire);
    bool cleanup_retained = cleanup != NULL;

    char turn_name[HYP_LOCK_REGISTRY_NAME_CAP];
    char rw_name[HYP_LOCK_REGISTRY_NAME_CAP];
    bool names_ok = hyp_lock_registry_resource_names("rollback-cleanup", turn_name, rw_name);
    hyp_private_file_lock_t *probe = NULL;
    hyp_private_file_lock_status_t while_cleanup_pending =
        names_ok ? hyp_private_file_lock_try_acquire(fixture.directory, rw_name,
                                                     HYP_PRIVATE_FILE_LOCK_EX, &probe)
                 : HYP_PRIVATE_FILE_LOCK_IO;
    if (probe) {
        (void)hyp_private_file_lock_release(&probe);
    }
    hyp_private_file_lock_status_t free_while_pending =
        started ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;
    bool registry_preserved = fixture.registry != NULL;
    hyp_private_file_lock_status_t cleanup_release =
        cleanup ? hyp_lock_lease_release(&cleanup) : HYP_PRIVATE_FILE_LOCK_IO;
    if (cleanup) {
        (void)hyp_lock_lease_release(&cleanup);
    }
    hyp_private_file_lock_status_t final_free =
        registry_preserved ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_TRUE(hook_set);
    ASSERT_TRUE(fault_set);
    ASSERT_TRUE(reached_native_ready);
    ASSERT_EQ(status, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(cleanup_retained);
    ASSERT_TRUE(names_ok);
    ASSERT_EQ(while_cleanup_pending, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_EQ(free_while_pending, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_TRUE(registry_preserved);
    ASSERT_EQ(cleanup_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NULL(cleanup);
    ASSERT_EQ(final_free, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

#ifndef _WIN32
static int lock_registry_abort_bookkeeping_failure_retains_cleanup(
    hyp_lock_registry_abort_failure_t failure, const char *resource_key) {
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    lock_registry_rollback_fault_t fault;
    atomic_init(&fault.cancel_token, false);
    atomic_init(&fault.native_ready, false);
    bool hook_set = started && hyp_lock_registry_set_stage_hook_for_test(
                                   fixture.registry, lock_registry_cancel_at_native_ready, &fault);
    bool abort_fault_set = hook_set && hyp_lock_registry_fail_next_abort_bookkeeping_for_test(
                                           fixture.registry, failure);
    bool release_fault_set =
        abort_fault_set &&
        hyp_lock_registry_fail_next_native_release_step_for_test(
            fixture.registry, HYP_LOCK_REGISTRY_RELEASE_RW, HYP_PRIVATE_FILE_LOCK_RELEASE_UNLOCK);

    hyp_lock_lease_t *cleanup = NULL;
    hyp_private_file_lock_status_t status =
        release_fault_set
            ? hyp_lock_registry_acquire(fixture.registry, resource_key, HYP_PRIVATE_FILE_LOCK_EX,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS,
                                        &fault.cancel_token, &cleanup)
            : HYP_PRIVATE_FILE_LOCK_IO;
    bool reached_native_ready = atomic_load_explicit(&fault.native_ready, memory_order_acquire);
    bool cleanup_retained = cleanup != NULL;
    bool owns_rw =
        hyp_lock_lease_has_release_handle_for_test(cleanup, HYP_LOCK_REGISTRY_RELEASE_RW);
    bool owns_turn =
        hyp_lock_lease_has_release_handle_for_test(cleanup, HYP_LOCK_REGISTRY_RELEASE_TURN);
    bool used_exact_lock_failure_path =
        failure != HYP_LOCK_REGISTRY_ABORT_FAIL_LOCK ||
        hyp_lock_lease_used_abort_lock_failure_path_for_test(cleanup);
    size_t waiter_before_release = hyp_lock_registry_waiter_count(fixture.registry);
    size_t pending_before_release =
        hyp_lock_registry_pending_cleanup_count_for_test(fixture.registry);

    char turn_name[HYP_LOCK_REGISTRY_NAME_CAP];
    char rw_name[HYP_LOCK_REGISTRY_NAME_CAP];
    bool names_ok = hyp_lock_registry_resource_names(resource_key, turn_name, rw_name);
    hyp_private_file_lock_t *probe = NULL;
    hyp_private_file_lock_status_t native_before_release =
        names_ok ? hyp_private_file_lock_try_acquire(fixture.directory, rw_name,
                                                     HYP_PRIVATE_FILE_LOCK_EX, &probe)
                 : HYP_PRIVATE_FILE_LOCK_IO;
    if (probe) {
        (void)hyp_private_file_lock_release(&probe);
    }
    hyp_private_file_lock_status_t free_while_waiter =
        started ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;
    bool registry_preserved = fixture.registry != NULL;

    hyp_private_file_lock_status_t first_cleanup_release =
        cleanup ? hyp_lock_lease_release(&cleanup) : HYP_PRIVATE_FILE_LOCK_IO;
    bool retained_after_native_failure = cleanup != NULL;
    size_t waiter_after_detach = hyp_lock_registry_waiter_count(fixture.registry);
    size_t pending_after_detach =
        hyp_lock_registry_pending_cleanup_count_for_test(fixture.registry);
    probe = NULL;
    hyp_private_file_lock_status_t native_while_pending =
        names_ok ? hyp_private_file_lock_try_acquire(fixture.directory, rw_name,
                                                     HYP_PRIVATE_FILE_LOCK_EX, &probe)
                 : HYP_PRIVATE_FILE_LOCK_IO;
    if (probe) {
        (void)hyp_private_file_lock_release(&probe);
    }
    hyp_private_file_lock_status_t free_while_pending =
        registry_preserved ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;

    hyp_private_file_lock_status_t second_cleanup_release =
        cleanup ? hyp_lock_lease_release(&cleanup) : HYP_PRIVATE_FILE_LOCK_IO;
    size_t waiter_after_cleanup = hyp_lock_registry_waiter_count(fixture.registry);
    size_t pending_after_cleanup =
        hyp_lock_registry_pending_cleanup_count_for_test(fixture.registry);
    hyp_lock_lease_t *after = NULL;
    hyp_private_file_lock_status_t after_status =
        cleanup_retained && fixture.registry
            ? hyp_lock_registry_acquire(fixture.registry, resource_key, HYP_PRIVATE_FILE_LOCK_EX,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &after)
            : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t after_release =
        after ? hyp_lock_lease_release(&after) : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t final_free = cleanup_retained && fixture.registry
                                                    ? hyp_lock_registry_free(&fixture.registry)
                                                    : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_TRUE(hook_set);
    ASSERT_TRUE(abort_fault_set);
    ASSERT_TRUE(release_fault_set);
    ASSERT_TRUE(reached_native_ready);
    ASSERT_EQ(status, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(cleanup_retained);
    ASSERT_TRUE(owns_rw);
    ASSERT_TRUE(owns_turn);
    ASSERT_TRUE(used_exact_lock_failure_path);
    ASSERT_EQ(waiter_before_release, 1);
    ASSERT_EQ(pending_before_release, 0);
    ASSERT_TRUE(names_ok);
    ASSERT_EQ(native_before_release, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_EQ(free_while_waiter, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_TRUE(registry_preserved);
    ASSERT_EQ(first_cleanup_release, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(retained_after_native_failure);
    ASSERT_EQ(waiter_after_detach, 0);
    ASSERT_EQ(pending_after_detach, 1);
    ASSERT_EQ(native_while_pending, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_EQ(free_while_pending, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_EQ(second_cleanup_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NULL(cleanup);
    ASSERT_EQ(waiter_after_cleanup, 0);
    ASSERT_EQ(pending_after_cleanup, 0);
    ASSERT_EQ(after_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(after_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(final_free, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
}
#endif

TEST(lock_registry_terminal_close_error_finishes_pending_accounting) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX consumed-close registry accounting runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *reader = NULL;
    hyp_private_file_lock_status_t acquired =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "terminal-close-accounting",
                                        HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &reader)
            : HYP_PRIVATE_FILE_LOCK_IO;
    bool retryable_close_set = hyp_lock_lease_fail_next_release_step_for_test(
        reader, HYP_LOCK_REGISTRY_RELEASE_RW, HYP_PRIVATE_FILE_LOCK_RELEASE_CLOSE);
    hyp_private_file_lock_status_t first_release =
        retryable_close_set ? hyp_lock_lease_release(&reader) : HYP_PRIVATE_FILE_LOCK_IO;
    bool retained_pending = reader != NULL;
    size_t active_pending = hyp_lock_registry_active_lease_count_for_test(fixture.registry);
    size_t cleanup_pending = hyp_lock_registry_pending_cleanup_count_for_test(fixture.registry);

    bool terminal_close_set =
        hyp_lock_lease_fail_close_after_consuming_for_test(reader, HYP_LOCK_REGISTRY_RELEASE_RW);
    hyp_private_file_lock_status_t terminal_release =
        terminal_close_set ? hyp_lock_lease_release(&reader) : HYP_PRIVATE_FILE_LOCK_IO;
    size_t cleanup_finished = hyp_lock_registry_pending_cleanup_count_for_test(fixture.registry);
    hyp_lock_lease_t *after = NULL;
    hyp_private_file_lock_status_t after_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "terminal-close-accounting",
                                        HYP_PRIVATE_FILE_LOCK_EX,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &after)
            : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t after_release =
        after ? hyp_lock_lease_release(&after) : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t final_free =
        started ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(acquired, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(retryable_close_set);
    ASSERT_EQ(first_release, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(retained_pending);
    ASSERT_EQ(active_pending, 0);
    ASSERT_EQ(cleanup_pending, 1);
    ASSERT_TRUE(terminal_close_set);
    ASSERT_EQ(terminal_release, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_NULL(reader);
    ASSERT_EQ(cleanup_finished, 0);
    ASSERT_EQ(after_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(after_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(final_free, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

TEST(lock_registry_abort_lock_failure_returns_waiter_cleanup_lease) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry abort cleanup runs on POSIX");
#else
    return lock_registry_abort_bookkeeping_failure_retains_cleanup(
        HYP_LOCK_REGISTRY_ABORT_FAIL_LOCK, "abort-lock-cleanup");
#endif
}

TEST(lock_registry_abort_remove_failure_returns_waiter_cleanup_lease) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry abort cleanup runs on POSIX");
#else
    return lock_registry_abort_bookkeeping_failure_retains_cleanup(
        HYP_LOCK_REGISTRY_ABORT_FAIL_REMOVE, "abort-remove-cleanup");
#endif
}

TEST(lock_registry_never_upgrades_shared_lease_in_place) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry RED runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *reader = NULL;
    hyp_private_file_lock_status_t reader_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "no-upgrade", HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &reader)
            : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_waiter_t writer = {.registry = fixture.registry,
                                     .resource_key = "no-upgrade",
                                     .mode = HYP_PRIVATE_FILE_LOCK_EX,
                                     .status = HYP_PRIVATE_FILE_LOCK_IO};
    atomic_init(&writer.cancel_token, false);
    atomic_init(&writer.finished, false);
    hyp_thread_t writer_thread;
    bool writer_started =
        reader_status == HYP_PRIVATE_FILE_LOCK_OK &&
        hyp_thread_create(&writer_thread, 0, lock_registry_waiter_run, &writer) == 0;
    char turn_name[HYP_LOCK_REGISTRY_NAME_CAP];
    char rw_name[HYP_LOCK_REGISTRY_NAME_CAP];
    bool names_ok = hyp_lock_registry_resource_names("no-upgrade", turn_name, rw_name);
    bool writer_has_turn = false;
    uint64_t deadline = hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS;
    while (writer_started && names_ok && hyp_now_ms() < deadline) {
        hyp_private_file_lock_t *probe = NULL;
        hyp_private_file_lock_status_t probe_status = hyp_private_file_lock_try_acquire(
            fixture.directory, turn_name, HYP_PRIVATE_FILE_LOCK_EX, &probe);
        if (probe_status == HYP_PRIVATE_FILE_LOCK_BUSY) {
            writer_has_turn = true;
            break;
        }
        if (probe) {
            (void)hyp_private_file_lock_release(&probe);
        }
        if (atomic_load_explicit(&writer.finished, memory_order_acquire)) {
            break;
        }
        lock_registry_test_yield();
    }
    bool finished_before_reader_release =
        atomic_load_explicit(&writer.finished, memory_order_acquire);
    hyp_private_file_lock_status_t reader_release =
        reader ? hyp_lock_lease_release(&reader) : HYP_PRIVATE_FILE_LOCK_IO;
    if (writer_started) {
        (void)hyp_thread_join(&writer_thread);
    }
    hyp_private_file_lock_status_t writer_release =
        writer.lease ? hyp_lock_lease_release(&writer.lease) : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(reader_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(writer_started);
    ASSERT_TRUE(names_ok);
    ASSERT_TRUE(writer_has_turn);
    ASSERT_FALSE(finished_before_reader_release);
    ASSERT_EQ(reader_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(writer.status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(writer_release, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

TEST(lock_registry_reader_close_failure_retains_lease_and_accounting) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry lifecycle runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *reader = NULL;
    hyp_private_file_lock_status_t acquired =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "retry-reader-close",
                                        HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &reader)
            : HYP_PRIVATE_FILE_LOCK_IO;
    bool fault_set = hyp_lock_lease_fail_next_release_step_for_test(
        reader, HYP_LOCK_REGISTRY_RELEASE_RW, HYP_PRIVATE_FILE_LOCK_RELEASE_CLOSE);
    hyp_private_file_lock_status_t first_release =
        fault_set ? hyp_lock_lease_release(&reader) : HYP_PRIVATE_FILE_LOCK_IO;
    bool retained = reader != NULL;
    size_t active_after_failure = hyp_lock_registry_active_lease_count_for_test(fixture.registry);
    size_t pending_after_failure =
        hyp_lock_registry_pending_cleanup_count_for_test(fixture.registry);

    hyp_lock_lease_t *blocked_writer = NULL;
    hyp_private_file_lock_status_t while_close_pending =
        started ? hyp_lock_registry_acquire(fixture.registry, "retry-reader-close",
                                            HYP_PRIVATE_FILE_LOCK_EX, hyp_now_ms() + 50, NULL,
                                            &blocked_writer)
                : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t blocked_writer_release =
        blocked_writer ? hyp_lock_lease_release(&blocked_writer) : HYP_PRIVATE_FILE_LOCK_IO;

    bool duplicate_unlock_fault_set = hyp_lock_lease_fail_next_release_step_for_test(
        reader, HYP_LOCK_REGISTRY_RELEASE_RW, HYP_PRIVATE_FILE_LOCK_RELEASE_UNLOCK);
    hyp_private_file_lock_status_t retry =
        reader ? hyp_lock_lease_release(&reader) : HYP_PRIVATE_FILE_LOCK_IO;
    if (reader) {
        (void)hyp_lock_lease_release(&reader);
    }
    size_t active_after_retry = hyp_lock_registry_active_lease_count_for_test(fixture.registry);
    hyp_lock_lease_t *after = NULL;
    hyp_private_file_lock_status_t after_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "retry-reader-close",
                                        HYP_PRIVATE_FILE_LOCK_EX,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &after)
            : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t after_release =
        after ? hyp_lock_lease_release(&after) : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(acquired, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(fault_set);
    ASSERT_EQ(first_release, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(retained);
    ASSERT_EQ(active_after_failure, 0);
    ASSERT_EQ(pending_after_failure, 1);
    ASSERT_EQ(while_close_pending, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NULL(blocked_writer);
    ASSERT_EQ(blocked_writer_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(duplicate_unlock_fault_set);
    ASSERT_EQ(retry, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NULL(reader);
    ASSERT_EQ(active_after_retry, 0);
    ASSERT_EQ(after_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(after_release, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

TEST(lock_registry_writer_partial_release_retries_rw_then_turn) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry lifecycle runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *writer = NULL;
    hyp_private_file_lock_status_t acquired =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "retry-writer-turn",
                                        HYP_PRIVATE_FILE_LOCK_EX,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &writer)
            : HYP_PRIVATE_FILE_LOCK_IO;
    bool fault_set = hyp_lock_lease_fail_next_release_step_for_test(
        writer, HYP_LOCK_REGISTRY_RELEASE_TURN, HYP_PRIVATE_FILE_LOCK_RELEASE_UNLOCK);
    hyp_private_file_lock_status_t first_release =
        fault_set ? hyp_lock_lease_release(&writer) : HYP_PRIVATE_FILE_LOCK_IO;
    bool retained = writer != NULL;
    bool rw_closed =
        !hyp_lock_lease_has_release_handle_for_test(writer, HYP_LOCK_REGISTRY_RELEASE_RW);
    bool turn_retained =
        hyp_lock_lease_has_release_handle_for_test(writer, HYP_LOCK_REGISTRY_RELEASE_TURN);
    size_t active_after_failure = hyp_lock_registry_active_lease_count_for_test(fixture.registry);
    size_t pending_after_failure =
        hyp_lock_registry_pending_cleanup_count_for_test(fixture.registry);

    char turn_name[HYP_LOCK_REGISTRY_NAME_CAP];
    char rw_name[HYP_LOCK_REGISTRY_NAME_CAP];
    bool names_ok = hyp_lock_registry_resource_names("retry-writer-turn", turn_name, rw_name);
    hyp_private_file_lock_t *rw_probe = NULL;
    hyp_private_file_lock_status_t rw_status =
        names_ok ? hyp_private_file_lock_try_acquire(fixture.directory, rw_name,
                                                     HYP_PRIVATE_FILE_LOCK_EX, &rw_probe)
                 : HYP_PRIVATE_FILE_LOCK_IO;
    if (rw_probe) {
        (void)hyp_private_file_lock_release(&rw_probe);
    }
    hyp_private_file_lock_t *turn_probe = NULL;
    hyp_private_file_lock_status_t turn_status =
        names_ok ? hyp_private_file_lock_try_acquire(fixture.directory, turn_name,
                                                     HYP_PRIVATE_FILE_LOCK_EX, &turn_probe)
                 : HYP_PRIVATE_FILE_LOCK_IO;
    if (turn_probe) {
        (void)hyp_private_file_lock_release(&turn_probe);
    }

    hyp_lock_lease_t *next_writer = NULL;
    hyp_private_file_lock_status_t next_writer_status =
        started ? hyp_lock_registry_acquire(fixture.registry, "retry-writer-turn",
                                            HYP_PRIVATE_FILE_LOCK_EX, hyp_now_ms() + 50, NULL,
                                            &next_writer)
                : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t next_writer_release =
        next_writer ? hyp_lock_lease_release(&next_writer) : HYP_PRIVATE_FILE_LOCK_IO;

    hyp_private_file_lock_status_t retry =
        writer ? hyp_lock_lease_release(&writer) : HYP_PRIVATE_FILE_LOCK_IO;
    if (writer) {
        (void)hyp_lock_lease_release(&writer);
    }
    size_t active_after_retry = hyp_lock_registry_active_lease_count_for_test(fixture.registry);
    hyp_lock_lease_t *after = NULL;
    hyp_private_file_lock_status_t after_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "retry-writer-turn",
                                        HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &after)
            : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t after_release =
        after ? hyp_lock_lease_release(&after) : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(acquired, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(fault_set);
    ASSERT_EQ(first_release, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(retained);
    ASSERT_TRUE(rw_closed);
    ASSERT_TRUE(turn_retained);
    ASSERT_EQ(active_after_failure, 0);
    ASSERT_EQ(pending_after_failure, 1);
    ASSERT_TRUE(names_ok);
    ASSERT_EQ(rw_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(turn_status, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_EQ(next_writer_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(next_writer_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(retry, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NULL(writer);
    ASSERT_EQ(active_after_retry, 0);
    ASSERT_EQ(after_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(after_release, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

TEST(lock_registry_free_refuses_active_lease) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry lifecycle runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *lease = NULL;
    hyp_private_file_lock_status_t acquired =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "free-active", HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &lease)
            : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t busy =
        started ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;
    bool registry_preserved = fixture.registry != NULL;
    hyp_private_file_lock_status_t released =
        lease ? hyp_lock_lease_release(&lease) : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t freed =
        registry_preserved ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;
    bool registry_cleared = fixture.registry == NULL;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(acquired, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(busy, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_TRUE(registry_preserved);
    ASSERT_EQ(released, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(freed, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(registry_cleared);
    PASS();
#endif
}

TEST(lock_registry_free_retires_identity_and_rejects_stale_pointer) {
#ifdef _WIN32
    SKIP_PLATFORM("native registry fixture retirement runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_registry_t *stale = fixture.registry;
    hyp_private_file_lock_status_t first_free =
        started ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;
    bool caller_cleared = fixture.registry == NULL;
    bool retired = hyp_lock_registry_is_retired_for_test(stale);

    hyp_lock_registry_t *fresh = started ? hyp_lock_registry_new(fixture.directory) : NULL;
    bool identity_not_reused = fresh && fresh != stale;
    hyp_lock_lease_t *stale_lease = NULL;
    hyp_private_file_lock_status_t stale_acquire = HYP_PRIVATE_FILE_LOCK_IO;
    hyp_lock_cancel_token_t stale_cancel_token;
    atomic_init(&stale_cancel_token, false);
    hyp_private_file_lock_status_t stale_cancel = HYP_PRIVATE_FILE_LOCK_OK;
    hyp_private_file_lock_status_t stale_free = HYP_PRIVATE_FILE_LOCK_IO;
    bool stale_free_preserved = true;
    if (retired && identity_not_reused) {
        stale_acquire = hyp_lock_registry_acquire(
            stale, "retired-registry", HYP_PRIVATE_FILE_LOCK_SH,
            hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &stale_lease);
        stale_cancel = hyp_lock_registry_request_cancel(stale, &stale_cancel_token);
        hyp_lock_registry_t *stale_copy = stale;
        stale_free = hyp_lock_registry_free(&stale_copy);
        stale_free_preserved = stale_copy == stale;
    }

    hyp_lock_lease_t *fresh_lease = NULL;
    hyp_private_file_lock_status_t fresh_acquire =
        fresh ? hyp_lock_registry_acquire(fresh, "retired-registry", HYP_PRIVATE_FILE_LOCK_SH,
                                          hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL,
                                          &fresh_lease)
              : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t fresh_release =
        fresh_lease ? hyp_lock_lease_release(&fresh_lease) : HYP_PRIVATE_FILE_LOCK_IO;
    fixture.registry = fresh;
    hyp_private_file_lock_status_t fresh_free =
        fresh ? hyp_lock_registry_free(&fixture.registry) : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(first_free, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(caller_cleared);
    ASSERT_TRUE(retired);
    ASSERT_TRUE(identity_not_reused);
    ASSERT_EQ(stale_acquire, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_NULL(stale_lease);
    ASSERT_EQ(stale_cancel, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(atomic_load_explicit(&stale_cancel_token, memory_order_acquire));
    ASSERT_EQ(stale_free, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(stale_free_preserved);
    ASSERT_EQ(fresh_acquire, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(fresh_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(fresh_free, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

TEST(lock_registry_large_queue_parks_non_head_waiters) {
#ifdef _WIN32
    SKIP_PLATFORM("native registry parking fixture runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *holder = NULL;
    hyp_private_file_lock_status_t holder_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "large-queue-parking",
                                        HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &holder)
            : HYP_PRIVATE_FILE_LOCK_IO;

    lock_registry_waiter_t waiters[LOCK_REGISTRY_PARKING_WAITERS];
    hyp_thread_t threads[LOCK_REGISTRY_PARKING_WAITERS];
    size_t thread_count = 0;
    memset(waiters, 0, sizeof(waiters));
    for (;
         holder_status == HYP_PRIVATE_FILE_LOCK_OK && thread_count < LOCK_REGISTRY_PARKING_WAITERS;
         thread_count++) {
        waiters[thread_count] = (lock_registry_waiter_t){
            .registry = fixture.registry,
            .resource_key = "large-queue-parking",
            .mode = HYP_PRIVATE_FILE_LOCK_EX,
            .status = HYP_PRIVATE_FILE_LOCK_IO,
        };
        atomic_init(&waiters[thread_count].cancel_token, false);
        atomic_init(&waiters[thread_count].finished, false);
        if (hyp_thread_create(&threads[thread_count], 0, lock_registry_waiter_run,
                              &waiters[thread_count]) != 0) {
            break;
        }
    }

    bool all_queued = false;
    bool tails_parked = false;
    uint64_t observe_deadline = hyp_now_ms() + 500;
    while (thread_count == LOCK_REGISTRY_PARKING_WAITERS && hyp_now_ms() < observe_deadline) {
        all_queued =
            hyp_lock_registry_waiter_count(fixture.registry) == LOCK_REGISTRY_PARKING_WAITERS;
        tails_parked = hyp_lock_registry_condition_waiter_count_for_test(fixture.registry) >=
                       LOCK_REGISTRY_PARKING_WAITERS - 1;
        if (all_queued && tails_parked) {
            break;
        }
        lock_registry_test_yield();
    }
    size_t attempting = hyp_lock_registry_attempting_waiter_count_for_test(fixture.registry);
    uint64_t waits_before = hyp_lock_registry_condition_wait_call_count_for_test(fixture.registry);
    hyp_usleep(25000);
    uint64_t waits_after = hyp_lock_registry_condition_wait_call_count_for_test(fixture.registry);

    for (size_t index = 0; index < thread_count; index++) {
        (void)hyp_lock_registry_request_cancel(fixture.registry, &waiters[index].cancel_token);
    }
    bool all_cancelled = true;
    for (size_t index = 0; index < thread_count; index++) {
        all_cancelled = hyp_thread_join(&threads[index]) == 0 &&
                        waiters[index].status == HYP_PRIVATE_FILE_LOCK_BUSY && all_cancelled;
        if (waiters[index].lease) {
            (void)hyp_lock_lease_release(&waiters[index].lease);
        }
    }
    hyp_private_file_lock_status_t holder_release =
        holder ? hyp_lock_lease_release(&holder) : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(holder_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(thread_count, LOCK_REGISTRY_PARKING_WAITERS);
    ASSERT_TRUE(all_queued);
    ASSERT_TRUE(tails_parked);
    ASSERT_EQ(attempting, 1);
    ASSERT_GTE(waits_before, LOCK_REGISTRY_PARKING_WAITERS - 1);
    ASSERT_TRUE(waits_after - waits_before < 128);
    ASSERT_TRUE(all_cancelled);
    ASSERT_EQ(holder_release, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

TEST(lock_registry_cancel_request_wakes_parked_tail) {
#ifdef _WIN32
    SKIP_PLATFORM("native registry cancellation fixture runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *holder = NULL;
    hyp_private_file_lock_status_t holder_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "cancel-parked-tail",
                                        HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &holder)
            : HYP_PRIVATE_FILE_LOCK_IO;

    lock_registry_waiter_t head = {.registry = fixture.registry,
                                   .resource_key = "cancel-parked-tail",
                                   .mode = HYP_PRIVATE_FILE_LOCK_EX,
                                   .status = HYP_PRIVATE_FILE_LOCK_IO};
    atomic_init(&head.cancel_token, false);
    atomic_init(&head.finished, false);
    hyp_thread_t head_thread;
    bool head_started = holder_status == HYP_PRIVATE_FILE_LOCK_OK &&
                        hyp_thread_create(&head_thread, 0, lock_registry_waiter_run, &head) == 0;
    bool head_attempting = false;
    uint64_t head_deadline = hyp_now_ms() + 500;
    while (head_started && hyp_now_ms() < head_deadline) {
        head_attempting = hyp_lock_registry_waiter_count(fixture.registry) == 1 &&
                          hyp_lock_registry_attempting_waiter_count_for_test(fixture.registry) == 1;
        if (head_attempting) {
            break;
        }
        lock_registry_test_yield();
    }

    lock_registry_waiter_t tail = {.registry = fixture.registry,
                                   .resource_key = "cancel-parked-tail",
                                   .mode = HYP_PRIVATE_FILE_LOCK_EX,
                                   .status = HYP_PRIVATE_FILE_LOCK_IO};
    atomic_init(&tail.cancel_token, false);
    atomic_init(&tail.finished, false);
    hyp_thread_t tail_thread;
    bool tail_started =
        head_attempting && hyp_thread_create(&tail_thread, 0, lock_registry_waiter_run, &tail) == 0;
    bool tail_parked = false;
    uint64_t tail_deadline = hyp_now_ms() + 500;
    while (tail_started && hyp_now_ms() < tail_deadline) {
        tail_parked = hyp_lock_registry_waiter_count(fixture.registry) == 2 &&
                      hyp_lock_registry_attempting_waiter_count_for_test(fixture.registry) == 1 &&
                      hyp_lock_registry_condition_waiter_count_for_test(fixture.registry) >= 1;
        if (tail_parked) {
            break;
        }
        lock_registry_test_yield();
    }

    hyp_private_file_lock_status_t cancel_status =
        tail_parked ? hyp_lock_registry_request_cancel(fixture.registry, &tail.cancel_token)
                    : HYP_PRIVATE_FILE_LOCK_IO;
    bool tail_woke = false;
    uint64_t wake_deadline = hyp_now_ms() + 500;
    while (tail_started && hyp_now_ms() < wake_deadline) {
        tail_woke = atomic_load_explicit(&tail.finished, memory_order_acquire);
        if (tail_woke) {
            break;
        }
        lock_registry_test_yield();
    }
    bool head_still_waiting =
        head_started && !atomic_load_explicit(&head.finished, memory_order_acquire);

    if (head_started) {
        (void)hyp_lock_registry_request_cancel(fixture.registry, &head.cancel_token);
    }
    hyp_private_file_lock_status_t holder_release =
        holder ? hyp_lock_lease_release(&holder) : HYP_PRIVATE_FILE_LOCK_IO;
    if (tail_started) {
        (void)hyp_thread_join(&tail_thread);
    }
    if (head_started) {
        (void)hyp_thread_join(&head_thread);
    }
    size_t remaining_waiters =
        started ? hyp_lock_registry_waiter_count(fixture.registry) : (size_t)-1;
    if (tail.lease) {
        (void)hyp_lock_lease_release(&tail.lease);
    }
    if (head.lease) {
        (void)hyp_lock_lease_release(&head.lease);
    }
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(holder_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(head_started);
    ASSERT_TRUE(head_attempting);
    ASSERT_TRUE(tail_started);
    ASSERT_TRUE(tail_parked);
    ASSERT_EQ(cancel_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(tail_woke);
    ASSERT_TRUE(head_still_waiting);
    ASSERT_EQ(tail.status, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_NULL(tail.lease);
    ASSERT_EQ(head.status, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_NULL(head.lease);
    ASSERT_EQ(holder_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(remaining_waiters, 0);
    PASS();
#endif
}

typedef struct {
    hyp_lock_registry_t *registry;
    hyp_lock_cancel_token_t cancel_token;
    atomic_bool ready;
    atomic_bool go;
    atomic_bool finished;
    uint64_t deadline_ms;
    uint64_t returned_ms;
    hyp_private_file_lock_status_t status;
    hyp_lock_lease_t *lease;
} lock_registry_deadline_waiter_t;

#ifndef _WIN32
static void *lock_registry_deadline_waiter_run(void *opaque) {
    lock_registry_deadline_waiter_t *waiter = opaque;
    atomic_store_explicit(&waiter->ready, true, memory_order_release);
    while (!atomic_load_explicit(&waiter->go, memory_order_acquire)) {
        lock_registry_test_yield();
    }
    waiter->status =
        hyp_lock_registry_acquire(waiter->registry, "absolute-deadline", HYP_PRIVATE_FILE_LOCK_EX,
                                  waiter->deadline_ms, &waiter->cancel_token, &waiter->lease);
    waiter->returned_ms = hyp_now_ms();
    atomic_store_explicit(&waiter->finished, true, memory_order_release);
    return NULL;
}
#endif

TEST(lock_registry_absolute_deadline_survives_repeated_wakes) {
#ifdef _WIN32
    SKIP_PLATFORM("native registry deadline fixture runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *holder = NULL;
    hyp_private_file_lock_status_t holder_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "absolute-deadline",
                                        HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &holder)
            : HYP_PRIVATE_FILE_LOCK_IO;

    lock_registry_waiter_t head = {.registry = fixture.registry,
                                   .resource_key = "absolute-deadline",
                                   .mode = HYP_PRIVATE_FILE_LOCK_EX,
                                   .status = HYP_PRIVATE_FILE_LOCK_IO};
    atomic_init(&head.cancel_token, false);
    atomic_init(&head.finished, false);
    hyp_thread_t head_thread;
    bool head_started = holder_status == HYP_PRIVATE_FILE_LOCK_OK &&
                        hyp_thread_create(&head_thread, 0, lock_registry_waiter_run, &head) == 0;
    bool head_attempting = false;
    uint64_t head_deadline = hyp_now_ms() + 500;
    while (head_started && hyp_now_ms() < head_deadline) {
        head_attempting = hyp_lock_registry_waiter_count(fixture.registry) == 1 &&
                          hyp_lock_registry_attempting_waiter_count_for_test(fixture.registry) == 1;
        if (head_attempting) {
            break;
        }
        lock_registry_test_yield();
    }

    lock_registry_deadline_waiter_t tail = {.registry = fixture.registry,
                                            .status = HYP_PRIVATE_FILE_LOCK_IO};
    atomic_init(&tail.cancel_token, false);
    atomic_init(&tail.ready, false);
    atomic_init(&tail.go, false);
    atomic_init(&tail.finished, false);
    hyp_thread_t tail_thread;
    bool tail_started =
        head_attempting &&
        hyp_thread_create(&tail_thread, 0, lock_registry_deadline_waiter_run, &tail) == 0;
    uint64_t ready_deadline = hyp_now_ms() + 500;
    while (tail_started && !atomic_load_explicit(&tail.ready, memory_order_acquire) &&
           hyp_now_ms() < ready_deadline) {
        lock_registry_test_yield();
    }
    bool tail_ready = tail_started && atomic_load_explicit(&tail.ready, memory_order_acquire);
    uint64_t deadline_start = hyp_now_ms();
    tail.deadline_ms = deadline_start + 200;
    atomic_store_explicit(&tail.go, true, memory_order_release);

    bool tail_queued = false;
    uint64_t queue_deadline = deadline_start + 100;
    while (tail_ready && hyp_now_ms() < queue_deadline) {
        tail_queued = hyp_lock_registry_waiter_count(fixture.registry) == 2 &&
                      hyp_lock_registry_attempting_waiter_count_for_test(fixture.registry) == 1;
        if (tail_queued) {
            break;
        }
        lock_registry_test_yield();
    }

    hyp_lock_cancel_token_t unrelated_token;
    atomic_init(&unrelated_token, false);
    bool broadcasts_ok = true;
    uint64_t observe_deadline = deadline_start + 600;
    while (tail_ready && !atomic_load_explicit(&tail.finished, memory_order_acquire) &&
           hyp_now_ms() < observe_deadline) {
        broadcasts_ok = hyp_lock_registry_request_cancel(fixture.registry, &unrelated_token) ==
                            HYP_PRIVATE_FILE_LOCK_OK &&
                        broadcasts_ok;
        hyp_usleep(5000);
    }
    bool returned_at_deadline =
        tail_ready && atomic_load_explicit(&tail.finished, memory_order_acquire);
    uint64_t elapsed_ms = returned_at_deadline ? tail.returned_ms - deadline_start : UINT64_MAX;

    if (!returned_at_deadline && tail_started) {
        (void)hyp_lock_registry_request_cancel(fixture.registry, &tail.cancel_token);
    }
    if (head_started) {
        (void)hyp_lock_registry_request_cancel(fixture.registry, &head.cancel_token);
    }
    hyp_private_file_lock_status_t holder_release =
        holder ? hyp_lock_lease_release(&holder) : HYP_PRIVATE_FILE_LOCK_IO;
    if (tail_started) {
        (void)hyp_thread_join(&tail_thread);
    }
    if (head_started) {
        (void)hyp_thread_join(&head_thread);
    }
    if (tail.lease) {
        (void)hyp_lock_lease_release(&tail.lease);
    }
    if (head.lease) {
        (void)hyp_lock_lease_release(&head.lease);
    }
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(holder_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(head_started);
    ASSERT_TRUE(head_attempting);
    ASSERT_TRUE(tail_started);
    ASSERT_TRUE(tail_ready);
    ASSERT_TRUE(tail_queued);
    ASSERT_TRUE(broadcasts_ok);
    ASSERT_TRUE(returned_at_deadline);
    ASSERT_GTE(elapsed_ms, 150);
    ASSERT_TRUE(elapsed_ms < 350);
    ASSERT_EQ(tail.status, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_NULL(tail.lease);
    ASSERT_EQ(head.status, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_NULL(head.lease);
    ASSERT_EQ(holder_release, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

typedef struct {
    hyp_lock_registry_t *registry;
    unsigned int id;
    atomic_int *ready;
    atomic_bool *go;
    atomic_int *readers;
    atomic_int *writers;
    atomic_int *violations;
} lock_registry_stress_worker_t;

#ifndef _WIN32
static void *lock_registry_stress_run(void *opaque) {
    lock_registry_stress_worker_t *worker = opaque;
    (void)atomic_fetch_add_explicit(worker->ready, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(worker->go, memory_order_acquire)) {
        lock_registry_test_yield();
    }
    for (unsigned int iteration = 0; iteration < LOCK_REGISTRY_STRESS_ITERATIONS; iteration++) {
        hyp_private_file_lock_mode_t mode = (iteration + worker->id) % 5U == 0U
                                                ? HYP_PRIVATE_FILE_LOCK_EX
                                                : HYP_PRIVATE_FILE_LOCK_SH;
        hyp_lock_lease_t *lease = NULL;
        hyp_private_file_lock_status_t status =
            hyp_lock_registry_acquire(worker->registry, "stress", mode,
                                      hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &lease);
        if (status != HYP_PRIVATE_FILE_LOCK_OK || !lease) {
            (void)atomic_fetch_add_explicit(worker->violations, 1, memory_order_relaxed);
            return NULL;
        }
        if (mode == HYP_PRIVATE_FILE_LOCK_EX) {
            if (atomic_fetch_add_explicit(worker->writers, 1, memory_order_acq_rel) != 0 ||
                atomic_load_explicit(worker->readers, memory_order_acquire) != 0) {
                (void)atomic_fetch_add_explicit(worker->violations, 1, memory_order_relaxed);
            }
            lock_registry_test_yield();
            if (atomic_load_explicit(worker->readers, memory_order_acquire) != 0) {
                (void)atomic_fetch_add_explicit(worker->violations, 1, memory_order_relaxed);
            }
            (void)atomic_fetch_sub_explicit(worker->writers, 1, memory_order_acq_rel);
        } else {
            if (atomic_load_explicit(worker->writers, memory_order_acquire) != 0) {
                (void)atomic_fetch_add_explicit(worker->violations, 1, memory_order_relaxed);
            }
            (void)atomic_fetch_add_explicit(worker->readers, 1, memory_order_acq_rel);
            if (atomic_load_explicit(worker->writers, memory_order_acquire) != 0) {
                (void)atomic_fetch_add_explicit(worker->violations, 1, memory_order_relaxed);
            }
            lock_registry_test_yield();
            (void)atomic_fetch_sub_explicit(worker->readers, 1, memory_order_acq_rel);
        }
        if (hyp_lock_lease_release(&lease) != HYP_PRIVATE_FILE_LOCK_OK) {
            (void)atomic_fetch_add_explicit(worker->violations, 1, memory_order_relaxed);
            return NULL;
        }
    }
    return NULL;
}
#endif

TEST(lock_registry_concurrent_shared_exclusive_stress) {
#ifdef _WIN32
    SKIP_PLATFORM("POSIX native-directory registry RED runs on POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_thread_t threads[LOCK_REGISTRY_STRESS_THREADS];
    lock_registry_stress_worker_t workers[LOCK_REGISTRY_STRESS_THREADS];
    atomic_int ready;
    atomic_bool go;
    atomic_int readers;
    atomic_int writers;
    atomic_int violations;
    atomic_init(&ready, 0);
    atomic_init(&go, false);
    atomic_init(&readers, 0);
    atomic_init(&writers, 0);
    atomic_init(&violations, 0);
    size_t created = 0;
    for (; started && created < LOCK_REGISTRY_STRESS_THREADS; created++) {
        workers[created] = (lock_registry_stress_worker_t){
            .registry = fixture.registry,
            .id = (unsigned int)created,
            .ready = &ready,
            .go = &go,
            .readers = &readers,
            .writers = &writers,
            .violations = &violations,
        };
        if (hyp_thread_create(&threads[created], 0, lock_registry_stress_run, &workers[created]) !=
            0) {
            break;
        }
    }
    uint64_t ready_deadline = hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS;
    while (created == LOCK_REGISTRY_STRESS_THREADS &&
           atomic_load_explicit(&ready, memory_order_acquire) != LOCK_REGISTRY_STRESS_THREADS &&
           hyp_now_ms() < ready_deadline) {
        lock_registry_test_yield();
    }
    bool all_ready =
        atomic_load_explicit(&ready, memory_order_acquire) == LOCK_REGISTRY_STRESS_THREADS;
    atomic_store_explicit(&go, true, memory_order_release);
    for (size_t index = 0; index < created; index++) {
        (void)hyp_thread_join(&threads[index]);
    }
    int violation_count = atomic_load_explicit(&violations, memory_order_acquire);
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(created, LOCK_REGISTRY_STRESS_THREADS);
    ASSERT_TRUE(all_ready);
    ASSERT_EQ(violation_count, 0);
    PASS();
#endif
}

#ifndef _WIN32
static bool lock_registry_pipe_write(int fd, char value) {
    ssize_t written;
    do {
        written = write(fd, &value, 1);
    } while (written < 0 && errno == EINTR);
    return written == 1;
}

static bool lock_registry_pipe_read(int fd, uint32_t timeout_ms, char *value_out) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
    int ready;
    do {
        ready = poll(&descriptor, 1, (int)timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready != 1 || (descriptor.revents & POLLIN) == 0) {
        return false;
    }
    ssize_t received;
    do {
        received = read(fd, value_out, 1);
    } while (received < 0 && errno == EINTR);
    return received == 1;
}

static bool lock_registry_child_wait(pid_t child, int *status_out) {
    uint64_t deadline = hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS;
    while (hyp_now_ms() < deadline) {
        pid_t result = waitpid(child, status_out, WNOHANG);
        if (result == child) {
            return true;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        lock_registry_test_yield();
    }
    (void)kill(child, SIGKILL);
    return waitpid(child, status_out, 0) == child && false;
}

static int lock_registry_writer_child(const char *root, int started_fd, int acquired_fd,
                                      int release_fd) {
    hyp_private_lock_directory_t *directory = lock_registry_test_directory_open(root);
    hyp_lock_registry_t *registry = hyp_lock_registry_new(directory);
    bool started = registry && lock_registry_pipe_write(started_fd, 'S');
    hyp_lock_lease_t *lease = NULL;
    hyp_private_file_lock_status_t status =
        started
            ? hyp_lock_registry_acquire(registry, "writer-preference", HYP_PRIVATE_FILE_LOCK_EX,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &lease)
            : HYP_PRIVATE_FILE_LOCK_IO;
    bool reported =
        lock_registry_pipe_write(acquired_fd, status == HYP_PRIVATE_FILE_LOCK_OK ? 'W' : 'E');
    char command = 0;
    bool released = status == HYP_PRIVATE_FILE_LOCK_OK &&
                    lock_registry_pipe_read(release_fd, LOCK_REGISTRY_TEST_TIMEOUT_MS, &command) &&
                    command == 'X' && hyp_lock_lease_release(&lease) == HYP_PRIVATE_FILE_LOCK_OK;
    hyp_private_file_lock_status_t freed = hyp_lock_registry_free(&registry);
    hyp_private_lock_directory_close(directory);
    return started && reported && released && freed == HYP_PRIVATE_FILE_LOCK_OK ? 0 : 1;
}

typedef struct {
    int fd;
    bool reported;
} lock_registry_stage_pipe_t;

static void lock_registry_stage_pipe_report(void *opaque, hyp_private_file_lock_mode_t mode,
                                            hyp_lock_registry_stage_t stage) {
    lock_registry_stage_pipe_t *stage_pipe = opaque;
    if (!stage_pipe->reported && mode == HYP_PRIVATE_FILE_LOCK_SH &&
        stage == HYP_LOCK_REGISTRY_STAGE_TURN_BUSY) {
        stage_pipe->reported = lock_registry_pipe_write(stage_pipe->fd, 'T');
    }
}

static int lock_registry_reader_child(const char *root, int acquired_fd, int attempted_fd) {
    hyp_private_lock_directory_t *directory = lock_registry_test_directory_open(root);
    hyp_lock_registry_t *registry = hyp_lock_registry_new(directory);
    lock_registry_stage_pipe_t stage_pipe = {.fd = attempted_fd};
    bool hook_set = registry && hyp_lock_registry_set_stage_hook_for_test(
                                    registry, lock_registry_stage_pipe_report, &stage_pipe);
    hyp_lock_lease_t *lease = NULL;
    hyp_private_file_lock_status_t status =
        hook_set
            ? hyp_lock_registry_acquire(registry, "writer-preference", HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &lease)
            : HYP_PRIVATE_FILE_LOCK_IO;
    bool reported =
        lock_registry_pipe_write(acquired_fd, status == HYP_PRIVATE_FILE_LOCK_OK ? 'R' : 'E');
    bool released = status == HYP_PRIVATE_FILE_LOCK_OK &&
                    hyp_lock_lease_release(&lease) == HYP_PRIVATE_FILE_LOCK_OK;
    hyp_private_file_lock_status_t freed = hyp_lock_registry_free(&registry);
    hyp_private_lock_directory_close(directory);
    return hook_set && stage_pipe.reported && reported && released &&
                   freed == HYP_PRIVATE_FILE_LOCK_OK
               ? 0
               : 1;
}

static int lock_registry_inherited_child(hyp_lock_registry_t *registry,
                                         hyp_lock_lease_t *inherited_lease, int report_fd) {
    hyp_lock_lease_t *new_lease = NULL;
    hyp_private_file_lock_status_t acquire_status =
        hyp_lock_registry_acquire(registry, "fork-registry", HYP_PRIVATE_FILE_LOCK_EX,
                                  hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &new_lease);
    hyp_private_file_lock_status_t release_status = hyp_lock_lease_release(&inherited_lease);
    hyp_private_file_lock_status_t free_status = hyp_lock_registry_free(&registry);
    bool reported = lock_registry_pipe_write(report_fd, (char)acquire_status) &&
                    lock_registry_pipe_write(report_fd, (char)release_status) &&
                    lock_registry_pipe_write(report_fd, (char)free_status);
    if (new_lease) {
        (void)hyp_lock_lease_release(&new_lease);
    }
    return reported ? 0 : 1;
}
#endif

TEST(lock_registry_cross_process_writer_beats_late_reader) {
#ifdef _WIN32
    SKIP_PLATFORM("fork/pipe writer-preference proof applies only to POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *initial_reader = NULL;
    hyp_private_file_lock_status_t initial_status =
        started ? hyp_lock_registry_acquire(
                      fixture.registry, "writer-preference", HYP_PRIVATE_FILE_LOCK_SH,
                      hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &initial_reader)
                : HYP_PRIVATE_FILE_LOCK_IO;
    int writer_started[2] = {-1, -1};
    int writer_acquired[2] = {-1, -1};
    int writer_release[2] = {-1, -1};
    bool writer_pipes = initial_status == HYP_PRIVATE_FILE_LOCK_OK && pipe(writer_started) == 0 &&
                        pipe(writer_acquired) == 0 && pipe(writer_release) == 0;
    pid_t writer = writer_pipes ? fork() : -1;
    if (writer == 0) {
        (void)close(writer_started[0]);
        (void)close(writer_acquired[0]);
        (void)close(writer_release[1]);
        int child_result = lock_registry_writer_child(fixture.root, writer_started[1],
                                                      writer_acquired[1], writer_release[0]);
        _exit(child_result);
    }

    char report = 0;
    bool writer_announced = false;
    bool writer_has_turn = false;
    char turn_name[HYP_LOCK_REGISTRY_NAME_CAP];
    char rw_name[HYP_LOCK_REGISTRY_NAME_CAP];
    bool names_ok = hyp_lock_registry_resource_names("writer-preference", turn_name, rw_name);
    if (writer > 0) {
        (void)close(writer_started[1]);
        (void)close(writer_acquired[1]);
        (void)close(writer_release[0]);
        writer_announced =
            lock_registry_pipe_read(writer_started[0], LOCK_REGISTRY_TEST_TIMEOUT_MS, &report) &&
            report == 'S';
        uint64_t deadline = hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS;
        while (writer_announced && names_ok && hyp_now_ms() < deadline) {
            hyp_private_file_lock_t *probe = NULL;
            hyp_private_file_lock_status_t probe_status = hyp_private_file_lock_try_acquire(
                fixture.directory, turn_name, HYP_PRIVATE_FILE_LOCK_EX, &probe);
            if (probe_status == HYP_PRIVATE_FILE_LOCK_BUSY) {
                writer_has_turn = true;
                break;
            }
            if (probe) {
                (void)hyp_private_file_lock_release(&probe);
            }
            if (probe_status != HYP_PRIVATE_FILE_LOCK_OK) {
                break;
            }
            lock_registry_test_yield();
        }
    }

    int late_attempted[2] = {-1, -1};
    int late_acquired[2] = {-1, -1};
    bool late_pipe = writer_has_turn && pipe(late_attempted) == 0 && pipe(late_acquired) == 0;
    pid_t late_reader = late_pipe ? fork() : -1;
    if (late_reader == 0) {
        (void)close(late_attempted[0]);
        (void)close(late_acquired[0]);
        (void)close(writer_started[0]);
        (void)close(writer_acquired[0]);
        (void)close(writer_release[1]);
        _exit(lock_registry_reader_child(fixture.root, late_acquired[1], late_attempted[1]));
    }
    if (late_reader > 0) {
        (void)close(late_attempted[1]);
        (void)close(late_acquired[1]);
    }

    char attempted_report = 0;
    bool late_reached_turn =
        late_reader > 0 &&
        lock_registry_pipe_read(late_attempted[0], LOCK_REGISTRY_TEST_TIMEOUT_MS,
                                &attempted_report) &&
        attempted_report == 'T';

    /* All forks are complete before introducing a second parent thread. */
    lock_registry_waiter_t local_reader = {
        .registry = fixture.registry,
        .resource_key = "writer-preference",
        .mode = HYP_PRIVATE_FILE_LOCK_SH,
        .status = HYP_PRIVATE_FILE_LOCK_IO,
    };
    atomic_init(&local_reader.cancel_token, false);
    atomic_init(&local_reader.finished, false);
    hyp_thread_t local_reader_thread;
    bool local_reader_started =
        late_reached_turn &&
        hyp_thread_create(&local_reader_thread, 0, lock_registry_waiter_run, &local_reader) == 0;
    bool local_reader_queued = false;
    uint64_t local_deadline = hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS;
    while (local_reader_started && hyp_now_ms() < local_deadline) {
        if (hyp_lock_registry_waiter_count(fixture.registry) == 1) {
            local_reader_queued = true;
            break;
        }
        if (atomic_load_explicit(&local_reader.finished, memory_order_acquire)) {
            break;
        }
        lock_registry_test_yield();
    }

    hyp_private_file_lock_status_t initial_release =
        initial_reader ? hyp_lock_lease_release(&initial_reader) : HYP_PRIVATE_FILE_LOCK_IO;
    char writer_report = 0;
    bool writer_won = writer > 0 &&
                      lock_registry_pipe_read(writer_acquired[0], LOCK_REGISTRY_TEST_TIMEOUT_MS,
                                              &writer_report) &&
                      writer_report == 'W';
    struct pollfd late_probe = {
        .fd = late_reader > 0 ? late_acquired[0] : -1,
        .events = POLLIN,
        .revents = 0,
    };
    int late_ready_while_writer = late_reader > 0 ? poll(&late_probe, 1, 0) : -1;
    bool local_ready_while_writer =
        atomic_load_explicit(&local_reader.finished, memory_order_acquire);
    bool writer_commanded = writer_won && lock_registry_pipe_write(writer_release[1], 'X');
    int writer_status = -1;
    bool writer_exited = writer > 0 && lock_registry_child_wait(writer, &writer_status);
    if (local_reader_started) {
        (void)hyp_thread_join(&local_reader_thread);
    }
    hyp_private_file_lock_status_t local_reader_release =
        local_reader.lease ? hyp_lock_lease_release(&local_reader.lease) : HYP_PRIVATE_FILE_LOCK_IO;
    char late_report = 0;
    bool late_followed =
        late_reader > 0 &&
        lock_registry_pipe_read(late_acquired[0], LOCK_REGISTRY_TEST_TIMEOUT_MS, &late_report) &&
        late_report == 'R';
    int late_status = -1;
    bool late_exited = late_reader > 0 && lock_registry_child_wait(late_reader, &late_status);
    if (writer > 0) {
        (void)close(writer_started[0]);
        (void)close(writer_acquired[0]);
        (void)close(writer_release[1]);
    }
    if (late_reader > 0) {
        (void)close(late_attempted[0]);
        (void)close(late_acquired[0]);
    }
    if (initial_reader) {
        (void)hyp_lock_lease_release(&initial_reader);
    }
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(initial_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(writer_pipes);
    ASSERT_GT(writer, 0);
    ASSERT_TRUE(writer_announced);
    ASSERT_TRUE(names_ok);
    ASSERT_TRUE(writer_has_turn);
    ASSERT_TRUE(local_reader_started);
    ASSERT_TRUE(local_reader_queued);
    ASSERT_TRUE(late_pipe);
    ASSERT_GT(late_reader, 0);
    ASSERT_TRUE(late_reached_turn);
    ASSERT_EQ(initial_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(writer_won);
    ASSERT_EQ(late_ready_while_writer, 0);
    ASSERT_FALSE(local_ready_while_writer);
    ASSERT_TRUE(writer_commanded);
    ASSERT_TRUE(writer_exited);
    ASSERT_TRUE(WIFEXITED(writer_status));
    ASSERT_EQ(WEXITSTATUS(writer_status), 0);
    ASSERT_EQ(local_reader.status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(local_reader_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(late_followed);
    ASSERT_TRUE(late_exited);
    ASSERT_TRUE(WIFEXITED(late_status));
    ASSERT_EQ(WEXITSTATUS(late_status), 0);
    PASS();
#endif
}

TEST(lock_registry_fork_child_rejects_inherited_registry) {
#ifdef _WIN32
    SKIP_PLATFORM("fork inheritance applies only to POSIX");
#else
    lock_registry_fixture_t fixture;
    bool started = lock_registry_fixture_start(&fixture);
    hyp_lock_lease_t *reader = NULL;
    hyp_private_file_lock_status_t reader_status =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "fork-registry", HYP_PRIVATE_FILE_LOCK_SH,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &reader)
            : HYP_PRIVATE_FILE_LOCK_IO;
    int reports[2] = {-1, -1};
    bool pipe_ok = reader_status == HYP_PRIVATE_FILE_LOCK_OK && pipe(reports) == 0;
    pid_t child = pipe_ok ? fork() : -1;
    if (child == 0) {
        (void)close(reports[0]);
        int result = lock_registry_inherited_child(fixture.registry, reader, reports[1]);
        _exit(result);
    }
    if (child > 0) {
        (void)close(reports[1]);
    }

    char acquire_report = 0;
    char release_report = 0;
    char free_report = 0;
    bool reports_ok =
        child > 0 &&
        lock_registry_pipe_read(reports[0], LOCK_REGISTRY_TEST_TIMEOUT_MS, &acquire_report) &&
        lock_registry_pipe_read(reports[0], LOCK_REGISTRY_TEST_TIMEOUT_MS, &release_report) &&
        lock_registry_pipe_read(reports[0], LOCK_REGISTRY_TEST_TIMEOUT_MS, &free_report);
    int child_status = -1;
    bool child_exited = child > 0 && lock_registry_child_wait(child, &child_status);
    if (child > 0) {
        (void)close(reports[0]);
    }

    char turn_name[HYP_LOCK_REGISTRY_NAME_CAP];
    char rw_name[HYP_LOCK_REGISTRY_NAME_CAP];
    bool names_ok = hyp_lock_registry_resource_names("fork-registry", turn_name, rw_name);
    hyp_private_file_lock_t *probe = NULL;
    hyp_private_file_lock_status_t parent_still_locked =
        names_ok ? hyp_private_file_lock_try_acquire(fixture.directory, rw_name,
                                                     HYP_PRIVATE_FILE_LOCK_EX, &probe)
                 : HYP_PRIVATE_FILE_LOCK_IO;
    if (probe) {
        (void)hyp_private_file_lock_release(&probe);
    }
    hyp_private_file_lock_status_t reader_release =
        reader ? hyp_lock_lease_release(&reader) : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_lock_lease_t *writer = NULL;
    hyp_private_file_lock_status_t writer_after =
        started
            ? hyp_lock_registry_acquire(fixture.registry, "fork-registry", HYP_PRIVATE_FILE_LOCK_EX,
                                        hyp_now_ms() + LOCK_REGISTRY_TEST_TIMEOUT_MS, NULL, &writer)
            : HYP_PRIVATE_FILE_LOCK_IO;
    hyp_private_file_lock_status_t writer_release =
        writer ? hyp_lock_lease_release(&writer) : HYP_PRIVATE_FILE_LOCK_IO;
    lock_registry_fixture_finish(&fixture);

    ASSERT_TRUE(started);
    ASSERT_EQ(reader_status, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_TRUE(pipe_ok);
    ASSERT_GT(child, 0);
    ASSERT_TRUE(reports_ok);
    ASSERT_EQ((unsigned char)acquire_report, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_EQ((unsigned char)release_report, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ((unsigned char)free_report, HYP_PRIVATE_FILE_LOCK_IO);
    ASSERT_TRUE(child_exited);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0);
    ASSERT_TRUE(names_ok);
    ASSERT_EQ(parent_still_locked, HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_EQ(reader_release, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(writer_after, HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(writer_release, HYP_PRIVATE_FILE_LOCK_OK);
    PASS();
#endif
}

SUITE(lock_registry) {
    RUN_TEST(lock_registry_cancelled_wait_rolls_back_and_does_not_barge);
    RUN_TEST(lock_registry_failed_rollback_returns_cleanup_only_lease);
    RUN_TEST(lock_registry_abort_lock_failure_returns_waiter_cleanup_lease);
    RUN_TEST(lock_registry_abort_remove_failure_returns_waiter_cleanup_lease);
    RUN_TEST(lock_registry_never_upgrades_shared_lease_in_place);
    RUN_TEST(lock_registry_reader_close_failure_retains_lease_and_accounting);
    RUN_TEST(lock_registry_writer_partial_release_retries_rw_then_turn);
    RUN_TEST(lock_registry_terminal_close_error_finishes_pending_accounting);
    RUN_TEST(lock_registry_free_refuses_active_lease);
    RUN_TEST(lock_registry_free_retires_identity_and_rejects_stale_pointer);
    RUN_TEST(lock_registry_large_queue_parks_non_head_waiters);
    RUN_TEST(lock_registry_cancel_request_wakes_parked_tail);
    RUN_TEST(lock_registry_absolute_deadline_survives_repeated_wakes);
    RUN_TEST(lock_registry_concurrent_shared_exclusive_stress);
    RUN_TEST(lock_registry_cross_process_writer_beats_late_reader);
    RUN_TEST(lock_registry_fork_child_rejects_inherited_registry);
}
