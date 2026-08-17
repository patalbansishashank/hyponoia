#ifndef HYP_STORE_RECORD_STORE_H
#define HYP_STORE_RECORD_STORE_H

#include "foundation/record.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * record_store.h — durable persistence for the append-only record set (C1u
 * writer, C2u reader). record.h defines what a record IS; this file only makes
 * a set of them survive a process, and it must not weaken a single property
 * the contract already paid for.
 *
 * ── Not a log ────────────────────────────────────────────────────────────────
 *
 * The obvious implementation is an append-only log, and it is wrong. A log has
 * an order, the order is machine-local — sequence numbers, offsets, insertion
 * time — and the moment that order means anything, two stores that hold the
 * same records stop being the same store, and the union (record.h's one design
 * choice) is broken. So the on-disk form is an ID-KEYED SET, order
 * non-semantic:
 *
 *   - The record id is the ONLY key. The table is WITHOUT ROWID, so the
 *     engine-supplied insertion counter does not merely go unused — it does
 *     not exist. There is no sequence column, no offset, no "position", and
 *     nothing to leak into semantics, order, or sync.
 *   - Enumeration is CANONICAL: ascending id, always, on every machine — the
 *     same order hyp_record_set_at() gives. Two stores that received the same
 *     records in different orders enumerate identically and digest
 *     identically; write, reopen, enumerate is the same order too. Insertion
 *     history is not recoverable from the store, which is the point.
 *   - Reading in time order is the CALLER sorting by timestamp_ms, a field of
 *     the record. It is never this store remembering who came first.
 *
 * ── The writer refuses mutation by construction ──────────────────────────────
 *
 * Append is the entire write surface. There is no update, no delete, no
 * upsert, and no function through which either could be expressed — not a
 * forbidden path, an absent one. Appending an id the store already holds is a
 * no-op absorbed by the union (I8), verified to be the SAME record; a
 * different record under an existing id is refused, never merged and never
 * overwritten. Every record is verified on the way in AND on the way out —
 * a row edited behind the store's back (the id no longer the digest of the
 * fields) is a confident HYP_RECORD_STORE_ERR_TAMPERED, not data.
 *
 * ── What the reader is, and is not ───────────────────────────────────────────
 *
 * The reader returns records and their anchor STRINGS. It never resolves an
 * anchor, never derives a workspace, never decides whether an anchor still
 * points at live code — resolution is C3u, orphan-ness is C4u, both derived at
 * read time from (record, index state), neither stored here. Consequently
 * there is NO WORKSPACE COLUMN and no workspace argument on any query:
 * decisions are global; workspace scoping comes from resolving the anchor; an
 * unanchored decision genuinely has no workspace, and the reader can name
 * exactly those (unanchored_only) because absent is an answer, not a gap.
 *
 * anchor, origin and thread stay OPAQUE here (I7): equality is byte equality,
 * prefix is byte prefix (memcmp semantics — no wildcards, no case folding, no
 * character sets), and nothing is ever parsed. Timestamps come in on records;
 * this file reads no clock (I6).
 *
 * ── On disk ──────────────────────────────────────────────────────────────────
 *
 * One SQLite database, `records.db`, inside the store directory the caller
 * names; the filename is part of the on-disk contract. The schema records
 * HYP_RECORD_CONTRACT and open REFUSES a store speaking any other — a v2
 * store must never be half-read as v1. Rows store the kind by its stable NAME,
 * never the ordinal, so renumbering the enum cannot rewrite anyone's store.
 *
 * Thread safety: one handle must not be used concurrently. Cross-process
 * access is serialized by the engine (busy_timeout); durability is a synced
 * commit per append, so a killed process loses nothing already appended —
 * C6u's "the store IS the pending state" leans on exactly that.
 */

/* ── Handle and statuses ────────────────────────────────────────────────── */

typedef struct hyp_record_store hyp_record_store_t;

/*
 * Every refusal this surface can issue. Same discipline as the record
 * contract: there is no "probably fine" path, and a store that is wrong is
 * wrong permanently.
 */
typedef enum {
    HYP_RECORD_STORE_OK = 0,
    HYP_RECORD_STORE_ERR_NULL,      /* a required argument was NULL */
    HYP_RECORD_STORE_ERR_OPEN,      /* the directory or database could not be opened */
    HYP_RECORD_STORE_ERR_CONTRACT,  /* the store on disk speaks a different record contract */
    HYP_RECORD_STORE_ERR_IO,        /* the engine refused mid-operation */
    HYP_RECORD_STORE_ERR_TAMPERED,  /* a record's id does not match its fields */
    HYP_RECORD_STORE_ERR_COLLISION, /* same id, different fields — refused, never merged */
    HYP_RECORD_STORE_ERR_QUERY,     /* a query that contradicts itself */
    HYP_RECORD_STORE_ERR_ALLOC      /* out of memory */
} hyp_record_store_status_t;

/* Stable, human-facing reason for a status. Never NULL. */
const char *hyp_record_store_status_reason(hyp_record_store_status_t status);

