#ifndef HYP_MEMORY_ORPHAN_H
#define HYP_MEMORY_ORPHAN_H

#include "foundation/record.h"
#include "memory/anchor.h"
#include "store/record_store.h"
#include "store/store.h"

#include <stdint.h>

/*
 * orphan.h — the read path over anchored memory, and the one place the
 * re-attach decision is made. The decision is always the same: no.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * ORPHAN-NESS CANNOT LIVE ON THE RECORD
 * ═════════════════════════════════════════════════════════════════════════
 *
 * The record id is a digest of every field, the anchor among them, so marking
 * a record orphaned would change its id — it would not be that record any
 * more. There is no mutation to reach for and no flag to set, and that is a
 * property of the contract rather than a rule anyone has to remember.
 *
 * So orphan-ness is DERIVED, here, at read time, from the pair
 * (record, index state). Both halves matter. The same record is resolved on
 * one machine and orphaned on another whose tree is at a different commit, and
 * NEITHER MACHINE IS WRONG — two machines may legitimately disagree, and a
 * stored flag would have made one of them silently authoritative.
 *
 * "Visibly orphaned" therefore means visible in the read path: it is what this
 * module renders, not a bit anybody stores. The one thing that may be written
 * is a NEW record — a `signal` whose `parent` is the record it observed
 * (hyp_orphan_signal_build). That is an append, it leaves the observed record
 * untouched because it cannot do otherwise, and building the view never
 * performs it: this module has no store-writing function at all, so a sweep
 * that wrote something would not compile.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHAT THE READ PATH MAY NOT DO
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Never silently dropped, never silently re-attached. Both directions are
 * failures and they are not symmetric:
 *
 *   A FALSE NEGATIVE makes a decision unfindable. Bad, and recoverable —
 *   the record is still in the store and a later read finds it.
 *   A FALSE POSITIVE attaches last month's reasoning to code that never had
 *   it. Worse, and invisible: the reader has an answer, it is confident, and
 *   nothing in it says where it came from.
 *
 * Which is why a content-hash match at a different address is EVIDENCE and
 * never a conclusion (I5). Cross-directory move, file rename and copy-paste
 * all produce exactly one observation — the same content at an address the
 * anchor does not name — and no amount of care here can tell them apart,
 * because the difference is not in the data. A copy-paste yields several
 * candidates and the honest answer for every one of them is the same: here is
 * what was found, nothing was attached, you decide.
 *
 * ── The candidate list is a floor, and says so ──────────────────────────
 *
 * A scan that could not read a span publishes scan_skipped. A caller that
 * absorbed it would be turning "at least these" into "these", which is the
 * one transformation a floor does not survive. The rendering discloses it.
 *
 * ── Absent and empty are different answers ──────────────────────────────
 *
 * An anchor with no recorded hash gives the scan nothing to look for, so no
 * scan runs and the candidate list is ABSENT: look elsewhere. An anchor with a
 * hash whose scan found nothing has an EMPTY list: there is nothing. Only one
 * of those is ever true and the rendering spells them differently.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE RENDERED VIEW — the contract a reader is held to
 * ═════════════════════════════════════════════════════════════════════════
 *
 * One line per fact, `key: value`, stable keys. This is the surface a client
 * sees, so it is the surface tests assert; checking the struct alone would
 * check the shape of the apparatus rather than what a consumer receives.
 *
 *   record: <id>
 *   anchor: <the anchor string, verbatim>
 *   anchor_status: resolved | resolved-edited | orphaned | ambiguous |
 *                  unknown-workspace | error
 *   attached: <project>/<file>:<start>-<end>   only when a status attaches
 *   reattached: no                             on every non-attaching status
 *   candidates: not-scanned | none | <n>
 *   candidate: <address> <project>/<file>:<start>-<end> evidence=<relation>
 *   candidates_are_a_floor: <n> span(s) unread
 *   reason: <text>                             error / unknown-workspace /
 *                                              ambiguous
 *
 * `reattached: no` is not decoration. It is the assertion surface for the one
 * failure that looks exactly like success from the outside, and a reader who
 * sees candidates without it has been handed a decision by something.
 */

/* ── Statuses ───────────────────────────────────────────────────────────── */

