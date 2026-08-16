/*
 * test_record_store.c — the durable record store (C1u writer, C2u reader).
 *
 * The organising claim is the assertion row this unit carries: the store is
 * NOT A LOG. The same records written to two stores in two orders must yield
 * equal digests AND identical enumeration order (canonical, by id), and
 * write→reopen→enumerate must be that same order again. Around it: the writer
 * has no update path and the schema nowhere to hide a sequence — asserted
 * from a RAW engine connection, the client no API can lie to — and the reader
 * verifies every row on the way out, so a store edited behind the API's back
 * becomes a named refusal instead of data.
 *
 * Queries follow the record contract's grammar: absent means unconstrained,
 * a coherent question with no matches gets an EMPTY set (empty means "there
 * is nothing"), and a query that contradicts itself is refused. Opaque fields
 * are matched as bytes — a '%' in an anchor is a byte, never a wildcard, and
 * there is a fixture whose whole job is to catch an engine wildcard leaking.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <store/record_store.h>

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 2026-02-14T12:26:40.000Z — fixed and caller-supplied; no test here reads a
 * clock to build a record, and neither does the store (I6). */
#define FIXED_MS INT64_C(1770985600000)

/* ── Fixtures ───────────────────────────────────────────────────────────── */

static hyp_record_input_t base_input(void) {
    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_DECISION;
    in.author = "agent:c1u";
    in.timestamp_ms = FIXED_MS;
    in.content = "the store is an id-keyed set, so order is not a fact about it";
    in.anchor = NULL;
    in.origin = NULL;
    in.thread = NULL;
    in.parent = NULL;
    in.redactions = 0;
    return in;
}

static const hyp_record_t *rec_of(const char *content, const char *anchor) {
    hyp_record_input_t in = base_input();
    in.content = content;
    in.anchor = anchor;
    const hyp_record_t *rec = NULL;
    if (hyp_record_build(&in, &rec) != HYP_RECORD_OK) {
        return NULL;
    }
    return rec;
}

static void db_path_of(char *out, size_t cap, const char *dir) {
    (void)snprintf(out, cap, "%s/records.db", dir);
}

/* Run one statement over a RAW connection — the store API has no way to say
 * any of the things these tests need said (UPDATE, DELETE), which is the
 * point of using it. */
static int raw_exec(const char *dir, const char *sql) {
    char path[512];
    db_path_of(path, sizeof(path), dir);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        (void)sqlite3_close(db);
        return -1;
    }
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    sqlite3_free(err);
    (void)sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

/*
 * Enumerate ids straight off the disk: a RAW connection, NO order clause, and
 * a projection the id index does not cover, so the engine hands back the
 * table's OWN order rather than one this test asked for.
 *
 * This clause exists because hyp_record_set_t cannot supply it. A set keeps a
 * sorted array, so it RE-CANONICALISES whatever it is handed — a store that
 * had quietly become a log would still satisfy every assertion made through
 * the set. The store's own order is only visible here. On an id-keyed set
 * (WITHOUT ROWID, id as the storage key) it is ascending id and there is no
 * other order for it to be; on a log it would be arrival, and arrival differs
 * between two machines that received the same records.
 */
static int raw_scan_ids(const char *dir, char ids[][HYP_RECORD_ID_LEN + 1], size_t cap,
                        size_t *out_n) {
    *out_n = 0;
    char path[512];
    db_path_of(path, sizeof(path), dir);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        (void)sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, content FROM records;", -1, &stmt, NULL) != SQLITE_OK) {
        (void)sqlite3_finalize(stmt);
        (void)sqlite3_close(db);
        return -1;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW && *out_n < cap) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        (void)snprintf(ids[*out_n], HYP_RECORD_ID_LEN + 1, "%s", id ? id : "");
        (*out_n)++;
    }
    (void)sqlite3_finalize(stmt);
    (void)sqlite3_close(db);
    return 0;
}

/* True when the result set holds exactly the expected records, enumerated in
 * strictly ascending id order — the canonical order and no other. */
static int set_is_exactly(const hyp_record_set_t *got, const hyp_record_t *const *expected,
                          size_t n) {
    if (hyp_record_set_count(got) != n) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        const hyp_record_t *found = hyp_record_set_get(got, expected[i]->id);
        if (!found || !hyp_record_equal(found, expected[i])) {
            return 0;
        }
    }
    for (size_t i = 1; i < n; i++) {
        if (strcmp(hyp_record_set_at(got, i - 1)->id, hyp_record_set_at(got, i)->id) >= 0) {
            return 0;
        }
    }
    return 1;
}

/*
 * The query fixture. Seven records chosen so every axis has a YES and a NO,
 * and so an engine wildcard leaking into "opaque bytes" is caught: m1's
 * anchor is made of LIKE/GLOB metacharacters, and m2's plain "abc" is the
 * record a leaked '%' would wrongly match.
 */
enum { FX_D1 = 0, FX_D2, FX_S1, FX_V1, FX_M1, FX_M2, FX_U1, FX_COUNT };

