/*
 * test_feed.c — the feed boundary (D3) and the completeness audit (D4).
 *
 * Two adapters drive the same interface here. The TOY adapter is the boundary
 * proof: an in-memory array source written strictly against feed.h — if it
 * needed a core change, the boundary would be in the wrong place, and the fix
 * would be the boundary, not the toy. The MULTICA adapter is the first real
 * one, driven through fixture rowsets that deliver text cells exactly the way
 * the postgres wire would.
 *
 * The audit's assertion row: N rows with two byte-identical → the audit
 * reports COMPLETE; delete one → the audit NAMES the missing origin, never
 * "N−1 of N".
 */
#include "test_framework.h"

#include "../src/feed/feed.h"
#include "../src/feed/multica_adapter.h"

#include <stdint.h>
#include <string.h>

/* ── The toy adapter: the second-adapter proof, in ~40 lines ────────────── */

/*
 * TOY-ADAPTER-BEGIN
 *
 * Everything between these two markers is written against feed.h ALONE.
 * tests/test_feed_contract.sh lifts this region out and compiles it with
 * nothing else included, which is what turns "the boundary is in the right
 * place" from a review opinion into something a machine rejects. Do not reach
 * for a Multica symbol, or a record-set helper, in here: the whole claim is
 * that a second adapter needs neither.
 */

/* A fixed, caller-supplied instant: 2026-02-14T12:26:40.000Z. The source states
 * the event time; nothing downstream of here reads a clock. */
#define FIXED_MS INT64_C(1770985600000)

typedef struct {
    const hyp_feed_item_t *items;
    size_t count;
    size_t at;
} toy_feed_t;

static hyp_feed_status_t toy_next(hyp_feed_source_t *src, hyp_feed_item_t *out) {
    toy_feed_t *toy = (toy_feed_t *)src->ctx;
    if (toy->at >= toy->count) {
        return HYP_FEED_END;
    }
    *out = toy->items[toy->at++];
    return out->reason ? HYP_FEED_SKIP : HYP_FEED_OK;
}

static hyp_feed_source_t toy_open(toy_feed_t *toy, const hyp_feed_item_t *items, size_t count) {
    toy->items = items;
    toy->count = count;
    toy->at = 0;
    hyp_feed_source_t src;
    memset(&src, 0, sizeof(src));
    src.name = "toy";
    src.ctx = toy;
    src.next = toy_next;
    src.close = NULL; /* nothing to release: the arrays are the caller's */
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

/*
 * A deliberately DEFECTIVE source, for the one refusal the toy cannot express.
 * The toy decides notice-from-item by whether a reason is present, so a notice
 * with no stated reason is not a shape it can produce — which is the toy being
 * a well-behaved adapter, not the toy being inadequate. The core's refusal of
 * that shape still has to be proven, and proving it needs a source that
 * misbehaves on purpose.
 */
static hyp_feed_status_t reasonless_notice_next(hyp_feed_source_t *src, hyp_feed_item_t *out) {
    bool *spent = (bool *)src->ctx;
    if (*spent) {
        return HYP_FEED_END;
    }
    *spent = true;
    memset(out, 0, sizeof(*out));
    out->origin = "toy:9";
    out->reason = NULL; /* declined, with no policy named for declining it */
    return HYP_FEED_SKIP;
}
/* TOY-ADAPTER-END */

/* The record holding this origin, or NULL. A linear scan on purpose: the test
 * asserts through the same read surface a client has, and the client has the
 * enumeration. */
static const hyp_record_t *find_by_origin(const hyp_record_set_t *store, const char *origin) {
    for (size_t i = 0;; i++) {
        const hyp_record_t *rec = hyp_record_set_at(store, i);
        if (!rec) {
            return NULL;
        }
        if (rec->origin && strcmp(rec->origin, origin) == 0) {
            return rec;
        }
    }
}

/* ── Statuses ───────────────────────────────────────────────────────────── */

TEST(feed_status_reasons_are_present_and_distinct) {
    const hyp_feed_status_t all[] = {HYP_FEED_OK,
                                     HYP_FEED_END,
                                     HYP_FEED_SKIP,
                                     HYP_FEED_ERR_NO_SCRUBBER,
                                     HYP_FEED_ERR_NULL,
                                     HYP_FEED_ERR_SOURCE,
                                     HYP_FEED_ERR_SCHEMA,
                                     HYP_FEED_ERR_ITEM,
                                     HYP_FEED_ERR_ORIGIN,
                                     HYP_FEED_ERR_PRECURSOR,
                                     HYP_FEED_ERR_DUPLICATE,
                                     HYP_FEED_ERR_ALLOC};
    size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const char *reason = hyp_feed_status_reason(all[i]);
        ASSERT_NOT_NULL(reason);
        ASSERT_TRUE(reason[0] != '\0');
        for (size_t j = i + 1; j < n; j++) {
            ASSERT_STR_NEQ(reason, hyp_feed_status_reason(all[j]));
        }
    }
    PASS();
}

/* ── Ingest through the toy: the boundary carries everything the core needs ─ */

TEST(feed_ingest_builds_records_and_reingest_is_idempotent) {
    const hyp_feed_item_t items[] = {
        toy_item("toy:1", "one", NULL),
        toy_item("toy:2", "two", NULL),
        toy_item("toy:3", "three", NULL),
    };
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);

    hyp_feed_source_t src = toy_open(&toy, items, 3);
    hyp_feed_ingest_stats_t stats;
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_OK);
    ASSERT_EQ(stats.items, 3);
    ASSERT_EQ(stats.built, 3);
    ASSERT_EQ(stats.absorbed, 0);
    ASSERT_EQ(hyp_record_set_count(store), 3);

    /* What a client reads is what the adapter supplied, through C2. */
    const hyp_record_t *rec = find_by_origin(store, "toy:2");
    ASSERT_NOT_NULL(rec);
    ASSERT_STR_EQ(rec->content, "two");
    ASSERT_STR_EQ(rec->author, "agent:toy");
    ASSERT_STR_EQ(rec->thread, "toy:thread-1");
    ASSERT_EQ(rec->timestamp_ms, FIXED_MS);
    ASSERT_EQ((int)rec->kind, (int)HYP_RECORD_MESSAGE);

    char before[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(store, before);

    /* The same pull again: a union of the same records, stated as numbers. */
    src = toy_open(&toy, items, 3);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_OK);
    ASSERT_EQ(stats.items, 3);
    ASSERT_EQ(stats.built, 0);
    ASSERT_EQ(stats.absorbed, 3);
    char after[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(store, after);
    ASSERT_STR_EQ(after, before);

    hyp_record_set_free(store);
    PASS();
}

TEST(feed_ingest_is_atomic) {
    /* Item 3 is refused by the record contract; items 1 and 2 must NOT land.
     * A half-ingested feed would leave the audit reporting a shortfall no
     * re-pull could explain. */
    const hyp_feed_item_t bad[] = {
        toy_item("toy:1", "one", NULL),
        toy_item("toy:2", "two", NULL),
        toy_item("toy:3", "", NULL), /* empty content: ERR_CONTENT */
    };
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    char before[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(store, before);

    hyp_feed_source_t src = toy_open(&toy, bad, 3);
    hyp_feed_ingest_stats_t stats;
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_ERR_ITEM);
    ASSERT_EQ((int)stats.record_status, (int)HYP_RECORD_ERR_CONTENT);
    ASSERT_EQ(hyp_record_set_count(store), 0);
    char after[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(store, after);
    ASSERT_STR_EQ(after, before);

    /* The control: fix the one bad item and the same feed lands whole. */
    const hyp_feed_item_t good[] = {
        toy_item("toy:1", "one", NULL),
        toy_item("toy:2", "two", NULL),
        toy_item("toy:3", "three", NULL),
    };
    src = toy_open(&toy, good, 3);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_OK);
    ASSERT_EQ(hyp_record_set_count(store), 3);

    hyp_record_set_free(store);
    PASS();
}

