/*
 * transcript_ingest.c — the transcript path: pull, scrub, build, merge. See
 * transcript_ingest.h for the contract, and in particular for the four
 * mechanisms that make an unscrubbed transcript record impossible to express
 * rather than merely refused.
 *
 * Three structural properties of THIS FILE carry that contract, and
 * tests/test_transcript_contract.sh enforces each of them at source level
 * because none of the three can be asserted by a C test about itself:
 *
 *   1. NO hyp_record_build(). The only record constructor reachable from here
 *      is hyp_record_ingest_scrubbed(), which refuses without a scrubber. A
 *      direct call to the builder would be a second door in the one module
 *      whose whole job is that there is one.
 *   2. NO CLOCK. An item's timestamp is the source's event time. A clock read
 *      here would make ids depend on the machine that ingested the row, and a
 *      sync that should be a union would double the store instead.
 *   3. NO PARSING, NO FEED VOCABULARY. origin, thread and parent_origin are
 *      byte strings: copied, compared, never looked inside. A separator found
 *      is a format adopted, and the next adapter would have to translate into
 *      the first one's dialect rather than into ours.
 */
#include "ingest/transcript_ingest.h"

#include "foundation/hash_table.h"

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

/* ── Owned copies of a pull ─────────────────────────────────────────────── */

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

static bool str_equal(const char *a, const char *b) {
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false; /* absent and empty are not the same answer */
    }
    return strcmp(a, b) == 0;
}

/*
 * One staged item. Note what is NOT here: `kind`, because every accepted item
 * is a message and anything else was refused at the door; and `redactions`,
 * because the adapter's claim about its own scrubbing is dropped rather than
 * carried. Dropping it is mechanism 2 of the header, made concrete — there is
 * no field in this struct, and none in hyp_record_ingest_input_t, for such a
 * claim to travel in.
 *
 * `content` is the RAW text. It is scrubbed once, at build time, by the seam
 * that owns the scrubbed copy; scrubbing it here as well would be a second
 * pass whose count is zero (the scrub is idempotent) and would therefore
 * report a cleaned record as verbatim.
 */
typedef struct {
    char *author;
    char *content;
    char *origin;
    char *thread;
    char *parent_origin;
    int64_t timestamp_ms;
    bool built;
} staged_item_t;

typedef struct {
    staged_item_t *items;
    size_t count;
    size_t cap;
    size_t yields;           /* item yields including in-pull repeats of one origin */
    HYPHashTable *by_origin; /* origin -> staged_item_t*, first occurrence */
    char **skip_origins;     /* owned; the hash table below borrows them */
    size_t skip_count;
    size_t skip_cap;
    HYPHashTable *skipped; /* origin -> itself, as a set */
} pull_t;

static void staged_item_release(staged_item_t *item) {
    free(item->author);
    free(item->content);
    free(item->origin);
    free(item->thread);
    free(item->parent_origin);
}

static void pull_release(pull_t *pull) {
    for (size_t i = 0; i < pull->count; i++) {
        staged_item_release(&pull->items[i]);
    }
    free(pull->items);
    for (size_t i = 0; i < pull->skip_count; i++) {
        free(pull->skip_origins[i]);
    }
    free(pull->skip_origins);
    hyp_ht_free(pull->by_origin);
    hyp_ht_free(pull->skipped);
    memset(pull, 0, sizeof(*pull));
}

static bool grow(void **array, size_t *cap, size_t needed, size_t elem_size) {
    if (needed <= *cap) {
        return true;
    }
    size_t cap_next = *cap ? *cap : 16;
    while (cap_next < needed) {
        cap_next *= 2;
    }
    void *grown = realloc(*array, cap_next * elem_size);
    if (!grown) {
        return false;
    }
    *array = grown;
    *cap = cap_next;
    return true;
}

/*
 * Whether a repeated origin is the same row again. The redaction count is
 * deliberately absent from this comparison: it is not data this ingest keeps,
 * so two yields of one origin differing ONLY in what the adapter claims to
 * have scrubbed are the same transcript row, and the scrub run here decides
 * the count either way.
 */
static bool item_matches(const staged_item_t *have, const hyp_feed_item_t *item) {
    return have->timestamp_ms == item->timestamp_ms && str_equal(have->author, item->author) &&
           str_equal(have->content, item->content) && str_equal(have->thread, item->thread) &&
           str_equal(have->parent_origin, item->parent_origin);
}

