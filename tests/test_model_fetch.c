/*
 * test_model_fetch.c — the fetch-on-first-use path for the `ask` lane's
 * embedding weights (NEXT-STEPS.md §2.1, runs/ASK/T7-model-fetch.json).
 *
 * Two things are being tested here and they are not the same thing.
 *
 * The first is the DIGEST GATE. A verification that has never been seen to
 * reject anything is a claim, not a gate, so the rejections are tested
 * directly and in more ways than the acceptance: one flipped bit, a truncated
 * file, a file that cannot be read, and a full-size download whose contents
 * are wrong. Each must refuse, and the refusing ones must leave NOTHING at the
 * name a later run would trust.
 *
 * The second is the FAILURE PATHS, which is where a downloader actually lives.
 * No network, DNS failure, an HTTP error on the pinned revision, a full disk,
 * an interruption, a server that will not resume — all six are driven through
 * the transfer seam rather than hoped about, because §0 is explicit that this
 * is the class of defect that wastes an afternoon.
 *
 * The tests never touch the network: HYP_CACHE_DIR points at a temp directory
 * and the byte-moving step is replaced. The one thing they cannot fake is the
 * success path (nothing but the real 639 MB hashes to the pinned digest); that
 * leg was run for real and is recorded in T7-model-fetch.json.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <cli/model_fetch.h>
#include <foundation/compat.h>
#include <foundation/compat_fs.h>
#include <foundation/platform.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

enum { MF_MB = 1024 * 1024, MF_SMALL = 4096, MF_URL_MAX = 1024, MF_HEX = 64 };

/* Digests produced by sha256sum(1), not by the code under test. Hashing that
 * only agrees with itself is not verification. */
#define MF_SHA_EMPTY "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define MF_SHA_HYPONOIA "aa0df22000a6c2e0dc436acf6dfd79be4b2db83c46fd54deeb825bf04a8dd877"
#define MF_SHA_1MIB_ZEROS "30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58"

/* ── Fixture ─────────────────────────────────────────────────────── */

static char g_cache[512];

static void mf_begin(void) {
    char *dir = th_mktempdir("hyp_model_fetch");
    if (dir) {
        snprintf(g_cache, sizeof(g_cache), "%s", dir);
        hyp_setenv("HYP_CACHE_DIR", g_cache, 1);
    } else {
        g_cache[0] = '\0';
    }
}

static void mf_end(void) {
    hyp_unsetenv("HYP_CACHE_DIR");
    if (g_cache[0]) {
        th_rmtree(g_cache);
        g_cache[0] = '\0';
    }
}

/* Write `n` bytes of `fill` at the front of a NEW file. */
static bool mf_write_file(const char *path, int64_t n, unsigned char fill) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        hyp_mkdir_p(dir, 0700);
    }
    FILE *fp = hyp_fopen(path, "wb");
    if (!fp) {
        return false;
    }
    unsigned char chunk[MF_SMALL];
    memset(chunk, fill, sizeof(chunk));
    int64_t left = n;
    while (left > 0) {
        size_t want = left < (int64_t)sizeof(chunk) ? (size_t)left : sizeof(chunk);
        if (fwrite(chunk, 1, want, fp) != want) {
            (void)fclose(fp);
            return false;
        }
        left -= (int64_t)want;
    }
    return fclose(fp) == 0;
}

/* Make `path` exactly `n` bytes, sparsely. 639 MB of holes costs no disk and
 * is the only way a test can afford to exercise the exact-size branch. */
static bool mf_set_size(const char *path, int64_t n) {
#ifndef _WIN32
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        hyp_mkdir_p(dir, 0700);
    }
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        return false;
    }
    int rc = ftruncate(fd, (off_t)n);
    (void)close(fd);
    return rc == 0;
#else
    (void)path;
    (void)n;
    return false;
#endif
}

/* ── The transfer seam ───────────────────────────────────────────── */

typedef struct {
    int calls;
    int exit_code;       /* what "curl" returns */
    int exit_code_2;     /* what it returns on the second call, if >= 0 */
    int64_t total_bytes; /* size to leave in dest; < 0 leaves it untouched */
    unsigned char fill;
    int64_t last_resume_from;
    char last_url[MF_URL_MAX];
    char last_dest[MF_URL_MAX];
} mf_transfer_state_t;