TEST(feed_ingest_requires_an_origin) {
    /* Without the source's own row identity the audit is impossible, so an
     * item without one is refused — not defaulted, not dropped. */
    hyp_feed_item_t anonymous = toy_item(NULL, "content", NULL);
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);

    hyp_feed_source_t src = toy_open(&toy, &anonymous, 1);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_ORIGIN);
    ASSERT_EQ(hyp_record_set_count(store), 0);

    hyp_feed_item_t empty_origin = toy_item("", "content", NULL);
    src = toy_open(&toy, &empty_origin, 1);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_ORIGIN);

    /* A skip notice needs a name too — and a stated reason. */
    hyp_feed_item_t nameless_skip = toy_skip(NULL, "because");
    src = toy_open(&toy, &nameless_skip, 1);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_ORIGIN);

    /* And a notice with no stated policy is a source defect, not a policy —
     * the audit would otherwise carry a decline it cannot account for. */
    bool spent = false;
    hyp_feed_source_t defective;
    memset(&defective, 0, sizeof(defective));
    defective.name = "toy-defective";
    defective.ctx = &spent;
    defective.next = reasonless_notice_next;
    ASSERT_EQ(hyp_feed_ingest(&defective, hyp_scrub_text, store, NULL), HYP_FEED_ERR_SOURCE);
    ASSERT_EQ(hyp_record_set_count(store), 0);

    hyp_record_set_free(store);
    PASS();
}

TEST(feed_ingest_resolves_precursors_in_any_yield_order) {
    /* c names b, b names a — and the pull yields them backwards. Resolution
     * is over the pull, not the stream position, so all three land, linked. */
    const hyp_feed_item_t items[] = {
        toy_item("toy:c", "third", "toy:b"),
        toy_item("toy:b", "second", "toy:a"),
        toy_item("toy:a", "first", NULL),
    };
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_source_t src = toy_open(&toy, items, 3);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_OK);
    ASSERT_EQ(hyp_record_set_count(store), 3);

    const hyp_record_t *a = find_by_origin(store, "toy:a");
    const hyp_record_t *b = find_by_origin(store, "toy:b");
    const hyp_record_t *c = find_by_origin(store, "toy:c");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(a->parent);
    ASSERT_NOT_NULL(b->parent);
    ASSERT_STR_EQ(b->parent, a->id);
    ASSERT_NOT_NULL(c->parent);
    ASSERT_STR_EQ(c->parent, b->id);

    hyp_record_set_free(store);
    PASS();
}

TEST(feed_ingest_refuses_an_unknown_precursor) {
    /* A precursor the pull never yielded is a confident error, never a
     * dangling guess — resolving against the store would make the result
     * depend on local state, and two machines would mint different ids. */
    const hyp_feed_item_t items[] = {
        toy_item("toy:1", "one", NULL),
        toy_item("toy:2", "two", "toy:99"),
    };
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_source_t src = toy_open(&toy, items, 2);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_PRECURSOR);
    ASSERT_EQ(hyp_record_set_count(store), 0); /* atomic: toy:1 did not land */

    /* A cycle is the same defect wearing a loop. */
    const hyp_feed_item_t knot[] = {toy_item("toy:s", "self", "toy:s")};
    src = toy_open(&toy, knot, 1);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_PRECURSOR);

    /* The control: name a precursor the pull holds and the same feed lands. */
    const hyp_feed_item_t fixed[] = {
        toy_item("toy:1", "one", NULL),
        toy_item("toy:2", "two", "toy:1"),
    };
    src = toy_open(&toy, fixed, 2);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_OK);
    ASSERT_EQ(hyp_record_set_count(store), 2);

    hyp_record_set_free(store);
    PASS();
}

TEST(feed_ingest_refuses_one_origin_with_two_contents) {
    const hyp_feed_item_t twins_differ[] = {
        toy_item("toy:1", "one", NULL),
        toy_item("toy:1", "another one", NULL),
    };
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_source_t src = toy_open(&toy, twins_differ, 2);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_DUPLICATE);
    ASSERT_EQ(hyp_record_set_count(store), 0);

    /* The control: the same origin repeated IDENTICALLY is the source
     * repeating itself — absorbed, exactly as a re-ingest would be. */
    const hyp_feed_item_t twins_same[] = {
        toy_item("toy:1", "one", NULL),
        toy_item("toy:1", "one", NULL),
    };
    src = toy_open(&toy, twins_same, 2);
    hyp_feed_ingest_stats_t stats;
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_OK);
    ASSERT_EQ(stats.items, 2);
    ASSERT_EQ(stats.built, 1);
    ASSERT_EQ(stats.absorbed, 1);
    ASSERT_EQ(hyp_record_set_count(store), 1);

    hyp_record_set_free(store);
    PASS();
}

TEST(feed_skip_notices_are_counted_and_break_no_chain) {
    /* b is declined by policy; c names it as precursor. The chain resolves to
     * "no parent" — deterministically, on every machine that runs the pull —
     * and the notice is counted, not lost. */
    const hyp_feed_item_t items[] = {
        toy_item("toy:a", "first", NULL),
        toy_skip("toy:b", "empty-by-policy"),
        toy_item("toy:c", "third", "toy:b"),
    };
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_source_t src = toy_open(&toy, items, 3);
    hyp_feed_ingest_stats_t stats;
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_OK);
    ASSERT_EQ(stats.items, 2);
    ASSERT_EQ(stats.skipped, 1);
    ASSERT_EQ(hyp_record_set_count(store), 2);
    const hyp_record_t *c = find_by_origin(store, "toy:c");
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(c->parent);

    hyp_record_set_free(store);
    PASS();
}

/* ── D4: the audit, and its assertion row ───────────────────────────────── */

TEST(feed_audit_complete_with_identical_rows_and_names_the_missing_one) {
    /*
     * The assertion row this test exists to hold: N rows with two
     * byte-identical → audit reports COMPLETE; delete one → audit NAMES the
     * missing origin, not "N−1 of N".
     *
     * toy:2 and toy:5 share content, author, timestamp and thread down to the
     * byte. In a content-addressed store without origin they would be ONE
     * record, and a count audit would report an unfixable shortfall — dedup
     * and loss look the same from a count. The origin SET tells them apart.
     */
    const hyp_feed_item_t items[] = {
        toy_item("toy:1", "alpha", NULL),  toy_item("toy:2", "ok", NULL),
        toy_item("toy:3", "beta", NULL),   toy_item("toy:4", "gamma", NULL),
        toy_item("toy:5", "ok", NULL),     toy_item("toy:6", "delta", NULL),
    };
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_source_t src = toy_open(&toy, items, 6);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_OK);
    ASSERT_EQ(hyp_record_set_count(store), 6); /* the twins stayed distinct */

    hyp_feed_audit_t report;
    src = toy_open(&toy, items, 6);
    ASSERT_EQ(hyp_feed_audit_run(&src, store, &report), HYP_FEED_OK);
    ASSERT_TRUE(hyp_feed_audit_complete(&report));
    ASSERT_EQ(report.expected, 6);
    ASSERT_EQ(report.present, 6);
    ASSERT_EQ(report.missing_count, 0);
    ASSERT_STR_EQ(report.feed, "toy");
    hyp_feed_audit_dispose(&report);

    /* Now a store that lost exactly one row: ingest everything except toy:4
     * into a fresh store and audit THE FULL FEED against it. */
    const hyp_feed_item_t partial[] = {
        toy_item("toy:1", "alpha", NULL), toy_item("toy:2", "ok", NULL),
        toy_item("toy:3", "beta", NULL),  toy_item("toy:5", "ok", NULL),
        toy_item("toy:6", "delta", NULL),
    };
    hyp_record_set_t *lossy = hyp_record_set_create();
    ASSERT_NOT_NULL(lossy);
    src = toy_open(&toy, partial, 5);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, lossy, NULL), HYP_FEED_OK);

    src = toy_open(&toy, items, 6);
    ASSERT_EQ(hyp_feed_audit_run(&src, lossy, &report), HYP_FEED_OK);
    ASSERT_FALSE(hyp_feed_audit_complete(&report));
    ASSERT_EQ(report.expected, 6);
    ASSERT_EQ(report.present, 5);
    ASSERT_EQ(report.missing_count, 1);
    ASSERT_NOT_NULL(report.missing);
    ASSERT_STR_EQ(report.missing[0], "toy:4"); /* named, not counted */
    hyp_feed_audit_dispose(&report);

    hyp_record_set_free(store);
    hyp_record_set_free(lossy);
    PASS();
}

