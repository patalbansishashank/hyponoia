/*
 * record_store.c — the record set, made durable. See record_store.h for the
 * contract: why this is an id-keyed set and not a log, why append is the
 * entire write surface, and what the reader refuses to know.
 *
 * Three structural properties, mirrored from record.c's discipline:
 *
 *   1. NO CLOCK (I6). Timestamps arrive on records; nothing here captures one.
 *      There is no <time.h> in this file and no "created_at"-style column for
 *      the store to invent one in.
 *   2. NOTHING IS PARSED (I7). anchor/origin/thread are bound and compared as
 *      bytes. Equality happens in the engine under BINARY collation (byte
 *      equality); prefix happens HERE, as memcmp, precisely because the
 *      engine's LIKE/GLOB/substr each carry parsing semantics (wildcards,
 *      case folding, character counting) that opaque bytes must never meet.
 *   3. NO ORDER BUT THE CANONICAL ONE. Every SELECT that can return more than
 *      one row says ORDER BY id, and the table is WITHOUT ROWID so there is no
 *      insertion counter to accidentally order by, expose, or sync.
 */
#include "store/record_store.h"

#include "foundation/compat_fs.h" /* hyp_mkdir_p */
#include "foundation/constants.h" /* HYP_ALLOC_ONE, HYP_SZ_512, HYP_NOT_FOUND */

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RS_COL_ID = 0, /* SELECT column order — matches RS_SELECT below */
    RS_COL_KIND = 1,
    RS_COL_AUTHOR = 2,
    RS_COL_TIMESTAMP = 3,
    RS_COL_CONTENT = 4,
    RS_COL_ANCHOR = 5,
    RS_COL_ORIGIN = 6,
    RS_COL_THREAD = 7,
    RS_COL_PARENT = 8,
    RS_COL_REDACTIONS = 9,
    RS_DIR_MODE = 0700, /* records may hold things said in private */
    RS_SQL_MAX = 1024,  /* worst-case assembled query, all clauses active */
};

struct hyp_record_store {
    sqlite3 *db;
    char errbuf[HYP_SZ_512];

    /* Prepared statements for the fixed-shape paths (lazily created, cached
     * for the handle's lifetime). Queries are assembled per call. */
    sqlite3_stmt *stmt_insert;
    sqlite3_stmt *stmt_get;
    sqlite3_stmt *stmt_count;
};

/* ── Small helpers ──────────────────────────────────────────────────────── */

const char *hyp_record_store_status_reason(hyp_record_store_status_t status) {
    switch (status) {
    case HYP_RECORD_STORE_OK:
        return "ok";
    case HYP_RECORD_STORE_ERR_NULL:
        return "a required argument was NULL";
    case HYP_RECORD_STORE_ERR_OPEN:
        return "the store directory or database could not be opened";
    case HYP_RECORD_STORE_ERR_CONTRACT:
        return "the store on disk was written under a different record contract";
    case HYP_RECORD_STORE_ERR_IO:
        return "the storage engine refused the operation";
    case HYP_RECORD_STORE_ERR_TAMPERED:
        return "a record's id does not match its fields — it was altered outside the contract";
    case HYP_RECORD_STORE_ERR_COLLISION:
        return "two different records claim the same id — refused, never merged";
    case HYP_RECORD_STORE_ERR_QUERY:
        return "the query contradicts itself and can never be a real question";
    case HYP_RECORD_STORE_ERR_ALLOC:
        return "out of memory";
    }
    return "unknown record store status";
}

const char *hyp_record_store_error(const hyp_record_store_t *store) {
    if (!store || !store->errbuf[0]) {
        return "";
    }
    return store->errbuf;
}

static void rs_set_error(hyp_record_store_t *s, const char *msg) {
    (void)snprintf(s->errbuf, sizeof(s->errbuf), "%s", msg);
}

static void rs_set_error_sqlite(hyp_record_store_t *s, const char *prefix) {
    (void)snprintf(s->errbuf, sizeof(s->errbuf), "%s: %s", prefix, sqlite3_errmsg(s->db));
}

/* Isolate the int-to-ptr cast SQLITE_TRANSIENT expands to (store.c idiom). */
static sqlite3_destructor_type rs_transient(void) {
    union {
        uintptr_t i;
        sqlite3_destructor_type fn;
    } u;
    u.i = (uintptr_t)HYP_NOT_FOUND;
    return u.fn;
}