/* ── Open, close ────────────────────────────────────────────────────────── */

/* Open (creating if absent) the record store in `dir`. The directory is
 * created 0700 if missing — records may hold things that were said in
 * private. Refuses a store written under a different record contract. */
hyp_record_store_status_t hyp_record_store_open(const char *dir, hyp_record_store_t **out);

/* Close the handle. NULL is a no-op. Records already appended are on disk;
 * close is bookkeeping, never a flush something could be lost by skipping. */
void hyp_record_store_close(hyp_record_store_t *store);

/* Last failure detail for this handle, for logs. Never NULL. */
const char *hyp_record_store_error(const hyp_record_store_t *store);

/* ── The writer (C1u): append is the entire write surface ───────────────── */

/* Append one record. *out_added (optional) distinguishes the two successes:
 * true when the record was new, false when the identical record was already
 * present — the union absorbing a duplicate, not an error (I8). Refuses
 * ERR_TAMPERED when rec's id does not match its fields, and ERR_COLLISION
 * when the id is on disk with different fields — which, as in record.h, is
 * unreachable short of a SHA-256 collision, and refused rather than resolved
 * because the alternative is silently keeping whichever arrived first. */
hyp_record_store_status_t hyp_record_store_append(hyp_record_store_t *store,
                                                  const hyp_record_t *rec, bool *out_added);

/* Union a set into the store — the primitive sync (C6u) is built from: push
 * is "append my set into theirs", pull is the reverse, and running it twice
 * is free. ATOMIC: one transaction; any refusal rolls the whole batch back,
 * so the store never holds half a merge. *out_added (optional) receives how
 * many records were new. */
hyp_record_store_status_t hyp_record_store_append_set(hyp_record_store_t *store,
                                                      const hyp_record_set_t *set,
                                                      size_t *out_added);

/* ── The reader (C2u) ───────────────────────────────────────────────────── */

/* How many records the store holds. */
hyp_record_store_status_t hyp_record_store_count(hyp_record_store_t *store, size_t *out_count);

/* The record with this id, or NULL — absent means "look elsewhere" and is OK,
 * not an error. The row is verified on the way out; the caller owns the
 * record and releases it with hyp_record_free(). */
hyp_record_store_status_t hyp_record_store_get(hyp_record_store_t *store, const char *id,
                                               const hyp_record_t **out);

/* Load the whole store into a set, canonical order, every row verified. The
 * caller owns the set (hyp_record_set_free). */
hyp_record_store_status_t hyp_record_store_load(hyp_record_store_t *store, hyp_record_set_t **out);

/* A digest over the ids in canonical order — the same value
 * hyp_record_set_digest() gives for the loaded set, so a store and an
 * in-memory set are directly comparable. Two stores agree if and only if
 * their digests do; there is no "N behind", because a union has no total
 * order. `out` must hold HYP_RECORD_ID_LEN + 1 bytes. */
hyp_record_store_status_t hyp_record_store_digest(hyp_record_store_t *store,
                                                  char out[HYP_RECORD_ID_LEN + 1]);

/*
 * A query. Every field is optional; a zeroed struct matches everything, and
 * active filters compose by AND. String matching is byte-exact — the opaque
 * fields are compared, never interpreted (I7).
 *
 * Refused as ERR_QUERY, because a category error deserves a confident error
 * rather than a plausible empty answer: anchored_only with unanchored_only;
 * an anchor or anchor_prefix alongside unanchored_only; the empty string on
 * any field (the contract never stores one, so asking for it is a mistake,
 * not a question — absent is spelled NULL); a parent that is not a record id;
 * a kind that is not one of the five. A coherent query that merely matches
 * nothing returns OK and an EMPTY set: empty means "there is nothing", and
 * only one of those answers is ever true.
 */
typedef struct {
    bool has_kind;
    hyp_record_kind_t kind; /* read only when has_kind */

    const char *author; /* exact bytes */

    const char *anchor;        /* exact bytes, never parsed */
    const char *anchor_prefix; /* leading bytes, memcmp semantics — no wildcards */
    bool anchored_only;        /* only records carrying an anchor (C4u reads these) */
    bool unanchored_only;      /* only records carrying none — genuinely no workspace */

    const char *origin; /* exact bytes */
    const char *thread; /* exact bytes */
    const char *parent; /* exact record id */

    bool has_since;
    int64_t since_ms; /* timestamp_ms >= since_ms, read only when has_since */
    bool has_until;
    int64_t until_ms; /* timestamp_ms <= until_ms, read only when has_until */
} hyp_record_store_query_t;

/* Run a query. The result is a set in canonical order — a time-range query
 * still enumerates by id; sorting by time is the caller's move. The caller
 * owns the set (hyp_record_set_free). */
hyp_record_store_status_t hyp_record_store_query(hyp_record_store_t *store,
                                                 const hyp_record_store_query_t *query,
                                                 hyp_record_set_t **out);

#endif /* HYP_STORE_RECORD_STORE_H */