TEST(feed_audit_reports_skips_separately_from_loss) {
    const hyp_feed_item_t items[] = {
        toy_item("toy:1", "alpha", NULL),
        toy_skip("toy:2", "empty-by-policy"),
        toy_item("toy:3", "beta", NULL),
    };
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_source_t src = toy_open(&toy, items, 3);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_OK);

    hyp_feed_audit_t report;
    src = toy_open(&toy, items, 3);
    ASSERT_EQ(hyp_feed_audit_run(&src, store, &report), HYP_FEED_OK);
    /* Declined by stated policy is NOT loss: the audit stays complete and the
     * decline is on the record, by name, with the adapter's reason. */
    ASSERT_TRUE(hyp_feed_audit_complete(&report));
    ASSERT_EQ(report.expected, 2);
    ASSERT_EQ(report.missing_count, 0);
    ASSERT_EQ(report.skipped_count, 1);
    ASSERT_STR_EQ(report.skipped[0], "toy:2");
    ASSERT_STR_EQ(report.skip_reasons[0], "empty-by-policy");
    hyp_feed_audit_dispose(&report);

    hyp_record_set_free(store);
    PASS();
}

TEST(feed_audit_is_per_feed_never_global) {
    /* Two feeds share one store. Each audit sees only its own origins: a
     * record from the other feed neither satisfies nor alarms it, and locally
     * authored records (no origin at all) are invisible to both. */
    const hyp_feed_item_t feed_a[] = {toy_item("a:1", "from a", NULL)};
    const hyp_feed_item_t feed_b[] = {toy_item("b:1", "from b", NULL),
                                      toy_item("b:2", "also from b", NULL)};
    toy_feed_t toy;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);

    hyp_feed_source_t src = toy_open(&toy, feed_a, 1);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_OK);
    src = toy_open(&toy, feed_b, 2);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_OK);

    hyp_record_input_t local;
    memset(&local, 0, sizeof(local));
    local.kind = HYP_RECORD_DECISION;
    local.author = "agent:local";
    local.timestamp_ms = FIXED_MS;
    local.content = "authored here, from no feed";
    const hyp_record_t *rec = NULL;
    ASSERT_EQ(hyp_record_build(&local, &rec), HYP_RECORD_OK);
    ASSERT_EQ(hyp_record_set_add(store, rec, NULL), HYP_RECORD_OK);
    hyp_record_free(rec);
    ASSERT_EQ(hyp_record_set_count(store), 4);

    hyp_feed_audit_t report;
    src = toy_open(&toy, feed_a, 1);
    ASSERT_EQ(hyp_feed_audit_run(&src, store, &report), HYP_FEED_OK);
    ASSERT_TRUE(hyp_feed_audit_complete(&report));
    ASSERT_EQ(report.expected, 1); /* b's rows and the local record are not here */
    hyp_feed_audit_dispose(&report);

    src = toy_open(&toy, feed_b, 2);
    ASSERT_EQ(hyp_feed_audit_run(&src, store, &report), HYP_FEED_OK);
    ASSERT_TRUE(hyp_feed_audit_complete(&report));
    ASSERT_EQ(report.expected, 2);
    hyp_feed_audit_dispose(&report);

    hyp_record_set_free(store);
    PASS();
}

TEST(feed_refuses_nothing_quietly) {
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_audit_t report;

    /* The scrubber is checked before anything else, so a call missing BOTH
     * the source and the scrubber names the scrubber. A caller who wired no
     * scrubber must not be told about an argument they did supply. */
    ASSERT_EQ(hyp_feed_ingest(NULL, NULL, NULL, NULL), HYP_FEED_ERR_NO_SCRUBBER);
    ASSERT_STR_NEQ(hyp_feed_status_reason(HYP_FEED_ERR_NO_SCRUBBER),
                   hyp_feed_status_reason(HYP_FEED_ERR_NULL));
    ASSERT_EQ(hyp_feed_ingest(NULL, hyp_scrub_text, store, NULL), HYP_FEED_ERR_NULL);
    ASSERT_EQ(hyp_feed_audit_run(NULL, store, &report), HYP_FEED_ERR_NULL);
    ASSERT_EQ(hyp_feed_audit_run(NULL, store, NULL), HYP_FEED_ERR_NULL);

    toy_feed_t toy;
    hyp_feed_source_t src = toy_open(&toy, NULL, 0);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, NULL, NULL), HYP_FEED_ERR_NULL);
    ASSERT_EQ(hyp_feed_ingest(&src, NULL, store, NULL), HYP_FEED_ERR_NO_SCRUBBER);

    /* An empty feed is a real answer: nothing expected, nothing missing. */
    src = toy_open(&toy, NULL, 0);
    ASSERT_EQ(hyp_feed_audit_run(&src, store, &report), HYP_FEED_OK);
    ASSERT_TRUE(hyp_feed_audit_complete(&report));
    ASSERT_EQ(report.expected, 0);
    hyp_feed_audit_dispose(&report);
    hyp_feed_audit_dispose(&report); /* dispose of a zeroed report is safe */
    hyp_feed_source_close(&src);     /* NULL close hook is a no-op */
    hyp_feed_source_close(NULL);

    hyp_record_set_free(store);
    PASS();
}

/* ── The Multica adapter, through fixture rowsets ───────────────────────── */

typedef struct {
    const char *const *names;
    size_t ncols;
    const char *const *cells; /* nrows * ncols; NULL is SQL NULL */
    size_t nrows;
    size_t at;
    long fail_at; /* row index at which the source dies, or -1 */
} fix_rows_ctx_t;

static size_t fix_column_count(const hyp_multica_rows_t *rows) {
    return ((const fix_rows_ctx_t *)rows->ctx)->ncols;
}

static const char *fix_column_name(const hyp_multica_rows_t *rows, size_t col) {
    const fix_rows_ctx_t *ctx = (const fix_rows_ctx_t *)rows->ctx;
    return col < ctx->ncols ? ctx->names[col] : NULL;
}

static int fix_next_row(hyp_multica_rows_t *rows) {
    fix_rows_ctx_t *ctx = (fix_rows_ctx_t *)rows->ctx;
    if (ctx->fail_at >= 0 && (long)ctx->at == ctx->fail_at) {
        return -1;
    }
    if (ctx->at >= ctx->nrows) {
        return 0;
    }
    ctx->at++;
    return 1;
}

static const char *fix_value(const hyp_multica_rows_t *rows, size_t col) {
    const fix_rows_ctx_t *ctx = (const fix_rows_ctx_t *)rows->ctx;
    return ctx->cells[(ctx->at - 1) * ctx->ncols + col];
}

