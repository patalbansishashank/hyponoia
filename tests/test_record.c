/*
 * test_record.c — the append-only record contract (C2).
 *
 * The suite is organised by the properties the contract claims, and every
 * property has a control that says NO. A test that only ever demonstrates the
 * happy path proves that the code runs, not that it is right: the id-derivation
 * tests are paired with per-field controls that must change the id, the
 * validator's refusals are paired with the same input accepted once the bad
 * field is fixed, and the set digest is shown to distinguish sets before it is
 * used to prove two merges agree.
 *
 * Where possible the assertion is on what a CLIENT reads — the enumeration, the
 * digest, the fields it gets back — rather than on an internal the client never
 * sees.
 */
#include "test_framework.h"

#include "../src/foundation/record.h"

#include <stdint.h>
#include <string.h>

/* ── Fixtures ───────────────────────────────────────────────────────────── */

/* 2026-02-14T12:26:40.000Z — a fixed, caller-supplied instant. Nothing in this
 * suite reads a clock to build a record, which is the point. */
#define FIXED_MS INT64_C(1770985600000)

static hyp_record_input_t base_input(void) {
    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_DECISION;
    in.author = "agent:c2";
    in.timestamp_ms = FIXED_MS;
    in.content = "merging two stores is a union, so there is nothing to resolve";
    in.anchor = "opaque-address-from-the-identity-contract";
    in.origin = "feed:row-91";
    in.thread = "conversation-7";
    in.parent = NULL;
    in.redactions = 0;
    return in;
}

static int build_ok(const hyp_record_input_t *in, const hyp_record_t **out) {
    return hyp_record_build(in, out) == HYP_RECORD_OK && *out != NULL;
}

/* ── Kinds and statuses ─────────────────────────────────────────────────── */

TEST(record_kind_names_round_trip) {
    for (int i = 0; i < (int)HYP_RECORD_KIND_COUNT; i++) {
        const char *name = hyp_record_kind_name((hyp_record_kind_t)i);
        ASSERT_NOT_NULL(name);
        hyp_record_kind_t back;
        ASSERT_TRUE(hyp_record_kind_from_name(name, &back));
        ASSERT_EQ((int)back, i);
    }
    /* The control: an unknown kind is refused, never resolved to a neighbour. */
    hyp_record_kind_t back;
    ASSERT_FALSE(hyp_record_kind_from_name("task_message", &back));
    ASSERT_FALSE(hyp_record_kind_from_name("", &back));
    ASSERT_NULL(hyp_record_kind_name(HYP_RECORD_KIND_COUNT));
    PASS();
}

TEST(record_status_reasons_are_present_and_distinct) {
    /* Every status must say something, and no two may say the same thing — a
     * shared string is how two different refusals become one indistinguishable
     * error message in a log. */
    const hyp_record_status_t all[] = {
        HYP_RECORD_OK,         HYP_RECORD_ERR_NULL,      HYP_RECORD_ERR_KIND,
        HYP_RECORD_ERR_AUTHOR, HYP_RECORD_ERR_TIMESTAMP, HYP_RECORD_ERR_CONTENT,
        HYP_RECORD_ERR_ANCHOR, HYP_RECORD_ERR_ORIGIN,    HYP_RECORD_ERR_THREAD,
        HYP_RECORD_ERR_PARENT, HYP_RECORD_ERR_TAMPERED,  HYP_RECORD_ERR_COLLISION,
        HYP_RECORD_ERR_ALLOC};
    size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const char *reason = hyp_record_status_reason(all[i]);
        ASSERT_NOT_NULL(reason);
        ASSERT_TRUE(reason[0] != '\0');
        for (size_t j = i + 1; j < n; j++) {
            ASSERT_STR_NEQ(reason, hyp_record_status_reason(all[j]));
        }
    }
    PASS();
}

/* ── Identity: derived from content, on any machine, with no coordination ── */

TEST(record_id_is_deterministic) {
    hyp_record_input_t in = base_input();

    const hyp_record_t *a = NULL;
    const hyp_record_t *b = NULL;
    ASSERT_TRUE(build_ok(&in, &a));
    ASSERT_TRUE(build_ok(&in, &b));
    ASSERT_STR_EQ(a->id, b->id);
    ASSERT_EQ((int)strlen(a->id), HYP_RECORD_ID_LEN);
    ASSERT_TRUE(hyp_record_id_is_valid(a->id));

    /* Deriving the id without building must agree with building — one
     * derivation, not two that could drift. */
    char derived[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_derive_id(&in, derived), HYP_RECORD_OK);
    ASSERT_STR_EQ(derived, a->id);

    /* Strings are copied, not borrowed: the same content in a different buffer
     * is the same record. This is what makes two machines agree — they never
     * share a pointer, only bytes. */
    char content_copy[128];
    snprintf(content_copy, sizeof(content_copy), "%s", in.content);
    hyp_record_input_t elsewhere = in;
    elsewhere.content = content_copy;
    const hyp_record_t *c = NULL;
    ASSERT_TRUE(build_ok(&elsewhere, &c));
    ASSERT_STR_EQ(c->id, a->id);

    hyp_record_free(a);
    hyp_record_free(b);
    hyp_record_free(c);
    PASS();
}

/*
 * The golden vector. Every claim about the encoding — the domain tag, the
 * length prefixes, the field names, the kind spelled out rather than numbered —
 * is invisible to every other test in this file, because they all compare ids
 * computed by the same code. This one pins the bytes. If it changes, every
 * record ever written has a new id, and that must never happen by accident.
 */
TEST(record_id_golden_vector) {
    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_MESSAGE;
    in.author = "agent:golden";
    in.timestamp_ms = INT64_C(1700000000000);
    in.content = "hello";
    in.anchor = NULL;
    in.origin = NULL;
    in.thread = NULL;
    in.parent = NULL;
    in.redactions = 0;

    char id[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_derive_id(&in, id), HYP_RECORD_OK);
    /* Independently re-derived from the description in record.h, by a second
     * implementation of the encoding that shares no code with this one —
     * tests/test_record_contract.sh recomputes it and fails if the two ends
     * disagree. A golden value produced only by the code under test would pin
     * whatever that code does, bug included. */
    ASSERT_STR_EQ(id, "8efb33963a34bb9656076bdc02a51b8b11547b4e2555d147f680ec0d5af4bfd9");
    PASS();
}

