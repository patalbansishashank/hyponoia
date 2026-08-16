/*
 * test_adr_records.c — the ADR document folded into the append-only record
 * set, and the writer that replaced the mutable one. Track C, unit C7u.
 *
 * The organising claim is IDEMPOTENCE, and it is the unit's acceptance:
 * folding the same ADR twice yields exactly one record set, asserted by
 * digest, on one machine and across two that never spoke. That property rests
 * entirely on where the timestamp comes from — the row's own `updated_at`,
 * never a clock read while folding — because the record id commits to it. A
 * clock read here would make the same document two records, and merging two
 * machines' stores would double the corpus instead of deduplicating it. The
 * planted control for this file reads a clock and watches the cross-machine
 * test fail.
 *
 * The second claim is that NOTHING IS LOST. Two updates leave two records and
 * the superseded text is still readable, where the mutable UPSERT left one row
 * and no way back. The project_summaries row survives as a PROJECTION: it is
 * refreshed from the record just appended, so re-folding the writer's own
 * output adds nothing — which is what distinguishes a projection from a second
 * store that can disagree.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <foundation/platform.h>
#include <memory/adr_records.h>
#include <store/record_store.h>
#include <store/store.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Golden instants, each checked against an independent renderer rather than
 * against this file's own parser: a self-consistent inverse can be wrong in
 * both directions at once. */
#define ADR_T_2026 INT64_C(1770985600000) /* 2026-02-13T12:26:40Z */
#define ADR_T_LEAP INT64_C(951868799000)  /* 2000-02-29T23:59:59Z, leap day */
#define ADR_T_EPOCH INT64_C(1000)         /* 1970-01-01T00:00:01Z */

#define ADR_ISO_2026 "2026-02-13T12:26:40Z"
#define ADR_ISO_LEAP "2000-02-29T23:59:59Z"
#define ADR_ISO_EPOCH "1970-01-01T00:00:01Z"

#define ADR_DOC_ONE "## PURPOSE\nOne store for decisions, append only.\n"
#define ADR_DOC_TWO "## PURPOSE\nOne store for decisions.\n\n## TRADEOFFS\nImmutability.\n"

/* ── Fixture: a project store holding one ADR with a KNOWN instant ──────── */

static hyp_store_t *adr_fixture_store(const char *project, const char *content,
                                      const char *updated_at) {
    hyp_store_t *store = hyp_store_open_memory();
    if (!store) {
        return NULL;
    }
    if (hyp_store_upsert_project(store, project, "/tmp/adr-records-fixture") != HYP_STORE_OK ||
        hyp_store_adr_store_at(store, project, content, updated_at) != HYP_STORE_OK) {
        hyp_store_close(store);
        return NULL;
    }
    return store;
}

/* The id a document MUST fold to, derived here from the same facts the row
 * holds. Comparing against this rather than against a second fold is what makes
 * "the instant comes from the row" a deterministic assertion: two folds a
 * millisecond apart can agree by accident, and a test that can pass for the
 * wrong reason is the failure mode a control exists to catch. */
static void adr_expected_id(const char *project, const char *content, int64_t ms,
                            char out[HYP_RECORD_ID_LEN + 1]) {
    const hyp_record_t *rec = NULL;
    out[0] = '\0';
    if (hyp_adr_record_build(project, content, ms, &rec) == HYP_RECORD_OK) {
        snprintf(out, HYP_RECORD_ID_LEN + 1, "%s", rec->id);
        hyp_record_free(rec);
    }
}

static hyp_record_store_t *adr_fixture_records(const char *tmp, const char *leaf) {
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s/%s", tmp, leaf);
    hyp_record_store_t *records = NULL;
    if (hyp_record_store_open(dir, &records) != HYP_RECORD_STORE_OK) {
        return NULL;
    }
    return records;
}

/* ── 1 · The instant encoding, which every id depends on ────────────────── */

