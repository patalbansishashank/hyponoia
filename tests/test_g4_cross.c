/*
 * test_g4_cross.c — cross-machine and cross-harness (unit G4).
 *
 * The claim under test is the one the plan states last and tests nowhere else:
 * a decision written by one agent is found by a different agent, in a
 * different harness, on a different machine.
 *
 * ── WHAT "A DIFFERENT MACHINE" IS HERE, AND WHAT IT IS NOT ───────────────────
 *
 * There is one machine. Rather than pretend otherwise, this file names the
 * properties that make a second machine different, constructs the ones it can,
 * and refuses to claim the ones it cannot. Constructed:
 *
 *   a different filesystem root and cache   two store directories under two
 *                                           roots, HYP_MEMORY_DIR and
 *                                           HYP_CACHE_DIR pointed at each, so
 *                                           the two stores have never seen one
 *                                           another and neither can reach the
 *                                           machine's real memory.
 *   a different process                     every write and every read below
 *                                           happens in a forked child that
 *                                           sets its own environment and exits;
 *                                           nothing is carried across in
 *                                           memory, and the parent asserts
 *                                           against bytes on disk.
 *   a different clock reading               the record contract takes the
 *                                           timestamp from the caller and reads
 *                                           no clock, so clock skew between two
 *                                           machines reduces EXACTLY to a
 *                                           different value in timestamp_ms.
 *                                           Supplying the same value is
 *                                           therefore not a proxy for two
 *                                           agreeing clocks — it is the same
 *                                           thing. The two children below run
 *                                           at measurably different instants
 *                                           and in different TZ settings, and
 *                                           the parent asserts the instants
 *                                           differed, so "the clock does not
 *                                           reach the id" is observed rather
 *                                           than assumed.
 *   a different workspace-id derivation     two checkouts of the same member
 *                                           repositories at different paths.
 *                                           A6 flagged this as the residual and
 *                                           left it for this unit to measure;
 *                                           it is measured below rather than
 *                                           asserted away.
 *
 * NOT constructed, and no test here should be read as covering them: a second
 * physical machine; a different operating system, libc or filesystem
 * semantics; a different CPU word size or endianness; clock skew across a
 * network with a transport in between; two builds of hyponoia at different
 * versions exchanging records. fork() gives a separate process and a separate
 * address space, not a separate program image.
 *
 * ── WHAT "A DIFFERENT HARNESS" IS HERE ───────────────────────────────────────
 *
 * That half needs no simulation, and the crossings are deliberate rather than
 * incidental. Records are written through the MCP writer and through the CLI
 * comment migrator; they are moved by the CLI sync command and by the
 * directory-level exchange; they are read back through the MCP reader, through
 * the store's own query surface, and through the loaded set. Every crossing
 * below writes through one wire and reads through another.
 *
 * ── THE TWO NEGATIVE CONTROLS ────────────────────────────────────────────────
 *
 * Neither is decoration and neither is hypothetical. The first captures a
 * timestamp instead of taking the caller's, which is the one thing I6 forbids,
 * and shows the two machines minting two ids for one decision — a duplicate
 * that no later merge can repair, because records are never mutated. The
 * second rewrites ids during a merge, which is what any machine-local sequence
 * number does, and shows the cross-machine find failing by id: the record
 * agent A wrote is not in agent B's store under the name agent A knows it by.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <foundation/compat.h>
#include <foundation/identity.h>
#include <foundation/record.h>
#include <mcp/mcp.h>
#include <memory/comment_migrate.h>
#include <memory/sync.h>
#include <pipeline/pipeline.h>
#include <store/record_store.h>
#include <store/workspace_resolve.h>
#include <yyjson/yyjson.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

enum { G4_PATH = 512, G4_TEXT = 8192 };

/*
 * Caller-supplied and fixed. Every id derived from these is the same on every
 * machine and in every run, which is the property the first test is about.
 */
#define G4_FIXED_MS INT64_C(1770985600000)
#define G4_AUTHOR "agent:g4-alpha"
#define G4_DECISION                                                                  \
    "The encoder contract is pinned because a vector produced under one contract "   \
    "and read under another compares as similarity rather than refusing, and a "     \
    "silent mixture of two spaces is indistinguishable from a bad retriever."

/* A marker distinctive enough that a free-text search for it cannot match
 * anything else in a store. */
#define G4_MARKER "quenched-carbide-lattice"

/* ── Small file helpers ─────────────────────────────────────────────────── */

static bool g4_read_file(const char *path, char *out, size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return false;
    }
    size_t got = fread(out, 1, cap - 1, fh);
    (void)fclose(fh);
    out[got] = '\0';
    return got > 0;
}

/* ── Two machines ───────────────────────────────────────────────────────── */

typedef struct {
    char root[G4_PATH];
    char memory[G4_PATH];
    char cache[G4_PATH];
} g4_machine_t;

static bool g4_machine_make(g4_machine_t *m, const char *base, const char *name) {
    if (snprintf(m->root, sizeof(m->root), "%s/%s", base, name) < 0 ||
        snprintf(m->memory, sizeof(m->memory), "%s/%s/memory", base, name) < 0 ||
        snprintf(m->cache, sizeof(m->cache), "%s/%s/cache", base, name) < 0) {
        return false;
    }
    /* The memory directory is created 0700 by the store's own open path; making
     * it here would test this file's mkdir instead of the store's. */
    return th_mkdir_p(m->root) == 0 && th_mkdir_p(m->cache) == 0;
}

/*
 * Become this machine. Called only inside a forked child, so the parent's
 * environment — and the environment every other suite in this binary runs
 * under — is never touched. HYP_MEMORY_PEER is cleared because a peer
 * inherited from the surrounding shell would make the exchange under test an
 * exchange with something else.
 */
static bool g4_become(const g4_machine_t *m) {
    return hyp_setenv("HYP_MEMORY_DIR", m->memory, 1) == 0 &&
           hyp_setenv("HYP_CACHE_DIR", m->cache, 1) == 0 &&
           hyp_setenv("HYP_INDEX_SUPERVISOR", "0", 1) == 0 && hyp_unsetenv("HYP_MEMORY_PEER") == 0;
}

#ifndef _WIN32
/*
 * Run `body` in a child and return its exit status, or a negative value when
 * the child did not exit normally. A child that fails exits non-zero itself;
 * falling off the end is success.
 */
static int g4_child(void (*body)(const void *), const void *ctx) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        body(ctx);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return -2;
    }
    if (!WIFEXITED(status)) {
        return -3;
    }
    return WEXITSTATUS(status);
}

/* The transport, through the CLI command a person types — a third harness
 * between the writer and the reader, run in its own process so nothing about
 * the exchange is carried in this one's memory. */
typedef struct {
    const g4_machine_t *from;
    const g4_machine_t *to;
} g4_sync_ctx_t;

static void g4_push(const void *vctx) {
    const g4_sync_ctx_t *ctx = (const g4_sync_ctx_t *)vctx;
    if (!g4_become(ctx->from)) {
        _exit(11);
    }
    char *argv[] = {(char *)"--push",
                    (char *)"--local",
                    (char *)ctx->from->memory,
                    (char *)"--peer",
                    (char *)ctx->to->memory};
    _exit(hyp_cmd_sync((int)(sizeof(argv) / sizeof(argv[0])), argv));
}

static int g4_run_sync_push(const g4_machine_t *from, const g4_machine_t *to) {
    g4_sync_ctx_t ctx = {.from = from, .to = to};
    return g4_child(g4_push, &ctx);
}
#endif /* _WIN32 */

