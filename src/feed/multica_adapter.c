/*
 * multica_adapter.c — Multica behind the feed boundary. See multica_adapter.h
 * for the contract: the pinned SQL and column sets, the null-policy table, the
 * attribution rule, and why the live libpq wire is a documented follow-up
 * rather than a silent stub.
 *
 * Everything Multica-specific in the tree lives in this file and its header.
 * tests/test_feed_contract.sh enforces that the core feed files speak none of
 * this vocabulary — and that THIS file does, so the gate is known to be able
 * to tell the two apart.
 */
#include "feed/multica_adapter.h"

#include "foundation/constants.h" /* HYP_ALLOC_ONE */
#include "foundation/hash_table.h"

#include "yyjson/yyjson.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── The pinned SQL ─────────────────────────────────────────────────────── */

/*
 * Everything the mapping needs, computed in the SQL so the adapter parses one
 * integer and no datetime: created_at becomes epoch milliseconds, jsonb is
 * rendered to text (deterministically — jsonb normalises), uuids and the
 * attribution flag are cast to text. The LATERAL block resolves each task's
 * delegation parent to that task's FIRST message, which is what a child
 * task's opening record points back at. The ORDER BY is part of the pin: the
 * adapter verifies it row by row and refuses a wire that violates it, because
 * a wire that reorders is not running this SQL.
 */
const char *const HYP_MULTICA_MESSAGES_SQL =
    "SELECT tm.id::text AS id,\n"
    "       tm.task_id::text AS task_id,\n"
    "       tm.seq::text AS seq,\n"
    "       tm.type AS type,\n"
    "       tm.tool AS tool,\n"
    "       tm.content AS content,\n"
    "       tm.input::text AS input,\n"
    "       tm.output AS output,\n"
    "       (floor(extract(epoch FROM tm.created_at) * 1000))::bigint::text AS created_at_ms,\n"
    "       a.name AS agent_name,\n"
    "       q.parent_task_id::text AS parent_task_id,\n"
    "       q.issue_id::text AS issue_id,\n"
    "       q.session_id AS session_id,\n"
    "       pf.id::text AS parent_task_first_message_id,\n"
    "       coalesce(wa.attribution_fail_closed, wi.attribution_fail_closed)::text\n"
    "           AS attribution_fail_closed\n"
    "FROM task_message tm\n"
    "LEFT JOIN agent_task_queue q ON q.id = tm.task_id\n"
    "LEFT JOIN agent a ON a.id = q.agent_id\n"
    "LEFT JOIN workspace wa ON wa.id = a.workspace_id\n"
    "LEFT JOIN issue i ON i.id = q.issue_id\n"
    "LEFT JOIN workspace wi ON wi.id = i.workspace_id\n"
    "LEFT JOIN LATERAL (\n"
    "    SELECT pm.id FROM task_message pm\n"
    "    WHERE pm.task_id = q.parent_task_id\n"
    "    ORDER BY pm.seq ASC NULLS LAST, pm.id ASC\n"
    "    LIMIT 1\n"
    ") pf ON TRUE\n"
    "ORDER BY tm.task_id ASC NULLS LAST, tm.seq ASC NULLS LAST, tm.id ASC;\n";

const char *const HYP_MULTICA_ISSUES_SQL =
    "SELECT i.id::text AS id,\n"
    "       i.workspace_id::text AS workspace_id,\n"
    "       i.title AS title,\n"
    "       i.description AS description,\n"
    "       i.status AS status,\n"
    "       i.priority AS priority,\n"
    "       i.number::text AS number,\n"
    "       i.creator_type AS creator_type,\n"
    "       i.creator_id::text AS creator_id,\n"
    "       i.parent_issue_id::text AS parent_issue_id,\n"
    "       (floor(extract(epoch FROM i.created_at) * 1000))::bigint::text AS created_at_ms,\n"
    "       w.attribution_fail_closed::text AS attribution_fail_closed\n"
    "FROM issue i\n"
    "LEFT JOIN workspace w ON w.id = i.workspace_id\n"
    "ORDER BY i.id ASC;\n";

const char *const HYP_MULTICA_WORKSPACES_SQL =
    "SELECT w.id::text AS id,\n"
    "       w.name AS name,\n"
    "       w.slug AS slug,\n"
    "       w.repos::text AS repos,\n"
    "       w.issue_prefix AS issue_prefix,\n"
    "       w.attribution_fail_closed::text AS attribution_fail_closed\n"
    "FROM workspace w\n"
    "ORDER BY w.id ASC;\n";