static int fixture_populate(hyp_record_store_t *s, const hyp_record_t *recs[FX_COUNT]) {
    hyp_record_input_t in[FX_COUNT];
    for (int i = 0; i < FX_COUNT; i++) {
        in[i] = base_input();
        in[i].timestamp_ms = FIXED_MS + (int64_t)(i + 1) * 1000;
        recs[i] = NULL;
    }
    in[FX_D1].content = "decide alpha foo";
    in[FX_D1].anchor = "hyp1:ws/alpha#foo";
    in[FX_D1].origin = "adapterx:row-1";
    in[FX_D2].content = "decide alpha foobar";
    in[FX_D2].anchor = "hyp1:ws/alpha#foobar";
    in[FX_S1].kind = HYP_RECORD_SIGNAL;
    in[FX_S1].content = "signal beta";
    in[FX_S1].anchor = "hyp1:ws/beta#foo";
    in[FX_S1].thread = "conv:9";
    in[FX_V1].kind = HYP_RECORD_VERDICT;
    in[FX_V1].author = "agent:two";
    in[FX_V1].content = "verdict on the alpha decision";
    in[FX_M1].kind = HYP_RECORD_MESSAGE;
    in[FX_M1].content = "anchor made of metacharacters";
    in[FX_M1].anchor = "a%_*?[b]c";
    in[FX_M2].kind = HYP_RECORD_MESSAGE;
    in[FX_M2].content = "anchor that a leaked wildcard would match";
    in[FX_M2].anchor = "abc";
    in[FX_U1].kind = HYP_RECORD_SUMMARY;
    in[FX_U1].content = "summary attached to nothing";
    in[FX_U1].origin = "adapterx:row-2";
    in[FX_U1].thread = "conv:9";

    /* The verdict points at the decision, so parent queries have a target. */
    if (hyp_record_build(&in[FX_D1], &recs[FX_D1]) != HYP_RECORD_OK) {
        return 0;
    }
    in[FX_V1].parent = recs[FX_D1]->id;

    for (int i = 0; i < FX_COUNT; i++) {
        if (i != FX_D1 && hyp_record_build(&in[i], &recs[i]) != HYP_RECORD_OK) {
            return 0;
        }
        bool added = false;
        if (hyp_record_store_append(s, recs[i], &added) != HYP_RECORD_STORE_OK || !added) {
            return 0;
        }
    }
    return 1;
}

static void fixture_free(const hyp_record_t *recs[FX_COUNT]) {
    for (int i = 0; i < FX_COUNT; i++) {
        hyp_record_free(recs[i]);
    }
}

/* ── Open, close, empty ─────────────────────────────────────────────────── */

