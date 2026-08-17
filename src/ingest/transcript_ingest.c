/*
 * transcript_ingest.c — the transcript path: pull, scrub, build, merge. See
 * transcript_ingest.h for the contract, and in particular for the mechanisms
 * that make an unscrubbed transcript record impossible to express rather than
 * merely refused.
 *
 * WHAT THIS FILE DOES NOT DO IS THE POINT. It does not drain a source, does
 * not resolve precursors, does not detect a repeated origin, and does not
 * construct a record. All of that is the feed boundary's, which is where the
 * scrub gate lives; a second copy here would be a second derivation of the
 * same rules, free to drift, and the one it could drift away from is the gate.
 * What is added here is what the boundary deliberately does not know: that a
 * transcript is a conversation of MESSAGES, and that it belongs in a durable
 * store rather than in memory.
 *
 * Two structural properties of THIS FILE, enforced at source level by
 * tests/test_transcript_contract.sh because a C test cannot assert either
 * about itself:
 *
 *   1. NO RECORD BUILDER. Nothing on the transcript path constructs a record
 *      except through the scrub seam. The gate checks this file and the feed
 *      boundary together, since together they are the path.
 *   2. NO CLOCK, NO PARSING, NO FEED VOCABULARY. An item's timestamp is the
 *      source's event time; origin and thread are byte strings that are copied
 *      and compared, never looked inside.
 */
#include "ingest/transcript_ingest.h"

#include <stdlib.h>
#include <string.h>

const char *hyp_transcript_status_reason(hyp_transcript_status_t status) {
    switch (status) {
    case HYP_TRANSCRIPT_OK:
        return "ok";
    case HYP_TRANSCRIPT_ERR_NULL:
        return "a required argument was NULL";
    case HYP_TRANSCRIPT_ERR_NO_SCRUBBER:
        return "no scrubber was wired, so no transcript record may be built";
    case HYP_TRANSCRIPT_ERR_KIND:
        return "an item arrived that is not a transcript message";
    case HYP_TRANSCRIPT_ERR_SOURCE:
        return "the source failed or contradicted itself mid-pull";
    case HYP_TRANSCRIPT_ERR_SCHEMA:
        return "the source's shape is not the pinned one";
    case HYP_TRANSCRIPT_ERR_ORIGIN:
        return "an item arrived without the source's own name for its row";
    case HYP_TRANSCRIPT_ERR_ITEM:
        return "an item was refused by the record contract";
    case HYP_TRANSCRIPT_ERR_PRECURSOR:
        return "an item names a precursor this pull never yielded";
    case HYP_TRANSCRIPT_ERR_DUPLICATE:
        return "one origin was yielded twice with different contents";
    case HYP_TRANSCRIPT_ERR_STORE:
        return "the durable store refused the merge";
    case HYP_TRANSCRIPT_ERR_ALLOC:
        return "out of memory, or the scrubber refused the text";
    }
    return "unknown transcript status";
}

/* The boundary's refusals, carried through one for one. Collapsing two of them
 * into a single status here would undo the boundary's own rule that no two
 * refusals become one indistinguishable log line. */
static hyp_transcript_status_t from_feed(hyp_feed_status_t st) {
    switch (st) {
    case HYP_FEED_OK:
        return HYP_TRANSCRIPT_OK;
    case HYP_FEED_ERR_NO_SCRUBBER:
        return HYP_TRANSCRIPT_ERR_NO_SCRUBBER;
    case HYP_FEED_ERR_NULL:
        return HYP_TRANSCRIPT_ERR_NULL;
    case HYP_FEED_ERR_SCHEMA:
        return HYP_TRANSCRIPT_ERR_SCHEMA;
    case HYP_FEED_ERR_ITEM:
        return HYP_TRANSCRIPT_ERR_ITEM;
    case HYP_FEED_ERR_ORIGIN:
        return HYP_TRANSCRIPT_ERR_ORIGIN;
    case HYP_FEED_ERR_PRECURSOR:
        return HYP_TRANSCRIPT_ERR_PRECURSOR;
    case HYP_FEED_ERR_DUPLICATE:
        return HYP_TRANSCRIPT_ERR_DUPLICATE;
    case HYP_FEED_ERR_ALLOC:
        return HYP_TRANSCRIPT_ERR_ALLOC;
    case HYP_FEED_END:
    case HYP_FEED_SKIP:
    case HYP_FEED_ERR_SOURCE:
        break;
    }
    /* END and SKIP are yield-level answers a completed ingest never returns.
     * Meeting one means the boundary contradicted its own contract, which is
     * a source problem by any other name. */
    return HYP_TRANSCRIPT_ERR_SOURCE;
}

/* Nothing was merged, so nothing about this run is a fact about the store. The
 * redaction counters would otherwise describe records that do not exist, which
 * reads as evidence that a scrub protected something. */
static void forget_the_run(hyp_transcript_ingest_stats_t *stats) {
    stats->built = 0;
    stats->absorbed = 0;
    stats->redactions = 0;
    stats->items_redacted = 0;
}

