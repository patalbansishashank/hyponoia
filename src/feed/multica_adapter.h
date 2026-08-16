#ifndef HYP_FEED_MULTICA_ADAPTER_H
#define HYP_FEED_MULTICA_ADAPTER_H

#include "feed/feed.h"

#include <stddef.h>

/*
 * multica_adapter.h — Multica as the FIRST adapter behind the feed boundary.
 * An implementation of feed.h, never a dependency of it: nothing in the core
 * names Multica, and tests/test_feed_contract.sh holds that line file-level.
 * Every piece of Multica vocabulary — table names, column names, identifier
 * formats — lives in this pair of files and nowhere else.
 *
 * ── The wire, and what is tested here ────────────────────────────────────────
 *
 * Multica is a postgres database. libpq is NOT vendored in this tree, so the
 * adapter is defined over an injected row seam (hyp_multica_rows_t below): the
 * pinned SQL strings produce a result set, and the seam delivers it as text
 * cells exactly the way libpq would (PQnfields/PQfname/PQgetvalue; every value
 * a string, SQL NULL as NULL). Tests drive the adapter through fixture rows.
 * THE LIVE CONNECTION IS A DOCUMENTED FOLLOW-UP: a thin libpq-backed
 * hyp_multica_rows_t that runs the SQL strings verbatim. Nothing here stubs
 * it silently — until it exists there is no live pull, and this header says
 * so.
 *
 * ── Column pinning, and the fail-closed rule ─────────────────────────────────
 *
 * Every pull declares the EXACT result-set columns it expects — names, order
 * and count — and refuses the whole pull on any mismatch: a missing column, an
 * EXTRA column, a renamed column, a reordered column all fail. Extra columns
 * fail deliberately: the SQL is ours, so a wire that returns columns we did
 * not ask for is not running our SQL, and "probably compatible" is how a
 * schema drift ships. Matching is exact bytes; postgres lowercases unquoted
 * identifiers and the pinned SQL uses only such.
 *
 * The tables' own shapes are pinned separately: HYP_MULTICA_SCHEMA_PIN_SQL
 * reads the catalog and hyp_multica_schema_pin_check() verifies it — the
 * verified column SETS of task_message, issue and workspace exactly; a
 * REQUIRED SUBSET of agent_task_queue and agent (their remaining columns are
 * pinned as IGNORED, explicitly including delegated_from_task_id,
 * retry_of_task_id and rerun_of_task_id: task lineage is read from
 * parent_task_id ONLY); and the four foreign keys the mapping rides on:
 *
 *     task_message.task_id        -> agent_task_queue.id   (the author hop)
 *     agent_task_queue.agent_id   -> agent.id              (the author hop)
 *     agent_task_queue.issue_id   -> issue.id
 *     agent_task_queue.parent_task_id -> agent_task_queue.id
 *
 * The first is pinned because a LEFT JOIN over a silently renamed or unkeyed
 * table would not error — it would return NULL agents forever. A schema that
 * drifts must die loudly, at the pin, before a single row is mapped.
 *
 * The required subsets are sized by what was VERIFIED, not by what sounds
 * likely. agent_task_queue's list was read off a live catalog; `agent` was not,
 * so only id, name and workspace_id are required there — and workspace_id is
 * itself UNVERIFIED, listed only because the attribution join rides on it.
 * Requiring an unconfirmed column is not extra safety: it fails closed against
 * a schema that is entirely correct, and then refuses every row forever.
 *
 * ── The mapping ──────────────────────────────────────────────────────────────
 *
 * task_message rows become kind=message records:
 *
 *   origin  "multica:task_message:<id>". The row's own identity and nothing
 *           else. issue_id and session_id are deliberately NOT folded in:
 *           origin is what makes re-ingest idempotent, so it must be exactly
 *           as stable as the row's primary key — task metadata that can be
 *           backfilled upstream would mint new origins for old rows and turn
 *           the audit against itself. Both columns are still selected and
 *           pinned (the wire is exercised and verified); they are task-level
 *           facts and the task-level record is D1's business, not a row's.
 *   thread  "multica:task:<task_id>" — one task execution is one thread.
 *   author  hyp_multica_author_of(): the three-hop join task_message.task_id
 *           -> agent_task_queue -> agent supplies agent.name. The rule is a
 *           named function, not an inline literal — see its declaration.
 *   parent  the previous YIELDED message of the same task when there is one;
 *           otherwise, for a task's first message, the first message of its
 *           parent_task_id task (delegation/retry lineage), when that task
 *           has any; otherwise absent. Chains skip over policy-skipped rows.
 *   content composed as:
 *               "type=<type>" then " tool=<tool>" when tool is present
 *               then, for each of content, input, output that is non-NULL:
 *               "\n<label>:\n<value>"
 *           "~" stands in for an absent type. input is jsonb rendered by
 *           postgres (::text), which normalises it deterministically.
 *   timestamp_ms  created_at, converted to epoch milliseconds IN THE SQL, so
 *           the adapter parses one integer and no datetime format.
 *   redactions 0 — fixture rows only. D2's scrub is a hard precondition of
 *           the first REAL ingest and slots in as a source wrapper (scrub
 *           before yield, so the id commits to scrubbed bytes); this adapter
 *           does not connect to anything until that exists.
 *
 * issue rows become kind=signal records (a work item is an observation about
 * work, not a transcript turn): origin/thread "multica:issue:<id>", parent
 * from parent_issue_id, author from creator_type/creator_id via
 * hyp_multica_issue_author_of(), content composed as
 * "title=<title|~>\nstatus=<status|~> priority=<priority|~> number=<n|~>"
 * plus "\n\n<description>" when present. The CHECK-constraint enums the
 * schema declares are pinned at value level: a status, priority or
 * creator_type outside the verified sets is HYP_FEED_ERR_SCHEMA.
 *
 * workspace rows are NOT records: they feed the workspace resolver (A6). The
 * pull returns typed rows. `repos` is jsonb whose column is verified but
 * whose INNER SHAPE IS NOT — nobody has read a live row. The declared,
 * UNVERIFIED inner pin is: a JSON array (or SQL NULL); anything else refuses.
 * This must be confirmed against a live row before A6 relies on it.
 *
 * ── Null policy, per consumed column (everything nullable upstream) ──────────
 *
 * Multica's schema pins nothing at the database layer, so this adapter pins
 * MORE, not less: every consumed column has an explicit policy. REFUSE aborts
 * the pull (atomic, nothing lands); SKIP yields a named, counted notice the
 * audit reports separately from loss; absent-marker folds "~" into composed
 * content; tolerated means the mapping simply carries less.
 *
 *   task_message.id        NULL -> REFUSE. A row with no identity cannot even
 *                                  be reported missing by name; it poisons
 *                                  the audit itself.
 *   task_message.task_id   NULL -> SKIP "unowned-row" (no thread, no author
 *                                  path; the row is accounted for by name).
 *   created_at (both pulls) NULL -> SKIP "no-timestamp". The event time is
 *                                  the source's to state; inventing one here
 *                                  would make ids machine-dependent.
 *   content+input+output all NULL -> SKIP "empty-row" (policy, counted, so
 *                                  the audit tells "declined" from "lost").
 *   issue title+description both NULL -> SKIP "empty-row".
 *   type                   NULL -> absent-marker in composed content.
 *   tool                   NULL -> omitted from composed content.
 *   seq                    NULL -> tolerated; ordering falls back to id.
 *   agent.name (join)      NULL -> hyp_multica_author_of() decides — see the
 *                                  attribution rule below.
 *   parent_task_id, parent-task first message, parent_issue_id, issue_id,
 *   session_id             NULL -> tolerated: absent linkage, absent facts.
 *   issue.id, workspace.id NULL -> REFUSE, same identity argument.
 *   status/priority/creator_type NULL -> tolerated (absent-marker / rule);
 *                                  non-NULL outside the pinned enums -> the
 *                                  pull is refused as schema drift.
 *
 * Skip order when several policies match one row: identity first (REFUSE),
 * then "unowned-row", then "no-timestamp", then "empty-row".
 *
 * ── Attribution, fail closed ─────────────────────────────────────────────────
 *
 * workspace.attribution_fail_closed is read (reachable via agent.workspace_id
 * or, failing that, issue.workspace_id) and surfaced on workspace rows. When
 * attribution cannot complete (no agent row / no creator):
 *
 *   flag false            -> the named default author "...:unattributed"
 *   flag true OR UNKNOWN  -> the pull is REFUSED. Unknown is treated as true:
 *                            fail closed is the default, never the fallback.
 */

