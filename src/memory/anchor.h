#ifndef HYP_MEMORY_ANCHOR_H
#define HYP_MEMORY_ANCHOR_H

#include <stdbool.h>
#include <stddef.h>

#include "foundation/constants.h"
#include "foundation/identity.h"
#include "store/store.h"

/*
 * anchor.h — anchor resolution against the live index. NEXT-STEPS §4 Phase 1,
 * unit C3u [C1].
 *
 * A record (C2) carries `anchor` as OPAQUE BYTES — the core never parses it,
 * and that opacity is load-bearing (record.h). This module is the one producer
 * and the one consumer of what those bytes mean for the memory subsystem, and
 * this header is where the format is written down. Nothing else may parse an
 * anchor; a second parser is a second thing to drift.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE ANCHOR STRING
 * ═════════════════════════════════════════════════════════════════════════
 *
 *      <address>                        hyp1:ws/repo#qn        (no hash)
 *      <span-hash>@<address>            3f9c…e2@hyp1:ws/repo#qn
 *
 * The span hash — HYP_ADDR_SPAN_HASH_LEN lowercase hex, exactly what
 * hyp_addr_span_hash produces — rides in FRONT of the address, not behind it.
 * That is not taste. The qualified name is the address's TERMINAL field and
 * may contain any printable byte, '@' included, so a suffix could collide
 * with a QN that happens to end in '@' + 32 hex chars — re-opening exactly
 * the ambiguity identity.h closed by making the QN terminal. A prefix cannot
 * collide: an address always begins "hyp1:" and 'y' is not a hex digit, so
 * the two grammars are disjoint at byte 1.
 *
 * The hash is OPTIONAL. Absent means "no content observation was recorded",
 * not "the content was empty" — an empty span still hashes to a value.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHAT RESOLUTION IS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Resolution classifies an anchor against the LIVE tree: symbol identity
 * first, content hash second, LINE NUMBERS NEVER. Lines appear only in the
 * answer — where the symbol is NOW — and are never consulted to decide it.
 * That is why a symbol that slides 200 lines stays resolved (C5u) with no
 * special case: there was never a line number in the question.
 *
 *   RESOLVED          the QN is in the index, and either no hash was recorded
 *                     or the recorded hash matches the span's current content.
 *   RESOLVED_EDITED   the QN is in the index, the hash differs. STILL
 *                     ATTACHED — the QN is the identity; the reasoning may be
 *                     stale, which is a different problem from being lost.
 *   ORPHANED          the QN is not in the index. When a hash was recorded,
 *                     the workspace is scanned for spans whose CURRENT content
 *                     hashes to it, and every one becomes a CANDIDATE.
 *   UNKNOWN_WORKSPACE the address names a workspace that matches none known.
 *                     A visible error NAMING the workspace, never an empty
 *                     result — empty would read as "nothing attached", which
 *                     is a different and false claim (§4 assertion table).
 *   ERROR             could not classify: unparseable anchor, store failure,
 *                     an unreadable span where a hash needed comparing. NOT a
 *                     state of the anchor; "I could not tell" must never be
 *                     read as any of the four above.
 *
 * ── Candidates are evidence, never a decision ───────────────────────────
 *
 * Invariant I5: cross-directory move, file rename and copy-paste all produce
 * the same observation — a different address whose content hashes the same,
 * HYP_ADDR_REL_CONTENT_ONLY — and this contract cannot tell them apart, so it
 * does not pretend to. A copy-paste yields SEVERAL candidates with the
 * original untouched among them; re-attaching to one automatically would pin
 * last month's reasoning to code that never had it. So: a hash match with a
 * different QN is NEVER status RESOLVED. This module GATHERS candidates with
 * their evidence; C4u presents them; nothing re-attaches.
 *
 * ── The rendezvous namespace ────────────────────────────────────────────
 *
 * A workspace-scoped address (no repo field — a route, a queue, an env key)
 * resolves against the whole workspace on the SAME code path: the scope comes
 * from hyp_addr_qn_scope (I2, shape-derived), and the only difference is the
 * set of projects the QN is looked up in — {the repo} versus {every project
 * in the store}. Two repos sharing the name is the point, so the QN found in
 * ANY project resolves.
 *
 * ── What "known workspace" means before A1/A6 land ──────────────────────
 *
 * The store predates the workspace table (A1) and the workspace resolver
 * (A6). Until they land, the caller states the workspace this index serves:
 *
 *   workspace != NULL   the anchor's workspace must equal it.
 *   workspace == NULL   workspace-of-one: each indexed project is its own
 *                       workspace under its own slug (identity.h — the
 *                       workspace id of a workspace of one IS the repo's
 *                       slug), so the anchor's workspace must name an indexed
 *                       project.
 *
 * A6's resolver plugs into this parameter; the classification logic does not
 * change when it does.
 *
 * ── Cost, stated ────────────────────────────────────────────────────────
 *
 * The candidate scan reads and hashes every span in the workspace — one file
 * read per node. Correct first; a batch caller (C4u sweeping a whole store)
 * should group by file or reuse the vector table's hashes once F2 keys them
 * on content, but no such shortcut is taken here because the vector table is
 * optional and gated on encoder parameters (I3 keeps those out of identity).
 */

