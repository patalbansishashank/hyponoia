/*
 * feed.c — pull-source ingest and the set-of-origins completeness audit. See
 * feed.h for the contract: why pull and never push, why origin is required,
 * why precursor resolution is in-pull only, and why failure is atomic.
 *
 * THREE STRUCTURAL PROPERTIES, all enforced by a contract test rather than by
 * anyone's discipline:
 *
 *   1. NO CLOCK. Item timestamps are the source's event time, supplied by the
 *      adapter. A clock read here would make ids machine-dependent and turn
 *      re-ingest into a duplicate storm.
 *   2. NO PARSING, NO FEED VOCABULARY. origin, thread and parent_origin are
 *      byte strings: copied, compared, never looked inside. The moment this
 *      file interpreted one it would inherit that adapter's schema, and the
 *      second adapter would have to translate into the first one's dialect.
 *   3. NO hyp_record_build(). The only record constructor reachable from here
 *      is hyp_record_ingest_scrubbed(), which cannot run without a scrubber.
 *      A direct call to the builder is a path on which unscrubbed feed text
 *      becomes a permanent, id-bound, syncing record — and an adapter's own
 *      claim about how much it scrubbed cannot reach a record either, because
 *      hyp_feed_item_t has no field to carry one.
 */
#include "feed/feed.h"

#include "foundation/constants.h" /* HYP_ALLOC_ONE */
#include "foundation/hash_table.h"

#include <stdlib.h>
#include <string.h>

const char *hyp_feed_status_reason(hyp_feed_status_t status) {
    switch (status) {
    case HYP_FEED_OK:
        return "ok";
    case HYP_FEED_END:
        return "the pull is exhausted";
    case HYP_FEED_SKIP:
        return "the adapter declined this row by stated policy";
    case HYP_FEED_ERR_NO_SCRUBBER:
        return "no scrubber was wired, so no record may be built from this feed";
    case HYP_FEED_ERR_NULL:
        return "a required argument was NULL";
    case HYP_FEED_ERR_SOURCE:
        return "the source failed or contradicted itself mid-pull";
    case HYP_FEED_ERR_SCHEMA:
        return "the source's shape is not the pinned one";
    case HYP_FEED_ERR_ITEM:
        return "an item was refused by the record contract";
    case HYP_FEED_ERR_ORIGIN:
        return "an item arrived without the source's own name for its row";
    case HYP_FEED_ERR_PRECURSOR:
        return "an item names a precursor this pull never yielded";
    case HYP_FEED_ERR_DUPLICATE:
        return "one origin was yielded twice with different contents";
    case HYP_FEED_ERR_ALLOC:
        return "out of memory";
    }
    return "unknown feed status";
}