/*
 * Per-field controls for the id. content + author + timestamp are the substance
 * of identity, but the id commits to EVERY field — because if it did not, two
 * records differing only in (say) anchor would collide, and a union keyed on id
 * would hold one id and two payloads with no way out but picking one. Picking
 * one is last-writer-wins, which this contract does not have.
 *
 * tests/test_record_contract.sh derives the field list from record.h and fails
 * if any field lacks its marker below, so a tenth field cannot be added without
 * a control that proves the id notices it.
 */
TEST(record_id_commits_to_every_field) {
    hyp_record_input_t base = base_input();
    char base_id[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_derive_id(&base, base_id), HYP_RECORD_OK);

    hyp_record_input_t v;
    char id[HYP_RECORD_ID_LEN + 1];

    /* id-sensitivity: kind */
    v = base;
    v.kind = HYP_RECORD_SIGNAL;
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    /* id-sensitivity: author */
    v = base;
    v.author = "agent:somebody-else";
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    /* id-sensitivity: timestamp_ms */
    v = base;
    v.timestamp_ms = base.timestamp_ms + 1;
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    /* id-sensitivity: content */
    v = base;
    v.content = "merging two stores is a union, so there is nothing to resolve.";
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    /* id-sensitivity: anchor */
    v = base;
    v.anchor = "a different opaque address";
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    /* id-sensitivity: origin */
    v = base;
    v.origin = "feed:row-92";
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    /* id-sensitivity: thread */
    v = base;
    v.thread = "conversation-8";
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    /* id-sensitivity: parent */
    v = base;
    v.parent = "0000000000000000000000000000000000000000000000000000000000000000";
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    /* id-sensitivity: redactions */
    v = base;
    v.redactions = 1;
    ASSERT_EQ(hyp_record_derive_id(&v, id), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id, base_id);

    PASS();
}

TEST(record_id_distinguishes_absent_from_present) {
    /* Absent means "look elsewhere". The encoding gives it its own byte, so a
     * record with no anchor can never hash to one that has any anchor at all —
     * including one whose value happens to be adjacent in the preimage. */
    hyp_record_input_t with = base_input();
    hyp_record_input_t without = with;
    without.anchor = NULL;

    char id_with[HYP_RECORD_ID_LEN + 1];
    char id_without[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_derive_id(&with, id_with), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_derive_id(&without, id_without), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id_with, id_without);

    /* And a field boundary cannot be crossed: moving bytes from one opaque
     * field into the next is a different record, not the same one. */
    hyp_record_input_t shifted = base_input();
    shifted.origin = "feed:row-9";
    shifted.thread = "1conversation-7";
    char id_shifted[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_derive_id(&shifted, id_shifted), HYP_RECORD_OK);
    ASSERT_STR_NEQ(id_shifted, id_with);
    PASS();
}

/* ── Re-ingest: the same source row twice is one record ─────────────────── */

TEST(reingest_of_one_row_is_one_record) {
    /* A feed adapter re-runs. Same row, same source time, same author, same
     * origin — so the same id, so a union of one. No "already seen" table is
     * consulted, and none has to be: idempotence is a property of the id. */
    hyp_record_input_t row;
    memset(&row, 0, sizeof(row));
    row.kind = HYP_RECORD_MESSAGE;
    row.author = "agent:ingest";
    row.timestamp_ms = INT64_C(1699999999999);
    row.content = "ran the suite; 12 failures hid behind 1";
    row.origin = "feed:message:5150";
    row.thread = "feed:task:88";

    const hyp_record_t *first = NULL;
    const hyp_record_t *second = NULL;
    ASSERT_TRUE(build_ok(&row, &first));
    ASSERT_TRUE(build_ok(&row, &second));
    ASSERT_STR_EQ(first->id, second->id);

    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    bool added = false;
    ASSERT_EQ(hyp_record_set_add(store, first, &added), HYP_RECORD_OK);
    ASSERT_TRUE(added);
    ASSERT_EQ(hyp_record_set_add(store, second, &added), HYP_RECORD_OK);
    ASSERT_FALSE(added); /* absorbed, not an error, not a second row */
    ASSERT_EQ(hyp_record_set_count(store), 1);

    hyp_record_free(first);
    hyp_record_free(second);
    hyp_record_set_free(store);
    PASS();
}

TEST(distinct_source_rows_stay_distinct) {
    /*
     * The reason `origin` exists, stated as a test.
     *
     * Two different rows of a feed can carry byte-identical text, by the same
     * author, at the same millisecond — a repeated tool result is the obvious
     * case. Without the source's own identity in the record they collapse into
     * one, and then a completeness audit comparing stored records against the
     * source's count(*) reports a permanent shortfall that is indistinguishable
     * from real loss. Dedup and loss look the same from a count.
     */
    hyp_record_input_t row;
    memset(&row, 0, sizeof(row));
    row.kind = HYP_RECORD_MESSAGE;
    row.author = "agent:ingest";
    row.timestamp_ms = INT64_C(1699999999999);
    row.content = "ok";
    row.thread = "feed:task:88";

    hyp_record_input_t row_a = row;
    row_a.origin = "feed:message:5150";
    hyp_record_input_t row_b = row;
    row_b.origin = "feed:message:5151";

    const hyp_record_t *a = NULL;
    const hyp_record_t *b = NULL;
    ASSERT_TRUE(build_ok(&row_a, &a));
    ASSERT_TRUE(build_ok(&row_b, &b));
    ASSERT_STR_NEQ(a->id, b->id);

    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(hyp_record_set_add(store, a, NULL), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_add(store, b, NULL), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_count(store), 2); /* the audit can count these */

    /* The control, and the hazard it documents: drop the source identity and
     * the two rows really do become one record. This is not a bug to fix here —
     * it is why an adapter must supply an origin. */
    hyp_record_input_t anon_a = row_a;
    hyp_record_input_t anon_b = row_b;
    anon_a.origin = NULL;
    anon_b.origin = NULL;
    char id_a[HYP_RECORD_ID_LEN + 1];
    char id_b[HYP_RECORD_ID_LEN + 1];
    ASSERT_EQ(hyp_record_derive_id(&anon_a, id_a), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_derive_id(&anon_b, id_b), HYP_RECORD_OK);
    ASSERT_STR_EQ(id_a, id_b);

    hyp_record_free(a);
    hyp_record_free(b);
    hyp_record_set_free(store);
    PASS();
}