static mf_transfer_state_t g_transfer;

static int mf_transfer(const char *url, const char *dest, int64_t resume_from, void *ud) {
    (void)ud;
    g_transfer.calls++;
    g_transfer.last_resume_from = resume_from;
    snprintf(g_transfer.last_url, sizeof(g_transfer.last_url), "%s", url ? url : "");
    snprintf(g_transfer.last_dest, sizeof(g_transfer.last_dest), "%s", dest ? dest : "");
    if (g_transfer.total_bytes >= 0 && dest) {
        if (g_transfer.fill == 0) {
            (void)mf_set_size(dest, g_transfer.total_bytes);
        } else {
            (void)mf_write_file(dest, g_transfer.total_bytes, g_transfer.fill);
        }
    }
    if (g_transfer.calls > 1 && g_transfer.exit_code_2 >= 0) {
        return g_transfer.exit_code_2;
    }
    return g_transfer.exit_code;
}

static void mf_arm(int exit_code, int64_t total_bytes, unsigned char fill) {
    memset(&g_transfer, 0, sizeof(g_transfer));
    g_transfer.exit_code = exit_code;
    g_transfer.exit_code_2 = -1;
    g_transfer.total_bytes = total_bytes;
    g_transfer.fill = fill;
    hyp_model_set_transfer_for_test(mf_transfer, NULL);
}

static void mf_disarm(void) {
    hyp_model_set_transfer_for_test(NULL, NULL);
}

static hyp_model_fetch_opts_t mf_opts_yes(void) {
    hyp_model_fetch_opts_t opts = {0};
    opts.assume_yes = true;
    opts.quiet = true;
    return opts;
}

/* ── The pin ─────────────────────────────────────────────────────── */

/* "Fetch the latest" is how a verified download becomes an unverified one.
 * The URL must name one immutable revision and the digest must look like a
 * digest — both are the sort of thing a refactor silently loosens. */