static hyp_multica_rows_t fix_rows(fix_rows_ctx_t *ctx, const char *const *names, size_t ncols,
                                   const char *const *cells, size_t nrows) {
    ctx->names = names;
    ctx->ncols = ncols;
    ctx->cells = cells;
    ctx->nrows = nrows;
    ctx->at = 0;
    ctx->fail_at = -1;
    hyp_multica_rows_t rows;
    rows.ctx = ctx;
    rows.column_count = fix_column_count;
    rows.column_name = fix_column_name;
    rows.next_row = fix_next_row;
    rows.value = fix_value;
    return rows;
}

static size_t messages_ncols(void) {
    size_t n = 0;
    while (HYP_MULTICA_MESSAGES_COLUMNS[n]) {
        n++;
    }
    return n;
}

/* One task_message result row: 15 cells in pinned order. */
#define MSG_ROW(id, task, seqno, type, tool, content, input, output, when, agent, ptask, iss, \
                sess, pfirst, flag)                                                           \
    id, task, seqno, type, tool, content, input, output, when, agent, ptask, iss, sess,       \
        pfirst, flag

/* Two tasks; t2 was delegated from t1. All attributed, flag known false. */
static const char *const MSG_FIXTURE[] = {
    /* clang-format off */
    MSG_ROW("m1", "t1", "1", "message", NULL, "hello", NULL, NULL, "1700000000001",
            "opus", NULL, NULL, NULL, NULL, "false"),
    MSG_ROW("m2", "t1", "2", "tool_use", "grep", NULL, "{\"q\":1}", "ok", "1700000000002",
            "opus", NULL, NULL, NULL, NULL, "false"),
    MSG_ROW("m3", "t2", "1", "message", NULL, "child says hi", NULL, NULL, "1700000000003",
            "haiku", "t1", "iss-9", "sess-7", "m1", "false"),
    /* clang-format on */
};

TEST(multica_pin_accepts_its_own_columns) {
    /* The positive control for every pin refusal below: the declared column
     * list, unaltered, opens. */
    fix_rows_ctx_t ctx;
    hyp_multica_rows_t rows =
        fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), MSG_FIXTURE, 3);
    hyp_feed_source_t src;
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    hyp_feed_source_close(&src);
    PASS();
}

TEST(multica_pin_fails_closed_on_any_column_mismatch) {
    size_t n = messages_ncols();
    const char *renamed[16];
    for (size_t i = 0; i < n; i++) {
        renamed[i] = HYP_MULTICA_MESSAGES_COLUMNS[i];
    }
    renamed[2] = "sequence"; /* seq, renamed upstream */

    fix_rows_ctx_t ctx;
    hyp_feed_source_t src;
    hyp_multica_rows_t rows = fix_rows(&ctx, renamed, n, MSG_FIXTURE, 3);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_ERR_SCHEMA);

    /* One column short. */
    rows = fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, n - 1, MSG_FIXTURE, 3);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_ERR_SCHEMA);

    /* One column extra: refused just as hard — a result set we did not ask
     * for is not an answer to our SQL. */
    const char *widened[17];
    for (size_t i = 0; i < n; i++) {
        widened[i] = HYP_MULTICA_MESSAGES_COLUMNS[i];
    }
    widened[n] = "surprise";
    rows = fix_rows(&ctx, widened, n + 1, MSG_FIXTURE, 3);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_ERR_SCHEMA);

    /* Reordered. */
    const char *swapped[16];
    for (size_t i = 0; i < n; i++) {
        swapped[i] = HYP_MULTICA_MESSAGES_COLUMNS[i];
    }
    swapped[0] = HYP_MULTICA_MESSAGES_COLUMNS[1];
    swapped[1] = HYP_MULTICA_MESSAGES_COLUMNS[0];
    rows = fix_rows(&ctx, swapped, n, MSG_FIXTURE, 3);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_ERR_SCHEMA);
    PASS();
}

TEST(multica_messages_map_as_documented) {
    fix_rows_ctx_t ctx;
    hyp_multica_rows_t rows =
        fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), MSG_FIXTURE, 3);
    hyp_feed_source_t src;
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);

    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_ingest_stats_t stats;
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_OK);
    hyp_feed_source_close(&src);
    ASSERT_EQ(stats.items, 3);
    ASSERT_EQ(stats.built, 3);
    ASSERT_EQ(hyp_record_set_count(store), 3);

    const hyp_record_t *m1 = find_by_origin(store, "multica:task_message:m1");
    ASSERT_NOT_NULL(m1);
    ASSERT_EQ((int)m1->kind, (int)HYP_RECORD_MESSAGE);
    ASSERT_STR_EQ(m1->author, "multica:agent:opus");
    ASSERT_STR_EQ(m1->thread, "multica:task:t1");
    ASSERT_EQ(m1->timestamp_ms, INT64_C(1700000000001));
    ASSERT_STR_EQ(m1->content, "type=message\ncontent:\nhello");
    ASSERT_NULL(m1->parent); /* first message of an undelegated task */
    ASSERT_EQ(m1->redactions, 0);

    /* The composition rule, with tool, input and output present. */
    const hyp_record_t *m2 = find_by_origin(store, "multica:task_message:m2");
    ASSERT_NOT_NULL(m2);
    ASSERT_STR_EQ(m2->content, "type=tool_use tool=grep\ninput:\n{\"q\":1}\noutput:\nok");
    ASSERT_NOT_NULL(m2->parent);
    ASSERT_STR_EQ(m2->parent, m1->id); /* within-task predecessor */

    /* The delegated task's first message points at the parent task's first. */
    const hyp_record_t *m3 = find_by_origin(store, "multica:task_message:m3");
    ASSERT_NOT_NULL(m3);
    ASSERT_STR_EQ(m3->author, "multica:agent:haiku");
    ASSERT_STR_EQ(m3->thread, "multica:task:t2");
    ASSERT_NOT_NULL(m3->parent);
    ASSERT_STR_EQ(m3->parent, m1->id);

    hyp_record_set_free(store);
    PASS();
}

TEST(multica_null_policy_is_the_stated_one) {
    /* One row per policy line: unowned, no timestamp, empty payload — each a
     * NAMED skip — and an identity-less row, which refuses the whole pull. */
    static const char *const POLICY_FIXTURE[] = {
        /* clang-format off */
        MSG_ROW("p1", NULL, NULL, "message", NULL, "orphan", NULL, NULL, "1700000000001",
                NULL, NULL, NULL, NULL, NULL, "false"),
        MSG_ROW("p2", "t1", "1", "message", NULL, "kept", NULL, NULL, NULL,
                "opus", NULL, NULL, NULL, NULL, "false"),
        MSG_ROW("p3", "t1", "2", "message", NULL, NULL, NULL, NULL, "1700000000002",
                "opus", NULL, NULL, NULL, NULL, "false"),
        MSG_ROW("p4", "t1", "3", "message", NULL, "alive", NULL, NULL, "1700000000003",
                "opus", NULL, NULL, NULL, NULL, "false"),
        /* clang-format on */
    };
    fix_rows_ctx_t ctx;
    hyp_multica_rows_t rows =
        fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), POLICY_FIXTURE, 4);
    hyp_feed_source_t src;
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);

    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_audit_t report;
    hyp_feed_source_t audit_src;
    /* Its OWN cursor. Sharing `ctx` with the ingest above would leave the audit
     * reading a rowset the ingest had already drained — an EMPTY pull, which
     * reports "complete" for the most vacuous of reasons. */
    fix_rows_ctx_t audit_ctx;
    hyp_multica_rows_t audit_rows =
        fix_rows(&audit_ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), POLICY_FIXTURE, 4);

    hyp_feed_ingest_stats_t stats;
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_OK);
    hyp_feed_source_close(&src);
    ASSERT_EQ(stats.items, 1);   /* only p4 became a record */
    ASSERT_EQ(stats.skipped, 3); /* each decline is on the books */
    const hyp_record_t *p4 = find_by_origin(store, "multica:task_message:p4");
    ASSERT_NOT_NULL(p4);
    /* p2 and p3 were skipped, so p4 has no precursor to point at. */
    ASSERT_NULL(p4->parent);

    /* The audit tells "declined by policy" from "lost", by name and reason. */
    ASSERT_EQ(hyp_multica_messages_open(&audit_rows, &audit_src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_audit_run(&audit_src, store, &report), HYP_FEED_OK);
    hyp_feed_source_close(&audit_src);
    ASSERT_TRUE(hyp_feed_audit_complete(&report));
    /* "Complete" over an empty pull is not the same answer: name what the audit
     * actually weighed before believing it. */
    ASSERT_EQ(report.expected, 1);
    ASSERT_EQ(report.present, 1);
    ASSERT_EQ(report.skipped_count, 3);
    ASSERT_STR_EQ(report.skipped[0], "multica:task_message:p1");
    ASSERT_STR_EQ(report.skip_reasons[0], HYP_MULTICA_SKIP_UNOWNED);
    ASSERT_STR_EQ(report.skipped[1], "multica:task_message:p2");
    ASSERT_STR_EQ(report.skip_reasons[1], HYP_MULTICA_SKIP_NO_TIMESTAMP);
    ASSERT_STR_EQ(report.skipped[2], "multica:task_message:p3");
    ASSERT_STR_EQ(report.skip_reasons[2], HYP_MULTICA_SKIP_EMPTY);
    hyp_feed_audit_dispose(&report);

    /* A row with no identity cannot even be skipped by name: the pull dies. */
    static const char *const NAMELESS_FIXTURE[] = {
        MSG_ROW(NULL, "t1", "1", "message", NULL, "x", NULL, NULL, "1700000000001", "opus",
                NULL, NULL, NULL, NULL, "false"),
    };
    rows = fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), NAMELESS_FIXTURE, 1);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_SOURCE);
    hyp_feed_source_close(&src);

    hyp_record_set_free(store);
    PASS();
}