TEST(build_never_reads_a_clock) {
    /*
     * Asserted from the client's side, not by reading the source: build the
     * same input, let real time pass, build again, and require the same id.
     * A captured timestamp would make these two differ — and would make two
     * machines disagree about the same event, which turns sync into a duplicate
     * storm instead of a union.
     */
    hyp_record_input_t in = base_input();
    const hyp_record_t *before = NULL;
    ASSERT_TRUE(build_ok(&in, &before));

    int64_t started = hyp_record_wall_clock_ms();
    ASSERT_GT(started, INT64_C(1577836800000)); /* after 2020-01-01 */
    ASSERT_LTE(started, HYP_RECORD_MAX_TIMESTAMP_MS);
    int64_t now = started;
    for (int spins = 0; spins < 200000000 && now == started; spins++) {
        now = hyp_record_wall_clock_ms();
    }
    ASSERT_GT(now, started); /* the clock really did move */

    const hyp_record_t *after = NULL;
    ASSERT_TRUE(build_ok(&in, &after));
    ASSERT_STR_EQ(after->id, before->id);
    ASSERT_EQ(after->timestamp_ms, FIXED_MS);

    hyp_record_free(before);
    hyp_record_free(after);
    PASS();
}

/* ── Immutability and tamper evidence ───────────────────────────────────── */

TEST(record_verify_accepts_an_untouched_record) {
    /* The positive control for the tamper test below: a verifier that always
     * said NO would pass that test and mean nothing. */
    hyp_record_input_t in = base_input();
    const hyp_record_t *rec = NULL;
    ASSERT_TRUE(build_ok(&in, &rec));
    ASSERT_TRUE(hyp_record_verify(rec));

    /* Every field a client reads is the field it supplied. */
    ASSERT_EQ((int)rec->kind, (int)in.kind);
    ASSERT_STR_EQ(rec->author, in.author);
    ASSERT_EQ(rec->timestamp_ms, in.timestamp_ms);
    ASSERT_STR_EQ(rec->content, in.content);
    ASSERT_STR_EQ(rec->anchor, in.anchor);
    ASSERT_STR_EQ(rec->origin, in.origin);
    ASSERT_STR_EQ(rec->thread, in.thread);
    ASSERT_NULL(rec->parent);
    ASSERT_EQ(rec->redactions, in.redactions);

    hyp_record_free(rec);
    PASS();
}

TEST(record_verify_detects_a_mutation) {
    /*
     * The compiler already refuses `rec->content = x` and `*rec = other` —
     * every member is const, so a record is not assignable. That is checked in
     * tests/test_record_contract.sh, which tries to compile both and requires
     * the compiler to reject them.
     *
     * This is the backstop for the case the compiler cannot see: someone who
     * casts the const away and writes through the pointer anyway. The id is a
     * digest of the fields, so the edit is detectable rather than silent.
     */
    hyp_record_input_t in = base_input();
    const hyp_record_t *rec = NULL;
    ASSERT_TRUE(build_ok(&in, &rec));
    ASSERT_TRUE(hyp_record_verify(rec));

    char *content = NULL;
    memcpy(&content, &rec->content, sizeof(content));
    char original = content[0];
    content[0] = (char)(original ^ 0x20);
    ASSERT_FALSE(hyp_record_verify(rec)); /* caught */

    content[0] = original;
    ASSERT_TRUE(hyp_record_verify(rec)); /* and it was that byte, nothing else */

    hyp_record_free(rec);
    PASS();
}

TEST(set_refuses_a_tampered_record) {
    /* A mutated record cannot enter a store. This is the property C6u relies on
     * when it takes records from a peer it did not build. */
    hyp_record_input_t in = base_input();
    const hyp_record_t *rec = NULL;
    ASSERT_TRUE(build_ok(&in, &rec));

    hyp_record_set_t *set = hyp_record_set_create();
    ASSERT_NOT_NULL(set);

    char *anchor = NULL;
    memcpy(&anchor, &rec->anchor, sizeof(anchor));
    char original = anchor[0];
    anchor[0] = (char)(original ^ 0x20);

    ASSERT_EQ(hyp_record_set_add(set, rec, NULL), HYP_RECORD_ERR_TAMPERED);
    ASSERT_EQ(hyp_record_set_count(set), 0);

    /* Control: restore the byte and the very same record is accepted. */
    anchor[0] = original;
    ASSERT_EQ(hyp_record_set_add(set, rec, NULL), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_count(set), 1);

    hyp_record_free(rec);
    hyp_record_set_free(set);
    PASS();
}

/* ── Fail closed: what the validator refuses ────────────────────────────── */

TEST(build_refuses_malformed_input) {
    hyp_record_input_t good = base_input();
    const hyp_record_t *rec = NULL;

    /* The control first: the fixture itself is accepted, so the refusals below
     * are about the field that was broken and not about the fixture. */
    ASSERT_EQ(hyp_record_build(&good, &rec), HYP_RECORD_OK);
    hyp_record_free(rec);
    rec = NULL;

    ASSERT_EQ(hyp_record_build(NULL, &rec), HYP_RECORD_ERR_NULL);
    ASSERT_NULL(rec);

    hyp_record_input_t bad = good;
    bad.kind = HYP_RECORD_KIND_COUNT;
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_ERR_KIND);
    ASSERT_NULL(rec);

    bad = good;
    bad.author = NULL;
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_ERR_AUTHOR);
    bad.author = "";
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_ERR_AUTHOR);

    bad = good;
    bad.content = NULL;
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_ERR_CONTENT);
    bad.content = "";
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_ERR_CONTENT);

    /* Present-but-empty is refused on every optional field: it is what a
     * half-filled struct produces, and "anchored to nothing" is not a statement
     * anyone means to make. Absent is spelled NULL. */
    bad = good;
    bad.anchor = "";
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_ERR_ANCHOR);
    bad = good;
    bad.origin = "";
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_ERR_ORIGIN);
    bad = good;
    bad.thread = "";
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_ERR_THREAD);

    /* Absent, however, is fine on all three. */
    bad = good;
    bad.anchor = NULL;
    bad.origin = NULL;
    bad.thread = NULL;
    ASSERT_EQ(hyp_record_build(&bad, &rec), HYP_RECORD_OK);
    hyp_record_free(rec);
    rec = NULL;

    ASSERT_NULL(rec);
    PASS();
}