TEST(adr_records_timestamp_encoding_is_an_exact_inverse) {
    struct {
        int64_t ms;
        const char *iso;
    } golden[] = {{ADR_T_2026, ADR_ISO_2026},
                  {ADR_T_LEAP, ADR_ISO_LEAP},
                  {ADR_T_EPOCH, ADR_ISO_EPOCH},
                  {INT64_C(4107542400000), "2100-03-01T00:00:00Z"},
                  {INT64_C(94694399000), "1972-12-31T23:59:59Z"},
                  {INT64_C(1709164800000), "2024-02-29T00:00:00Z"}};

    for (size_t i = 0; i < sizeof(golden) / sizeof(golden[0]); i++) {
        int64_t parsed = 0;
        ASSERT_TRUE(hyp_adr_timestamp_to_ms(golden[i].iso, &parsed));
        ASSERT_EQ(parsed, golden[i].ms);

        char rendered[HYP_ADR_TIMESTAMP_LEN];
        ASSERT_TRUE(hyp_adr_timestamp_from_ms(golden[i].ms, rendered, sizeof(rendered)));
        ASSERT_STR_EQ(rendered, golden[i].iso);
    }

    /* A shape that is not the stored encoding is refused, never guessed at:
     * an invented date would mint an id no other machine can reproduce. */
    int64_t ignored = 0;
    ASSERT_FALSE(hyp_adr_timestamp_to_ms(NULL, &ignored));
    ASSERT_FALSE(hyp_adr_timestamp_to_ms("", &ignored));
    ASSERT_FALSE(hyp_adr_timestamp_to_ms("2026-02-13T12:26:40", &ignored));
    ASSERT_FALSE(hyp_adr_timestamp_to_ms("2026-02-13 12:26:40Z", &ignored));
    ASSERT_FALSE(hyp_adr_timestamp_to_ms("2026-13-13T12:26:40Z", &ignored));
    ASSERT_FALSE(hyp_adr_timestamp_to_ms("not-a-timestamp-abcd", &ignored));
    ASSERT_FALSE(hyp_adr_timestamp_to_ms("0000-01-01T00:00:00Z", &ignored));

    /* Sub-second wall-clock readings floor rather than round, so a record and
     * the row projecting it always denote the same second. */
    ASSERT_EQ(hyp_adr_floor_to_second_ms(ADR_T_2026 + 999), ADR_T_2026);
    ASSERT_EQ(hyp_adr_floor_to_second_ms(ADR_T_2026), ADR_T_2026);
    PASS();
}

/* ── 2 · The mapping, field by field ────────────────────────────────────── */

TEST(adr_records_fold_maps_the_document_to_a_decision) {
    const hyp_record_t *rec = NULL;
    ASSERT_EQ(hyp_adr_record_build("hyponoia", ADR_DOC_ONE, ADR_T_2026, &rec), HYP_RECORD_OK);
    ASSERT_NOT_NULL(rec);

    ASSERT_EQ((int)rec->kind, (int)HYP_RECORD_DECISION);
    ASSERT_STR_EQ(rec->content, ADR_DOC_ONE); /* byte for byte, never split by section */
    ASSERT_STR_EQ(rec->origin, "hyponoia");   /* the ADR's project id */
    ASSERT_NULL(rec->anchor);                 /* about a project, not about a span */
    ASSERT_NULL(rec->thread);
    ASSERT_NULL(rec->parent);
    ASSERT_EQ(rec->redactions, 0);
    ASSERT_EQ(rec->timestamp_ms, ADR_T_2026); /* the row's own instant */
    ASSERT_STR_EQ(rec->author, HYP_ADR_RECORD_AUTHOR);
    ASSERT_TRUE(hyp_record_verify(rec));

    /* Determinism is the whole requirement: same inputs, same id, every time
     * and everywhere. Nothing about the machine enters the preimage. */
    const hyp_record_t *again = NULL;
    ASSERT_EQ(hyp_adr_record_build("hyponoia", ADR_DOC_ONE, ADR_T_2026, &again), HYP_RECORD_OK);
    ASSERT_STR_EQ(again->id, rec->id);
    hyp_record_free(again);

    /* Distinct projects hold distinct ADRs, so they never collapse. */
    const hyp_record_t *other = NULL;
    ASSERT_EQ(hyp_adr_record_build("other", ADR_DOC_ONE, ADR_T_2026, &other), HYP_RECORD_OK);
    ASSERT_TRUE(strcmp(other->id, rec->id) != 0);
    hyp_record_free(other);

    hyp_record_free(rec);
    PASS();
}