TEST(multica_attribution_fails_closed) {
    /* The named rule, directly. */
    char author[HYP_RECORD_MAX_AUTHOR + 1];
    ASSERT_EQ(hyp_multica_author_of("opus", 1, author, sizeof(author)), HYP_FEED_OK);
    ASSERT_STR_EQ(author, "multica:agent:opus");
    ASSERT_EQ(hyp_multica_author_of(NULL, 0, author, sizeof(author)), HYP_FEED_OK);
    ASSERT_STR_EQ(author, "multica:agent:unattributed");
    ASSERT_EQ(hyp_multica_author_of(NULL, 1, author, sizeof(author)), HYP_FEED_ERR_SOURCE);
    /* Unknown is treated as TRUE: fail closed is the default, not the
     * fallback. */
    ASSERT_EQ(hyp_multica_author_of(NULL, -1, author, sizeof(author)), HYP_FEED_ERR_SOURCE);
    ASSERT_EQ(hyp_multica_author_of("", 0, author, sizeof(author)), HYP_FEED_OK);
    ASSERT_STR_EQ(author, "multica:agent:unattributed"); /* empty is absent */

    ASSERT_EQ(hyp_multica_issue_author_of("member", "u-1", -1, author, sizeof(author)),
              HYP_FEED_OK);
    ASSERT_STR_EQ(author, "multica:member:u-1");
    ASSERT_EQ(hyp_multica_issue_author_of("member", NULL, 0, author, sizeof(author)),
              HYP_FEED_OK);
    ASSERT_STR_EQ(author, "multica:creator:unattributed");
    ASSERT_EQ(hyp_multica_issue_author_of(NULL, NULL, 1, author, sizeof(author)),
              HYP_FEED_ERR_SOURCE);
    ASSERT_EQ(hyp_multica_issue_author_of(NULL, NULL, -1, author, sizeof(author)),
              HYP_FEED_ERR_SOURCE);

    /* And through the wire: an unattributable row under flag true — or under
     * an UNKNOWN flag — refuses the pull; under flag false it lands with the
     * named default. */
    static const char *const UNATTRIBUTED_TRUE[] = {
        MSG_ROW("m1", "t1", "1", "message", NULL, "x", NULL, NULL, "1700000000001", NULL,
                NULL, NULL, NULL, NULL, "true"),
    };
    static const char *const UNATTRIBUTED_UNKNOWN[] = {
        MSG_ROW("m1", "t1", "1", "message", NULL, "x", NULL, NULL, "1700000000001", NULL,
                NULL, NULL, NULL, NULL, NULL),
    };
    static const char *const UNATTRIBUTED_FALSE[] = {
        MSG_ROW("m1", "t1", "1", "message", NULL, "x", NULL, NULL, "1700000000001", NULL,
                NULL, NULL, NULL, NULL, "false"),
    };
    fix_rows_ctx_t ctx;
    hyp_feed_source_t src;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);

    hyp_multica_rows_t rows =
        fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), UNATTRIBUTED_TRUE, 1);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_SOURCE);
    hyp_feed_source_close(&src);
    ASSERT_EQ(hyp_record_set_count(store), 0);

    rows = fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), UNATTRIBUTED_UNKNOWN, 1);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_SOURCE);
    hyp_feed_source_close(&src);

    rows = fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), UNATTRIBUTED_FALSE, 1);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_OK);
    hyp_feed_source_close(&src);
    const hyp_record_t *rec = find_by_origin(store, "multica:task_message:m1");
    ASSERT_NOT_NULL(rec);
    ASSERT_STR_EQ(rec->author, "multica:agent:unattributed");

    hyp_record_set_free(store);
    PASS();
}

TEST(multica_order_pin_refuses_a_wire_that_reorders) {
    /* seq going backwards within a task: not our ORDER BY, not our SQL. */
    static const char *const BACKWARDS[] = {
        MSG_ROW("m2", "t1", "2", "message", NULL, "b", NULL, NULL, "1700000000002", "opus",
                NULL, NULL, NULL, NULL, "false"),
        MSG_ROW("m1", "t1", "1", "message", NULL, "a", NULL, NULL, "1700000000001", "opus",
                NULL, NULL, NULL, NULL, "false"),
    };
    /* A task's group split by another task's rows. */
    static const char *const REGROUPED[] = {
        MSG_ROW("m1", "t1", "1", "message", NULL, "a", NULL, NULL, "1700000000001", "opus",
                NULL, NULL, NULL, NULL, "false"),
        MSG_ROW("m2", "t2", "1", "message", NULL, "b", NULL, NULL, "1700000000002", "opus",
                NULL, NULL, NULL, NULL, "false"),
        MSG_ROW("m3", "t1", "2", "message", NULL, "c", NULL, NULL, "1700000000003", "opus",
                NULL, NULL, NULL, NULL, "false"),
    };
    fix_rows_ctx_t ctx;
    hyp_feed_source_t src;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);

    hyp_multica_rows_t rows =
        fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), BACKWARDS, 2);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_SOURCE);
    hyp_feed_source_close(&src);
    ASSERT_EQ(hyp_record_set_count(store), 0);

    rows = fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), REGROUPED, 3);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_SOURCE);
    hyp_feed_source_close(&src);

    /* And a source that dies mid-pull aborts the pull, atomically. */
    hyp_multica_rows_t dying =
        fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), MSG_FIXTURE, 3);
    ctx.fail_at = 2;
    ASSERT_EQ(hyp_multica_messages_open(&dying, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_SOURCE);
    hyp_feed_source_close(&src);
    ASSERT_EQ(hyp_record_set_count(store), 0);

    hyp_record_set_free(store);
    PASS();
}

