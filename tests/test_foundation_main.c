/*
 * test_foundation_main.c — entry point for the foundation-only test binary.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * `make -f Makefile.hyp test-foundation` builds a SMALL binary: the foundation
 * layer plus its suites, and nothing else. That is the whole point of the
 * target — a seconds-long build/run loop while working inside src/foundation,
 * with no grammars, no store, no LSP, no extraction.
 *
 * tests/test_main.c cannot serve that link. It is the entry point for the FULL
 * runner and it hard-references every suite in the tree (~140 `suite_*`
 * symbols) plus the MCP/daemon/index-worker re-exec probes that the full suite
 * spawns this binary to perform. Linking it into a foundation-only closure
 * leaves all of that undefined, which is exactly how the target came to be
 * permanently broken.
 *
 * So this runner owns the foundation slice and nothing more. The two runners
 * follow the same pattern already used by tests/repro/repro_main.c: own
 * counters, own suite table, shared test_framework.h macros.
 *
 * ADDING A FOUNDATION SUITE
 *   1. add tests/test_<name>.c to TEST_FOUNDATION_SUITE_SRCS in Makefile.hyp
 *   2. forward-declare + RUN_SELECTED_SUITE it below
 *   3. add it to the Foundation block in tests/test_main.c too — the full
 *      runner is the gate; this one is the fast loop.
 */

/* Global test counters (declared extern in test_framework.h) */
int tf_pass_count = 0;
int tf_fail_count = 0;
int tf_skip_count = 0;

#include "test_framework.h"
#include "test_helpers.h"
#include "foundation/compat.h" /* hyp_mkdtemp, hyp_setenv, hyp_unsetenv */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Suites may derive paths from HOME; none of them may touch the developer's
 * real cache. Same contract as tests/test_main.c: a private HOME, no inherited
 * HYP_CACHE_DIR, and an atexit sweep so an early return still cleans up. */
static char tf_home_sentinel[512];

static void tf_cleanup_cache_sentinel(void) {
    if (tf_home_sentinel[0]) {
        th_rmtree(tf_home_sentinel);
    }
}

static bool tf_setup_cache_sentinel(void) {
    snprintf(tf_home_sentinel, sizeof(tf_home_sentinel), "/tmp/hyp-test-home-XXXXXX");
    if (!hyp_mkdtemp(tf_home_sentinel)) {
        return false;
    }
    hyp_setenv("HOME", tf_home_sentinel, 1);
    hyp_unsetenv("HYP_CACHE_DIR");
    atexit(tf_cleanup_cache_sentinel);
    return true;
}

/* Suite selection mirrors the full runner's contract so a name that works with
 * build/c/test-runner works here unchanged: no args runs everything,
 * `--list-suites` prints the table without running it, and any other args are
 * treated as an explicit suite allowlist. */
static bool g_list_only = false;
static int g_suite_argc = 1;
static char **g_suite_argv = NULL;
static bool *g_suite_arg_matched = NULL;

static bool suite_requested(const char *name) {
    if (g_suite_argc <= 1) {
        return true;
    }
    bool requested = false;
    for (int i = 1; i < g_suite_argc; i++) {
        if (strcmp(g_suite_argv[i], name) == 0) {
            g_suite_arg_matched[i] = true;
            requested = true;
        }
    }
    return requested;
}

#define RUN_SELECTED_SUITE(name)             \
    do {                                     \
        if (g_list_only) {                   \
            printf("%s\n", #name);           \
        } else if (suite_requested(#name)) { \
            RUN_SUITE(name);                 \
        }                                    \
    } while (0)

/* Forward declarations of the foundation suite functions */
extern void suite_arena(void);
extern void suite_hash_table(void);
extern void suite_dyn_array(void);
extern void suite_str_intern(void);
extern void suite_record(void);
extern void suite_log(void);
extern void suite_str_util(void);
extern void suite_workspace(void);
extern void suite_platform(void);
extern void suite_diagnostics(void);
extern void suite_subprocess(void);
extern void suite_private_file_lock(void);
extern void suite_lock_registry(void);
extern void suite_dump_verify(void);

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--list-suites") == 0) {
        g_list_only = true;
        g_suite_argc = 1; /* no suite-name args to match */
    } else {
        g_suite_argc = argc;
        g_suite_argv = argv;
    }
    if (g_suite_argc > 1) {
        g_suite_arg_matched = calloc((size_t)argc, sizeof(*g_suite_arg_matched));
        if (!g_suite_arg_matched) {
            fprintf(stderr, "Failed to allocate test-suite argument tracking\n");
            return 1;
        }
    }
    if (!g_list_only) {
        if (!tf_setup_cache_sentinel()) {
            fprintf(stderr, "failed to create isolated test cache\n");
            free(g_suite_arg_matched);
            return 2;
        }
        printf("\n  hyponoia  C foundation test suite\n");
    }

    /* Foundation — keep in step with the Foundation block of tests/test_main.c */
    RUN_SELECTED_SUITE(arena);
    RUN_SELECTED_SUITE(hash_table);
    RUN_SELECTED_SUITE(dyn_array);
    RUN_SELECTED_SUITE(str_intern);
    RUN_SELECTED_SUITE(record);
    RUN_SELECTED_SUITE(log);
    RUN_SELECTED_SUITE(str_util);
    RUN_SELECTED_SUITE(workspace);
    RUN_SELECTED_SUITE(platform);
    RUN_SELECTED_SUITE(diagnostics);
    RUN_SELECTED_SUITE(subprocess);
    RUN_SELECTED_SUITE(private_file_lock);
    RUN_SELECTED_SUITE(lock_registry);
    RUN_SELECTED_SUITE(dump_verify);

    if (g_list_only) {
        fflush(stdout);
        return 0;
    }
    bool any_suite_matched = false;
    for (int i = 1; i < g_suite_argc; i++) {
        any_suite_matched = any_suite_matched || g_suite_arg_matched[i];
    }
    fflush(stdout);
    for (int i = 1; i < g_suite_argc; i++) {
        if (!g_suite_arg_matched[i]) {
            fprintf(stderr, "Unknown test suite: %s\n", g_suite_argv[i]);
            tf_fail_count++;
        }
    }
    if (g_suite_argc > 1 && !any_suite_matched) {
        fprintf(stderr, "No matching test suites requested\n");
    }
    free(g_suite_arg_matched);
    g_suite_arg_matched = NULL;

    TEST_SUMMARY();
}