TEST(model_pin_names_one_immutable_revision) {
    const char *url = HYP_MODEL_ASK_URL;
    ASSERT_NOT_NULL(strstr(url, "https://"));
    ASSERT_NOT_NULL(strstr(url, HYP_MODEL_ASK_REVISION));
    ASSERT_NOT_NULL(strstr(url, HYP_MODEL_ASK_FILE));
    ASSERT_NULL(strstr(url, "/main/"));
    ASSERT_NULL(strstr(url, "latest"));

    ASSERT_EQ(strlen(HYP_MODEL_ASK_SHA256), MF_HEX);
    for (size_t i = 0; i < strlen(HYP_MODEL_ASK_SHA256); i++) {
        char c = HYP_MODEL_ASK_SHA256[i];
        ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    ASSERT_EQ(strlen(HYP_MODEL_ASK_REVISION), 40);
    ASSERT_EQ((long long)HYP_MODEL_ASK_BYTES, 639150592LL);
    PASS();
}

/* The hasher is an oracle only if it agrees with one. */
TEST(model_sha256_agrees_with_the_reference_tool) {
    mf_begin();
    char hex[HYP_MODEL_HEX_SIZE];

    ASSERT(mf_write_file(TH_PATH(g_cache, "empty"), 0, 0));
    ASSERT_EQ(hyp_model_sha256_file(TH_PATH(g_cache, "empty"), hex, sizeof(hex)), 0);
    ASSERT_STR_EQ(hex, MF_SHA_EMPTY);

    ASSERT_EQ(th_write_file(TH_PATH(g_cache, "word"), "hyponoia"), 0);
    ASSERT_EQ(hyp_model_sha256_file(TH_PATH(g_cache, "word"), hex, sizeof(hex)), 0);
    ASSERT_STR_EQ(hex, MF_SHA_HYPONOIA);

    /* Crosses the 64 KiB read chunk sixteen times: an off-by-one in the
     * streaming loop would show up here and nowhere else. */
    ASSERT(mf_write_file(TH_PATH(g_cache, "zeros"), MF_MB, 0));
    ASSERT_EQ(hyp_model_sha256_file(TH_PATH(g_cache, "zeros"), hex, sizeof(hex)), 0);
    ASSERT_STR_EQ(hex, MF_SHA_1MIB_ZEROS);

    mf_end();
    PASS();
}

/* ── Where it lands ──────────────────────────────────────────────── */

TEST(model_path_follows_the_cache_convention) {
    mf_begin();
    char dir[HYP_MODEL_PATH_MAX];
    char path[HYP_MODEL_PATH_MAX];
    char expect[HYP_MODEL_PATH_MAX];

    ASSERT_NOT_NULL(hyp_model_ask_dir(dir, sizeof(dir)));
    snprintf(expect, sizeof(expect), "%s/models", g_cache);
    ASSERT_STR_EQ(dir, expect);

    ASSERT_NOT_NULL(hyp_model_ask_path(path, sizeof(path)));
    snprintf(expect, sizeof(expect), "%s/models/%s", g_cache, HYP_MODEL_ASK_FILE);
    ASSERT_STR_EQ(path, expect);
    mf_end();
    PASS();
}

TEST(model_probe_reports_absent_partial_present_and_wrong_size) {
    mf_begin();
    hyp_model_probe_t probe;

    hyp_model_ask_probe(&probe);
    ASSERT_EQ(probe.state, HYP_MODEL_ABSENT);
    ASSERT_EQ(probe.bytes, -1);
    ASSERT_EQ(probe.part_bytes, 0);

    ASSERT(mf_write_file(probe.part_path, MF_MB, 'z'));
    hyp_model_ask_probe(&probe);
    ASSERT_EQ(probe.state, HYP_MODEL_PARTIAL);
    ASSERT_EQ(probe.part_bytes, MF_MB);

    /* A file at the good name that is the wrong length is NOT present. */
    ASSERT(mf_write_file(probe.path, MF_MB, 'z'));
    hyp_model_ask_probe(&probe);
    ASSERT_EQ(probe.state, HYP_MODEL_WRONG_SIZE);
    ASSERT_FALSE(hyp_model_ask_present());

#ifndef _WIN32
    ASSERT(mf_set_size(probe.path, HYP_MODEL_ASK_BYTES));
    hyp_model_ask_probe(&probe);
    ASSERT_EQ(probe.state, HYP_MODEL_PRESENT);
    ASSERT_EQ(probe.bytes, (long long)HYP_MODEL_ASK_BYTES);
    ASSERT_TRUE(hyp_model_ask_present());
#endif
    mf_end();
    PASS();
}

/* Nowhere to put it is its own answer, not "absent" — the remedy differs. */
TEST(model_probe_reports_no_cache_dir_separately) {
    const char *home_saved = hyp_get_home_dir();
    char home_copy[512] = {0};
    if (home_saved) {
        snprintf(home_copy, sizeof(home_copy), "%s", home_saved);
    }
    hyp_unsetenv("HYP_CACHE_DIR");
    hyp_unsetenv("HOME");
#ifdef _WIN32
    hyp_unsetenv("USERPROFILE");
#endif
    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);

    char detail[HYP_MODEL_MSG_MAX];
    char remedy[HYP_MODEL_MSG_MAX];
    hyp_model_unavailable_text("proj", detail, sizeof(detail), remedy, sizeof(remedy));

    /* Restore the environment BEFORE asserting: a failed assertion returns
     * immediately, and leaving HOME unset would poison every later suite. */
    if (home_copy[0]) {
        hyp_setenv("HOME", home_copy, 1);
    }

    ASSERT_EQ(probe.state, HYP_MODEL_NO_CACHE_DIR);
    ASSERT_STR_EQ(probe.path, "");
    ASSERT_NOT_NULL(strstr(remedy, "HYP_CACHE_DIR"));
    PASS();
}

/* ── The digest gate ─────────────────────────────────────────────── */

TEST(model_verify_accepts_exactly_the_right_bytes) {
    mf_begin();
    char err[HYP_MODEL_MSG_MAX];
    ASSERT_EQ(th_write_file(TH_PATH(g_cache, "good"), "hyponoia"), 0);
    ASSERT_EQ(
        hyp_model_verify_file(TH_PATH(g_cache, "good"), MF_SHA_HYPONOIA, true, err, sizeof(err)),
        0);
    ASSERT_STR_EQ(err, "");
    ASSERT_TRUE(hyp_file_exists(TH_PATH(g_cache, "good")));
    mf_end();
    PASS();
}