TEST(multica_end_to_end_audit_names_the_lost_row) {
    /* Ingest all three fixture rows, audit: complete. Rebuild a store missing
     * m2, audit the full feed: exactly m2's origin is named. */
    fix_rows_ctx_t ctx;
    hyp_feed_source_t src;
    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);

    hyp_multica_rows_t rows =
        fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), MSG_FIXTURE, 3);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_OK);
    hyp_feed_source_close(&src);

    hyp_feed_audit_t report;
    rows = fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), MSG_FIXTURE, 3);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_audit_run(&src, store, &report), HYP_FEED_OK);
    hyp_feed_source_close(&src);
    ASSERT_TRUE(hyp_feed_audit_complete(&report));
    ASSERT_EQ(report.expected, 3);
    ASSERT_STR_EQ(report.feed, "multica:task_message");
    hyp_feed_audit_dispose(&report);

    /* The lossy store: rows m1 and m3 only. */
    static const char *const WITHOUT_M2[] = {
        MSG_ROW("m1", "t1", "1", "message", NULL, "hello", NULL, NULL, "1700000000001",
                "opus", NULL, NULL, NULL, NULL, "false"),
        MSG_ROW("m3", "t2", "1", "message", NULL, "child says hi", NULL, NULL,
                "1700000000003", "haiku", "t1", "iss-9", "sess-7", "m1", "false"),
    };
    hyp_record_set_t *lossy = hyp_record_set_create();
    ASSERT_NOT_NULL(lossy);
    rows = fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), WITHOUT_M2, 2);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, lossy, NULL), HYP_FEED_OK);
    hyp_feed_source_close(&src);

    rows = fix_rows(&ctx, HYP_MULTICA_MESSAGES_COLUMNS, messages_ncols(), MSG_FIXTURE, 3);
    ASSERT_EQ(hyp_multica_messages_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_audit_run(&src, lossy, &report), HYP_FEED_OK);
    hyp_feed_source_close(&src);
    ASSERT_FALSE(hyp_feed_audit_complete(&report));
    ASSERT_EQ(report.missing_count, 1);
    ASSERT_STR_EQ(report.missing[0], "multica:task_message:m2");
    hyp_feed_audit_dispose(&report);

    hyp_record_set_free(store);
    hyp_record_set_free(lossy);
    PASS();
}

/* ── The issues feed ────────────────────────────────────────────────────── */

static size_t issues_ncols(void) {
    size_t n = 0;
    while (HYP_MULTICA_ISSUES_COLUMNS[n]) {
        n++;
    }
    return n;
}

#define ISSUE_ROW(id, ws, title, desc, status, prio, num, ctype, cid, parent, when, flag) \
    id, ws, title, desc, status, prio, num, ctype, cid, parent, when, flag

TEST(multica_issues_map_and_pin_their_enums) {
    static const char *const ISSUE_FIXTURE[] = {
        /* clang-format off */
        ISSUE_ROW("i1", "w1", "fix the gate", "it lies", "todo", "high", "12",
                  "member", "u-1", NULL, "1700000000004", "false"),
        ISSUE_ROW("i2", "w1", "follow-up", NULL, "backlog", "none", "13",
                  "agent", "a-1", "i1", "1700000000005", "false"),
        ISSUE_ROW("i3", "w1", NULL, NULL, "todo", "low", "14",
                  "member", "u-1", NULL, "1700000000006", "false"),
        /* clang-format on */
    };
    fix_rows_ctx_t ctx;
    hyp_feed_source_t src;
    hyp_multica_rows_t rows =
        fix_rows(&ctx, HYP_MULTICA_ISSUES_COLUMNS, issues_ncols(), ISSUE_FIXTURE, 3);
    ASSERT_EQ(hyp_multica_issues_open(&rows, &src), HYP_FEED_OK);

    hyp_record_set_t *store = hyp_record_set_create();
    ASSERT_NOT_NULL(store);
    hyp_feed_ingest_stats_t stats;
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, &stats), HYP_FEED_OK);
    hyp_feed_source_close(&src);
    ASSERT_EQ(stats.items, 2);
    ASSERT_EQ(stats.skipped, 1); /* i3: nothing worth a record */

    const hyp_record_t *i1 = find_by_origin(store, "multica:issue:i1");
    ASSERT_NOT_NULL(i1);
    ASSERT_EQ((int)i1->kind, (int)HYP_RECORD_SIGNAL);
    ASSERT_STR_EQ(i1->author, "multica:member:u-1");
    ASSERT_STR_EQ(i1->thread, "multica:issue:i1"); /* heads its own thread */
    ASSERT_STR_EQ(i1->content,
                  "title=fix the gate\nstatus=todo priority=high number=12\n\nit lies");
    ASSERT_NULL(i1->parent);

    const hyp_record_t *i2 = find_by_origin(store, "multica:issue:i2");
    ASSERT_NOT_NULL(i2);
    ASSERT_STR_EQ(i2->author, "multica:agent:a-1");
    ASSERT_STR_EQ(i2->content, "title=follow-up\nstatus=backlog priority=none number=13");
    ASSERT_NOT_NULL(i2->parent);
    ASSERT_STR_EQ(i2->parent, i1->id); /* parent-issue lineage */

    /* A status outside the pinned CHECK enum is schema drift, refused. */
    static const char *const DRIFTED[] = {
        ISSUE_ROW("i1", "w1", "t", NULL, "someday-maybe", "high", "1", "member", "u-1", NULL,
                  "1700000000004", "false"),
    };
    rows = fix_rows(&ctx, HYP_MULTICA_ISSUES_COLUMNS, issues_ncols(), DRIFTED, 1);
    ASSERT_EQ(hyp_multica_issues_open(&rows, &src), HYP_FEED_OK);
    ASSERT_EQ(hyp_feed_ingest(&src, hyp_scrub_text, store, NULL), HYP_FEED_ERR_SCHEMA);
    hyp_feed_source_close(&src);

    hyp_record_set_free(store);
    PASS();
}

/* ── The workspace pull ─────────────────────────────────────────────────── */

static size_t workspaces_ncols(void) {
    size_t n = 0;
    while (HYP_MULTICA_WORKSPACES_COLUMNS[n]) {
        n++;
    }
    return n;
}

TEST(multica_workspaces_pull_typed_rows_and_pin_repos_shape) {
    static const char *const WS_FIXTURE[] = {
        "w1", "team", "team-slug", "[{\"path\":\"/srv/a\"}]", "TM", "true",
        "w2", NULL,   NULL,        NULL,                      NULL, NULL,
    };
    fix_rows_ctx_t ctx;
    hyp_multica_rows_t rows =
        fix_rows(&ctx, HYP_MULTICA_WORKSPACES_COLUMNS, workspaces_ncols(), WS_FIXTURE, 2);

    hyp_multica_workspace_t *items = NULL;
    size_t count = 0;
    ASSERT_EQ(hyp_multica_workspaces_pull(&rows, &items, &count), HYP_FEED_OK);
    ASSERT_EQ(count, 2);
    ASSERT_STR_EQ(items[0].id, "w1");
    ASSERT_STR_EQ(items[0].name, "team");
    ASSERT_STR_EQ(items[0].slug, "team-slug");
    ASSERT_STR_EQ(items[0].repos_json, "[{\"path\":\"/srv/a\"}]");
    ASSERT_STR_EQ(items[0].issue_prefix, "TM");
    ASSERT_EQ(items[0].attribution_fail_closed, 1); /* surfaced, as asked */
    ASSERT_STR_EQ(items[1].id, "w2");
    ASSERT_NULL(items[1].name);
    ASSERT_NULL(items[1].repos_json);
    ASSERT_EQ(items[1].attribution_fail_closed, -1); /* unknown stays unknown */
    hyp_multica_workspaces_free(items, count);

    /* The declared (UNVERIFIED) inner pin: repos must be a JSON array. A
     * different shape refuses rather than yielding something half-read. */
    static const char *const BAD_REPOS[] = {
        "w1", NULL, NULL, "{\"not\":\"an array\"}", NULL, NULL,
    };
    rows = fix_rows(&ctx, HYP_MULTICA_WORKSPACES_COLUMNS, workspaces_ncols(), BAD_REPOS, 1);
    ASSERT_EQ(hyp_multica_workspaces_pull(&rows, &items, &count), HYP_FEED_ERR_SCHEMA);
    ASSERT_NULL(items);

    static const char *const NAMELESS[] = {
        NULL, NULL, NULL, NULL, NULL, NULL,
    };
    rows = fix_rows(&ctx, HYP_MULTICA_WORKSPACES_COLUMNS, workspaces_ncols(), NAMELESS, 1);
    ASSERT_EQ(hyp_multica_workspaces_pull(&rows, &items, &count), HYP_FEED_ERR_SOURCE);
    PASS();
}