TEST(build_refuses_impossible_timestamps) {
    hyp_record_input_t in = base_input();
    const hyp_record_t *rec = NULL;

    in.timestamp_ms = 0; /* what a zeroed struct hands you */
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_ERR_TIMESTAMP);
    in.timestamp_ms = -1;
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_ERR_TIMESTAMP);

    /* Microseconds pasted into a millisecond field. Caught on the first row
     * rather than after a whole feed has been ingested at the wrong
     * resolution — the upper bound exists for exactly this. */
    in.timestamp_ms = FIXED_MS * 1000;
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_ERR_TIMESTAMP);
    ASSERT_NULL(rec);

    /* The controls: both ends of the accepted range are accepted. */
    in.timestamp_ms = HYP_RECORD_MIN_TIMESTAMP_MS;
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_OK);
    hyp_record_free(rec);
    rec = NULL;
    in.timestamp_ms = HYP_RECORD_MAX_TIMESTAMP_MS;
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_OK);
    hyp_record_free(rec);
    PASS();
}

TEST(parent_must_be_one_of_our_ids) {
    hyp_record_input_t in = base_input();
    const hyp_record_t *rec = NULL;

    /* A foreign identifier in this field would become a reference that dangles
     * forever. It is refused now instead. */
    in.parent = "task-42";
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_ERR_PARENT);
    in.parent = "6A9DFE36EC72E1D3B0A4BD7F0E0E6F2B0AA6E37BBF1EB31E7B09ED6A3A4B3F4D";
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_ERR_PARENT); /* one spelling only */
    in.parent = "6a9dfe36ec72e1d3b0a4bd7f0e0e6f2b0aa6e37bbf1eb31e7b09ed6a3a4b3f4";
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_ERR_PARENT); /* 63 chars */
    ASSERT_NULL(rec);

    /* The control: a real id is accepted, and so is no parent at all. */
    hyp_record_input_t other = base_input();
    other.content = "the record this one answers";
    const hyp_record_t *parent = NULL;
    ASSERT_TRUE(build_ok(&other, &parent));

    in.parent = parent->id;
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_OK);
    ASSERT_STR_EQ(rec->parent, parent->id);
    hyp_record_free(rec);
    rec = NULL;

    /* A parent that is not in the store is DANGLING, not invalid: under partial
     * sync it may simply not have arrived yet. Absence means look elsewhere. */
    hyp_record_set_t *set = hyp_record_set_create();
    ASSERT_NOT_NULL(set);
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_add(set, rec, NULL), HYP_RECORD_OK);
    ASSERT_NULL(hyp_record_set_get(set, rec->parent));

    hyp_record_free(rec);
    hyp_record_free(parent);
    hyp_record_set_free(set);
    PASS();
}

TEST(record_id_is_valid_says_no) {
    ASSERT_FALSE(hyp_record_id_is_valid(NULL));
    ASSERT_FALSE(hyp_record_id_is_valid(""));
    ASSERT_FALSE(hyp_record_id_is_valid("not-an-id"));
    ASSERT_FALSE(hyp_record_id_is_valid(
        "6a9dfe36ec72e1d3b0a4bd7f0e0e6f2b0aa6e37bbf1eb31e7b09ed6a3a4b3f4dd"));
    ASSERT_FALSE(
        hyp_record_id_is_valid("6a9dfe36ec72e1d3b0a4bd7f0e0e6f2b0aa6e37bbf1eb31e7b09ed6a3a4b3g4d"));
    ASSERT_TRUE(
        hyp_record_id_is_valid("6a9dfe36ec72e1d3b0a4bd7f0e0e6f2b0aa6e37bbf1eb31e7b09ed6a3a4b3f4d"));
    ASSERT_TRUE(
        hyp_record_id_is_valid("0000000000000000000000000000000000000000000000000000000000000000"));
    PASS();
}

/* ── Opacity: anchor, origin and thread are bytes, not structures ───────── */

TEST(opaque_fields_are_never_interpreted) {
    /*
     * Anything a producer might put in these fields must survive untouched:
     * separators, whitespace, quotes, braces, high bytes, a whole JSON
     * document. Nothing here parses them, so nothing here can be confused by
     * them — which is what lets the identity contract and each feed adapter
     * choose their own formats without this file knowing.
     */
    static const char *const shapes[] = {
        "repo:hyponoia#src/foundation/record.c@f00d",
        "a value with spaces, commas, and a: colon",
        "{\"json\":[1,2,3],\"nested\":{\"k\":\"v\"}}",
        "\t\r  leading and trailing whitespace  ",
        "unicode: \xe2\x86\x92 \xf0\x9f\x94\x91 done",
        "N", /* the absent marker, as a value */
        "S",
        "=",
    };
    size_t n = sizeof(shapes) / sizeof(shapes[0]);

    char seen[sizeof(shapes) / sizeof(shapes[0])][HYP_RECORD_ID_LEN + 1];
    for (size_t i = 0; i < n; i++) {
        hyp_record_input_t in = base_input();
        in.anchor = shapes[i];
        in.origin = shapes[i];
        in.thread = shapes[i];

        const hyp_record_t *rec = NULL;
        ASSERT_TRUE(build_ok(&in, &rec));
        ASSERT_STR_EQ(rec->anchor, shapes[i]);
        ASSERT_STR_EQ(rec->origin, shapes[i]);
        ASSERT_STR_EQ(rec->thread, shapes[i]);
        ASSERT_TRUE(hyp_record_verify(rec));
        snprintf(seen[i], sizeof(seen[i]), "%s", rec->id);
        hyp_record_free(rec);

        for (size_t j = 0; j < i; j++) {
            ASSERT_STR_NEQ(seen[i], seen[j]);
        }
    }
    PASS();
}

TEST(opaque_fields_respect_their_caps) {
    /* A cap is a refusal, never a truncation: truncating would change the
     * content and therefore the id, which is corruption wearing the costume of
     * success. */
    char big[HYP_RECORD_MAX_ANCHOR + 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    hyp_record_input_t in = base_input();
    const hyp_record_t *rec = NULL;
    in.anchor = big;
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_ERR_ANCHOR);
    ASSERT_NULL(rec);

    /* Control: exactly at the cap is accepted, and comes back whole. */
    big[HYP_RECORD_MAX_ANCHOR] = '\0';
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_OK);
    ASSERT_EQ((int)strlen(rec->anchor), HYP_RECORD_MAX_ANCHOR);
    hyp_record_free(rec);
    PASS();
}