/* ── 2b · A folded ADR is an ORDINARY record, carrying no marker ────────── */

TEST(adr_records_folded_document_is_indistinguishable_from_any_other_record) {
    /* The assertion row this unit carries says no new memory surface may
     * depend on the deprecated tool. At the data layer that means a reader
     * needs no ADR vocabulary to find a folded ADR: the record the fold
     * produces must be byte-for-byte the record the plain contract produces
     * from the same facts. A namespace prefix, a marker in the content, a
     * synthetic anchor — any of them would make the memory surface learn what
     * an ADR is in order to read one. */
    hyp_record_input_t plain;
    memset(&plain, 0, sizeof(plain));
    plain.kind = HYP_RECORD_DECISION;
    plain.author = HYP_ADR_RECORD_AUTHOR;
    plain.timestamp_ms = ADR_T_2026;
    plain.content = ADR_DOC_ONE;
    plain.origin = "hyponoia";

    const hyp_record_t *by_contract = NULL;
    ASSERT_EQ(hyp_record_build(&plain, &by_contract), HYP_RECORD_OK);

    const hyp_record_t *by_fold = NULL;
    ASSERT_EQ(hyp_adr_record_build("hyponoia", ADR_DOC_ONE, ADR_T_2026, &by_fold), HYP_RECORD_OK);

    ASSERT_TRUE(hyp_record_equal(by_contract, by_fold));
    ASSERT_STR_EQ(by_contract->id, by_fold->id);

    hyp_record_free(by_contract);
    hyp_record_free(by_fold);
    PASS();
}

/* ── 3 · ACCEPTANCE: folding twice yields one record set ────────────────── */

TEST(adr_records_folding_twice_yields_one_record_set) {
    const char *made = th_mktempdir("hyp_adr_idem");
    ASSERT_NOT_NULL(made);
    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s", made);

    hyp_store_t *store = adr_fixture_store("proj", ADR_DOC_ONE, ADR_ISO_2026);
    ASSERT_NOT_NULL(store);
    hyp_record_store_t *records = adr_fixture_records(tmp, "records");
    ASSERT_NOT_NULL(records);

    hyp_adr_fold_result_t first = {0};
    ASSERT_EQ(hyp_adr_fold_store(store, records, &first), HYP_RECORD_STORE_OK);
    ASSERT_EQ(first.documents, 1);
    ASSERT_EQ(first.added, 1);
    ASSERT_EQ(first.present, 0);
    ASSERT_EQ(first.refused, 0);

    /* The one record is the one the row's own instant produces. Asserted
     * against an independently derived id, not against a second fold. */
    char expected[HYP_RECORD_ID_LEN + 1];
    adr_expected_id("proj", ADR_DOC_ONE, ADR_T_2026, expected);
    ASSERT_TRUE(expected[0] != '\0');
    const hyp_record_t *stored = NULL;
    ASSERT_EQ(hyp_record_store_get(records, expected, &stored), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(stored);
    hyp_record_free(stored);

    char digest_after_first[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records, digest_after_first), HYP_RECORD_STORE_OK);

    hyp_adr_fold_result_t second = {0};
    ASSERT_EQ(hyp_adr_fold_store(store, records, &second), HYP_RECORD_STORE_OK);
    ASSERT_EQ(second.documents, 1);
    ASSERT_EQ(second.added, 0);   /* the union absorbing a duplicate */
    ASSERT_EQ(second.present, 1); /* named, not merely counted as zero */

    size_t count = 0;
    ASSERT_EQ(hyp_record_store_count(records, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 1);

    char digest_after_second[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records, digest_after_second), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_after_second, digest_after_first);

    hyp_record_store_close(records);
    hyp_store_close(store);
    (void)th_rmtree(tmp);
    PASS();
}

