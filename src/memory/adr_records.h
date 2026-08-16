#ifndef HYP_MEMORY_ADR_RECORDS_H
#define HYP_MEMORY_ADR_RECORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "foundation/record.h"
#include "store/record_store.h"
#include "store/store.h"

/*
 * adr_records.h — the ADR document folded into the append-only record set.
 * Phase 1 Track C, unit C7u [C1u].
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHAT AN ADR IS IN THIS TREE, WHICH IS NOT WHAT THE NAME SUGGESTS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Not a directory of numbered decision documents. ONE markdown document per
 * project, capped at HYP_ADR_MAX_LENGTH, living in a single `project_summaries`
 * row keyed by project name, with sections a convention names (PURPOSE, STACK,
 * ARCHITECTURE, PATTERNS, TRADEOFFS, PHILOSOPHY) and nothing enforces. Two
 * writers reach it — the MCP tool `manage_adr(mode:"update")` and the UI's
 * `POST /api/adr` — and both wrote the same UPSERT, replacing the whole
 * document. The row carries `created_at` and `updated_at` and no author column.
 *
 * That is a decision store: mutable, last-writer-wins, and one where every
 * earlier text is destroyed by the write that supersedes it. Placing the
 * append-only record set beside it would give one concept two stores that can
 * disagree, which is the failure shape this plan has already shipped once at
 * the UI layer, at the data layer instead.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE MAPPING
 * ═════════════════════════════════════════════════════════════════════════
 *
 *   kind          decision. An ADR is why someone built it this way, written
 *                 down to be found later, which is that kind exactly.
 *   content       the document text, byte for byte. Never a section split:
 *                 the sections are a convention over one blob, and splitting
 *                 would invent boundaries the store never recorded.
 *   origin        the project id, verbatim. Distinct projects hold distinct
 *                 ADRs, so distinct origins keep them distinct records; the
 *                 same project re-migrated collapses to one id, which is the
 *                 idempotence this unit is accepted on.
 *   anchor        NULL. An ADR is about a project, not about a span. A guessed
 *                 anchor would attach a whole architecture document to whatever
 *                 symbol looked plausible, and a false attach is worse than
 *                 none.
 *   timestamp_ms  the row's own `updated_at`, parsed. NEVER a clock read at
 *                 migration time — see below, it is the whole ballgame.
 *   author        HYP_ADR_RECORD_AUTHOR. See below.
 *   thread        NULL — an ADR belongs to no conversation.
 *   parent        NULL — it points at no other record.
 *   redactions    0. Content is copied unchanged; nothing is scrubbed here,
 *                 and claiming a redaction that did not happen would be worse
 *                 than claiming none.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * ATTRIBUTION, AND WHY A CLOCK READ HERE WOULD BE THE BUG
 * ═════════════════════════════════════════════════════════════════════════
 *
 * TIMESTAMP. The record id commits to the timestamp. Reading a clock during
 * migration would stamp every folded ADR with the instant the migration ran,
 * so the same ADR migrated on two machines would produce two ids, and merging
 * those two stores would DOUBLE the corpus rather than deduplicate it — the
 * union is only a union because identity is derived from the event, never from
 * the ingest. So the timestamp is the row's `updated_at`: a durable fact stored
 * beside the text, identical on every machine holding that row. Migrating the
 * same store twice, or on two machines, yields one record set.
 *
 * `updated_at` and not `created_at`: the content in the row is the document as
 * it stood at its LAST write. Pairing the surviving text with the first write's
 * time would date a claim to a moment when the text said something else.
 *
 * AUTHOR. The ADR store never recorded one. There is no author column, the MCP
 * tool takes no author argument, and the UI endpoint sends none — so an ADR's
 * true author is not merely hard to find, it was never captured. The record
 * contract requires an author, so this file supplies a DETERMINISTIC
 * substitute: the name of the writer that produced the document. It is a
 * constant, identical on every machine, which is what the id needs; and it is
 * honest, because "written through manage_adr by someone unrecorded" is exactly
 * what is known. A per-machine substitute (the OS user, the hostname) would
 * satisfy the field and break the union, which is the same trap as a clock.
 *
 * REVISIONS. There are none to migrate, and that is a finding rather than a
 * shortcut. `project_summaries` holds one row per project and the write is an
 * UPSERT, so every text before the current one was overwritten in place and is
 * unrecoverable from disk. The migration therefore folds exactly one record per
 * ADR — the surviving text — and cannot do better, because the mutable store
 * already discarded the rest. FROM THE WRITER BELOW ONWARD every revision is
 * its own record: each write appends, nothing is replaced, and the document's
 * history accumulates instead of collapsing.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * ONE AUTHORITY, ONE PROJECTION
 * ═════════════════════════════════════════════════════════════════════════
 *
 * hyp_adr_write() is the ONE writer, and both ends call it. It does three
 * things in a fixed order:
 *
 *   1. folds every ADR the database already holds into the record store, so
 *      the text about to be superseded survives the write that supersedes it,
 *      and so does any row whose project was deleted out from under it;
 *   2. appends the incoming text as its own record;
 *   3. refreshes the `project_summaries` row FROM the record it just appended.
 *
 * Step 3 keeps a row that reads mutable, and the distinction that makes it
 * legitimate is worth stating plainly: the row is a PROJECTION, not a store. It
 * holds no fact the record set lacks, it is written by nothing except this
 * function, losing it costs nothing recoverable, and it exists because the UI's
 * ADR pane and `manage_adr(mode:"get")` read it — dropping it would ship a dead
 * pane, which is the very failure shape this unit exists to prevent. What went
 * away is not the row; it is the LOSS. No text reaches the projection without
 * first being a permanent record.
 *
 * The projection is stamped with the same instant the record carries, rendered
 * by the same encoder. That is what makes the fold IDEMPOTENT over the writer's
 * own output: re-reading the row this function wrote rebuilds the identical
 * record, so a later migration absorbs it as a duplicate instead of minting a
 * second id for the same document.
 */