/* THE test. One bit of one byte, and the file must be refused AND removed —
 * because the failure this exists to prevent is a corrupt blob sitting at the
 * good name until a later run loads it without asking again. */
TEST(model_verify_rejects_one_flipped_bit_and_deletes_the_file) {
    mf_begin();
    char err[HYP_MODEL_MSG_MAX];
    const char *path = TH_PATH(g_cache, "flipped");
    /* "hyponoia" with the last byte one bit different. */
    ASSERT_EQ(th_write_file(path, "hyponoib"), 0);
    ASSERT_NEQ(hyp_model_verify_file(path, MF_SHA_HYPONOIA, true, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, "MISMATCH"));
    ASSERT_NOT_NULL(strstr(err, MF_SHA_HYPONOIA));
    ASSERT_FALSE(hyp_file_exists(path));
    mf_end();
    PASS();
}

TEST(model_verify_rejects_a_truncated_file) {
    mf_begin();
    char err[HYP_MODEL_MSG_MAX];
    const char *path = TH_PATH(g_cache, "short");
    ASSERT_EQ(th_write_file(path, "hypono"), 0);
    ASSERT_NEQ(hyp_model_verify_file(path, MF_SHA_HYPONOIA, true, err, sizeof(err)), 0);
    ASSERT_FALSE(hyp_file_exists(path));
    mf_end();
    PASS();
}

/* An empty file hashes to a perfectly valid digest. It must not be mistaken
 * for the artifact just because hashing succeeded. */
TEST(model_verify_rejects_an_empty_file) {
    mf_begin();
    char err[HYP_MODEL_MSG_MAX];
    const char *path = TH_PATH(g_cache, "empty");
    ASSERT(mf_write_file(path, 0, 0));
    ASSERT_NEQ(hyp_model_verify_file(path, HYP_MODEL_ASK_SHA256, false, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, MF_SHA_EMPTY));
    mf_end();
    PASS();
}

/* `--verify` answers a question; it does not delete 639 MB the user may want
 * to look at. The fetch path deletes because it owns the bytes it just wrote. */
TEST(model_verify_keeps_the_file_when_told_not_to_delete) {
    mf_begin();
    char err[HYP_MODEL_MSG_MAX];
    const char *path = TH_PATH(g_cache, "kept");
    ASSERT_EQ(th_write_file(path, "hyponoib"), 0);
    ASSERT_NEQ(hyp_model_verify_file(path, MF_SHA_HYPONOIA, false, err, sizeof(err)), 0);
    ASSERT_TRUE(hyp_file_exists(path));
    mf_end();
    PASS();
}

/* A file that cannot be read has no digest, so it cannot have a matching one. */
TEST(model_verify_refuses_what_it_cannot_read) {
    mf_begin();
    char err[HYP_MODEL_MSG_MAX];
    ASSERT_NEQ(
        hyp_model_verify_file(TH_PATH(g_cache, "nope"), MF_SHA_HYPONOIA, true, err, sizeof(err)),
        0);
    ASSERT_NOT_NULL(strstr(err, "cannot be verified"));
    ASSERT_NEQ(hyp_model_verify_file(NULL, MF_SHA_HYPONOIA, false, err, sizeof(err)), 0);
    mf_end();
    PASS();
}

/* ── The unavailable message ─────────────────────────────────────── */