/* ── 4 · Two machines fold to the same set, and merging them adds nothing ── */

TEST(adr_records_two_machines_fold_to_the_same_records) {
    const char *made = th_mktempdir("hyp_adr_twomach");
    ASSERT_NOT_NULL(made);
    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s", made);

    /* Two machines holding the same ADR row. They never speak; the only thing
     * they share is the document and its stored instant. */
    hyp_store_t *store_a = adr_fixture_store("proj", ADR_DOC_ONE, ADR_ISO_2026);
    hyp_store_t *store_b = adr_fixture_store("proj", ADR_DOC_ONE, ADR_ISO_2026);
    ASSERT_NOT_NULL(store_a);
    ASSERT_NOT_NULL(store_b);

    hyp_record_store_t *records_a = adr_fixture_records(tmp, "a");
    hyp_record_store_t *records_b = adr_fixture_records(tmp, "b");
    ASSERT_NOT_NULL(records_a);
    ASSERT_NOT_NULL(records_b);

    ASSERT_EQ(hyp_adr_fold_store(store_a, records_a, NULL), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_adr_fold_store(store_b, records_b, NULL), HYP_RECORD_STORE_OK);

    /* Each machine holds THE record the row's instant produces — checked
     * against an id derived from the row, so a fold that read a clock fails
     * here whether or not the two machines happened to read the same
     * millisecond. Comparing the two stores to each other alone would let a
     * clock-reading fold pass by coincidence. */
    char expected[HYP_RECORD_ID_LEN + 1];
    adr_expected_id("proj", ADR_DOC_ONE, ADR_T_2026, expected);
    ASSERT_TRUE(expected[0] != '\0');
    const hyp_record_t *in_a = NULL;
    const hyp_record_t *in_b = NULL;
    ASSERT_EQ(hyp_record_store_get(records_a, expected, &in_a), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_get(records_b, expected, &in_b), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(in_a);
    ASSERT_NOT_NULL(in_b);
    ASSERT_TRUE(hyp_record_equal(in_a, in_b));
    hyp_record_free(in_a);
    hyp_record_free(in_b);

    char digest_a[HYP_RECORD_ID_LEN + 1];
    char digest_b[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records_a, digest_a), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_store_digest(records_b, digest_b), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_a, digest_b);

    /* Merging is the real test: a machine-dependent field would show up here
     * as two records where the corpus has one. */
    hyp_record_set_t *from_b = NULL;
    ASSERT_EQ(hyp_record_store_load(records_b, &from_b), HYP_RECORD_STORE_OK);
    size_t added = 99;
    ASSERT_EQ(hyp_record_store_append_set(records_a, from_b, &added), HYP_RECORD_STORE_OK);
    ASSERT_EQ(added, 0);
    hyp_record_set_free(from_b);

    size_t merged_count = 0;
    ASSERT_EQ(hyp_record_store_count(records_a, &merged_count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(merged_count, 1);

    char digest_after_merge[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records_a, digest_after_merge), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_after_merge, digest_a);

    hyp_record_store_close(records_a);
    hyp_record_store_close(records_b);
    hyp_store_close(store_a);
    hyp_store_close(store_b);
    (void)th_rmtree(tmp);
    PASS();
}

/* ── 5 · An ADR whose project row is gone is still a decision ───────────── */

TEST(adr_records_fold_covers_a_document_the_projects_table_forgot) {
    const char *made = th_mktempdir("hyp_adr_orphan");
    ASSERT_NOT_NULL(made);
    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s", made);

    hyp_store_t *store = adr_fixture_store("deleted-proj", ADR_DOC_ONE, ADR_ISO_2026);
    ASSERT_NOT_NULL(store);

    /* Deleting a project leaves the ADR row standing — the tables are not
     * linked. Enumerating projects to find documents would silently skip this
     * one, and a decision skipped by a fold is a decision lost. */
    ASSERT_EQ(hyp_store_delete_project(store, "deleted-proj"), HYP_STORE_OK);
    hyp_adr_t still_there;
    memset(&still_there, 0, sizeof(still_there));
    ASSERT_EQ(hyp_store_adr_get(store, "deleted-proj", &still_there), HYP_STORE_OK);
    hyp_store_adr_free(&still_there);

    hyp_record_store_t *records = adr_fixture_records(tmp, "records");
    ASSERT_NOT_NULL(records);
    hyp_adr_fold_result_t result = {0};
    ASSERT_EQ(hyp_adr_fold_store(store, records, &result), HYP_RECORD_STORE_OK);
    ASSERT_EQ(result.documents, 1);
    ASSERT_EQ(result.added, 1);

    hyp_record_store_close(records);
    hyp_store_close(store);
    (void)th_rmtree(tmp);
    PASS();
}

/* ── 6 · A row with no readable instant is refused, not invented ────────── */

TEST(adr_records_refuses_a_document_whose_instant_cannot_be_read) {
    const char *made = th_mktempdir("hyp_adr_badtime");
    ASSERT_NOT_NULL(made);
    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s", made);

    hyp_store_t *store = adr_fixture_store("proj", ADR_DOC_ONE, "whenever it was");
    ASSERT_NOT_NULL(store);
    hyp_record_store_t *records = adr_fixture_records(tmp, "records");
    ASSERT_NOT_NULL(records);

    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records, before), HYP_RECORD_STORE_OK);

    hyp_adr_fold_result_t result = {0};
    ASSERT_EQ(hyp_adr_fold_store(store, records, &result), HYP_RECORD_STORE_OK);
    ASSERT_EQ(result.documents, 1);
    ASSERT_EQ(result.refused, 1);
    ASSERT_EQ(result.added, 0);

    /* Refused means nothing was written, not "written with a plausible date". */
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records, after), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(after, before);

    /* And the row is untouched: refusing to fold never destroys the document. */
    hyp_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(hyp_store_adr_get(store, "proj", &adr), HYP_STORE_OK);
    ASSERT_STR_EQ(adr.content, ADR_DOC_ONE);
    hyp_store_adr_free(&adr);

    hyp_record_store_close(records);
    hyp_store_close(store);
    (void)th_rmtree(tmp);
    PASS();
}