const char *const HYP_MULTICA_SCHEMA_PIN_SQL =
    "SELECT 'column' AS pin_kind,\n"
    "       c.table_name || '.' || c.column_name AS pin_name,\n"
    "       '' AS pin_detail\n"
    "FROM information_schema.columns c\n"
    "WHERE c.table_schema = 'public'\n"
    "  AND c.table_name IN\n"
    "      ('task_message', 'issue', 'workspace', 'agent_task_queue', 'agent')\n"
    "UNION ALL\n"
    "SELECT 'fk',\n"
    "       tc.table_name || '.' || kcu.column_name,\n"
    "       ccu.table_name || '.' || ccu.column_name\n"
    "FROM information_schema.table_constraints tc\n"
    "JOIN information_schema.key_column_usage kcu\n"
    "  ON kcu.constraint_name = tc.constraint_name\n"
    " AND kcu.constraint_schema = tc.constraint_schema\n"
    "JOIN information_schema.constraint_column_usage ccu\n"
    "  ON ccu.constraint_name = tc.constraint_name\n"
    " AND ccu.constraint_schema = tc.constraint_schema\n"
    "WHERE tc.constraint_type = 'FOREIGN KEY'\n"
    "  AND tc.table_schema = 'public'\n"
    "  AND tc.table_name IN\n"
    "      ('task_message', 'issue', 'workspace', 'agent_task_queue', 'agent')\n"
    "ORDER BY 1, 2, 3;\n";

/* ── The pinned result-set columns ──────────────────────────────────────── */

const char *const HYP_MULTICA_MESSAGES_COLUMNS[] = {
    "id",         "task_id",   "seq",
    "type",       "tool",      "content",
    "input",      "output",    "created_at_ms",
    "agent_name", "parent_task_id", "issue_id",
    "session_id", "parent_task_first_message_id", "attribution_fail_closed",
    NULL};

enum {
    MC_ID = 0,
    MC_TASK_ID,
    MC_SEQ,
    MC_TYPE,
    MC_TOOL,
    MC_CONTENT,
    MC_INPUT,
    MC_OUTPUT,
    MC_CREATED_AT_MS,
    MC_AGENT_NAME,
    MC_PARENT_TASK_ID,
    MC_ISSUE_ID,
    MC_SESSION_ID,
    MC_PARENT_TASK_FIRST_MESSAGE_ID,
    MC_ATTRIBUTION_FAIL_CLOSED
};

const char *const HYP_MULTICA_ISSUES_COLUMNS[] = {
    "id",           "workspace_id", "title",           "description",
    "status",       "priority",     "number",          "creator_type",
    "creator_id",   "parent_issue_id", "created_at_ms", "attribution_fail_closed",
    NULL};

enum {
    IC_ID = 0,
    IC_WORKSPACE_ID,
    IC_TITLE,
    IC_DESCRIPTION,
    IC_STATUS,
    IC_PRIORITY,
    IC_NUMBER,
    IC_CREATOR_TYPE,
    IC_CREATOR_ID,
    IC_PARENT_ISSUE_ID,
    IC_CREATED_AT_MS,
    IC_ATTRIBUTION_FAIL_CLOSED
};

const char *const HYP_MULTICA_WORKSPACES_COLUMNS[] = {
    "id", "name", "slug", "repos", "issue_prefix", "attribution_fail_closed", NULL};

enum { WC_ID = 0, WC_NAME, WC_SLUG, WC_REPOS, WC_ISSUE_PREFIX, WC_ATTRIBUTION_FAIL_CLOSED };

static const char *const SCHEMA_PIN_COLUMNS[] = {"pin_kind", "pin_name", "pin_detail", NULL};

/* The value-level end of the schema's CHECK constraints. A value outside these
 * sets means the live schema drifted from the verified one: schema refusal,
 * not a row problem. */
static const char *const ISSUE_STATUS_VALUES[] = {
    "backlog", "todo", "in_progress", "in_review", "done", "blocked", "cancelled", NULL};
static const char *const ISSUE_PRIORITY_VALUES[] = {"urgent", "high", "medium", "low", "none",
                                                    NULL};
static const char *const ISSUE_CREATOR_TYPE_VALUES[] = {"member", "agent", NULL};

/* ── Small helpers ──────────────────────────────────────────────────────── */

static char *dup_str(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (out) {
        memcpy(out, s, n);
    }
    return out;
}

