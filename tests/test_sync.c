/*
 * test_sync.c — sync as a union of append-only records (unit C6u).
 *
 * The organising claim is the assertion this unit carries: THERE IS NO OUTBOX.
 * Offline writes then sync gives equal digests, syncing twice changes nothing,
 * and a process killed between the write and the sync loses nothing, because
 * the store IS the pending state.
 *
 * The kill test is the one that matters, and it is the only one that can tell
 * "there is no outbox" from "we did not write one today": a second place a
 * write can live is invisible to every test that lets the writer finish. A
 * child appends records with the store's own writer, is killed with SIGKILL
 * before anything else can run — no unwind, no close, no flush — and the
 * parent then syncs and demands every record. The test asserts the child was
 * SIGNALLED rather than exited, because a control that did not fire proves
 * nothing about the code it was aimed at.
 *
 * Around it: direction is one-way when asked for one way, a peer on a
 * different record contract is refused before anything merges, a corrupted
 * peer's bytes cannot enter, and the store's own refusals arrive unreinterpreted.
 * The trigger is tested end to end through the PRODUCTION installer — the same
 * call the daemon makes — with a real git repository, and it asserts the client's
 * view: by the time the reindex callback runs, the peer's records are already
 * readable locally.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <daemon/bootstrap.h>
#include <foundation/compat.h>
#include <foundation/compat_fs.h>
#include <foundation/constants.h>
#include <foundation/platform.h>
#include <memory/sync.h>
#include <store/record_store.h>
#include <store/store.h>
#include <watcher/watcher.h>

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Caller-supplied and fixed: nothing in this file reads a clock to build a
 * record, so every id below is the same on every machine and every run. */
#define SY_FIXED_MS INT64_C(1770985600000)

enum { SY_PATH = 512 };

/* ── Fixtures ───────────────────────────────────────────────────────────── */

static const hyp_record_t *sy_rec(const char *content) {
    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_DECISION;
    in.author = "agent:c6u";
    in.timestamp_ms = SY_FIXED_MS;
    in.content = content;
    const hyp_record_t *rec = NULL;
    if (hyp_record_build(&in, &rec) != HYP_RECORD_OK) {
        return NULL;
    }
    return rec;
}

static int sy_append(const char *dir, const hyp_record_t *rec) {
    hyp_record_store_t *s = NULL;
    if (hyp_record_store_open(dir, &s) != HYP_RECORD_STORE_OK) {
        return 0;
    }
    hyp_record_store_status_t st = hyp_record_store_append(s, rec, NULL);
    hyp_record_store_close(s);
    return st == HYP_RECORD_STORE_OK;
}

static int sy_count(const char *dir, size_t *out) {
    hyp_record_store_t *s = NULL;
    if (hyp_record_store_open(dir, &s) != HYP_RECORD_STORE_OK) {
        return 0;
    }
    hyp_record_store_status_t st = hyp_record_store_count(s, out);
    hyp_record_store_close(s);
    return st == HYP_RECORD_STORE_OK;
}

static int sy_digest(const char *dir, char out[HYP_RECORD_ID_LEN + 1]) {
    hyp_record_store_t *s = NULL;
    if (hyp_record_store_open(dir, &s) != HYP_RECORD_STORE_OK) {
        return 0;
    }
    hyp_record_store_status_t st = hyp_record_store_digest(s, out);
    hyp_record_store_close(s);
    return st == HYP_RECORD_STORE_OK;
}

static int sy_holds(const char *dir, const char *id) {
    hyp_record_store_t *s = NULL;
    if (hyp_record_store_open(dir, &s) != HYP_RECORD_STORE_OK) {
        return 0;
    }
    const hyp_record_t *got = NULL;
    hyp_record_store_status_t st = hyp_record_store_get(s, id, &got);
    int held = st == HYP_RECORD_STORE_OK && got != NULL;
    hyp_record_free(got);
    hyp_record_store_close(s);
    return held;
}

/* One statement over a RAW connection: corrupting a row and rewriting the
 * contract tag are things the store's API deliberately cannot say. */
static int sy_raw_exec(const char *dir, const char *sql) {
    char path[SY_PATH];
    (void)snprintf(path, sizeof(path), "%s/records.db", dir);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        (void)sqlite3_close(db);
        return -1;
    }
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    sqlite3_free(err);
    (void)sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

/* ── The assertion row: offline writes, then sync ───────────────────────── */

