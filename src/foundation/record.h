#ifndef HYP_FOUNDATION_RECORD_H
#define HYP_FOUNDATION_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * record.h — the append-only record. One shape, five uses.
 *
 * A decision, a verdict, a summary, a signal and a transcript message are the
 * same thing wearing different labels: a piece of text, attributed to someone,
 * at a moment, optionally pointing at code. They differ in `kind` and in
 * nothing else, so they get one shape and one identity rule.
 *
 * Records are the OPPOSITE of the code index. The index is per-machine, tracks
 * the working tree, and is regenerable — losing it costs CPU. Records are
 * global, derived from nothing, and lost forever if lost. That asymmetry is
 * what buys everything below: because a record can never be recomputed, it may
 * never be rewritten either.
 *
 * ── The one design choice, and what it deletes ───────────────────────────────
 *
 * MERGING TWO STORES IS A UNION. There is no conflict resolution, no
 * last-writer-wins, no merge UI, and there is no place to put one. Two machines
 * that have never spoken, that were offline for a week, that ingested the same
 * feed in a different order, converge by concatenating and deduplicating. That
 * single property deletes the entire class of distributed-write problems, and
 * immutability is the price paid deliberately to get it.
 *
 * It only holds because identity is DERIVED FROM CONTENT. Two machines that
 * independently observe the same event compute the same id with no
 * coordination, so the union deduplicates them; two different events can never
 * land on one id, so the union never has to choose between payloads. Choosing
 * between payloads IS last-writer-wins, whatever it is called.
 *
 * ── Immutability, made structural rather than documented ─────────────────────
 *
 * Three independent mechanisms, because a comment saying "do not mutate this"
 * is not a mechanism:
 *
 *   1. Every member of hyp_record_t is const-qualified. `rec->author = x` and
 *      `*rec = other` are COMPILE ERRORS, not review findings. A struct with
 *      const members is not assignable, so there is no whole-record overwrite
 *      either. tests/test_record_contract.sh proves the compiler still refuses.
 *   2. There is no setter. The only producer is hyp_record_build(), the only
 *      shape it produces is finished, and nothing in this header takes a
 *      non-const hyp_record_t *.
 *   3. hyp_record_verify() recomputes the id from the fields. Anything that
 *      casts away const and edits a byte becomes DETECTABLE, and every entry
 *      point that accepts a record from elsewhere calls it. Tamper-evidence is
 *      the backstop for the case the compiler cannot see.
 *
 * ── The id, and exactly what goes into it ────────────────────────────────────
 *
 *   id = SHA-256, lowercase hex, of a domain-tagged, length-prefixed canonical
 *        encoding of EVERY field of the record — kind, author, timestamp,
 *        content, anchor, origin, thread, parent, redactions.
 *
 * content + author + timestamp are the SUBSTANCE of identity: they are what
 * makes two machines agree. The rest of the fields are in the preimage because
 * leaving them out makes the union LOSSY. Two signals with the same text, by
 * the same author, at the same millisecond, anchored to two different symbols
 * are two records; if the id covered only the three substantive fields they
 * would collide, and a union keyed on id would hold one id and two payloads —
 * with no way out except picking one. That is last-writer-wins by another name.
 * So the preimage is total: every field a client can read is a field the id
 * commits to.
 *
 * Three details of the encoding, each defending against a specific way this
 * goes wrong:
 *
 *   - DOMAIN TAG. The preimage opens with HYP_RECORD_CONTRACT. A future record
 *     shape is a different tag and therefore a disjoint id space, so a v2
 *     record can never be mistaken for a v1 one. Note the consequence: ADDING A
 *     FIELD CHANGES EVERY ID. The field set is a frozen contract, not a
 *     starting point.
 *   - KIND BY NAME, NEVER BY ORDINAL. The encoding uses the stable string from
 *     hyp_record_kind_name(). Reordering the enum would otherwise silently
 *     change every id in the store while every test still passed.
 *   - LENGTH-PREFIXED, WITH FIELD NAMES. Every field contributes its name and
 *     its byte length, so no value can impersonate a delimiter and no value can
 *     slide into the neighbouring field. This is what lets `anchor`, `origin`
 *     and `thread` stay genuinely opaque: nothing here needs to know what is
 *     inside them, so nothing here has an escaping rule to get wrong.
 *
 * ── Timestamps are CALLER-SUPPLIED, never captured ───────────────────────────
 *
 * hyp_record_build() does not read a clock. It cannot: a captured timestamp is
 * the local wall clock at the moment of the call, so the same event ingested on
 * two machines would produce two different ids and sync would be a duplicate
 * storm instead of a union. Ingesting the same feed twice would double the
 * store. The whole property depends on the timestamp being a fact ABOUT the
 * event rather than about the ingest.
 *
 * The rule for callers is therefore: the timestamp is the moment the event
 * happened. An adapter passes the SOURCE's own time (a message's creation time
 * in the feed it came from). Something authored right here calls
 * hyp_record_wall_clock_ms() once and passes the result. The consequence is
 * exactly the one that matters: re-ingesting the same source row produces a
 * byte-identical record, hence the same id, hence a union of one — re-ingest is
 * idempotent by construction rather than by an "already seen" table someone has
 * to remember to consult.
 *
 * The cost is that a caller can supply a wrong or dishonest time. That is
 * accepted: the id commits to the timestamp, so a different time is a different
 * record and can never silently rewrite an existing one. A record is a claim
 * about what someone said, not a notarised attestation of when.
 *
 * NOTE: hyp_now_ms() in platform.h is a MONOTONIC counter from an arbitrary
 * epoch, for measuring durations. It is not a wall clock and must never be used
 * as a record timestamp. hyp_record_wall_clock_ms() exists so there is exactly
 * one answer to "what time is it" on this path.
 *
 * ── Opaque fields, and why the opacity is load-bearing ───────────────────────
 *
 * `anchor` holds an address produced by the identity contract (C1). `origin`
 * holds whatever a feed adapter uses to name the row it came from. `thread`
 * holds whatever an adapter uses to name a conversation. This header treats all
 * three as byte strings: never parsed, never validated beyond a length cap,
 * never interpreted. Only the producer knows the format, and only the producer
 * may decode it.
 *
 * That is not laziness, it is the boundary. If the record shape learned to
 * parse a feed's identifiers it would acquire that feed's vocabulary, and the
 * next adapter would have to translate into the first adapter's dialect instead
 * of into ours. The test for the boundary is: a second adapter must be writable
 * without touching this file.
 *
 * `parent`, by contrast, holds one of OUR ids, so it is validated strictly — an
 * identifier from somewhere else in that field is a confident error rather than
 * a dangling reference discovered months later.
 */

