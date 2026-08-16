/*
 * test_watched_ingest.c — watched-directory ingest, the generic feed adapter
 * (§4 D5).
 *
 * The suite is organised by the claims the adapter makes, and the two that
 * matter most each carry the assertion a negative control must break:
 *
 *   - ORIGIN IS POSITIONAL. Two byte-identical rows at two file positions are
 *     two records, and the same position re-ingested is the same record. Make
 *     the derivation content-based and origin_offsets_make_identical_rows_
 *     distinct fails — the duplicate collapses, exactly the dedup-vs-loss
 *     ambiguity D4's audit exists to rule out.
 *   - FAILURE IS PER FILE. One malformed file is quarantined by name; every
 *     other file still yields. Make a bad file abort the scan and
 *     a_bad_file_fails_closed_alone_and_named fails.
 *
 * Fixtures are SYNTHETIC, generated into a scratch directory at run time —
 * never a real transcript, and nothing key-like beyond the plainly fake
 * "hyp_fixture_not_a_key" marker used to prove the scrub seam runs. Every
 * timestamp is a constant from the fixture, because the adapter reading a
 * clock is one of the failures under test (I6).
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "../src/foundation/compat.h"
#include "../src/ingest/watched_ingest.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 2026-02-14T12:26:40.000Z — fixed, supplied by the fixture, never captured. */
#define FIXED_MS INT64_C(1770985600000)

/* ── Scrubbers ──────────────────────────────────────────────────────────── */

static bool identity_scrub(void *ctx, const char *content, size_t len, char **out,
                           uint32_t *redactions) {
    (void)ctx;
    (void)len;
    char *copy = (char *)malloc(strlen(content) + 1);
    if (!copy) {
        return false;
    }
    strcpy(copy, content);
    *out = copy;
    *redactions = 0;
    return true;
}

/* Replaces every "hyp_fixture_not_a_key" with "[redacted]" and counts. A toy
 * standing where the secret scrubber (D2) slots in; the seam, not the scrub,
 * is what this suite asserts. */
