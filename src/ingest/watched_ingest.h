#ifndef HYP_INGEST_WATCHED_INGEST_H
#define HYP_INGEST_WATCHED_INGEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "foundation/record.h"

/*
 * watched_ingest.h — watched-directory ingest: the generic feed adapter (D5).
 *
 * A directory of append-only JSONL files is the integration surface that every
 * harness already has: anything that can append a line to a file can feed the
 * store, with no client library, no socket and no schema negotiation. This
 * adapter scans a watch root, parses what grew since the last scan, and yields
 * one record input per line. It is the SECOND adapter behind the feed boundary,
 * and it is written against foundation/record.h alone — if it needed anything
 * else from the core, the boundary would be in the wrong place.
 *
 * The full format document is docs/WATCHED-INGEST.md. The normative summary:
 *
 * ── The on-disk format (hyp feed v1) ─────────────────────────────────────────
 *
 * A feed file is UTF-8 JSON Lines: one JSON object per '\n'-terminated line,
 * appended and never rewritten. Files end in `.jsonl` and live anywhere under
 * the watch root; one file per session is the intended shape. The writer
 * supplies WHAT WAS SAID; the adapter derives WHERE IT CAME FROM.
 *
 *   field          required  meaning (maps 1:1 onto hyp_record_input_t)
 *   ─────          ────────  ──────────────────────────────────────────
 *   v              no        format version; absent means 1, anything else is
 *                            refused rather than guessed at
 *   kind           no        one of decision|verdict|summary|signal|message;
 *                            absent means "message"
 *   author         yes       who spoke: an agent id, a person, a harness
 *   timestamp_ms   yes       WHEN THE EVENT HAPPENED, integer milliseconds
 *                            since the Unix epoch, UTC — always the source's
 *                            own time; this adapter never reads a clock (I6)
 *   content        yes       the text, non-empty
 *   anchor         no        identity-contract address, passed through opaquely
 *   thread         no        conversation membership; ABSENT derives the file's
 *                            own thread (see below), explicit null means
 *                            "belongs to no conversation" — absent and null are
 *                            different answers, as everywhere in this contract
 *   parent         no        a hyp record id (64 lowercase hex) or null
 *
 * `origin`, `redactions` and `id` are REFUSED when a line supplies them: they
 * are the adapter's to derive, and silently ignoring an attempt to set them
 * would be fail-open provenance forgery. Unknown OTHER keys are ignored so a
 * harness may enrich its own lines without breaking ingest.
 *
 * ── Origin, and why it is positional ─────────────────────────────────────────
 *
 *   origin = "wd1:" <relpath> "@" <byte-offset>
 *   thread = "wd1:" <relpath>              (when the line does not say)
 *
 * <relpath> is the file's path relative to the watch root, '/'-separated;
 * <byte-offset> is the decimal offset of the line's first byte. The same file
 * position therefore re-ingests to the SAME record (same origin, same fields,
 * same id — I8, idempotence with no dedup table), and two byte-identical rows
 * at two positions stay two records — which is what makes a set-of-origins
 * completeness audit (D4) a real comparison instead of a count that cannot
 * tell dedup from loss. The consequence is stated plainly: feed files must be
 * APPENDED TO, never rewritten, renamed or truncated. A renamed file is a new
 * origin space and re-ingests as duplicates the union keeps; a shrunk file is
 * refused by name and stays refused.
 *
 * ── Scrubbing is in FRONT of this yield path, structurally ───────────────────
 *
 * The record id commits to content, so an unscrubbed record is permanent. The
 * scanner therefore takes a scrub callback at construction and REFUSES to be
 * built without one; every content string passes through it before the record
 * input is assembled, and the returned redaction count rides on the record.
 * The identity scrubber is for TESTS ONLY — real ingest wires the secret
 * scrubber (D2) here, and nothing else on this path may touch content.
 *
 * ── Failure is per file, named, and never blocking ───────────────────────────
 *
 * A malformed line fails ITS FILE closed: the valid prefix stays ingested, the
 * file is quarantined with a named reason and the offending offset, and every
 * other file is unaffected. A partial trailing line — a harness mid-append —
 * is not an error; it is "not yet", retried on the next scan. Failed files are
 * re-examined on later scans (a fixed file heals), except a shrunk file, which
 * stays refused for the scanner's lifetime because its consumed offsets can no
 * longer be trusted.
 *
 * ── What this is NOT ─────────────────────────────────────────────────────────
 *
 * The inotify/kqueue trigger belongs to the index watcher (C6u's territory);
 * this module is the scan-and-ingest function it will call, and scan_once() is
 * the whole surface. The sink is deliberately a plain callback taking a
 * hyp_record_input_t: the feed interface (D3) lands by implementing its yield
 * with this sink, and the scanner never builds records and never touches the
 * store.
 */

/* A single line, including its newline, may not exceed this. Refusal, never
 * truncation: truncating would change content and therefore the id. */
#define HYP_WATCHED_MAX_LINE (16u * 1024u * 1024u)

/* Where a file stands after a scan. */
typedef enum {
    HYP_WATCHED_FILE_OK = 0,  /* consumed to end of file */
    HYP_WATCHED_FILE_PENDING, /* trailing bytes with no newline — not yet */
    HYP_WATCHED_FILE_FAILED   /* quarantined; reason and detail say why */
} hyp_watched_file_state_t;

/* Every way a file can be refused. Named, because "malformed" is not a
 * finding anyone can act on. */