/* ── 7 · The writer appends; it never replaces ──────────────────────────── */

TEST(adr_records_writer_keeps_every_superseded_document) {
    const char *made = th_mktempdir("hyp_adr_writer");
    ASSERT_NOT_NULL(made);
    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s", made);

    hyp_store_t *store = adr_fixture_store("proj", ADR_DOC_ONE, ADR_ISO_2026);
    ASSERT_NOT_NULL(store);
    hyp_record_store_t *records = adr_fixture_records(tmp, "records");
    ASSERT_NOT_NULL(records);

    /* A write one hour after the document already on the row. */
    ASSERT_EQ(hyp_adr_write(store, records, "proj", ADR_DOC_TWO, ADR_T_2026 + INT64_C(3600000)),
              HYP_STORE_OK);

    /* Two records: the document that was superseded AND the one that
     * superseded it. The mutable UPSERT left exactly one and no way back. */
    size_t count = 0;
    ASSERT_EQ(hyp_record_store_count(records, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 2);

    hyp_record_store_query_t query;
    memset(&query, 0, sizeof(query));
    query.has_kind = true;
    query.kind = HYP_RECORD_DECISION;
    query.origin = "proj";
    hyp_record_set_t *found = NULL;
    ASSERT_EQ(hyp_record_store_query(records, &query, &found), HYP_RECORD_STORE_OK);
    ASSERT_EQ(hyp_record_set_count(found), 2);

    bool saw_first = false;
    bool saw_second = false;
    for (size_t i = 0; i < hyp_record_set_count(found); i++) {
        const hyp_record_t *rec = hyp_record_set_at(found, i);
        ASSERT_NULL(rec->anchor);
        if (strcmp(rec->content, ADR_DOC_ONE) == 0) {
            saw_first = true;
            ASSERT_EQ(rec->timestamp_ms, ADR_T_2026);
        } else if (strcmp(rec->content, ADR_DOC_TWO) == 0) {
            saw_second = true;
            ASSERT_EQ(rec->timestamp_ms, ADR_T_2026 + INT64_C(3600000));
        }
    }
    ASSERT_TRUE(saw_first);
    ASSERT_TRUE(saw_second);
    hyp_record_set_free(found);

    /* The projection holds the current document, which is what the UI and
     * mode:"get" read — unchanged bytes, backed by a record that cannot be
     * overwritten. */
    hyp_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(hyp_store_adr_get(store, "proj", &adr), HYP_STORE_OK);
    ASSERT_STR_EQ(adr.content, ADR_DOC_TWO);
    hyp_store_adr_free(&adr);

    hyp_record_store_close(records);
    hyp_store_close(store);
    (void)th_rmtree(tmp);
    PASS();
}

/* ── 8 · The projection is a projection, not a second store ─────────────── */

TEST(adr_records_refolding_the_writers_own_output_adds_nothing) {
    const char *made = th_mktempdir("hyp_adr_proj");
    ASSERT_NOT_NULL(made);
    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s", made);

    hyp_store_t *store = hyp_store_open_memory();
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(hyp_store_upsert_project(store, "proj", "/tmp/adr-records-projection"),
              HYP_STORE_OK);
    hyp_record_store_t *records = adr_fixture_records(tmp, "records");
    ASSERT_NOT_NULL(records);

    ASSERT_EQ(hyp_adr_write(store, records, "proj", ADR_DOC_ONE, ADR_T_2026 + 137), HYP_STORE_OK);

    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records, before), HYP_RECORD_STORE_OK);

    /* Reading the row back and folding it must rebuild the identical record.
     * If it did not, the row would be carrying information the record set does
     * not have, which is the definition of a second store. */
    hyp_adr_fold_result_t result = {0};
    ASSERT_EQ(hyp_adr_fold_store(store, records, &result), HYP_RECORD_STORE_OK);
    ASSERT_EQ(result.documents, 1);
    ASSERT_EQ(result.added, 0);
    ASSERT_EQ(result.present, 1);

    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records, after), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(after, before);

    hyp_record_store_close(records);
    hyp_store_close(store);
    (void)th_rmtree(tmp);
    PASS();
}