static bool counting_scrub(void *ctx, const char *content, size_t len, char **out,
                           uint32_t *redactions) {
    (void)ctx;
    (void)len;
    static const char needle[] = "hyp_fixture_not_a_key";
    static const char mask[] = "[redacted]";
    char *copy = (char *)malloc(strlen(content) + 1);
    if (!copy) {
        return false;
    }
    uint32_t hits = 0;
    const char *src = content;
    char *dst = copy;
    while (*src) {
        if (strncmp(src, needle, sizeof(needle) - 1) == 0) {
            memcpy(dst, mask, sizeof(mask) - 1);
            dst += sizeof(mask) - 1;
            src += sizeof(needle) - 1;
            hits++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    *out = copy;
    *redactions = hits;
    return true;
}

static bool refusing_scrub(void *ctx, const char *content, size_t len, char **out,
                           uint32_t *redactions) {
    (void)ctx;
    (void)content;
    (void)len;
    (void)out;
    (void)redactions;
    return false;
}

/* ── A sink that records the client's view ──────────────────────────────── */

enum { MAX_ROWS = 64 };

typedef struct {
    char id[HYP_RECORD_ID_LEN + 1];
    int kind;
    char author[64];
    int64_t timestamp_ms;
    char content[256];
    char origin[HYP_RECORD_MAX_ORIGIN + 1];
    bool has_anchor;
    char anchor[128];
    bool has_thread;
    char thread[HYP_RECORD_MAX_THREAD + 1];
    bool has_parent;
    uint32_t redactions;
    int build_status; /* hyp_record_build on the yielded row — the client's view */
} seen_row_t;

typedef struct {
    int count;
    int accept_limit; /* refuse rows beyond this many; -1 means accept all */
    seen_row_t rows[MAX_ROWS];
} collector_t;

static void collector_init(collector_t *c) {
    memset(c, 0, sizeof(*c));
    c->accept_limit = -1;
}

static bool collect_sink(void *ctx, const hyp_record_input_t *row) {
    collector_t *c = (collector_t *)ctx;
    if (c->accept_limit >= 0 && c->count >= c->accept_limit) {
        return false;
    }
    if (c->count >= MAX_ROWS) {
        return false;
    }
    seen_row_t *r = &c->rows[c->count];
    memset(r, 0, sizeof(*r));
    r->kind = (int)row->kind;
    snprintf(r->author, sizeof(r->author), "%s", row->author ? row->author : "");
    r->timestamp_ms = row->timestamp_ms;
    snprintf(r->content, sizeof(r->content), "%s", row->content ? row->content : "");
    snprintf(r->origin, sizeof(r->origin), "%s", row->origin ? row->origin : "");
    r->has_anchor = row->anchor != NULL;
    snprintf(r->anchor, sizeof(r->anchor), "%s", row->anchor ? row->anchor : "");
    r->has_thread = row->thread != NULL;
    snprintf(r->thread, sizeof(r->thread), "%s", row->thread ? row->thread : "");
    r->has_parent = row->parent != NULL;
    r->redactions = row->redactions;
    if (hyp_record_derive_id(row, r->id) != HYP_RECORD_OK) {
        r->id[0] = '\0';
    }
    const hyp_record_t *rec = NULL;
    r->build_status = (int)hyp_record_build(row, &rec);
    hyp_record_free(rec);
    c->count++;
    return true;
}

/* ── Fixture plumbing ───────────────────────────────────────────────────── */

/* Scratch roots live under the build tree when there is one, so fixtures
 * never land in the repository; /tmp is the fallback for a bare runner. */
static bool fixture_root(char *buf, size_t cap) {
    snprintf(buf, cap, "build/wingest-XXXXXX");
    if (hyp_mkdtemp(buf)) {
        return true;
    }
    snprintf(buf, cap, "/tmp/wingest-XXXXXX");
    return hyp_mkdtemp(buf) != NULL;
}

static bool find_file(const hyp_watched_scanner_t *s, const char *relpath,
                      hyp_watched_file_t *out) {
    for (size_t i = 0; i < hyp_watched_file_count(s); i++) {
        if (hyp_watched_file_at(s, i, out) && strcmp(out->relpath, relpath) == 0) {
            return true;
        }
    }
    return false;
}

/* A well-formed minimal line: author, timestamp_ms, content. */
static void minimal_line(char *buf, size_t cap, const char *author, int64_t ts,
                         const char *content) {
    snprintf(buf, cap, "{\"author\":\"%s\",\"timestamp_ms\":%" PRId64 ",\"content\":\"%s\"}",
             author, ts, content);
}

/* ── The scrub precondition ─────────────────────────────────────────────── */

TEST(scanner_refuses_unscrubbed_ingest) {
    /* No scrubber, no scanner: unscrubbed ingest is impossible to express,
     * not merely discouraged — the id commits to content and a record is
     * permanent (D2 is a precondition of real ingest, not a follow-up). */
    ASSERT_NULL(hyp_watched_scanner_create("somewhere", NULL, NULL));
    ASSERT_NULL(hyp_watched_scanner_create(NULL, identity_scrub, NULL));
    ASSERT_NULL(hyp_watched_scanner_create("", identity_scrub, NULL));

    hyp_watched_scanner_t *s = hyp_watched_scanner_create("somewhere", identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    hyp_watched_scanner_free(s);
    hyp_watched_scanner_free(NULL); /* NULL is a no-op, as everywhere */
    PASS();
}

TEST(scrub_runs_before_the_record_and_counts) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    char line[512];
    minimal_line(line, sizeof(line), "agent:leaky", FIXED_MS,
                 "the token hyp_fixture_not_a_key then hyp_fixture_not_a_key again");
    char body[600];
    snprintf(body, sizeof(body), "%s\n", line);
    ASSERT_EQ(th_write_file(TH_PATH(root, "leak.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, counting_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 1);
    /* The yielded content is the SCRUBBED content — the record the sink can
     * build has already lost the secret, and says how much it lost. */
    ASSERT_STR_EQ(c.rows[0].content, "the token [redacted] then [redacted] again");
    ASSERT_EQ((int)c.rows[0].redactions, 2);
    ASSERT_EQ(c.rows[0].build_status, (int)HYP_RECORD_OK);
    hyp_watched_scanner_free(s);

    /* A scrubber that refuses fails the file closed: no row passes it. */
    hyp_watched_scanner_t *s2 = hyp_watched_scanner_create(root, refusing_scrub, NULL);
    ASSERT_NOT_NULL(s2);
    collector_t c2;
    collector_init(&c2);
    ASSERT_TRUE(hyp_watched_scan_once(s2, collect_sink, &c2, NULL));
    ASSERT_EQ(c2.count, 0);
    hyp_watched_file_t info;
    ASSERT_TRUE(find_file(s2, "leak.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_FAILED);
    ASSERT_EQ((int)info.reason, (int)HYP_WATCHED_REASON_SCRUB_FAILED);
    hyp_watched_scanner_free(s2);

    th_rmtree(root);
    PASS();
}

/* ── The format, as a client reads it back ──────────────────────────────── */

TEST(happy_path_rows_map_onto_the_record_contract) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    const char *l0 = "{\"v\":1,\"kind\":\"decision\",\"author\":\"agent:one\","
                     "\"timestamp_ms\":1770985600000,\"content\":\"chose the union\","
                     "\"anchor\":\"addr:opaque\",\"thread\":\"t-explicit\",\"parent\":null}";
    const char *l1 = "{\"author\":\"agent:two\",\"timestamp_ms\":1770985600001,"
                     "\"content\":\"a minimal message\"}";
    const char *l2 = "{\"kind\":\"signal\",\"author\":\"agent:two\","
                     "\"timestamp_ms\":1770985600002,\"content\":\"unknown keys ride along\","
                     "\"session_note\":\"ignored by the adapter\"}";
    char body[1024];
    snprintf(body, sizeof(body), "%s\n%s\n%s\n", l0, l1, l2);
    ASSERT_EQ(th_write_file(TH_PATH(root, "a.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    hyp_watched_report_t report;
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, &report));

    ASSERT_EQ(c.count, 3);
    ASSERT_EQ((int)report.rows_yielded, 3);
    ASSERT_EQ((int)report.files_seen, 1);
    ASSERT_EQ((int)report.files_failed, 0);

    /* Row 0: everything explicit. */
    ASSERT_EQ(c.rows[0].kind, (int)HYP_RECORD_DECISION);
    ASSERT_STR_EQ(c.rows[0].author, "agent:one");
    ASSERT_TRUE(c.rows[0].timestamp_ms == FIXED_MS);
    ASSERT_TRUE(c.rows[0].has_anchor);
    ASSERT_STR_EQ(c.rows[0].anchor, "addr:opaque");
    ASSERT_TRUE(c.rows[0].has_thread);
    ASSERT_STR_EQ(c.rows[0].thread, "t-explicit");
    ASSERT_FALSE(c.rows[0].has_parent); /* explicit null: points at nothing */

    /* Row 1: the defaults. kind falls back to message; the thread is the
     * file's own; anchor stays absent. */
    ASSERT_EQ(c.rows[1].kind, (int)HYP_RECORD_MESSAGE);
    ASSERT_FALSE(c.rows[1].has_anchor);
    ASSERT_TRUE(c.rows[1].has_thread);
    ASSERT_STR_EQ(c.rows[1].thread, "wd1:a.jsonl");

    /* Row 2: an unknown key neither blocks nor leaks into the record. */
    ASSERT_EQ(c.rows[2].kind, (int)HYP_RECORD_SIGNAL);

    /* Origins are path plus byte offset, byte-for-byte what the derivation
     * helpers say — D4's audit walks the same functions. */
    char expected[HYP_RECORD_MAX_ORIGIN + 1];
    ASSERT_TRUE(hyp_watched_origin("a.jsonl", 0, expected, sizeof(expected)));
    ASSERT_STR_EQ(c.rows[0].origin, expected);
    ASSERT_TRUE(hyp_watched_origin("a.jsonl", strlen(l0) + 1, expected, sizeof(expected)));
    ASSERT_STR_EQ(c.rows[1].origin, expected);
    ASSERT_TRUE(
        hyp_watched_origin("a.jsonl", strlen(l0) + strlen(l1) + 2, expected, sizeof(expected)));
    ASSERT_STR_EQ(c.rows[2].origin, expected);

    /* Every yielded row is buildable — the promise the sink relies on. */
    for (int i = 0; i < c.count; i++) {
        ASSERT_EQ(c.rows[i].build_status, (int)HYP_RECORD_OK);
        ASSERT_TRUE(hyp_record_id_is_valid(c.rows[i].id));
    }

    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

TEST(crlf_and_blank_lines_are_tolerated) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    char l0[256];
    char l1[256];
    minimal_line(l0, sizeof(l0), "agent:win", FIXED_MS, "written with CRLF");
    minimal_line(l1, sizeof(l1), "agent:win", FIXED_MS + 1, "after a blank line");
    char body[600];
    snprintf(body, sizeof(body), "%s\r\n\n%s\n", l0, l1);
    ASSERT_EQ(th_write_file(TH_PATH(root, "crlf.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 2);
    ASSERT_STR_EQ(c.rows[0].content, "written with CRLF");
    ASSERT_STR_EQ(c.rows[1].content, "after a blank line");
    /* The second row's offset counts the CR and the blank line: offsets are
     * byte-true to the file, not to a normalised view of it. */
    char expected[HYP_RECORD_MAX_ORIGIN + 1];
    ASSERT_TRUE(hyp_watched_origin("crlf.jsonl", strlen(l0) + 3, expected, sizeof(expected)));
    ASSERT_STR_EQ(c.rows[1].origin, expected);
    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

TEST(null_and_absent_are_different_answers) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    const char *absent = "{\"author\":\"a\",\"timestamp_ms\":1770985600000,\"content\":\"x\"}";
    const char *null_thread = "{\"author\":\"a\",\"timestamp_ms\":1770985600000,"
                              "\"content\":\"x\",\"thread\":null}";
    const char *named = "{\"author\":\"a\",\"timestamp_ms\":1770985600000,"
                        "\"content\":\"x\",\"thread\":\"t-9\"}";
    char body[1024];
    snprintf(body, sizeof(body), "%s\n%s\n%s\n", absent, null_thread, named);
    ASSERT_EQ(th_write_file(TH_PATH(root, "t.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 3);
    /* Absent: the conversation is the file. Null: there is no conversation.
     * Only one of those is ever true, and they must not collapse. */
    ASSERT_TRUE(c.rows[0].has_thread);
    ASSERT_STR_EQ(c.rows[0].thread, "wd1:t.jsonl");
    ASSERT_FALSE(c.rows[1].has_thread);
    ASSERT_TRUE(c.rows[2].has_thread);
    ASSERT_STR_EQ(c.rows[2].thread, "t-9");
    /* And the three are three distinct records. */
    ASSERT_STR_NEQ(c.rows[0].id, c.rows[1].id);
    ASSERT_STR_NEQ(c.rows[1].id, c.rows[2].id);
    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

/* ── Origin: positional, idempotent, audit-ready (I8, D4) ───────────────── */

TEST(origin_offsets_make_identical_rows_distinct) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    /* The same event reported twice IS two rows — a harness that repeats
     * itself is a fact about the session, and D4's audit must be able to see
     * both. Content-derived origins would collapse them (the control that
     * must break this test). */
    char line[256];
    minimal_line(line, sizeof(line), "agent:echo", FIXED_MS, "same words twice");
    char body[600];
    snprintf(body, sizeof(body), "%s\n%s\n", line, line);
    ASSERT_EQ(th_write_file(TH_PATH(root, "twice.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 2);
    ASSERT_STR_NEQ(c.rows[0].origin, c.rows[1].origin);
    ASSERT_STR_NEQ(c.rows[0].id, c.rows[1].id);
    hyp_watched_scanner_free(s);

    /* Re-ingest from scratch: same positions, same origins, same ids — a
     * union of one, with no dedup table anywhere (I8). */
    hyp_watched_scanner_t *again = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(again);
    collector_t c2;
    collector_init(&c2);
    ASSERT_TRUE(hyp_watched_scan_once(again, collect_sink, &c2, NULL));
    ASSERT_EQ(c2.count, 2);
    ASSERT_STR_EQ(c.rows[0].id, c2.rows[0].id);
    ASSERT_STR_EQ(c.rows[1].id, c2.rows[1].id);
    hyp_watched_scanner_free(again);

    th_rmtree(root);
    PASS();
}

TEST(growth_yields_only_the_new_rows) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    char l0[256];
    minimal_line(l0, sizeof(l0), "agent:g", FIXED_MS, "first");
    char body[300];
    snprintf(body, sizeof(body), "%s\n", l0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "grow.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 1);

    /* Append a row BYTE-IDENTICAL to the first. Growth must yield exactly the
     * new row, and it must be a NEW record: same words, new position. */
    ASSERT_EQ(th_append_file(TH_PATH(root, "grow.jsonl"), body), 0);
    collector_t c2;
    collector_init(&c2);
    hyp_watched_report_t report;
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c2, &report));
    ASSERT_EQ(c2.count, 1);
    ASSERT_EQ((int)report.rows_yielded, 1);
    char expected[HYP_RECORD_MAX_ORIGIN + 1];
    ASSERT_TRUE(hyp_watched_origin("grow.jsonl", strlen(l0) + 1, expected, sizeof(expected)));
    ASSERT_STR_EQ(c2.rows[0].origin, expected);
    ASSERT_STR_NEQ(c2.rows[0].id, c.rows[0].id);

    /* A third scan with nothing new yields nothing — growth, not presence,
     * is what triggers work. */
    collector_t c3;
    collector_init(&c3);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c3, NULL));
    ASSERT_EQ(c3.count, 0);
    hyp_watched_scanner_free(s);

    /* And a cold rescan agrees with the incremental one, row for row. */
    hyp_watched_scanner_t *cold = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(cold);
    collector_t all;
    collector_init(&all);
    ASSERT_TRUE(hyp_watched_scan_once(cold, collect_sink, &all, NULL));
    ASSERT_EQ(all.count, 2);
    ASSERT_STR_EQ(all.rows[0].id, c.rows[0].id);
    ASSERT_STR_EQ(all.rows[1].id, c2.rows[0].id);
    hyp_watched_scanner_free(cold);

    th_rmtree(root);
    PASS();
}

TEST(timestamps_come_from_the_file) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    /* A deliberately old instant. If the adapter consulted any clock, the
     * yielded timestamp or the derived id would drift between runs; both are
     * pinned here to the fixture's own numbers. */
    char line[256];
    minimal_line(line, sizeof(line), "agent:old", INT64_C(946684800000), "written long ago");
    char body[300];
    snprintf(body, sizeof(body), "%s\n", line);
    ASSERT_EQ(th_write_file(TH_PATH(root, "old.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 1);
    ASSERT_TRUE(c.rows[0].timestamp_ms == INT64_C(946684800000));
    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

/* ── Partial writes and growth boundaries ───────────────────────────────── */

TEST(partial_last_line_is_not_yet) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    char l0[256];
    char l1[256];
    minimal_line(l0, sizeof(l0), "agent:p", FIXED_MS, "complete");
    minimal_line(l1, sizeof(l1), "agent:p", FIXED_MS + 1, "arrives in two writes");

    /* First half of l1, no newline: a harness mid-append. */
    char body[600];
    snprintf(body, sizeof(body), "%s\n%.20s", l0, l1);
    ASSERT_EQ(th_write_file(TH_PATH(root, "mid.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    hyp_watched_report_t report;
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, &report));
    ASSERT_EQ(c.count, 1); /* the complete line, and only it */
    ASSERT_EQ((int)report.files_pending, 1);
    ASSERT_EQ((int)report.files_failed, 0); /* not an error — not yet */
    hyp_watched_file_t info;
    ASSERT_TRUE(find_file(s, "mid.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_PENDING);
    ASSERT_TRUE(info.consumed == strlen(l0) + 1);

    /* The rest of the line lands. The row is yielded from ITS line start —
     * the same origin a cold rescan would derive. */
    char rest[600];
    snprintf(rest, sizeof(rest), "%s\n", l1 + 20);
    ASSERT_EQ(th_append_file(TH_PATH(root, "mid.jsonl"), rest), 0);
    collector_t c2;
    collector_init(&c2);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c2, NULL));
    ASSERT_EQ(c2.count, 1);
    ASSERT_STR_EQ(c2.rows[0].content, "arrives in two writes");
    char expected[HYP_RECORD_MAX_ORIGIN + 1];
    ASSERT_TRUE(hyp_watched_origin("mid.jsonl", strlen(l0) + 1, expected, sizeof(expected)));
    ASSERT_STR_EQ(c2.rows[0].origin, expected);
    ASSERT_TRUE(find_file(s, "mid.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_OK);

    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

/* ── Failure: per file, named, non-blocking ─────────────────────────────── */

TEST(a_bad_file_fails_closed_alone_and_named) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    char good[256];
    minimal_line(good, sizeof(good), "agent:ok", FIXED_MS, "healthy row");
    char one_line[300];
    snprintf(one_line, sizeof(one_line), "%s\n", good);
    ASSERT_EQ(th_write_file(TH_PATH(root, "good1.jsonl"), one_line), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "good2.jsonl"), one_line), 0);

    /* bad.jsonl: a valid line, then garbage, then a line that would be valid.
     * The prefix is ingested; nothing after the bad line is. */
    char bad_body[900];
    snprintf(bad_body, sizeof(bad_body), "%s\n{this is not json\n%s\n", good, good);
    ASSERT_EQ(th_write_file(TH_PATH(root, "bad.jsonl"), bad_body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    hyp_watched_report_t report;
    /* The scan COMPLETES: a bad file is a finding, not an abort (the control
     * that must break this test makes it an abort). */
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, &report));
    ASSERT_EQ((int)report.files_seen, 3);
    ASSERT_EQ((int)report.files_failed, 1);
    ASSERT_EQ(c.count, 3); /* good1 + good2 + bad's valid prefix */

    hyp_watched_file_t info;
    ASSERT_TRUE(find_file(s, "bad.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_FAILED);
    ASSERT_EQ((int)info.reason, (int)HYP_WATCHED_REASON_BAD_JSON);
    ASSERT_TRUE(info.failed_at == strlen(good) + 1); /* the reason NAMES the line */
    ASSERT_TRUE(info.detail != NULL && info.detail[0] != '\0');
    ASSERT_TRUE(info.rows == 1);

    ASSERT_TRUE(find_file(s, "good1.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_OK);
    ASSERT_TRUE(find_file(s, "good2.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_OK);

    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

TEST(a_failed_file_heals_when_fixed) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    char good[256];
    minimal_line(good, sizeof(good), "agent:h", FIXED_MS, "kept");
    char body[600];
    snprintf(body, sizeof(body), "%s\n{oops\n", good);
    ASSERT_EQ(th_write_file(TH_PATH(root, "heal.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 1);
    hyp_watched_file_t info;
    ASSERT_TRUE(find_file(s, "heal.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_FAILED);

    /* The operator replaces the bad tail with a longer, valid line. The file
     * did not shrink, so the next scan retries from the frozen offset and the
     * quarantine lifts. Already-ingested rows keep their positions. */
    char fixed_tail[300];
    minimal_line(fixed_tail, sizeof(fixed_tail), "agent:h", FIXED_MS + 1, "repaired and longer");
    char fixed_body[900];
    snprintf(fixed_body, sizeof(fixed_body), "%s\n%s\n", good, fixed_tail);
    ASSERT_EQ(th_write_file(TH_PATH(root, "heal.jsonl"), fixed_body), 0);

    collector_t c2;
    collector_init(&c2);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c2, NULL));
    ASSERT_EQ(c2.count, 1);
    ASSERT_STR_EQ(c2.rows[0].content, "repaired and longer");
    ASSERT_TRUE(find_file(s, "heal.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_OK);

    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

TEST(every_refusal_is_named) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    /* One file per way a line can be wrong; the scan must name each one and
     * yield nothing. Filenames sort with their case. */
    struct {
        const char *name;
        const char *line;
        hyp_watched_reason_t reason;
    } cases[] = {
        {"array.jsonl", "[1,2,3]", HYP_WATCHED_REASON_BAD_JSON},
        {"badkind.jsonl",
         "{\"kind\":\"task_message\",\"author\":\"a\",\"timestamp_ms\":1,\"content\":\"x\"}",
         HYP_WATCHED_REASON_BAD_FIELD},
        {"badparent.jsonl",
         "{\"author\":\"a\",\"timestamp_ms\":1,\"content\":\"x\",\"parent\":\"not-an-id\"}",
         HYP_WATCHED_REASON_ROW_REFUSED},
        {"emptycontent.jsonl", "{\"author\":\"a\",\"timestamp_ms\":1,\"content\":\"\"}",
         HYP_WATCHED_REASON_ROW_REFUSED},
        {"forgedid.jsonl",
         "{\"id\":\"abc\",\"author\":\"a\",\"timestamp_ms\":1,\"content\":\"x\"}",
         HYP_WATCHED_REASON_FORGED_FIELD},
        {"forgedorigin.jsonl",
         "{\"origin\":\"row-7\",\"author\":\"a\",\"timestamp_ms\":1,\"content\":\"x\"}",
         HYP_WATCHED_REASON_FORGED_FIELD},
        {"forgedredactions.jsonl",
         "{\"redactions\":0,\"author\":\"a\",\"timestamp_ms\":1,\"content\":\"x\"}",
         HYP_WATCHED_REASON_FORGED_FIELD},
        {"noauthor.jsonl", "{\"timestamp_ms\":1,\"content\":\"x\"}",
         HYP_WATCHED_REASON_BAD_FIELD},
        {"tsstring.jsonl",
         "{\"author\":\"a\",\"timestamp_ms\":\"2026-02-14T12:26:40Z\",\"content\":\"x\"}",
         HYP_WATCHED_REASON_BAD_FIELD},
        {"v2.jsonl", "{\"v\":2,\"author\":\"a\",\"timestamp_ms\":1,\"content\":\"x\"}",
         HYP_WATCHED_REASON_BAD_VERSION},
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < n; i++) {
        char body[512];
        snprintf(body, sizeof(body), "%s\n", cases[i].line);
        ASSERT_EQ(th_write_file(TH_PATH(root, cases[i].name), body), 0);
    }

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    hyp_watched_report_t report;
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, &report));
    ASSERT_EQ(c.count, 0);
    ASSERT_EQ(report.files_failed, n);

    for (size_t i = 0; i < n; i++) {
        hyp_watched_file_t info;
        ASSERT_TRUE(find_file(s, cases[i].name, &info));
        ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_FAILED);
        ASSERT_EQ((int)info.reason, (int)cases[i].reason);
        ASSERT_TRUE(info.detail != NULL && info.detail[0] != '\0');
    }

    /* Every reason names itself distinctly — two refusals sharing a string
     * become one indistinguishable log line. */
    for (int a = 0; a <= (int)HYP_WATCHED_REASON_ALLOC; a++) {
        const char *name = hyp_watched_reason_name((hyp_watched_reason_t)a);
        ASSERT_NOT_NULL(name);
        ASSERT_TRUE(name[0] != '\0');
        for (int b = a + 1; b <= (int)HYP_WATCHED_REASON_ALLOC; b++) {
            ASSERT_STR_NEQ(name, hyp_watched_reason_name((hyp_watched_reason_t)b));
        }
    }

    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

TEST(oversize_is_refused_never_truncated) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    /* Content over the record cap: parses fine, and the record contract
     * refuses it BY NAME. Truncating would ingest bytes the file never held
     * under an id that claims it did. */
    size_t big = (size_t)HYP_RECORD_MAX_CONTENT + 1;
    char *buf = (char *)malloc(big + 256);
    ASSERT_NOT_NULL(buf);
    int head = snprintf(buf, 256, "{\"author\":\"a\",\"timestamp_ms\":1,\"content\":\"");
    memset(buf + head, 'a', big);
    memcpy(buf + head + big, "\"}\n", 4);
    ASSERT_EQ(th_write_file(TH_PATH(root, "bigcontent.jsonl"), buf), 0);
    free(buf);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 0);
    hyp_watched_file_t info;
    ASSERT_TRUE(find_file(s, "bigcontent.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_FAILED);
    ASSERT_EQ((int)info.reason, (int)HYP_WATCHED_REASON_ROW_REFUSED);
    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

TEST(a_shrunk_file_is_refused_and_stays_refused) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    char l0[256];
    char l1[256];
    minimal_line(l0, sizeof(l0), "agent:s", FIXED_MS, "first");
    minimal_line(l1, sizeof(l1), "agent:s", FIXED_MS + 1, "second");
    char body[600];
    snprintf(body, sizeof(body), "%s\n%s\n", l0, l1);
    ASSERT_EQ(th_write_file(TH_PATH(root, "shrink.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 2);

    /* The file is rewritten shorter — its offsets no longer mean what the
     * ingested origins say they mean. Refused by name, and permanently:
     * regrowing it does not restore trust. */
    char shorter[300];
    snprintf(shorter, sizeof(shorter), "%s\n", l0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "shrink.jsonl"), shorter), 0);
    collector_t c2;
    collector_init(&c2);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c2, NULL));
    ASSERT_EQ(c2.count, 0);
    hyp_watched_file_t info;
    ASSERT_TRUE(find_file(s, "shrink.jsonl", &info));
    ASSERT_EQ((int)info.state, (int)HYP_WATCHED_FILE_FAILED);
    ASSERT_EQ((int)info.reason, (int)HYP_WATCHED_REASON_SHRUNK);

    char longer[900];
    snprintf(longer, sizeof(longer), "%s\n%s\n%s\n", l0, l1, l0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "shrink.jsonl"), longer), 0);
    collector_t c3;
    collector_init(&c3);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c3, NULL));
    ASSERT_EQ(c3.count, 0);
    ASSERT_TRUE(find_file(s, "shrink.jsonl", &info));
    ASSERT_EQ((int)info.reason, (int)HYP_WATCHED_REASON_SHRUNK);

    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

/* ── The sink can say stop, and loses nothing ───────────────────────────── */

TEST(sink_refusal_aborts_and_nothing_is_lost) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));

    char l0[256];
    char l1[256];
    char l2[256];
    minimal_line(l0, sizeof(l0), "agent:q", FIXED_MS, "one");
    minimal_line(l1, sizeof(l1), "agent:q", FIXED_MS + 1, "two");
    minimal_line(l2, sizeof(l2), "agent:q", FIXED_MS + 2, "three");
    char body[900];
    snprintf(body, sizeof(body), "%s\n%s\n%s\n", l0, l1, l2);
    ASSERT_EQ(th_write_file(TH_PATH(root, "stop.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    c.accept_limit = 1; /* the store fills after one row */
    ASSERT_FALSE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 1);
    ASSERT_STR_EQ(c.rows[0].content, "one");

    /* The refused row was NOT consumed: the next scan starts exactly there.
     * A sink error costs a retry, never a record. */
    collector_t c2;
    collector_init(&c2);
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c2, NULL));
    ASSERT_EQ(c2.count, 2);
    ASSERT_STR_EQ(c2.rows[0].content, "two");
    ASSERT_STR_EQ(c2.rows[1].content, "three");

    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

/* ── Discovery ──────────────────────────────────────────────────────────── */

TEST(discovery_is_recursive_filtered_and_deterministic) {
    char root[256];
    ASSERT_TRUE(fixture_root(root, sizeof(root)));
    ASSERT_EQ(th_mkdir_p(TH_PATH(root, "nest/deeper")), 0);

    char line[256];
    minimal_line(line, sizeof(line), "agent:d", FIXED_MS, "found");
    char body[300];
    snprintf(body, sizeof(body), "%s\n", line);
    ASSERT_EQ(th_write_file(TH_PATH(root, "zeta.jsonl"), body), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "nest/deeper/alpha.jsonl"), body), 0);
    /* Not feed files: wrong suffix, hidden. */
    ASSERT_EQ(th_write_file(TH_PATH(root, "notes.txt"), body), 0);
    ASSERT_EQ(th_write_file(TH_PATH(root, ".hidden.jsonl"), body), 0);

    hyp_watched_scanner_t *s = hyp_watched_scanner_create(root, identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    hyp_watched_report_t report;
    ASSERT_TRUE(hyp_watched_scan_once(s, collect_sink, &c, &report));
    ASSERT_EQ((int)report.files_seen, 2);
    ASSERT_EQ(c.count, 2);
    /* Deterministic order: relpath-sorted, so every machine yields the same
     * sequence over the same tree. */
    char expected[HYP_RECORD_MAX_ORIGIN + 1];
    ASSERT_TRUE(hyp_watched_origin("nest/deeper/alpha.jsonl", 0, expected, sizeof(expected)));
    ASSERT_STR_EQ(c.rows[0].origin, expected);
    ASSERT_TRUE(hyp_watched_origin("zeta.jsonl", 0, expected, sizeof(expected)));
    ASSERT_STR_EQ(c.rows[1].origin, expected);
    /* Distinct files, identical bytes, distinct records. */
    ASSERT_STR_NEQ(c.rows[0].id, c.rows[1].id);

    hyp_watched_scanner_free(s);
    th_rmtree(root);
    PASS();
}

TEST(derivation_helpers_refuse_truncation) {
    char out[HYP_RECORD_MAX_ORIGIN + 1];
    ASSERT_TRUE(hyp_watched_origin("a.jsonl", 42, out, sizeof(out)));
    ASSERT_STR_EQ(out, "wd1:a.jsonl@42");
    ASSERT_TRUE(hyp_watched_thread("a.jsonl", out, sizeof(out)));
    ASSERT_STR_EQ(out, "wd1:a.jsonl");
    /* Too small a buffer is a refusal, never a truncation — a truncated
     * origin would alias a different file position. */
    ASSERT_FALSE(hyp_watched_origin("a.jsonl", 42, out, 8));
    ASSERT_FALSE(hyp_watched_origin("", 0, out, sizeof(out)));
    ASSERT_FALSE(hyp_watched_origin(NULL, 0, out, sizeof(out)));
    ASSERT_FALSE(hyp_watched_thread(NULL, out, sizeof(out)));
    PASS();
}

TEST(scan_refuses_a_missing_root) {
    /* No root is a confident error, not an empty result: absent means "look
     * elsewhere", and a scan that reports zero rows from a root that is not
     * there would be claiming there is nothing to look at. */
    hyp_watched_scanner_t *s =
        hyp_watched_scanner_create("build/wingest-this-root-does-not-exist", identity_scrub, NULL);
    ASSERT_NOT_NULL(s);
    collector_t c;
    collector_init(&c);
    ASSERT_FALSE(hyp_watched_scan_once(s, collect_sink, &c, NULL));
    ASSERT_EQ(c.count, 0);
    ASSERT_FALSE(hyp_watched_scan_once(NULL, collect_sink, &c, NULL));
    ASSERT_FALSE(hyp_watched_scan_once(s, NULL, &c, NULL));
    hyp_watched_scanner_free(s);
    PASS();
}

SUITE(watched_ingest) {
    /* The scrub precondition */
    RUN_TEST(scanner_refuses_unscrubbed_ingest);
    RUN_TEST(scrub_runs_before_the_record_and_counts);
    /* The format */
    RUN_TEST(happy_path_rows_map_onto_the_record_contract);
    RUN_TEST(crlf_and_blank_lines_are_tolerated);
    RUN_TEST(null_and_absent_are_different_answers);
    /* Origin: positional, idempotent, audit-ready */
    RUN_TEST(origin_offsets_make_identical_rows_distinct);
    RUN_TEST(growth_yields_only_the_new_rows);
    RUN_TEST(timestamps_come_from_the_file);
    /* Partial writes */
    RUN_TEST(partial_last_line_is_not_yet);
    /* Failure: per file, named, non-blocking */
    RUN_TEST(a_bad_file_fails_closed_alone_and_named);
    RUN_TEST(a_failed_file_heals_when_fixed);
    RUN_TEST(every_refusal_is_named);
    RUN_TEST(oversize_is_refused_never_truncated);
    RUN_TEST(a_shrunk_file_is_refused_and_stays_refused);
    /* The sink boundary */
    RUN_TEST(sink_refusal_aborts_and_nothing_is_lost);
    /* Discovery */
    RUN_TEST(discovery_is_recursive_filtered_and_deterministic);
    RUN_TEST(derivation_helpers_refuse_truncation);
    RUN_TEST(scan_refuses_a_missing_root);
}