TEST(offline_writes_then_sync_agree_and_syncing_twice_changes_nothing) {
    const char *tmp = th_mktempdir("hyp_c6u_union");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    char peer[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    (void)snprintf(peer, sizeof(peer), "%s/peer", root);

    /* Two machines that have never spoken. Each writes offline; neither is a
     * mode, both are ordinary local appends. */
    const hyp_record_t *mine = sy_rec("written here while the peer was unreachable");
    const hyp_record_t *theirs = sy_rec("written there while we were unreachable");
    const hyp_record_t *shared = sy_rec("observed independently by both");
    ASSERT_NOT_NULL(mine);
    ASSERT_NOT_NULL(theirs);
    ASSERT_NOT_NULL(shared);
    ASSERT_TRUE(sy_append(local, mine));
    ASSERT_TRUE(sy_append(local, shared));
    ASSERT_TRUE(sy_append(peer, theirs));
    ASSERT_TRUE(sy_append(peer, shared));

    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(local, peer, HYP_SYNC_BOTH, &result), HYP_SYNC_OK);
    ASSERT_FALSE(result.agreed_before);
    ASSERT_EQ(result.pulled, 1); /* theirs; shared was absorbed */
    ASSERT_EQ(result.pushed, 1); /* mine */
    ASSERT_STR_EQ(result.local_digest, result.peer_digest);

    /* Lossless in both directions: every record survives the merge intact. */
    size_t count = 0;
    ASSERT_TRUE(sy_count(local, &count));
    ASSERT_EQ(count, 3);
    ASSERT_TRUE(sy_count(peer, &count));
    ASSERT_EQ(count, 3);
    ASSERT_TRUE(sy_holds(local, theirs->id));
    ASSERT_TRUE(sy_holds(peer, mine->id));

    /* Sync twice → identical. A retried sync is free rather than dangerous,
     * and agreement is one digest comparison, not a position either side
     * remembers. */
    char after_first[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(sy_digest(local, after_first));
    hyp_sync_result_t again;
    ASSERT_EQ(hyp_sync_dirs(local, peer, HYP_SYNC_BOTH, &again), HYP_SYNC_OK);
    ASSERT_TRUE(again.agreed_before);
    ASSERT_EQ(again.pulled, 0);
    ASSERT_EQ(again.pushed, 0);
    ASSERT_STR_EQ(again.local_digest, after_first);
    ASSERT_STR_EQ(again.peer_digest, after_first);

    /* Commutative: the same exchange run from the peer's point of view lands
     * on the same digest, so which side ran it is not a fact about the result. */
    hyp_sync_result_t reversed;
    ASSERT_EQ(hyp_sync_dirs(peer, local, HYP_SYNC_BOTH, &reversed), HYP_SYNC_OK);
    ASSERT_TRUE(reversed.agreed_before);
    ASSERT_STR_EQ(reversed.local_digest, after_first);

    hyp_record_free(mine);
    hyp_record_free(theirs);
    hyp_record_free(shared);
    (void)th_rmtree(root);
    PASS();
}

/* ── The kill test ──────────────────────────────────────────────────────── */

#ifndef _WIN32
TEST(kill_between_write_and_sync_loses_nothing) {
    const char *tmp = th_mktempdir("hyp_c6u_kill");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    char peer[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    (void)snprintf(peer, sizeof(peer), "%s/peer", root);

    /* Ids are content-derived, so the parent knows exactly what the child is
     * about to write without the child telling it. */
    const hyp_record_t *r[3];
    r[0] = sy_rec("first record, committed before the kill");
    r[1] = sy_rec("second record, committed before the kill");
    r[2] = sy_rec("third record, the last thing the process ever did");
    ASSERT_NOT_NULL(r[0]);
    ASSERT_NOT_NULL(r[1]);
    ASSERT_NOT_NULL(r[2]);

    /* The parent holds no handle across the fork: the child's store is its
     * own, so what survives is what the engine committed, not what a shared
     * descriptor happened to flush. */
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        hyp_record_store_t *s = NULL;
        if (hyp_record_store_open(local, &s) != HYP_RECORD_STORE_OK) {
            _exit(2);
        }
        for (int i = 0; i < 3; i++) {
            if (hyp_record_store_append(s, r[i], NULL) != HYP_RECORD_STORE_OK) {
                _exit(3);
            }
        }
        /* No close, no finalize, no atexit, no chance to note anything down
         * anywhere else. This is the moment an outbox would still be empty. */
        (void)kill(getpid(), SIGKILL);
        _exit(4);
    }

    int status = 0;
    ASSERT_TRUE(waitpid(pid, &status, 0) == pid);
    /* The control must have fired: an ordinary exit here would make every
     * assertion below a statement about a clean shutdown instead. */
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);

    /* Reopen and sync. Nothing told the sync what was pending, because the
     * store is what pending means. */
    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(local, peer, HYP_SYNC_BOTH, &result), HYP_SYNC_OK);
    ASSERT_EQ(result.pushed, 3);
    ASSERT_STR_EQ(result.local_digest, result.peer_digest);

    size_t count = 0;
    ASSERT_TRUE(sy_count(local, &count));
    ASSERT_EQ(count, 3);
    ASSERT_TRUE(sy_count(peer, &count));
    ASSERT_EQ(count, 3);
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(sy_holds(local, r[i]->id));
        ASSERT_TRUE(sy_holds(peer, r[i]->id));
        hyp_record_free(r[i]);
    }

    (void)th_rmtree(root);
    PASS();
}
#endif /* _WIN32 */