static int rs_bind_text(sqlite3_stmt *stmt, int col, const char *v) {
    return sqlite3_bind_text(stmt, col, v, HYP_NOT_FOUND, rs_transient());
}

/* Bind text, or NULL as SQL NULL — absent and empty are different answers,
 * and the column keeps the distinction. */
static int rs_bind_text_opt(sqlite3_stmt *stmt, int col, const char *v) {
    if (!v) {
        return sqlite3_bind_null(stmt, col);
    }
    return rs_bind_text(stmt, col, v);
}

static int rs_exec(hyp_record_store_t *s, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(s->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        (void)snprintf(s->errbuf, sizeof(s->errbuf), "exec: %s", err ? err : "unknown");
        sqlite3_free(err);
        return rc;
    }
    return SQLITE_OK;
}

static sqlite3_stmt *rs_prepare_cached(hyp_record_store_t *s, sqlite3_stmt **slot,
                                       const char *sql) {
    if (*slot) {
        (void)sqlite3_reset(*slot);
        (void)sqlite3_clear_bindings(*slot);
        return *slot;
    }
    if (sqlite3_prepare_v2(s->db, sql, HYP_NOT_FOUND, slot, NULL) != SQLITE_OK) {
        rs_set_error_sqlite(s, "prepare");
        return NULL;
    }
    return *slot;
}

/* ── Schema ─────────────────────────────────────────────────────────────── */

/*
 * WITHOUT ROWID on both tables: the id IS the storage key, and the engine's
 * insertion counter is not suppressed but absent — `SELECT rowid FROM records`
 * is an error, which tests/test_record_store.c asserts from a raw connection.
 * The kind is stored by NAME (the same stable string the id preimage uses),
 * never the ordinal. The column set is exactly id + the nine frozen record
 * fields: no sequence, no workspace, no store-invented timestamp.
 */
static const char *const RS_DDL =
    "CREATE TABLE IF NOT EXISTS record_store_meta ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ") WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS records ("
    "  id TEXT PRIMARY KEY,"
    "  kind TEXT NOT NULL,"
    "  author TEXT NOT NULL,"
    "  timestamp_ms INTEGER NOT NULL,"
    "  content TEXT NOT NULL,"
    "  anchor TEXT,"
    "  origin TEXT,"
    "  thread TEXT,"
    "  parent TEXT,"
    "  redactions INTEGER NOT NULL"
    ") WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS idx_records_anchor ON records(anchor);"
    "CREATE INDEX IF NOT EXISTS idx_records_thread ON records(thread);"
    "CREATE INDEX IF NOT EXISTS idx_records_timestamp ON records(timestamp_ms);";

/* A macro so every reader concatenates the SAME column list at compile time —
 * RS_COL_* above indexes into this order and nothing else. */
#define RS_SELECT                                                                      \
    "SELECT id, kind, author, timestamp_ms, content, anchor, origin, thread, parent, " \
    "redactions FROM records"

/* One row of one scalar query; *out_found says whether there was a row. */
static int rs_scalar_text(hyp_record_store_t *s, const char *sql, char *out, size_t cap,
                          bool *out_found) {
    *out_found = false;
    out[0] = '\0';
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, HYP_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        rs_set_error_sqlite(s, "prepare");
        return SQLITE_ERROR;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(stmt, 0);
        (void)snprintf(out, cap, "%s", value ? value : "");
        *out_found = true;
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    } else {
        rs_set_error_sqlite(s, "step");
    }
    (void)sqlite3_finalize(stmt);
    return rc;
}

/*
 * Open refuses a store speaking any other contract: a future record shape is
 * a disjoint id space, and half-reading it as v1 would be a plausible wrong
 * answer of exactly the kind this system exists to refuse. An EMPTY store
 * with no tag is adopted and stamped — that is what a fresh database is. A
 * POPULATED store with no tag is refused: rows of unknown provenance are not
 * data, and stamping them would be adoption by accident.
 */