TEST(model_unavailable_says_what_is_missing_how_big_and_what_to_type) {
    mf_begin();
    char detail[HYP_MODEL_MSG_MAX];
    char remedy[HYP_MODEL_MSG_MAX];
    hyp_model_unavailable_text("lld-elf", detail, sizeof(detail), remedy, sizeof(remedy));

    ASSERT_NOT_NULL(strstr(detail, HYP_MODEL_ASK_MODEL));
    ASSERT_NOT_NULL(strstr(detail, HYP_MODEL_ASK_SIZE_TEXT));
    ASSERT_NOT_NULL(strstr(detail, "lld-elf"));
    /* The sentence that stops an agent reading "unavailable" as "no results". */
    ASSERT_NOT_NULL(strstr(detail, "NOTHING was searched"));

    ASSERT_NOT_NULL(strstr(remedy, HYP_MODEL_ASK_COMMAND));
    ASSERT_NOT_NULL(strstr(remedy, HYP_MODEL_ASK_SIZE_TEXT));
    ASSERT_NOT_NULL(strstr(remedy, "SHA-256"));
    ASSERT_NOT_NULL(strstr(remedy, g_cache));        /* where it will live */
    ASSERT_NOT_NULL(strstr(remedy, "rm "));          /* and how to get rid of it */
    ASSERT_NOT_NULL(strstr(remedy, "search_graph")); /* what still works meanwhile */
    mf_end();
    PASS();
}

TEST(model_unavailable_offers_a_resume_when_a_partial_exists) {
    mf_begin();
    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT(mf_write_file(probe.part_path, 3 * MF_MB, 'q'));

    char detail[HYP_MODEL_MSG_MAX];
    char remedy[HYP_MODEL_MSG_MAX];
    hyp_model_unavailable_text("lld-elf", detail, sizeof(detail), remedy, sizeof(remedy));
    ASSERT_NOT_NULL(strstr(detail, "partly downloaded"));
    ASSERT_NOT_NULL(strstr(detail, "3 MB"));
    ASSERT_NOT_NULL(strstr(remedy, "RESUME"));
    mf_end();
    PASS();
}

#ifndef _WIN32
/* A caller must not be able to report "the model is missing" while it is not. */
TEST(model_unavailable_does_not_claim_absence_when_present) {
    mf_begin();
    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT(mf_set_size(probe.path, HYP_MODEL_ASK_BYTES));

    char detail[HYP_MODEL_MSG_MAX];
    char remedy[HYP_MODEL_MSG_MAX];
    hyp_model_unavailable_text("lld-elf", detail, sizeof(detail), remedy, sizeof(remedy));
    ASSERT_NOT_NULL(strstr(detail, "ARE present"));
    ASSERT_NOT_NULL(strstr(remedy, "Nothing to fetch"));
    mf_end();
    PASS();
}
#endif

/* Every outcome has to tell the user something they can act on. */
TEST(model_every_failure_says_what_to_do) {
    static const hyp_model_fetch_result_t all[] = {HYP_MODEL_FETCH_OK,
                                                   HYP_MODEL_FETCH_ALREADY_PRESENT,
                                                   HYP_MODEL_FETCH_DECLINED,
                                                   HYP_MODEL_FETCH_NO_CACHE_DIR,
                                                   HYP_MODEL_FETCH_MKDIR_FAILED,
                                                   HYP_MODEL_FETCH_NO_SPACE,
                                                   HYP_MODEL_FETCH_NO_DOWNLOADER,
                                                   HYP_MODEL_FETCH_DNS,
                                                   HYP_MODEL_FETCH_OFFLINE,
                                                   HYP_MODEL_FETCH_HTTP,
                                                   HYP_MODEL_FETCH_TLS,
                                                   HYP_MODEL_FETCH_DISK_FULL,
                                                   HYP_MODEL_FETCH_INTERRUPTED,
                                                   HYP_MODEL_FETCH_DIGEST_MISMATCH,
                                                   HYP_MODEL_FETCH_INSTALL_FAILED,
                                                   HYP_MODEL_FETCH_TRANSFER_FAILED};
    size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const char *text = hyp_model_fetch_result_text(all[i]);
        ASSERT_NOT_NULL(text);
        ASSERT_GT(strlen(text), 10);
        for (size_t j = 0; j < i; j++) {
            ASSERT_STR_NEQ(text, hyp_model_fetch_result_text(all[j]));
        }
    }
    PASS();
}

/* ── Consent ─────────────────────────────────────────────────────── */

#ifndef _WIN32
/* 639 MB must never be the default answer to a question. With no terminal to
 * ask and no --yes, the fetch refuses and the transfer is never reached. */