/* ── The schema pin ─────────────────────────────────────────────────────── */

/* The catalog fixture is spelled here INDEPENDENTLY of the adapter's own
 * tables — two descriptions of the schema, and the check is that they agree.
 * Copying the adapter's arrays in would pin whatever the adapter does, bug
 * included. */
static const char *const SCHEMA_OK_NAMES[] = {"pin_kind", "pin_name", "pin_detail"};

#define COL(name) "column", name, ""
#define FK(from, to) "fk", from, to

static const char *const SCHEMA_OK_CELLS[] = {
    COL("task_message.id"), COL("task_message.task_id"), COL("task_message.seq"),
    COL("task_message.type"), COL("task_message.tool"), COL("task_message.content"),
    COL("task_message.input"), COL("task_message.output"), COL("task_message.created_at"),
    COL("issue.id"), COL("issue.workspace_id"), COL("issue.title"), COL("issue.description"),
    COL("issue.status"), COL("issue.priority"), COL("issue.assignee_type"),
    COL("issue.assignee_id"), COL("issue.creator_type"), COL("issue.creator_id"),
    COL("issue.parent_issue_id"), COL("issue.acceptance_criteria"), COL("issue.context_refs"),
    COL("issue.position"), COL("issue.due_date"), COL("issue.created_at"),
    COL("issue.updated_at"), COL("issue.number"), COL("issue.project_id"),
    COL("issue.origin_type"), COL("issue.origin_id"), COL("issue.first_executed_at"),
    COL("issue.start_date"), COL("issue.metadata"), COL("issue.stage"), COL("issue.properties"),
    COL("workspace.id"), COL("workspace.name"), COL("workspace.slug"),
    COL("workspace.description"), COL("workspace.settings"), COL("workspace.created_at"),
    COL("workspace.updated_at"), COL("workspace.context"), COL("workspace.repos"),
    COL("workspace.issue_prefix"), COL("workspace.issue_counter"), COL("workspace.avatar_url"),
    COL("workspace.attribution_fail_closed"),
    COL("agent_task_queue.id"), COL("agent_task_queue.agent_id"),
    COL("agent_task_queue.parent_task_id"), COL("agent_task_queue.issue_id"),
    COL("agent_task_queue.session_id"), COL("agent_task_queue.work_dir"),
    COL("agent_task_queue.started_at"), COL("agent_task_queue.completed_at"),
    COL("agent_task_queue.result"), COL("agent_task_queue.status"),
    /* IGNORED orchestration columns: present upstream, tolerated here. */
    COL("agent_task_queue.delegated_from_task_id"), COL("agent_task_queue.retry_of_task_id"),
    COL("agent_task_queue.rerun_of_task_id"),
    COL("agent.id"), COL("agent.name"), COL("agent.workspace_id"), COL("agent.kind"),
    COL("agent.model"),
    FK("task_message.task_id", "agent_task_queue.id"),
    FK("agent_task_queue.agent_id", "agent.id"),
    FK("agent_task_queue.issue_id", "issue.id"),
    FK("agent_task_queue.parent_task_id", "agent_task_queue.id"),
    /* A foreign key we do not depend on: required none the less to be
     * TOLERATED — we pin ours, we do not forbid theirs. */
    FK("issue.workspace_id", "workspace.id"),
};

enum { SCHEMA_OK_ROWS = sizeof(SCHEMA_OK_CELLS) / sizeof(SCHEMA_OK_CELLS[0]) / 3 };

/* Copy the catalog fixture, dropping every row that names `drop`. */
static size_t catalog_without(const char **dst, const char *drop) {
    size_t kept = 0;
    for (size_t r = 0; r < (size_t)SCHEMA_OK_ROWS; r++) {
        if (strcmp(SCHEMA_OK_CELLS[r * 3 + 1], drop) == 0) {
            continue;
        }
        dst[kept * 3] = SCHEMA_OK_CELLS[r * 3];
        dst[kept * 3 + 1] = SCHEMA_OK_CELLS[r * 3 + 1];
        dst[kept * 3 + 2] = SCHEMA_OK_CELLS[r * 3 + 2];
        kept++;
    }
    return kept;
}

TEST(multica_schema_pin_separates_required_from_ignored) {
    /*
     * Nothing was verified about `agent` beyond id and name, so nothing more is
     * REQUIRED there. This is the direction that is easy to get wrong in the
     * safe-looking way: pinning a column nobody confirmed exists does not make
     * the adapter stricter, it makes it refuse a perfectly correct schema — and
     * then every row, forever, with a message blaming upstream drift.
     */
    const char *cells[SCHEMA_OK_ROWS * 3];
    fix_rows_ctx_t ctx;
    size_t kept = catalog_without(cells, "agent.kind");
    hyp_multica_rows_t rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, cells, kept);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_OK);
    kept = catalog_without(cells, "agent.model");
    rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, cells, kept);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_OK);

    /* And the other direction, or the paragraph above is just a smaller pin:
     * the three columns the mapping does ride on are still required. */
    kept = catalog_without(cells, "agent.name");
    rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, cells, kept);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_ERR_SCHEMA);
    kept = catalog_without(cells, "agent.workspace_id");
    rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, cells, kept);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_ERR_SCHEMA);
    kept = catalog_without(cells, "agent.id");
    rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, cells, kept);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_ERR_SCHEMA);
    PASS();
}

TEST(multica_schema_pin_accepts_the_verified_catalog) {
    fix_rows_ctx_t ctx;
    hyp_multica_rows_t rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, SCHEMA_OK_CELLS, SCHEMA_OK_ROWS);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_OK);
    PASS();
}

TEST(multica_schema_pin_fails_closed_on_drift) {
    /* A load-bearing foreign key gone: the exact silent failure the pin
     * exists for — a LEFT JOIN over an unkeyed table would not error, it
     * would return NULL agents forever. Drop the last FK-but-one (ours) by
     * copying the fixture without that row. */
    const char *cells[SCHEMA_OK_ROWS * 3];
    size_t kept = 0;
    for (size_t r = 0; r < (size_t)SCHEMA_OK_ROWS; r++) {
        const char *kind = SCHEMA_OK_CELLS[r * 3];
        const char *name = SCHEMA_OK_CELLS[r * 3 + 1];
        if (strcmp(kind, "fk") == 0 && strcmp(name, "task_message.task_id") == 0) {
            continue;
        }
        cells[kept * 3] = SCHEMA_OK_CELLS[r * 3];
        cells[kept * 3 + 1] = SCHEMA_OK_CELLS[r * 3 + 1];
        cells[kept * 3 + 2] = SCHEMA_OK_CELLS[r * 3 + 2];
        kept++;
    }
    fix_rows_ctx_t ctx;
    hyp_multica_rows_t rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, cells, kept);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_ERR_SCHEMA);

    /* A verified column gone from an exactly pinned table. */
    kept = 0;
    for (size_t r = 0; r < (size_t)SCHEMA_OK_ROWS; r++) {
        if (strcmp(SCHEMA_OK_CELLS[r * 3 + 1], "task_message.seq") == 0) {
            continue;
        }
        cells[kept * 3] = SCHEMA_OK_CELLS[r * 3];
        cells[kept * 3 + 1] = SCHEMA_OK_CELLS[r * 3 + 1];
        cells[kept * 3 + 2] = SCHEMA_OK_CELLS[r * 3 + 2];
        kept++;
    }
    rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, cells, kept);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_ERR_SCHEMA);

    /* An EXTRA column on an exactly pinned table: drift in the other
     * direction, refused just as hard. */
    const char *widened[(SCHEMA_OK_ROWS + 1) * 3];
    memcpy(widened, SCHEMA_OK_CELLS, sizeof(SCHEMA_OK_CELLS));
    widened[SCHEMA_OK_ROWS * 3] = "column";
    widened[SCHEMA_OK_ROWS * 3 + 1] = "task_message.surprise";
    widened[SCHEMA_OK_ROWS * 3 + 2] = "";
    rows = fix_rows(&ctx, SCHEMA_OK_NAMES, 3, widened, SCHEMA_OK_ROWS + 1);
    ASSERT_EQ(hyp_multica_schema_pin_check(&rows), HYP_FEED_ERR_SCHEMA);
    PASS();
}