/* ── The set: union, commutative, idempotent ────────────────────────────── */

static const hyp_record_t *make(const char *content, const char *anchor) {
    hyp_record_input_t in = base_input();
    in.content = content;
    in.anchor = anchor;
    const hyp_record_t *rec = NULL;
    if (hyp_record_build(&in, &rec) != HYP_RECORD_OK) {
        return NULL;
    }
    return rec;
}

static int fill(hyp_record_set_t *set, const hyp_record_t *const *recs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (hyp_record_set_add(set, recs[i], NULL) != HYP_RECORD_OK) {
            return 0;
        }
    }
    return 1;
}

TEST(set_digest_says_no) {
    /* Establish that the digest DISTINGUISHES before using it as evidence that
     * two merges agree. A digest that always matched would make every union
     * test below pass without meaning anything. */
    const hyp_record_t *r1 = make("one", NULL);
    const hyp_record_t *r2 = make("two", NULL);
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);

    hyp_record_set_t *a = hyp_record_set_create();
    hyp_record_set_t *b = hyp_record_set_create();
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    char da[HYP_RECORD_ID_LEN + 1];
    char db[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(a, da);
    hyp_record_set_digest(b, db);
    ASSERT_STR_EQ(da, db); /* two empty sets agree */

    ASSERT_EQ(hyp_record_set_add(a, r1, NULL), HYP_RECORD_OK);
    hyp_record_set_digest(a, da);
    ASSERT_STR_NEQ(da, db); /* one record is not no records */

    ASSERT_EQ(hyp_record_set_add(b, r2, NULL), HYP_RECORD_OK);
    hyp_record_set_digest(b, db);
    ASSERT_STR_NEQ(da, db); /* different records, different digests */

    /* And a set digest is not the id of its only member: separate domains. */
    ASSERT_STR_NEQ(da, r1->id);

    hyp_record_free(r1);
    hyp_record_free(r2);
    hyp_record_set_free(a);
    hyp_record_set_free(b);
    PASS();
}

TEST(merge_is_a_union) {
    /* Nothing lost, nothing altered. A = {1,2}, B = {2,3}. */
    const hyp_record_t *r1 = make("record one", "anchor-1");
    const hyp_record_t *r2 = make("record two", "anchor-2");
    const hyp_record_t *r3 = make("record three", "anchor-3");
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_NOT_NULL(r3);

    hyp_record_set_t *a = hyp_record_set_create();
    hyp_record_set_t *b = hyp_record_set_create();
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    const hyp_record_t *in_a[] = {r1, r2};
    const hyp_record_t *in_b[] = {r2, r3};
    ASSERT_TRUE(fill(a, in_a, 2));
    ASSERT_TRUE(fill(b, in_b, 2));

    ASSERT_EQ(hyp_record_set_merge(a, b), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_count(a), 3); /* the overlap is absorbed, not doubled */

    /* Nothing altered: every original comes back field-for-field. Compared by
     * fields rather than by id, because equal ids are the thing under test. */
    const hyp_record_t *const originals[] = {r1, r2, r3};
    for (size_t i = 0; i < 3; i++) {
        const hyp_record_t *found = hyp_record_set_get(a, originals[i]->id);
        ASSERT_NOT_NULL(found);
        ASSERT_TRUE(hyp_record_equal(found, originals[i]));
        ASSERT_TRUE(hyp_record_verify(found));
    }

    /* And B is untouched by having been merged from. */
    ASSERT_EQ(hyp_record_set_count(b), 2);

    hyp_record_free(r1);
    hyp_record_free(r2);
    hyp_record_free(r3);
    hyp_record_set_free(a);
    hyp_record_set_free(b);
    PASS();
}

TEST(union_keeps_records_that_share_all_three_substantive_fields) {
    /*
     * The reason the id covers EVERY field, stated as the consequence a client
     * sees rather than as a property of the digest.
     *
     * These seven records have identical content, identical author and an
     * identical timestamp. They differ in one other field each. If the id were
     * derived from only those three substantive fields they would all collide,
     * and a union keyed on id would hold one id and seven payloads — with no way
     * out but picking one, which is last-writer-wins wearing a different name.
     * Six records would be lost, and lost records do not come back: they are
     * derived from nothing.
     */
    hyp_record_input_t base = base_input();
    base.anchor = NULL;
    base.origin = NULL;
    base.thread = NULL;
    base.parent = NULL;
    base.redactions = 0;

    hyp_record_input_t variants[7];
    variants[0] = base;
    variants[1] = base;
    variants[1].anchor = "an address the others do not have";
    variants[2] = base;
    variants[2].origin = "feed:row-1";
    variants[3] = base;
    variants[3].thread = "conversation-1";
    variants[4] = base;
    variants[4].parent = "0000000000000000000000000000000000000000000000000000000000000000";
    variants[5] = base;
    variants[5].redactions = 1;
    variants[6] = base;
    variants[6].kind = HYP_RECORD_SIGNAL;

    size_t n = sizeof(variants) / sizeof(variants[0]);
    hyp_record_set_t *set = hyp_record_set_create();
    ASSERT_NOT_NULL(set);

    const hyp_record_t *built[7];
    for (size_t i = 0; i < n; i++) {
        built[i] = NULL;
        ASSERT_TRUE(build_ok(&variants[i], &built[i]));
        /* the three substantive fields really are identical across all seven */
        ASSERT_STR_EQ(built[i]->content, built[0]->content);
        ASSERT_STR_EQ(built[i]->author, built[0]->author);
        ASSERT_EQ(built[i]->timestamp_ms, built[0]->timestamp_ms);
        ASSERT_EQ(hyp_record_set_add(set, built[i], NULL), HYP_RECORD_OK);
    }

    ASSERT_EQ(hyp_record_set_count(set), n); /* nothing collapsed */
    for (size_t i = 0; i < n; i++) {
        const hyp_record_t *found = hyp_record_set_get(set, built[i]->id);
        ASSERT_NOT_NULL(found);
        ASSERT_TRUE(hyp_record_equal(found, built[i])); /* nothing altered */
        hyp_record_free(built[i]);
    }
    hyp_record_set_free(set);
    PASS();
}