static hyp_record_store_status_t rs_check_contract(hyp_record_store_t *s) {
    char tag[HYP_SZ_512];
    bool found = false;
    if (rs_scalar_text(s, "SELECT value FROM record_store_meta WHERE key = 'contract';", tag,
                       sizeof(tag), &found) != SQLITE_OK) {
        return HYP_RECORD_STORE_ERR_IO;
    }
    if (found) {
        if (strcmp(tag, HYP_RECORD_CONTRACT) == 0) {
            return HYP_RECORD_STORE_OK;
        }
        (void)snprintf(s->errbuf, sizeof(s->errbuf), "store speaks '%s', this build speaks '%s'",
                       tag, HYP_RECORD_CONTRACT);
        return HYP_RECORD_STORE_ERR_CONTRACT;
    }
    char rows[HYP_SZ_512];
    bool have_count = false;
    if (rs_scalar_text(s, "SELECT COUNT(*) FROM records;", rows, sizeof(rows), &have_count) !=
            SQLITE_OK ||
        !have_count) {
        return HYP_RECORD_STORE_ERR_IO;
    }
    if (strcmp(rows, "0") != 0) {
        rs_set_error(s, "store holds records but carries no contract tag");
        return HYP_RECORD_STORE_ERR_CONTRACT;
    }
    /* OR IGNORE: a concurrent open stamping the same fresh store writes the
     * identical value, and absorbing that duplicate is the house move. */
    if (rs_exec(s, "INSERT OR IGNORE INTO record_store_meta(key, value)"
                   " VALUES('contract', '" HYP_RECORD_CONTRACT "');") != SQLITE_OK) {
        return HYP_RECORD_STORE_ERR_IO;
    }
    return HYP_RECORD_STORE_OK;
}

hyp_record_store_status_t hyp_record_store_open(const char *dir, hyp_record_store_t **out) {
    if (!out) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    *out = NULL;
    if (!dir || !dir[0]) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    if (!hyp_mkdir_p(dir, RS_DIR_MODE)) {
        return HYP_RECORD_STORE_ERR_OPEN;
    }

    char path[HYP_SZ_1K];
    int written = snprintf(path, sizeof(path), "%s/records.db", dir);
    if (written <= 0 || written >= (int)sizeof(path)) {
        return HYP_RECORD_STORE_ERR_OPEN;
    }

    hyp_record_store_t *s = calloc(HYP_ALLOC_ONE, sizeof(*s));
    if (!s) {
        return HYP_RECORD_STORE_ERR_ALLOC;
    }
    if (sqlite3_open_v2(path, &s->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        if (s->db) {
            (void)sqlite3_close(s->db);
        }
        free(s);
        return HYP_RECORD_STORE_ERR_OPEN;
    }

    /* Durability over throughput: this store's writes are decisions someone
     * took the trouble to record, arriving a few at a time. FULL sync means a
     * commit that returned is a commit that survives — C6u's "kill between
     * write and sync loses nothing" is bought right here. */
    hyp_record_store_status_t st = HYP_RECORD_STORE_ERR_OPEN;
    if (rs_exec(s, "PRAGMA busy_timeout = 10000;") == SQLITE_OK &&
        rs_exec(s, "PRAGMA journal_mode = WAL;") == SQLITE_OK &&
        rs_exec(s, "PRAGMA synchronous = FULL;") == SQLITE_OK && rs_exec(s, RS_DDL) == SQLITE_OK) {
        st = rs_check_contract(s);
    }
    if (st != HYP_RECORD_STORE_OK) {
        (void)sqlite3_close(s->db);
        free(s);
        return st;
    }
    *out = s;
    return HYP_RECORD_STORE_OK;
}

void hyp_record_store_close(hyp_record_store_t *store) {
    if (!store) {
        return;
    }
    (void)sqlite3_finalize(store->stmt_insert);
    (void)sqlite3_finalize(store->stmt_get);
    (void)sqlite3_finalize(store->stmt_count);
    (void)sqlite3_close(store->db);
    free(store);
}

/* ── Rows back into records ─────────────────────────────────────────────── */

static const char *rs_column_opt(sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        return NULL; /* absent, not empty — the difference survives storage */
    }
    return (const char *)sqlite3_column_text(stmt, col);
}

/*
 * Rebuild the record a row claims to be, through hyp_record_build — the one
 * producer — and require the derived id to equal the stored key. This is the
 * reader's half of immutability: a row edited behind the store's back stops
 * being data and becomes a named refusal. A row the contract itself would not
 * build (an unknown kind, an empty optional, a wild redaction count) is the
 * same refusal: nothing edited by hand is ever returned as a record.
 */