TEST(record_store_opens_creates_and_reopens_empty) {
    const char *tmp = th_mktempdir("hyp_recstore_open");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s/nested/store", tmp); /* creation is mkdir -p */
    char root[512];
    (void)snprintf(root, sizeof(root), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(NULL, &s), HYP_RECORD_STORE_ERR_NULL);
    ASSERT_EQ(hyp_record_store_open("", &s), HYP_RECORD_STORE_ERR_NULL);
    ASSERT_EQ(hyp_record_store_open(dir, NULL), HYP_RECORD_STORE_ERR_NULL);

    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(s);

    size_t count = 99;
    ASSERT_EQ(hyp_record_store_count(s, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 0);

    /* An empty store and an empty set are the same emptiness. */
    char store_digest[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(s, store_digest), HYP_RECORD_STORE_OK);
    hyp_record_set_t *empty = hyp_record_set_create();
    ASSERT_NOT_NULL(empty);
    char set_digest[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(empty, set_digest);
    ASSERT_STR_EQ(store_digest, set_digest);
    hyp_record_set_free(empty);

    /* Absent means look elsewhere — and it is OK, not an error. */
    const hyp_record_t *missing = NULL;
    ASSERT_EQ(hyp_record_store_get(
                  s, "0000000000000000000000000000000000000000000000000000000000000000", &missing),
              HYP_RECORD_STORE_OK);
    ASSERT_NULL(missing);

    hyp_record_store_close(s);
    s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_count(s, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 0);
    hyp_record_store_close(s);
    hyp_record_store_close(NULL); /* no-op, like every free here */

    (void)th_rmtree(root);
    PASS();
}

/* ── Round trip: what was appended is what comes back ───────────────────── */

TEST(record_store_round_trips_across_reopen) {
    const char *tmp = th_mktempdir("hyp_recstore_rt");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    const hyp_record_t *recs[FX_COUNT];
    ASSERT_TRUE(fixture_populate(s, recs));
    size_t count = 0;
    ASSERT_EQ(hyp_record_store_count(s, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, FX_COUNT);
    hyp_record_store_close(s);

    /* A different handle — the view a second process gets. */
    s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    hyp_record_set_t *loaded = NULL;
    ASSERT_EQ(hyp_record_store_load(s, &loaded), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(set_is_exactly(loaded, recs, FX_COUNT));
    for (size_t i = 0; i < hyp_record_set_count(loaded); i++) {
        ASSERT_TRUE(hyp_record_verify(hyp_record_set_at(loaded, i)));
    }
    hyp_record_set_free(loaded);

    /* get returns the record field-for-field, opaque bytes untouched. */
    const hyp_record_t *got = NULL;
    ASSERT_EQ(hyp_record_store_get(s, recs[FX_M1]->id, &got), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(got);
    ASSERT_TRUE(hyp_record_equal(got, recs[FX_M1]));
    ASSERT_STR_EQ(got->anchor, "a%_*?[b]c");
    hyp_record_free(got);

    hyp_record_store_close(s);
    fixture_free(recs);
    (void)th_rmtree(dir);
    PASS();
}

/* ── NOT A LOG: the unit's assertion row ────────────────────────────────── */

TEST(two_write_orders_one_enumeration) {
    /*
     * Same records, two stores, two arrival orders → equal digests AND
     * identical enumeration, canonical by id. Then write, reopen, enumerate →
     * the same order again. If insertion order can be recovered from the read
     * path, this store is a log wearing a costume, and the union C6u builds
     * on it inherits a machine-local fact it can never reconcile.
     */
    const char *tmp = th_mktempdir("hyp_recstore_orders");
    ASSERT_NOT_NULL(tmp);
    char root[512];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char dir_a[512];
    char dir_b[512];
    (void)snprintf(dir_a, sizeof(dir_a), "%s/a", root);
    (void)snprintf(dir_b, sizeof(dir_b), "%s/b", root);

    enum { N = 6 };
    const hyp_record_t *recs[N];
    static const char *const contents[N] = {"r-one",  "r-two",  "r-three",
                                            "r-four", "r-five", "r-six"};
    for (size_t i = 0; i < N; i++) {
        recs[i] = rec_of(contents[i], "hyp1:ws/repo#same-symbol");
        ASSERT_NOT_NULL(recs[i]);
    }

    hyp_record_store_t *a = NULL;
    hyp_record_store_t *b = NULL;
    ASSERT_EQ(hyp_record_store_open(dir_a, &a), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_open(dir_b, &b), HYP_RECORD_STORE_OK);
    for (size_t i = 0; i < N; i++) {
        ASSERT_EQ(hyp_record_store_append(a, recs[i], NULL), HYP_RECORD_STORE_OK);
        ASSERT_EQ(hyp_record_store_append(b, recs[N - 1 - i], NULL), HYP_RECORD_STORE_OK);
    }

    /* Control first: the digest can say NO — a partial store differs. */
    char digest_a[HYP_RECORD_ID_LEN + 1];
    char digest_b[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_t *empty = hyp_record_set_create();
    ASSERT_NOT_NULL(empty);
    hyp_record_set_digest(empty, digest_b);
    hyp_record_set_free(empty);
    ASSERT_EQ(hyp_record_store_digest(a, digest_a), HYP_RECORD_STORE_OK);
    ASSERT_STR_NEQ(digest_a, digest_b);

    ASSERT_EQ(hyp_record_store_digest(b, digest_b), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_a, digest_b); /* order of arrival is not a fact about the store */

    hyp_record_set_t *from_a = NULL;
    hyp_record_set_t *from_b = NULL;
    ASSERT_EQ(hyp_record_store_load(a, &from_a), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_load(b, &from_b), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_set_count(from_a), N);
    ASSERT_EQ(hyp_record_set_count(from_b), N);

    char order[N][HYP_RECORD_ID_LEN + 1];
    for (size_t i = 0; i < N; i++) {
        const hyp_record_t *x = hyp_record_set_at(from_a, i);
        const hyp_record_t *y = hyp_record_set_at(from_b, i);
        ASSERT_NOT_NULL(x);
        ASSERT_NOT_NULL(y);
        ASSERT_STR_EQ(x->id, y->id); /* identical enumeration */
        if (i > 0) {
            /* strictly ascending: total, machine-independent, insertion-blind */
            ASSERT_LT(strcmp(hyp_record_set_at(from_a, i - 1)->id, x->id), 0);
        }
        (void)snprintf(order[i], sizeof(order[i]), "%s", x->id);
    }
    hyp_record_set_free(from_a);
    hyp_record_set_free(from_b);
    hyp_record_store_close(b);
    hyp_record_store_close(a);

    /*
     * The same claim one level down, where the set cannot answer for the
     * store: the ROWS ON DISK, unordered, in both stores, are the same
     * sequence — and it is the canonical one. Arrival order is not merely
     * unused by the reader, it was never written down.
     */
    char scan_a[N][HYP_RECORD_ID_LEN + 1];
    char scan_b[N][HYP_RECORD_ID_LEN + 1];
    size_t n_a = 0;
    size_t n_b = 0;
    ASSERT_EQ(raw_scan_ids(dir_a, scan_a, N, &n_a), 0);
    ASSERT_EQ(raw_scan_ids(dir_b, scan_b, N, &n_b), 0);
    ASSERT_EQ(n_a, N);
    ASSERT_EQ(n_b, N);
    for (size_t i = 0; i < N; i++) {
        ASSERT_STR_EQ(scan_a[i], order[i]);
        ASSERT_STR_EQ(scan_b[i], order[i]);
    }

    /* Write, reopen, enumerate → same order. Not "similar": the same. */
    a = NULL;
    ASSERT_EQ(hyp_record_store_open(dir_a, &a), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_load(a, &from_a), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_set_count(from_a), N);
    for (size_t i = 0; i < N; i++) {
        ASSERT_STR_EQ(hyp_record_set_at(from_a, i)->id, order[i]);
    }
    hyp_record_set_free(from_a);
    hyp_record_store_close(a);

    for (size_t i = 0; i < N; i++) {
        hyp_record_free(recs[i]);
    }
    (void)th_rmtree(root);
    PASS();
}

/* ── I8: the same record twice is one record ────────────────────────────── */

TEST(same_record_twice_is_absorbed) {
    const char *tmp = th_mktempdir("hyp_recstore_dup");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);

    hyp_record_input_t in = base_input();
    const hyp_record_t *first = NULL;
    const hyp_record_t *again = NULL;
    ASSERT_EQ(hyp_record_build(&in, &first), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_build(&in, &again), HYP_RECORD_OK); /* fresh allocation, same record */

    bool added = false;
    ASSERT_EQ(hyp_record_store_append(s, first, &added), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(added);
    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(s, before), HYP_RECORD_STORE_OK);

    ASSERT_EQ(hyp_record_store_append(s, first, &added), HYP_RECORD_STORE_OK);
    ASSERT_FALSE(added); /* absorbed, not an error, not a second row */
    ASSERT_EQ(hyp_record_store_append(s, again, &added), HYP_RECORD_STORE_OK);
    ASSERT_FALSE(added);

    size_t count = 0;
    ASSERT_EQ(hyp_record_store_count(s, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 1);
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(s, after), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(after, before);

    /* Control: one differing field is a different record and a second row. */
    in.redactions = 1;
    const hyp_record_t *different = NULL;
    ASSERT_EQ(hyp_record_build(&in, &different), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_store_append(s, different, &added), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(added);
    ASSERT_EQ(hyp_record_store_count(s, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 2);

    hyp_record_free(first);
    hyp_record_free(again);
    hyp_record_free(different);
    hyp_record_store_close(s);
    (void)th_rmtree(dir);
    PASS();
}

/* ── The schema has nowhere to hide a sequence ──────────────────────────── */

TEST(schema_exposes_no_sequence_and_no_extra_column) {
    /*
     * Asserted from a RAW connection, because this is a property of the bytes
     * on disk, not of what the API chooses to show. Three teeth:
     *
     *   1. `SELECT rowid` FAILS: the table is WITHOUT ROWID, so the engine's
     *      insertion counter does not exist to be leaked, ordered by, or
     *      synced. Not unused — absent.
     *   2. The column set is EXACTLY id + the nine frozen record fields. Any
     *      surplus column — seq, offset, position, workspace, a store-invented
     *      timestamp — fails the count before anyone argues about its name.
     *   3. No AUTOINCREMENT anywhere in the DDL, and the kind column stores
     *      the stable NAME, so renumbering the enum cannot rewrite a store.
     */
    const char *tmp = th_mktempdir("hyp_recstore_schema");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    const hyp_record_t *rec = rec_of("a decision to inspect on disk", NULL);
    ASSERT_NOT_NULL(rec);
    ASSERT_EQ(hyp_record_store_append(s, rec, NULL), HYP_RECORD_STORE_OK);
    hyp_record_store_close(s);

    char path[512];
    db_path_of(path, sizeof(path), dir);
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open(path, &db), SQLITE_OK);

    /* 1. There is no rowid to select. */
    sqlite3_stmt *stmt = NULL;
    ASSERT_NEQ(sqlite3_prepare_v2(db, "SELECT rowid FROM records;", -1, &stmt, NULL), SQLITE_OK);
    (void)sqlite3_finalize(stmt);
    stmt = NULL;

    /* 2. Exactly the contract's columns, nothing else. */
    static const char *const expected_cols[] = {"id",      "kind",      "author", "timestamp_ms",
                                                "content", "anchor",    "origin", "thread",
                                                "parent",  "redactions"};
    const size_t n_expected = sizeof(expected_cols) / sizeof(expected_cols[0]);
    bool seen[sizeof(expected_cols) / sizeof(expected_cols[0])] = {false};
    size_t n_cols = 0;
    ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA table_info(records);", -1, &stmt, NULL), SQLITE_OK);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        ASSERT_NOT_NULL(name);
        n_cols++;
        bool known = false;
        for (size_t i = 0; i < n_expected; i++) {
            if (strcmp(name, expected_cols[i]) == 0) {
                seen[i] = true;
                known = true;
            }
        }
        ASSERT_TRUE(known); /* a column the contract does not have */
    }
    (void)sqlite3_finalize(stmt);
    stmt = NULL;
    ASSERT_EQ(n_cols, n_expected);
    for (size_t i = 0; i < n_expected; i++) {
        ASSERT_TRUE(seen[i]);
    }

    /* 3. WITHOUT ROWID in the DDL, AUTOINCREMENT nowhere; kind is the name. */
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE name = 'records';", -1,
                                 &stmt, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *ddl = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(ddl);
    ASSERT_NOT_NULL(strstr(ddl, "WITHOUT ROWID"));
    ASSERT_NULL(strstr(ddl, "AUTOINCREMENT"));
    (void)sqlite3_finalize(stmt);
    stmt = NULL;

    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT kind FROM records;", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "decision");
    (void)sqlite3_finalize(stmt);
    (void)sqlite3_close(db);

    hyp_record_free(rec);
    (void)th_rmtree(dir);
    PASS();
}

/* ── The writer refuses what the contract refuses ───────────────────────── */

TEST(append_refuses_a_tampered_record_and_writes_nothing) {
    const char *tmp = th_mktempdir("hyp_recstore_tamper");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    const hyp_record_t *rec = rec_of("a record about to be altered", "hyp1:ws/repo#foo");
    ASSERT_NOT_NULL(rec);

    /* Cast the const away the way an attacker would, flip one byte. */
    char *content = NULL;
    memcpy(&content, &rec->content, sizeof(content));
    char original = content[0];
    content[0] = (char)(original ^ 0x20);

    bool added = true;
    ASSERT_EQ(hyp_record_store_append(s, rec, &added), HYP_RECORD_STORE_ERR_TAMPERED);
    ASSERT_FALSE(added);
    size_t count = 99;
    ASSERT_EQ(hyp_record_store_count(s, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 0); /* nothing got in */

    /* Control: restore the byte and the very same record is accepted. */
    content[0] = original;
    ASSERT_EQ(hyp_record_store_append(s, rec, &added), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(added);

    hyp_record_free(rec);
    hyp_record_store_close(s);
    (void)th_rmtree(dir);
    PASS();
}

TEST(reader_refuses_a_store_edited_behind_its_back) {
    /*
     * There is no update path through the API — record_store.h simply has no
     * function that could express one — so the only way to mutate a row is a
     * raw engine connection. Do exactly that, and require the reader to turn
     * the edited row into a named refusal rather than data. The refusal is
     * scoped to what was read: the untouched record is still served.
     */
    const char *tmp = th_mktempdir("hyp_recstore_edit");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    const hyp_record_t *good = rec_of("the record nobody touches", NULL);
    const hyp_record_t *bad = rec_of("the record someone edits", NULL);
    ASSERT_NOT_NULL(good);
    ASSERT_NOT_NULL(bad);
    ASSERT_EQ(hyp_record_store_append(s, good, NULL), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(s, bad, NULL), HYP_RECORD_STORE_OK);
    hyp_record_store_close(s);

    char sql[256];
    (void)snprintf(sql, sizeof(sql),
                   "UPDATE records SET content = 'rewritten by hand' WHERE id = '%s';", bad->id);
    ASSERT_EQ(raw_exec(dir, sql), 0);

    s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);

    hyp_record_set_t *loaded = NULL;
    ASSERT_EQ(hyp_record_store_load(s, &loaded), HYP_RECORD_STORE_ERR_TAMPERED);
    ASSERT_NULL(loaded);
    char digest[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(s, digest), HYP_RECORD_STORE_ERR_TAMPERED);

    const hyp_record_t *got = NULL;
    ASSERT_EQ(hyp_record_store_get(s, bad->id, &got), HYP_RECORD_STORE_ERR_TAMPERED);
    ASSERT_NULL(got);
    ASSERT_TRUE(hyp_record_store_error(s)[0] != '\0'); /* the refusal is named */

    ASSERT_EQ(hyp_record_store_get(s, good->id, &got), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(got);
    ASSERT_TRUE(hyp_record_equal(got, good));
    hyp_record_free(got);

    /* A count is a count of rows, never a verification — still 2. */
    size_t count = 0;
    ASSERT_EQ(hyp_record_store_count(s, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 2);

    hyp_record_free(good);
    hyp_record_free(bad);
    hyp_record_store_close(s);
    (void)th_rmtree(dir);
    PASS();
}

/* ── The reader: every axis says yes AND no ─────────────────────────────── */

TEST(query_answers_each_axis_and_says_no) {
    const char *tmp = th_mktempdir("hyp_recstore_query");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    const hyp_record_t *fx[FX_COUNT];
    ASSERT_TRUE(fixture_populate(s, fx));

    hyp_record_store_query_t q;
    hyp_record_set_t *got = NULL;

    /* kind */
    memset(&q, 0, sizeof(q));
    q.has_kind = true;
    q.kind = HYP_RECORD_DECISION;
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    const hyp_record_t *decisions[] = {fx[FX_D1], fx[FX_D2]};
    ASSERT_TRUE(set_is_exactly(got, decisions, 2));
    hyp_record_set_free(got);

    /* author, and the empty answer: a coherent question with no matches gets
     * an EMPTY set — empty means "there is nothing", and it is not an error. */
    memset(&q, 0, sizeof(q));
    q.author = "agent:two";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    const hyp_record_t *verdicts[] = {fx[FX_V1]};
    ASSERT_TRUE(set_is_exactly(got, verdicts, 1));
    hyp_record_set_free(got);
    q.author = "agent:nobody";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(got);
    ASSERT_EQ(hyp_record_set_count(got), 0);
    hyp_record_set_free(got);

    /* anchor equality is byte equality: no prefix bleed, and a value made of
     * metacharacters is matched as the bytes it is. */
    memset(&q, 0, sizeof(q));
    q.anchor = "hyp1:ws/alpha#foo";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    const hyp_record_t *exact_foo[] = {fx[FX_D1]};
    ASSERT_TRUE(set_is_exactly(got, exact_foo, 1)); /* NOT foobar */
    hyp_record_set_free(got);
    q.anchor = "a%_*?[b]c";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    const hyp_record_t *meta[] = {fx[FX_M1]};
    ASSERT_TRUE(set_is_exactly(got, meta, 1));
    hyp_record_set_free(got);

    /* anchor prefix is memcmp, not a pattern language. "a%" must match ONLY
     * the record whose anchor literally starts with 'a','%' — an engine LIKE
     * leaking through would match "abc" too and fail here. */
    memset(&q, 0, sizeof(q));
    q.anchor_prefix = "a%";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(set_is_exactly(got, meta, 1));
    hyp_record_set_free(got);
    q.anchor_prefix = "a"; /* control: the wider byte prefix takes both */
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    const hyp_record_t *a_pair[] = {fx[FX_M1], fx[FX_M2]};
    ASSERT_TRUE(set_is_exactly(got, a_pair, 2));
    hyp_record_set_free(got);
    q.anchor_prefix = "hyp1:ws/alpha#";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(set_is_exactly(got, decisions, 2)); /* alpha yes, beta no */
    hyp_record_set_free(got);
    q.anchor_prefix = "hyp1:ws/alpha#foo"; /* a prefix that is also an exact value */
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(set_is_exactly(got, decisions, 2)); /* foo AND foobar: bytes, not words */
    hyp_record_set_free(got);

    /* time range, both bounds inclusive */
    memset(&q, 0, sizeof(q));
    q.has_since = true;
    q.since_ms = FIXED_MS + 2000;
    q.has_until = true;
    q.until_ms = FIXED_MS + 4000;
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    const hyp_record_t *mid[] = {fx[FX_D2], fx[FX_S1], fx[FX_V1]};
    ASSERT_TRUE(set_is_exactly(got, mid, 3));
    hyp_record_set_free(got);
    /* an empty RANGE is a real question and its true answer is nothing */
    q.since_ms = FIXED_MS + 4000;
    q.until_ms = FIXED_MS + 2000;
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_set_count(got), 0);
    hyp_record_set_free(got);

    /* parent — ours, exact */
    memset(&q, 0, sizeof(q));
    q.parent = fx[FX_D1]->id;
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(set_is_exactly(got, verdicts, 1));
    hyp_record_set_free(got);
    q.parent = fx[FX_V1]->id; /* nothing points at the verdict */
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_set_count(got), 0);
    hyp_record_set_free(got);

    /* origin and thread equality, opaque both */
    memset(&q, 0, sizeof(q));
    q.origin = "adapterx:row-1";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(set_is_exactly(got, exact_foo, 1));
    hyp_record_set_free(got);
    memset(&q, 0, sizeof(q));
    q.thread = "conv:9";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    const hyp_record_t *conv[] = {fx[FX_S1], fx[FX_U1]};
    ASSERT_TRUE(set_is_exactly(got, conv, 2));
    hyp_record_set_free(got);

    /* filters compose by AND */
    memset(&q, 0, sizeof(q));
    q.has_kind = true;
    q.kind = HYP_RECORD_DECISION;
    q.has_since = true;
    q.since_ms = FIXED_MS + 2000;
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    const hyp_record_t *late_decision[] = {fx[FX_D2]};
    ASSERT_TRUE(set_is_exactly(got, late_decision, 1));
    hyp_record_set_free(got);

    /* a zeroed query is load: everything, canonical order */
    memset(&q, 0, sizeof(q));
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(set_is_exactly(got, fx, FX_COUNT));
    hyp_record_set_free(got);

    fixture_free(fx);
    hyp_record_store_close(s);
    (void)th_rmtree(dir);
    PASS();
}

TEST(unanchored_is_an_answer_not_a_gap) {
    /*
     * C2u's sentence, as a test: decisions are global, workspace scoping
     * comes from resolving the anchor, and an unanchored decision genuinely
     * has no workspace. So the reader can name the unanchored exactly, the
     * anchored exactly, and the two partition the store. The reader hands
     * back anchor STRINGS — resolving them is C3u's job, and nothing here
     * knows what is inside one.
     */
    const char *tmp = th_mktempdir("hyp_recstore_part");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    const hyp_record_t *fx[FX_COUNT];
    ASSERT_TRUE(fixture_populate(s, fx));

    hyp_record_store_query_t q;
    hyp_record_set_t *anchored = NULL;
    hyp_record_set_t *unanchored = NULL;

    memset(&q, 0, sizeof(q));
    q.anchored_only = true;
    ASSERT_EQ(hyp_record_store_query(s, &q, &anchored), HYP_RECORD_STORE_OK);
    const hyp_record_t *with[] = {fx[FX_D1], fx[FX_D2], fx[FX_S1], fx[FX_M1], fx[FX_M2]};
    ASSERT_TRUE(set_is_exactly(anchored, with, 5));
    for (size_t i = 0; i < hyp_record_set_count(anchored); i++) {
        ASSERT_NOT_NULL(hyp_record_set_at(anchored, i)->anchor);
    }

    memset(&q, 0, sizeof(q));
    q.unanchored_only = true;
    ASSERT_EQ(hyp_record_store_query(s, &q, &unanchored), HYP_RECORD_STORE_OK);
    const hyp_record_t *without[] = {fx[FX_V1], fx[FX_U1]};
    ASSERT_TRUE(set_is_exactly(unanchored, without, 2));
    for (size_t i = 0; i < hyp_record_set_count(unanchored); i++) {
        ASSERT_NULL(hyp_record_set_at(unanchored, i)->anchor);
    }

    /* the partition is exact: everything is in exactly one half */
    ASSERT_EQ(hyp_record_set_count(anchored) + hyp_record_set_count(unanchored), FX_COUNT);

    hyp_record_set_free(anchored);
    hyp_record_set_free(unanchored);
    fixture_free(fx);
    hyp_record_store_close(s);
    (void)th_rmtree(dir);
    PASS();
}

TEST(query_refuses_category_errors) {
    /*
     * A query that contradicts itself gets ERR_QUERY, never a plausible empty
     * set — because an empty set MEANS something here ("there is nothing"),
     * and a category error must not be allowed to counterfeit it.
     */
    const char *tmp = th_mktempdir("hyp_recstore_badq");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    const hyp_record_t *rec = rec_of("one record so refusals are not vacuous", "hyp1:ws/r#f");
    ASSERT_NOT_NULL(rec);
    ASSERT_EQ(hyp_record_store_append(s, rec, NULL), HYP_RECORD_STORE_OK);

    hyp_record_store_query_t q;
    hyp_record_set_t *got = NULL;

    /* anchored and unanchored at once */
    memset(&q, 0, sizeof(q));
    q.anchored_only = true;
    q.unanchored_only = true;
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);
    ASSERT_NULL(got);

    /* an anchor value on a query for the unanchored */
    memset(&q, 0, sizeof(q));
    q.unanchored_only = true;
    q.anchor = "hyp1:ws/r#f";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);
    memset(&q, 0, sizeof(q));
    q.unanchored_only = true;
    q.anchor_prefix = "hyp1:";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);

    /* the empty string: never stored, so never a question. Absent is NULL. */
    memset(&q, 0, sizeof(q));
    q.author = "";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);
    memset(&q, 0, sizeof(q));
    q.anchor = "";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);
    memset(&q, 0, sizeof(q));
    q.anchor_prefix = "";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);
    memset(&q, 0, sizeof(q));
    q.origin = "";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);
    memset(&q, 0, sizeof(q));
    q.thread = "";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);

    /* parent is OURS: a foreign identifier is a mistake, not a miss */
    memset(&q, 0, sizeof(q));
    q.parent = "task-42";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);
    q.parent = "6A9DFE36EC72E1D3B0A4BD7F0E0E6F2B0AA6E37BBF1EB31E7B09ED6A3A4B3F4D";
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);

    /* a kind that is not a kind */
    memset(&q, 0, sizeof(q));
    q.has_kind = true;
    q.kind = HYP_RECORD_KIND_COUNT;
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_ERR_QUERY);

    /* Controls: each shape, corrected, is a real question that answers. */
    memset(&q, 0, sizeof(q));
    q.anchored_only = true;
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_set_count(got), 1);
    hyp_record_set_free(got);
    got = NULL;
    memset(&q, 0, sizeof(q));
    q.parent = rec->id; /* well-formed, matches nothing: empty, not an error */
    ASSERT_EQ(hyp_record_store_query(s, &q, &got), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_set_count(got), 0);
    hyp_record_set_free(got);

    hyp_record_free(rec);
    hyp_record_store_close(s);
    (void)th_rmtree(dir);
    PASS();
}