TEST(merge_is_commutative) {
    /* merge(A, B) == merge(B, A). Sync order is not a fact about the result, so
     * two peers converge without agreeing on who goes first. */
    const hyp_record_t *r1 = make("alpha", "anchor-1");
    const hyp_record_t *r2 = make("beta", "anchor-2");
    const hyp_record_t *r3 = make("gamma", "anchor-3");
    const hyp_record_t *r4 = make("delta", "anchor-4");
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_NOT_NULL(r3);
    ASSERT_NOT_NULL(r4);

    hyp_record_set_t *a1 = hyp_record_set_create();
    hyp_record_set_t *b1 = hyp_record_set_create();
    hyp_record_set_t *a2 = hyp_record_set_create();
    hyp_record_set_t *b2 = hyp_record_set_create();
    ASSERT_NOT_NULL(a1);
    ASSERT_NOT_NULL(b1);
    ASSERT_NOT_NULL(a2);
    ASSERT_NOT_NULL(b2);

    const hyp_record_t *side_a[] = {r1, r2, r3};
    const hyp_record_t *side_b[] = {r3, r4};
    ASSERT_TRUE(fill(a1, side_a, 3));
    ASSERT_TRUE(fill(b1, side_b, 2));
    ASSERT_TRUE(fill(a2, side_a, 3));
    ASSERT_TRUE(fill(b2, side_b, 2));

    /* ABSOLUTE, BEFORE ANYTHING IS COMPARED. Every assertion below this point
     * compares one product of the set implementation against another product of
     * the same implementation, and two such values agree perfectly when the sets
     * are both EMPTY. Against an add that silently does nothing, the digests
     * still match, the counts still match, and the loop over them runs zero
     * times — the flagship evidence for I9 passes while the union does not
     * exist. Pin what the sets actually hold first, by id, so the comparison
     * afterwards is evidence about a union rather than about a vacuum. */
    ASSERT_EQ(hyp_record_set_count(a1), 3);
    ASSERT_EQ(hyp_record_set_count(b1), 2);
    ASSERT_EQ(hyp_record_set_count(a2), 3);
    ASSERT_EQ(hyp_record_set_count(b2), 2);

    ASSERT_EQ(hyp_record_set_merge(a1, b1), HYP_RECORD_OK); /* A then B */
    ASSERT_EQ(hyp_record_set_merge(b2, a2), HYP_RECORD_OK); /* B then A */

    /* {1,2,3} ∪ {3,4} is four records — named, not counted, so a merge that
     * produced four of the wrong things would still fail here. */
    const hyp_record_t *all[] = {r1, r2, r3, r4};
    ASSERT_EQ(hyp_record_set_count(a1), 4);
    ASSERT_EQ(hyp_record_set_count(b2), 4);
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        ASSERT_NOT_NULL(hyp_record_set_get(a1, all[i]->id));
        ASSERT_NOT_NULL(hyp_record_set_get(b2, all[i]->id));
    }

    ASSERT_EQ(hyp_record_set_count(a1), hyp_record_set_count(b2));

    char d1[HYP_RECORD_ID_LEN + 1];
    char d2[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(a1, d1);
    hyp_record_set_digest(b2, d2);
    ASSERT_STR_EQ(d1, d2);

    /* The enumeration a client reads is identical too, not merely its digest. */
    for (size_t i = 0; i < hyp_record_set_count(a1); i++) {
        const hyp_record_t *x = hyp_record_set_at(a1, i);
        const hyp_record_t *y = hyp_record_set_at(b2, i);
        ASSERT_NOT_NULL(x);
        ASSERT_NOT_NULL(y);
        ASSERT_TRUE(hyp_record_equal(x, y));
    }

    hyp_record_free(r1);
    hyp_record_free(r2);
    hyp_record_free(r3);
    hyp_record_free(r4);
    hyp_record_set_free(a1);
    hyp_record_set_free(b1);
    hyp_record_set_free(a2);
    hyp_record_set_free(b2);
    PASS();
}

