/*
 * test_transcript.c — transcript store and ingest (D1).
 *
 * THE ORGANISING CLAIM, and this unit's row in the assertion map: ingest
 * REFUSES when no scrubber is wired. Not "a scrubber is available" — the scrub
 * unit's own suite already proves that, and proving it twice proves nothing new
 * — but "unscrubbed ingest is impossible". A scrub test can only show that
 * scrubbing works; only an INGEST test can show that ingest waits for it.
 *
 * That claim needs three assertions, not one, because each of the three ways it
 * could be false looks like success from the other two:
 *
 *   1. NO SCRUBBER, NO RECORD. A NULL scrubber is refused BY NAME, the store's
 *      digest is unchanged, and the source is never even asked for its first
 *      item — refusing after draining a transcript into memory would be a
 *      refusal about the row rather than about the wiring.
 *   2. THE COUNT IS PRODUCED, NEVER SUPPLIED. A feed item carries a redaction
 *      count and an adapter may set it to anything. The stored record's count
 *      is what this ingest's scrubber did, and a lying adapter cannot move it.
 *   3. THE ID BINDS THE SCRUBBED TEXT. Scrubbing after construction would be a
 *      DIFFERENT record, not a repair, so the id of what is stored must be the
 *      id of the clean text and must not be the id the raw text would have had.
 *      The store is then asked for the raw text's id and must not have it.
 *
 * NO KEY-SHAPED LITERAL APPEARS IN THIS FILE. Every fake key is assembled at
 * run time from the shared vendor prefix table plus an obviously-dummy body, so
 * no line here can match the pre-push history scanner's own rule.
 *
 * Everything is asserted through the CLIENT'S VIEW: records are read back out
 * of the durable store on a second handle, never inspected on the way in.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <foundation/record.h>
#include <foundation/scrub.h>
#include <ingest/transcript_ingest.h>
#include <store/record_store.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A fixed, caller-supplied instant. Nothing in this file reads a clock, and
 * neither does anything under test: an id that depended on the ingesting
 * machine would turn a sync into a duplicate storm. */
#define FIXED_MS INT64_C(1770985600000)

/* 23 key-body bytes, comfortably over the scrub's threshold, unmistakably
 * fake, and carrying no vendor prefix of its own so the literal is inert. */
#define DUMMY_BODY "FAKEFAKEFAKEFAKEFAKE123"

/* prefix + DUMMY_BODY, built at run time. False on truncation. */
static bool make_fake_key(const char *prefix, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s%s", prefix, DUMMY_BODY);
    return n > 0 && (size_t)n < cap;
}

/* ── A toy source, and how many times it was asked ──────────────────────── */

typedef struct {
    const hyp_feed_item_t *items;
    size_t count;
    size_t at;
    size_t next_calls; /* the instrument: was this source drained at all? */
} toy_feed_t;

static hyp_feed_status_t toy_next(hyp_feed_source_t *src, hyp_feed_item_t *out) {
    toy_feed_t *toy = (toy_feed_t *)src->ctx;
    toy->next_calls++;
    if (toy->at >= toy->count) {
        return HYP_FEED_END;
    }
    *out = toy->items[toy->at++];
    return out->reason ? HYP_FEED_SKIP : HYP_FEED_OK;
}

static hyp_feed_source_t toy_open(toy_feed_t *toy, const hyp_feed_item_t *items, size_t count) {
    memset(toy, 0, sizeof(*toy));
    toy->items = items;
    toy->count = count;
    hyp_feed_source_t src;
    memset(&src, 0, sizeof(src));
    src.name = "toy-transcript";
    src.ctx = toy;
    src.next = toy_next;
    src.close = NULL; /* the arrays are the caller's */
    return src;
}

static hyp_feed_item_t toy_item(const char *origin, const char *content, const char *parent) {
    hyp_feed_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = HYP_RECORD_MESSAGE;
    item.author = "agent:toy";
    item.timestamp_ms = FIXED_MS;
    item.content = content;
    item.origin = origin;
    item.thread = "toy:thread-1";
    item.parent_origin = parent;
    return item;
}

static hyp_feed_item_t toy_skip(const char *origin, const char *reason) {
    hyp_feed_item_t item;
    memset(&item, 0, sizeof(item));
    item.origin = origin;
    item.reason = reason;
    return item;
}

