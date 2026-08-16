#ifndef HYP_FOUNDATION_SCRUB_H
#define HYP_FOUNDATION_SCRUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "foundation/record.h"

/*
 * scrub.h — the secret scrub, and the only door transcripts enter through.
 *
 * Full transcripts are wanted, and transcripts contain pasted keys — the
 * session that scheduled this unit contained two. The record contract (C2)
 * makes the consequence sharper than "embarrassing": the id commits to
 * `content`, records are immutable, and sync is a union, so an unscrubbed
 * record is PERMANENT and may already be on another machine by the time anyone
 * notices. There is no post-hoc scrub. Scrubbing after the build would change
 * the content, which changes the id, which is a different record — the
 * original is still there, still syncing.
 *
 * So the order is structural, not policed: scrub runs BEFORE record
 * construction, the scrubbed text is what the id binds, and the record's
 * frozen `redactions` field carries the span count so a reader can tell
 * "verbatim" (0) from "cleaned, two secrets removed" (2) without diffing
 * against a source it must never see. The correct order is the only order that
 * produces a valid id.
 *
 * Where that guarantee stops, precisely, because the difference is D1's whole
 * assertion: hyp_record_build() checks NOTHING about scrubbing. record.h (C2)
 * is frozen and public, so a caller that goes around this file can still mint
 * an unscrubbed record with `redactions` set by hand. What this file guarantees
 * is exact and local — through THIS door the count cannot be supplied and the
 * scrub cannot be skipped — and the obligation it leaves is that ingest opens
 * no second door. That is why D1's row reads "unscrubbed ingest is impossible",
 * asserted at D1's own boundary, rather than "a scrubber is available".
 *
 * ── One prefix table, three readers ──────────────────────────────────────────
 *
 * The vendor prefix list below is THE list. It lives in a header, rather than
 * as a static local inside the one detector that needed it first, so that
 * every detector links (or parses) ONE definition instead of carrying a copy:
 *
 *   1. hyp_config_validate() (src/cli/cli.c) — refuses a pasted key where an
 *      environment variable NAME belongs.
 *   2. hyp_scrub_text() (this file) — replaces pasted keys in transcript text
 *      before a record exists.
 *   3. scripts/check-secret-history.sh — the pre-push scanner parses the
 *      initializer line in scrub.c textually and refuses to push a key. It
 *      also parses HYP_SCRUB_MIN_BODY from this header, so the "how much body
 *      makes it a key" threshold has one home too.
 *
 * A prefix added to the table is therefore picked up by the config guard, the
 * scrub, the scrub's per-prefix test (which iterates the table rather than a
 * list of its own), and the push scanner, with no second site to forget.
 *
 * ── What a hit is ────────────────────────────────────────────────────────────
 *
 * Exactly the pre-push scanner's rule, deliberately — one definition of "looks
 * like a key", two enforcement points:
 *
 *   boundary  the byte before the prefix is not [A-Za-z0-9_] (or the prefix
 *             is at the start of the text) — "compa-..." is a word, not a key
 *   prefix    a table entry matches at this position
 *   body      at least HYP_SCRUB_MIN_BODY bytes of [A-Za-z0-9_-] follow the
 *             prefix; the replaced span extends to the END of that run
 *
 * The threshold is the scanner's reasoning verbatim: comfortably under the
 * shortest real key (Voyage ~40, Google 35), comfortably over the filler
 * fixtures tests use ("pa-abc123"). A short prefix-shaped word passes
 * untouched, which is what lets clean prose through verbatim.
 *
 * ── The marker: [REDACTED:<prefix>] ──────────────────────────────────────────
 *
 * The replacement PRESERVES A DISTINGUISHABLE MARKER: the span becomes
 * "[REDACTED:" + the matched prefix + "]". Decided, for three reasons:
 *
 *   - A reader (human or agent) can tell WHAT KIND of secret was removed —
 *     "[REDACTED:voy-]" says a Voyage-shaped key was here — without any way to
 *     recover it. A bare "[REDACTED]" would erase exactly the information a
 *     transcript reader legitimately needs ("which key did I paste?").
 *   - The kind is named BY THE PREFIX ITSELF, not by a prefix→vendor-name map,
 *     because such a map would be a second enumeration that drifts from the
 *     table. The prefix is public knowledge; only the body was secret.
 *   - The marker cannot itself match the hit rule ("]" is not a body byte), so
 *     scrubbing is idempotent: scrub(scrub(x)) == scrub(x), with no growth.
 *
 * The marker does NOT preserve the secret's length. Length is itself weak
 * information about a key, and a fixed-shape marker means two different keys
 * from one vendor scrub to byte-identical text — acceptable, because
 * `redactions` still says how many spans were removed and `origin` (C2) keeps
 * distinct source rows distinct.
 *
 * ── Determinism, and why it is load-bearing ──────────────────────────────────
 *
 * Two machines scrubbing the same bytes MUST produce the same bytes: the
 * scrubbed text is what the record id binds, and the union (I8/I9) only
 * deduplicates re-ingest if the ids agree. So the scrub is a pure function of
 * its input and the compiled-in table: no locale (explicit byte ranges, never
 * <ctype.h>), no clock, no randomness, no environment. Longest qualifying
 * prefix wins at a position, so a future nested prefix pair (say "sk-" and
 * "sk-proj-") still has exactly one deterministic answer.
 */

