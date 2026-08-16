#ifndef HYP_FEED_FEED_H
#define HYP_FEED_FEED_H

#include "foundation/record.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * feed.h — the feed interface. A PULL source of items, and the audit that a
 * pull makes possible.
 *
 * A feed is something outside this system that holds attributable text we want
 * as records: an orchestrator's transcript store, a directory another harness
 * writes into, anything. This header is the whole of what the core knows about
 * one. Everything specific to a particular feed — its schema, its identifier
 * formats, its column names — lives in that feed's ADAPTER, behind this
 * boundary, and never reaches the core. The test for the boundary is not a
 * review opinion: a second adapter must be writable against this header without
 * touching it, and tests/test_feed.c contains exactly that — a toy in-memory
 * adapter written only against this interface. tests/test_feed_contract.sh
 * holds the file-level end: the core feed files grep clean of any feed's
 * vocabulary, the same discipline record.h already enforces for itself.
 *
 * ── Pull, never push ─────────────────────────────────────────────────────────
 *
 * The interface is a pull because a pull is the only feed that can prove it has
 * everything. A pusher that silently drops a session is invisible — every count
 * it produces is a floor. A pull enumerates the source's own rows, so the
 * question "do we hold everything the source holds?" has a computable answer,
 * and hyp_feed_audit_run() below computes it. There is deliberately no
 * push-shaped entry point here.
 *
 * ── What an item is ──────────────────────────────────────────────────────────
 *
 * One item becomes one record through the C2 build path. The adapter supplies:
 *
 *   origin   REQUIRED. The source's OWN name for the row, adapter-prefixed.
 *            Identity is content-derived, so two source rows carrying identical
 *            text by the same author at the same millisecond would collapse
 *            into one record — and the audit would then report a permanent
 *            shortfall indistinguishable from real loss. origin makes distinct
 *            rows distinct records, re-ingest exactly idempotent, and the audit
 *            a straight set comparison. The core never parses it.
 *   thread   The source's name for the conversation the item belongs to, or
 *            NULL. Opaque, same discipline as origin.
 *   parent_origin
 *            The ORIGIN of this item's precursor, or NULL. Not a record id:
 *            an adapter knows its source's own identifiers, not our digests.
 *            Ingest resolves it to the precursor's record id — see below.
 *   author / timestamp_ms / content / kind / redactions
 *            Exactly what hyp_record_build() takes. The timestamp is the
 *            SOURCE's event time, never a clock read here — re-ingesting the
 *            same row must produce the same id on every machine.
 *
 * Feed items carry no anchor: attaching transcript text to code is a read-side
 * analysis, not something an ingest should guess at.
 *
 * ── Skip notices ─────────────────────────────────────────────────────────────
 *
 * An adapter may meet a source row that its stated policy declines to turn
 * into a record (a row with nothing in it, a row missing its event time). If
 * such rows simply vanished, the audit could not tell "declined by policy"
 * from "lost" — the exact confusion this interface exists to remove. So the
 * source yields a SKIP notice instead: origin plus a reason, no record. The
 * audit reports skipped origins by name, separately from missing ones, and
 * completeness is judged over what remains.
 *
 * ── Resolution and determinism ───────────────────────────────────────────────
 *
 * parent_origin resolves WITHIN THE PULL, in any yield order — ingest buffers
 * the pull and builds precursors first, so an adapter is free to yield in
 * whatever order its source provides. Resolution never consults the store:
 * what a pull produces must be a pure function of the pull, or two machines
 * ingesting the same feed would mint different ids and the union would double
 * instead of deduplicate. A parent_origin the pull never yielded is refused —
 * a confident error, not a dangling guess. A parent_origin naming a SKIPPED
 * row resolves to "no parent": the precursor is accounted for, recordless by
 * policy, deterministically on every machine.
 *
 * ── Failure is atomic ────────────────────────────────────────────────────────
 *
 * Ingest stages everything and merges once. Any refusal — a malformed item, a
 * source error mid-pull, an unresolvable precursor — leaves the store exactly
 * as it was. A half-ingested feed would make "what is in my store" depend on
 * where an error happened, and the audit would then report a shortfall that no
 * re-pull could explain.
 */

/* Every answer this interface can give. One reason per status, distinct, so
 * two different refusals can never become one indistinguishable log line. */
typedef enum {
    HYP_FEED_OK = 0,
    HYP_FEED_END,           /* the pull is exhausted — not an error */
    HYP_FEED_SKIP,          /* a skip notice: origin + reason, no record */
    HYP_FEED_ERR_NULL,      /* a required pointer was NULL */
    HYP_FEED_ERR_SOURCE,    /* the source failed or contradicted itself mid-pull */
    HYP_FEED_ERR_SCHEMA,    /* the source's shape is not the pinned one */
    HYP_FEED_ERR_ITEM,      /* an item the record contract refused */
    HYP_FEED_ERR_ORIGIN,    /* an item or notice arrived without an origin */
    HYP_FEED_ERR_PRECURSOR, /* a precursor this pull never yielded */
    HYP_FEED_ERR_DUPLICATE, /* one origin, two different items, one pull */
    HYP_FEED_ERR_ALLOC      /* out of memory */
} hyp_feed_status_t;