/* ── The row seam ────────────────────────────────────────────────────────── */

/*
 * One result set, delivered as text. Implemented over libpq for the live
 * connection (follow-up) and over in-memory fixtures in tests. The struct is
 * caller-owned; the adapter only reads through the function pointers.
 */
typedef struct hyp_multica_rows hyp_multica_rows_t;
struct hyp_multica_rows {
    void *ctx;
    size_t (*column_count)(const hyp_multica_rows_t *rows);
    const char *(*column_name)(const hyp_multica_rows_t *rows, size_t col);
    /* Advance to the next row: 1 = a row is ready, 0 = exhausted, -1 = the
     * source failed. */
    int (*next_row)(hyp_multica_rows_t *rows);
    /* Current row, column col: the value as text, or NULL for SQL NULL.
     * Borrowed until the next next_row() call. */
    const char *(*value)(const hyp_multica_rows_t *rows, size_t col);
};

/* ── The pinned SQL. The live wire runs these verbatim. ──────────────────── */

extern const char *const HYP_MULTICA_MESSAGES_SQL;
extern const char *const HYP_MULTICA_ISSUES_SQL;
extern const char *const HYP_MULTICA_WORKSPACES_SQL;
extern const char *const HYP_MULTICA_SCHEMA_PIN_SQL;