static hyp_record_store_status_t rs_row_to_record(hyp_record_store_t *s, sqlite3_stmt *stmt,
                                                  const hyp_record_t **out) {
    *out = NULL;
    const char *id = (const char *)sqlite3_column_text(stmt, RS_COL_ID);
    const char *kind_name = (const char *)sqlite3_column_text(stmt, RS_COL_KIND);

    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    if (!id || !kind_name || !hyp_record_kind_from_name(kind_name, &in.kind)) {
        rs_set_error(s, "row carries an id or kind this contract does not have");
        return HYP_RECORD_STORE_ERR_TAMPERED;
    }
    in.author = (const char *)sqlite3_column_text(stmt, RS_COL_AUTHOR);
    in.timestamp_ms = sqlite3_column_int64(stmt, RS_COL_TIMESTAMP);
    in.content = (const char *)sqlite3_column_text(stmt, RS_COL_CONTENT);
    in.anchor = rs_column_opt(stmt, RS_COL_ANCHOR);
    in.origin = rs_column_opt(stmt, RS_COL_ORIGIN);
    in.thread = rs_column_opt(stmt, RS_COL_THREAD);
    in.parent = rs_column_opt(stmt, RS_COL_PARENT);
    int64_t redactions = sqlite3_column_int64(stmt, RS_COL_REDACTIONS);
    if (redactions < 0 || redactions > (int64_t)UINT32_MAX) {
        rs_set_error(s, "row carries a redaction count no record could have");
        return HYP_RECORD_STORE_ERR_TAMPERED;
    }
    in.redactions = (uint32_t)redactions;

    const hyp_record_t *rec = NULL;
    hyp_record_status_t rst = hyp_record_build(&in, &rec);
    if (rst == HYP_RECORD_ERR_ALLOC) {
        return HYP_RECORD_STORE_ERR_ALLOC;
    }
    if (rst != HYP_RECORD_OK) {
        (void)snprintf(s->errbuf, sizeof(s->errbuf), "row is not a record: %s",
                       hyp_record_status_reason(rst));
        return HYP_RECORD_STORE_ERR_TAMPERED;
    }
    if (strcmp(rec->id, id) != 0) {
        hyp_record_free(rec);
        rs_set_error(s, "row's id is not the digest of its fields");
        return HYP_RECORD_STORE_ERR_TAMPERED;
    }
    *out = rec;
    return HYP_RECORD_STORE_OK;
}

static hyp_record_store_status_t rs_get(hyp_record_store_t *s, const char *id,
                                        const hyp_record_t **out) {
    *out = NULL;
    sqlite3_stmt *stmt = rs_prepare_cached(s, &s->stmt_get, RS_SELECT " WHERE id = ?1;");
    if (!stmt) {
        return HYP_RECORD_STORE_ERR_IO;
    }
    if (rs_bind_text(stmt, 1, id) != SQLITE_OK) {
        rs_set_error_sqlite(s, "bind");
        return HYP_RECORD_STORE_ERR_IO;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        return HYP_RECORD_STORE_OK; /* absent: look elsewhere */
    }
    if (rc != SQLITE_ROW) {
        rs_set_error_sqlite(s, "get");
        return HYP_RECORD_STORE_ERR_IO;
    }
    return rs_row_to_record(s, stmt, out);
}

hyp_record_store_status_t hyp_record_store_get(hyp_record_store_t *store, const char *id,
                                               const hyp_record_t **out) {
    if (!out) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    *out = NULL;
    if (!store || !id) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    return rs_get(store, id, out);
}

/* ── The writer ─────────────────────────────────────────────────────────── */

