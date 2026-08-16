#ifndef HYP_INGEST_TRANSCRIPT_INGEST_H
#define HYP_INGEST_TRANSCRIPT_INGEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "feed/feed.h"
#include "foundation/record.h"
#include "foundation/scrub.h"
#include "store/record_store.h"

/*
 * transcript_ingest.h — transcripts, from a pull source into the durable
 * record store, through the scrub and by no other route.
 *
 * A transcript is the one record kind nobody sits down to write. It is
 * whatever was said, verbatim, and what gets said includes pasted keys. The
 * record contract makes that permanent rather than embarrassing: the id
 * commits to `content`, records are never rewritten, and merging two stores is
 * a union — so an unscrubbed transcript record cannot be repaired afterwards.
 * A scrub after the fact changes the content, which changes the id, which is a
 * DIFFERENT record standing beside the original while the original keeps
 * syncing to every machine that asks.
 *
 * This file therefore has one job beyond moving rows: make the unscrubbed path
 * something a caller cannot express.
 *
 * ── Four mechanisms, because a runtime refusal alone is only a runtime check ─
 *
 *   1. THE SIGNATURE. hyp_transcript_ingest() takes an hyp_scrub_fn as a
 *      required argument. There is no variant without one, no default, and no
 *      flag to discover. NULL is refused with HYP_TRANSCRIPT_ERR_NO_SCRUBBER
 *      before the source is drained: nothing is built, nothing is stored, and
 *      the source is not even asked for its first item.
 *   2. THE TYPE. Content reaches record construction inside
 *      hyp_record_ingest_input_t, which has no `redactions` member. A feed item
 *      does carry a redaction count, and this module DROPS IT: an adapter's
 *      claim about how much it already scrubbed has nowhere to go. The count on
 *      a stored transcript record is what this ingest's own scrubber produced,
 *      or the record does not exist.
 *   3. THE ABSENT SYMBOL. This translation unit never calls hyp_record_build().
 *      Its only record constructor is hyp_record_ingest_scrubbed(), which
 *      cannot run without a scrubber. tests/test_transcript_contract.sh holds
 *      that as a source-level property with a positive control, so an edit that
 *      reaches for the builder directly fails a gate rather than a review.
 *   4. NO SECOND DOOR. The same gate DERIVES its check from this header rather
 *      than from a list: every declaration here that takes a hyp_feed_source_t
 *      must also take an hyp_scrub_fn. A convenience entry point added later
 *      without one is a failing test, not a discovery.
 *
 * The general feed ingest (feed.h) remains the surface for feeds that are not
 * transcripts. It builds from the item's own redaction count and so trusts its
 * adapter to have scrubbed. Transcript text is given no such trust here.
 *
 * ── Transcripts are messages ─────────────────────────────────────────────────
 *
 * Every item ingested through this file becomes kind = message. An item of any
 * other kind is REFUSED rather than coerced: a source yielding decisions into a
 * transcript ingest has been wired to the wrong function, and relabelling its
 * rows would file text in the store under a kind its author never claimed.
 *
 * ── Idempotence, and the three things carrying it ────────────────────────────
 *
 * The same source rows re-ingested produce the same ids, so the store absorbs
 * them: no dedup table, no "already seen" set, no high-water mark that could
 * disagree with the store. That rests on three properties holding at once — the
 * timestamp is the source's event time and never a clock read here; `origin` is
 * the adapter's own name for the row, so two rows carrying identical text stay
 * two records; and the scrub is deterministic, so the bytes the id binds are
 * the same on every machine. A re-ingest reports items == absorbed and
 * built == 0, which is that property stated as numbers a caller can read.
 *
 * ── Atomic, and opaque ───────────────────────────────────────────────────────
 *
 * The pull is drained and every record built before the store is touched, then
 * merged in one transaction. Any refusal leaves the store byte-identical, by
 * its own digest: a half-ingested transcript would make "what do I hold" depend
 * on where an error happened, and a completeness audit would then report a
 * shortfall that no re-pull could explain.
 *
 * `origin` and `thread` are copied and compared, never parsed. This file knows
 * no feed's vocabulary and holds no identifier format of its own; only the
 * adapter that wrote an identifier may read it back.
 */

/*
 * Every answer this surface can give. One reason per status, distinct, so two
 * different refusals can never collapse into one indistinguishable log line.
 */