/* ── The union primitive C6u builds on ──────────────────────────────────── */

TEST(append_set_is_a_union_commutative_idempotent_atomic) {
    const char *tmp = th_mktempdir("hyp_recstore_union");
    ASSERT_NOT_NULL(tmp);
    char root[512];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char dir_a[512];
    char dir_b[512];
    char dir_a2[512];
    char dir_b2[512];
    (void)snprintf(dir_a, sizeof(dir_a), "%s/a", root);
    (void)snprintf(dir_b, sizeof(dir_b), "%s/b", root);
    (void)snprintf(dir_a2, sizeof(dir_a2), "%s/a2", root);
    (void)snprintf(dir_b2, sizeof(dir_b2), "%s/b2", root);

    const hyp_record_t *r1 = rec_of("alpha", "hyp1:w/r#a");
    const hyp_record_t *r2 = rec_of("beta", "hyp1:w/r#b");
    const hyp_record_t *r3 = rec_of("gamma", "hyp1:w/r#c");
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_NOT_NULL(r3);

    /* The expected union, stated in the contract type. */
    hyp_record_set_t *want = hyp_record_set_create();
    ASSERT_NOT_NULL(want);
    ASSERT_EQ(hyp_record_set_add(want, r1, NULL), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_add(want, r2, NULL), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_add(want, r3, NULL), HYP_RECORD_OK);
    char want_digest[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(want, want_digest);
    hyp_record_set_free(want);

    /* A = {r1, r2}, B = {r2, r3}. Push B into A. */
    hyp_record_store_t *a = NULL;
    hyp_record_store_t *b = NULL;
    ASSERT_EQ(hyp_record_store_open(dir_a, &a), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_open(dir_b, &b), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(a, r1, NULL), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(a, r2, NULL), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(b, r2, NULL), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(b, r3, NULL), HYP_RECORD_STORE_OK);

    hyp_record_set_t *from_b = NULL;
    ASSERT_EQ(hyp_record_store_load(b, &from_b), HYP_RECORD_STORE_OK);
    size_t added = 0;
    ASSERT_EQ(hyp_record_store_append_set(a, from_b, &added), HYP_RECORD_STORE_OK);
    ASSERT_EQ(added, 1); /* r3 was new; r2 was absorbed */
    char digest_a[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(a, digest_a), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_a, want_digest);

    /* Idempotent: a retried sync is free rather than dangerous. */
    ASSERT_EQ(hyp_record_store_append_set(a, from_b, &added), HYP_RECORD_STORE_OK);
    ASSERT_EQ(added, 0);
    ASSERT_EQ(hyp_record_store_digest(a, digest_a), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_a, want_digest);
    hyp_record_set_free(from_b);

    /* Commutative: the reverse push, in fresh stores, converges identically. */
    hyp_record_store_t *a2 = NULL;
    hyp_record_store_t *b2 = NULL;
    ASSERT_EQ(hyp_record_store_open(dir_a2, &a2), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_open(dir_b2, &b2), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(a2, r1, NULL), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(a2, r2, NULL), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(b2, r2, NULL), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append(b2, r3, NULL), HYP_RECORD_STORE_OK);
    hyp_record_set_t *from_a2 = NULL;
    ASSERT_EQ(hyp_record_store_load(a2, &from_a2), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_append_set(b2, from_a2, &added), HYP_RECORD_STORE_OK);
    ASSERT_EQ(added, 1);
    char digest_b2[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(b2, digest_b2), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_b2, want_digest);
    hyp_record_set_free(from_a2);

    /*
     * Atomic: a batch with a tampered member writes NOTHING, even the healthy
     * records it would have taken first. The record that sorts EARLIER is the
     * healthy one, so the engine really does insert it before meeting the bad
     * one — the rollback is proven to revert work, not to skip it.
     */
    const hyp_record_t *n1 = rec_of("new record one", NULL);
    const hyp_record_t *n2 = rec_of("new record two", NULL);
    ASSERT_NOT_NULL(n1);
    ASSERT_NOT_NULL(n2);
    hyp_record_set_t *src = hyp_record_set_create();
    ASSERT_NOT_NULL(src);
    ASSERT_EQ(hyp_record_set_add(src, n1, NULL), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_add(src, n2, NULL), HYP_RECORD_OK);
    const char *later_id = strcmp(n1->id, n2->id) > 0 ? n1->id : n2->id;
    const hyp_record_t *planted = hyp_record_set_get(src, later_id);
    ASSERT_NOT_NULL(planted);
    char *content = NULL;
    memcpy(&content, &planted->content, sizeof(content));
    char original = content[0];
    content[0] = (char)(original ^ 0x20);

    size_t count_before = 0;
    ASSERT_EQ(hyp_record_store_count(a, &count_before), HYP_RECORD_STORE_OK);
    added = 99;
    ASSERT_EQ(hyp_record_store_append_set(a, src, &added), HYP_RECORD_STORE_ERR_TAMPERED);
    ASSERT_EQ(added, 0);
    size_t count_after = 0;
    ASSERT_EQ(hyp_record_store_count(a, &count_after), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count_after, count_before); /* the healthy record was rolled back */
    ASSERT_EQ(hyp_record_store_digest(a, digest_a), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_a, want_digest);

    /* Control: repair the byte and the same batch lands whole. */
    content[0] = original;
    ASSERT_EQ(hyp_record_store_append_set(a, src, &added), HYP_RECORD_STORE_OK);
    ASSERT_EQ(added, 2);
    ASSERT_EQ(hyp_record_store_count(a, &count_after), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count_after, count_before + 2);

    hyp_record_set_free(src);
    hyp_record_free(n1);
    hyp_record_free(n2);
    hyp_record_free(r1);
    hyp_record_free(r2);
    hyp_record_free(r3);
    hyp_record_store_close(a);
    hyp_record_store_close(b);
    hyp_record_store_close(a2);
    hyp_record_store_close(b2);
    (void)th_rmtree(root);
    PASS();
}

/* ── Fail closed at open ────────────────────────────────────────────────── */

TEST(open_refuses_a_foreign_or_untagged_store) {
    const char *tmp = th_mktempdir("hyp_recstore_contract");
    ASSERT_NOT_NULL(tmp);
    char root[512];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s/store", root);
    char dir_empty[512];
    (void)snprintf(dir_empty, sizeof(dir_empty), "%s/empty", root);

    hyp_record_store_t *s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_OK);
    const hyp_record_t *rec = rec_of("a record under the v1 contract", NULL);
    ASSERT_NOT_NULL(rec);
    ASSERT_EQ(hyp_record_store_append(s, rec, NULL), HYP_RECORD_STORE_OK);
    hyp_record_free(rec);
    hyp_record_store_close(s);

    /* A future contract is a DISJOINT id space: refuse, never half-read. */
    ASSERT_EQ(raw_exec(dir, "UPDATE record_store_meta SET value = 'hyp-record-v2'"
                            " WHERE key = 'contract';"),
              0);
    s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_ERR_CONTRACT);
    ASSERT_NULL(s);

    /* Rows of unknown provenance: a populated store with NO tag is refused,
     * not adopted — stamping it would be adoption by accident. */
    ASSERT_EQ(raw_exec(dir, "DELETE FROM record_store_meta WHERE key = 'contract';"), 0);
    ASSERT_EQ(hyp_record_store_open(dir, &s), HYP_RECORD_STORE_ERR_CONTRACT);
    ASSERT_NULL(s);

    /* Control: an EMPTY tagless store is what a fresh database is — adopted,
     * stamped, and usable. */
    ASSERT_EQ(hyp_record_store_open(dir_empty, &s), HYP_RECORD_STORE_OK);
    hyp_record_store_close(s);
    ASSERT_EQ(raw_exec(dir_empty, "DELETE FROM record_store_meta WHERE key = 'contract';"), 0);
    s = NULL;
    ASSERT_EQ(hyp_record_store_open(dir_empty, &s), HYP_RECORD_STORE_OK);
    hyp_record_store_close(s);

    (void)th_rmtree(root);
    PASS();
}