/* ── Scrubbers ─────────────────────────────────────────────────────────── */

/* The production scrub, with a call counter around it. The suite is
 * single-threaded, so a file-static instrument is honest here. */
static size_t g_scrub_calls;

static bool counting_scrub(const char *text, char **out_text, uint32_t *out_redactions) {
    g_scrub_calls++;
    return hyp_scrub_text(text, out_text, out_redactions);
}

/* A scrubber that cannot do its job. The row must not pass. */
static bool refusing_scrub(const char *text, char **out_text, uint32_t *out_redactions) {
    (void)text;
    (void)out_text;
    (void)out_redactions;
    return false;
}

/* ── Store helpers ─────────────────────────────────────────────────────── */

static bool digest_of(hyp_record_store_t *store, char out[HYP_RECORD_ID_LEN + 1]) {
    return hyp_record_store_digest(store, out) == HYP_RECORD_STORE_OK;
}

static size_t count_of(hyp_record_store_t *store) {
    size_t n = (size_t)-1;
    if (hyp_record_store_count(store, &n) != HYP_RECORD_STORE_OK) {
        return (size_t)-1;
    }
    return n;
}

/* The record carrying this origin, read back out of the store through the
 * surface a client has: query by the opaque bytes, then fetch by id. Caller
 * frees. Returns NULL when nothing carries that origin. */
static const hyp_record_t *stored_by_origin(hyp_record_store_t *store, const char *origin) {
    hyp_record_store_query_t query;
    memset(&query, 0, sizeof(query));
    query.origin = origin;

    hyp_record_set_t *hits = NULL;
    if (hyp_record_store_query(store, &query, &hits) != HYP_RECORD_STORE_OK) {
        return NULL;
    }
    const hyp_record_t *owned = NULL;
    const hyp_record_t *hit = hyp_record_set_at(hits, 0);
    if (hit) {
        (void)hyp_record_store_get(store, hit->id, &owned);
    }
    hyp_record_set_free(hits);
    return owned;
}

/* ── 1. The assertion row: no scrubber, no record, and no pull either ───── */

TEST(transcript_ingest_refuses_when_no_scrubber_is_wired) {
    const char *tmp = th_mktempdir("hyp_transcript_noscrub");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);
    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, before));

    hyp_feed_item_t items[] = {
        toy_item("toy:1", "a turn of a conversation that must not be stored", NULL)};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);

    hyp_transcript_ingest_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    hyp_transcript_status_t st = hyp_transcript_ingest(&src, NULL, store, &stats);

    /* Refused BY NAME. A generic "a required argument was NULL" would be the
     * same refusal wearing a costume that hides which wire is missing. */
    ASSERT_EQ(st, HYP_TRANSCRIPT_ERR_NO_SCRUBBER);
    ASSERT_STR_NEQ(hyp_transcript_status_reason(HYP_TRANSCRIPT_ERR_NO_SCRUBBER),
                   hyp_transcript_status_reason(HYP_TRANSCRIPT_ERR_NULL));

    /* Nothing built, nothing stored, and the store byte-identical by digest —
     * the same evidence a second machine would use. */
    ASSERT_EQ(stats.built, 0);
    ASSERT_EQ(stats.items, 0);
    ASSERT_EQ(count_of(store), 0);
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, after));
    ASSERT_STR_EQ(before, after);

    /* And the transcript was never even READ. Draining first and refusing
     * afterwards would still store nothing, but it would have pulled the text
     * into this process to decide something that was already decided. */
    ASSERT_EQ(toy.next_calls, 0);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── 2. The count is produced by the scrub, never supplied by the caller ── */