typedef enum {
    HYP_TRANSCRIPT_OK = 0,
    HYP_TRANSCRIPT_ERR_NULL,        /* a required argument was NULL */
    HYP_TRANSCRIPT_ERR_NO_SCRUBBER, /* no scrubber wired: nothing may be built */
    HYP_TRANSCRIPT_ERR_KIND,        /* an item that is not a transcript message */
    HYP_TRANSCRIPT_ERR_SOURCE,      /* the source failed or contradicted itself */
    HYP_TRANSCRIPT_ERR_ORIGIN,      /* an item or notice arrived without an origin */
    HYP_TRANSCRIPT_ERR_ITEM,        /* the record contract refused the row */
    HYP_TRANSCRIPT_ERR_PRECURSOR,   /* a precursor this pull never yielded */
    HYP_TRANSCRIPT_ERR_DUPLICATE,   /* one origin, two different rows, one pull */
    HYP_TRANSCRIPT_ERR_STORE,       /* the durable store refused the merge */
    HYP_TRANSCRIPT_ERR_ALLOC        /* out of memory, or the scrubber said no */
} hyp_transcript_status_t;

/* Stable, human-facing reason for a status. Never NULL. */
const char *hyp_transcript_status_reason(hyp_transcript_status_t status);

/*
 * What an ingest did.
 *
 * `redactions` and `items_redacted` are reported because a caller wants to know
 * that the scrub fired without ever seeing what it removed. They are counts of
 * this run, not of the store: the per-record count rides on the record itself,
 * where a reader can tell "verbatim" from "cleaned" without a source to diff
 * against.
 */
typedef struct {
    size_t items;          /* item yields, in-pull repeats included; notices are not */
    size_t skipped;        /* skip notices yielded */
    size_t built;          /* records the store gained */
    size_t absorbed;       /* items - built: already stored, or repeated in-pull */
    uint64_t redactions;   /* spans the scrubber replaced across this ingest */
    size_t items_redacted; /* items in which it replaced at least one */
    /* On HYP_TRANSCRIPT_ERR_ITEM: which field the record contract refused. */
    hyp_record_status_t record_status;
    /* On HYP_TRANSCRIPT_ERR_STORE: what the store said. */
    hyp_record_store_status_t store_status;
} hyp_transcript_ingest_stats_t;

/*
 * Drain a transcript source into a durable store, scrubbing every item before
 * the record that will carry it exists.
 *
 * `scrub` is REQUIRED — see mechanism 1 above. hyp_scrub_text is the production
 * scrubber; a test may pass an instrumented one with the same contract.
 *
 * Atomic: on any status but HYP_TRANSCRIPT_OK the store is untouched and its
 * digest is unchanged. `stats` is optional and is written on success and on
 * failure alike — on failure it describes how far the pull got.
 */
hyp_transcript_status_t hyp_transcript_ingest(hyp_feed_source_t *src, hyp_scrub_fn scrub,
                                              hyp_record_store_t *store,
                                              hyp_transcript_ingest_stats_t *stats);

/*
 * ── Reading a conversation back ──────────────────────────────────────────────
 *
 * The record store enumerates by id, canonically, because a union has no other
 * order that is the same on two machines. A conversation, though, is read in
 * the order it was said, and that ordering is a QUERY rather than a fact the
 * store keeps: timestamp_ms ascending, ties broken by id so the result is a
 * TOTAL order and therefore identical everywhere. Ties are not hypothetical — a
 * harness that writes a turn and the tool call it made in the same instant
 * produces two messages at one millisecond — and leaving that tie to iteration
 * order would let one transcript read two ways on two machines holding exactly
 * the same records.
 *
 * The thread value is matched as BYTES. It is the adapter's name for a
 * conversation and is never parsed here.
 */
typedef struct {
    /* Owns the records; released by hyp_transcript_thread_dispose(). */
    hyp_record_set_t *records;
    /* `count` borrowed pointers into `records`, in the order above. */
    const hyp_record_t *const *messages;
    size_t count;
} hyp_transcript_thread_t;

/* Load one conversation. A thread with no messages is OK and an EMPTY result —
 * empty means "there is nothing", and absent would mean "look elsewhere". */
hyp_transcript_status_t hyp_transcript_thread_load(hyp_record_store_t *store, const char *thread,
                                                   hyp_transcript_thread_t *out);

/* Release a loaded conversation. Safe on a zeroed struct; never frees it. */
void hyp_transcript_thread_dispose(hyp_transcript_thread_t *thread);

#endif /* HYP_INGEST_TRANSCRIPT_INGEST_H */