/* The minimum run of key-body bytes [A-Za-z0-9_-] after a vendor prefix for a
 * span to count as a key. Parsed textually by scripts/check-secret-history.sh;
 * keep the define on one line. */
#define HYP_SCRUB_MIN_BODY 20

/* The vendor prefix table. Defined in scrub.c — the initializer there is
 * parsed textually by scripts/check-secret-history.sh, which is why it must
 * stay a single line of string literals. */
extern const char *const hyp_vendor_prefixes[];
extern const size_t hyp_vendor_prefix_count;

/*
 * Scrub `text`. On success, *out_text receives a malloc'd NUL-terminated copy
 * with every hit replaced by its marker (caller frees), and *out_redactions
 * the number of spans replaced — 0 means the copy is byte-identical to the
 * input. Returns false ONLY when an argument is NULL or allocation fails; the
 * outputs are untouched on failure. The scrub never refuses text: deciding
 * whether text may become a record is hyp_record_build()'s job, not this one.
 */
bool hyp_scrub_text(const char *text, char **out_text, uint32_t *out_redactions);

/* The shape a scrubber must have, so the ingest seam below can take one as an
 * argument. hyp_scrub_text is the production scrubber; a test may pass an
 * instrumented one. Same contract: false only for NULL or allocation failure,
 * and byte-identical output for byte-identical input, on any machine. */
typedef bool (*hyp_scrub_fn)(const char *text, char **out_text, uint32_t *out_redactions);

/*
 * ── The ingest seam ──────────────────────────────────────────────────────────
 *
 * What a caller supplies to ingest text as a record. This is
 * hyp_record_input_t minus `redactions` — the omission IS the design. Through
 * this seam the redaction count is not validated, it is IMPOSSIBLE to supply:
 * the struct has nowhere to put one, so the count on the finished record is
 * exactly what the scrubber did, always. `content` is the RAW text, pre-scrub;
 * every other field means what it means in hyp_record_input_t (record.h).
 */
typedef struct {
    hyp_record_kind_t kind;
    const char *author;
    int64_t timestamp_ms;
    const char *content; /* RAW text; the record gets the scrubbed copy */
    const char *anchor;
    const char *origin;
    const char *thread;
    const char *parent;
} hyp_record_ingest_input_t;

/*
 * Scrub, then build: the two steps in the only valid order, as one call.
 *
 * On success *out receives a finished record (caller frees with
 * hyp_record_free) whose `content` is the scrubbed text and whose
 * `redactions` is the scrubber's span count. Every hyp_record_build()
 * refusal passes through unchanged — the seam adds no leniency.
 *
 * `scrub` is REQUIRED. Passing NULL refuses with HYP_RECORD_ERR_NULL and
 * builds nothing: an ingest path with no scrubber wired cannot construct a
 * record at all. THIS IS THE REFUSAL AN INGEST MUST ASSERT. A transcript store
 * constructs records ONLY through this seam, and its test asserts that ingest
 * with no scrubber wired is refused — not "a scrubber is available", but
 * "unscrubbed ingest is impossible", because only the second claim is about
 * the code. A scrubber that itself fails (allocation) refuses with
 * HYP_RECORD_ERR_ALLOC, again building nothing: fail closed, because a record
 * that slips past the scrub is permanent.
 */
hyp_record_status_t hyp_record_ingest_scrubbed(const hyp_record_ingest_input_t *in,
                                               hyp_scrub_fn scrub, const hyp_record_t **out);

#endif /* HYP_FOUNDATION_SCRUB_H */