/* ── Store helpers ──────────────────────────────────────────────────────── */

static bool g4_count(const char *dir, size_t *out) {
    hyp_record_store_t *s = NULL;
    if (hyp_record_store_open(dir, &s) != HYP_RECORD_STORE_OK) {
        return false;
    }
    bool ok = hyp_record_store_count(s, out) == HYP_RECORD_STORE_OK;
    hyp_record_store_close(s);
    return ok;
}

static bool g4_digest(const char *dir, char out[HYP_RECORD_ID_LEN + 1]) {
    hyp_record_store_t *s = NULL;
    if (hyp_record_store_open(dir, &s) != HYP_RECORD_STORE_OK) {
        return false;
    }
    bool ok = hyp_record_store_digest(s, out) == HYP_RECORD_STORE_OK;
    hyp_record_store_close(s);
    return ok;
}

/* The id of the only record in a store, refusing when there is not exactly
 * one — "the first of several" would make every comparison below ambiguous. */
static bool g4_only_id(const char *dir, char out[HYP_RECORD_ID_LEN + 1]) {
    out[0] = '\0';
    hyp_record_store_t *s = NULL;
    if (hyp_record_store_open(dir, &s) != HYP_RECORD_STORE_OK) {
        return false;
    }
    hyp_record_set_t *set = NULL;
    bool ok = hyp_record_store_load(s, &set) == HYP_RECORD_STORE_OK && set &&
              hyp_record_set_count(set) == 1U;
    if (ok) {
        const hyp_record_t *rec = hyp_record_set_at(set, 0U);
        ok = rec != NULL && snprintf(out, HYP_RECORD_ID_LEN + 1, "%s", rec->id) > 0;
    }
    hyp_record_set_free(set);
    hyp_record_store_close(s);
    return ok;
}

static bool g4_holds(const char *dir, const char *id) {
    hyp_record_store_t *s = NULL;
    if (hyp_record_store_open(dir, &s) != HYP_RECORD_STORE_OK) {
        return false;
    }
    const hyp_record_t *got = NULL;
    bool held = hyp_record_store_get(s, id, &got) == HYP_RECORD_STORE_OK && got != NULL;
    hyp_record_free(got);
    hyp_record_store_close(s);
    return held;
}

/* ── The clock ──────────────────────────────────────────────────────────── */

/* Spin until the wall clock has moved. Two machines whose clocks read the same
 * millisecond would make the captured-timestamp control pass for the wrong
 * reason, so the control's premise is established rather than hoped for. */
static void g4_advance_clock(int64_t ms) {
    int64_t start = hyp_record_wall_clock_ms();
    while (hyp_record_wall_clock_ms() - start < ms) {
        /* deliberately busy: sleeping is not portable at this resolution */
    }
}

/* ── The client's view of an MCP answer ─────────────────────────────────── */

/*
 * What a real client holds, not what the server emitted: the envelope is
 * unwrapped to the text of its first content block. Checking the envelope
 * instead is how three tools shipped rendering blank while four tests passed.
 * Caller frees.
 */
