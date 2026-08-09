/* RED contract for daemon/local-CLI cross-process project coordination. */
#include "test_framework.h"
#include "test_helpers.h"

#include "daemon/ipc.h"
#include "daemon/project_lock.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"

#include <stdint.h>
#include <stdio.h>

enum { PROJECT_LOCK_TEST_PATH_CAP = 1024 };

static void project_lock_test_release(hyp_project_lock_lease_t **lease) {
    while (lease && *lease && hyp_project_lock_lease_release(lease) != HYP_PRIVATE_FILE_LOCK_OK) {
        hyp_usleep(1000);
    }
}

TEST(project_lock_coordinates_instances_projects_wildcard_and_case_aliases) {
    char runtime_parent[PROJECT_LOCK_TEST_PATH_CAP];
    (void)snprintf(runtime_parent, sizeof(runtime_parent), "%s/hyp-project-lock-XXXXXX",
                   hyp_tmpdir());
    ASSERT_NOT_NULL(hyp_mkdtemp(runtime_parent));

    hyp_daemon_ipc_endpoint_t *endpoint =
        hyp_daemon_ipc_endpoint_new("0123456789abcdef", runtime_parent);
    hyp_project_lock_manager_t *first = hyp_project_lock_manager_new(endpoint);
    hyp_project_lock_manager_t *second = hyp_project_lock_manager_new(endpoint);
    ASSERT_NOT_NULL(endpoint);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);

    hyp_project_lock_lease_t *foo = NULL;
    hyp_project_lock_lease_t *alias = NULL;
    hyp_project_lock_lease_t *bar = NULL;
    ASSERT_EQ(hyp_project_lock_try_acquire(first, "Foo", &foo), HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NOT_NULL(foo);
    ASSERT_EQ(hyp_project_lock_try_acquire(second, "foo", &alias), HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_NULL(alias);
    ASSERT_EQ(hyp_project_lock_acquire(second, "bar", UINT64_MAX, NULL, &bar),
              HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NOT_NULL(bar);

    project_lock_test_release(&bar);
    project_lock_test_release(&foo);

    hyp_project_lock_lease_t *all = NULL;
    ASSERT_EQ(hyp_project_lock_acquire(first, "*", UINT64_MAX, NULL, &all),
              HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NOT_NULL(all);
    ASSERT_EQ(hyp_project_lock_try_acquire(second, "unrelated", &bar), HYP_PRIVATE_FILE_LOCK_BUSY);
    ASSERT_NULL(bar);

    project_lock_test_release(&all);
    ASSERT_EQ(hyp_project_lock_manager_free(&second), HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_EQ(hyp_project_lock_manager_free(&first), HYP_PRIVATE_FILE_LOCK_OK);
    ASSERT_NULL(second);
    ASSERT_NULL(first);
    hyp_daemon_ipc_endpoint_free(endpoint);
    (void)th_rmtree(runtime_parent);
    PASS();
}

SUITE(project_lock) {
    RUN_TEST(project_lock_coordinates_instances_projects_wildcard_and_case_aliases);
}