TEST(merge_is_idempotent) {
    const hyp_record_t *r1 = make("alpha", "anchor-1");
    const hyp_record_t *r2 = make("beta", "anchor-2");
    const hyp_record_t *r3 = make("gamma", "anchor-3");
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_NOT_NULL(r3);

    hyp_record_set_t *a = hyp_record_set_create();
    hyp_record_set_t *b = hyp_record_set_create();
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    const hyp_record_t *side_a[] = {r1, r2};
    const hyp_record_t *side_b[] = {r2, r3};
    ASSERT_TRUE(fill(a, side_a, 2));
    ASSERT_TRUE(fill(b, side_b, 2));

    char before[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(a, before);

    /* merge(A, A) == A */
    ASSERT_EQ(hyp_record_set_merge(a, a), HYP_RECORD_OK);
    char after_self[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(a, after_self);
    ASSERT_STR_EQ(after_self, before);
    ASSERT_EQ(hyp_record_set_count(a), 2);

    /* Merging the same peer twice changes nothing the second time — a retried
     * sync is free rather than dangerous. */
    ASSERT_EQ(hyp_record_set_merge(a, b), HYP_RECORD_OK);
    char once[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(a, once);
    size_t count_once = hyp_record_set_count(a);

    ASSERT_EQ(hyp_record_set_merge(a, b), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_merge(a, b), HYP_RECORD_OK);
    char twice[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(a, twice);
    ASSERT_STR_EQ(twice, once);
    ASSERT_EQ(hyp_record_set_count(a), count_once);

    /* The control: the merge did do something the first time. */
    ASSERT_STR_NEQ(once, before);
    ASSERT_EQ(count_once, 3);

    hyp_record_free(r1);
    hyp_record_free(r2);
    hyp_record_free(r3);
    hyp_record_set_free(a);
    hyp_record_set_free(b);
    PASS();
}

TEST(merge_with_an_empty_set_is_identity) {
    const hyp_record_t *r1 = make("alpha", "anchor-1");
    ASSERT_NOT_NULL(r1);

    hyp_record_set_t *a = hyp_record_set_create();
    hyp_record_set_t *empty = hyp_record_set_create();
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(empty);
    ASSERT_EQ(hyp_record_set_add(a, r1, NULL), HYP_RECORD_OK);

    /* Both directions below assert that two digests AGREE, and the cheapest way
     * to make two digests agree is for both sets to be empty. So: state what is
     * in each set absolutely, and prove in this test that the digest can tell
     * "one record" from "no records" at all — otherwise "identity" is satisfied
     * by an add that never happened. */
    ASSERT_EQ(hyp_record_set_count(a), 1);
    ASSERT_EQ(hyp_record_set_count(empty), 0);
    ASSERT_NOT_NULL(hyp_record_set_get(a, r1->id));

    char before[HYP_RECORD_ID_LEN + 1];
    char nothing[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(a, before);
    hyp_record_set_digest(empty, nothing);
    ASSERT_STR_NEQ(before, nothing);

    ASSERT_EQ(hyp_record_set_merge(a, empty), HYP_RECORD_OK);
    char after[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(a, after);
    ASSERT_STR_EQ(after, before);
    ASSERT_EQ(hyp_record_set_count(a), 1);
    ASSERT_NOT_NULL(hyp_record_set_get(a, r1->id));
    ASSERT_TRUE(hyp_record_equal(hyp_record_set_at(a, 0), r1));

    ASSERT_EQ(hyp_record_set_merge(empty, a), HYP_RECORD_OK);
    char filled[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(empty, filled);
    ASSERT_STR_EQ(filled, before);
    /* The receiving side really received it: absorbing nothing would leave the
     * empty set empty, and `filled == before` would still hold if `before` were
     * itself the digest of nothing. */
    ASSERT_EQ(hyp_record_set_count(empty), 1);
    ASSERT_NOT_NULL(hyp_record_set_get(empty, r1->id));
    ASSERT_TRUE(hyp_record_equal(hyp_record_set_at(empty, 0), r1));

    hyp_record_free(r1);
    hyp_record_set_free(a);
    hyp_record_set_free(empty);
    PASS();
}

TEST(set_order_is_canonical_not_insertion) {
    /*
     * The mechanism behind commutativity, tested directly: what a client
     * enumerates depends on the records, never on the order they arrived. Sort
     * by arrival and merge(A,B) and merge(B,A) would enumerate differently
     * while holding the same records — and every "did we converge?" check
     * downstream would be answering a different question than it claims to.
     */
    const hyp_record_t *r1 = make("alpha", "anchor-1");
    const hyp_record_t *r2 = make("beta", "anchor-2");
    const hyp_record_t *r3 = make("gamma", "anchor-3");
    const hyp_record_t *r4 = make("delta", "anchor-4");
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_NOT_NULL(r3);
    ASSERT_NOT_NULL(r4);

    hyp_record_set_t *forward = hyp_record_set_create();
    hyp_record_set_t *backward = hyp_record_set_create();
    ASSERT_NOT_NULL(forward);
    ASSERT_NOT_NULL(backward);

    const hyp_record_t *in_order[] = {r1, r2, r3, r4};
    const hyp_record_t *reversed[] = {r4, r3, r2, r1};
    ASSERT_TRUE(fill(forward, in_order, 4));
    ASSERT_TRUE(fill(backward, reversed, 4));

    ASSERT_EQ(hyp_record_set_count(forward), 4);
    ASSERT_EQ(hyp_record_set_count(backward), 4);
    for (size_t i = 0; i < 4; i++) {
        const hyp_record_t *x = hyp_record_set_at(forward, i);
        const hyp_record_t *y = hyp_record_set_at(backward, i);
        ASSERT_NOT_NULL(x);
        ASSERT_NOT_NULL(y);
        ASSERT_STR_EQ(x->id, y->id);
        if (i > 0) {
            /* strictly ascending, so the order is total and machine-independent */
            ASSERT_LT(strcmp(hyp_record_set_at(forward, i - 1)->id, x->id), 0);
        }
    }
    ASSERT_NULL(hyp_record_set_at(forward, 4));

    hyp_record_free(r1);
    hyp_record_free(r2);
    hyp_record_free(r3);
    hyp_record_free(r4);
    hyp_record_set_free(forward);
    hyp_record_set_free(backward);
    PASS();
}

TEST(merge_leaves_the_target_alone_when_it_refuses) {
    /*
     * Atomicity. A merge that gave up halfway would leave a store whose
     * contents depend on where the error happened, and "what is in my store"
     * would stop being a function of what was written to it.
     */
    const hyp_record_t *good = make("a good record", "anchor-1");
    const hyp_record_t *ok2 = make("another good record", "anchor-2");
    const hyp_record_t *bad = make("a record that will be altered", "anchor-3");
    ASSERT_NOT_NULL(good);
    ASSERT_NOT_NULL(ok2);
    ASSERT_NOT_NULL(bad);

    hyp_record_set_t *dst = hyp_record_set_create();
    hyp_record_set_t *src = hyp_record_set_create();
    ASSERT_NOT_NULL(dst);
    ASSERT_NOT_NULL(src);
    ASSERT_EQ(hyp_record_set_add(dst, good, NULL), HYP_RECORD_OK);
    const hyp_record_t *side[] = {ok2, bad};
    ASSERT_TRUE(fill(src, side, 2));

    /* Corrupt a record already inside src, behind the set's back. */
    const hyp_record_t *planted = hyp_record_set_get(src, bad->id);
    ASSERT_NOT_NULL(planted);
    char *content = NULL;
    memcpy(&content, &planted->content, sizeof(content));
    char original = content[0];
    content[0] = (char)(original ^ 0x20);

    char before[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(dst, before);

    ASSERT_EQ(hyp_record_set_merge(dst, src), HYP_RECORD_ERR_TAMPERED);

    char after[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(dst, after);
    ASSERT_STR_EQ(after, before); /* not even the healthy record got in */
    ASSERT_EQ(hyp_record_set_count(dst), 1);

    /* The control: repair the byte and the same merge succeeds. */
    content[0] = original;
    ASSERT_EQ(hyp_record_set_merge(dst, src), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_count(dst), 3);

    hyp_record_free(good);
    hyp_record_free(ok2);
    hyp_record_free(bad);
    hyp_record_set_free(dst);
    hyp_record_set_free(src);
    PASS();
}

TEST(set_rejects_nothing_and_handles_nothing) {
    /* Nulls are refused rather than absorbed, and an empty set answers
     * "nothing" instead of pretending. */
    ASSERT_EQ(hyp_record_set_add(NULL, NULL, NULL), HYP_RECORD_ERR_NULL);
    ASSERT_EQ(hyp_record_set_merge(NULL, NULL), HYP_RECORD_ERR_NULL);
    ASSERT_EQ(hyp_record_set_count(NULL), 0);
    ASSERT_NULL(hyp_record_set_get(NULL, "x"));
    ASSERT_NULL(hyp_record_set_at(NULL, 0));

    hyp_record_set_t *set = hyp_record_set_create();
    ASSERT_NOT_NULL(set);
    ASSERT_EQ(hyp_record_set_add(set, NULL, NULL), HYP_RECORD_ERR_NULL);
    ASSERT_EQ(hyp_record_set_count(set), 0);
    ASSERT_NULL(hyp_record_set_get(set, NULL));
    ASSERT_NULL(hyp_record_set_get(
        set, "0000000000000000000000000000000000000000000000000000000000000000"));
    hyp_record_set_free(set);
    hyp_record_set_free(NULL);
    hyp_record_free(NULL);
    PASS();
}

/* ── A scrubbed record is expressible ───────────────────────────────────── */

TEST(a_scrubbed_record_says_so) {
    /*
     * D2 scrubs secrets at ingest. The scrub happens BEFORE the record is
     * built, because the id commits to the content and a scrub afterwards would
     * be a mutation — which is impossible by construction. What the record
     * carries is the count, so a reader can tell "verbatim" from "cleaned, one
     * secret removed" without diffing against a source it may not have.
     */
    hyp_record_input_t verbatim = base_input();
    verbatim.content = "the key is [redacted]";
    verbatim.redactions = 0;

    hyp_record_input_t scrubbed = verbatim;
    scrubbed.redactions = 1;

    const hyp_record_t *a = NULL;
    const hyp_record_t *b = NULL;
    ASSERT_TRUE(build_ok(&verbatim, &a));
    ASSERT_TRUE(build_ok(&scrubbed, &b));

    /* Same text, different provenance, so different records — a store holds
     * both and a reader can tell them apart. */
    ASSERT_STR_NEQ(a->id, b->id);
    ASSERT_EQ(a->redactions, 0);
    ASSERT_EQ(b->redactions, 1);

    hyp_record_free(a);
    hyp_record_free(b);
    PASS();
}

/* ── One shape, five uses ───────────────────────────────────────────────── */

TEST(all_five_kinds_are_the_same_shape) {
    /* Nothing about a kind changes what a record IS. They build, verify, merge
     * and enumerate through exactly the same path. */
    hyp_record_set_t *set = hyp_record_set_create();
    ASSERT_NOT_NULL(set);

    const hyp_record_t *built[HYP_RECORD_KIND_COUNT];
    for (int i = 0; i < (int)HYP_RECORD_KIND_COUNT; i++) {
        hyp_record_input_t in = base_input();
        in.kind = (hyp_record_kind_t)i;
        built[i] = NULL;
        ASSERT_TRUE(build_ok(&in, &built[i]));
        ASSERT_TRUE(hyp_record_verify(built[i]));
        ASSERT_EQ(hyp_record_set_add(set, built[i], NULL), HYP_RECORD_OK);
    }
    ASSERT_EQ(hyp_record_set_count(set), (size_t)HYP_RECORD_KIND_COUNT);

    for (int i = 0; i < (int)HYP_RECORD_KIND_COUNT; i++) {
        const hyp_record_t *found = hyp_record_set_get(set, built[i]->id);
        ASSERT_NOT_NULL(found);
        ASSERT_EQ((int)found->kind, i);
        hyp_record_free(built[i]);
    }
    hyp_record_set_free(set);
    PASS();
}

TEST(equal_compares_fields_not_identity) {
    const hyp_record_t *a = make("same", "anchor-1");
    const hyp_record_t *b = make("same", "anchor-1");
    const hyp_record_t *c = make("different", "anchor-1");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);

    ASSERT_TRUE(hyp_record_equal(a, a));
    ASSERT_TRUE(hyp_record_equal(a, b)); /* two allocations, one record */
    ASSERT_FALSE(hyp_record_equal(a, c));
    ASSERT_FALSE(hyp_record_equal(a, NULL));
    ASSERT_FALSE(hyp_record_equal(NULL, a));
    ASSERT_TRUE(hyp_record_equal(NULL, NULL));

    /* Absent and empty are not the same answer, and equality knows it. */
    const hyp_record_t *unanchored = make("same", NULL);
    ASSERT_NOT_NULL(unanchored);
    ASSERT_FALSE(hyp_record_equal(a, unanchored));

    hyp_record_free(a);
    hyp_record_free(b);
    hyp_record_free(c);
    hyp_record_free(unanchored);
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(record) {
    /* Kinds and statuses */
    RUN_TEST(record_kind_names_round_trip);
    RUN_TEST(record_status_reasons_are_present_and_distinct);
    /* Identity */
    RUN_TEST(record_id_is_deterministic);
    RUN_TEST(record_id_golden_vector);
    RUN_TEST(record_id_commits_to_every_field);
    RUN_TEST(record_id_distinguishes_absent_from_present);
    /* Re-ingest and the completeness audit */
    RUN_TEST(reingest_of_one_row_is_one_record);
    RUN_TEST(distinct_source_rows_stay_distinct);
    RUN_TEST(build_never_reads_a_clock);
    /* Immutability */
    RUN_TEST(record_verify_accepts_an_untouched_record);
    RUN_TEST(record_verify_detects_a_mutation);
    RUN_TEST(set_refuses_a_tampered_record);
    /* Fail closed */
    RUN_TEST(build_refuses_malformed_input);
    RUN_TEST(build_refuses_impossible_timestamps);
    RUN_TEST(parent_must_be_one_of_our_ids);
    RUN_TEST(record_id_is_valid_says_no);
    /* Opacity */
    RUN_TEST(opaque_fields_are_never_interpreted);
    RUN_TEST(opaque_fields_respect_their_caps);
    /* The union */
    RUN_TEST(set_digest_says_no);
    RUN_TEST(merge_is_a_union);
    RUN_TEST(union_keeps_records_that_share_all_three_substantive_fields);
    RUN_TEST(merge_is_commutative);
    RUN_TEST(merge_is_idempotent);
    RUN_TEST(merge_with_an_empty_set_is_identity);
    RUN_TEST(set_order_is_canonical_not_insertion);
    RUN_TEST(merge_leaves_the_target_alone_when_it_refuses);
    RUN_TEST(set_rejects_nothing_and_handles_nothing);
    /* Five uses, one shape */
    RUN_TEST(a_scrubbed_record_says_so);
    RUN_TEST(all_five_kinds_are_the_same_shape);
    RUN_TEST(equal_compares_fields_not_identity);
}