TEST(transcript_redaction_count_comes_from_the_scrub_not_the_adapter) {
    const char *tmp = th_mktempdir("hyp_transcript_count");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    ASSERT_GTE((long long)hyp_vendor_prefix_count, 1);
    char key[128];
    ASSERT_TRUE(make_fake_key(hyp_vendor_prefixes[0], key, sizeof(key)));
    char raw[512];
    (void)snprintf(raw, sizeof(raw), "here is the key %s, please use it", key);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    /* A LYING ADAPTER. It claims nine spans were already removed; its text
     * says otherwise. The claim has no field to travel in and never arrives. */
    hyp_feed_item_t items[] = {toy_item("toy:1", raw, NULL)};
    items[0].redactions = 9;

    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);
    hyp_transcript_ingest_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    g_scrub_calls = 0;
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, &stats), HYP_TRANSCRIPT_OK);
    ASSERT_EQ(stats.built, 1);
    ASSERT_EQ(stats.redactions, 1);
    ASSERT_EQ(stats.items_redacted, 1);
    /* Exactly one scrub per item: a second pass would find nothing (the scrub
     * is idempotent) and would report a cleaned record as verbatim. */
    ASSERT_EQ(g_scrub_calls, 1);

    const hyp_record_t *rec = stored_by_origin(store, "toy:1");
    ASSERT_NOT_NULL(rec);
    ASSERT_EQ(rec->redactions, 1); /* the scrub's answer, not the adapter's 9 */
    ASSERT_EQ(rec->kind, HYP_RECORD_MESSAGE);

    /* And the secret is gone from the bytes the id commits to. */
    char marker[128];
    (void)snprintf(marker, sizeof(marker), "[REDACTED:%s]", hyp_vendor_prefixes[0]);
    ASSERT_NOT_NULL(strstr(rec->content, marker));
    ASSERT_NULL(strstr(rec->content, DUMMY_BODY));

    hyp_record_free(rec);
    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── 3. The id binds the scrubbed text, so a later scrub is a new record ── */

TEST(transcript_id_binds_the_scrubbed_text_not_the_raw_text) {
    const char *tmp = th_mktempdir("hyp_transcript_id");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    char key[128];
    ASSERT_TRUE(make_fake_key(hyp_vendor_prefixes[0], key, sizeof(key)));
    char raw[512];
    (void)snprintf(raw, sizeof(raw), "the pasted key was %s", key);

    /* The two ids this row could have, computed independently of the ingest.
     * The scrub runs here only to obtain the clean bytes; the count is taken
     * from it too, because both are in the preimage. */
    char *clean = NULL;
    uint32_t redactions = 0;
    ASSERT_TRUE(hyp_scrub_text(raw, &clean, &redactions));
    ASSERT_EQ(redactions, 1);

    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_MESSAGE;
    in.author = "agent:toy";
    in.timestamp_ms = FIXED_MS;
    in.origin = "toy:1";
    in.thread = "toy:thread-1";

    char raw_id[HYP_RECORD_ID_LEN + 1];
    in.content = raw;
    in.redactions = 0; /* what a post-hoc scrub would have had to start from */
    ASSERT_EQ(hyp_record_derive_id(&in, raw_id), HYP_RECORD_OK);

    char clean_id[HYP_RECORD_ID_LEN + 1];
    in.content = clean;
    in.redactions = redactions;
    ASSERT_EQ(hyp_record_derive_id(&in, clean_id), HYP_RECORD_OK);

    /* The whole reason the order is structural rather than policed. */
    ASSERT_STR_NEQ(raw_id, clean_id);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);
    hyp_feed_item_t items[] = {toy_item("toy:1", raw, NULL)};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, NULL), HYP_TRANSCRIPT_OK);

    /* What the store holds is the clean record — and asking it for the raw
     * one gets ABSENT, which here means "that record was never minted". */
    const hyp_record_t *got = NULL;
    ASSERT_EQ(hyp_record_store_get(store, clean_id, &got), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(got);
    ASSERT_STR_EQ(got->content, clean);
    hyp_record_free(got);

    got = NULL;
    ASSERT_EQ(hyp_record_store_get(store, raw_id, &got), HYP_RECORD_STORE_OK);
    ASSERT_NULL(got);
    ASSERT_EQ(count_of(store), 1);

    free(clean);
    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── Idempotence: the same rows again change nothing ────────────────────── */

TEST(transcript_reingest_of_the_same_rows_is_exactly_idempotent) {
    const char *tmp = th_mktempdir("hyp_transcript_idem");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    hyp_feed_item_t items[] = {toy_item("toy:1", "first turn", NULL),
                               toy_item("toy:2", "second turn", "toy:1"),
                               toy_item("toy:3", "third turn", "toy:2")};

    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 3);
    hyp_transcript_ingest_stats_t first;
    memset(&first, 0, sizeof(first));
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, &first), HYP_TRANSCRIPT_OK);
    ASSERT_EQ(first.items, 3);
    ASSERT_EQ(first.built, 3);
    ASSERT_EQ(first.absorbed, 0);
    char after_first[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, after_first));

    /* A fresh pull over the same rows: a source is one pull, never rewound. */
    hyp_feed_source_t again = toy_open(&toy, items, 3);
    hyp_transcript_ingest_stats_t second;
    memset(&second, 0, sizeof(second));
    ASSERT_EQ(hyp_transcript_ingest(&again, counting_scrub, store, &second), HYP_TRANSCRIPT_OK);
    ASSERT_EQ(second.items, 3);
    ASSERT_EQ(second.built, 0);     /* the union absorbed every one */
    ASSERT_EQ(second.absorbed, 3);  /* stated as numbers a caller can read */
    ASSERT_EQ(count_of(store), 3);  /* no dedup table was consulted */
    char after_second[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, after_second));
    ASSERT_STR_EQ(after_first, after_second);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── Opacity: origin and thread arrive as bytes and leave as those bytes ── */