/* ── 8b · A reindex copies the row; it does not author one ──────────────── */

TEST(adr_records_a_rebuilt_generation_folds_to_the_same_record) {
    const char *made = th_mktempdir("hyp_adr_rebuild");
    ASSERT_NOT_NULL(made);
    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s", made);

    /* Publication REPLACES the database file, so the ADR row is copied into
     * the new generation rather than surviving in place. A copy that restamped
     * the instant would make the same document a second decision record after
     * every reindex — and two machines that reindexed at different moments
     * would hold two records for one document and never deduplicate. This is
     * the property the copy has to have, asserted through the same store
     * primitive publication uses. */
    hyp_store_t *before = adr_fixture_store("proj", ADR_DOC_ONE, ADR_ISO_2026);
    ASSERT_NOT_NULL(before);

    hyp_adr_t carried;
    memset(&carried, 0, sizeof(carried));
    ASSERT_EQ(hyp_store_adr_get(before, "proj", &carried), HYP_STORE_OK);
    ASSERT_STR_EQ(carried.updated_at, ADR_ISO_2026);

    hyp_store_t *rebuilt = hyp_store_open_memory();
    ASSERT_NOT_NULL(rebuilt);
    ASSERT_EQ(hyp_store_upsert_project(rebuilt, "proj", "/tmp/adr-records-fixture"), HYP_STORE_OK);
    ASSERT_EQ(hyp_store_adr_store_at(rebuilt, "proj", carried.content, carried.updated_at),
              HYP_STORE_OK);
    hyp_store_adr_free(&carried);

    hyp_record_store_t *records = adr_fixture_records(tmp, "records");
    ASSERT_NOT_NULL(records);
    ASSERT_EQ(hyp_adr_fold_store(before, records, NULL), HYP_RECORD_STORE_OK);
    char digest_before[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records, digest_before), HYP_RECORD_STORE_OK);

    hyp_adr_fold_result_t after_rebuild = {0};
    ASSERT_EQ(hyp_adr_fold_store(rebuilt, records, &after_rebuild), HYP_RECORD_STORE_OK);
    ASSERT_EQ(after_rebuild.documents, 1);
    ASSERT_EQ(after_rebuild.added, 0);
    ASSERT_EQ(after_rebuild.present, 1);

    size_t count = 0;
    ASSERT_EQ(hyp_record_store_count(records, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ(count, 1);
    char digest_after[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_store_digest(records, digest_after), HYP_RECORD_STORE_OK);
    ASSERT_STR_EQ(digest_after, digest_before);

    hyp_record_store_close(records);
    hyp_store_close(rebuilt);
    hyp_store_close(before);
    (void)th_rmtree(tmp);
    PASS();
}

/* ── 9 · No record store, no write. There is no fallback path ───────────── */

TEST(adr_records_write_fails_closed_without_a_record_store) {
    hyp_store_t *store = adr_fixture_store("proj", ADR_DOC_ONE, ADR_ISO_2026);
    ASSERT_NOT_NULL(store);

    ASSERT_EQ(hyp_adr_write(store, NULL, "proj", ADR_DOC_TWO, ADR_T_2026), HYP_STORE_ERR);

    /* The document on the row is the one that was there. A write that could
     * not become a record did not happen at all — replacing the row anyway is
     * the mutable path with a different entry condition. */
    hyp_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(hyp_store_adr_get(store, "proj", &adr), HYP_STORE_OK);
    ASSERT_STR_EQ(adr.content, ADR_DOC_ONE);
    hyp_store_adr_free(&adr);

    hyp_store_close(store);
    PASS();
}

/* ── 10 · Where the records live, and how a test keeps off a real corpus ── */

TEST(adr_records_default_location_follows_the_cache_dir) {
    const char *made = th_mktempdir("hyp_adr_home");
    ASSERT_NOT_NULL(made);
    char tmp[256];
    (void)snprintf(tmp, sizeof(tmp), "%s", made);

    const char *saved = getenv("HYP_CACHE_DIR");
    char *saved_copy = saved ? hyp_strdup(saved) : NULL;
    ASSERT_EQ(hyp_setenv("HYP_CACHE_DIR", tmp, 1), 0);

    hyp_record_store_t *records = NULL;
    hyp_record_store_status_t status = hyp_adr_records_open(NULL, &records);

    char expected[512];
    (void)snprintf(expected, sizeof(expected), "%s/memory/records.db", tmp);
    bool exists = hyp_file_exists(expected);

    if (records) {
        hyp_record_store_close(records);
    }
    if (saved_copy) {
        (void)hyp_setenv("HYP_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        (void)hyp_unsetenv("HYP_CACHE_DIR");
    }
    (void)th_rmtree(tmp);

    ASSERT_EQ(status, HYP_RECORD_STORE_OK);
    ASSERT_TRUE(exists);
    PASS();
}

SUITE(adr_records) {
    RUN_TEST(adr_records_timestamp_encoding_is_an_exact_inverse);
    RUN_TEST(adr_records_fold_maps_the_document_to_a_decision);
    RUN_TEST(adr_records_folded_document_is_indistinguishable_from_any_other_record);
    RUN_TEST(adr_records_folding_twice_yields_one_record_set);
    RUN_TEST(adr_records_two_machines_fold_to_the_same_records);
    RUN_TEST(adr_records_fold_covers_a_document_the_projects_table_forgot);
    RUN_TEST(adr_records_refuses_a_document_whose_instant_cannot_be_read);
    RUN_TEST(adr_records_writer_keeps_every_superseded_document);
    RUN_TEST(adr_records_refolding_the_writers_own_output_adds_nothing);
    RUN_TEST(adr_records_a_rebuilt_generation_folds_to_the_same_record);
    RUN_TEST(adr_records_write_fails_closed_without_a_record_store);
    RUN_TEST(adr_records_default_location_follows_the_cache_dir);
}