/* The pinned result-set columns, exact names in exact order, NULL-terminated.
 * Exposed so tests assert the pin against the declaration, not a copy of it. */
extern const char *const HYP_MULTICA_MESSAGES_COLUMNS[];
extern const char *const HYP_MULTICA_ISSUES_COLUMNS[];
extern const char *const HYP_MULTICA_WORKSPACES_COLUMNS[];

/* Skip reasons, verbatim, so the audit's account is assertable. */
#define HYP_MULTICA_SKIP_UNOWNED "unowned-row"
#define HYP_MULTICA_SKIP_NO_TIMESTAMP "no-timestamp"
#define HYP_MULTICA_SKIP_EMPTY "empty-row"

/* ── The two record feeds ────────────────────────────────────────────────── */

/*
 * Open a feed source over a result set of the corresponding pinned SQL. Pins
 * the columns immediately: on any mismatch nothing is opened and the status is
 * HYP_FEED_ERR_SCHEMA. On success *src is filled and owns internal state until
 * hyp_feed_source_close(); `rows` stays caller-owned and must outlive the
 * source.
 */
hyp_feed_status_t hyp_multica_messages_open(hyp_multica_rows_t *rows, hyp_feed_source_t *src);
hyp_feed_status_t hyp_multica_issues_open(hyp_multica_rows_t *rows, hyp_feed_source_t *src);

/* ── The workspace pull (for the resolver, not for records) ──────────────── */

typedef struct {
    char *id;           /* never NULL */
    char *name;         /* NULL when the source holds none */
    char *slug;         /* NULL when the source holds none */
    char *repos_json;   /* raw jsonb text; shape-pinned array, or NULL */
    char *issue_prefix; /* NULL when the source holds none */
    /* workspace.attribution_fail_closed, surfaced: 1 true, 0 false,
     * -1 SQL NULL — reported as unknown, never coerced to an answer. */
    int attribution_fail_closed;
} hyp_multica_workspace_t;

hyp_feed_status_t hyp_multica_workspaces_pull(hyp_multica_rows_t *rows,
                                              hyp_multica_workspace_t **out, size_t *out_count);
void hyp_multica_workspaces_free(hyp_multica_workspace_t *items, size_t count);

/* ── The schema pin ──────────────────────────────────────────────────────── */

/* Verify a result set of HYP_MULTICA_SCHEMA_PIN_SQL against the pinned table
 * shapes and foreign keys described above. HYP_FEED_OK or a refusal. */
hyp_feed_status_t hyp_multica_schema_pin_check(hyp_multica_rows_t *rows);

/* ── Attribution rules, named and testable ───────────────────────────────── */

/*
 * The author mapping for task messages, as ONE function so the policy has one
 * home. attribution_fail_closed: 1 true, 0 false, -1 unknown (treated as
 * true). Writes "multica:agent:<name>" — or the named default — into out.
 * Refuses (HYP_FEED_ERR_SOURCE) when attribution cannot complete and the flag
 * says fail closed, or when the name would overflow the record's author cap:
 * caps are refusals, never truncation.
 */
hyp_feed_status_t hyp_multica_author_of(const char *agent_name, int attribution_fail_closed,
                                        char *out, size_t cap);

/* The author mapping for issues: "multica:<creator_type>:<creator_id>" when
 * both halves are present; otherwise the same flag rule as above with the
 * named default "multica:creator:unattributed". */
hyp_feed_status_t hyp_multica_issue_author_of(const char *creator_type, const char *creator_id,
                                              int attribution_fail_closed, char *out, size_t cap);

#endif /* HYP_FEED_MULTICA_ADAPTER_H */