/* ── No second place a write can live ───────────────────────────────────── */

TEST(the_store_is_the_only_thing_on_disk) {
    const char *tmp = th_mktempdir("hyp_c6u_shape");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    char peer[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    (void)snprintf(peer, sizeof(peer), "%s/peer", root);

    const hyp_record_t *rec = sy_rec("a decision worth keeping");
    ASSERT_NOT_NULL(rec);
    ASSERT_TRUE(sy_append(local, rec));
    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(local, peer, HYP_SYNC_BOTH, &result), HYP_SYNC_OK);
    ASSERT_EQ(result.pushed, 1);

    /*
     * Structural half of "no outbox": after a write AND a sync, the store
     * directory holds the database and the engine's own sidecars, and nothing
     * else. A pending list, a queue file or a cursor would show up here — and
     * would be state that can disagree with the store.
     */
    hyp_dir_t *d = hyp_opendir(local);
    ASSERT_NOT_NULL(d);
    int unexpected = 0;
    hyp_dirent_t *ent = NULL;
    while ((ent = hyp_readdir(d)) != NULL) {
        if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }
        if (strncmp(ent->name, "records.db", strlen("records.db")) != 0) {
            unexpected++;
        }
    }
    hyp_closedir(d);
    ASSERT_EQ(unexpected, 0);

    hyp_record_free(rec);
    (void)th_rmtree(root);
    PASS();
}

/* ── Direction ──────────────────────────────────────────────────────────── */