typedef enum {
    HYP_ORPHAN_OK = 0,
    HYP_ORPHAN_ERR_NULL,          /* a required argument was NULL */
    HYP_ORPHAN_ERR_QUERY,         /* a query that contradicts what a view is */
    HYP_ORPHAN_ERR_STORE,         /* the record store refused */
    HYP_ORPHAN_ERR_NOT_ORPHANED,  /* asked to signal an entry that attaches */
    HYP_ORPHAN_ERR_RECORD,        /* the record contract refused the signal */
    HYP_ORPHAN_ERR_ALLOC          /* out of memory */
} hyp_orphan_status_t;

/* Stable, human-facing reason for a status. Never NULL. */
const char *hyp_orphan_status_reason(hyp_orphan_status_t status);

/* ── The view ───────────────────────────────────────────────────────────── */

/* One record and what its anchor resolves to right now. The record is borrowed
 * from the view and dies with it; the resolution's candidates are owned by the
 * view too. */
typedef struct {
    const hyp_record_t *record;
    hyp_anchor_res_t res;
} hyp_orphan_entry_t;

typedef struct hyp_orphan_view hyp_orphan_view_t;

/* Classify every ANCHORED record matching `query` against the live index.
 * `query` may be NULL for "every anchored record"; `workspace` is the
 * workspace this index serves, NULL for a workspace of one (anchor.h).
 *
 * READ ONLY. The record store is queried and nothing else — no append, no
 * update, and no path through which either could happen.
 *
 * anchored_only is forced on: a record with no anchor has no anchor status,
 * and giving it one would be inventing a claim about it. Asking for
 * unanchored_only is therefore a contradiction and is refused rather than
 * answered with an empty view. */
hyp_orphan_status_t hyp_orphan_view_build(hyp_record_store_t *records, hyp_store_t *index,
                                          const char *workspace,
                                          const hyp_record_store_query_t *query,
                                          hyp_orphan_view_t **out);

/* Release the view, its records and every candidate array in it. NULL is a
 * no-op. */
void hyp_orphan_view_free(hyp_orphan_view_t *view);

/* How many entries the view holds — the number of anchored records the query
 * matched, never a filtered subset of them. */
int hyp_orphan_view_count(const hyp_orphan_view_t *view);

/* Entry `index`, or NULL when out of range. Order is the record store's
 * canonical order, so two machines holding the same records enumerate the
 * same way. */
const hyp_orphan_entry_t *hyp_orphan_view_at(const hyp_orphan_view_t *view, int index);

/* How many entries carry `status`. */
int hyp_orphan_view_count_status(const hyp_orphan_view_t *view, hyp_anchor_status_t status);

/* ── Presentation ───────────────────────────────────────────────────────── */

/* Render one entry, or the whole view, in the form documented above. Returns a
 * malloc'd NUL-terminated string the caller frees, or NULL on allocation
 * failure. Deterministic: the same (record, index state) renders byte-for-byte
 * the same, which is what lets the signal below carry a stable id. */
char *hyp_orphan_render_entry(const hyp_orphan_entry_t *entry);
char *hyp_orphan_render(const hyp_orphan_view_t *view);

/* ── The one thing that may be written ──────────────────────────────────── */

/* Build the `signal` that records an orphaning observation. It is a NEW
 * record:
 *
 *   kind    = signal — an observation, which is what this is;
 *   parent  = the observed record's id, which is how it is found again;
 *   anchor  = NONE. The anchor it observed does not resolve, and a record is
 *             never created already-orphaned; carrying it would mint a second
 *             unresolvable anchor to explain the first;
 *   content = the rendering, so the evidence travels with the observation.
 *
 * `author` and `timestamp_ms` are the caller's (I6: nothing here reads a
 * clock, so the same observation re-derived on two machines is one record).
 * Refuses an entry whose anchor attaches — there is nothing to observe.
 *
 * Appending is the caller's move, with hyp_record_store_append. That is
 * deliberate: detection and writing are different acts, and a sweep that
 * appended as it went could not be asked to just look. */
hyp_orphan_status_t hyp_orphan_signal_build(const hyp_orphan_entry_t *entry, const char *author,
                                            int64_t timestamp_ms, const hyp_record_t **out);

#endif /* HYP_MEMORY_ORPHAN_H */
