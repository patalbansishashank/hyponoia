/*
 * daemon.h — Process-local coordination and wire framing for the HYP daemon.
 *
 * Transport and worker supervision live outside this module. The coordinator
 * binds clients and resource subscriptions to transport connections, coalesces
 * shared work, and defines the daemon's terminal shutdown transition.
 */
#ifndef HYP_DAEMON_H
#define HYP_DAEMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Permanent framing version for the account-wide rendezvous endpoint. Never
 * bump this for detailed runtime payload changes: incompatible executable
 * generations must still exchange the stable HELLO conflict envelope. */
#define HYP_DAEMON_RENDEZVOUS_FRAME_VERSION 1U
#define HYP_DAEMON_FRAME_HEADER_SIZE 12U
#define HYP_DAEMON_MAX_FRAME_SIZE (10U * 1024U * 1024U)
#define HYP_DAEMON_KEY_SIZE 17U

typedef enum {
    HYP_DAEMON_FRAME_REQUEST = 1,
    HYP_DAEMON_FRAME_RESPONSE = 2,
} hyp_daemon_frame_type_t;

typedef struct {
    hyp_daemon_frame_type_t type;
    uint16_t flags;
    uint32_t length;
} hyp_daemon_frame_t;

typedef struct hyp_daemon_coordinator hyp_daemon_coordinator_t;

typedef uint64_t hyp_daemon_client_id_t;
typedef uint64_t hyp_daemon_subscription_id_t;

#define HYP_DAEMON_CLIENT_ID_INVALID ((hyp_daemon_client_id_t)0)
#define HYP_DAEMON_SUBSCRIPTION_ID_INVALID ((hyp_daemon_subscription_id_t)0)

typedef enum {
    HYP_DAEMON_COORDINATOR_RUNNING = 1,
    HYP_DAEMON_COORDINATOR_STOPPING = 2,
} hyp_daemon_coordinator_state_t;

typedef enum {
    HYP_DAEMON_SUBSCRIPTION_REJECTED = 0,
    HYP_DAEMON_SUBSCRIPTION_STARTED = 1,
    HYP_DAEMON_SUBSCRIPTION_JOINED = 2,
} hyp_daemon_subscription_result_t;

typedef enum {
    HYP_DAEMON_JOB_NONE = 0,
    HYP_DAEMON_JOB_RUNNING = 1,
    HYP_DAEMON_JOB_CANCEL_REQUESTED = 2,
    HYP_DAEMON_JOB_REAPING = 3,
} hyp_daemon_job_state_t;

typedef void (*hyp_daemon_job_cancel_fn)(const char *project_key, void *context);
typedef void (*hyp_daemon_watch_release_fn)(const char *project_key, void *context);

typedef struct {
    hyp_daemon_job_cancel_fn cancel_job;
    hyp_daemon_watch_release_fn release_watch;
    void *context;
} hyp_daemon_coordinator_hooks_t;

/* lease_timeout_ms is fixed for the coordinator lifetime. All timestamps must
 * come from the same monotonic clock domain. */
hyp_daemon_coordinator_t *hyp_daemon_coordinator_new(uint64_t lease_timeout_ms);

/* A PERMANENT coordinator (backing a `daemon start` generation) never
 * self-transitions to STOPPING when its client count reaches zero; only the
 * explicit stop/drain paths end it. */
void hyp_daemon_coordinator_set_permanent(hyp_daemon_coordinator_t *coordinator, bool permanent);
/* The caller must first quiesce coordinator calls and hook invocations. */
void hyp_daemon_coordinator_free(hyp_daemon_coordinator_t *coordinator);

/* Hooks are copied. Their context must remain valid until the coordinator is
 * quiescent. Hooks are always invoked after releasing the coordinator mutex. */
bool hyp_daemon_coordinator_set_hooks(hyp_daemon_coordinator_t *coordinator,
                                      const hyp_daemon_coordinator_hooks_t *hooks);