/* Bumping this is a new id space for every record ever written. See above. */
#define HYP_RECORD_CONTRACT "hyp-record-v1"

/* Lowercase hex SHA-256, no NUL. Buffers are HYP_RECORD_ID_LEN + 1. */
#define HYP_RECORD_ID_LEN 64

/*
 * Caps. Every one of them is a refusal, never a truncation: truncating would
 * change the content and therefore the id, which is silent corruption wearing
 * the costume of success. An adapter that hits a cap must split or refuse the
 * row and say so.
 */
enum {
    HYP_RECORD_MAX_AUTHOR = 256,
    HYP_RECORD_MAX_ANCHOR = 1024,
    HYP_RECORD_MAX_ORIGIN = 512,
    HYP_RECORD_MAX_THREAD = 512,
    HYP_RECORD_MAX_CONTENT = 4 * 1024 * 1024
};

/*
 * Timestamp bounds, in milliseconds since the Unix epoch, UTC.
 *
 * The lower bound rejects 0, which is what a zeroed struct hands you — "unset"
 * must not be indistinguishable from "1970". The upper bound is 9999-12-31 and
 * exists to catch unit confusion at the boundary: a microsecond value pasted
 * into a millisecond field lands in the year 56000 and is caught on the first
 * row rather than after a feed has been ingested at the wrong resolution.
 */
#define HYP_RECORD_MIN_TIMESTAMP_MS INT64_C(1)
#define HYP_RECORD_MAX_TIMESTAMP_MS INT64_C(253402300799999)

/*
 * The five uses. A record's kind never changes what a record IS; it changes
 * what a reader does with it.
 */