static hyp_transcript_status_t pull_take_item(pull_t *pull, const hyp_feed_item_t *item) {
    if (!item->origin || item->origin[0] == '\0') {
        return HYP_TRANSCRIPT_ERR_ORIGIN;
    }
    if (item->kind != HYP_RECORD_MESSAGE) {
        /* Refused, never relabelled: the caller wired a non-transcript source
         * to the transcript ingest, and coercion would file the row under a
         * kind nobody claimed for it. */
        return HYP_TRANSCRIPT_ERR_KIND;
    }
    if (item->parent_origin && item->parent_origin[0] == '\0') {
        /* Present-but-empty names nothing; refusing beats guessing. */
        return HYP_TRANSCRIPT_ERR_PRECURSOR;
    }
    pull->yields++;

    staged_item_t *seen = (staged_item_t *)hyp_ht_get(pull->by_origin, item->origin);
    if (seen) {
        /* The same origin again. Identical is the source repeating itself,
         * absorbed exactly as a re-ingest would be. Different is two rows
         * claiming one identity, and there is no rule for picking a winner. */
        return item_matches(seen, item) ? HYP_TRANSCRIPT_OK : HYP_TRANSCRIPT_ERR_DUPLICATE;
    }
    if (hyp_ht_has(pull->skipped, item->origin)) {
        return HYP_TRANSCRIPT_ERR_DUPLICATE; /* declined and delivered: contradiction */
    }

    if (!grow((void **)&pull->items, &pull->cap, pull->count + 1, sizeof(*pull->items))) {
        return HYP_TRANSCRIPT_ERR_ALLOC;
    }
    staged_item_t *slot = &pull->items[pull->count];
    memset(slot, 0, sizeof(*slot));
    slot->timestamp_ms = item->timestamp_ms;
    slot->author = dup_str(item->author);
    slot->content = dup_str(item->content);
    slot->origin = dup_str(item->origin);
    slot->thread = dup_str(item->thread);
    slot->parent_origin = dup_str(item->parent_origin);
    if ((item->author && !slot->author) || (item->content && !slot->content) || !slot->origin ||
        (item->thread && !slot->thread) || (item->parent_origin && !slot->parent_origin)) {
        staged_item_release(slot);
        return HYP_TRANSCRIPT_ERR_ALLOC;
    }
    pull->count++;
    hyp_ht_set(pull->by_origin, slot->origin, slot);
    return HYP_TRANSCRIPT_OK;
}

static hyp_transcript_status_t pull_take_skip(pull_t *pull, const hyp_feed_item_t *item) {
    if (!item->origin || item->origin[0] == '\0') {
        return HYP_TRANSCRIPT_ERR_ORIGIN;
    }
    if (!item->reason || item->reason[0] == '\0') {
        /* A decline with no stated policy is a source defect, not a policy. */
        return HYP_TRANSCRIPT_ERR_SOURCE;
    }
    if (hyp_ht_has(pull->by_origin, item->origin)) {
        return HYP_TRANSCRIPT_ERR_DUPLICATE; /* delivered and declined: contradiction */
    }
    if (hyp_ht_has(pull->skipped, item->origin)) {
        return HYP_TRANSCRIPT_OK; /* the same notice again */
    }
    if (!grow((void **)&pull->skip_origins, &pull->skip_cap, pull->skip_count + 1,
              sizeof(*pull->skip_origins))) {
        return HYP_TRANSCRIPT_ERR_ALLOC;
    }
    char *origin = dup_str(item->origin);
    if (!origin) {
        return HYP_TRANSCRIPT_ERR_ALLOC;
    }
    pull->skip_origins[pull->skip_count++] = origin;
    hyp_ht_set(pull->skipped, origin, origin);
    return HYP_TRANSCRIPT_OK;
}

/* Drain a source into owned memory. On any status but OK the pull holds
 * whatever arrived before the refusal, for the caller's stats, and must still
 * be released. */
static hyp_transcript_status_t pull_drain(hyp_feed_source_t *src, pull_t *pull) {
    memset(pull, 0, sizeof(*pull));
    pull->by_origin = hyp_ht_create(0);
    pull->skipped = hyp_ht_create(0);
    if (!pull->by_origin || !pull->skipped) {
        return HYP_TRANSCRIPT_ERR_ALLOC;
    }
    for (;;) {
        hyp_feed_item_t item;
        memset(&item, 0, sizeof(item));
        hyp_feed_status_t st = src->next(src, &item);
        hyp_transcript_status_t taken;
        if (st == HYP_FEED_END) {
            return HYP_TRANSCRIPT_OK;
        }
        if (st == HYP_FEED_OK) {
            taken = pull_take_item(pull, &item);
        } else if (st == HYP_FEED_SKIP) {
            taken = pull_take_skip(pull, &item);
        } else if (st == HYP_FEED_ERR_ALLOC) {
            taken = HYP_TRANSCRIPT_ERR_ALLOC;
        } else {
            /* The source abandoned the pull. Its own reason is the finding;
             * this layer records that the pull did not complete. */
            taken = HYP_TRANSCRIPT_ERR_SOURCE;
        }
        if (taken != HYP_TRANSCRIPT_OK) {
            return taken;
        }
    }
}