TEST(pull_and_push_each_move_one_way_only) {
    const char *tmp = th_mktempdir("hyp_c6u_dir");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    char peer[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    (void)snprintf(peer, sizeof(peer), "%s/peer", root);

    const hyp_record_t *mine = sy_rec("only this machine has this");
    const hyp_record_t *theirs = sy_rec("only the peer has this");
    ASSERT_NOT_NULL(mine);
    ASSERT_NOT_NULL(theirs);
    ASSERT_TRUE(sy_append(local, mine));
    ASSERT_TRUE(sy_append(peer, theirs));

    /* Pull takes theirs and leaves the peer exactly as it was. */
    hyp_sync_result_t pulled;
    ASSERT_EQ(hyp_sync_dirs(local, peer, HYP_SYNC_PULL, &pulled), HYP_SYNC_OK);
    ASSERT_EQ(pulled.pulled, 1);
    ASSERT_EQ(pulled.pushed, 0);
    size_t count = 0;
    ASSERT_TRUE(sy_count(local, &count));
    ASSERT_EQ(count, 2);
    ASSERT_TRUE(sy_count(peer, &count));
    ASSERT_EQ(count, 1);
    ASSERT_FALSE(sy_holds(peer, mine->id));
    /* A one-way exchange legitimately ends in disagreement: that is what was
     * asked for, and the result says so rather than implying convergence. */
    ASSERT_STR_NEQ(pulled.local_digest, pulled.peer_digest);

    /* Push sends what the peer lacks, and the two agree afterwards. */
    hyp_sync_result_t pushed;
    ASSERT_EQ(hyp_sync_dirs(local, peer, HYP_SYNC_PUSH, &pushed), HYP_SYNC_OK);
    ASSERT_EQ(pushed.pulled, 0);
    ASSERT_EQ(pushed.pushed, 1);
    ASSERT_STR_EQ(pushed.local_digest, pushed.peer_digest);
    ASSERT_TRUE(sy_holds(peer, mine->id));

    hyp_record_free(mine);
    hyp_record_free(theirs);
    (void)th_rmtree(root);
    PASS();
}

/* ── Fail closed ────────────────────────────────────────────────────────── */

TEST(a_peer_on_another_contract_is_refused_and_nothing_merges) {
    const char *tmp = th_mktempdir("hyp_c6u_contract");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    char peer[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    (void)snprintf(peer, sizeof(peer), "%s/peer", root);

    const hyp_record_t *mine = sy_rec("ours, under the contract this build speaks");
    const hyp_record_t *theirs = sy_rec("theirs, under a contract this build does not");
    ASSERT_NOT_NULL(mine);
    ASSERT_NOT_NULL(theirs);
    ASSERT_TRUE(sy_append(local, mine));
    ASSERT_TRUE(sy_append(peer, theirs));
    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(sy_digest(local, before));

    ASSERT_EQ(sy_raw_exec(peer, "UPDATE record_store_meta SET value = 'hyp-record-v2'"
                                " WHERE key = 'contract';"),
              0);

    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(local, peer, HYP_SYNC_BOTH, &result), HYP_SYNC_ERR_CONTRACT);
    /* Named: which side, and in the store's own vocabulary. */
    ASSERT_EQ(result.failed_side, HYP_SYNC_SIDE_PEER);
    ASSERT_EQ(result.store_status, HYP_RECORD_STORE_ERR_CONTRACT);
    ASSERT_TRUE(result.detail[0] != '\0');
    /* Refused BEFORE anything moved, in both directions. */
    ASSERT_FALSE(result.agreed_before);
    ASSERT_EQ(result.pulled, 0);
    ASSERT_EQ(result.pushed, 0);
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(sy_digest(local, after));
    ASSERT_STR_EQ(after, before);
    ASSERT_FALSE(sy_holds(local, theirs->id));

    hyp_record_free(mine);
    hyp_record_free(theirs);
    (void)th_rmtree(root);
    PASS();
}

TEST(a_corrupted_peer_cannot_get_its_bytes_in) {
    const char *tmp = th_mktempdir("hyp_c6u_tamper");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    char peer[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    (void)snprintf(peer, sizeof(peer), "%s/peer", root);

    const hyp_record_t *mine = sy_rec("ours, untouched");
    const hyp_record_t *theirs = sy_rec("theirs, about to be edited behind the store's back");
    ASSERT_NOT_NULL(mine);
    ASSERT_NOT_NULL(theirs);
    ASSERT_TRUE(sy_append(local, mine));
    ASSERT_TRUE(sy_append(peer, theirs));
    char before[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(sy_digest(local, before));

    ASSERT_EQ(sy_raw_exec(peer, "UPDATE records SET content = 'edited behind the store''s back';"),
              0);

    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(local, peer, HYP_SYNC_PULL, &result), HYP_SYNC_ERR_STORE);
    /* The taxonomy is carried out verbatim: tampered is not "sync failed". */
    ASSERT_EQ(result.store_status, HYP_RECORD_STORE_ERR_TAMPERED);
    ASSERT_EQ(result.failed_side, HYP_SYNC_SIDE_PEER);
    ASSERT_EQ(result.pulled, 0);
    char after[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(sy_digest(local, after));
    ASSERT_STR_EQ(after, before);

    hyp_record_free(mine);
    hyp_record_free(theirs);
    (void)th_rmtree(root);
    PASS();
}

TEST(a_store_cannot_exchange_with_itself) {
    const char *tmp = th_mktempdir("hyp_c6u_self");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    const hyp_record_t *rec = sy_rec("one store, named twice");
    ASSERT_NOT_NULL(rec);
    ASSERT_TRUE(sy_append(local, rec));

    /* Identical spelling, and the same directory reached by another spelling:
     * both are refused, because "we agree with ourselves" is trivially true
     * and answers a question nobody asked. */
    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(local, local, HYP_SYNC_BOTH, &result), HYP_SYNC_ERR_SAME_PATH);
    char aliased[SY_PATH];
    (void)snprintf(aliased, sizeof(aliased), "%s/./local", root);
    ASSERT_EQ(hyp_sync_dirs(local, aliased, HYP_SYNC_BOTH, &result), HYP_SYNC_ERR_SAME_PATH);
    ASSERT_FALSE(result.agreed_before);

    hyp_record_free(rec);
    (void)th_rmtree(root);
    PASS();
}

TEST(no_peer_configured_is_absent_never_agreement) {
    char saved[HYP_SZ_1K];
    bool had = hyp_safe_getenv("HYP_MEMORY_PEER", saved, sizeof(saved), NULL) != NULL;
    (void)hyp_unsetenv("HYP_MEMORY_PEER");

    char peer[SY_PATH];
    ASSERT_FALSE(hyp_memory_peer_dir(peer, sizeof(peer)));
    hyp_sync_result_t result;
    /* Absent means "look elsewhere"; it must never be reported as an exchange
     * that found nothing to do. */
    ASSERT_EQ(hyp_sync_configured(HYP_SYNC_BOTH, &result), HYP_SYNC_ERR_NO_PEER);
    ASSERT_FALSE(result.agreed_before);
    ASSERT_EQ(result.pulled, 0);
    ASSERT_EQ(result.pushed, 0);

    if (had) {
        (void)hyp_setenv("HYP_MEMORY_PEER", saved, 1);
    }
    PASS();
}

TEST(the_local_store_directory_has_one_answer) {
    char saved[HYP_SZ_1K];
    bool had = hyp_safe_getenv("HYP_MEMORY_DIR", saved, sizeof(saved), NULL) != NULL;

    (void)hyp_setenv("HYP_MEMORY_DIR", "/tmp/hyp-c6u-explicit", 1);
    char dir[SY_PATH];
    ASSERT_TRUE(hyp_memory_store_dir(dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "/tmp/hyp-c6u-explicit");

    /* Unset falls back to the cache directory, and never to the empty string:
     * a store nothing can name is not a default. */
    (void)hyp_unsetenv("HYP_MEMORY_DIR");
    if (hyp_memory_store_dir(dir, sizeof(dir))) {
        ASSERT_TRUE(dir[0] != '\0');
        ASSERT_NOT_NULL(strstr(dir, "memory"));
    }

    if (had) {
        (void)hyp_setenv("HYP_MEMORY_DIR", saved, 1);
    }
    PASS();
}

TEST(sync_status_reasons_are_present_and_distinct) {
    const hyp_sync_status_t all[] = {HYP_SYNC_OK,        HYP_SYNC_ERR_NULL,
                                     HYP_SYNC_ERR_NO_PEER, HYP_SYNC_ERR_SAME_PATH,
                                     HYP_SYNC_ERR_OPEN,  HYP_SYNC_ERR_CONTRACT,
                                     HYP_SYNC_ERR_STORE};
    size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const char *reason = hyp_sync_status_reason(all[i]);
        ASSERT_NOT_NULL(reason);
        ASSERT_TRUE(reason[0] != '\0');
        for (size_t j = i + 1; j < n; j++) {
            ASSERT_STR_NEQ(reason, hyp_sync_status_reason(all[j]));
        }
    }
    /* NULL arguments are refused, not guessed at. */
    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_exchange(NULL, NULL, HYP_SYNC_BOTH, &result), HYP_SYNC_ERR_NULL);
    ASSERT_EQ(hyp_sync_dirs(NULL, NULL, HYP_SYNC_BOTH, &result), HYP_SYNC_ERR_NULL);
    ASSERT_EQ(hyp_sync_dirs("/tmp", "/tmp", HYP_SYNC_BOTH, NULL), HYP_SYNC_ERR_NULL);
    PASS();
}

/* ── Both ends of the command, not just one ─────────────────────────────── */

TEST(the_sync_command_reaches_its_own_implementation) {
    /*
     * A subcommand this binary dispatches must also be a subcommand this
     * binary CLASSIFIES, and the two enumerations are written in different
     * files. An unlisted top-level command does not merely lose a daemon: it
     * falls through to the MCP client role and serves the protocol on stdin,
     * so `hyponoia sync` would hang instead of merging anything, and every
     * test of the sync surface itself would still pass.
     */
    char *plain[] = {"hyponoia", "sync", NULL};
    char *flags[] = {"hyponoia", "sync", "--peer", "/tmp/hyp-c6u-peer", NULL};
    ASSERT_EQ(hyp_daemon_process_role(2, plain), HYP_DAEMON_PROCESS_STATELESS);
    ASSERT_EQ(hyp_daemon_process_role(4, flags), HYP_DAEMON_PROCESS_STATELESS);
    /* And the other direction: an opaque tool argument that happens to be
     * spelled "sync" must never bypass the mandatory daemon. */
    char *tool[] = {"hyponoia", "cli", "search_graph", "sync", NULL};
    ASSERT_EQ(hyp_daemon_process_role(4, tool), HYP_DAEMON_PROCESS_LOCAL_CLI);
    PASS();
}

/* ── The trigger: a repository changes, and memory is already there ─────── */

/* Portable git, as in the watcher's own suite: identity and non-interactive
 * config injected with -c so no global config is needed. */
static int sy_git(const char *dir, const char *args) {
    char cmd[HYP_SZ_1K];
    (void)snprintf(cmd, sizeof(cmd),
                   "git -C \"%s\" -c user.name=t -c user.email=t@t.io "
                   "-c init.defaultBranch=master -c commit.gpgsign=false %s",
                   dir, args);
    return system(cmd);
}

/* What the reindex callback saw when it ran. The trigger's whole claim is
 * that memory is present BEFORE the reindex, so the count is read from
 * inside the callback rather than after the poll. */
static char sy_trigger_local[SY_PATH];
static int sy_trigger_index_calls;
static size_t sy_trigger_count_at_index;

static int sy_trigger_index_fn(const char *name, const char *path, void *ud) {
    (void)name;
    (void)path;
    (void)ud;
    sy_trigger_index_calls++;
    sy_trigger_count_at_index = 0;
    (void)sy_count(sy_trigger_local, &sy_trigger_count_at_index);
    return 0;
}

TEST(a_changed_repository_pulls_memory_in_the_same_pass) {
    const char *tmp = th_mktempdir("hyp_c6u_trigger");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    char peer[SY_PATH];
    char repo[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    (void)snprintf(peer, sizeof(peer), "%s/peer", root);
    (void)snprintf(repo, sizeof(repo), "%s/repo", root);
    ASSERT_TRUE(hyp_mkdir_p(repo, 0700));

    char saved_dir[HYP_SZ_1K];
    char saved_peer[HYP_SZ_1K];
    bool had_dir = hyp_safe_getenv("HYP_MEMORY_DIR", saved_dir, sizeof(saved_dir), NULL) != NULL;
    bool had_peer = hyp_safe_getenv("HYP_MEMORY_PEER", saved_peer, sizeof(saved_peer), NULL) != NULL;
    (void)hyp_setenv("HYP_MEMORY_DIR", local, 1);
    (void)hyp_setenv("HYP_MEMORY_PEER", peer, 1);
    (void)snprintf(sy_trigger_local, sizeof(sy_trigger_local), "%s", local);

    /* Somebody else recorded something about this work. */
    const hyp_record_t *theirs = sy_rec("why the parser stopped folding constants");
    ASSERT_NOT_NULL(theirs);
    ASSERT_TRUE(sy_append(peer, theirs));

    if (sy_git(repo, "init -q") != 0) {
        (void)th_rmtree(root);
        FAIL("git init failed");
    }
    char file[SY_PATH];
    (void)snprintf(file, sizeof(file), "%s/file.txt", repo);
    th_write_file(file, "hello\n");
    (void)sy_git(repo, "add file.txt");
    (void)sy_git(repo, "commit -q -m init");

    hyp_store_t *store = hyp_store_open_memory();
    hyp_watcher_t *w = hyp_watcher_new(store, sy_trigger_index_fn, NULL);
    ASSERT_NOT_NULL(w);
    /* The PRODUCTION installer — the same call the daemon host makes. A test
     * double here would prove the mechanism and nothing about the wiring. */
    hyp_sync_install_watcher_trigger(w);
    ASSERT_TRUE(hyp_watcher_watch(w, "trigger-repo", repo));

    sy_trigger_index_calls = 0;
    sy_trigger_count_at_index = 0;

    /* First poll establishes the baseline: no change, so no reindex AND no
     * pull. The trigger is a changed repository, not the existence of one. */
    ASSERT_EQ(hyp_watcher_poll_once(w), 0);
    ASSERT_EQ(sy_trigger_index_calls, 0);
    size_t count = 0;
    ASSERT_TRUE(sy_count(local, &count));
    ASSERT_EQ(count, 0);

    /* The tree changes. */
    th_append_file(file, "world\n");
    (void)sy_git(repo, "add file.txt");
    (void)sy_git(repo, "commit -q -m second");
    hyp_watcher_touch(w, "trigger-repo");
    (void)hyp_watcher_poll_once(w);

    ASSERT_EQ(sy_trigger_index_calls, 1);
    /* The client's view: by the time the reindex ran, the record was already
     * readable locally. Same pass, and memory first. */
    ASSERT_EQ(sy_trigger_count_at_index, 1);
    ASSERT_TRUE(sy_holds(local, theirs->id));

    /*
     * Live evidence for the union, taken from the wiring rather than from a
     * fixture: the two digests after a sync nothing in this test performed
     * directly. Printed because a number that only a passing assertion ever
     * saw is a number nobody has read.
     */
    char live_local[HYP_RECORD_ID_LEN + 1];
    char live_peer[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(sy_digest(local, live_local));
    ASSERT_TRUE(sy_digest(peer, live_peer));
    printf("  sync/live: after watcher-fired pull   local=%s\n", live_local);
    printf("  sync/live:                            peer =%s  equal=%s\n", live_peer,
           strcmp(live_local, live_peer) == 0 ? "yes" : "no");
    ASSERT_STR_EQ(live_local, live_peer);

    /* Idempotence on live input: a second change fires the trigger again, and
     * a second pull over the same peer moves nothing and changes no digest. */
    th_append_file(file, "again\n");
    (void)sy_git(repo, "add file.txt");
    (void)sy_git(repo, "commit -q -m third");
    hyp_watcher_touch(w, "trigger-repo");
    (void)hyp_watcher_poll_once(w);

    hyp_watcher_free(w);
    hyp_store_close(store);

    ASSERT_EQ(sy_trigger_index_calls, 2);
    char twice_local[HYP_RECORD_ID_LEN + 1];
    char twice_peer[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(sy_digest(local, twice_local));
    ASSERT_TRUE(sy_digest(peer, twice_peer));
    printf("  sync/live: after a second pull        local=%s\n", twice_local);
    printf("  sync/live:                            peer =%s  equal=%s\n", twice_peer,
           strcmp(twice_local, twice_peer) == 0 ? "yes" : "no");
    ASSERT_STR_EQ(twice_local, live_local);
    ASSERT_STR_EQ(twice_peer, live_peer);
    size_t after = 0;
    ASSERT_TRUE(sy_count(local, &after));
    ASSERT_EQ(after, 1);

    hyp_record_free(theirs);
    if (had_dir) {
        (void)hyp_setenv("HYP_MEMORY_DIR", saved_dir, 1);
    } else {
        (void)hyp_unsetenv("HYP_MEMORY_DIR");
    }
    if (had_peer) {
        (void)hyp_setenv("HYP_MEMORY_PEER", saved_peer, 1);
    } else {
        (void)hyp_unsetenv("HYP_MEMORY_PEER");
    }
    (void)th_rmtree(root);
    PASS();
}

#ifndef _WIN32
TEST(a_killed_writer_is_recovered_through_the_wired_trigger) {
    /*
     * The same claim as the kill test above, made on the path that actually
     * runs: nothing here calls the sync function. A writer on the other side
     * is killed between its writes and any sync; a repository changes here;
     * the watcher fires the production trigger; every record the killed
     * process committed is present locally when the reindex runs.
     */
    const char *tmp = th_mktempdir("hyp_c6u_wiredkill");
    ASSERT_NOT_NULL(tmp);
    char root[SY_PATH];
    (void)snprintf(root, sizeof(root), "%s", tmp);
    char local[SY_PATH];
    char peer[SY_PATH];
    char repo[SY_PATH];
    (void)snprintf(local, sizeof(local), "%s/local", root);
    (void)snprintf(peer, sizeof(peer), "%s/peer", root);
    (void)snprintf(repo, sizeof(repo), "%s/repo", root);
    ASSERT_TRUE(hyp_mkdir_p(repo, 0700));

    char saved_dir[HYP_SZ_1K];
    char saved_peer[HYP_SZ_1K];
    bool had_dir = hyp_safe_getenv("HYP_MEMORY_DIR", saved_dir, sizeof(saved_dir), NULL) != NULL;
    bool had_peer = hyp_safe_getenv("HYP_MEMORY_PEER", saved_peer, sizeof(saved_peer), NULL) != NULL;
    (void)hyp_setenv("HYP_MEMORY_DIR", local, 1);
    (void)hyp_setenv("HYP_MEMORY_PEER", peer, 1);
    (void)snprintf(sy_trigger_local, sizeof(sy_trigger_local), "%s", local);

    const hyp_record_t *r[3];
    r[0] = sy_rec("the other agent's first decision");
    r[1] = sy_rec("the other agent's second decision");
    r[2] = sy_rec("the last thing it committed before it died");
    ASSERT_NOT_NULL(r[0]);
    ASSERT_NOT_NULL(r[1]);
    ASSERT_NOT_NULL(r[2]);

    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        hyp_record_store_t *s = NULL;
        if (hyp_record_store_open(peer, &s) != HYP_RECORD_STORE_OK) {
            _exit(2);
        }
        for (int i = 0; i < 3; i++) {
            if (hyp_record_store_append(s, r[i], NULL) != HYP_RECORD_STORE_OK) {
                _exit(3);
            }
        }
        (void)kill(getpid(), SIGKILL);
        _exit(4);
    }
    int status = 0;
    ASSERT_TRUE(waitpid(pid, &status, 0) == pid);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);

    if (sy_git(repo, "init -q") != 0) {
        (void)th_rmtree(root);
        FAIL("git init failed");
    }
    char file[SY_PATH];
    (void)snprintf(file, sizeof(file), "%s/file.txt", repo);
    th_write_file(file, "hello\n");
    (void)sy_git(repo, "add file.txt");
    (void)sy_git(repo, "commit -q -m init");

    hyp_store_t *store = hyp_store_open_memory();
    hyp_watcher_t *w = hyp_watcher_new(store, sy_trigger_index_fn, NULL);
    ASSERT_NOT_NULL(w);
    hyp_sync_install_watcher_trigger(w);
    ASSERT_TRUE(hyp_watcher_watch(w, "wired-kill-repo", repo));
    sy_trigger_index_calls = 0;
    sy_trigger_count_at_index = 0;
    ASSERT_EQ(hyp_watcher_poll_once(w), 0);
    size_t count = 0;
    ASSERT_TRUE(sy_count(local, &count));
    ASSERT_EQ(count, 0);

    th_append_file(file, "world\n");
    (void)sy_git(repo, "add file.txt");
    (void)sy_git(repo, "commit -q -m second");
    hyp_watcher_touch(w, "wired-kill-repo");
    (void)hyp_watcher_poll_once(w);
    hyp_watcher_free(w);
    hyp_store_close(store);

    ASSERT_EQ(sy_trigger_index_calls, 1);
    ASSERT_EQ(sy_trigger_count_at_index, 3);
    char live_local[HYP_RECORD_ID_LEN + 1];
    char live_peer[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(sy_digest(local, live_local));
    ASSERT_TRUE(sy_digest(peer, live_peer));
    printf("  sync/live: killed writer, wired pull  local=%s\n", live_local);
    printf("  sync/live:                            peer =%s  equal=%s\n", live_peer,
           strcmp(live_local, live_peer) == 0 ? "yes" : "no");
    ASSERT_STR_EQ(live_local, live_peer);
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(sy_holds(local, r[i]->id));
        hyp_record_free(r[i]);
    }

    if (had_dir) {
        (void)hyp_setenv("HYP_MEMORY_DIR", saved_dir, 1);
    } else {
        (void)hyp_unsetenv("HYP_MEMORY_DIR");
    }
    if (had_peer) {
        (void)hyp_setenv("HYP_MEMORY_PEER", saved_peer, 1);
    } else {
        (void)hyp_unsetenv("HYP_MEMORY_PEER");
    }
    (void)th_rmtree(root);
    PASS();
}
#endif /* _WIN32 */

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(sync) {
    /* the assertion row */
    RUN_TEST(offline_writes_then_sync_agree_and_syncing_twice_changes_nothing);
#ifndef _WIN32
    RUN_TEST(kill_between_write_and_sync_loses_nothing);
#endif
    RUN_TEST(the_store_is_the_only_thing_on_disk);
    /* direction */
    RUN_TEST(pull_and_push_each_move_one_way_only);
    /* fail closed */
    RUN_TEST(a_peer_on_another_contract_is_refused_and_nothing_merges);
    RUN_TEST(a_corrupted_peer_cannot_get_its_bytes_in);
    RUN_TEST(a_store_cannot_exchange_with_itself);
    RUN_TEST(no_peer_configured_is_absent_never_agreement);
    RUN_TEST(the_local_store_directory_has_one_answer);
    RUN_TEST(sync_status_reasons_are_present_and_distinct);
    /* both ends */
    RUN_TEST(the_sync_command_reaches_its_own_implementation);
    /* the trigger */
    RUN_TEST(a_changed_repository_pulls_memory_in_the_same_pass);
#ifndef _WIN32
    RUN_TEST(a_killed_writer_is_recovered_through_the_wired_trigger);
#endif
}