TEST(model_fetch_refuses_to_run_unattended_without_yes) {
    mf_begin();
    mf_arm(0, -1, 0);
    int saved = dup(0);
    int devnull = open("/dev/null", O_RDONLY);
    ASSERT_GTE(devnull, 0);
    (void)dup2(devnull, 0);

    hyp_model_fetch_opts_t opts = {0};
    opts.quiet = true;
    hyp_model_fetch_result_t result = hyp_model_fetch(&opts);

    (void)dup2(saved, 0);
    (void)close(saved);
    (void)close(devnull);

    ASSERT_EQ(result, HYP_MODEL_FETCH_DECLINED);
    ASSERT_EQ(g_transfer.calls, 0);
    mf_disarm();
    mf_end();
    PASS();
}
#endif

/* ── Failure paths ───────────────────────────────────────────────── */

TEST(model_fetch_translates_every_transport_failure) {
    struct {
        int curl_exit;
        hyp_model_fetch_result_t expect;
    } cases[] = {
        {6, HYP_MODEL_FETCH_DNS},             /* could not resolve host */
        {5, HYP_MODEL_FETCH_DNS},             /* could not resolve proxy */
        {7, HYP_MODEL_FETCH_OFFLINE},         /* could not connect */
        {28, HYP_MODEL_FETCH_OFFLINE},        /* timed out */
        {22, HYP_MODEL_FETCH_HTTP},           /* 404 on the pinned revision */
        {23, HYP_MODEL_FETCH_DISK_FULL},      /* write error */
        {35, HYP_MODEL_FETCH_TLS},            /* TLS connect */
        {60, HYP_MODEL_FETCH_TLS},            /* CA bundle */
        {127, HYP_MODEL_FETCH_NO_DOWNLOADER}, /* curl not on PATH */
        {-1, HYP_MODEL_FETCH_NO_DOWNLOADER},  /* spawn failed outright */
        {99, HYP_MODEL_FETCH_TRANSFER_FAILED} /* anything else */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mf_begin();
        mf_arm(cases[i].curl_exit, -1, 0);
        hyp_model_fetch_opts_t opts = mf_opts_yes();
        hyp_model_fetch_result_t result = hyp_model_fetch(&opts);
        ASSERT_EQ(result, cases[i].expect);
        ASSERT_EQ(g_transfer.calls, 1);
        /* Nothing was installed on any failure path. */
        ASSERT_FALSE(hyp_model_ask_present());
        mf_disarm();
        mf_end();
    }
    PASS();
}

/* hyp_exec_no_shell reports "could not spawn" and "died on a signal" with the
 * same -1. Telling someone whose download the OOM killer ate that curl is not
 * installed sends them to fix the wrong thing, so bytes on disk break the tie. */
TEST(model_fetch_does_not_blame_a_missing_curl_for_a_killed_one) {
    mf_begin();
    mf_arm(-1, 9 * MF_MB, 'k'); /* curl wrote 9 MB, then died on a signal */
    hyp_model_fetch_opts_t opts = mf_opts_yes();
    ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_INTERRUPTED);

    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT_EQ(probe.state, HYP_MODEL_PARTIAL);
    ASSERT_EQ(probe.part_bytes, 9 * MF_MB);
    mf_disarm();
    mf_end();
    PASS();
}

/* An interrupted fetch keeps its bytes and the next one continues from them.
 * This machine has restarted twice mid-work; interruption is the normal case,
 * not the exceptional one. */
TEST(model_fetch_keeps_the_partial_and_resumes_from_it) {
    mf_begin();
    mf_arm(0, 5 * MF_MB, 'a'); /* "curl" succeeds but the file is short */
    hyp_model_fetch_opts_t opts = mf_opts_yes();
    ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_INTERRUPTED);

    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT_EQ(probe.state, HYP_MODEL_PARTIAL);
    ASSERT_EQ(probe.part_bytes, 5 * MF_MB);
    ASSERT_FALSE(hyp_file_exists(probe.path)); /* and nothing at the good name */
    mf_disarm();

    /* Second attempt: the fetcher hands the transfer the byte offset it
     * already has, which is what makes a 639 MB download survivable. */
    mf_arm(0, 7 * MF_MB, 'a');
    ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_INTERRUPTED);
    ASSERT_EQ(g_transfer.last_resume_from, 5 * MF_MB);
    ASSERT_STR_EQ(g_transfer.last_url, HYP_MODEL_ASK_URL);
    mf_disarm();
    mf_end();
    PASS();
}