hyp_daemon_coordinator_state_t hyp_daemon_coordinator_state(hyp_daemon_coordinator_t *coordinator);

/* Client IDs are daemon-issued, nonzero, monotonic, and never recycled. */
hyp_daemon_client_id_t hyp_daemon_client_connected(hyp_daemon_coordinator_t *coordinator,
                                                   uint64_t now_ms);
bool hyp_daemon_client_disconnected(hyp_daemon_coordinator_t *coordinator,
                                    hyp_daemon_client_id_t client_id, uint64_t now_ms);
bool hyp_daemon_client_heartbeat(hyp_daemon_coordinator_t *coordinator,
                                 hyp_daemon_client_id_t client_id, uint64_t now_ms);
size_t hyp_daemon_expire_leases(hyp_daemon_coordinator_t *coordinator, uint64_t now_ms);
size_t hyp_daemon_active_clients(hyp_daemon_coordinator_t *coordinator);

/* Every accepted subscription receives a unique daemon-issued handle. The
 * first subscriber starts the physical resource; later subscribers join it. */
hyp_daemon_subscription_result_t hyp_daemon_job_subscribe(
    hyp_daemon_coordinator_t *coordinator, hyp_daemon_client_id_t client_id,
    const char *project_key, hyp_daemon_subscription_id_t *subscription_id);
hyp_daemon_subscription_result_t hyp_daemon_watch_subscribe(
    hyp_daemon_coordinator_t *coordinator, hyp_daemon_client_id_t client_id,
    const char *project_key, hyp_daemon_subscription_id_t *subscription_id);
bool hyp_daemon_job_unsubscribe(hyp_daemon_coordinator_t *coordinator,
                                hyp_daemon_client_id_t client_id,
                                hyp_daemon_subscription_id_t subscription_id);
bool hyp_daemon_watch_unsubscribe(hyp_daemon_coordinator_t *coordinator,
                                  hyp_daemon_client_id_t client_id,
                                  hyp_daemon_subscription_id_t subscription_id);

size_t hyp_daemon_job_subscribers(hyp_daemon_coordinator_t *coordinator, const char *project_key);
size_t hyp_daemon_watch_subscribers(hyp_daemon_coordinator_t *coordinator, const char *project_key);
size_t hyp_daemon_active_jobs(hyp_daemon_coordinator_t *coordinator);
size_t hyp_daemon_active_watches(hyp_daemon_coordinator_t *coordinator);
hyp_daemon_job_state_t hyp_daemon_job_state(hyp_daemon_coordinator_t *coordinator,
                                            const char *project_key);

/* Cancellation is two phase. Losing the final subscriber requests cancel;
 * the job remains active until its supervisor reports completion/reaping. */
bool hyp_daemon_job_reaping(hyp_daemon_coordinator_t *coordinator, const char *project_key);
bool hyp_daemon_job_reaped(hyp_daemon_coordinator_t *coordinator, const char *project_key,
                           uint64_t now_ms);
bool hyp_daemon_job_completed(hyp_daemon_coordinator_t *coordinator, const char *project_key,
                              uint64_t now_ms);

/* STOPPING is terminal. Exit is ready only after every job/watch is gone. */
bool hyp_daemon_should_exit(hyp_daemon_coordinator_t *coordinator, uint64_t now_ms);

/* Encode/decode the permanently stable 12-byte "HYPD" rendezvous frame header
 * in network byte order. Detailed operation ABIs live above this framing. */
bool hyp_daemon_frame_header_encode(uint8_t header[HYP_DAEMON_FRAME_HEADER_SIZE],
                                    hyp_daemon_frame_type_t type, uint16_t flags, uint32_t length);
bool hyp_daemon_frame_header_decode(const uint8_t header[HYP_DAEMON_FRAME_HEADER_SIZE],
                                    hyp_daemon_frame_t *frame);

#endif /* HYP_DAEMON_H */