static hyp_record_store_status_t rs_append_one(hyp_record_store_t *s, const hyp_record_t *rec,
                                               bool *added) {
    *added = false;
    if (!rec || !rec->id) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    /* The writer's half of immutability: nothing whose id is not the digest
     * of its own fields gets in, so nothing on disk can need "fixing". */
    if (!hyp_record_verify(rec)) {
        rs_set_error(s, "record refused: id does not match fields");
        return HYP_RECORD_STORE_ERR_TAMPERED;
    }

    sqlite3_stmt *stmt = rs_prepare_cached(
        s, &s->stmt_insert,
        "INSERT OR IGNORE INTO records"
        " (id, kind, author, timestamp_ms, content, anchor, origin, thread, parent, redactions)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);");
    if (!stmt) {
        return HYP_RECORD_STORE_ERR_IO;
    }
    if (rs_bind_text(stmt, 1, rec->id) != SQLITE_OK ||
        rs_bind_text(stmt, 2, hyp_record_kind_name(rec->kind)) != SQLITE_OK ||
        rs_bind_text(stmt, 3, rec->author) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, rec->timestamp_ms) != SQLITE_OK ||
        rs_bind_text(stmt, 5, rec->content) != SQLITE_OK ||
        rs_bind_text_opt(stmt, 6, rec->anchor) != SQLITE_OK ||
        rs_bind_text_opt(stmt, 7, rec->origin) != SQLITE_OK ||
        rs_bind_text_opt(stmt, 8, rec->thread) != SQLITE_OK ||
        rs_bind_text_opt(stmt, 9, rec->parent) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 10, (int64_t)rec->redactions) != SQLITE_OK) {
        rs_set_error_sqlite(s, "bind");
        return HYP_RECORD_STORE_ERR_IO;
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        rs_set_error_sqlite(s, "append");
        return HYP_RECORD_STORE_ERR_IO;
    }
    if (sqlite3_changes(s->db) == 1) {
        *added = true;
        return HYP_RECORD_STORE_OK;
    }

    /* The id is already on disk. Either the same record — the union absorbing
     * a duplicate (I8), the normal case — or a different one under the same
     * id, refused. As in record.h, the second branch is unreachable short of
     * a SHA-256 collision: both sides have been verified, so both ids are
     * digests of their own fields. It stays because silently keeping
     * whichever arrived first is the one failure a union must not have. */
    const hyp_record_t *stored = NULL;
    hyp_record_store_status_t st = rs_get(s, rec->id, &stored);
    if (st != HYP_RECORD_STORE_OK) {
        return st;
    }
    if (!stored) {
        rs_set_error(s, "row vanished between insert and read-back");
        return HYP_RECORD_STORE_ERR_IO;
    }
    bool identical = hyp_record_equal(stored, rec);
    hyp_record_free(stored);
    if (!identical) {
        rs_set_error(s, "id already present with different fields");
        return HYP_RECORD_STORE_ERR_COLLISION;
    }
    return HYP_RECORD_STORE_OK;
}

hyp_record_store_status_t hyp_record_store_append(hyp_record_store_t *store,
                                                  const hyp_record_t *rec, bool *out_added) {
    if (out_added) {
        *out_added = false;
    }
    if (!store || !rec) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    bool added = false;
    hyp_record_store_status_t st = rs_append_one(store, rec, &added);
    if (st == HYP_RECORD_STORE_OK && out_added) {
        *out_added = added;
    }
    return st;
}

hyp_record_store_status_t hyp_record_store_append_set(hyp_record_store_t *store,
                                                      const hyp_record_set_t *set,
                                                      size_t *out_added) {
    if (out_added) {
        *out_added = 0;
    }
    if (!store || !set) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    /* One transaction, so a refusal anywhere leaves the store exactly as it
     * was — a half-done merge would make the store's contents a function of
     * where an error happened rather than of what was written to it. */
    if (rs_exec(store, "BEGIN IMMEDIATE;") != SQLITE_OK) {
        return HYP_RECORD_STORE_ERR_IO;
    }
    size_t added_count = 0;
    hyp_record_store_status_t st = HYP_RECORD_STORE_OK;
    size_t count = hyp_record_set_count(set);
    for (size_t i = 0; i < count; i++) {
        bool added = false;
        st = rs_append_one(store, hyp_record_set_at(set, i), &added);
        if (st != HYP_RECORD_STORE_OK) {
            break;
        }
        if (added) {
            added_count++;
        }
    }
    if (st != HYP_RECORD_STORE_OK) {
        (void)rs_exec(store, "ROLLBACK;");
        return st;
    }
    if (rs_exec(store, "COMMIT;") != SQLITE_OK) {
        (void)rs_exec(store, "ROLLBACK;");
        return HYP_RECORD_STORE_ERR_IO;
    }
    if (out_added) {
        *out_added = added_count;
    }
    return HYP_RECORD_STORE_OK;
}

/* ── The reader ─────────────────────────────────────────────────────────── */