/* A server that will not honour a Range header (curl 33) or a partial file it
 * disagrees with (curl 36) is recoverable exactly once, by starting over. */
TEST(model_fetch_restarts_when_the_server_will_not_resume) {
    int codes[] = {33, 36};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        mf_begin();
        hyp_model_probe_t probe;
        hyp_model_ask_probe(&probe);
        ASSERT(mf_write_file(probe.part_path, 2 * MF_MB, 'b'));

        mf_arm(codes[i], -1, 0);
        g_transfer.exit_code_2 = 7; /* the retry then fails offline; that is fine */
        hyp_model_fetch_opts_t opts = mf_opts_yes();
        ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_OFFLINE);
        ASSERT_EQ(g_transfer.calls, 2);
        ASSERT_EQ(g_transfer.last_resume_from, 0); /* restarted, not resumed */
        mf_disarm();
        mf_end();
    }
    PASS();
}

#ifndef _WIN32
/* The gate, end to end: a download of exactly the right LENGTH but the wrong
 * CONTENT is rejected, the partial file is deleted, and nothing is left at the
 * name a later run would trust. */
TEST(model_fetch_rejects_full_size_garbage_and_installs_nothing) {
    mf_begin();
    mf_arm(0, HYP_MODEL_ASK_BYTES, 0); /* right size, all zeros */
    hyp_model_fetch_opts_t opts = mf_opts_yes();
    ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_DIGEST_MISMATCH);

    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT_EQ(probe.state, HYP_MODEL_ABSENT);
    ASSERT_FALSE(hyp_file_exists(probe.path));
    ASSERT_FALSE(hyp_file_exists(probe.part_path));
    mf_disarm();
    mf_end();
    PASS();
}

/* A partial longer than the artifact cannot be resumed into — a byte range
 * past the end of the object is a 416, not a download. */
TEST(model_fetch_discards_an_oversized_partial) {
    mf_begin();
    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT(mf_set_size(probe.part_path, HYP_MODEL_ASK_BYTES + MF_MB));

    mf_arm(7, -1, 0);
    hyp_model_fetch_opts_t opts = mf_opts_yes();
    ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_OFFLINE);
    ASSERT_EQ(g_transfer.last_resume_from, 0);
    mf_disarm();
    mf_end();
    PASS();
}

TEST(model_fetch_is_a_noop_when_present_unless_forced) {
    mf_begin();
    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT(mf_set_size(probe.path, HYP_MODEL_ASK_BYTES));

    mf_arm(0, -1, 0);
    hyp_model_fetch_opts_t opts = mf_opts_yes();
    ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_ALREADY_PRESENT);
    ASSERT_EQ(g_transfer.calls, 0);

    opts.force = true;
    g_transfer.exit_code = 7;
    ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_OFFLINE);
    ASSERT_EQ(g_transfer.calls, 1);
    mf_disarm();
    mf_end();
    PASS();
}
#endif

/* A file of the wrong length at the good name is in the way of the rename and
 * cannot be resumed into. It is named and removed, not silently trusted. */
TEST(model_fetch_clears_a_wrong_size_file_out_of_the_way) {
    mf_begin();
    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT(mf_write_file(probe.path, MF_MB, 'x'));

    mf_arm(7, -1, 0);
    hyp_model_fetch_opts_t opts = mf_opts_yes();
    ASSERT_EQ(hyp_model_fetch(&opts), HYP_MODEL_FETCH_OFFLINE);
    ASSERT_FALSE(hyp_file_exists(probe.path));
    mf_disarm();
    mf_end();
    PASS();
}

/* ── The command ─────────────────────────────────────────────────── */

/* NEXT-STEPS.md §1: upstream's `install --help` ignored unknown flags and
 * performed a real installation. Not on a command whose accident costs
 * 639 MB. */