TEST(transcript_preserves_origin_and_thread_byte_for_byte) {
    const char *tmp = th_mktempdir("hyp_transcript_opaque");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    /* Shapes that would tempt a parser: separators, a wildcard byte, an
     * offset suffix. None of them is a format this ingest knows. */
    const char *weird_origin = "wd1:a/b c@42%_[x]";
    const char *weird_thread = "wd1:a/b c";

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    hyp_feed_item_t items[] = {toy_item(weird_origin, "opaque in, opaque out", NULL)};
    items[0].thread = weird_thread;
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, NULL), HYP_TRANSCRIPT_OK);

    const hyp_record_t *rec = stored_by_origin(store, weird_origin);
    ASSERT_NOT_NULL(rec);
    ASSERT_STR_EQ(rec->origin, weird_origin);
    ASSERT_STR_EQ(rec->thread, weird_thread);
    ASSERT_NULL(rec->anchor); /* attaching text to code is a read-side analysis */
    hyp_record_free(rec);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── Transcripts are messages, and a mis-wired source is a refusal ──────── */

TEST(transcript_refuses_an_item_that_is_not_a_message) {
    const char *tmp = th_mktempdir("hyp_transcript_kind");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);
    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, before));

    hyp_feed_item_t items[] = {toy_item("toy:1", "a fine first turn", NULL),
                               toy_item("toy:2", "not a turn at all", NULL)};
    items[1].kind = HYP_RECORD_DECISION;

    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 2);
    hyp_transcript_ingest_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, &stats), HYP_TRANSCRIPT_ERR_KIND);

    /* Refused, not relabelled — and atomically, so the good first row is not
     * stored either. Half a transcript is a shortfall no re-pull explains. */
    ASSERT_EQ(count_of(store), 0);
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, after));
    ASSERT_STR_EQ(before, after);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── Atomicity, against a store that already holds something ───────────── */

TEST(transcript_a_refused_pull_leaves_the_store_exactly_as_it_was) {
    const char *tmp = th_mktempdir("hyp_transcript_atomic");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    hyp_feed_item_t good[] = {toy_item("toy:1", "a turn that stays", NULL)};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, good, 1);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, NULL), HYP_TRANSCRIPT_OK);
    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, before));

    /* Three rows, the middle one unattributed. The record contract refuses an
     * absent author, and the pull goes with it. */
    hyp_feed_item_t bad[] = {toy_item("toy:2", "would have been fine", NULL),
                             toy_item("toy:3", "no author", NULL),
                             toy_item("toy:4", "also fine", NULL)};
    bad[1].author = NULL;

    hyp_feed_source_t src2 = toy_open(&toy, bad, 3);
    hyp_transcript_ingest_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT_EQ(hyp_transcript_ingest(&src2, counting_scrub, store, &stats), HYP_TRANSCRIPT_ERR_ITEM);
    ASSERT_EQ(stats.record_status, HYP_RECORD_ERR_AUTHOR);
    ASSERT_EQ(stats.built, 0);

    ASSERT_EQ(count_of(store), 1);
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, after));
    ASSERT_STR_EQ(before, after);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── A scrubber that says no takes the whole pull with it ───────────────── */