typedef enum {
    HYP_WATCHED_REASON_NONE = 0,      /* OK or PENDING */
    HYP_WATCHED_REASON_IO,            /* cannot stat, open or read the file */
    HYP_WATCHED_REASON_SHRUNK,        /* smaller than what was already ingested */
    HYP_WATCHED_REASON_LINE_TOO_LONG, /* a line over HYP_WATCHED_MAX_LINE */
    HYP_WATCHED_REASON_BAD_JSON,      /* a line that is not one JSON object */
    HYP_WATCHED_REASON_BAD_VERSION,   /* "v" present and not the integer 1 */
    HYP_WATCHED_REASON_BAD_FIELD,     /* wrong type, missing required, unknown kind */
    HYP_WATCHED_REASON_FORGED_FIELD,  /* the line supplied origin/redactions/id */
    HYP_WATCHED_REASON_ROW_REFUSED,   /* the record contract refused the row */
    HYP_WATCHED_REASON_SCRUB_FAILED,  /* the scrubber said no; the row must not pass */
    HYP_WATCHED_REASON_ALLOC          /* out of memory */
} hyp_watched_reason_t;

/* Stable, human-facing name for a reason. Never NULL. */
const char *hyp_watched_reason_name(hyp_watched_reason_t reason);

/* One watched file, as a client reads it after a scan. `relpath` and `detail`
 * are borrowed from the scanner and valid until the next scan or free. */
typedef struct {
    const char *relpath; /* '/'-separated, relative to the watch root */
    hyp_watched_file_state_t state;
    hyp_watched_reason_t reason; /* NONE unless FAILED */
    const char *detail;          /* names the line and the field; never NULL */
    uint64_t consumed;           /* offset of the first byte not yet ingested */
    uint64_t failed_at;          /* line-start offset of the refused line */
    uint64_t rows;               /* rows yielded from this file, lifetime */
} hyp_watched_file_t;

/*
 * The scrub seam (D2). Runs on every content string BEFORE the record input is
 * assembled — the only order that can produce a valid id, since the id commits
 * to content. On success returns true, stores a NUL-terminated scrubbed copy
 * the scanner will free(), and the number of spans replaced. Returning false
 * fails the file closed: a row that cannot be scrubbed must not be ingested.
 */
typedef bool (*hyp_watched_scrub_fn)(void *ctx, const char *content, size_t content_len,
                                     char **scrubbed_out, uint32_t *redactions_out);

/*
 * The yield. One call per row, fields valid only during the call; the row has
 * already been scrubbed, its origin/thread derived, and pre-validated against
 * the record contract, so hyp_record_build() on it cannot refuse. Return false
 * to abort the whole scan (a store error is not a file's fault): the row's
 * offset is NOT consumed, so the next scan re-yields it and nothing is lost.
 */
typedef bool (*hyp_watched_sink_fn)(void *ctx, const hyp_record_input_t *row);

typedef struct hyp_watched_scanner hyp_watched_scanner_t;

/* Per-scan totals. */
typedef struct {
    size_t files_seen;     /* candidate .jsonl files found this pass */
    size_t files_failed;   /* of those, quarantined after this pass */
    size_t files_pending;  /* of those, mid-append after this pass */
    uint64_t rows_yielded; /* rows handed to the sink this pass */
} hyp_watched_report_t;

/*
 * Create a scanner over a watch root. `scrub` is REQUIRED: passing NULL
 * returns NULL, because unscrubbed ingest must be impossible to express, not
 * merely discouraged. The scanner keeps in-memory per-file progress only —
 * losing it is safe, because a cold rescan re-derives the same origins and
 * therefore the same ids.
 */
hyp_watched_scanner_t *hyp_watched_scanner_create(const char *root, hyp_watched_scrub_fn scrub,
                                                  void *scrub_ctx);
void hyp_watched_scanner_free(hyp_watched_scanner_t *scanner);

/*
 * One pass: find new or grown `.jsonl` files under the root (recursively;
 * dotfiles and symlinks are skipped), parse what grew, yield rows in
 * deterministic order (files sorted by relpath, lines in file order). Returns
 * true when the pass completed — per-file failures are still true; read the
 * report and the file states. Returns false when the pass itself could not run
 * (bad arguments, unreadable root, out of memory) or the sink aborted it.
 * `out_report` may be NULL.
 */
bool hyp_watched_scan_once(hyp_watched_scanner_t *scanner, hyp_watched_sink_fn sink, void *sink_ctx,
                           hyp_watched_report_t *out_report);

/* The per-file view, for tests, operators and D4's audit. Files are in
 * relpath order; index out of range returns false. */
size_t hyp_watched_file_count(const hyp_watched_scanner_t *scanner);
bool hyp_watched_file_at(const hyp_watched_scanner_t *scanner, size_t index,
                         hyp_watched_file_t *out);

/*
 * The origin and default-thread derivations, as single sources of truth — the
 * scan uses these, and anything auditing a feed directory (D4) must use the
 * same functions rather than reimplement the format. False when the result
 * would not fit `cap` (a refusal, never a truncation).
 */
bool hyp_watched_origin(const char *relpath, uint64_t line_offset,
                        char out[HYP_RECORD_MAX_ORIGIN + 1], size_t cap);
bool hyp_watched_thread(const char *relpath, char out[HYP_RECORD_MAX_THREAD + 1], size_t cap);

#endif /* HYP_INGEST_WATCHED_INGEST_H */