/* ── Status hygiene ─────────────────────────────────────────────────────── */

TEST(record_store_status_reasons_are_present_and_distinct) {
    const hyp_record_store_status_t all[] = {HYP_RECORD_STORE_OK,
                                             HYP_RECORD_STORE_ERR_NULL,
                                             HYP_RECORD_STORE_ERR_OPEN,
                                             HYP_RECORD_STORE_ERR_CONTRACT,
                                             HYP_RECORD_STORE_ERR_IO,
                                             HYP_RECORD_STORE_ERR_TAMPERED,
                                             HYP_RECORD_STORE_ERR_COLLISION,
                                             HYP_RECORD_STORE_ERR_QUERY,
                                             HYP_RECORD_STORE_ERR_ALLOC};
    size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const char *reason = hyp_record_store_status_reason(all[i]);
        ASSERT_NOT_NULL(reason);
        ASSERT_TRUE(reason[0] != '\0');
        for (size_t j = i + 1; j < n; j++) {
            ASSERT_STR_NEQ(reason, hyp_record_store_status_reason(all[j]));
        }
    }
    /* error() on a healthy handle-less call is "" and never NULL */
    ASSERT_STR_EQ(hyp_record_store_error(NULL), "");
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(record_store) {
    RUN_TEST(record_store_opens_creates_and_reopens_empty);
    RUN_TEST(record_store_round_trips_across_reopen);
    /* not a log */
    RUN_TEST(two_write_orders_one_enumeration);
    RUN_TEST(same_record_twice_is_absorbed);
    RUN_TEST(schema_exposes_no_sequence_and_no_extra_column);
    /* immutability, both directions */
    RUN_TEST(append_refuses_a_tampered_record_and_writes_nothing);
    RUN_TEST(reader_refuses_a_store_edited_behind_its_back);
    /* the reader */
    RUN_TEST(query_answers_each_axis_and_says_no);
    RUN_TEST(unanchored_is_an_answer_not_a_gap);
    RUN_TEST(query_refuses_category_errors);
    /* the union primitive */
    RUN_TEST(append_set_is_a_union_commutative_idempotent_atomic);
    /* fail closed */
    RUN_TEST(open_refuses_a_foreign_or_untagged_store);
    RUN_TEST(record_store_status_reasons_are_present_and_distinct);
}
