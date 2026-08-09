/* Internal constructors for platform runtime-path code and focused tests. */
#ifndef HYP_PRIVATE_FILE_LOCK_INTERNAL_H
#define HYP_PRIVATE_FILE_LOCK_INTERNAL_H

#include "foundation/private_file_lock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hyp_private_fork_condition hyp_private_fork_condition_t;

typedef enum {
    HYP_PRIVATE_FORK_WAIT_SIGNALED = 0,
    HYP_PRIVATE_FORK_WAIT_TIMEOUT = 1,
    HYP_PRIVATE_FORK_WAIT_ERROR = 2,
} hyp_private_fork_wait_status_t;

/* Condition variable associated with the global fork guard. Wait atomically
 * releases that guard and always reacquires it before returning. */
hyp_private_fork_condition_t *hyp_private_fork_condition_new(void);
void hyp_private_fork_condition_free(hyp_private_fork_condition_t *condition);
void hyp_private_fork_condition_broadcast_while_guarded(hyp_private_fork_condition_t *condition);
hyp_private_fork_wait_status_t hyp_private_fork_condition_wait_until_while_guarded(
    hyp_private_fork_condition_t *condition, uint64_t deadline_ms);

#ifdef _WIN32
/* On success ownership of directory_handle transfers to the returned object.
 * On failure the caller still owns it. The handle must allow attribute and
 * security queries; stable_path must be a local drive path naming that exact
 * non-reparse directory. */
hyp_private_file_lock_status_t hyp_private_lock_directory_adopt_windows(
    void *directory_handle, const char *stable_path, hyp_private_lock_directory_t **directory_out);
/* Models LockFileEx contention after opening the handle, then makes the first
 * CloseHandle cleanup attempt fail before invocation. */
bool hyp_private_lock_directory_fail_lock_attempt_cleanup_for_test(
    hyp_private_lock_directory_t *directory);

#else
/* On success ownership of directory_fd transfers to the returned object. On
 * failure the caller still owns it. stable_path must name that exact handle. */
hyp_private_file_lock_status_t hyp_private_lock_directory_adopt_posix(
    int directory_fd, const char *stable_path, hyp_private_lock_directory_t **directory_out);
#endif

/* Focused invariant seam: production callers never receive a native handle. */
bool hyp_private_file_lock_is_cloexec_for_test(const hyp_private_file_lock_t *lock);

/* True after the native SH/EX ownership has been released, even when closing
 * the descriptor/handle remains retryable. */
bool hyp_private_file_lock_unlock_complete(const hyp_private_file_lock_t *lock);

/* A held lock file may carry one small fixed-format coordination record.
 * Reads require SH or EX ownership; writes require EX ownership. The handle
 * validated at acquisition remains the authority, so callers never reopen a
 * user-controlled path. Payloads are capped at 4096 bytes. */
hyp_private_file_lock_status_t hyp_private_file_lock_payload_read(hyp_private_file_lock_t *lock,
                                                                  void *buffer, size_t capacity,
                                                                  size_t *length_out);
hyp_private_file_lock_status_t hyp_private_file_lock_payload_write(hyp_private_file_lock_t *lock,
                                                                   const void *buffer,
                                                                   size_t length);

/* Forces the next successfully acquired native lock down the post-lock
 * validation cleanup path and injects pre-call release failures there. */
bool hyp_private_lock_directory_fail_post_acquire_cleanup_for_test(
    hyp_private_lock_directory_t *directory, bool fail_unlock, bool fail_close);

typedef enum {
    HYP_PRIVATE_FILE_LOCK_RELEASE_UNLOCK = 1,
    HYP_PRIVATE_FILE_LOCK_RELEASE_CLOSE = 2,
} hyp_private_file_lock_release_step_t;

/* One-shot native release fault seam. The selected operation reports failure
 * without invoking the OS primitive, so retry behavior can be tested without
 * corrupting or replacing a real descriptor/handle. */
bool hyp_private_file_lock_fail_next_release_step_for_test(
    hyp_private_file_lock_t *lock, hyp_private_file_lock_release_step_t step);
unsigned int hyp_private_file_lock_release_step_attempts_for_test(
    const hyp_private_file_lock_t *lock, hyp_private_file_lock_release_step_t step);

#ifndef _WIN32
/* Models close(2) consuming fd ownership while reporting an error. */
bool hyp_private_file_lock_fail_close_after_consuming_for_test(hyp_private_file_lock_t *lock);
int hyp_private_file_lock_native_fd_for_test(const hyp_private_file_lock_t *lock);
#endif

/* Global process-lifetime gate for registry live-set mutation and teardown.
 * POSIX atfork prepare takes this same gate before closing child-side lock
 * descriptors. Never call a private-file-lock operation while holding it:
 * those operations take the gate internally on every platform. */
bool hyp_private_file_lock_fork_guard_enter(void);
void hyp_private_file_lock_fork_guard_leave(void);

#endif /* HYP_PRIVATE_FILE_LOCK_INTERNAL_H */