static char *g4_client_text(const char *envelope, bool *out_is_error) {
    if (out_is_error) {
        *out_is_error = true;
    }
    if (!envelope) {
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(envelope, strlen(envelope), 0);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *flag = yyjson_obj_get(root, "isError");
    if (out_is_error) {
        *out_is_error = flag ? yyjson_get_bool(flag) : false;
    }
    yyjson_val *content = yyjson_obj_get(root, "content");
    yyjson_val *first = (content && yyjson_is_arr(content)) ? yyjson_arr_get(content, 0) : NULL;
    yyjson_val *text = first ? yyjson_obj_get(first, "text") : NULL;
    const char *str = (text && yyjson_is_str(text)) ? yyjson_get_str(text) : NULL;
    char *copy = str ? hyp_strdup(str) : NULL;
    yyjson_doc_free(doc);
    return copy;
}

/* ── The comment manifest, hand-built ───────────────────────────────────── */

/*
 * Two blocks, two authors, two fixed commit times — the shape
 * scripts/migrate-comments.py emits from git blame. Written by hand here
 * because this unit is about what happens to a record after it exists, and a
 * fixture repository would put git's availability between the test and its
 * claim.
 */
#define G4_BLOB_A "0123456789abcdef0123456789abcdef01234567"
#define G4_BLOB_B "89abcdef0123456789abcdef0123456789abcdef"
#define G4_BLAME_A "Ada Known <ada@known.invalid>"
#define G4_BLAME_B "Bo Later <bo@later.invalid>"
#define G4_BLAME_A_MS "1700000000000"
#define G4_BLAME_B_MS "1750000000000"

static void g4_field(FILE *fh, const char *name, const char *value) {
    (void)fprintf(fh, "%s %zu\n%s\n", name, strlen(value), value);
}

static bool g4_write_manifest(const char *path) {
    FILE *fh = fopen(path, "wb");
    if (!fh) {
        return false;
    }
    (void)fputs("hyp-comment-manifest-v1\n", fh);

    g4_field(fh, "path", "src/foundation/thing.c");
    g4_field(fh, "blob", G4_BLOB_A);
    g4_field(fh, "lines", "12-20");
    g4_field(fh, "author", G4_BLAME_A);
    g4_field(fh, "timestamp_ms", G4_BLAME_A_MS);
    g4_field(fh, "content",
             "The " G4_MARKER " table is pinned rather than derived, because a derived "
             "table that disagrees with the pinned one is indistinguishable from a "
             "correct table read at the wrong moment.");

    g4_field(fh, "path", "src/store/other.c");
    g4_field(fh, "blob", G4_BLOB_B);
    g4_field(fh, "lines", "3-9");
    g4_field(fh, "author", G4_BLAME_B);
    g4_field(fh, "timestamp_ms", G4_BLAME_B_MS);
    g4_field(fh, "content",
             "Absent and empty are answered differently here: absent means look "
             "elsewhere, empty means there is nothing, and only one of them is ever "
             "true.");

    (void)fclose(fh);
    return true;
}

enum { G4_MANIFEST_ITEMS = 2 };

/* ══════════════════════════════════════════════════════════════════════════
 *  1 · The same decision, written on two machines, is one record
 * ══════════════════════════════════════════════════════════════════════════ */

#ifndef _WIN32

typedef struct {
    const g4_machine_t *machine;
    const char *tz;
    bool capture_clock; /* the negative control's one difference */
} g4_write_ctx_t;

/*
 * One machine writing one decision. The timestamp is the caller's constant
 * unless the control asked for a captured one, and that single line is the
 * whole difference between a union and a duplicate storm.
 */
static void g4_write_decision(const void *vctx) {
    const g4_write_ctx_t *ctx = (const g4_write_ctx_t *)vctx;
    if (!g4_become(ctx->machine)) {
        _exit(11);
    }
    if (ctx->tz && hyp_setenv("TZ", ctx->tz, 1) != 0) {
        _exit(12);
    }

    int64_t clock_reading = hyp_record_wall_clock_ms();

    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_DECISION;
    in.author = G4_AUTHOR;
    in.timestamp_ms = ctx->capture_clock ? clock_reading : G4_FIXED_MS;
    in.content = G4_DECISION;

    const hyp_record_t *rec = NULL;
    if (hyp_record_build(&in, &rec) != HYP_RECORD_OK || !rec) {
        _exit(13);
    }

    hyp_record_store_t *store = NULL;
    if (hyp_record_store_open(ctx->machine->memory, &store) != HYP_RECORD_STORE_OK) {
        _exit(14);
    }
    if (hyp_record_store_append(store, rec, NULL) != HYP_RECORD_STORE_OK) {
        _exit(15);
    }
    hyp_record_store_close(store);

    /* The clock reading goes to disk so the parent can prove the two children
     * really did read different instants. A control whose premise is untested
     * is not a control. */
    char path[G4_PATH];
    char text[64];
    (void)snprintf(path, sizeof(path), "%s/clock", ctx->machine->root);
    (void)snprintf(text, sizeof(text), "%lld", (long long)clock_reading);
    if (th_write_file(path, text) != 0) {
        _exit(16);
    }
    hyp_record_free(rec);
}

TEST(two_machines_writing_one_decision_mint_one_id_and_already_agree) {
    const char *tmp = th_mktempdir("hyp_g4_same");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    g4_machine_t alpha;
    g4_machine_t beta;
    ASSERT_TRUE(g4_machine_make(&alpha, base, "alpha"));
    ASSERT_TRUE(g4_machine_make(&beta, base, "beta"));

    /* Two processes, two roots, two caches, two timezone settings, and two
     * instants — everything a second machine differs in that this venue can
     * construct. */
    g4_write_ctx_t first = {.machine = &alpha, .tz = "UTC", .capture_clock = false};
    ASSERT_EQ(g4_child(g4_write_decision, &first), 0);
    g4_advance_clock(3);
    g4_write_ctx_t second = {.machine = &beta, .tz = "Pacific/Kiritimati", .capture_clock = false};
    ASSERT_EQ(g4_child(g4_write_decision, &second), 0);

    /* The premise: the two children genuinely read different clocks. */
    char clock_a[64];
    char clock_b[64];
    char path[G4_PATH];
    (void)snprintf(path, sizeof(path), "%s/clock", alpha.root);
    ASSERT_TRUE(g4_read_file(path, clock_a, sizeof(clock_a)));
    (void)snprintf(path, sizeof(path), "%s/clock", beta.root);
    ASSERT_TRUE(g4_read_file(path, clock_b, sizeof(clock_b)));
    ASSERT_STR_NEQ(clock_a, clock_b);

    /* The claim: the id did not move with the clock, the timezone, the
     * process, the cache or the filesystem root. */
    char id_a[HYP_RECORD_ID_LEN + 1];
    char id_b[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(g4_only_id(alpha.memory, id_a));
    ASSERT_TRUE(g4_only_id(beta.memory, id_b));
    ASSERT_STR_EQ(id_a, id_b);

    /* And therefore the two stores agree BEFORE any exchange: the digest
     * compare short-circuits, nothing is loaded and nothing moves. Two
     * machines that have never spoken are already in agreement. */
    char digest_a[HYP_RECORD_ID_LEN + 1];
    char digest_b[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(g4_digest(alpha.memory, digest_a));
    ASSERT_TRUE(g4_digest(beta.memory, digest_b));
    ASSERT_STR_EQ(digest_a, digest_b);

    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(alpha.memory, beta.memory, HYP_SYNC_BOTH, &result), HYP_SYNC_OK);
    ASSERT_TRUE(result.agreed_before);
    ASSERT_EQ(result.pulled, 0U);
    ASSERT_EQ(result.pushed, 0U);

    size_t count = 0U;
    ASSERT_TRUE(g4_count(alpha.memory, &count));
    ASSERT_EQ(count, 1U);
    ASSERT_TRUE(g4_count(beta.memory, &count));
    ASSERT_EQ(count, 1U);

    (void)th_rmtree(base);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  NEGATIVE CONTROL 1 · a captured timestamp
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * The same construction with one line changed: the writer reads a clock
 * instead of taking the caller's value. This is the failure I6 exists to
 * prevent, and it is not hypothetical — one shipped writing surface does
 * exactly this, deliberately, and the test below it states why that is
 * correct there and fatal here.
 */
TEST(negative_control_a_captured_timestamp_makes_two_machines_disagree_forever) {
    const char *tmp = th_mktempdir("hyp_g4_captured");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    g4_machine_t alpha;
    g4_machine_t beta;
    ASSERT_TRUE(g4_machine_make(&alpha, base, "alpha"));
    ASSERT_TRUE(g4_machine_make(&beta, base, "beta"));

    g4_write_ctx_t first = {.machine = &alpha, .tz = "UTC", .capture_clock = true};
    ASSERT_EQ(g4_child(g4_write_decision, &first), 0);
    g4_advance_clock(3);
    g4_write_ctx_t second = {.machine = &beta, .tz = "UTC", .capture_clock = true};
    ASSERT_EQ(g4_child(g4_write_decision, &second), 0);

    /* One decision, two ids. Nothing downstream can repair this: the id is the
     * digest of the record and records are never mutated. */
    char id_a[HYP_RECORD_ID_LEN + 1];
    char id_b[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(g4_only_id(alpha.memory, id_a));
    ASSERT_TRUE(g4_only_id(beta.memory, id_b));
    ASSERT_STR_NEQ(id_a, id_b);

    /* And the union faithfully keeps both, which is the union behaving
     * correctly on input that is already wrong — the duplicate storm. */
    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(alpha.memory, beta.memory, HYP_SYNC_BOTH, &result), HYP_SYNC_OK);
    ASSERT_FALSE(result.agreed_before);
    ASSERT_EQ(result.pulled, 1U);
    ASSERT_EQ(result.pushed, 1U);

    size_t count = 0U;
    ASSERT_TRUE(g4_count(alpha.memory, &count));
    ASSERT_EQ(count, 2U);
    ASSERT_TRUE(g4_count(beta.memory, &count));
    ASSERT_EQ(count, 2U);

    (void)th_rmtree(base);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  2 · MCP writer on one machine, MCP reader on the other
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const g4_machine_t *machine;
    const char *tool;
    const char *args;
    const char *out_path;
} g4_mcp_ctx_t;

/* One MCP call on one machine, its CLIENT-VISIBLE text written to disk. */
static void g4_mcp_call(const void *vctx) {
    const g4_mcp_ctx_t *ctx = (const g4_mcp_ctx_t *)vctx;
    if (!g4_become(ctx->machine)) {
        _exit(11);
    }
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    if (!srv) {
        _exit(12);
    }
    char *envelope = hyp_mcp_handle_tool(srv, ctx->tool, ctx->args);
    if (!envelope) {
        hyp_mcp_server_free(srv);
        _exit(13);
    }
    bool is_error = false;
    char *text = g4_client_text(envelope, &is_error);
    free(envelope);
    hyp_mcp_server_free(srv);
    if (!text) {
        _exit(14);
    }
    /* The error flag rides along so the parent asserts on the client's view of
     * it rather than inferring it from the prose. */
    char body[G4_TEXT];
    (void)snprintf(body, sizeof(body), "%s\n%s", is_error ? "error" : "ok", text);
    free(text);
    if (th_write_file(ctx->out_path, body) != 0) {
        _exit(15);
    }
}

/* Split what g4_mcp_call wrote: the flag line, then the answer. */
static bool g4_split_answer(const char *raw, bool *out_is_error, const char **out_body) {
    const char *nl = strchr(raw, '\n');
    if (!nl) {
        return false;
    }
    *out_is_error = strncmp(raw, "error", 5) == 0;
    *out_body = nl + 1;
    return true;
}

/* The one record in a JSON search answer, or NULL. */
static yyjson_val *g4_single_row(yyjson_doc *doc, uint64_t *out_matched) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *matched = yyjson_obj_get(root, "matched");
    *out_matched = matched ? yyjson_get_uint(matched) : 0U;
    yyjson_val *rows = yyjson_obj_get(root, "records");
    if (!rows || !yyjson_is_arr(rows) || yyjson_arr_size(rows) != 1U) {
        return NULL;
    }
    return yyjson_arr_get(rows, 0);
}

static const char *g4_row_str(yyjson_val *row, const char *key) {
    yyjson_val *v = row ? yyjson_obj_get(row, key) : NULL;
    return (v && yyjson_is_str(v)) ? yyjson_get_str(v) : NULL;
}

TEST(a_decision_written_through_the_mcp_writer_is_found_through_the_mcp_reader_elsewhere) {
    const char *tmp = th_mktempdir("hyp_g4_mcp");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    g4_machine_t alpha;
    g4_machine_t beta;
    ASSERT_TRUE(g4_machine_make(&alpha, base, "alpha"));
    ASSERT_TRUE(g4_machine_make(&beta, base, "beta"));

    char raw[G4_TEXT];
    bool is_error = true;
    const char *body = NULL;

    /* ── Agent A, machine alpha, MCP harness: write. ── */
    char wrote_path[G4_PATH];
    (void)snprintf(wrote_path, sizeof(wrote_path), "%s/wrote.json", base);
    g4_mcp_ctx_t write_ctx = {.machine = &alpha,
                              .tool = "record_memory",
                              .args = "{\"kind\":\"decision\",\"title\":\"" G4_MARKER
                                      " stays pinned\",\"body\":\"A derived table that "
                                      "disagrees with the pinned one reads exactly like a "
                                      "correct table consulted at the wrong moment.\"}",
                              .out_path = wrote_path};
    ASSERT_EQ(g4_child(g4_mcp_call, &write_ctx), 0);
    ASSERT_TRUE(g4_read_file(wrote_path, raw, sizeof(raw)));
    ASSERT_TRUE(g4_split_answer(raw, &is_error, &body));
    ASSERT_FALSE(is_error);

    yyjson_doc *wrote = yyjson_read(body, strlen(body), 0);
    ASSERT_NOT_NULL(wrote);
    yyjson_val *id_val = yyjson_obj_get(yyjson_doc_get_root(wrote), "id");
    ASSERT_TRUE(id_val && yyjson_is_str(id_val));
    char written_id[HYP_RECORD_ID_LEN + 1];
    (void)snprintf(written_id, sizeof(written_id), "%s", yyjson_get_str(id_val));
    yyjson_doc_free(wrote);
    ASSERT_TRUE(hyp_record_id_is_valid(written_id));

    /* ── Agent B, machine beta, before anything crosses. ──
     *
     * ABSENT, not empty. Beta has never recorded anything, so `records` must be
     * missing entirely rather than sent as an empty list: an empty list is the
     * claim "there is nothing", and the truth here is "there is nowhere here it
     * could be". An agent that cannot tell those apart believes it has checked.
     */
    char before_path[G4_PATH];
    (void)snprintf(before_path, sizeof(before_path), "%s/before.json", base);
    g4_mcp_ctx_t before_ctx = {.machine = &beta,
                               .tool = "search_memory",
                               .args = "{\"format\":\"json\",\"query\":\"" G4_MARKER "\"}",
                               .out_path = before_path};
    ASSERT_EQ(g4_child(g4_mcp_call, &before_ctx), 0);
    ASSERT_TRUE(g4_read_file(before_path, raw, sizeof(raw)));
    ASSERT_TRUE(g4_split_answer(raw, &is_error, &body));
    ASSERT_FALSE(is_error);
    yyjson_doc *before = yyjson_read(body, strlen(body), 0);
    ASSERT_NOT_NULL(before);
    ASSERT_NULL(yyjson_obj_get(yyjson_doc_get_root(before), "records"));
    ASSERT_NOT_NULL(yyjson_obj_get(yyjson_doc_get_root(before), "memory"));
    yyjson_doc_free(before);

    /* ── The crossing, through the CLI: a third harness moves the record. ── */
    ASSERT_EQ(g4_run_sync_push(&alpha, &beta), 0);

    /* ── Agent B, machine beta, MCP harness: read. ── */
    char after_path[G4_PATH];
    (void)snprintf(after_path, sizeof(after_path), "%s/after.json", base);
    g4_mcp_ctx_t after_ctx = {.machine = &beta,
                              .tool = "search_memory",
                              .args = "{\"format\":\"json\",\"query\":\"" G4_MARKER "\"}",
                              .out_path = after_path};
    ASSERT_EQ(g4_child(g4_mcp_call, &after_ctx), 0);
    ASSERT_TRUE(g4_read_file(after_path, raw, sizeof(raw)));
    ASSERT_TRUE(g4_split_answer(raw, &is_error, &body));
    ASSERT_FALSE(is_error);

    yyjson_doc *after = yyjson_read(body, strlen(body), 0);
    ASSERT_NOT_NULL(after);
    uint64_t matched = 0U;
    yyjson_val *row = g4_single_row(after, &matched);
    ASSERT_EQ(matched, 1U);
    ASSERT_NOT_NULL(row);
    /* The same record, by the name agent A was given — not merely "a record
     * turned up". */
    ASSERT_STR_EQ(g4_row_str(row, "id"), written_id);
    ASSERT_STR_EQ(g4_row_str(row, "kind"), "decision");
    ASSERT_NOT_NULL(strstr(g4_row_str(row, "content"), G4_MARKER));
    /* The author is the writing surface, never a caller-stated name. */
    ASSERT_STR_EQ(g4_row_str(row, "author"), "hyponoia-mcp");
    yyjson_doc_free(after);

    /* And the same record read through a fourth wire: the store's own query
     * surface, on beta, with no MCP server involved. */
    ASSERT_TRUE(g4_holds(beta.memory, written_id));

    (void)th_rmtree(base);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  3 · CLI migrator writes, MCP reader on the other machine finds it
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const g4_machine_t *machine;
    const char *manifest;
    const char *repo;      /* NULL exercises the cwd-derived default */
    const char *workspace; /* NULL is a workspace of one */
    const char *chdir_to;  /* NULL leaves the child where it started */
} g4_migrate_ctx_t;

static void g4_run_migrate(const void *vctx) {
    const g4_migrate_ctx_t *ctx = (const g4_migrate_ctx_t *)vctx;
    if (!g4_become(ctx->machine)) {
        _exit(11);
    }
    if (ctx->chdir_to && chdir(ctx->chdir_to) != 0) {
        _exit(12);
    }
    char *argv[10];
    int argc = 0;
    argv[argc++] = (char *)"--manifest";
    argv[argc++] = (char *)ctx->manifest;
    argv[argc++] = (char *)"--store";
    argv[argc++] = (char *)ctx->machine->memory;
    if (ctx->repo) {
        argv[argc++] = (char *)"--repo";
        argv[argc++] = (char *)ctx->repo;
    }
    if (ctx->workspace) {
        argv[argc++] = (char *)"--workspace";
        argv[argc++] = (char *)ctx->workspace;
    }
    _exit(hyp_cmd_migrate_comments(argc, argv));
}

TEST(a_comment_relocated_by_the_cli_migrator_is_found_by_the_mcp_reader_elsewhere) {
    const char *tmp = th_mktempdir("hyp_g4_cli");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    g4_machine_t alpha;
    g4_machine_t beta;
    ASSERT_TRUE(g4_machine_make(&alpha, base, "alpha"));
    ASSERT_TRUE(g4_machine_make(&beta, base, "beta"));

    char manifest[G4_PATH];
    (void)snprintf(manifest, sizeof(manifest), "%s/manifest", base);
    ASSERT_TRUE(g4_write_manifest(manifest));

    /* ── Machine alpha, CLI harness: relocate the prose. ── */
    g4_migrate_ctx_t migrate = {.machine = &alpha,
                                .manifest = manifest,
                                .repo = "backend",
                                .workspace = "acme",
                                .chdir_to = NULL};
    ASSERT_EQ(g4_child(g4_run_migrate, &migrate), 0);
    size_t count = 0U;
    ASSERT_TRUE(g4_count(alpha.memory, &count));
    ASSERT_EQ(count, (size_t)G4_MANIFEST_ITEMS);

    /* ── The crossing. ── */
    ASSERT_EQ(g4_run_sync_push(&alpha, &beta), 0);

    /* ── Machine beta, MCP harness: read it back. ── */
    char answer_path[G4_PATH];
    (void)snprintf(answer_path, sizeof(answer_path), "%s/answer.json", base);
    g4_mcp_ctx_t read_ctx = {.machine = &beta,
                             .tool = "search_memory",
                             .args = "{\"format\":\"json\",\"query\":\"" G4_MARKER "\"}",
                             .out_path = answer_path};
    ASSERT_EQ(g4_child(g4_mcp_call, &read_ctx), 0);

    char raw[G4_TEXT];
    bool is_error = true;
    const char *body = NULL;
    ASSERT_TRUE(g4_read_file(answer_path, raw, sizeof(raw)));
    ASSERT_TRUE(g4_split_answer(raw, &is_error, &body));
    ASSERT_FALSE(is_error);

    yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
    ASSERT_NOT_NULL(doc);
    uint64_t matched = 0U;
    yyjson_val *row = g4_single_row(doc, &matched);
    ASSERT_EQ(matched, 1U);
    ASSERT_NOT_NULL(row);
    ASSERT_NOT_NULL(strstr(g4_row_str(row, "content"), G4_MARKER));
    /* Attribution survived the crossing: the author is the person git blames
     * for the comment, not the migrator and not the reading machine. A run
     * that authored the corpus as itself would collapse six authors onto one
     * and mint fresh ids on every machine. */
    ASSERT_STR_EQ(g4_row_str(row, "author"), G4_BLAME_A);
    /* And the time is the commit's, so the reader on the far machine sees when
     * the reasoning was written rather than when it arrived. */
    ASSERT_STR_EQ(g4_row_str(row, "written_at"), "2023-11-14T22:13:20Z");
    yyjson_doc_free(doc);

    (void)th_rmtree(base);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  4 · The cross-harness gap this unit was built to find
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * CHARACTERIZATION, and it is a DEFECT rather than a property.
 *
 * The MCP reader refuses a status filter on the stated ground that no record
 * in the store carries an anchor, so "attached" and "orphaned" partition an
 * empty set. That ground is a claim about the STORE, and a second harness
 * writing into the same store falsifies it: the CLI comment migrator anchors
 * every record it writes. Both ends pass their own tests — the reader never
 * writes an anchor, the migrator never asks the reader anything — and the
 * combination tells an agent that anchored records do not exist while handing
 * it anchored records.
 *
 * Two things are pinned, keyed on the PROPERTY rather than on any sentence:
 * anchored records are present in the store, and the reader refuses anyway.
 *
 * Exit condition: when the anchor resolver is wired into the reader, this test
 * converts to asserting that status='attached' returns exactly the anchored
 * records, and the refusal disappears. It must not be deleted before then —
 * a silently removed characterization is how a known defect becomes an unknown
 * one.
 */
TEST(the_mcp_reader_refuses_a_status_filter_while_holding_records_it_says_cannot_exist) {
    const char *tmp = th_mktempdir("hyp_g4_status");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    g4_machine_t alpha;
    ASSERT_TRUE(g4_machine_make(&alpha, base, "alpha"));

    char manifest[G4_PATH];
    (void)snprintf(manifest, sizeof(manifest), "%s/manifest", base);
    ASSERT_TRUE(g4_write_manifest(manifest));

    g4_migrate_ctx_t migrate = {.machine = &alpha,
                                .manifest = manifest,
                                .repo = "backend",
                                .workspace = "acme",
                                .chdir_to = NULL};
    ASSERT_EQ(g4_child(g4_run_migrate, &migrate), 0);

    /* Ground truth, from the store the reader reads. */
    hyp_record_store_t *store = NULL;
    ASSERT_EQ(hyp_record_store_open(alpha.memory, &store), HYP_RECORD_STORE_OK);
    hyp_record_store_query_t anchored;
    memset(&anchored, 0, sizeof(anchored));
    anchored.anchored_only = true;
    hyp_record_set_t *set = NULL;
    ASSERT_EQ(hyp_record_store_query(store, &anchored, &set), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(set);
    ASSERT_EQ(hyp_record_set_count(set), (size_t)G4_MANIFEST_ITEMS);
    hyp_record_set_free(set);
    hyp_record_store_close(store);

    /* The client's view: the reader refuses, on a premise the line above
     * disproves. */
    char answer_path[G4_PATH];
    (void)snprintf(answer_path, sizeof(answer_path), "%s/status.json", base);
    g4_mcp_ctx_t status_ctx = {.machine = &alpha,
                               .tool = "search_memory",
                               .args = "{\"format\":\"json\",\"status\":\"attached\"}",
                               .out_path = answer_path};
    ASSERT_EQ(g4_child(g4_mcp_call, &status_ctx), 0);

    char raw[G4_TEXT];
    bool is_error = false;
    const char *body = NULL;
    ASSERT_TRUE(g4_read_file(answer_path, raw, sizeof(raw)));
    ASSERT_TRUE(g4_split_answer(raw, &is_error, &body));
    ASSERT_TRUE(is_error);

    /* The same reader, asked without the filter, DOES hand the anchored
     * records over — so the refusal is not the store being unreadable. */
    char all_path[G4_PATH];
    (void)snprintf(all_path, sizeof(all_path), "%s/all.json", base);
    g4_mcp_ctx_t all_ctx = {.machine = &alpha,
                            .tool = "search_memory",
                            .args = "{\"format\":\"json\"}",
                            .out_path = all_path};
    ASSERT_EQ(g4_child(g4_mcp_call, &all_ctx), 0);
    ASSERT_TRUE(g4_read_file(all_path, raw, sizeof(raw)));
    ASSERT_TRUE(g4_split_answer(raw, &is_error, &body));
    ASSERT_FALSE(is_error);
    yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *matched = yyjson_obj_get(yyjson_doc_get_root(doc), "matched");
    ASSERT_EQ((size_t)yyjson_get_uint(matched), (size_t)G4_MANIFEST_ITEMS);
    yyjson_doc_free(doc);

    (void)th_rmtree(base);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  4b · The shipped CLI reproduces the residual by default
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * The measurement two sections below is made with slugs this file derives. This
 * one makes it with the slug the SHIPPED command derives when nobody passes
 * --repo: the project name of the working directory. Two operators running the
 * same documented command over the same manifest, from two checkouts at
 * different paths, therefore produce two disjoint sets of records — and
 * nothing in the run says so, because both runs report success and identical
 * counts.
 */
TEST(the_cli_migrator_default_repo_slug_makes_two_checkouts_disagree) {
    const char *tmp = th_mktempdir("hyp_g4_default");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    g4_machine_t alpha;
    g4_machine_t beta;
    ASSERT_TRUE(g4_machine_make(&alpha, base, "alpha"));
    ASSERT_TRUE(g4_machine_make(&beta, base, "beta-elsewhere"));

    char checkout_a[G4_PATH];
    char checkout_b[G4_PATH];
    (void)snprintf(checkout_a, sizeof(checkout_a), "%s/alpha/backend", base);
    (void)snprintf(checkout_b, sizeof(checkout_b), "%s/beta-elsewhere/backend", base);
    ASSERT_EQ(th_mkdir_p(checkout_a), 0);
    ASSERT_EQ(th_mkdir_p(checkout_b), 0);

    char manifest[G4_PATH];
    (void)snprintf(manifest, sizeof(manifest), "%s/manifest", base);
    ASSERT_TRUE(g4_write_manifest(manifest));

    /* Same command, same manifest, same declared workspace; only the working
     * directory differs, which is the one thing two machines cannot help. */
    g4_migrate_ctx_t run_a = {.machine = &alpha,
                              .manifest = manifest,
                              .repo = NULL,
                              .workspace = "acme",
                              .chdir_to = checkout_a};
    g4_migrate_ctx_t run_b = {.machine = &beta,
                              .manifest = manifest,
                              .repo = NULL,
                              .workspace = "acme",
                              .chdir_to = checkout_b};
    ASSERT_EQ(g4_child(g4_run_migrate, &run_a), 0);
    ASSERT_EQ(g4_child(g4_run_migrate, &run_b), 0);

    size_t count = 0U;
    ASSERT_TRUE(g4_count(alpha.memory, &count));
    ASSERT_EQ(count, (size_t)G4_MANIFEST_ITEMS);
    ASSERT_TRUE(g4_count(beta.memory, &count));
    ASSERT_EQ(count, (size_t)G4_MANIFEST_ITEMS);

    char digest_a[HYP_RECORD_ID_LEN + 1];
    char digest_b[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(g4_digest(alpha.memory, digest_a));
    ASSERT_TRUE(g4_digest(beta.memory, digest_b));
    ASSERT_STR_NEQ(digest_a, digest_b);

    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(alpha.memory, beta.memory, HYP_SYNC_BOTH, &result), HYP_SYNC_OK);
    ASSERT_EQ(result.pulled, (size_t)G4_MANIFEST_ITEMS);
    ASSERT_EQ(result.pushed, (size_t)G4_MANIFEST_ITEMS);
    ASSERT_TRUE(g4_count(alpha.memory, &count));
    ASSERT_EQ(count, (size_t)(2 * G4_MANIFEST_ITEMS));

    /* Pinned, the same two runs agree before they ever speak. */
    g4_machine_t pinned_a;
    g4_machine_t pinned_b;
    ASSERT_TRUE(g4_machine_make(&pinned_a, base, "pinned-alpha"));
    ASSERT_TRUE(g4_machine_make(&pinned_b, base, "pinned-beta"));
    run_a.machine = &pinned_a;
    run_a.repo = "backend";
    run_b.machine = &pinned_b;
    run_b.repo = "backend";
    ASSERT_EQ(g4_child(g4_run_migrate, &run_a), 0);
    ASSERT_EQ(g4_child(g4_run_migrate, &run_b), 0);
    ASSERT_TRUE(g4_digest(pinned_a.memory, digest_a));
    ASSERT_TRUE(g4_digest(pinned_b.memory, digest_b));
    ASSERT_STR_EQ(digest_a, digest_b);

    (void)th_rmtree(base);
    PASS();
}

#endif /* _WIN32 */

/* ══════════════════════════════════════════════════════════════════════════
 *  5 · The workspace id across two checkout paths — the measured residual
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * A6 derives the workspace id from the SORTED member slugs and states the
 * residual in its own header: a member slug is a function of the repository's
 * canonical PATH, so two machines holding the same repositories at different
 * paths derive different slugs, hence a different workspace id, and its stated
 * remedy is naming the workspace in the TOML. This unit is where that cost is
 * measured rather than assumed, and the measurement finds the remedy is
 * PARTIAL: it repairs the workspace field of an address and cannot reach the
 * repo field, which is the member slug itself.
 */

static bool g4_make_checkout(const char *base, const char *machine, const char *name,
                             char *out_root, size_t out_sz) {
    if (snprintf(out_root, out_sz, "%s/%s/%s", base, machine, name) < 0) {
        return false;
    }
    return th_mkdir_p(out_root) == 0;
}

static bool g4_write_toml(const char *base, const char *machine, const char *ws_name) {
    char path[G4_PATH];
    char text[G4_TEXT];
    if (snprintf(path, sizeof(path), "%s/%s/%s", base, machine, HYP_WSR_TOML_NAME) < 0) {
        return false;
    }
    (void)snprintf(text, sizeof(text),
                   "%s%s%s"
                   "[[repos]]\n"
                   "path = \"backend\"\n"
                   "role = \"member\"\n"
                   "\n"
                   "[[repos]]\n"
                   "path = \"frontend\"\n"
                   "role = \"member\"\n",
                   ws_name ? "name = \"" : "", ws_name ? ws_name : "", ws_name ? "\"\n\n" : "");
    return th_write_file(path, text) == 0;
}

static hyp_wsr_err_t g4_resolve(const char *base, const char *machine, hyp_wsr_resolved_t *out,
                                char *err, size_t err_sz) {
    char start[G4_PATH];
    char toml[G4_PATH];
    (void)snprintf(start, sizeof(start), "%s/%s", base, machine);
    (void)snprintf(toml, sizeof(toml), "%s/%s/%s", base, machine, HYP_WSR_TOML_NAME);
    return hyp_wsr_resolve(start, toml, NULL, out, err, err_sz);
}

TEST(the_same_repositories_at_two_checkout_paths_derive_two_workspace_ids) {
    const char *tmp = th_mktempdir("hyp_g4_ws");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    char scratch[G4_PATH];
    ASSERT_TRUE(g4_make_checkout(base, "alpha", "backend", scratch, sizeof(scratch)));
    ASSERT_TRUE(g4_make_checkout(base, "alpha", "frontend", scratch, sizeof(scratch)));
    ASSERT_TRUE(g4_make_checkout(base, "beta-elsewhere", "backend", scratch, sizeof(scratch)));
    ASSERT_TRUE(g4_make_checkout(base, "beta-elsewhere", "frontend", scratch, sizeof(scratch)));

    /* ── Without a declared name: the residual, measured. ── */
    ASSERT_TRUE(g4_write_toml(base, "alpha", NULL));
    ASSERT_TRUE(g4_write_toml(base, "beta-elsewhere", NULL));

    hyp_wsr_resolved_t alpha;
    hyp_wsr_resolved_t beta;
    char err[512];
    ASSERT_EQ(g4_resolve(base, "alpha", &alpha, err, sizeof(err)), HYP_WSR_OK);
    ASSERT_EQ(g4_resolve(base, "beta-elsewhere", &beta, err, sizeof(err)), HYP_WSR_OK);
    ASSERT_EQ(alpha.member_count, 2);
    ASSERT_EQ(beta.member_count, 2);

    /* Members sort by slug on both sides, so this compares like with like. */
    ASSERT_STR_NEQ(alpha.members[0].slug, beta.members[0].slug);
    ASSERT_STR_NEQ(alpha.members[1].slug, beta.members[1].slug);
    ASSERT_STR_NEQ(alpha.id, beta.id);
    (void)fprintf(stderr,
                  "  [G4] no declared name: workspace id alpha='%s' beta='%s' — DIFFERENT\n",
                  alpha.id, beta.id);

    /* ── With the documented remedy: a declared name. ── */
    ASSERT_TRUE(g4_write_toml(base, "alpha", "acme"));
    ASSERT_TRUE(g4_write_toml(base, "beta-elsewhere", "acme"));
    ASSERT_EQ(g4_resolve(base, "alpha", &alpha, err, sizeof(err)), HYP_WSR_OK);
    ASSERT_EQ(g4_resolve(base, "beta-elsewhere", &beta, err, sizeof(err)), HYP_WSR_OK);
    ASSERT_STR_EQ(alpha.id, "acme");
    ASSERT_STR_EQ(beta.id, "acme");

    /*
     * THE FINDING. The remedy names the workspace and stops there. Every
     * member slug is still a function of its own path, and the slug is the
     * REPO field of an address — so two machines under one declared workspace
     * still mint two different addresses for the same file, and the anchor is
     * in the id preimage.
     */
    ASSERT_STR_NEQ(alpha.members[0].slug, beta.members[0].slug);
    (void)fprintf(stderr, "  [G4] declared name 'acme': repo slug alpha='%s' beta='%s' — STILL "
                          "DIFFERENT\n",
                  alpha.members[0].slug, beta.members[0].slug);

    hyp_addr_t addr_a;
    hyp_addr_t addr_b;
    const char *qn = "backend.src.foundation.thing";
    ASSERT_EQ(hyp_addr_init(&addr_a, alpha.id, alpha.members[0].slug, qn), HYP_ADDR_OK);
    ASSERT_EQ(hyp_addr_init(&addr_b, beta.id, beta.members[0].slug, qn), HYP_ADDR_OK);
    char text_a[HYP_ADDR_MAX + 1];
    char text_b[HYP_ADDR_MAX + 1];
    ASSERT_TRUE(hyp_addr_format(&addr_a, text_a, sizeof(text_a)));
    ASSERT_TRUE(hyp_addr_format(&addr_b, text_b, sizeof(text_b)));
    ASSERT_STR_NEQ(text_a, text_b);

    /* Pinning the repo slug — which only an operator can do, and which nothing
     * derives — is what closes it. Stated so the remedy is complete rather
     * than half-stated. */
    ASSERT_EQ(hyp_addr_init(&addr_b, beta.id, "backend", qn), HYP_ADDR_OK);
    ASSERT_EQ(hyp_addr_init(&addr_a, alpha.id, "backend", qn), HYP_ADDR_OK);
    ASSERT_TRUE(hyp_addr_format(&addr_a, text_a, sizeof(text_a)));
    ASSERT_TRUE(hyp_addr_format(&addr_b, text_b, sizeof(text_b)));
    ASSERT_STR_EQ(text_a, text_b);

    (void)th_rmtree(base);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  6 · What the residual costs at the record level
 * ══════════════════════════════════════════════════════════════════════════ */

/* Ingest one manifest under one repo slug into a fresh store. */
static bool g4_ingest(const char *dir, const char *manifest, const char *workspace,
                      const char *repo) {
    hyp_record_store_t *store = NULL;
    if (hyp_record_store_open(dir, &store) != HYP_RECORD_STORE_OK) {
        return false;
    }
    hyp_comment_migrate_opts_t opts = {.workspace = workspace, .repo = repo};
    hyp_comment_migrate_stats_t stats;
    char err[512];
    bool ok = hyp_comment_migrate_run(manifest, &opts, store, &stats, err, sizeof(err)) ==
              HYP_COMMENT_MIGRATE_OK;
    hyp_record_store_close(store);
    return ok;
}

TEST(two_checkout_paths_store_one_comment_twice_and_a_pinned_slug_stores_it_once) {
    const char *tmp = th_mktempdir("hyp_g4_cost");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    char manifest[G4_PATH];
    (void)snprintf(manifest, sizeof(manifest), "%s/manifest", base);
    ASSERT_TRUE(g4_write_manifest(manifest));

    /* The two slugs a real pair of machines would derive: the whole canonical
     * path, separators mapped, which is what the indexer and the CLI both
     * use. */
    char root_a[G4_PATH];
    char root_b[G4_PATH];
    ASSERT_TRUE(g4_make_checkout(base, "alpha", "backend", root_a, sizeof(root_a)));
    ASSERT_TRUE(g4_make_checkout(base, "beta-elsewhere", "backend", root_b, sizeof(root_b)));
    char *slug_a = hyp_project_name_from_path(root_a);
    char *slug_b = hyp_project_name_from_path(root_b);
    ASSERT_NOT_NULL(slug_a);
    ASSERT_NOT_NULL(slug_b);
    ASSERT_STR_NEQ(slug_a, slug_b);

    /* ── Unpinned: one comment, two records, on both machines, permanently. ── */
    char store_a[G4_PATH];
    char store_b[G4_PATH];
    (void)snprintf(store_a, sizeof(store_a), "%s/unpinned-a", base);
    (void)snprintf(store_b, sizeof(store_b), "%s/unpinned-b", base);
    ASSERT_TRUE(g4_ingest(store_a, manifest, "acme", slug_a));
    ASSERT_TRUE(g4_ingest(store_b, manifest, "acme", slug_b));

    char digest_a[HYP_RECORD_ID_LEN + 1];
    char digest_b[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(g4_digest(store_a, digest_a));
    ASSERT_TRUE(g4_digest(store_b, digest_b));
    ASSERT_STR_NEQ(digest_a, digest_b);

    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(store_a, store_b, HYP_SYNC_BOTH, &result), HYP_SYNC_OK);
    ASSERT_FALSE(result.agreed_before);
    size_t count = 0U;
    ASSERT_TRUE(g4_count(store_a, &count));
    ASSERT_EQ(count, (size_t)(2 * G4_MANIFEST_ITEMS));
    (void)fprintf(stderr,
                  "  [G4] unpinned repo slug: %d comments become %zu records after sync\n",
                  G4_MANIFEST_ITEMS, count);

    /* ── Pinned: the same two machines agree before they ever speak. ── */
    char pinned_a[G4_PATH];
    char pinned_b[G4_PATH];
    (void)snprintf(pinned_a, sizeof(pinned_a), "%s/pinned-a", base);
    (void)snprintf(pinned_b, sizeof(pinned_b), "%s/pinned-b", base);
    ASSERT_TRUE(g4_ingest(pinned_a, manifest, "acme", "backend"));
    ASSERT_TRUE(g4_ingest(pinned_b, manifest, "acme", "backend"));
    ASSERT_TRUE(g4_digest(pinned_a, digest_a));
    ASSERT_TRUE(g4_digest(pinned_b, digest_b));
    ASSERT_STR_EQ(digest_a, digest_b);

    ASSERT_EQ(hyp_sync_dirs(pinned_a, pinned_b, HYP_SYNC_BOTH, &result), HYP_SYNC_OK);
    ASSERT_TRUE(result.agreed_before);
    ASSERT_TRUE(g4_count(pinned_a, &count));
    ASSERT_EQ(count, (size_t)G4_MANIFEST_ITEMS);

    free(slug_a);
    free(slug_b);
    (void)th_rmtree(base);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════════════
 *  NEGATIVE CONTROL 2 · a union that rewrites ids
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * The break: a merge that mints a new id for every record it accepts, keyed on
 * the destination store and on how many records that store already held. That
 * is what a machine-local sequence number or an insertion offset does when it
 * reaches the id, and it is the specific shape the store's WITHOUT ROWID
 * schema exists to make unavailable.
 *
 * Both properties break together, and both are asserted: the merge stops being
 * idempotent, and the cross-machine find fails BY ID — the record agent A
 * wrote is not in agent B's store under the name agent A holds. The production
 * exchange is then run on a fresh peer in the same test, so the control is
 * observed and restored in one place.
 */
static bool g4_merge_rewriting_ids(const char *dst_dir, const char *src_dir) {
    hyp_record_store_t *src = NULL;
    hyp_record_store_t *dst = NULL;
    if (hyp_record_store_open(src_dir, &src) != HYP_RECORD_STORE_OK) {
        return false;
    }
    if (hyp_record_store_open(dst_dir, &dst) != HYP_RECORD_STORE_OK) {
        hyp_record_store_close(src);
        return false;
    }
    hyp_record_set_t *set = NULL;
    bool ok = hyp_record_store_load(src, &set) == HYP_RECORD_STORE_OK && set != NULL;
    size_t held = 0U;
    ok = ok && hyp_record_store_count(dst, &held) == HYP_RECORD_STORE_OK;
    size_t total = ok ? hyp_record_set_count(set) : 0U;
    for (size_t i = 0U; ok && i < total; i++) {
        const hyp_record_t *rec = hyp_record_set_at(set, i);
        char origin[HYP_RECORD_MAX_ORIGIN + 1];
        (void)snprintf(origin, sizeof(origin), "%s|%s|%zu", rec->origin ? rec->origin : "none",
                       dst_dir, held + i);
        hyp_record_input_t in;
        memset(&in, 0, sizeof(in));
        in.kind = rec->kind;
        in.author = rec->author;
        in.timestamp_ms = rec->timestamp_ms;
        in.content = rec->content;
        in.anchor = rec->anchor;
        in.origin = origin;
        in.thread = rec->thread;
        in.parent = rec->parent;
        in.redactions = rec->redactions;
        const hyp_record_t *minted = NULL;
        ok = hyp_record_build(&in, &minted) == HYP_RECORD_OK && minted != NULL &&
             hyp_record_store_append(dst, minted, NULL) == HYP_RECORD_STORE_OK;
        hyp_record_free(minted);
    }
    hyp_record_set_free(set);
    hyp_record_store_close(dst);
    hyp_record_store_close(src);
    return ok;
}

/* The id of the record carrying the marker, which is what an agent on the far
 * machine is looking for. */
static bool g4_marker_id(const char *dir, char out[HYP_RECORD_ID_LEN + 1]) {
    out[0] = '\0';
    hyp_record_store_t *store = NULL;
    if (hyp_record_store_open(dir, &store) != HYP_RECORD_STORE_OK) {
        return false;
    }
    hyp_record_set_t *set = NULL;
    bool ok = hyp_record_store_load(store, &set) == HYP_RECORD_STORE_OK && set != NULL;
    size_t total = ok ? hyp_record_set_count(set) : 0U;
    for (size_t i = 0U; i < total; i++) {
        const hyp_record_t *rec = hyp_record_set_at(set, i);
        if (rec && rec->content && strstr(rec->content, G4_MARKER)) {
            (void)snprintf(out, HYP_RECORD_ID_LEN + 1, "%s", rec->id);
            break;
        }
    }
    hyp_record_set_free(set);
    hyp_record_store_close(store);
    return out[0] != '\0';
}

TEST(negative_control_a_union_that_rewrites_ids_loses_the_cross_machine_find) {
    const char *tmp = th_mktempdir("hyp_g4_break");
    ASSERT_NOT_NULL(tmp);
    char base[G4_PATH];
    (void)snprintf(base, sizeof(base), "%s", tmp);

    char manifest[G4_PATH];
    (void)snprintf(manifest, sizeof(manifest), "%s/manifest", base);
    ASSERT_TRUE(g4_write_manifest(manifest));

    char alpha[G4_PATH];
    char broken[G4_PATH];
    char intact[G4_PATH];
    (void)snprintf(alpha, sizeof(alpha), "%s/alpha", base);
    (void)snprintf(broken, sizeof(broken), "%s/beta-broken", base);
    (void)snprintf(intact, sizeof(intact), "%s/beta-intact", base);
    ASSERT_TRUE(g4_ingest(alpha, manifest, "acme", "backend"));

    /* What agent A knows the decision by. */
    char wanted[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(g4_marker_id(alpha, wanted));

    /* ── The break, run once: the find fails. ── */
    ASSERT_TRUE(g4_merge_rewriting_ids(broken, alpha));
    size_t count = 0U;
    ASSERT_TRUE(g4_count(broken, &count));
    ASSERT_EQ(count, (size_t)G4_MANIFEST_ITEMS);
    /* The records are all there; not one of them answers to its own name. */
    ASSERT_FALSE(g4_holds(broken, wanted));
    char digest_broken[HYP_RECORD_ID_LEN + 1];
    char digest_alpha[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(g4_digest(alpha, digest_alpha));
    ASSERT_TRUE(g4_digest(broken, digest_broken));
    ASSERT_STR_NEQ(digest_alpha, digest_broken);

    /* ── The break, run twice: not idempotent either. ── */
    ASSERT_TRUE(g4_merge_rewriting_ids(broken, alpha));
    ASSERT_TRUE(g4_count(broken, &count));
    ASSERT_EQ(count, (size_t)(2 * G4_MANIFEST_ITEMS));

    /* ── Restored: the production exchange, same corpus, fresh peer. ── */
    hyp_sync_result_t result;
    ASSERT_EQ(hyp_sync_dirs(intact, alpha, HYP_SYNC_PULL, &result), HYP_SYNC_OK);
    ASSERT_EQ(result.pulled, (size_t)G4_MANIFEST_ITEMS);
    ASSERT_TRUE(g4_holds(intact, wanted));
    ASSERT_EQ(hyp_sync_dirs(intact, alpha, HYP_SYNC_PULL, &result), HYP_SYNC_OK);
    ASSERT_TRUE(result.agreed_before);
    ASSERT_TRUE(g4_count(intact, &count));
    ASSERT_EQ(count, (size_t)G4_MANIFEST_ITEMS);
    char digest_intact[HYP_RECORD_ID_LEN + 1];
    ASSERT_TRUE(g4_digest(intact, digest_intact));
    ASSERT_STR_EQ(digest_intact, digest_alpha);

    (void)th_rmtree(base);
    PASS();
}

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(g4_cross) {
#ifndef _WIN32
    /* cross-machine: one decision, one id */
    RUN_TEST(two_machines_writing_one_decision_mint_one_id_and_already_agree);
    RUN_TEST(negative_control_a_captured_timestamp_makes_two_machines_disagree_forever);
    /* cross-harness, across the machine boundary */
    RUN_TEST(a_decision_written_through_the_mcp_writer_is_found_through_the_mcp_reader_elsewhere);
    RUN_TEST(a_comment_relocated_by_the_cli_migrator_is_found_by_the_mcp_reader_elsewhere);
    RUN_TEST(the_mcp_reader_refuses_a_status_filter_while_holding_records_it_says_cannot_exist);
    RUN_TEST(the_cli_migrator_default_repo_slug_makes_two_checkouts_disagree);
#endif
    /* the workspace id, and what its residual costs */
    RUN_TEST(the_same_repositories_at_two_checkout_paths_derive_two_workspace_ids);
    RUN_TEST(two_checkout_paths_store_one_comment_twice_and_a_pinned_slug_stores_it_once);
    RUN_TEST(negative_control_a_union_that_rewrites_ids_loses_the_cross_machine_find);
}