hyp_record_store_status_t hyp_record_store_count(hyp_record_store_t *store, size_t *out_count) {
    if (!out_count) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    *out_count = 0;
    if (!store) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    sqlite3_stmt *stmt =
        rs_prepare_cached(store, &store->stmt_count, "SELECT COUNT(*) FROM records;");
    if (!stmt) {
        return HYP_RECORD_STORE_ERR_IO;
    }
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        rs_set_error_sqlite(store, "count");
        return HYP_RECORD_STORE_ERR_IO;
    }
    *out_count = (size_t)sqlite3_column_int64(stmt, 0);
    return HYP_RECORD_STORE_OK;
}

/* A query whose shape contradicts itself is refused before it touches the
 * engine. Value-level emptiness (a since after an until) is NOT refused: an
 * empty range is a real question whose true answer is the empty set. */
static hyp_record_store_status_t rs_validate_query(const hyp_record_store_query_t *q) {
    if (q->has_kind && !hyp_record_kind_name(q->kind)) {
        return HYP_RECORD_STORE_ERR_QUERY;
    }
    const char *strings[] = {q->author, q->anchor, q->anchor_prefix,
                             q->origin, q->thread, q->parent};
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        if (strings[i] && !strings[i][0]) {
            return HYP_RECORD_STORE_ERR_QUERY; /* "" is never stored; absent is NULL */
        }
    }
    if (q->anchored_only && q->unanchored_only) {
        return HYP_RECORD_STORE_ERR_QUERY;
    }
    if (q->unanchored_only && (q->anchor || q->anchor_prefix)) {
        return HYP_RECORD_STORE_ERR_QUERY;
    }
    if (q->parent && !hyp_record_id_is_valid(q->parent)) {
        return HYP_RECORD_STORE_ERR_QUERY; /* parent is OURS — a foreign id is a mistake */
    }
    return HYP_RECORD_STORE_OK;
}

static void rs_sql_cat(char *dst, size_t cap, const char *piece) {
    size_t len = strlen(dst);
    (void)snprintf(dst + len, cap - len, "%s", piece);
}

/*
 * Assemble and run a SELECT. q == NULL loads everything. Clauses that the
 * engine can answer with byte semantics (equality under BINARY collation,
 * integer comparison, NULL-ness) go in SQL; the anchor PREFIX is applied here
 * in C as memcmp, because every engine-side spelling of "prefix" (LIKE, GLOB,
 * substr) interprets bytes it has no business interpreting.
 */