typedef enum {
    HYP_RECORD_DECISION = 0, /* why something was done, written to be found later */
    HYP_RECORD_VERDICT,      /* the outcome of a measurement or a run */
    HYP_RECORD_SUMMARY,      /* a condensation of other records */
    HYP_RECORD_SIGNAL,       /* an observation: a failure, a smell, a datum */
    HYP_RECORD_MESSAGE,      /* one turn of a transcript */
    HYP_RECORD_KIND_COUNT
} hyp_record_kind_t;

/* Stable wire name for a kind, or NULL when the value is not a kind. These
 * strings are in the id preimage: renaming one rewrites every id. */
const char *hyp_record_kind_name(hyp_record_kind_t kind);

/* Inverse. False when the name is unknown — a reader that meets a kind it does
 * not have must say so, not fall back to a plausible neighbour. */
bool hyp_record_kind_from_name(const char *name, hyp_record_kind_t *out);

/*
 * Every refusal this contract can issue. There is no "probably fine" path: a
 * confident error beats a plausible wrong answer, and a record that is wrong is
 * permanent.
 */
typedef enum {
    HYP_RECORD_OK = 0,
    HYP_RECORD_ERR_NULL,      /* a required pointer was NULL */
    HYP_RECORD_ERR_KIND,      /* not one of the five kinds */
    HYP_RECORD_ERR_AUTHOR,    /* absent, empty, or over the cap */
    HYP_RECORD_ERR_TIMESTAMP, /* absent, non-positive, or out of range */
    HYP_RECORD_ERR_CONTENT,   /* absent, empty, or over the cap */
    HYP_RECORD_ERR_ANCHOR,    /* present but empty, or over the cap */
    HYP_RECORD_ERR_ORIGIN,    /* present but empty, or over the cap */
    HYP_RECORD_ERR_THREAD,    /* present but empty, or over the cap */
    HYP_RECORD_ERR_PARENT,    /* present but not a record id */
    HYP_RECORD_ERR_TAMPERED,  /* the id does not match the fields */
    HYP_RECORD_ERR_COLLISION, /* same id, different fields — never merged */
    HYP_RECORD_ERR_ALLOC      /* out of memory */
} hyp_record_status_t;

/* Stable, human-facing reason for a status. Never NULL. */
const char *hyp_record_status_reason(hyp_record_status_t status);

/*
 * What a caller supplies to build a record. Plain, mutable, caller-owned; all
 * strings are borrowed for the duration of the call and copied into the record.
 *
 * Absent and empty are different answers and are encoded differently. NULL on
 * an optional field means "there is nothing to look at here" — an unanchored
 * decision, a locally authored record with no upstream row, a record that
 * belongs to no conversation. The empty string is REFUSED on every field: it is
 * what a half-filled struct produces, and "anchored to nothing" is not a
 * statement anyone means to make.
 */
