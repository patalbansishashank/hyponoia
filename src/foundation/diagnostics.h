/*
 * diagnostics.h — Periodic diagnostics file writer.
 *
 * When HYP_DIAGNOSTICS=1, writes a snapshot and retained trajectory below a
 * fresh owner-private temporary directory every 5s. The start diagnostic
 * reports both randomized paths for soak-test and support tooling.
 */
#ifndef HYP_DIAGNOSTICS_H
#define HYP_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* Global query stats — updated by the MCP server on each tool call. */
typedef struct {
    atomic_int count;     /* total tool calls */
    atomic_int errors;    /* tool calls that returned isError=true */
    atomic_llong time_us; /* cumulative wall-clock time (microseconds) */
    atomic_llong max_us;  /* max single call time (microseconds) */
} hyp_query_stats_t;

/* Singleton query stats — MCP server increments these. */
extern hyp_query_stats_t g_query_stats;

/* Record a completed tool call. */
void hyp_diag_record_query(long long duration_us, bool is_error);

/* Start the diagnostics writer thread (if HYP_DIAGNOSTICS env is set).
 * Call once from main(). Returns true if started. */
bool hyp_diag_start(void);

/* Stop the writer within a bounded deadline and delete the live snapshot.
 * The trajectory remains for post-mortem support. */
void hyp_diag_stop(void);

#ifdef HYP_DIAGNOSTICS_ENABLE_TEST_API
/* Focused lifecycle/security seams; absent from production builds. */
bool hyp_diag_test_copy_paths(char *directory, size_t directory_size, char *snapshot,
                              size_t snapshot_size, char *trajectory, size_t trajectory_size);
void hyp_diag_test_hold_writer(bool hold);
bool hyp_diag_test_writer_reached(void);
bool hyp_diag_test_abandoned(void);
bool hyp_diag_test_reset_abandoned(void);
#endif

#endif /* HYP_DIAGNOSTICS_H */