TEST(transcript_a_scrubber_that_refuses_fails_closed) {
    const char *tmp = th_mktempdir("hyp_transcript_scrubfail");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);
    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, before));

    hyp_feed_item_t items[] = {toy_item("toy:1", "text nobody could clean", NULL)};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);
    hyp_transcript_status_t st = hyp_transcript_ingest(&src, refusing_scrub, store, NULL);

    /* A scrubber that fails and an allocation that fails are one status here,
     * because the seam gives this layer no way to tell them apart. Both abort
     * the pull and store nothing, so the two need no telling apart to act on;
     * inventing a distinction the seam cannot make would be the worse error. */
    ASSERT_EQ(st, HYP_TRANSCRIPT_ERR_ALLOC);
    ASSERT_EQ(count_of(store), 0);
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(digest_of(store, after));
    ASSERT_STR_EQ(before, after);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── Precursors resolve inside the pull, in any yield order ─────────────── */

TEST(transcript_resolves_precursors_in_any_yield_order) {
    const char *tmp = th_mktempdir("hyp_transcript_parent");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    /* Child first: the yield order is the adapter's business. */
    hyp_feed_item_t items[] = {toy_item("toy:2", "the reply", "toy:1"),
                               toy_item("toy:1", "the question", NULL)};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 2);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, NULL), HYP_TRANSCRIPT_OK);

    const hyp_record_t *parent = stored_by_origin(store, "toy:1");
    const hyp_record_t *child = stored_by_origin(store, "toy:2");
    ASSERT_NOT_NULL(parent);
    ASSERT_NOT_NULL(child);
    ASSERT_NULL(parent->parent);
    ASSERT_STR_EQ(child->parent, parent->id);
    hyp_record_free(parent);
    hyp_record_free(child);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

TEST(transcript_refuses_a_precursor_the_pull_never_yielded) {
    const char *tmp = th_mktempdir("hyp_transcript_noparent");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    hyp_feed_item_t items[] = {toy_item("toy:2", "a reply to nothing here", "toy:absent")};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, NULL),
              HYP_TRANSCRIPT_ERR_PRECURSOR);
    ASSERT_EQ(count_of(store), 0);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── A skip notice is accounted for, and breaks no chain ────────────────── */

TEST(transcript_skip_notices_are_not_records_and_break_no_chain) {
    const char *tmp = th_mktempdir("hyp_transcript_skip");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    hyp_feed_item_t items[] = {toy_item("toy:1", "the question", NULL),
                               toy_skip("toy:2", "the source declined this row by policy"),
                               toy_item("toy:3", "the reply", "toy:2")};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 3);
    hyp_transcript_ingest_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, &stats), HYP_TRANSCRIPT_OK);

    ASSERT_EQ(stats.items, 2);   /* notices are not items */
    ASSERT_EQ(stats.skipped, 1); /* and they are not silence either */
    ASSERT_EQ(stats.built, 2);
    ASSERT_EQ(count_of(store), 2);

    /* The precursor is accounted for and recordless by policy, so the child
     * points at nothing — deterministically, on every machine. */
    const hyp_record_t *child = stored_by_origin(store, "toy:3");
    ASSERT_NOT_NULL(child);
    ASSERT_NULL(child->parent);
    hyp_record_free(child);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── One origin, two contents, one pull: no rule picks a winner ─────────── */

TEST(transcript_refuses_one_origin_with_two_contents) {
    const char *tmp = th_mktempdir("hyp_transcript_dup");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    hyp_feed_item_t clash[] = {toy_item("toy:1", "one version", NULL),
                               toy_item("toy:1", "another version", NULL)};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, clash, 2);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, NULL),
              HYP_TRANSCRIPT_ERR_DUPLICATE);
    ASSERT_EQ(count_of(store), 0);

    /* The same origin repeated IDENTICALLY is the source repeating itself, and
     * is absorbed exactly as a re-ingest would be. */
    hyp_feed_item_t repeat[] = {toy_item("toy:1", "one version", NULL),
                                toy_item("toy:1", "one version", NULL)};
    hyp_feed_source_t src2 = toy_open(&toy, repeat, 2);
    hyp_transcript_ingest_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ASSERT_EQ(hyp_transcript_ingest(&src2, counting_scrub, store, &stats), HYP_TRANSCRIPT_OK);
    ASSERT_EQ(stats.items, 2);
    ASSERT_EQ(stats.built, 1);
    ASSERT_EQ(stats.absorbed, 1);
    ASSERT_EQ(count_of(store), 1);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── Reading a conversation back, in the order it was said ──────────────── */