/* The deterministic substitute for an author the ADR store never captured. It
 * is in the id preimage of every folded record: changing it re-mints the whole
 * migrated corpus, so treat it as frozen. */
#define HYP_ADR_RECORD_AUTHOR "manage_adr"

/* Seconds-resolution UTC instant, the encoding `project_summaries` stores.
 * Rendering and parsing are inverse over whole seconds, which is the property
 * the fold's idempotence rests on. */
#define HYP_ADR_TIMESTAMP_LEN 21

/* Parse "YYYY-MM-DDThh:mm:ssZ" to milliseconds since the Unix epoch, UTC.
 * False when the shape does not match or the instant is out of the record
 * contract's range — a confident refusal, never a guessed date. Civil-date
 * arithmetic only: no timezone database, no locale, same answer everywhere. */
bool hyp_adr_timestamp_to_ms(const char *iso, int64_t *out_ms);

/* Render milliseconds as "YYYY-MM-DDThh:mm:ssZ", truncating toward the epoch.
 * `buf` must hold at least HYP_ADR_TIMESTAMP_LEN bytes. */
bool hyp_adr_timestamp_from_ms(int64_t ms, char *buf, size_t bufsz);

/* Whole-second floor of a wall-clock reading, so a record's timestamp and its
 * projection row denote the same instant rather than one truncated copy. */
int64_t hyp_adr_floor_to_second_ms(int64_t ms);

/* Build the decision record for one ADR document. PURE: reads no clock, opens
 * nothing, and every input is a fact already on disk, so the same row yields
 * the same id on any machine. Fields per the mapping above. */
hyp_record_status_t hyp_adr_record_build(const char *project, const char *content,
                                         int64_t updated_at_ms, const hyp_record_t **out);

/* What a fold did. Counts are for the caller's log and for tests; the store's
 * digest is the authority on what it holds. */
typedef struct {
    size_t documents; /* ADR documents examined */
    size_t added;     /* records new to the store */
    size_t present;   /* records the store already held — the union absorbing */
    size_t refused;   /* documents that could not become a record, each named
                       * in the error string rather than skipped in silence */
} hyp_adr_fold_result_t;

/* Fold one project's ADR, if it has one, into `records`. A project with no ADR
 * is not an error: absent means there is nothing to fold, and the result says
 * documents == 0. Running it twice adds nothing the second time. */
hyp_record_store_status_t hyp_adr_fold_project(hyp_store_t *store, hyp_record_store_t *records,
                                               const char *project, hyp_adr_fold_result_t *out);

/* Fold every ADR the store holds. Same guarantees, over every project that
 * database has a document for. hyp_adr_write() runs it before every write, so
 * no ADR in a database anything writes to is left behind; a sweep across every
 * database on the machine needs a caller holding a lease on each, and this is
 * the function that caller runs. */
hyp_record_store_status_t hyp_adr_fold_store(hyp_store_t *store, hyp_record_store_t *records,
                                             hyp_adr_fold_result_t *out);

/* Open the machine's decision record store. `dir` NULL selects the default
 * location, which is the one rule for where records live: a `memory`
 * subdirectory of the resolved cache directory, so HYP_CACHE_DIR moves it and
 * a test never touches a real corpus. The caller closes with
 * hyp_record_store_close(). */
hyp_record_store_status_t hyp_adr_records_open(const char *dir, hyp_record_store_t **out);

/* THE writer behind every ADR update, MCP and UI alike. `event_ms` is the
 * moment the update happens, supplied by the caller because nothing on the
 * record path reads a clock; it is floored to a whole second so the record and
 * the projection denote one instant.
 *
 * Returns HYP_STORE_OK only when the record is durably appended AND the
 * projection reflects it. A record store that cannot be opened or appended to
 * is a FAILED write, never a quiet fall back to replacing the document: the
 * fallback is the mutable path, and having it available is the same as not
 * having removed it. */
int hyp_adr_write(hyp_store_t *store, hyp_record_store_t *records, const char *project,
                  const char *content, int64_t event_ms);

/* hyp_adr_write() against the machine's default record store, taking the event
 * instant from one wall-clock reading. This is what an ADR write site calls;
 * having it here rather than at each site is what keeps the MCP tool and the UI
 * endpoint provably on ONE path instead of on two that agree today. */
int hyp_adr_write_via_records(hyp_store_t *store, const char *project, const char *content);

#endif /* HYP_MEMORY_ADR_RECORDS_H */