/* ── The SQL and the declaration, compared to EACH OTHER ────────────────── */

/*
 * Every other pin test here feeds HYP_MULTICA_*_COLUMNS into a fixture and
 * then asserts the pin against that same array — the pin proven against
 * itself. The `AS` aliases in the SQL are the only thing deciding what a live
 * wire returns, and nothing read them. Rename one alias and the adapter
 * refuses every real pull forever with ERR_SCHEMA, blaming upstream drift for
 * a typo of ours, while every test in this file still prints PASS.
 *
 * So: parse the aliases out of the SQL text and compare them, in order,
 * against the declaration. Two artifacts, compared to each other. A third
 * hand-written list here would only be one more thing to keep in step.
 */

#define ALIAS_MAX 32
#define ALIAS_LEN 64

/* Explicit byte class, never <ctype.h>: a parser whose answer depends on the
 * machine's locale is not a pin. */
static bool alias_byte(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/*
 * The aliases a SQL statement's top-level select list declares, in order.
 * Returns 0 for "could not parse", and the caller treats 0 as a failure: a
 * parser that silently found nothing would make the comparison below
 * vacuously green, which is precisely the shape of defect it exists to close.
 * The select list ends at the first line-initial FROM — `extract(epoch FROM
 * ...)` is inside a call, never at the start of a line.
 */
static size_t sql_select_aliases(const char *sql, char out[][ALIAS_LEN], size_t cap) {
    const char *end = strstr(sql, "\nFROM ");
    if (!end) {
        return 0;
    }
    size_t n = 0;
    for (const char *p = sql; p + 3 <= end; p++) {
        if (!(p[0] == 'A' && p[1] == 'S' && p[2] == ' ')) {
            continue;
        }
        if (p != sql && p[-1] != ' ' && p[-1] != '\n') {
            continue; /* part of a longer word, not the keyword */
        }
        const char *id = p + 3;
        size_t len = 0;
        while (id[len] && alias_byte(id[len])) {
            len++;
        }
        if (len == 0 || len >= ALIAS_LEN || n >= cap) {
            return 0; /* fail closed rather than report a partial reading */
        }
        memcpy(out[n], id, len);
        out[n][len] = '\0';
        n++;
    }
    return n;
}

TEST(multica_sql_aliases_agree_with_the_pinned_columns) {
    char probe[ALIAS_MAX][ALIAS_LEN];

    /* The parser must be able to say NO, or "it found everything" is not
     * evidence about anything. */
    ASSERT_EQ(sql_select_aliases("SELECT 1", probe, ALIAS_MAX), 0);
    ASSERT_EQ(sql_select_aliases("SELECT x AS \nFROM t\n", probe, ALIAS_MAX), 0);
    ASSERT_EQ(sql_select_aliases(HYP_MULTICA_MESSAGES_SQL, probe, 2), 0); /* capped */

    const struct {
        const char *what;
        const char *sql;
        const char *const *declared;
    } pairs[] = {
        {"messages", HYP_MULTICA_MESSAGES_SQL, HYP_MULTICA_MESSAGES_COLUMNS},
        {"issues", HYP_MULTICA_ISSUES_SQL, HYP_MULTICA_ISSUES_COLUMNS},
        {"workspaces", HYP_MULTICA_WORKSPACES_SQL, HYP_MULTICA_WORKSPACES_COLUMNS},
        {"schema pin", HYP_MULTICA_SCHEMA_PIN_SQL, HYP_MULTICA_SCHEMA_PIN_COLUMNS},
    };

    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        char aliases[ALIAS_MAX][ALIAS_LEN];
        size_t parsed = sql_select_aliases(pairs[i].sql, aliases, ALIAS_MAX);
        if (parsed == 0) {
            printf("\n    the %s SQL yielded no aliases — the parse failed, so this "
                   "comparison would prove nothing\n",
                   pairs[i].what);
        }
        ASSERT_GT(parsed, 0);

        size_t declared = 0;
        while (pairs[i].declared[declared]) {
            declared++;
        }
        if (parsed != declared) {
            printf("\n    the %s SQL declares %zu aliases, the pinned column list has %zu\n",
                   pairs[i].what, parsed, declared);
        }
        ASSERT_EQ(parsed, declared);

        for (size_t c = 0; c < declared; c++) {
            if (strcmp(aliases[c], pairs[i].declared[c]) != 0) {
                printf("\n    the %s SQL and its pinned columns disagree at position %zu\n",
                       pairs[i].what, c);
            }
            ASSERT_STR_EQ(aliases[c], pairs[i].declared[c]);
        }
    }
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(feed) {
    /* Statuses */
    RUN_TEST(feed_status_reasons_are_present_and_distinct);
    /* The boundary, driven by the toy adapter */
    RUN_TEST(feed_ingest_builds_records_and_reingest_is_idempotent);
    RUN_TEST(feed_ingest_is_atomic);
    RUN_TEST(feed_ingest_requires_an_origin);
    RUN_TEST(feed_ingest_resolves_precursors_in_any_yield_order);
    RUN_TEST(feed_ingest_refuses_an_unknown_precursor);
    RUN_TEST(feed_ingest_refuses_one_origin_with_two_contents);
    RUN_TEST(feed_skip_notices_are_counted_and_break_no_chain);
    /* D4: the audit */
    RUN_TEST(feed_audit_complete_with_identical_rows_and_names_the_missing_one);
    RUN_TEST(feed_audit_reports_skips_separately_from_loss);
    RUN_TEST(feed_audit_is_per_feed_never_global);
    RUN_TEST(feed_refuses_nothing_quietly);
    /* The Multica adapter */
    RUN_TEST(multica_pin_accepts_its_own_columns);
    RUN_TEST(multica_pin_fails_closed_on_any_column_mismatch);
    RUN_TEST(multica_messages_map_as_documented);
    RUN_TEST(multica_null_policy_is_the_stated_one);
    RUN_TEST(multica_attribution_fails_closed);
    RUN_TEST(multica_order_pin_refuses_a_wire_that_reorders);
    RUN_TEST(multica_end_to_end_audit_names_the_lost_row);
    RUN_TEST(multica_issues_map_and_pin_their_enums);
    RUN_TEST(multica_workspaces_pull_typed_rows_and_pin_repos_shape);
    RUN_TEST(multica_schema_pin_accepts_the_verified_catalog);
    RUN_TEST(multica_schema_pin_separates_required_from_ignored);
    RUN_TEST(multica_schema_pin_fails_closed_on_drift);
    RUN_TEST(multica_sql_aliases_agree_with_the_pinned_columns);
}
