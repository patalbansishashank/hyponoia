/* project_lock.c — Shared daemon/local-CLI project mutation leases. */
#include "daemon/project_lock.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PROJECT_LOCK_KEY_CAP = 4096 };

static const char PROJECT_SET_KEY[] = "hyp-project-set-v1";

struct hyp_project_lock_manager {
    hyp_private_lock_directory_t *directory;
    hyp_lock_registry_t *registry;
};

struct hyp_project_lock_lease {
    hyp_lock_lease_t *project;
    hyp_lock_lease_t *project_set;
};

static bool project_lock_key(const char *project, char out[PROJECT_LOCK_KEY_CAP]) {
    if (!project || !project[0] || strcmp(project, "*") == 0) {
        return false;
    }
    static const char prefix[] = "hyp-project-v1:";
    size_t prefix_length = sizeof(prefix) - 1U;
    size_t project_length = strnlen(project, PROJECT_LOCK_KEY_CAP);
    if (project_length == 0 || project_length >= PROJECT_LOCK_KEY_CAP ||
        prefix_length + project_length >= PROJECT_LOCK_KEY_CAP) {
        return false;
    }
    memcpy(out, prefix, prefix_length);
    for (size_t index = 0; index < project_length; index++) {
        unsigned char ch = (unsigned char)project[index];
        out[prefix_length + index] = (char)(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    }
    out[prefix_length + project_length] = '\0';
    return true;
}

hyp_project_lock_manager_t *hyp_project_lock_manager_new(
    const hyp_daemon_ipc_endpoint_t *endpoint) {
    hyp_private_lock_directory_t *directory = NULL;
    hyp_private_file_lock_status_t directory_status =
        hyp_daemon_ipc_private_lock_directory_new(endpoint, &directory);
    if (directory_status != HYP_PRIVATE_FILE_LOCK_OK || !directory) {
        if (directory) {
            hyp_private_lock_directory_close(directory);
        }
        return NULL;
    }
    hyp_project_lock_manager_t *manager = calloc(1, sizeof(*manager));
    if (manager) {
        manager->directory = directory;
        manager->registry = hyp_lock_registry_new(directory);
    }
    if (!manager || !manager->registry) {
        free(manager);
        hyp_private_lock_directory_close(directory);
        return NULL;
    }
    return manager;
}

hyp_private_file_lock_status_t hyp_project_lock_lease_release(hyp_project_lock_lease_t **lease_io) {
    if (!lease_io || !*lease_io) {
        return HYP_PRIVATE_FILE_LOCK_IO;
    }
    hyp_project_lock_lease_t *lease = *lease_io;
    hyp_private_file_lock_status_t result = HYP_PRIVATE_FILE_LOCK_OK;
    if (lease->project) {
        result = hyp_lock_lease_release(&lease->project);
        if (lease->project) {
            return HYP_PRIVATE_FILE_LOCK_IO;
        }
    }
    if (lease->project_set) {
        hyp_private_file_lock_status_t set_status = hyp_lock_lease_release(&lease->project_set);
        if (set_status != HYP_PRIVATE_FILE_LOCK_OK) {
            result = set_status;
        }
        if (lease->project_set) {
            return HYP_PRIVATE_FILE_LOCK_IO;
        }
    }
    free(lease);
    *lease_io = NULL;
    return result;
}

static hyp_private_file_lock_status_t project_lock_failed_acquire(
    hyp_project_lock_lease_t *lease, hyp_private_file_lock_status_t status,
    hyp_project_lock_lease_t **lease_out) {
    if (!lease->project && !lease->project_set) {
        free(lease);
        return status;
    }
    hyp_project_lock_lease_t *cleanup = lease;
    hyp_private_file_lock_status_t cleanup_status = hyp_project_lock_lease_release(&cleanup);
    if (cleanup) {
        *lease_out = cleanup;
        return HYP_PRIVATE_FILE_LOCK_IO;
    }
    return cleanup_status == HYP_PRIVATE_FILE_LOCK_OK ? status : HYP_PRIVATE_FILE_LOCK_IO;
}

static hyp_private_file_lock_status_t project_lock_acquire_internal(
    hyp_project_lock_manager_t *manager, const char *project, uint64_t deadline_ms,
    const hyp_lock_cancel_token_t *cancel_token, bool try_once,
    hyp_project_lock_lease_t **lease_out) {
    if (lease_out) {
        *lease_out = NULL;
    }
    if (!manager || !manager->registry || !project || !project[0] || !lease_out) {
        return HYP_PRIVATE_FILE_LOCK_UNSAFE;
    }
    bool wildcard = strcmp(project, "*") == 0;
    char project_key[PROJECT_LOCK_KEY_CAP];
    if (!wildcard && !project_lock_key(project, project_key)) {
        return HYP_PRIVATE_FILE_LOCK_UNSAFE;
    }
    hyp_project_lock_lease_t *lease = calloc(1, sizeof(*lease));
    if (!lease) {
        return HYP_PRIVATE_FILE_LOCK_IO;
    }
    hyp_private_file_lock_status_t status =
        try_once ? hyp_lock_registry_try_acquire(manager->registry, PROJECT_SET_KEY,
                                                 wildcard ? HYP_PRIVATE_FILE_LOCK_EX
                                                          : HYP_PRIVATE_FILE_LOCK_SH,
                                                 &lease->project_set)
                 : hyp_lock_registry_acquire(manager->registry, PROJECT_SET_KEY,
                                             wildcard ? HYP_PRIVATE_FILE_LOCK_EX
                                                      : HYP_PRIVATE_FILE_LOCK_SH,
                                             deadline_ms, cancel_token, &lease->project_set);
    if (status != HYP_PRIVATE_FILE_LOCK_OK) {
        return project_lock_failed_acquire(lease, status, lease_out);
    }
    if (!wildcard) {
        status = try_once ? hyp_lock_registry_try_acquire(manager->registry, project_key,
                                                          HYP_PRIVATE_FILE_LOCK_EX, &lease->project)
                          : hyp_lock_registry_acquire(manager->registry, project_key,
                                                      HYP_PRIVATE_FILE_LOCK_EX, deadline_ms,
                                                      cancel_token, &lease->project);
        if (status != HYP_PRIVATE_FILE_LOCK_OK) {
            return project_lock_failed_acquire(lease, status, lease_out);
        }
    }
    *lease_out = lease;
    return HYP_PRIVATE_FILE_LOCK_OK;
}

hyp_private_file_lock_status_t hyp_project_lock_acquire(hyp_project_lock_manager_t *manager,
                                                        const char *project, uint64_t deadline_ms,
                                                        const hyp_lock_cancel_token_t *cancel_token,
                                                        hyp_project_lock_lease_t **lease_out) {
    return project_lock_acquire_internal(manager, project, deadline_ms, cancel_token, false,
                                         lease_out);
}

hyp_private_file_lock_status_t hyp_project_lock_try_acquire(hyp_project_lock_manager_t *manager,
                                                            const char *project,
                                                            hyp_project_lock_lease_t **lease_out) {
    return project_lock_acquire_internal(manager, project, UINT64_MAX, NULL, true, lease_out);
}

hyp_private_file_lock_status_t hyp_project_lock_request_cancel(hyp_project_lock_manager_t *manager,
                                                               hyp_lock_cancel_token_t *token) {
    return manager ? hyp_lock_registry_request_cancel(manager->registry, token)
                   : HYP_PRIVATE_FILE_LOCK_IO;
}

hyp_private_file_lock_status_t hyp_project_lock_manager_free(
    hyp_project_lock_manager_t **manager_io) {
    if (!manager_io || !*manager_io) {
        return HYP_PRIVATE_FILE_LOCK_IO;
    }
    hyp_project_lock_manager_t *manager = *manager_io;
    hyp_private_file_lock_status_t status = hyp_lock_registry_free(&manager->registry);
    if (status != HYP_PRIVATE_FILE_LOCK_OK) {
        return status;
    }
    hyp_private_lock_directory_close(manager->directory);
    manager->directory = NULL;
    free(manager);
    *manager_io = NULL;
    return HYP_PRIVATE_FILE_LOCK_OK;
}