TEST(transcript_thread_reads_back_in_a_total_time_order) {
    const char *tmp = th_mktempdir("hyp_transcript_thread");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    /* Yielded out of order, and two of them share a millisecond — a harness
     * writing a turn and the tool call it made in the same instant. */
    hyp_feed_item_t items[] = {
        toy_item("toy:3", "third", NULL), toy_item("toy:1", "first", NULL),
        toy_item("toy:2b", "second, same instant", NULL), toy_item("toy:2a", "second", NULL),
        toy_item("other:1", "a different conversation entirely", NULL)};
    items[0].timestamp_ms = FIXED_MS + 3000;
    items[1].timestamp_ms = FIXED_MS + 1000;
    items[2].timestamp_ms = FIXED_MS + 2000;
    items[3].timestamp_ms = FIXED_MS + 2000;
    items[4].timestamp_ms = FIXED_MS + 500;
    items[4].thread = "toy:thread-2";

    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 5);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, NULL), HYP_TRANSCRIPT_OK);

    hyp_transcript_thread_t thread;
    memset(&thread, 0, sizeof(thread));
    ASSERT_EQ(hyp_transcript_thread_load(store, "toy:thread-1", &thread), HYP_TRANSCRIPT_OK);
    ASSERT_EQ(thread.count, 4); /* the other conversation is not in this one */

    for (size_t i = 1; i < thread.count; i++) {
        const hyp_record_t *prev = thread.messages[i - 1];
        const hyp_record_t *here = thread.messages[i];
        ASSERT_LTE(prev->timestamp_ms, here->timestamp_ms);
        if (prev->timestamp_ms == here->timestamp_ms) {
            /* Ties break by id, so the order is total and machine-independent
             * rather than whatever the iteration happened to hand back. */
            ASSERT_LT(strcmp(prev->id, here->id), 0);
        }
    }
    ASSERT_STR_EQ(thread.messages[0]->content, "first");
    ASSERT_STR_EQ(thread.messages[3]->content, "third");
    hyp_transcript_thread_dispose(&thread);

    /* A conversation nobody had is EMPTY, not absent: empty means there is
     * nothing, absent would mean look elsewhere, and only one is ever true. */
    memset(&thread, 0, sizeof(thread));
    ASSERT_EQ(hyp_transcript_thread_load(store, "toy:thread-never", &thread), HYP_TRANSCRIPT_OK);
    ASSERT_EQ(thread.count, 0);
    hyp_transcript_thread_dispose(&thread);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── Refusals are distinguishable, or they are one log line ─────────────── */

TEST(transcript_refuses_nothing_quietly) {
    hyp_feed_item_t items[] = {toy_item("toy:1", "a turn", NULL)};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);

    /* Every argument, checked. The scrubber first: the store cannot be the
     * reason a caller hears about when the scrubber is also missing. */
    ASSERT_EQ(hyp_transcript_ingest(&src, NULL, NULL, NULL), HYP_TRANSCRIPT_ERR_NO_SCRUBBER);
    ASSERT_EQ(hyp_transcript_ingest(NULL, counting_scrub, NULL, NULL), HYP_TRANSCRIPT_ERR_NULL);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, NULL, NULL), HYP_TRANSCRIPT_ERR_NULL);
    ASSERT_EQ(toy.next_calls, 0);

    hyp_transcript_thread_t thread;
    ASSERT_EQ(hyp_transcript_thread_load(NULL, "t", &thread), HYP_TRANSCRIPT_ERR_NULL);
    hyp_transcript_thread_dispose(&thread);
    hyp_transcript_thread_dispose(NULL);

    /* One reason per status, all present, all distinct. Two refusals that read
     * the same are one refusal as far as anyone reading a log is concerned. */
    static const hyp_transcript_status_t ALL[] = {
        HYP_TRANSCRIPT_OK,          HYP_TRANSCRIPT_ERR_NULL,      HYP_TRANSCRIPT_ERR_NO_SCRUBBER,
        HYP_TRANSCRIPT_ERR_KIND,    HYP_TRANSCRIPT_ERR_SOURCE,    HYP_TRANSCRIPT_ERR_ORIGIN,
        HYP_TRANSCRIPT_ERR_ITEM,    HYP_TRANSCRIPT_ERR_PRECURSOR, HYP_TRANSCRIPT_ERR_DUPLICATE,
        HYP_TRANSCRIPT_ERR_STORE,   HYP_TRANSCRIPT_ERR_ALLOC};
    size_t n = sizeof(ALL) / sizeof(ALL[0]);
    for (size_t i = 0; i < n; i++) {
        const char *a = hyp_transcript_status_reason(ALL[i]);
        ASSERT_NOT_NULL(a);
        ASSERT_TRUE(a[0] != '\0');
        for (size_t j = i + 1; j < n; j++) {
            ASSERT_STR_NEQ(a, hyp_transcript_status_reason(ALL[j]));
        }
    }
    PASS();
}

