#ifndef HYP_MEMORY_SYNC_H
#define HYP_MEMORY_SYNC_H

#include "foundation/record.h"
#include "store/record_store.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * sync.h — two record stores made equal, by union (unit C6u).
 *
 * There is almost nothing here, and that is the deliverable. The record
 * contract made a merge a UNION — commutative, idempotent, lossless — and the
 * store turned that union into one atomic call, hyp_record_store_append_set().
 * Sync is therefore not a protocol to design: PUSH is "merge my set into
 * theirs", PULL is the same call with the arguments the other way round, and
 * this file is the plumbing that opens two stores, compares one digest each,
 * and moves records in whichever direction was asked for.
 *
 * Everything a sync engine normally contains is missing on purpose, and each
 * absence is load-bearing:
 *
 * ── There is no outbox, and no pending-write queue ───────────────────────────
 *
 * THE STORE IS THE PENDING STATE. A record is written with one synced commit
 * (WAL, synchronous FULL) and a commit that returned survives a kill, so
 * "what still has to be pushed" is answerable at any time from the store
 * alone: it is whatever the peer does not hold. An outbox would be a SECOND
 * place a write can live, and two places can disagree — a process killed
 * between appending the record and noting it in the outbox has a record no
 * sync will ever send, and one killed the other way round has an entry for a
 * record that does not exist. Neither failure is possible when there is only
 * one place to look. This is also why offline needs no mode: an offline write
 * is an ordinary local append, and the next sync merges it like any other.
 *
 * ── There is no cursor, no sequence number and no offset ─────────────────────
 *
 * A union has no total order, so "N behind" is not a quantity that exists.
 * Agreement is a single digest comparison (hyp_record_store_digest equals
 * hyp_record_set_digest of the loaded set), and it answers yes or no. What
 * moved is reported as counts of records this exchange added to each side —
 * a fact about this exchange, never a position either store keeps. Any cursor
 * would be machine-local order leaking back into the contract that exists to
 * be free of it.
 *
 * ── There is no conflict resolution, and nowhere to put one ──────────────────
 *
 * Two records with the same id are the same record; two with different ids are
 * different records and both are kept. The remaining case — one id, two field
 * sets — is refused by the store as ERR_COLLISION and surfaces here unchanged.
 * Refusing is not resolving. Nothing in this file chooses between payloads,
 * because choosing is last-writer-wins whatever it is called.
 *
 * ── It fails closed, and it never reinterprets a refusal ─────────────────────
 *
 * A peer speaking a different record contract is refused BEFORE a single
 * record moves, naming the side that refused — a half-merge across contracts
 * would be exactly the silent corruption the contract tag exists to prevent.
 * Every other refusal is the store's own word carried out verbatim in
 * hyp_sync_result_t::store_status: ERR_TAMPERED means a row's id is not the
 * digest of its fields (so a corrupted peer's bytes cannot enter), ERR_QUERY
 * is a category error, and neither is ever flattened into "sync failed".
 *
 * ── What it costs ────────────────────────────────────────────────────────────
 *
 * The store has no pagination and no delta primitive: load materialises every
 * match and append_set takes a whole set, so an exchange that has anything to
 * do is O(everything) on both sides. The digest compare in front of it makes
 * the common case — the two stores already agree — O(1), which is the case the
 * watcher trigger fires most often. An incremental exchange, if it is ever
 * needed, is digest-compare then id-set diff; it is NOT a cursor.
 *
 * ── Threads ──────────────────────────────────────────────────────────────────
 *
 * One record-store handle must not be used from two threads. The directory
 * entry points below open, exchange and close within the call, so a caller on
 * any thread — the watcher's poll thread included — gets handles nothing else
 * shares. Cross-process access is serialised by the engine.
 */

/* ── Where the stores live ──────────────────────────────────────────────── */

/*
 * THE local record store for this machine: HYP_MEMORY_DIR when set, otherwise
 * "memory" inside the resolved cache directory. One answer, so two callers
 * cannot sync a store nothing else reads. False when no path can be resolved
 * (no cache directory and no home), which is a refusal, not an empty string.
 */
bool hyp_memory_store_dir(char *out, size_t cap);

/*
 * The peer store to exchange with: HYP_MEMORY_PEER, a directory holding
 * another store of the same contract — a shared drive, a synced folder, a
 * checkout. False when none is configured, and that is ABSENT rather than
 * empty: a machine with no peer is not a machine that agrees with everyone.
 */
bool hyp_memory_peer_dir(char *out, size_t cap);

/* ── The exchange ───────────────────────────────────────────────────────── */

typedef enum {
    HYP_SYNC_PULL = 0, /* merge the peer's records into ours */
    HYP_SYNC_PUSH,     /* merge ours into the peer's */
    HYP_SYNC_BOTH      /* both; the union makes the order immaterial */
} hyp_sync_direction_t;

typedef enum { HYP_SYNC_SIDE_NONE = 0, HYP_SYNC_SIDE_LOCAL, HYP_SYNC_SIDE_PEER } hyp_sync_side_t;

typedef enum {
    HYP_SYNC_OK = 0,
    HYP_SYNC_ERR_NULL,      /* a required argument was NULL, or not a direction */
    HYP_SYNC_ERR_NO_PEER,   /* no peer configured — absent, not "nothing to do" */
    HYP_SYNC_ERR_SAME_PATH, /* both sides are one store; agreement with itself means nothing */
    HYP_SYNC_ERR_OPEN,      /* a store would not open; failed_side says which */
    HYP_SYNC_ERR_CONTRACT,  /* a side speaks a different record contract; nothing moved */
    HYP_SYNC_ERR_STORE      /* a store refused mid-exchange; store_status is its own word */
} hyp_sync_status_t;

/* Stable, human-facing reason for a status. Never NULL. */
const char *hyp_sync_status_reason(hyp_sync_status_t status);

enum { HYP_SYNC_DETAIL_MAX = 256 };

/*
 * What one exchange did. Counts are ADDITIONS made by this exchange, never a
 * position: `pulled` is how many records the local store did not have, and
 * `pushed` how many the peer did not. Both zero with agreed_before true is the
 * ordinary case and is not an error.
 *
 * The digests are read AFTER the exchange, so on a successful bidirectional
 * sync they are equal by construction and a caller can say so without
 * re-deriving it. On a one-directional sync they may legitimately differ: a
 * pull leaves the peer without our records, which is what was asked for.
 */
typedef struct {
    hyp_sync_direction_t direction;
    bool agreed_before; /* the digests matched, so nothing was loaded or moved */
    size_t pulled;
    size_t pushed;
    char local_digest[HYP_RECORD_ID_LEN + 1];
    char peer_digest[HYP_RECORD_ID_LEN + 1];

    /* On failure only. The store's status is propagated, never reinterpreted;
     * detail carries the failing store's own error text for a log. */
    hyp_sync_side_t failed_side;
    hyp_record_store_status_t store_status;
    char detail[HYP_SYNC_DETAIL_MAX];
} hyp_sync_result_t;

/*
 * Exchange between two OPEN handles. The caller owns both and must not share
 * either with another thread for the duration.
 */
hyp_sync_status_t hyp_sync_exchange(hyp_record_store_t *local, hyp_record_store_t *peer,
                                    hyp_sync_direction_t direction, hyp_sync_result_t *out);

/*
 * Exchange between two directories: open both, exchange, close both. Refuses
 * when the two resolve to the same store, and when either speaks a different
 * record contract.
 */
hyp_sync_status_t hyp_sync_dirs(const char *local_dir, const char *peer_dir,
                                hyp_sync_direction_t direction, hyp_sync_result_t *out);

/*
 * The configured exchange: hyp_memory_store_dir() against hyp_memory_peer_dir().
 * ERR_NO_PEER when none is configured — the caller decides whether that is
 * worth saying, and the watcher decides it is not.
 */
hyp_sync_status_t hyp_sync_configured(hyp_sync_direction_t direction, hyp_sync_result_t *out);

/* ── The trigger ────────────────────────────────────────────────────────── */

typedef struct hyp_watcher hyp_watcher_t;

/*
 * Install the memory pull on a watcher: when a watched repository changes
 * locally, the same poll pass that decides to re-index also pulls memory from
 * the configured peer.
 *
 * The trigger is the code index, not an agent and not a clock. A changed tree
 * is the moment an agent asks "what happened here", so the records about that
 * work must already be present when it asks; a clock would pull at moments
 * unrelated to anyone's question, and an agent-callable sync would make being
 * up to date depend on an agent choosing to ask. There is deliberately no MCP
 * tool for this.
 *
 * A pull failure never affects the re-index: they are independent, and a
 * missing peer is silent rather than a warning on every poll.
 */
void hyp_sync_install_watcher_trigger(hyp_watcher_t *watcher);

/* ── The manual case ────────────────────────────────────────────────────── */

/*
 * `hyponoia sync [--push|--pull] [--local <dir>] [--peer <dir>] [--json]`.
 * The only user-facing sync surface: a person types it. Returns a process exit
 * code — 0 agreed or exchanged, 1 refused, 2 unusable arguments.
 */
int hyp_cmd_sync(int argc, char **argv);

#endif /* HYP_MEMORY_SYNC_H */