typedef struct {
    hyp_record_kind_t kind;

    /* Who wrote it: an agent id, a person, a harness. Ours, opaque, required. */
    const char *author;

    /* When the EVENT happened, UTC ms since the Unix epoch. Required. Never
     * captured here — see the header comment; this is what makes re-ingest a
     * union rather than a duplicate storm. */
    int64_t timestamp_ms;

    /* The text. UTF-8, required, never empty. Not validated as UTF-8: the
     * encoding is byte-transparent, and refusing a real transcript message for
     * a stray byte would lose data that cannot be recovered. */
    const char *content;

    /* An identity-contract address for the code this record is about, or NULL
     * when it is about nothing in particular. OPAQUE — copied, hashed, compared
     * byte-for-byte, never parsed. */
    const char *anchor;

    /* The feed adapter's own name for the row this came from, or NULL when the
     * record was authored here rather than ingested. OPAQUE.
     *
     * This field is what makes a completeness audit possible AT ALL. Identity
     * is content-derived, so two distinct source rows that happen to carry
     * identical text, author and time collapse into one record — and a count of
     * stored records against the source's count(*) then shows a permanent
     * shortfall that is indistinguishable from real loss. Dedup and loss look
     * the same from a count. Carrying the source's own identity makes distinct
     * rows distinct records, so the audit is a real comparison rather than a
     * number nobody can act on. Re-ingesting the SAME row still yields the same
     * id, so idempotence survives.
     *
     * The core never parses it: only the adapter that wrote the format can read
     * it back. */
    const char *origin;

    /* Which conversation this record belongs to, or NULL when it belongs to
     * none. OPAQUE, adapter-supplied, same discipline as origin.
     *
     * Membership, not linkage — `parent` is a pointer at one other record,
     * `thread` is belonging to a set. They are not alternatives: a verdict on a
     * decision that was taken in a conversation points at that decision without
     * being part of the conversation.
     *
     * It exists because reconstructing a conversation from parent links alone
     * FAILS OPEN: one missing record silently splits a session into two
     * components, every count still balances, and nothing reports the break.
     * The reader then believes it has a whole conversation and does not — the
     * exact failure this system exists to prevent. */
    const char *thread;

    /* The id of another record this one points at, or NULL. A verdict names the
     * decision it judges; a transcript message names its predecessor. Unlike
     * the opaque fields this one is OURS, so it is validated as a record id.
     *
     * A parent that is not present in the store is a DANGLING reference, and
     * that is normal, not an error: under partial sync the parent may simply
     * not have arrived. Readers must treat absence as "look elsewhere". */
    const char *parent;

    /* How many spans a secret scrub replaced before this record was built.
     *
     * Scrubbing MUST happen before the build, because the id commits to the
     * content — a scrub afterwards would be a mutation, which is impossible by
     * construction. Recording the count is what lets a reader tell "verbatim"
     * (0) from "cleaned, two secrets removed" (2) without diffing against a
     * source it may not have. */
    uint32_t redactions;
} hyp_record_input_t;

/*
 * A finished record. Every member is const: mutation is a compile error, and
 * the whole struct is not assignable. The only way to obtain one is
 * hyp_record_build(); the only way to release one is hyp_record_free().
 *
 * All strings are owned by the record and live in the same allocation, so a
 * record is exactly one malloc and exactly one free.
 */
typedef struct {
    const char *const id; /* HYP_RECORD_ID_LEN hex chars, never NULL */
    const hyp_record_kind_t kind;
    const char *const author;
    const int64_t timestamp_ms;
    const char *const content;
    const char *const anchor; /* NULL when unanchored */
    const char *const origin; /* NULL when authored here */
    const char *const thread; /* NULL when in no conversation */
    const char *const parent; /* NULL when it points at nothing */
    const uint32_t redactions;
} hyp_record_t;

/* Build a record. Validates every field and derives the id; on success *out
 * receives a record the caller owns. On failure *out is set to NULL and the
 * status says which field refused. */
hyp_record_status_t hyp_record_build(const hyp_record_input_t *in, const hyp_record_t **out);

/* Release a record. NULL is a no-op. */
void hyp_record_free(const hyp_record_t *rec);

/* Derive an id without building anything. Same inputs, same id, on any machine.
 * This is the cheap way for an ingest to ask "do I already have this row?"
 * before paying for the record. */
hyp_record_status_t hyp_record_derive_id(const hyp_record_input_t *in,
                                         char out[HYP_RECORD_ID_LEN + 1]);

/* Recompute the id from the fields and compare. False means the record was
 * altered after it was built, or was never built by this contract. Every entry
 * point that accepts a record from outside its own process calls this. */
bool hyp_record_verify(const hyp_record_t *rec);

/* Field-by-field equality, ignoring the id. Used to prove that a merge altered
 * nothing — asserting equal ids would only prove that two hashes matched, which
 * is the thing under test rather than evidence about it. */
bool hyp_record_equal(const hyp_record_t *a, const hyp_record_t *b);

/* True when s is a well-formed record id: exactly HYP_RECORD_ID_LEN lowercase
 * hex characters. Uppercase is refused so an id has exactly one spelling. */
bool hyp_record_id_is_valid(const char *s);

/* Wall-clock milliseconds since the Unix epoch, UTC. NOT called by anything in
 * this contract — a caller authoring a record now calls it once and passes the
 * result, so there is one answer to "what time is it" instead of one per call
 * site. An adapter ingesting a feed uses the feed's time and never this. */
