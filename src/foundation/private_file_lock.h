/*
 * private_file_lock.h — Secure locks inside a prevalidated private directory.
 *
 * This is an internal foundation primitive. It deliberately does not choose a
 * product runtime path; callers must supply an opaque directory handle created
 * by the platform runtime-path layer.
 */
#ifndef HYP_PRIVATE_FILE_LOCK_H
#define HYP_PRIVATE_FILE_LOCK_H

#include <stdint.h>

typedef enum {
    HYP_PRIVATE_FILE_LOCK_OK = 0,
    HYP_PRIVATE_FILE_LOCK_BUSY = 1,
    HYP_PRIVATE_FILE_LOCK_UNSAFE = 2,
    HYP_PRIVATE_FILE_LOCK_IO = 3,
} hyp_private_file_lock_status_t;

typedef enum {
    HYP_PRIVATE_FILE_LOCK_SH = 1,
    HYP_PRIVATE_FILE_LOCK_EX = 2,
} hyp_private_file_lock_mode_t;

typedef struct hyp_private_lock_directory hyp_private_lock_directory_t;
typedef struct hyp_private_file_lock hyp_private_file_lock_t;

/* Basenames are fixed internal names, never paths. Acquisition is
 * nonblocking; BUSY is the only contention result. Stable lock files are never
 * unlinked by this API. Any non-NULL *lock_out on any status owns native
 * cleanup state and must be passed to hyp_private_file_lock_release(). */
hyp_private_file_lock_status_t hyp_private_file_lock_try_acquire(
    hyp_private_lock_directory_t *directory, const char *base_name,
    hyp_private_file_lock_mode_t mode, hyp_private_file_lock_t **lock_out);

/* OK terminally closes the native handle and clears *lock_io. IO retains a
 * non-NULL object only while native ownership is safely retryable. POSIX
 * close(2) consumes descriptor ownership once invoked even if it reports an
 * error, so that terminal IO case clears *lock_io to prevent fd-reuse races. */
hyp_private_file_lock_status_t hyp_private_file_lock_release(hyp_private_file_lock_t **lock_io);

void hyp_private_lock_directory_close(hyp_private_lock_directory_t *directory);
const char *hyp_private_lock_directory_path(const hyp_private_lock_directory_t *directory);

#endif /* HYP_PRIVATE_FILE_LOCK_H */
