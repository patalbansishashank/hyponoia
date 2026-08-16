/*
 * sync.c — the union, run between two stores. Design and rationale in sync.h.
 */
#include "memory/sync.h"

#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "watcher/watcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SYNC_PATH_MAX = HYP_SZ_4K };

/* ── Where the stores live ──────────────────────────────────────────────── */

static bool sync_copy_path(char *out, size_t cap, const char *value) {
    int written = snprintf(out, cap, "%s", value);
    if (written <= 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool hyp_memory_store_dir(char *out, size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    char configured[SYNC_PATH_MAX];
    const char *env = hyp_safe_getenv("HYP_MEMORY_DIR", configured, sizeof(configured), NULL);
    if (env && env[0]) {
        return sync_copy_path(out, cap, env);
    }
    const char *cache = hyp_resolve_cache_dir();
    if (!cache || !cache[0]) {
        return false;
    }
    int written = snprintf(out, cap, "%s/memory", cache);
    if (written <= 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool hyp_memory_peer_dir(char *out, size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    char configured[SYNC_PATH_MAX];
    const char *env = hyp_safe_getenv("HYP_MEMORY_PEER", configured, sizeof(configured), NULL);
    if (!env || !env[0]) {
        return false;
    }
    return sync_copy_path(out, cap, env);
}

/* ── Statuses ───────────────────────────────────────────────────────────── */

const char *hyp_sync_status_reason(hyp_sync_status_t status) {
    switch (status) {
    case HYP_SYNC_OK:
        return "ok";
    case HYP_SYNC_ERR_NULL:
        return "a required argument was missing, or the direction was not one of pull/push/both";
    case HYP_SYNC_ERR_NO_PEER:
        return "no peer store is configured (HYP_MEMORY_PEER)";
    case HYP_SYNC_ERR_SAME_PATH:
        return "both sides resolve to the same store, which cannot exchange with itself";
    case HYP_SYNC_ERR_OPEN:
        return "a record store could not be opened";
    case HYP_SYNC_ERR_CONTRACT:
        return "a store speaks a different record contract; nothing was merged";
    case HYP_SYNC_ERR_STORE:
        return "a record store refused the exchange";
    }
    return "unknown sync status";
}

/* ── The exchange ───────────────────────────────────────────────────────── */

static void sync_result_init(hyp_sync_result_t *out, hyp_sync_direction_t direction) {
    memset(out, 0, sizeof(*out));
    out->direction = direction;
    out->failed_side = HYP_SYNC_SIDE_NONE;
    out->store_status = HYP_RECORD_STORE_OK;
}

/*
 * Carry the store's own refusal out unchanged. ERR_TAMPERED, ERR_COLLISION and
 * ERR_QUERY each mean something different and specific; collapsing them into
 * one "sync failed" would throw away the only information a caller can act on.
 */
static hyp_sync_status_t sync_fail(hyp_sync_result_t *out, hyp_sync_side_t side,
                                   hyp_record_store_status_t status,
                                   const hyp_record_store_t *store) {
    out->failed_side = side;
    out->store_status = status;
    const char *detail = store ? hyp_record_store_error(store) : "";
    (void)snprintf(out->detail, sizeof(out->detail), "%s",
                   detail && detail[0] ? detail : hyp_record_store_status_reason(status));
    return HYP_SYNC_ERR_STORE;
}

/* One direction of the union: every record of `from` merged into `to`. */
static hyp_sync_status_t sync_merge_one_way(hyp_record_store_t *from, hyp_sync_side_t from_side,
                                            hyp_record_store_t *to, hyp_sync_side_t to_side,
                                            size_t *out_added, hyp_sync_result_t *out) {
    hyp_record_set_t *set = NULL;
    hyp_record_store_status_t st = hyp_record_store_load(from, &set);
    if (st != HYP_RECORD_STORE_OK) {
        return sync_fail(out, from_side, st, from);
    }
    size_t added = 0;
    st = hyp_record_store_append_set(to, set, &added);
    hyp_record_set_free(set);
    if (st != HYP_RECORD_STORE_OK) {
        return sync_fail(out, to_side, st, to);
    }
    *out_added = added;
    return HYP_SYNC_OK;
}

hyp_sync_status_t hyp_sync_exchange(hyp_record_store_t *local, hyp_record_store_t *peer,
                                    hyp_sync_direction_t direction, hyp_sync_result_t *out) {
    if (!out) {
        return HYP_SYNC_ERR_NULL;
    }
    sync_result_init(out, direction);
    if (!local || !peer) {
        return HYP_SYNC_ERR_NULL;
    }
    bool do_pull = direction == HYP_SYNC_PULL || direction == HYP_SYNC_BOTH;
    bool do_push = direction == HYP_SYNC_PUSH || direction == HYP_SYNC_BOTH;
    if (!do_pull && !do_push) {
        return HYP_SYNC_ERR_NULL;
    }

    /*
     * Agreement is one digest each. Equal digests mean equal record sets — the
     * id commits to every field and every stored row is verified — so there is
     * nothing to load, nothing to merge, and nothing that could be "behind".
     */
    hyp_record_store_status_t st = hyp_record_store_digest(local, out->local_digest);
    if (st != HYP_RECORD_STORE_OK) {
        return sync_fail(out, HYP_SYNC_SIDE_LOCAL, st, local);
    }
    st = hyp_record_store_digest(peer, out->peer_digest);
    if (st != HYP_RECORD_STORE_OK) {
        return sync_fail(out, HYP_SYNC_SIDE_PEER, st, peer);
    }
    if (strcmp(out->local_digest, out->peer_digest) == 0) {
        out->agreed_before = true;
        return HYP_SYNC_OK;
    }

    /* Pull first, so a caller asking for both has the peer's records the
     * moment the call returns. The union is commutative, so the order is a
     * property of this function and never of the result. */
    if (do_pull) {
        hyp_sync_status_t rc = sync_merge_one_way(peer, HYP_SYNC_SIDE_PEER, local,
                                                  HYP_SYNC_SIDE_LOCAL, &out->pulled, out);
        if (rc != HYP_SYNC_OK) {
            return rc;
        }
    }
    if (do_push) {
        hyp_sync_status_t rc = sync_merge_one_way(local, HYP_SYNC_SIDE_LOCAL, peer,
                                                  HYP_SYNC_SIDE_PEER, &out->pushed, out);
        if (rc != HYP_SYNC_OK) {
            return rc;
        }
    }

    st = hyp_record_store_digest(local, out->local_digest);
    if (st != HYP_RECORD_STORE_OK) {
        return sync_fail(out, HYP_SYNC_SIDE_LOCAL, st, local);
    }
    st = hyp_record_store_digest(peer, out->peer_digest);
    if (st != HYP_RECORD_STORE_OK) {
        return sync_fail(out, HYP_SYNC_SIDE_PEER, st, peer);
    }
    return HYP_SYNC_OK;
}

/* ── Over directories ───────────────────────────────────────────────────── */

static hyp_sync_status_t sync_open_side(const char *dir, hyp_sync_side_t side,
                                        hyp_record_store_t **out_store, hyp_sync_result_t *out) {
    hyp_record_store_status_t st = hyp_record_store_open(dir, out_store);
    if (st == HYP_RECORD_STORE_OK) {
        return HYP_SYNC_OK;
    }
    out->failed_side = side;
    out->store_status = st;
    /* The refused handle is gone, so the store cannot report its own detail;
     * name the path instead, because "which store" is the actionable half. */
    (void)snprintf(out->detail, sizeof(out->detail), "%s: %s", dir ? dir : "(null)",
                   hyp_record_store_status_reason(st));
    return st == HYP_RECORD_STORE_ERR_CONTRACT ? HYP_SYNC_ERR_CONTRACT : HYP_SYNC_ERR_OPEN;
}

/*
 * A store cannot exchange with itself. Two paths naming one directory would
 * report agreement — trivially true and completely uninformative — where the
 * caller meant two machines' stores, so it is a named refusal instead.
 */
static bool sync_same_store(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    if (strcmp(a, b) == 0) {
        return true;
    }
    char *ca = malloc(SYNC_PATH_MAX);
    char *cb = malloc(SYNC_PATH_MAX);
    bool same = false;
    if (ca && cb && hyp_canonical_path(a, ca, SYNC_PATH_MAX) == 1 &&
        hyp_canonical_path(b, cb, SYNC_PATH_MAX) == 1) {
        same = strcmp(ca, cb) == 0;
    }
    free(ca);
    free(cb);
    return same;
}

hyp_sync_status_t hyp_sync_dirs(const char *local_dir, const char *peer_dir,
                                hyp_sync_direction_t direction, hyp_sync_result_t *out) {
    if (!out) {
        return HYP_SYNC_ERR_NULL;
    }
    sync_result_init(out, direction);
    if (!local_dir || !local_dir[0] || !peer_dir || !peer_dir[0]) {
        return HYP_SYNC_ERR_NULL;
    }

    hyp_record_store_t *local = NULL;
    hyp_sync_status_t rc = sync_open_side(local_dir, HYP_SYNC_SIDE_LOCAL, &local, out);
    if (rc != HYP_SYNC_OK) {
        return rc;
    }
    hyp_record_store_t *peer = NULL;
    rc = sync_open_side(peer_dir, HYP_SYNC_SIDE_PEER, &peer, out);
    if (rc != HYP_SYNC_OK) {
        hyp_record_store_close(local);
        return rc;
    }
    /* Both directories exist by now (open creates them), so canonicalization
     * can answer. */
    if (sync_same_store(local_dir, peer_dir)) {
        hyp_record_store_close(peer);
        hyp_record_store_close(local);
        (void)snprintf(out->detail, sizeof(out->detail), "%s", local_dir);
        return HYP_SYNC_ERR_SAME_PATH;
    }

    rc = hyp_sync_exchange(local, peer, direction, out);
    hyp_record_store_close(peer);
    hyp_record_store_close(local);
    return rc;
}

hyp_sync_status_t hyp_sync_configured(hyp_sync_direction_t direction, hyp_sync_result_t *out) {
    if (!out) {
        return HYP_SYNC_ERR_NULL;
    }
    sync_result_init(out, direction);
    char local_dir[SYNC_PATH_MAX];
    if (!hyp_memory_store_dir(local_dir, sizeof(local_dir))) {
        return HYP_SYNC_ERR_OPEN;
    }
    char peer_dir[SYNC_PATH_MAX];
    if (!hyp_memory_peer_dir(peer_dir, sizeof(peer_dir))) {
        return HYP_SYNC_ERR_NO_PEER;
    }
    return hyp_sync_dirs(local_dir, peer_dir, direction, out);
}

/* ── The trigger ────────────────────────────────────────────────────────── */

/*
 * Fired by the watcher in the same poll pass that detected a local change.
 * PULL only: a changed tree is a question about to be asked, so what the rest
 * of the world knows must arrive. Push belongs to the moment a record is
 * written here, not to the moment someone else's tree moved.
 */
static void sync_watcher_pull(const char *project_name, const char *root_path, void *context) {
    (void)root_path;
    (void)context;
    hyp_sync_result_t result;
    hyp_sync_status_t st = hyp_sync_configured(HYP_SYNC_PULL, &result);
    if (st == HYP_SYNC_ERR_NO_PEER) {
        /* A machine with no peer is the common case, and every poll saying so
         * would be noise that trains people to ignore this log. */
        return;
    }
    if (st != HYP_SYNC_OK) {
        hyp_log_warn("memory.pull.err", "project", project_name ? project_name : "", "reason",
                     hyp_sync_status_reason(st), "detail", result.detail);
        return;
    }
    if (result.agreed_before || result.pulled == 0) {
        return;
    }
    char pulled[HYP_SZ_32];
    (void)snprintf(pulled, sizeof(pulled), "%zu", result.pulled);
    hyp_log_info("memory.pull", "project", project_name ? project_name : "", "records", pulled);
}

void hyp_sync_install_watcher_trigger(hyp_watcher_t *watcher) {
    hyp_watcher_set_memory_sync(watcher, sync_watcher_pull, NULL);
}
