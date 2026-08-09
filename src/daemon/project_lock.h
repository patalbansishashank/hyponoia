/* project_lock.h — Shared daemon/local-CLI project mutation leases. */
#ifndef HYP_DAEMON_PROJECT_LOCK_H
#define HYP_DAEMON_PROJECT_LOCK_H

#include "daemon/ipc.h"
#include "foundation/lock_registry.h"

#include <stdint.h>

typedef struct hyp_project_lock_manager hyp_project_lock_manager_t;
typedef struct hyp_project_lock_lease hyp_project_lock_lease_t;

/* Each manager is an independent process-local registry over the endpoint's
 * owner-only runtime directory. Separate HYP processes therefore coordinate
 * through the same native lock files without sharing memory. */
hyp_project_lock_manager_t *hyp_project_lock_manager_new(const hyp_daemon_ipc_endpoint_t *endpoint);

/* Normal projects hold SH(project-set) + EX(project). "*" holds
 * EX(project-set), blocking every named project. Project lock keys are ASCII
 * case-folded to cover filename aliases on case-insensitive filesystems. */
hyp_private_file_lock_status_t hyp_project_lock_acquire(hyp_project_lock_manager_t *manager,
                                                        const char *project, uint64_t deadline_ms,
                                                        const hyp_lock_cancel_token_t *cancel_token,
                                                        hyp_project_lock_lease_t **lease_out);

/* One fair, nonblocking attempt for UI/watcher paths. */
hyp_private_file_lock_status_t hyp_project_lock_try_acquire(hyp_project_lock_manager_t *manager,
                                                            const char *project,
                                                            hyp_project_lock_lease_t **lease_out);

hyp_private_file_lock_status_t hyp_project_lock_lease_release(hyp_project_lock_lease_t **lease_io);

hyp_private_file_lock_status_t hyp_project_lock_request_cancel(hyp_project_lock_manager_t *manager,
                                                               hyp_lock_cancel_token_t *token);

/* Refuses teardown while any lease/cleanup state remains. */
hyp_private_file_lock_status_t hyp_project_lock_manager_free(
    hyp_project_lock_manager_t **manager_io);

#endif /* HYP_DAEMON_PROJECT_LOCK_H */