/* ── An item with no origin is not a row anything can audit ─────────────── */

TEST(transcript_requires_an_origin) {
    const char *tmp = th_mktempdir("hyp_transcript_origin");
    ASSERT_NOT_NULL(tmp);
    char dir[512];
    (void)snprintf(dir, sizeof(dir), "%s", tmp);

    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(dir, &store), HYP_RECORD_STORE_OK);

    hyp_feed_item_t items[] = {toy_item(NULL, "a turn from nowhere", NULL)};
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);
    ASSERT_EQ(hyp_transcript_ingest(&src, counting_scrub, store, NULL), HYP_TRANSCRIPT_ERR_ORIGIN);
    ASSERT_EQ(count_of(store), 0);

    hyp_feed_item_t empty[] = {toy_item("", "a turn from nowhere", NULL)};
    hyp_feed_source_t src2 = toy_open(&toy, empty, 1);
    ASSERT_EQ(hyp_transcript_ingest(&src2, counting_scrub, store, NULL), HYP_TRANSCRIPT_ERR_ORIGIN);
    ASSERT_EQ(count_of(store), 0);

    hyp_record_store_close(store);
    (void)th_rmtree(dir);
    PASS();
}

/* ── TEMPORARY: reproduce the unscrubbed feed path before closing it ────── */

TEST(REPRO_feed_ingest_mints_an_unscrubbed_record) {
    char key[128];
    ASSERT_TRUE(make_fake_key(hyp_vendor_prefixes[0], key, sizeof(key)));
    char raw[512];
    (void)snprintf(raw, sizeof(raw), "here is the key %s", key);

    hyp_feed_item_t items[] = {toy_item("toy:1", raw, NULL)};
    items[0].redactions = 5; /* a claim the core never checks */
    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, items, 1);

    hyp_record_set_t *set = hyp_record_set_create();
    ASSERT_NOT_NULL(set);
    ASSERT_EQ(hyp_feed_ingest(&src, set, NULL), HYP_FEED_OK);
    const hyp_record_t *rec = hyp_record_set_at(set, 0);
    ASSERT_NOT_NULL(rec);
    ASSERT_NOT_NULL(strstr(rec->content, DUMMY_BODY)); /* the key is IN the id */
    ASSERT_EQ(rec->redactions, 5);                     /* the lie rode through */
    hyp_record_set_free(set);
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(transcript) {
    RUN_TEST(REPRO_feed_ingest_mints_an_unscrubbed_record);
    /* The assertion row, and the two properties that keep it honest */
    RUN_TEST(transcript_ingest_refuses_when_no_scrubber_is_wired);
    RUN_TEST(transcript_redaction_count_comes_from_the_scrub_not_the_adapter);
    RUN_TEST(transcript_id_binds_the_scrubbed_text_not_the_raw_text);
    /* The store half */
    RUN_TEST(transcript_reingest_of_the_same_rows_is_exactly_idempotent);
    RUN_TEST(transcript_preserves_origin_and_thread_byte_for_byte);
    RUN_TEST(transcript_thread_reads_back_in_a_total_time_order);
    /* Fail closed, every way in */
    RUN_TEST(transcript_refuses_an_item_that_is_not_a_message);
    RUN_TEST(transcript_a_refused_pull_leaves_the_store_exactly_as_it_was);
    RUN_TEST(transcript_a_scrubber_that_refuses_fails_closed);
    RUN_TEST(transcript_resolves_precursors_in_any_yield_order);
    RUN_TEST(transcript_refuses_a_precursor_the_pull_never_yielded);
    RUN_TEST(transcript_skip_notices_are_not_records_and_break_no_chain);
    RUN_TEST(transcript_refuses_one_origin_with_two_contents);
    RUN_TEST(transcript_requires_an_origin);
    RUN_TEST(transcript_refuses_nothing_quietly);
}