int64_t hyp_record_wall_clock_ms(void);

/*
 * ── A set of records, and the union ──────────────────────────────────────────
 *
 * A set holds at most one record per id. It is the smallest thing that can
 * express the property the whole contract exists for, and the store built on
 * top of it (C1u/C2u) must not weaken it:
 *
 *   union         merge(A, B) contains every record of A and of B, altered in
 *                 no way. Nothing is lost and nothing is rewritten.
 *   commutative   merge(A, B) equals merge(B, A). Sync order is not a fact
 *                 about the result, so two peers converge without agreeing on
 *                 who goes first.
 *   idempotent    merge(A, A) equals A, and merging the same peer twice changes
 *                 nothing. A retried sync is free rather than dangerous.
 *
 * There is no conflict resolution here because there is no conflict to resolve.
 * Two records with the same id are the same record. Two records with different
 * ids are different records and both are kept. The remaining case — the same id
 * with different fields — is a forged id or a SHA-256 collision, and it is
 * REFUSED, never merged and never overwritten. Refusing is not resolving.
 *
 * Being honest about that last branch: HYP_RECORD_ERR_COLLISION is unreachable
 * through this surface. Every record entering a set is rebuilt from its own
 * fields, so a set member's id is always the digest of its fields, and two
 * members sharing an id with different fields would BE a SHA-256 collision. A
 * forged record is caught earlier, as HYP_RECORD_ERR_TAMPERED. The branch stays
 * because the alternative is silently keeping whichever arrived first, and a
 * union that quietly drops a record is the one failure this contract exists to
 * make impossible. No test can exercise it; do not read its presence as
 * evidence that it has been.
 *
 * The set stores its own copy of everything added, so the caller keeps
 * ownership of what it passes and the set owns what it holds. Copying is also
 * how an added record gets verified: the copy is rebuilt from the fields, and a
 * record whose id does not match its own fields cannot get in.
 */
typedef struct hyp_record_set hyp_record_set_t;

hyp_record_set_t *hyp_record_set_create(void);

/* Frees the set and every record in it. NULL is a no-op. */
void hyp_record_set_free(hyp_record_set_t *set);

/* Add a copy of rec. *out_added (optional) distinguishes the two successes:
 * true when the record was new, false when an identical record was already
 * present — which is not an error, it is the union absorbing a duplicate.
 * Refuses HYP_RECORD_ERR_TAMPERED when rec's id does not match its fields, and
 * HYP_RECORD_ERR_COLLISION when the id is present with different fields. */
hyp_record_status_t hyp_record_set_add(hyp_record_set_t *set, const hyp_record_t *rec,
                                       bool *out_added);

/* Union src into dst. ATOMIC: everything is validated and copied before
 * anything is inserted, so a refusal leaves dst exactly as it was. A half-done
 * merge would be a store whose contents depend on where an error happened. */
hyp_record_status_t hyp_record_set_merge(hyp_record_set_t *dst, const hyp_record_set_t *src);

size_t hyp_record_set_count(const hyp_record_set_t *set);

/* The record with this id, or NULL. */
const hyp_record_t *hyp_record_set_get(const hyp_record_set_t *set, const char *id);

/* Enumeration in CANONICAL ORDER: ascending id. Content-addressed, so the order
 * is the same on every machine with no clock and no insertion history involved
 * — which is what makes two independently merged sets comparable at all.
 * Reading in time order is a query, and queries belong to the reader, not here.
 * Returns NULL when index is out of range. */
const hyp_record_t *hyp_record_set_at(const hyp_record_set_t *set, size_t index);

/* A digest over the ids in canonical order. Two sets have the same digest if
 * and only if they hold the same records: every id commits to its own fields
 * and every record in a set has been verified, so equal id sets are equal
 * record sets. This is what a client reads to decide whether two stores agree,
 * and it is how the union properties above are stated as a test. `out` must
 * hold HYP_RECORD_ID_LEN + 1 bytes. */
void hyp_record_set_digest(const hyp_record_set_t *set, char out[HYP_RECORD_ID_LEN + 1]);

#endif /* HYP_FOUNDATION_RECORD_H */