/* Stable, human-facing reason for a status. Never NULL. */
const char *hyp_feed_status_reason(hyp_feed_status_t status);

/*
 * One yield from a source. All strings are borrowed: valid until the next call
 * on the same source, owned by the adapter. On HYP_FEED_OK every field is
 * meaningful and `reason` is NULL; on HYP_FEED_SKIP only `origin` and `reason`
 * are read.
 */
typedef struct {
    hyp_record_kind_t kind;
    const char *author;
    int64_t timestamp_ms;
    const char *content;
    const char *origin;        /* required on every yield, notices included */
    const char *thread;        /* or NULL */
    const char *parent_origin; /* origin of the precursor, or NULL */
    uint32_t redactions;       /* spans a scrub replaced BEFORE this yield */
    const char *reason;        /* HYP_FEED_SKIP only: why there is no record */
} hyp_feed_item_t;

/*
 * A source: the one seam an adapter implements. Caller-owned struct, adapter-
 * filled. A source is one pull — open, drain, close — never rewound; the audit
 * takes its own freshly opened source for the same feed.
 *
 * `name` identifies the FEED INSTANCE for reporting (an audit finding must say
 * which feed it is about). Chosen by the adapter, opaque here.
 */
typedef struct hyp_feed_source hyp_feed_source_t;
struct hyp_feed_source {
    const char *name;
    void *ctx;
    /* Yield the next item or notice. HYP_FEED_OK / HYP_FEED_SKIP: *out is
     * valid until the next call. HYP_FEED_END: drained. Anything else: the
     * pull is abandoned, whole. */
    hyp_feed_status_t (*next)(hyp_feed_source_t *src, hyp_feed_item_t *out);
    /* Release everything the source holds. May be NULL. */
    void (*close)(hyp_feed_source_t *src);
};

/* Close a source. NULL, and a NULL close hook, are no-ops. */
void hyp_feed_source_close(hyp_feed_source_t *src);

/*
 * What an ingest did. `absorbed` counts items whose record was already present
 * — a re-ingested feed reports items == absorbed and built == 0, which is the
 * idempotence property stated as numbers a caller can read.
 */
typedef struct {
    size_t items;    /* items yielded (skip notices not included) */
    size_t skipped;  /* skip notices yielded */
    size_t built;    /* records the store gained */
    size_t absorbed; /* items - built: already present, or repeated in-pull */
    /* On HYP_FEED_ERR_ITEM: which field the record contract refused. */
    hyp_record_status_t record_status;
} hyp_feed_ingest_stats_t;

/*
 * Drain a source into a store, atomically. On any status but HYP_FEED_OK the
 * store is untouched. `stats` (optional) is written on success and on failure —
 * on failure it describes how far the pull got before the refusal.
 */
hyp_feed_status_t hyp_feed_ingest(hyp_feed_source_t *src, hyp_record_set_t *store,
                                  hyp_feed_ingest_stats_t *stats);

/*
 * ── The completeness audit ───────────────────────────────────────────────────
 *
 * Compares the SET of origins a feed yields against the origins present in the
 * store — never the counts. A count comparison has two ways to lie: dedup and
 * loss both shrink it, and equal counts can hide one row missing and one row
 * doubled. The set comparison costs the same and NAMES the missing rows, so a
 * shortfall is actionable instead of a number nobody can act on.
 *
 * Per-feed, never global: the query set is what THIS source yields, so records
 * from other feeds — or authored locally with no origin at all — are invisible
 * to it, in both directions.
 *
 * The audit answers "does the store hold every ROW the feed holds", keyed on
 * the source's own identity. A source row whose content was rewritten upstream
 * keeps its origin, so revision drift is a re-ingest concern, not a shortfall.
 */
typedef struct {
    const char *feed;    /* the audited source's name, borrowed from it */
    size_t expected;     /* distinct item origins the feed yielded */
    size_t present;      /* of those, origins some record in the store carries */
    size_t missing_count;
    /* The missing origins BY NAME, ascending, owned by the report. */
    const char *const *missing;
    size_t skipped_count;
    /* Origins the feed declined by policy, ascending, with the adapter's
     * reasons, parallel arrays, owned by the report. Not shortfall. */
    const char *const *skipped;
    const char *const *skip_reasons;
} hyp_feed_audit_t;

/* True when the store holds a record for every origin the feed yielded. */
bool hyp_feed_audit_complete(const hyp_feed_audit_t *report);

/*
 * Run the audit: drain `src`, compare against `store`, fill *out. The store is
 * only read. On success the caller owns the report's arrays and releases them
 * with hyp_feed_audit_dispose(); on failure *out is zeroed.
 */
hyp_feed_status_t hyp_feed_audit_run(hyp_feed_source_t *src, const hyp_record_set_t *store,
                                     hyp_feed_audit_t *out);

/* Release a report's arrays. Safe on a zeroed report; never frees the struct. */
void hyp_feed_audit_dispose(hyp_feed_audit_t *report);

#endif /* HYP_FEED_FEED_H */