void hyp_feed_source_close(hyp_feed_source_t *src) {
    if (src && src->close) {
        src->close(src);
    }
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

typedef struct {
    hyp_record_kind_t kind;
    char *author;
    char *content;
    char *origin;
    char *thread;
    char *parent_origin;
    int64_t timestamp_ms;
    bool built;
} owned_item_t;

typedef struct {
    owned_item_t *items;
    size_t count;
    size_t cap;
    size_t yields;           /* item yields including in-pull repeats of one origin */
    HYPHashTable *by_origin; /* origin -> owned_item_t*, first occurrence */
    char **skip_origins;     /* owned, parallel with skip_reasons */
    char **skip_reasons;
    size_t skip_count;
    size_t skip_cap;
    HYPHashTable *skipped; /* origin -> its owned reason */
} pull_t;

static void owned_item_release(owned_item_t *item) {
    free(item->author);
    free(item->content);
    free(item->origin);
    free(item->thread);
    free(item->parent_origin);
}

static void pull_release(pull_t *pull) {
    for (size_t i = 0; i < pull->count; i++) {
        owned_item_release(&pull->items[i]);
    }
    free(pull->items);
    for (size_t i = 0; i < pull->skip_count; i++) {
        free(pull->skip_origins[i]);
        free(pull->skip_reasons[i]);
    }
    free(pull->skip_origins);
    free(pull->skip_reasons);
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

static bool item_matches(const owned_item_t *have, const hyp_feed_item_t *item) {
    /* The redaction count is deliberately absent from this comparison: it is
     * not something an item can carry, so two yields of one origin are the
     * same row or they are not, and the scrub run here decides the count. */
    return have->kind == item->kind && have->timestamp_ms == item->timestamp_ms &&
           str_equal(have->author, item->author) && str_equal(have->content, item->content) &&
           str_equal(have->thread, item->thread) &&
           str_equal(have->parent_origin, item->parent_origin);
}

static hyp_feed_status_t pull_take_item(pull_t *pull, const hyp_feed_item_t *item) {
    if (!item->origin || item->origin[0] == '\0') {
        return HYP_FEED_ERR_ORIGIN;
    }
    if (item->parent_origin && item->parent_origin[0] == '\0') {
        /* Present-but-empty names nothing; refusing beats guessing. */
        return HYP_FEED_ERR_PRECURSOR;
    }
    pull->yields++;

    owned_item_t *seen = (owned_item_t *)hyp_ht_get(pull->by_origin, item->origin);
    if (seen) {
        /* The same origin again. Identical is the source repeating itself —
         * absorbed, exactly as re-ingest would be. Different is two rows
         * claiming one identity, and there is no rule for picking a winner. */
        return item_matches(seen, item) ? HYP_FEED_OK : HYP_FEED_ERR_DUPLICATE;
    }
    if (hyp_ht_has(pull->skipped, item->origin)) {
        return HYP_FEED_ERR_DUPLICATE; /* declined and delivered: contradiction */
    }

    if (!grow((void **)&pull->items, &pull->cap, pull->count + 1, sizeof(*pull->items))) {
        return HYP_FEED_ERR_ALLOC;
    }
    owned_item_t *slot = &pull->items[pull->count];
    memset(slot, 0, sizeof(*slot));
    slot->kind = item->kind;
    slot->timestamp_ms = item->timestamp_ms;
    slot->author = dup_str(item->author);
    slot->content = dup_str(item->content);
    slot->origin = dup_str(item->origin);
    slot->thread = dup_str(item->thread);
    slot->parent_origin = dup_str(item->parent_origin);
    if ((item->author && !slot->author) || (item->content && !slot->content) || !slot->origin ||
        (item->thread && !slot->thread) || (item->parent_origin && !slot->parent_origin)) {
        owned_item_release(slot);
        return HYP_FEED_ERR_ALLOC;
    }
    pull->count++;
    hyp_ht_set(pull->by_origin, slot->origin, slot);
    return HYP_FEED_OK;
}

static hyp_feed_status_t pull_take_skip(pull_t *pull, const hyp_feed_item_t *item) {
    if (!item->origin || item->origin[0] == '\0') {
        return HYP_FEED_ERR_ORIGIN;
    }
    if (!item->reason || item->reason[0] == '\0') {
        /* A decline with no stated policy is a source defect, not a policy. */
        return HYP_FEED_ERR_SOURCE;
    }
    if (hyp_ht_has(pull->by_origin, item->origin)) {
        return HYP_FEED_ERR_DUPLICATE; /* delivered and declined: contradiction */
    }
    if (hyp_ht_has(pull->skipped, item->origin)) {
        return HYP_FEED_OK; /* the same notice again; first reason stands */
    }
    if (!grow((void **)&pull->skip_origins, &pull->skip_cap, pull->skip_count + 1,
              sizeof(*pull->skip_origins))) {
        return HYP_FEED_ERR_ALLOC;
    }
    /* Both arrays share skip_cap; grow the second to match. */
    char **reasons = (char **)realloc(pull->skip_reasons, pull->skip_cap * sizeof(*reasons));
    if (!reasons) {
        return HYP_FEED_ERR_ALLOC;
    }
    pull->skip_reasons = reasons;
    char *origin = dup_str(item->origin);
    char *reason = dup_str(item->reason);
    if (!origin || !reason) {
        free(origin);
        free(reason);
        return HYP_FEED_ERR_ALLOC;
    }
    pull->skip_origins[pull->skip_count] = origin;
    pull->skip_reasons[pull->skip_count] = reason;
    pull->skip_count++;
    hyp_ht_set(pull->skipped, origin, reason);
    return HYP_FEED_OK;
}

/* Drain a source into owned memory. On any status but HYP_FEED_OK the pull
 * holds whatever arrived before the refusal, for the caller's stats, and must
 * still be released. */
static hyp_feed_status_t pull_drain(hyp_feed_source_t *src, pull_t *pull) {
    memset(pull, 0, sizeof(*pull));
    pull->by_origin = hyp_ht_create(0);
    pull->skipped = hyp_ht_create(0);
    if (!pull->by_origin || !pull->skipped) {
        return HYP_FEED_ERR_ALLOC;
    }
    for (;;) {
        hyp_feed_item_t item;
        memset(&item, 0, sizeof(item));
        hyp_feed_status_t st = src->next(src, &item);
        if (st == HYP_FEED_END) {
            return HYP_FEED_OK;
        }
        if (st == HYP_FEED_OK) {
            st = pull_take_item(pull, &item);
        } else if (st == HYP_FEED_SKIP) {
            st = pull_take_skip(pull, &item);
        }
        if (st != HYP_FEED_OK) {
            return st;
        }
    }
}

/* ── Ingest ─────────────────────────────────────────────────────────────── */

static hyp_feed_status_t map_record_status(hyp_record_status_t st, hyp_feed_ingest_stats_t *stats) {
    if (st == HYP_RECORD_ERR_ALLOC) {
        return HYP_FEED_ERR_ALLOC;
    }
    if (stats) {
        stats->record_status = st;
    }
    return HYP_FEED_ERR_ITEM;
}

hyp_feed_status_t hyp_feed_ingest(hyp_feed_source_t *src, hyp_scrub_fn scrub,
                                  hyp_record_set_t *store, hyp_feed_ingest_stats_t *stats) {
    hyp_feed_ingest_stats_t local;
    if (!stats) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));
    /*
     * The scrubber is checked FIRST, separately, and before the source is
     * asked for anything. Refusing later would still build no record, but it
     * would first pull a transcript into this process's memory to decide
     * something the wiring already decided — and the refusal a caller reads
     * must name the missing wire, not the row the wire happened to reach.
     */
    if (!scrub) {
        return HYP_FEED_ERR_NO_SCRUBBER;
    }
    if (!src || !src->next || !store) {
        return HYP_FEED_ERR_NULL;
    }

    pull_t pull;
    hyp_feed_status_t st = pull_drain(src, &pull);
    stats->items = pull.yields;
    stats->skipped = pull.skip_count;
    if (st != HYP_FEED_OK) {
        pull_release(&pull);
        return st;
    }

    hyp_record_set_t *staging = hyp_record_set_create();
    HYPHashTable *ids_by_origin = hyp_ht_create(0); /* origin -> staged record id */
    if (!staging || !ids_by_origin) {
        st = HYP_FEED_ERR_ALLOC;
        goto done;
    }

    /*
     * Build precursors first, in however many sweeps the pull's parent edges
     * need — the yield order is the adapter's business, not a constraint this
     * boundary exports. A sweep that builds nothing while items remain means
     * some precursor is not in the pull at all (or the edges form a cycle,
     * which is the same defect wearing a loop): refused, whole.
     */
    size_t remaining = pull.count;
    while (remaining > 0) {
        size_t progressed = 0;
        for (size_t i = 0; i < pull.count; i++) {
            owned_item_t *item = &pull.items[i];
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
             * The scrub seam, and the only record constructor this file can
             * reach. hyp_record_ingest_input_t has no `redactions` member and
             * neither does hyp_feed_item_t, so the count on the finished
             * record is what the scrubber just produced from this item's raw
             * text — there is nowhere for anyone else's number to travel.
             */
            hyp_record_ingest_input_t in;
            memset(&in, 0, sizeof(in));
            in.kind = item->kind;
            in.author = item->author;
            in.timestamp_ms = item->timestamp_ms;
            in.content = item->content; /* RAW; the seam scrubs it */
            in.anchor = NULL;
            in.origin = item->origin;
            in.thread = item->thread;
            in.parent = parent_id;

            const hyp_record_t *rec = NULL;
            hyp_record_status_t rst = hyp_record_ingest_scrubbed(&in, scrub, &rec);
            if (rst != HYP_RECORD_OK) {
                st = map_record_status(rst, stats);
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
                st = map_record_status(rst, stats);
                goto done;
            }
            const hyp_record_t *stored = hyp_record_set_get(staging, id);
            if (!stored) {
                st = HYP_FEED_ERR_ALLOC; /* cannot happen after a successful add */
                goto done;
            }
            /* Key and value both live in the staging set until the merge. */
            hyp_ht_set(ids_by_origin, stored->origin, (void *)(uintptr_t)(const void *)stored->id);
            item->built = true;
            progressed++;
            remaining--;
        }
        if (progressed == 0) {
            st = HYP_FEED_ERR_PRECURSOR;
            goto done;
        }
    }

    {
        size_t before = hyp_record_set_count(store);
        hyp_record_status_t rst = hyp_record_set_merge(store, staging);
        if (rst != HYP_RECORD_OK) {
            st = map_record_status(rst, stats);
            goto done;
        }
        stats->built = hyp_record_set_count(store) - before;
        stats->absorbed = stats->items - stats->built;
    }
    st = HYP_FEED_OK;

done:
    hyp_ht_free(ids_by_origin);
    hyp_record_set_free(staging);
    pull_release(&pull);
    if (st != HYP_FEED_OK) {
        /* Nothing merged, so nothing here is a fact about the store. The
         * redaction counters would otherwise describe records that do not
         * exist, which reads as evidence that a scrub protected something. */
        stats->built = 0;
        stats->absorbed = 0;
        stats->redactions = 0;
        stats->items_redacted = 0;
    }
    return st;
}