static hyp_record_store_status_t rs_select(hyp_record_store_t *s, const hyp_record_store_query_t *q,
                                           hyp_record_set_t **out) {
    static const hyp_record_store_query_t everything = {0};
    const hyp_record_store_query_t *qq = q ? q : &everything;

    /* ONE table drives both the SQL text and the binds, in one order — a
     * clause list and a bind list that merely promise to agree are two ends
     * waiting to disagree. */
    struct {
        bool active;
        const char *clause;
        const char *bind_text; /* text bind, or */
        bool bind_int;         /* integer bind, or neither */
        int64_t bind_value;
    } clauses[] = {
        {qq->has_kind, "kind = ?", qq->has_kind ? hyp_record_kind_name(qq->kind) : NULL, false, 0},
        {qq->author != NULL, "author = ?", qq->author, false, 0},
        {qq->anchor != NULL, "anchor = ?", qq->anchor, false, 0},
        {qq->anchored_only || qq->anchor_prefix != NULL, "anchor IS NOT NULL", NULL, false, 0},
        {qq->unanchored_only, "anchor IS NULL", NULL, false, 0},
        {qq->origin != NULL, "origin = ?", qq->origin, false, 0},
        {qq->thread != NULL, "thread = ?", qq->thread, false, 0},
        {qq->parent != NULL, "parent = ?", qq->parent, false, 0},
        {qq->has_since, "timestamp_ms >= ?", NULL, true, qq->since_ms},
        {qq->has_until, "timestamp_ms <= ?", NULL, true, qq->until_ms},
    };
    size_t n_clauses = sizeof(clauses) / sizeof(clauses[0]);

    char sql[RS_SQL_MAX];
    (void)snprintf(sql, sizeof(sql), "%s", RS_SELECT);
    const char *sep = " WHERE ";
    for (size_t i = 0; i < n_clauses; i++) {
        if (!clauses[i].active) {
            continue;
        }
        rs_sql_cat(sql, sizeof(sql), sep);
        rs_sql_cat(sql, sizeof(sql), clauses[i].clause);
        sep = " AND ";
    }
    /* The one order there is. Never the engine's, never arrival's. */
    rs_sql_cat(sql, sizeof(sql), " ORDER BY id;");

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, sql, HYP_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        rs_set_error_sqlite(s, "prepare query");
        return HYP_RECORD_STORE_ERR_IO;
    }
    int bind_ok = SQLITE_OK;
    int col = 1;
    for (size_t i = 0; i < n_clauses && bind_ok == SQLITE_OK; i++) {
        if (!clauses[i].active) {
            continue;
        }
        if (clauses[i].bind_text) {
            bind_ok = rs_bind_text(stmt, col, clauses[i].bind_text);
            col++;
        } else if (clauses[i].bind_int) {
            bind_ok = sqlite3_bind_int64(stmt, col, clauses[i].bind_value);
            col++;
        }
    }
    if (bind_ok != SQLITE_OK) {
        rs_set_error_sqlite(s, "bind query");
        (void)sqlite3_finalize(stmt);
        return HYP_RECORD_STORE_ERR_IO;
    }

    hyp_record_set_t *set = hyp_record_set_create();
    if (!set) {
        (void)sqlite3_finalize(stmt);
        return HYP_RECORD_STORE_ERR_ALLOC;
    }
    size_t prefix_len = qq->anchor_prefix ? strlen(qq->anchor_prefix) : 0;
    hyp_record_store_status_t st = HYP_RECORD_STORE_OK;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const hyp_record_t *rec = NULL;
        st = rs_row_to_record(s, stmt, &rec);
        if (st != HYP_RECORD_STORE_OK) {
            break;
        }
        if (prefix_len > 0 &&
            (!rec->anchor || strncmp(rec->anchor, qq->anchor_prefix, prefix_len) != 0)) {
            hyp_record_free(rec);
            continue; /* byte prefix, decided here, not by the engine */
        }
        hyp_record_status_t rst = hyp_record_set_add(set, rec, NULL);
        hyp_record_free(rec);
        if (rst != HYP_RECORD_OK) {
            st = (rst == HYP_RECORD_ERR_ALLOC) ? HYP_RECORD_STORE_ERR_ALLOC
                                               : HYP_RECORD_STORE_ERR_TAMPERED;
            break;
        }
    }
    if (st == HYP_RECORD_STORE_OK && rc != SQLITE_DONE) {
        rs_set_error_sqlite(s, "step query");
        st = HYP_RECORD_STORE_ERR_IO;
    }
    (void)sqlite3_finalize(stmt);
    if (st != HYP_RECORD_STORE_OK) {
        hyp_record_set_free(set);
        return st;
    }
    *out = set;
    return HYP_RECORD_STORE_OK;
}

hyp_record_store_status_t hyp_record_store_load(hyp_record_store_t *store, hyp_record_set_t **out) {
    if (!out) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    *out = NULL;
    if (!store) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    return rs_select(store, NULL, out);
}

hyp_record_store_status_t hyp_record_store_query(hyp_record_store_t *store,
                                                 const hyp_record_store_query_t *query,
                                                 hyp_record_set_t **out) {
    if (!out) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    *out = NULL;
    if (!store || !query) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    hyp_record_store_status_t st = rs_validate_query(query);
    if (st != HYP_RECORD_STORE_OK) {
        rs_set_error(store, hyp_record_store_status_reason(HYP_RECORD_STORE_ERR_QUERY));
        return st;
    }
    return rs_select(store, query, out);
}

hyp_record_store_status_t hyp_record_store_digest(hyp_record_store_t *store,
                                                  char out[HYP_RECORD_ID_LEN + 1]) {
    if (!store || !out) {
        return HYP_RECORD_STORE_ERR_NULL;
    }
    /* Through load, deliberately: the digest's encoding then has exactly one
     * implementation (record.c's), and every row is verified on the way — a
     * digest over unverified ids would vouch for bytes nobody checked. */
    hyp_record_set_t *set = NULL;
    hyp_record_store_status_t st = rs_select(store, NULL, &set);
    if (st != HYP_RECORD_STORE_OK) {
        return st;
    }
    hyp_record_set_digest(set, out);
    hyp_record_set_free(set);
    return HYP_RECORD_STORE_OK;
}