TEST(model_cmd_rejects_unknown_flags_instead_of_ignoring_them) {
    mf_begin();
    mf_arm(0, -1, 0);
    char *argv[] = {(char *)"--yes", (char *)"--dry-run"};
    ASSERT_EQ(hyp_cmd_fetch_model(2, argv), 2);
    ASSERT_EQ(g_transfer.calls, 0);

    char *help_argv[] = {(char *)"--help"};
    ASSERT_EQ(hyp_cmd_fetch_model(1, help_argv), 0);
    ASSERT_EQ(g_transfer.calls, 0);
    mf_disarm();
    mf_end();
    PASS();
}

/* Discoverability: a user who has fetched 639 MB can ask where it went. */
TEST(model_cmd_path_reports_the_location_without_fetching) {
    mf_begin();
    mf_arm(0, -1, 0);
    char *argv[] = {(char *)"--path"};
    ASSERT_EQ(hyp_cmd_fetch_model(1, argv), 0);
    ASSERT_EQ(g_transfer.calls, 0);
    mf_disarm();
    mf_end();
    PASS();
}

/* `--verify` on a file that is not the artifact must fail loudly and must not
 * download anything to "fix" it. */
TEST(model_cmd_verify_refuses_a_wrong_file_and_downloads_nothing) {
    mf_begin();
    mf_arm(0, -1, 0);
    char *argv[] = {(char *)"--verify"};
    ASSERT_EQ(hyp_cmd_fetch_model(1, argv), 1); /* nothing there to verify */
    ASSERT_EQ(g_transfer.calls, 0);

#ifndef _WIN32
    hyp_model_probe_t probe;
    hyp_model_ask_probe(&probe);
    ASSERT(mf_set_size(probe.path, HYP_MODEL_ASK_BYTES)); /* right size, wrong bytes */
    ASSERT_EQ(hyp_cmd_fetch_model(1, argv), 1);
    ASSERT_EQ(g_transfer.calls, 0);
    ASSERT_TRUE(hyp_file_exists(probe.path)); /* --verify does not delete */
#endif
    mf_disarm();
    mf_end();
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(model_fetch) {
    RUN_TEST(model_pin_names_one_immutable_revision);
    RUN_TEST(model_sha256_agrees_with_the_reference_tool);

    RUN_TEST(model_path_follows_the_cache_convention);
    RUN_TEST(model_probe_reports_absent_partial_present_and_wrong_size);
    RUN_TEST(model_probe_reports_no_cache_dir_separately);

    RUN_TEST(model_verify_accepts_exactly_the_right_bytes);
    RUN_TEST(model_verify_rejects_one_flipped_bit_and_deletes_the_file);
    RUN_TEST(model_verify_rejects_a_truncated_file);
    RUN_TEST(model_verify_rejects_an_empty_file);
    RUN_TEST(model_verify_keeps_the_file_when_told_not_to_delete);
    RUN_TEST(model_verify_refuses_what_it_cannot_read);

    RUN_TEST(model_unavailable_says_what_is_missing_how_big_and_what_to_type);
    RUN_TEST(model_unavailable_offers_a_resume_when_a_partial_exists);
#ifndef _WIN32
    RUN_TEST(model_unavailable_does_not_claim_absence_when_present);
#endif
    RUN_TEST(model_every_failure_says_what_to_do);

#ifndef _WIN32
    RUN_TEST(model_fetch_refuses_to_run_unattended_without_yes);
#endif
    RUN_TEST(model_fetch_translates_every_transport_failure);
    RUN_TEST(model_fetch_does_not_blame_a_missing_curl_for_a_killed_one);
    RUN_TEST(model_fetch_keeps_the_partial_and_resumes_from_it);
    RUN_TEST(model_fetch_restarts_when_the_server_will_not_resume);
#ifndef _WIN32
    RUN_TEST(model_fetch_rejects_full_size_garbage_and_installs_nothing);
    RUN_TEST(model_fetch_discards_an_oversized_partial);
    RUN_TEST(model_fetch_is_a_noop_when_present_unless_forced);
#endif
    RUN_TEST(model_fetch_clears_a_wrong_size_file_out_of_the_way);

    RUN_TEST(model_cmd_rejects_unknown_flags_instead_of_ignoring_them);
    RUN_TEST(model_cmd_path_reports_the_location_without_fetching);
    RUN_TEST(model_cmd_verify_refuses_a_wrong_file_and_downloads_nothing);
}