/* ── Ingest ─────────────────────────────────────────────────────────────── */

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
     * for anything. Refusing late would still build no record, but it would
     * have drained a transcript into this process's memory to decide it — and
     * the refusal a caller reads must be about the wiring, not about the row
     * the wiring happened to reach.
     */
    if (!scrub) {
        return HYP_TRANSCRIPT_ERR_NO_SCRUBBER;
    }
    if (!src || !src->next || !store) {
        return HYP_TRANSCRIPT_ERR_NULL;
    }

    pull_t pull;
    hyp_transcript_status_t st = pull_drain(src, &pull);
    stats->items = pull.yields;
    stats->skipped = pull.skip_count;
    if (st != HYP_TRANSCRIPT_OK) {
        pull_release(&pull);
        return st;
    }

    hyp_record_set_t *staging = hyp_record_set_create();
    HYPHashTable *ids_by_origin = hyp_ht_create(0); /* origin -> staged record id */
    if (!staging || !ids_by_origin) {
        st = HYP_TRANSCRIPT_ERR_ALLOC;
        goto done;
    }

    /*
     * Build precursors first, in however many sweeps the pull's parent edges
     * need — the yield order is the adapter's business, not a constraint this
     * ingest exports. A sweep that builds nothing while items remain means some
     * precursor is not in the pull at all, or the edges form a cycle, which is
     * the same defect wearing a loop: refused, whole.
     */
    size_t remaining = pull.count;
    while (remaining > 0) {
        size_t progressed = 0;
        for (size_t i = 0; i < pull.count; i++) {
            staged_item_t *item = &pull.items[i];
            if (item->built) {
                continue;
            }
            const char *parent_id = NULL;
            if (item->parent_origin) {
                if (hyp_ht_has(pull.skipped, item->parent_origin)) {
                    /* The precursor is accounted for and recordless by policy;
                     * deterministic on every machine that runs this pull. */
                    parent_id = NULL;
                } else {
                    parent_id = (const char *)hyp_ht_get(ids_by_origin, item->parent_origin);
                    if (!parent_id) {
                        continue; /* not built yet — a later sweep's problem */
                    }
                }
            }

            /*
             * The seam, and the only record constructor this file can reach.
             * hyp_record_ingest_input_t has no `redactions` member, so the
             * count on the finished record is the scrubber's answer and there
             * is nowhere for anyone else's to travel.
             */
            hyp_record_ingest_input_t in;
            memset(&in, 0, sizeof(in));
            in.kind = HYP_RECORD_MESSAGE;
            in.author = item->author;
            in.timestamp_ms = item->timestamp_ms;
            in.content = item->content;
            in.anchor = NULL; /* attaching transcript text to code is a read-side
                               * analysis, never a guess made during ingest */
            in.origin = item->origin;
            in.thread = item->thread;
            in.parent = parent_id;

            const hyp_record_t *rec = NULL;
            hyp_record_status_t rst = hyp_record_ingest_scrubbed(&in, scrub, &rec);
            if (rst != HYP_RECORD_OK) {
                if (rst == HYP_RECORD_ERR_ALLOC) {
                    /* The seam reports a scrubber that said no the same way it
                     * reports allocation failure. Both abort the whole ingest
                     * and store nothing, so telling them apart would change no
                     * decision; claiming a distinction the seam cannot make
                     * would be the worse error. */
                    st = HYP_TRANSCRIPT_ERR_ALLOC;
                } else {
                    stats->record_status = rst;
                    st = HYP_TRANSCRIPT_ERR_ITEM;
                }
                goto done;
            }
            stats->redactions += rec->redactions;
            if (rec->redactions > 0) {
                stats->items_redacted++;
            }
            rst = hyp_record_set_add(staging, rec, NULL);
            char id[HYP_RECORD_ID_LEN + 1];
            memcpy(id, rec->id, sizeof(id));
            hyp_record_free(rec);
            if (rst != HYP_RECORD_OK) {
                stats->record_status = rst;
                st = rst == HYP_RECORD_ERR_ALLOC ? HYP_TRANSCRIPT_ERR_ALLOC
                                                 : HYP_TRANSCRIPT_ERR_ITEM;
                goto done;
            }
            const hyp_record_t *staged = hyp_record_set_get(staging, id);
            if (!staged) {
                st = HYP_TRANSCRIPT_ERR_ALLOC; /* cannot happen after a successful add */
                goto done;
            }
            /* Key and value both live in the staging set until the merge. */
            hyp_ht_set(ids_by_origin, staged->origin, (void *)(uintptr_t)(const void *)staged->id);
            item->built = true;
            progressed++;
            remaining--;
        }
        if (progressed == 0) {
            st = HYP_TRANSCRIPT_ERR_PRECURSOR;
            goto done;
        }
    }

    {
        size_t added = 0;
        hyp_record_store_status_t sst = hyp_record_store_append_set(store, staging, &added);
        if (sst != HYP_RECORD_STORE_OK) {
            stats->store_status = sst;
            st = sst == HYP_RECORD_STORE_ERR_ALLOC ? HYP_TRANSCRIPT_ERR_ALLOC
                                                   : HYP_TRANSCRIPT_ERR_STORE;
            goto done;
        }
        stats->built = added;
        stats->absorbed = stats->items - added;
    }
    st = HYP_TRANSCRIPT_OK;

done:
    hyp_ht_free(ids_by_origin);
    hyp_record_set_free(staging);
    pull_release(&pull);
    if (st != HYP_TRANSCRIPT_OK) {
        /* Nothing was merged, so nothing about this run is a fact about the
         * store. The redaction counters describe records that will not exist. */
        stats->built = 0;
        stats->absorbed = 0;
        stats->redactions = 0;
        stats->items_redacted = 0;
    }
    return st;
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