static bool parse_i64(const char *s, int64_t *out) {
    if (!s || s[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

/* The flag column is boolean::text in the pinned SQL, so exactly "true" or
 * "false" arrives; SQL NULL is unknown. Anything else is not our SQL. */
static bool parse_tristate(const char *s, int *out) {
    if (!s) {
        *out = -1;
        return true;
    }
    if (strcmp(s, "true") == 0) {
        *out = 1;
        return true;
    }
    if (strcmp(s, "false") == 0) {
        *out = 0;
        return true;
    }
    return false;
}

static bool value_in(const char *value, const char *const *set) {
    for (size_t i = 0; set[i]; i++) {
        if (strcmp(value, set[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Growable text buffer for the composed strings a yield borrows. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} sb_t;

static void sb_reset(sb_t *sb) {
    sb->len = 0;
    sb->oom = false;
    if (sb->buf) {
        sb->buf[0] = '\0';
    }
}

static void sb_free(sb_t *sb) {
    free(sb->buf);
    memset(sb, 0, sizeof(*sb));
}

static void sb_put(sb_t *sb, const char *s) {
    if (sb->oom) {
        return;
    }
    size_t n = strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 64;
        while (cap < sb->len + n + 1) {
            cap *= 2;
        }
        char *grown = (char *)realloc(sb->buf, cap);
        if (!grown) {
            sb->oom = true;
            return;
        }
        sb->buf = grown;
        sb->cap = cap;
    }
    memcpy(sb->buf + sb->len, s, n + 1);
    sb->len += n;
}

/* ── Column pinning ─────────────────────────────────────────────────────── */

static hyp_feed_status_t pin_columns(hyp_multica_rows_t *rows, const char *const *expected) {
    if (!rows || !rows->column_count || !rows->column_name || !rows->next_row || !rows->value) {
        return HYP_FEED_ERR_NULL;
    }
    size_t n = 0;
    while (expected[n]) {
        n++;
    }
    /* Exact count: a missing column AND an extra column both refuse. The SQL
     * is ours; a result set we did not ask for is not an answer to it. */
    if (rows->column_count(rows) != n) {
        return HYP_FEED_ERR_SCHEMA;
    }
    for (size_t i = 0; i < n; i++) {
        const char *name = rows->column_name(rows, i);
        if (!name || strcmp(name, expected[i]) != 0) {
            return HYP_FEED_ERR_SCHEMA;
        }
    }
    return HYP_FEED_OK;
}

/* ── Attribution rules ──────────────────────────────────────────────────── */

static hyp_feed_status_t compose_author(char *out, size_t cap, const char *a, const char *b,
                                        const char *c, const char *d) {
    size_t need = strlen(a) + strlen(b) + strlen(c) + strlen(d) + 1;
    if (need > cap || need - 1 > HYP_RECORD_MAX_AUTHOR) {
        return HYP_FEED_ERR_SOURCE; /* a cap is a refusal, never a truncation */
    }
    snprintf(out, cap, "%s%s%s%s", a, b, c, d);
    return HYP_FEED_OK;
}

hyp_feed_status_t hyp_multica_author_of(const char *agent_name, int attribution_fail_closed,
                                        char *out, size_t cap) {
    if (!out || cap == 0) {
        return HYP_FEED_ERR_NULL;
    }
    if (agent_name && agent_name[0] != '\0') {
        return compose_author(out, cap, "multica:agent:", agent_name, "", "");
    }
    if (attribution_fail_closed == 0) {
        return compose_author(out, cap, "multica:agent:unattributed", "", "", "");
    }
    /* true, or unknown treated as true: fail closed is the default, never the
     * fallback. */
    return HYP_FEED_ERR_SOURCE;
}

hyp_feed_status_t hyp_multica_issue_author_of(const char *creator_type, const char *creator_id,
                                              int attribution_fail_closed, char *out, size_t cap) {
    if (!out || cap == 0) {
        return HYP_FEED_ERR_NULL;
    }
    if (creator_type && creator_type[0] != '\0' && creator_id && creator_id[0] != '\0') {
        return compose_author(out, cap, "multica:", creator_type, ":", creator_id);
    }
    if (attribution_fail_closed == 0) {
        return compose_author(out, cap, "multica:creator:unattributed", "", "", "");
    }
    return HYP_FEED_ERR_SOURCE;
}

/* ── The messages feed ──────────────────────────────────────────────────── */

typedef struct {
    hyp_multica_rows_t *rows;
    /* Ordering pin: tasks must arrive as contiguous groups, ascending
     * (seq NULLS LAST, id) within each. */
    HYPHashTable *finished; /* task ids whose group has closed */
    char **retired;         /* owned key strings referenced by `finished` */
    size_t retired_count;
    size_t retired_cap;
    char *cur_task;
    bool have_prev;
    bool prev_seq_null;
    int64_t prev_seq;
    char *prev_id;
    /* The previous YIELDED origin of the current task: skipped rows are not
     * precursors, so chains pass over them. */
    char *last_origin;
    /* Yield buffers, valid until the next call. */
    sb_t origin;
    sb_t thread;
    sb_t author;
    sb_t content;
    sb_t parent;
} mx_msgs_t;

static void mx_msgs_free(mx_msgs_t *st) {
    if (!st) {
        return;
    }
    hyp_ht_free(st->finished);
    for (size_t i = 0; i < st->retired_count; i++) {
        free(st->retired[i]);
    }
    free(st->retired);
    free(st->cur_task);
    free(st->prev_id);
    free(st->last_origin);
    sb_free(&st->origin);
    sb_free(&st->thread);
    sb_free(&st->author);
    sb_free(&st->content);
    sb_free(&st->parent);
    free(st);
}

static void mx_msgs_close(hyp_feed_source_t *src) {
    if (src) {
        mx_msgs_free((mx_msgs_t *)src->ctx);
        src->ctx = NULL;
    }
}

static bool replace_owned(char **slot, const char *value) {
    char *copy = dup_str(value);
    if (value && !copy) {
        return false;
    }
    free(*slot);
    *slot = copy;
    return true;
}

/* Contiguity + in-group order. Returns false on an ordering violation. */
static hyp_feed_status_t mx_msgs_order_pin(mx_msgs_t *st, const char *task, const char *id,
                                           const char *seq_cell) {
    if (!st->cur_task || strcmp(task, st->cur_task) != 0) {
        if (hyp_ht_has(st->finished, task)) {
            return HYP_FEED_ERR_SOURCE; /* a group came back: not our ORDER BY */
        }
        if (st->cur_task) {
            if (st->retired_count == st->retired_cap) {
                size_t cap = st->retired_cap ? st->retired_cap * 2 : 16;
                char **grown = (char **)realloc(st->retired, cap * sizeof(*grown));
                if (!grown) {
                    return HYP_FEED_ERR_ALLOC;
                }
                st->retired = grown;
                st->retired_cap = cap;
            }
            st->retired[st->retired_count++] = st->cur_task;
            hyp_ht_set(st->finished, st->cur_task, st);
            st->cur_task = NULL;
        }
        st->cur_task = dup_str(task);
        if (!st->cur_task) {
            return HYP_FEED_ERR_ALLOC;
        }
        st->have_prev = false;
        free(st->last_origin);
        st->last_origin = NULL;
    }

    bool seq_null = (seq_cell == NULL);
    int64_t seq_value = 0;
    if (!seq_null && !parse_i64(seq_cell, &seq_value)) {
        return HYP_FEED_ERR_SOURCE;
    }
    if (st->have_prev) {
        bool ok;
        if (st->prev_seq_null) {
            /* NULLS LAST: once a group's null-seq tail starts, only null-seq
             * rows may follow, ascending by id. */
            ok = seq_null && strcmp(id, st->prev_id) > 0;
        } else if (seq_null) {
            ok = true; /* the null tail begins */
        } else if (seq_value != st->prev_seq) {
            ok = seq_value > st->prev_seq;
        } else {
            ok = strcmp(id, st->prev_id) > 0;
        }
        if (!ok) {
            return HYP_FEED_ERR_SOURCE;
        }
    }
    st->have_prev = true;
    st->prev_seq_null = seq_null;
    st->prev_seq = seq_value;
    if (!replace_owned(&st->prev_id, id)) {
        return HYP_FEED_ERR_ALLOC;
    }
    return HYP_FEED_OK;
}

static hyp_feed_status_t mx_msgs_next(hyp_feed_source_t *src, hyp_feed_item_t *out) {
    mx_msgs_t *st = (mx_msgs_t *)src->ctx;
    if (!st || !out) {
        return HYP_FEED_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));

    int r = st->rows->next_row(st->rows);
    if (r == 0) {
        return HYP_FEED_END;
    }
    if (r < 0) {
        return HYP_FEED_ERR_SOURCE;
    }
    const hyp_multica_rows_t *rows = st->rows;

    /* Identity first: a row that cannot be named cannot even be skipped. */
    const char *id = rows->value(rows, MC_ID);
    if (!id || id[0] == '\0') {
        return HYP_FEED_ERR_SOURCE;
    }
    sb_reset(&st->origin);
    sb_put(&st->origin, "multica:task_message:");
    sb_put(&st->origin, id);
    if (st->origin.oom) {
        return HYP_FEED_ERR_ALLOC;
    }
    out->origin = st->origin.buf;

    const char *task = rows->value(rows, MC_TASK_ID);
    if (!task) {
        out->reason = HYP_MULTICA_SKIP_UNOWNED;
        return HYP_FEED_SKIP;
    }

    hyp_feed_status_t st_order = mx_msgs_order_pin(st, task, id, rows->value(rows, MC_SEQ));
    if (st_order != HYP_FEED_OK) {
        return st_order;
    }

    const char *when = rows->value(rows, MC_CREATED_AT_MS);
    if (!when) {
        out->reason = HYP_MULTICA_SKIP_NO_TIMESTAMP;
        return HYP_FEED_SKIP;
    }
    int64_t when_ms = 0;
    if (!parse_i64(when, &when_ms)) {
        return HYP_FEED_ERR_SOURCE;
    }

    const char *content = rows->value(rows, MC_CONTENT);
    const char *input = rows->value(rows, MC_INPUT);
    const char *output = rows->value(rows, MC_OUTPUT);
    if (!content && !input && !output) {
        out->reason = HYP_MULTICA_SKIP_EMPTY;
        return HYP_FEED_SKIP;
    }

    int fail_closed = -1;
    if (!parse_tristate(rows->value(rows, MC_ATTRIBUTION_FAIL_CLOSED), &fail_closed)) {
        return HYP_FEED_ERR_SOURCE;
    }
    char author_buf[HYP_RECORD_MAX_AUTHOR + 1];
    hyp_feed_status_t st_author =
        hyp_multica_author_of(rows->value(rows, MC_AGENT_NAME), fail_closed, author_buf,
                              sizeof(author_buf));
    if (st_author != HYP_FEED_OK) {
        return st_author;
    }
    sb_reset(&st->author);
    sb_put(&st->author, author_buf);

    sb_reset(&st->thread);
    sb_put(&st->thread, "multica:task:");
    sb_put(&st->thread, task);

    /* The precursor: the task's previous yielded message; for a task's first
     * message, the first message of its delegation parent, when it has one. */
    sb_reset(&st->parent);
    if (st->last_origin) {
        sb_put(&st->parent, st->last_origin);
    } else {
        const char *pf = rows->value(rows, MC_PARENT_TASK_FIRST_MESSAGE_ID);
        if (pf && pf[0] != '\0') {
            sb_put(&st->parent, "multica:task_message:");
            sb_put(&st->parent, pf);
        }
    }

    /* The composition rule, exactly as the header states it. */
    const char *type = rows->value(rows, MC_TYPE);
    sb_reset(&st->content);
    sb_put(&st->content, "type=");
    sb_put(&st->content, type ? type : "~");
    const char *tool = rows->value(rows, MC_TOOL);
    if (tool) {
        sb_put(&st->content, " tool=");
        sb_put(&st->content, tool);
    }
    if (content) {
        sb_put(&st->content, "\ncontent:\n");
        sb_put(&st->content, content);
    }
    if (input) {
        sb_put(&st->content, "\ninput:\n");
        sb_put(&st->content, input);
    }
    if (output) {
        sb_put(&st->content, "\noutput:\n");
        sb_put(&st->content, output);
    }

    if (st->author.oom || st->thread.oom || st->parent.oom || st->content.oom) {
        return HYP_FEED_ERR_ALLOC;
    }
    if (!replace_owned(&st->last_origin, st->origin.buf)) {
        return HYP_FEED_ERR_ALLOC;
    }

    out->kind = HYP_RECORD_MESSAGE;
    out->author = st->author.buf;
    out->timestamp_ms = when_ms;
    out->content = st->content.buf;
    out->thread = st->thread.buf;
    out->parent_origin = st->parent.len > 0 ? st->parent.buf : NULL;
    out->redactions = 0;
    out->reason = NULL;
    return HYP_FEED_OK;
}

hyp_feed_status_t hyp_multica_messages_open(hyp_multica_rows_t *rows, hyp_feed_source_t *src) {
    if (!src) {
        return HYP_FEED_ERR_NULL;
    }
    memset(src, 0, sizeof(*src));
    hyp_feed_status_t st = pin_columns(rows, HYP_MULTICA_MESSAGES_COLUMNS);
    if (st != HYP_FEED_OK) {
        return st;
    }
    mx_msgs_t *state = (mx_msgs_t *)calloc(HYP_ALLOC_ONE, sizeof(*state));
    if (!state) {
        return HYP_FEED_ERR_ALLOC;
    }
    state->rows = rows;
    state->finished = hyp_ht_create(0);
    if (!state->finished) {
        mx_msgs_free(state);
        return HYP_FEED_ERR_ALLOC;
    }
    src->name = "multica:task_message";
    src->ctx = state;
    src->next = mx_msgs_next;
    src->close = mx_msgs_close;
    return HYP_FEED_OK;
}

/* ── The issues feed ────────────────────────────────────────────────────── */

typedef struct {
    hyp_multica_rows_t *rows;
    char *prev_id; /* ordering pin: ascending id */
    sb_t origin;
    sb_t author;
    sb_t content;
    sb_t parent;
} mx_issues_t;

static void mx_issues_free(mx_issues_t *st) {
    if (!st) {
        return;
    }
    free(st->prev_id);
    sb_free(&st->origin);
    sb_free(&st->author);
    sb_free(&st->content);
    sb_free(&st->parent);
    free(st);
}

static void mx_issues_close(hyp_feed_source_t *src) {
    if (src) {
        mx_issues_free((mx_issues_t *)src->ctx);
        src->ctx = NULL;
    }
}

static hyp_feed_status_t mx_issues_next(hyp_feed_source_t *src, hyp_feed_item_t *out) {
    mx_issues_t *st = (mx_issues_t *)src->ctx;
    if (!st || !out) {
        return HYP_FEED_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));

    int r = st->rows->next_row(st->rows);
    if (r == 0) {
        return HYP_FEED_END;
    }
    if (r < 0) {
        return HYP_FEED_ERR_SOURCE;
    }
    const hyp_multica_rows_t *rows = st->rows;

    const char *id = rows->value(rows, IC_ID);
    if (!id || id[0] == '\0') {
        return HYP_FEED_ERR_SOURCE;
    }
    if (st->prev_id && strcmp(id, st->prev_id) <= 0) {
        return HYP_FEED_ERR_SOURCE; /* not our ORDER BY */
    }
    if (!replace_owned(&st->prev_id, id)) {
        return HYP_FEED_ERR_ALLOC;
    }
    sb_reset(&st->origin);
    sb_put(&st->origin, "multica:issue:");
    sb_put(&st->origin, id);
    if (st->origin.oom) {
        return HYP_FEED_ERR_ALLOC;
    }
    out->origin = st->origin.buf;

    /* The value-level end of the schema's CHECK constraints: a value outside
     * the pinned enums is schema drift, refused as such. */
    const char *status = rows->value(rows, IC_STATUS);
    if (status && !value_in(status, ISSUE_STATUS_VALUES)) {
        return HYP_FEED_ERR_SCHEMA;
    }
    const char *priority = rows->value(rows, IC_PRIORITY);
    if (priority && !value_in(priority, ISSUE_PRIORITY_VALUES)) {
        return HYP_FEED_ERR_SCHEMA;
    }
    const char *creator_type = rows->value(rows, IC_CREATOR_TYPE);
    if (creator_type && !value_in(creator_type, ISSUE_CREATOR_TYPE_VALUES)) {
        return HYP_FEED_ERR_SCHEMA;
    }

    const char *when = rows->value(rows, IC_CREATED_AT_MS);
    if (!when) {
        out->reason = HYP_MULTICA_SKIP_NO_TIMESTAMP;
        return HYP_FEED_SKIP;
    }
    int64_t when_ms = 0;
    if (!parse_i64(when, &when_ms)) {
        return HYP_FEED_ERR_SOURCE;
    }

    const char *title = rows->value(rows, IC_TITLE);
    const char *description = rows->value(rows, IC_DESCRIPTION);
    if (!title && !description) {
        out->reason = HYP_MULTICA_SKIP_EMPTY;
        return HYP_FEED_SKIP;
    }

    int fail_closed = -1;
    if (!parse_tristate(rows->value(rows, IC_ATTRIBUTION_FAIL_CLOSED), &fail_closed)) {
        return HYP_FEED_ERR_SOURCE;
    }
    char author_buf[HYP_RECORD_MAX_AUTHOR + 1];
    hyp_feed_status_t st_author = hyp_multica_issue_author_of(
        creator_type, rows->value(rows, IC_CREATOR_ID), fail_closed, author_buf,
        sizeof(author_buf));
    if (st_author != HYP_FEED_OK) {
        return st_author;
    }
    sb_reset(&st->author);
    sb_put(&st->author, author_buf);

    sb_reset(&st->parent);
    const char *parent_id = rows->value(rows, IC_PARENT_ISSUE_ID);
    if (parent_id && parent_id[0] != '\0') {
        sb_put(&st->parent, "multica:issue:");
        sb_put(&st->parent, parent_id);
    }

    const char *number = rows->value(rows, IC_NUMBER);
    sb_reset(&st->content);
    sb_put(&st->content, "title=");
    sb_put(&st->content, title ? title : "~");
    sb_put(&st->content, "\nstatus=");
    sb_put(&st->content, status ? status : "~");
    sb_put(&st->content, " priority=");
    sb_put(&st->content, priority ? priority : "~");
    sb_put(&st->content, " number=");
    sb_put(&st->content, number ? number : "~");
    if (description) {
        sb_put(&st->content, "\n\n");
        sb_put(&st->content, description);
    }

    if (st->author.oom || st->parent.oom || st->content.oom) {
        return HYP_FEED_ERR_ALLOC;
    }

    out->kind = HYP_RECORD_SIGNAL;
    out->author = st->author.buf;
    out->timestamp_ms = when_ms;
    out->content = st->content.buf;
    out->thread = st->origin.buf; /* an issue heads its own conversation */
    out->parent_origin = st->parent.len > 0 ? st->parent.buf : NULL;
    out->redactions = 0;
    out->reason = NULL;
    return HYP_FEED_OK;
}

hyp_feed_status_t hyp_multica_issues_open(hyp_multica_rows_t *rows, hyp_feed_source_t *src) {
    if (!src) {
        return HYP_FEED_ERR_NULL;
    }
    memset(src, 0, sizeof(*src));
    hyp_feed_status_t st = pin_columns(rows, HYP_MULTICA_ISSUES_COLUMNS);
    if (st != HYP_FEED_OK) {
        return st;
    }
    mx_issues_t *state = (mx_issues_t *)calloc(HYP_ALLOC_ONE, sizeof(*state));
    if (!state) {
        return HYP_FEED_ERR_ALLOC;
    }
    state->rows = rows;
    src->name = "multica:issue";
    src->ctx = state;
    src->next = mx_issues_next;
    src->close = mx_issues_close;
    return HYP_FEED_OK;
}

/* ── The workspace pull ─────────────────────────────────────────────────── */

void hyp_multica_workspaces_free(hyp_multica_workspace_t *items, size_t count) {
    if (!items) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(items[i].id);
        free(items[i].name);
        free(items[i].slug);
        free(items[i].repos_json);
        free(items[i].issue_prefix);
    }
    free(items);
}

/* The declared, UNVERIFIED inner pin on workspace.repos: a JSON array, or SQL
 * NULL. The column is verified; no live row has been read. Confirm against
 * one before the resolver (A6) relies on this. */
static bool repos_shape_ok(const char *repos_json) {
    yyjson_doc *doc = yyjson_read(repos_json, strlen(repos_json), 0);
    if (!doc) {
        return false;
    }
    bool ok = yyjson_is_arr(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    return ok;
}

hyp_feed_status_t hyp_multica_workspaces_pull(hyp_multica_rows_t *rows,
                                              hyp_multica_workspace_t **out, size_t *out_count) {
    if (!out || !out_count) {
        return HYP_FEED_ERR_NULL;
    }
    *out = NULL;
    *out_count = 0;
    hyp_feed_status_t st = pin_columns(rows, HYP_MULTICA_WORKSPACES_COLUMNS);
    if (st != HYP_FEED_OK) {
        return st;
    }

    hyp_multica_workspace_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;
    for (;;) {
        int r = rows->next_row(rows);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            st = HYP_FEED_ERR_SOURCE;
            goto fail;
        }
        const char *id = rows->value(rows, WC_ID);
        if (!id || id[0] == '\0') {
            st = HYP_FEED_ERR_SOURCE;
            goto fail;
        }
        const char *repos = rows->value(rows, WC_REPOS);
        if (repos && !repos_shape_ok(repos)) {
            st = HYP_FEED_ERR_SCHEMA;
            goto fail;
        }
        int fail_closed = -1;
        if (!parse_tristate(rows->value(rows, WC_ATTRIBUTION_FAIL_CLOSED), &fail_closed)) {
            st = HYP_FEED_ERR_SOURCE;
            goto fail;
        }

        if (count == cap) {
            size_t cap_next = cap ? cap * 2 : 8;
            hyp_multica_workspace_t *grown =
                (hyp_multica_workspace_t *)realloc(items, cap_next * sizeof(*grown));
            if (!grown) {
                st = HYP_FEED_ERR_ALLOC;
                goto fail;
            }
            items = grown;
            cap = cap_next;
        }
        hyp_multica_workspace_t *slot = &items[count];
        memset(slot, 0, sizeof(*slot));
        slot->id = dup_str(id);
        slot->name = dup_str(rows->value(rows, WC_NAME));
        slot->slug = dup_str(rows->value(rows, WC_SLUG));
        slot->repos_json = dup_str(repos);
        slot->issue_prefix = dup_str(rows->value(rows, WC_ISSUE_PREFIX));
        slot->attribution_fail_closed = fail_closed;
        count++; /* counted before the null checks so a failed slot is freed */
        if (!slot->id || (rows->value(rows, WC_NAME) && !slot->name) ||
            (rows->value(rows, WC_SLUG) && !slot->slug) || (repos && !slot->repos_json) ||
            (rows->value(rows, WC_ISSUE_PREFIX) && !slot->issue_prefix)) {
            st = HYP_FEED_ERR_ALLOC;
            goto fail;
        }
    }

    *out = items;
    *out_count = count;
    return HYP_FEED_OK;

fail:
    hyp_multica_workspaces_free(items, count);
    return st;
}

/* ── The schema pin ─────────────────────────────────────────────────────── */

/* The verified column SETS, pinned exactly: a missing column and an extra
 * column on these tables both refuse. */
static const char *const TASK_MESSAGE_TABLE_COLUMNS[] = {
    "task_message.id",      "task_message.task_id", "task_message.seq",
    "task_message.type",    "task_message.tool",    "task_message.content",
    "task_message.input",   "task_message.output",  "task_message.created_at",
    NULL};

static const char *const ISSUE_TABLE_COLUMNS[] = {
    "issue.id",           "issue.workspace_id",       "issue.title",
    "issue.description",  "issue.status",             "issue.priority",
    "issue.assignee_type", "issue.assignee_id",       "issue.creator_type",
    "issue.creator_id",   "issue.parent_issue_id",    "issue.acceptance_criteria",
    "issue.context_refs", "issue.position",           "issue.due_date",
    "issue.created_at",   "issue.updated_at",         "issue.number",
    "issue.project_id",   "issue.origin_type",        "issue.origin_id",
    "issue.first_executed_at", "issue.start_date",    "issue.metadata",
    "issue.stage",        "issue.properties",         NULL};

static const char *const WORKSPACE_TABLE_COLUMNS[] = {
    "workspace.id",          "workspace.name",          "workspace.slug",
    "workspace.description", "workspace.settings",      "workspace.created_at",
    "workspace.updated_at",  "workspace.context",       "workspace.repos",
    "workspace.issue_prefix", "workspace.issue_counter", "workspace.avatar_url",
    "workspace.attribution_fail_closed", NULL};

/* Consumed subsets: these must exist; the tables' other columns are pinned as
 * IGNORED and may come and go. For agent_task_queue that explicitly includes
 * delegated_from_task_id, retry_of_task_id and rerun_of_task_id — lineage is
 * read from parent_task_id ONLY. */
static const char *const QUEUE_REQUIRED_COLUMNS[] = {
    "agent_task_queue.id",          "agent_task_queue.agent_id",
    "agent_task_queue.parent_task_id", "agent_task_queue.issue_id",
    "agent_task_queue.session_id",  "agent_task_queue.work_dir",
    "agent_task_queue.started_at",  "agent_task_queue.completed_at",
    "agent_task_queue.result",      "agent_task_queue.status",
    NULL};

static const char *const AGENT_REQUIRED_COLUMNS[] = {"agent.id", "agent.name",
                                                     "agent.workspace_id", "agent.kind",
                                                     "agent.model", NULL};

/* The four foreign keys the mapping rides on. The first is the loud-death pin:
 * without it a renamed or unkeyed table would join to NULLs forever. */
static const struct {
    const char *from;
    const char *to;
} REQUIRED_FKS[] = {
    {"task_message.task_id", "agent_task_queue.id"},
    {"agent_task_queue.agent_id", "agent.id"},
    {"agent_task_queue.issue_id", "issue.id"},
    {"agent_task_queue.parent_task_id", "agent_task_queue.id"},
};

enum { REQUIRED_FK_COUNT = sizeof(REQUIRED_FKS) / sizeof(REQUIRED_FKS[0]) };

static size_t list_len(const char *const *list) {
    size_t n = 0;
    while (list[n]) {
        n++;
    }
    return n;
}

static bool mark_in(const char *const *list, bool *found, const char *name) {
    for (size_t i = 0; list[i]; i++) {
        if (strcmp(list[i], name) == 0) {
            found[i] = true;
            return true;
        }
    }
    return false;
}

static bool has_prefix(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

hyp_feed_status_t hyp_multica_schema_pin_check(hyp_multica_rows_t *rows) {
    hyp_feed_status_t st = pin_columns(rows, SCHEMA_PIN_COLUMNS);
    if (st != HYP_FEED_OK) {
        return st;
    }

    const char *const *exact[] = {TASK_MESSAGE_TABLE_COLUMNS, ISSUE_TABLE_COLUMNS,
                                  WORKSPACE_TABLE_COLUMNS};
    static const char *const exact_prefix[] = {"task_message.", "issue.", "workspace."};
    const char *const *subset[] = {QUEUE_REQUIRED_COLUMNS, AGENT_REQUIRED_COLUMNS};
    static const char *const subset_prefix[] = {"agent_task_queue.", "agent."};

    bool *found[3] = {NULL, NULL, NULL};
    bool *found_subset[2] = {NULL, NULL};
    bool found_fk[REQUIRED_FK_COUNT] = {false};
    for (size_t t = 0; t < 3; t++) {
        found[t] = (bool *)calloc(list_len(exact[t]), sizeof(bool));
    }
    for (size_t t = 0; t < 2; t++) {
        found_subset[t] = (bool *)calloc(list_len(subset[t]), sizeof(bool));
    }
    if (!found[0] || !found[1] || !found[2] || !found_subset[0] || !found_subset[1]) {
        st = HYP_FEED_ERR_ALLOC;
        goto done;
    }

    for (;;) {
        int r = rows->next_row(rows);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            st = HYP_FEED_ERR_SOURCE;
            goto done;
        }
        const char *pin_kind = rows->value(rows, 0);
        const char *pin_name = rows->value(rows, 1);
        const char *pin_detail = rows->value(rows, 2);
        if (!pin_kind || !pin_name) {
            st = HYP_FEED_ERR_SOURCE;
            goto done;
        }
        if (strcmp(pin_kind, "column") == 0) {
            bool matched = false;
            for (size_t t = 0; t < 3 && !matched; t++) {
                if (has_prefix(pin_name, exact_prefix[t])) {
                    if (!mark_in(exact[t], found[t], pin_name)) {
                        st = HYP_FEED_ERR_SCHEMA; /* an EXTRA column on an
                                                     exactly pinned table */
                        goto done;
                    }
                    matched = true;
                }
            }
            for (size_t t = 0; t < 2 && !matched; t++) {
                if (has_prefix(pin_name, subset_prefix[t])) {
                    (void)mark_in(subset[t], found_subset[t], pin_name);
                    matched = true; /* extras here are the IGNORED pin */
                }
            }
            if (!matched) {
                st = HYP_FEED_ERR_SCHEMA; /* a table our SQL never asked about */
                goto done;
            }
        } else if (strcmp(pin_kind, "fk") == 0) {
            if (!pin_detail) {
                st = HYP_FEED_ERR_SOURCE;
                goto done;
            }
            for (size_t i = 0; i < (size_t)REQUIRED_FK_COUNT; i++) {
                if (strcmp(pin_name, REQUIRED_FKS[i].from) == 0 &&
                    strcmp(pin_detail, REQUIRED_FKS[i].to) == 0) {
                    found_fk[i] = true;
                }
            }
            /* Other foreign keys among these tables are tolerated: we require
             * ours, we do not forbid theirs. */
        } else {
            st = HYP_FEED_ERR_SCHEMA; /* not a row our pinned SQL emits */
            goto done;
        }
    }

    for (size_t t = 0; t < 3; t++) {
        for (size_t i = 0; exact[t][i]; i++) {
            if (!found[t][i]) {
                st = HYP_FEED_ERR_SCHEMA; /* a verified column is gone */
                goto done;
            }
        }
    }
    for (size_t t = 0; t < 2; t++) {
        for (size_t i = 0; subset[t][i]; i++) {
            if (!found_subset[t][i]) {
                st = HYP_FEED_ERR_SCHEMA; /* a consumed column is gone */
                goto done;
            }
        }
    }
    for (size_t i = 0; i < (size_t)REQUIRED_FK_COUNT; i++) {
        if (!found_fk[i]) {
            st = HYP_FEED_ERR_SCHEMA; /* a load-bearing foreign key is gone */
            goto done;
        }
    }
    st = HYP_FEED_OK;

done:
    for (size_t t = 0; t < 3; t++) {
        free(found[t]);
    }
    for (size_t t = 0; t < 2; t++) {
        free(found_subset[t]);
    }
    return st;
}
