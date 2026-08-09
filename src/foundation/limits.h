/*
 * limits.h — Generous, env-configurable safety limits (Stage 2 / Track B4).
 *
 * Each knob has a generous default. Hitting a limit degrades to a *reported*
 * skip (surfaced via MCP/CLI/logfile), never a silent drop and never an
 * unbounded read (unbounded just trades a crash for an OOM/hang). Every limit
 * is env-overridable so an operator can tune it per-repo without a rebuild.
 */
#ifndef HYP_LIMITS_H
#define HYP_LIMITS_H

/* Result of an attempted per-file read, so callers can attribute a skip to the
 * right phase/reason instead of collapsing every failure into "read failed". */
typedef enum {
    HYP_READ_OK = 0,    /* file read successfully */
    HYP_READ_OPEN_FAIL, /* could not open (missing / permission) */
    HYP_READ_EMPTY,     /* zero/negative size — benign, nothing to index */
    HYP_READ_OVERSIZED, /* size exceeds hyp_max_file_bytes() */
    HYP_READ_OOM,       /* buffer allocation failed */
} hyp_read_status_t;

/* Maximum size (bytes) of a single source file the indexer will read into
 * memory. Files larger than this are skipped-and-reported (phase "oversized"),
 * never silently dropped. Override with HYP_MAX_FILE_BYTES (a positive integer
 * count of bytes). Default 512 MiB (raised from the historical 100 MB cap).
 *
 * The env var is read on each call — this is intentional: read_file() calls it
 * once per file (negligible), and reading fresh means a test / operator can
 * change the cap via setenv without a process restart or a stale memoized copy
 * leaking across runs. */
long hyp_max_file_bytes(void);

/* Maximum variable-length path depth for the Cypher engine (the `*min..max`
 * hop ceiling). BOTH the explicit (`*1..N`) and unbounded (`*`, `*..m`) forms
 * are clamped to this, so `[:CALLS*1..1000000]` degrades to a WARN-and-cap
 * rather than an unbounded (cyclic-graph DoS) traversal. Override with
 * HYP_CYPHER_MAX_DEPTH (a positive integer). Default 10. */
int hyp_cypher_max_depth(void);

/* Maximum traversal depth for client-driven MCP graph tools (trace_call_path,
 * detect_changes): the client `depth` argument is WARN-clamped to this so an
 * arbitrarily large value cannot drive an unbounded BFS over the shared store.
 * Override with HYP_MCP_MAX_DEPTH (a positive integer). Default 15. */
int hyp_mcp_max_depth(void);

#endif /* HYP_LIMITS_H */