/* ── The anchor string ───────────────────────────────────────────── */

/* Longest anchor, excluding the NUL: hash '@' address. */
#define HYP_ANCHOR_MAX (HYP_ADDR_SPAN_HASH_LEN + 1 + HYP_ADDR_MAX)

/* A parsed anchor: the address, and the recorded span hash if one rode along.
 * Branch on has_hash, never on span_hash[0] — absent and empty are different
 * claims and only one of them is ever true here. */
typedef struct {
    hyp_addr_t addr;
    char span_hash[HYP_ADDR_SPAN_HASH_LEN + 1]; /* "" iff !has_hash */
    bool has_hash;
} hyp_anchor_t;

/* Render an anchor string. `span_hash` may be NULL (no hash recorded); when
 * given it must be exactly what hyp_addr_span_hash produces. Returns false
 * and writes "" when the buffer cannot hold the whole anchor or the parts are
 * invalid — NEVER a truncation, for the same reason hyp_addr_format refuses:
 * a truncated anchor is a well-formed anchor for a different claim.
 * HYP_ANCHOR_MAX + 1 always suffices. */
bool hyp_anchor_format(const hyp_addr_t *addr, const char *span_hash, char *out, size_t out_sz);

/* Parse an anchor string. The exact inverse of hyp_anchor_format: it accepts
 * every string that function emits and no other. A malformed hash prefix is
 * HYP_ADDR_ERR_SCHEME — the string matched neither grammar, so it is not an
 * anchor. On refusal `out` is zeroed. */
hyp_addr_err_t hyp_anchor_parse(const char *s, hyp_anchor_t *out);

/* ── Classification ──────────────────────────────────────────────── */

typedef enum {
    /* Could not classify. Deliberately the zero value, so a zeroed struct
     * reads as "no classification happened", never as a state. */
    HYP_ANCHOR_ERROR = 0,
    HYP_ANCHOR_RESOLVED,
    HYP_ANCHOR_RESOLVED_EDITED,
    HYP_ANCHOR_ORPHANED,
    HYP_ANCHOR_UNKNOWN_WORKSPACE,
} hyp_anchor_status_t;

/* Stable, human-facing name. Never NULL, including for an unknown value. */
const char *hyp_anchor_status_str(hyp_anchor_status_t status);

/* One node whose CURRENT span content hashes to the anchor's recorded hash.
 * Evidence for C4u to present — never a re-attachment. */
typedef struct {
    char address[HYP_ADDR_MAX + 1]; /* the candidate's own canonical address */
    char qn[HYP_ADDR_QN_MAX + 1];
    char project[HYP_ADDR_SLUG_MAX + 1];
    char file_path[HYP_PATH_MAX]; /* where it is NOW — output, never input */
    int start_line;
    int end_line;
    char label[64];          /* Function, Class, Module, … — presentation aid */
    hyp_addr_rel_t relation; /* always HYP_ADDR_REL_CONTENT_ONLY */
} hyp_anchor_candidate_t;

/* The classification, and everything observed while making it. */
typedef struct {
    hyp_anchor_status_t status;
    hyp_anchor_t anchor; /* the parsed input, zeroed when parsing failed */

    /* RESOLVED / RESOLVED_EDITED: where the symbol is NOW. Lines are output
     * here and nowhere else — they were never an input to resolution. */
    char project[HYP_ADDR_SLUG_MAX + 1];
    char file_path[HYP_PATH_MAX];
    int start_line;
    int end_line;
    /* The span's current content hash, "" when it could not be read (only a
     * hashless RESOLVED can carry ""; a recorded hash that cannot be compared
     * is ERROR, because "could not tell" must not wear a state's clothes). */
    char current_hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    /* HYP_ADDR_REL_SAME or _EDITED when a recorded hash was compared;
     * HYP_ADDR_REL_INVALID when there was nothing to compare. */
    hyp_addr_rel_t relation;

    /* ORPHANED with a recorded hash: every content-hash match in the
     * workspace, sorted by address for deterministic presentation. NULL when
     * none. Candidates are a FLOOR whenever scan_skipped > 0. */
    hyp_anchor_candidate_t *candidates;
    int candidate_count;
    /* Spans the scan could not read or address. Nonzero means the candidate
     * list may be incomplete — C4u must disclose, not absorb. */
    int scan_skipped;

    /* ERROR / UNKNOWN_WORKSPACE: what happened, naming names. Always
     * non-empty for those two states. */
    char reason[512];
} hyp_anchor_res_t;

/* Classify `anchor` against `store`. `workspace` is the workspace this index
 * serves, NULL for workspace-of-one (see header comment). Always fills `out`
 * completely and returns out->status; on ERROR the reason says why. The store
 * is only read. */
hyp_anchor_status_t hyp_anchor_resolve(hyp_store_t *store, const char *workspace,
                                       const char *anchor, hyp_anchor_res_t *out);

/* Free the candidate array. Safe on NULL and on a zeroed struct. */
void hyp_anchor_res_free(hyp_anchor_res_t *res);

#endif /* HYP_MEMORY_ANCHOR_H */