hyp_transcript_status_t hyp_transcript_ingest(hyp_feed_source_t *src, hyp_scrub_fn scrub,
                                              hyp_record_store_t *store,
                                              hyp_transcript_ingest_stats_t *stats) {
    hyp_transcript_ingest_stats_t local;
    if (!stats) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));

    /*
     * The scrubber is checked FIRST and separately, before the source is asked
     * for anything. The boundary checks it again — that is not redundancy, it
     * is each surface refusing on its own terms, so neither can be reached
     * through the other with the gate open.
     */
    if (!scrub) {
        return HYP_TRANSCRIPT_ERR_NO_SCRUBBER;
    }
    if (!src || !src->next || !store) {
        return HYP_TRANSCRIPT_ERR_NULL;
    }

    /*
     * Staging is in memory and the store is not touched until every record
     * exists. A half-ingested transcript would make "what do I hold" depend on
     * where an error happened, and a completeness audit would then report a
     * shortfall no re-pull could explain.
     */
    hyp_record_set_t *staging = hyp_record_set_create();
    if (!staging) {
        return HYP_TRANSCRIPT_ERR_ALLOC;
    }

    hyp_feed_ingest_stats_t feed_stats;
    memset(&feed_stats, 0, sizeof(feed_stats));
    hyp_feed_status_t fst = hyp_feed_ingest(src, scrub, staging, &feed_stats);
    stats->items = feed_stats.items;
    stats->skipped = feed_stats.skipped;
    stats->record_status = feed_stats.record_status;
    stats->redactions = feed_stats.redactions;
    stats->items_redacted = feed_stats.items_redacted;
    if (fst != HYP_FEED_OK) {
        hyp_record_set_free(staging);
        forget_the_run(stats);
        return from_feed(fst);
    }

    /*
     * Transcripts are messages, and this is checked on what was BUILT rather
     * than on what an item claimed — the built record is the artifact that
     * would reach the store, so it is the one worth reading. A source yielding
     * any other kind has been wired to the wrong function; relabelling its
     * rows would file text under a kind nobody claimed for it, so the whole
     * pull is refused instead. Nothing is merged, so the refusal costs the
     * caller a re-pull and costs the store nothing.
     */
    for (size_t i = 0;; i++) {
        const hyp_record_t *rec = hyp_record_set_at(staging, i);
        if (!rec) {
            break;
        }
        if (rec->kind != HYP_RECORD_MESSAGE) {
            hyp_record_set_free(staging);
            forget_the_run(stats);
            return HYP_TRANSCRIPT_ERR_KIND;
        }
    }

    size_t added = 0;
    hyp_record_store_status_t sst = hyp_record_store_append_set(store, staging, &added);
    hyp_record_set_free(staging);
    if (sst != HYP_RECORD_STORE_OK) {
        stats->store_status = sst;
        forget_the_run(stats);
        return sst == HYP_RECORD_STORE_ERR_ALLOC ? HYP_TRANSCRIPT_ERR_ALLOC
                                                 : HYP_TRANSCRIPT_ERR_STORE;
    }
    stats->built = added;
    stats->absorbed = stats->items - added;
    return HYP_TRANSCRIPT_OK;
}

/* ── Reading a conversation back ────────────────────────────────────────── */

/* Ascending by event time, ties broken by id. A total order, so two machines
 * holding the same records read the same conversation. */
static int cmp_message(const void *a, const void *b) {
    const hyp_record_t *ra = *(const hyp_record_t *const *)a;
    const hyp_record_t *rb = *(const hyp_record_t *const *)b;
    if (ra->timestamp_ms < rb->timestamp_ms) {
        return -1;
    }
    if (ra->timestamp_ms > rb->timestamp_ms) {
        return 1;
    }
    return strcmp(ra->id, rb->id);
}

hyp_transcript_status_t hyp_transcript_thread_load(hyp_record_store_t *store, const char *thread,
                                                   hyp_transcript_thread_t *out) {
    if (!out) {
        return HYP_TRANSCRIPT_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));
    if (!store || !thread || thread[0] == '\0') {
        /* The empty string is not a thread the contract can store, so asking
         * for it is a mistake rather than a question. */
        return HYP_TRANSCRIPT_ERR_NULL;
    }

    hyp_record_store_query_t query;
    memset(&query, 0, sizeof(query));
    query.has_kind = true;
    query.kind = HYP_RECORD_MESSAGE;
    query.thread = thread; /* exact bytes; the value is the adapter's, not ours */

    hyp_record_set_t *set = NULL;
    hyp_record_store_status_t sst = hyp_record_store_query(store, &query, &set);
    if (sst != HYP_RECORD_STORE_OK) {
        return sst == HYP_RECORD_STORE_ERR_ALLOC ? HYP_TRANSCRIPT_ERR_ALLOC
                                                 : HYP_TRANSCRIPT_ERR_STORE;
    }

    size_t count = hyp_record_set_count(set);
    const hyp_record_t **messages = NULL;
    if (count > 0) {
        messages = (const hyp_record_t **)calloc(count, sizeof(*messages));
        if (!messages) {
            hyp_record_set_free(set);
            return HYP_TRANSCRIPT_ERR_ALLOC;
        }
        for (size_t i = 0; i < count; i++) {
            messages[i] = hyp_record_set_at(set, i);
        }
        qsort(messages, count, sizeof(*messages), cmp_message);
    }

    out->records = set;
    out->messages = (const hyp_record_t *const *)messages;
    out->count = count;
    return HYP_TRANSCRIPT_OK;
}

void hyp_transcript_thread_dispose(hyp_transcript_thread_t *thread) {
    if (!thread) {
        return;
    }
    free((void *)(uintptr_t)(const void *)thread->messages);
    hyp_record_set_free(thread->records);
    memset(thread, 0, sizeof(*thread));
}