/* ── The audit ──────────────────────────────────────────────────────────── */

bool hyp_feed_audit_complete(const hyp_feed_audit_t *report) {
    return report != NULL && report->missing_count == 0;
}

void hyp_feed_audit_dispose(hyp_feed_audit_t *report) {
    if (!report) {
        return;
    }
    for (size_t i = 0; i < report->missing_count; i++) {
        free((void *)(uintptr_t)(const void *)report->missing[i]);
    }
    free((void *)(uintptr_t)(const void *)report->missing);
    for (size_t i = 0; i < report->skipped_count; i++) {
        free((void *)(uintptr_t)(const void *)report->skipped[i]);
        free((void *)(uintptr_t)(const void *)report->skip_reasons[i]);
    }
    free((void *)(uintptr_t)(const void *)report->skipped);
    free((void *)(uintptr_t)(const void *)report->skip_reasons);
    memset(report, 0, sizeof(*report));
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

typedef struct {
    char *origin;
    char *reason;
} skip_pair_t;

static int cmp_skip_pair(const void *a, const void *b) {
    return strcmp(((const skip_pair_t *)a)->origin, ((const skip_pair_t *)b)->origin);
}

hyp_feed_status_t hyp_feed_audit_run(hyp_feed_source_t *src, const hyp_record_set_t *store,
                                     hyp_feed_audit_t *out) {
    if (!out) {
        return HYP_FEED_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));
    if (!src || !src->next || !store) {
        return HYP_FEED_ERR_NULL;
    }

    hyp_feed_status_t st;
    char **missing = NULL;
    size_t missing_count = 0;
    size_t missing_cap = 0;
    skip_pair_t *skips = NULL;
    size_t distinct = 0;

    /* Everything the store attributes to SOME feed row, keyed by the bytes the
     * adapter wrote — never interpreted, so the comparison works identically
     * for every adapter. Keys are borrowed from the store, which outlives this
     * call. */
    HYPHashTable *held = hyp_ht_create(0);
    if (!held) {
        return HYP_FEED_ERR_ALLOC;
    }
    for (size_t i = 0;; i++) {
        const hyp_record_t *rec = hyp_record_set_at(store, i);
        if (!rec) {
            break;
        }
        if (rec->origin) {
            hyp_ht_set(held, rec->origin, (void *)held);
        }
    }

    pull_t pull;
    st = pull_drain(src, &pull);
    if (st != HYP_FEED_OK) {
        goto fail;
    }

    distinct = pull.count;
    for (size_t i = 0; i < pull.count; i++) {
        const char *origin = pull.items[i].origin;
        if (hyp_ht_has(held, origin)) {
            continue;
        }
        if (!grow((void **)&missing, &missing_cap, missing_count + 1, sizeof(*missing))) {
            st = HYP_FEED_ERR_ALLOC;
            goto fail;
        }
        missing[missing_count] = dup_str(origin);
        if (!missing[missing_count]) {
            st = HYP_FEED_ERR_ALLOC;
            goto fail;
        }
        missing_count++;
    }
    if (missing_count > 1) {
        qsort(missing, missing_count, sizeof(*missing), cmp_str);
    }

    if (pull.skip_count > 0) {
        skips = (skip_pair_t *)calloc(pull.skip_count, sizeof(*skips));
        if (!skips) {
            st = HYP_FEED_ERR_ALLOC;
            goto fail;
        }
        for (size_t i = 0; i < pull.skip_count; i++) {
            skips[i].origin = dup_str(pull.skip_origins[i]);
            skips[i].reason = dup_str(pull.skip_reasons[i]);
            if (!skips[i].origin || !skips[i].reason) {
                st = HYP_FEED_ERR_ALLOC;
                goto fail;
            }
        }
        qsort(skips, pull.skip_count, sizeof(*skips), cmp_skip_pair);
    }

    /* Assemble the report only once nothing can fail any more. */
    out->feed = src->name;
    out->expected = distinct;
    out->missing = (const char *const *)missing;
    out->missing_count = missing_count;
    out->present = distinct - missing_count;
    if (pull.skip_count > 0) {
        char **skip_origins = (char **)calloc(pull.skip_count, sizeof(*skip_origins));
        char **skip_reasons = (char **)calloc(pull.skip_count, sizeof(*skip_reasons));
        if (!skip_origins || !skip_reasons) {
            free(skip_origins);
            free(skip_reasons);
            memset(out, 0, sizeof(*out));
            st = HYP_FEED_ERR_ALLOC;
            goto fail;
        }
        for (size_t i = 0; i < pull.skip_count; i++) {
            skip_origins[i] = skips[i].origin;
            skip_reasons[i] = skips[i].reason;
        }
        out->skipped = (const char *const *)skip_origins;
        out->skip_reasons = (const char *const *)skip_reasons;
        out->skipped_count = pull.skip_count;
        free(skips); /* the strings now belong to the report */
        skips = NULL;
    }

    hyp_ht_free(held);
    pull_release(&pull);
    return HYP_FEED_OK;

fail:
    for (size_t i = 0; i < missing_count; i++) {
        free(missing[i]);
    }
    free(missing);
    if (skips) {
        for (size_t i = 0; i < pull.skip_count; i++) {
            free(skips[i].origin);
            free(skips[i].reason);
        }
        free(skips);
    }
    hyp_ht_free(held);
    pull_release(&pull);
    return st;
}
