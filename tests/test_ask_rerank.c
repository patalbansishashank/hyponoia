/*
 * test_ask_rerank.c — the cross-encoder stage and its SECOND model
 * (NEXT-STEPS.md §2.2 lever 2, runs/ASK/L2-reranker.json).
 *
 * The thing worth testing here is not the arithmetic — a softmax over two
 * logits either runs or does not, and whether it RANKS well is a measurement,
 * not an assertion. What can go wrong silently is the bookkeeping around it:
 *
 *   1. TWO MODELS, INDEPENDENT. Before this lane there was one artifact and
 *      one "is the model here" question. There are now two, they are the same
 *      size and the same family, and the failure this guards is a probe or a
 *      message that answers about the wrong one. A user who has a working
 *      `ask` must never be told to re-download 639 MB they already have.
 *
 *   2. THE TWO UNAVAILABLE STORIES ARE DIFFERENT CLAIMS. Missing embedding
 *      weights mean NOTHING WAS SEARCHED. Missing reranker weights mean the
 *      answer is the dense ordering — a real answer, differently ordered. The
 *      embedder's sentence appearing over a missing reranker would be a lie
 *      about what happened, so the absence of "NOTHING was searched" is
 *      asserted directly rather than left to prose review.
 *
 *   3. THE PIN. Same gate as the embedding model: one immutable revision, a
 *      digest that looks like a digest, an exact byte count. A reranker whose
 *      URL says /main/ is a reranker that can change under a shipped binary.
 *
 * No network and no weights: HYP_CACHE_DIR points at a temp directory and the
 * byte-moving step is replaced by the same seam test_model_fetch.c uses. The
 * scoring path itself needs 639 MB and a GPU and is measured, not unit-tested;
 * what IS asserted here is that without those weights it says NO_WEIGHTS
 * rather than failing, because those two send a caller to different places.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <ask/ask_rerank.h>
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

enum { RRT_URL_MAX = 1024, RRT_HEX = 64, RRT_MSG = 1024 };

static char g_cache[512];

static void rrt_begin(void) {
    char *dir = th_mktempdir("hyp_ask_rerank");
    if (dir) {
        snprintf(g_cache, sizeof(g_cache), "%s", dir);
        hyp_setenv("HYP_CACHE_DIR", g_cache, 1);
    } else {
        g_cache[0] = '\0';
    }
}

static void rrt_end(void) {
    hyp_unsetenv("HYP_CACHE_DIR");
    if (g_cache[0]) {
        th_rmtree(g_cache);
        g_cache[0] = '\0';
    }
}

/* Make `path` exactly `n` bytes, sparsely — 639 MB of holes costs no disk and
 * is the only affordable way to exercise the exact-size branch. */
static bool rrt_set_size(const char *path, int64_t n) {
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

/* ── The pin ─────────────────────────────────────────────────────── */

TEST(rerank_pin_names_one_immutable_revision) {
    const char *url = HYP_MODEL_RERANK_URL;
    ASSERT_NOT_NULL(strstr(url, "https://"));
    ASSERT_NOT_NULL(strstr(url, HYP_MODEL_RERANK_REVISION));
    ASSERT_NOT_NULL(strstr(url, HYP_MODEL_RERANK_FILE));
    /* The two ways a pin stops being a pin. */
    ASSERT_NULL(strstr(url, "/main/"));
    ASSERT_NULL(strstr(url, "latest"));

    ASSERT_EQ(strlen(HYP_MODEL_RERANK_SHA256), RRT_HEX);
    for (size_t i = 0; i < strlen(HYP_MODEL_RERANK_SHA256); i++) {
        char c = HYP_MODEL_RERANK_SHA256[i];
        ASSERT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    ASSERT_EQ(strlen(HYP_MODEL_RERANK_REVISION), 40);
    ASSERT_EQ((long long)HYP_MODEL_RERANK_BYTES, 639150432LL);
    return 0;
}

/* The two artifacts must not be confusable at any point a caller can reach:
 * not the file on disk, not the URL, not the digest, not the command that
 * fetches them. They are the same size and the same family, which is exactly
 * why this is asserted rather than assumed. */
TEST(the_two_models_are_distinct_everywhere) {
    const hyp_model_spec_t *ask = hyp_model_ask_spec();
    const hyp_model_spec_t *rr = hyp_model_rerank_spec();
    ASSERT_NOT_NULL(ask);
    ASSERT_NOT_NULL(rr);
    ASSERT_STR_NEQ(ask->file, rr->file);
    ASSERT_STR_NEQ(ask->url, rr->url);
    ASSERT_STR_NEQ(ask->sha256, rr->sha256);
    ASSERT_STR_NEQ(ask->revision, rr->revision);
    ASSERT_STR_NEQ(ask->model, rr->model);
    ASSERT_STR_NEQ(ask->command, rr->command);
    /* And the sentences, which is the part a human reads. */
    ASSERT_STR_NEQ(ask->what, rr->what);
    ASSERT_STR_NEQ(ask->without_it, rr->without_it);
    return 0;
}

/* ── Independence ────────────────────────────────────────────────── */

/* The failure this exists for: one model's presence answering for the other.
 * Both directions, because a probe that reads the wrong path is wrong both
 * ways round and only one of them would ever be noticed in use. */
TEST(each_model_is_probed_on_its_own) {
    rrt_begin();
    ASSERT(g_cache[0] != '\0');

    ASSERT(!hyp_model_ask_present());
    ASSERT(!hyp_model_rerank_present());

    char ask_path[HYP_MODEL_PATH_MAX];
    char rr_path[HYP_MODEL_PATH_MAX];
    ASSERT_NOT_NULL(hyp_model_ask_path(ask_path, sizeof(ask_path)));
    ASSERT_NOT_NULL(hyp_model_rerank_path(rr_path, sizeof(rr_path)));
    ASSERT_STR_NEQ(ask_path, rr_path);
    /* Same directory, different names: one `rm -r` reclaims both. */
    ASSERT_NOT_NULL(strstr(ask_path, g_cache));
    ASSERT_NOT_NULL(strstr(rr_path, g_cache));

    /* Only the embedding model present. */
    ASSERT(rrt_set_size(ask_path, HYP_MODEL_ASK_BYTES));
    ASSERT(hyp_model_ask_present());
    ASSERT(!hyp_model_rerank_present());

    /* Only the reranker present. */
    ASSERT_EQ(hyp_unlink(ask_path), 0);
    ASSERT(rrt_set_size(rr_path, HYP_MODEL_RERANK_BYTES));
    ASSERT(!hyp_model_ask_present());
    ASSERT(hyp_model_rerank_present());

    /* Both. */
    ASSERT(rrt_set_size(ask_path, HYP_MODEL_ASK_BYTES));
    ASSERT(hyp_model_ask_present());
    ASSERT(hyp_model_rerank_present());

    rrt_end();
    return 0;
}

/* A file of the wrong length at the reranker's name is refused, not loaded —
 * the same exact-size rule the embedding model gets, because "close enough" is
 * how a truncated blob reaches the loader. */
TEST(a_wrong_sized_reranker_is_not_present) {
    rrt_begin();
    char rr_path[HYP_MODEL_PATH_MAX];
    ASSERT_NOT_NULL(hyp_model_rerank_path(rr_path, sizeof(rr_path)));

    ASSERT(rrt_set_size(rr_path, HYP_MODEL_RERANK_BYTES - 1));
    ASSERT(!hyp_model_rerank_present());
    ASSERT(rrt_set_size(rr_path, HYP_MODEL_RERANK_BYTES + 1));
    ASSERT(!hyp_model_rerank_present());
    /* And the embedding model's byte count is NOT accepted here: they differ
     * by 160 bytes, which is exactly the sort of near-miss a copy-paste of the
     * wrong constant produces. */
    ASSERT_NEQ((long long)HYP_MODEL_ASK_BYTES, (long long)HYP_MODEL_RERANK_BYTES);
    ASSERT(rrt_set_size(rr_path, HYP_MODEL_ASK_BYTES));
    ASSERT(!hyp_model_rerank_present());

    rrt_end();
    return 0;
}

/* ── The two unavailable stories ─────────────────────────────────── */

/* THE LOAD-BEARING ONE. Without the embedding model nothing was searched;
 * without the reranker everything was searched and the order is just the dense
 * one. Those are different claims about what happened, and the wrong one is
 * not a wording problem — it tells a caller their codebase has been examined
 * when it has not, or that it has not when it has. */
TEST(the_reranker_absence_does_not_claim_nothing_was_searched) {
    rrt_begin();

    char detail[RRT_MSG];
    char remedy[RRT_MSG];
    hyp_model_unavailable_text_spec(hyp_model_rerank_spec(), "lld-elf", detail, sizeof(detail),
                                    remedy, sizeof(remedy));
    ASSERT_NOT_NULL(strstr(detail, HYP_MODEL_RERANK_MODEL));
    ASSERT_NULL(strstr(detail, "NOTHING was searched"));
    ASSERT_NOT_NULL(strstr(detail, "dense ordering"));
    ASSERT_NOT_NULL(strstr(remedy, HYP_MODEL_RERANK_COMMAND));

    /* The embedding model's, for contrast, in the same cache state. */
    char adetail[RRT_MSG];
    char aremedy[RRT_MSG];
    hyp_model_unavailable_text(  "lld-elf", adetail, sizeof(adetail), aremedy, sizeof(aremedy));
    ASSERT_NOT_NULL(strstr(adetail, "NOTHING was searched"));
    ASSERT_NOT_NULL(strstr(adetail, HYP_MODEL_ASK_MODEL));
    ASSERT_STR_NEQ(detail, adetail);
    ASSERT_STR_NEQ(remedy, aremedy);

    rrt_end();
    return 0;
}

/* The remedy must name the command that actually fetches THIS model. The
 * embedding model's command fetches the embedding model, so pointing a missing
 * reranker at it produces "already present" and no reranker. */
TEST(the_reranker_remedy_names_the_reranker_command) {
    rrt_begin();
    char detail[RRT_MSG];
    char remedy[RRT_MSG];
    hyp_model_unavailable_text_spec(hyp_model_rerank_spec(), NULL, detail, sizeof(detail), remedy,
                                    sizeof(remedy));
    ASSERT_NOT_NULL(strstr(remedy, "--rerank"));
    ASSERT_NOT_NULL(strstr(remedy, HYP_MODEL_RERANK_SIZE_TEXT));
    ASSERT_NOT_NULL(strstr(remedy, "SHA-256"));
    rrt_end();
    return 0;
}

/* ── The fetch, through the seam ─────────────────────────────────── */

#ifdef HYP_ENABLE_TEST_SEAMS
typedef struct {
    int calls;
    char last_url[RRT_URL_MAX];
    char last_dest[RRT_URL_MAX];
} rrt_transfer_t;

static rrt_transfer_t g_tx;

static int rrt_transfer(const char *url, const char *dest, int64_t resume_from, void *ud) {
    (void)ud;
    (void)resume_from;
    g_tx.calls++;
    snprintf(g_tx.last_url, sizeof(g_tx.last_url), "%s", url ? url : "");
    snprintf(g_tx.last_dest, sizeof(g_tx.last_dest), "%s", dest ? dest : "");
    /* Right size, wrong bytes: the digest gate must be what rejects it, and it
     * must reject it — a 639 MB file of zeros hashing to the pinned digest
     * would mean the gate is not running at all. */
    (void)rrt_set_size(dest, HYP_MODEL_RERANK_BYTES);
    return 0;
}

/* Fetching the reranker must fetch THE RERANKER: the pinned reranker URL, into
 * the reranker's own .part file. A spec-driven fetcher that ignored its spec
 * would download the embedding model twice and report success. */
TEST(fetching_the_reranker_uses_the_reranker_pin) {
    rrt_begin();
    memset(&g_tx, 0, sizeof(g_tx));
    hyp_model_set_transfer_for_test(rrt_transfer, NULL);

    hyp_model_fetch_opts_t opts = {0};
    opts.assume_yes = true;
    opts.quiet = true;
    hyp_model_fetch_result_t r = hyp_model_fetch_spec(hyp_model_rerank_spec(), &opts);

    ASSERT_EQ(g_tx.calls, 1);
    ASSERT_STR_EQ(g_tx.last_url, HYP_MODEL_RERANK_URL);
    ASSERT_NOT_NULL(strstr(g_tx.last_dest, HYP_MODEL_RERANK_FILE));
    ASSERT_NOT_NULL(strstr(g_tx.last_dest, HYP_MODEL_PART_SUFFIX));
    /* Full size and wrong contents: REJECTED, and the file deleted rather than
     * left where a later run would trust it. */
    ASSERT_EQ((int)r, (int)HYP_MODEL_FETCH_DIGEST_MISMATCH);
    ASSERT(!hyp_model_rerank_present());
    /* And it did not touch the other model. */
    ASSERT(!hyp_model_ask_present());

    hyp_model_set_transfer_for_test(NULL, NULL);
    rrt_end();
    return 0;
}
#endif /* HYP_ENABLE_TEST_SEAMS */

/* ── The stage's own availability ────────────────────────────────── */

/* `available` is compiled-in AND on disk, and the two are genuinely different
 * questions with different remedies — a build vs a download. */
TEST(rerank_availability_is_build_and_disk) {
    rrt_begin();
    ASSERT(!hyp_model_rerank_present());
    ASSERT(!hyp_ask_rerank_available());

    char rr_path[HYP_MODEL_PATH_MAX];
    ASSERT_NOT_NULL(hyp_model_rerank_path(rr_path, sizeof(rr_path)));
    ASSERT(rrt_set_size(rr_path, HYP_MODEL_RERANK_BYTES));
    /* With the weights on disk, availability now tracks the BUILD alone. The
     * test binary links the stub encoder, so this is the honest answer for
     * whichever build is running rather than a hardcoded expectation. */
    ASSERT_EQ(hyp_ask_rerank_available(), hyp_ask_rerank_compiled_in());

    rrt_end();
    return 0;
}

/* Absent weights are NO_WEIGHTS, not FAILED. They send a caller to different
 * places: one is a download, the other is a bug report. */
TEST(scoring_without_weights_says_no_weights) {
    rrt_begin();
    char err[RRT_MSG] = "";
    const char *docs[2] = {"void f() {}", "void g() {}"};
    float out[2] = {-1.0f, -1.0f};
    hyp_ask_rerank_status_t st =
        hyp_ask_rerank_score("what does f do", "C++", docs, 2, out, err, sizeof(err));
    if (hyp_ask_rerank_compiled_in()) {
        ASSERT_EQ((int)st, (int)HYP_ASK_RERANK_NO_WEIGHTS);
    } else {
        ASSERT_EQ((int)st, (int)HYP_ASK_RERANK_NO_BACKEND);
    }
    /* On any non-OK outcome the output is UNTOUCHED, so a partial rerank can
     * never be mistaken for a complete one by a caller that forgot to check. */
    ASSERT(out[0] == -1.0f);
    ASSERT(out[1] == -1.0f);
    rrt_end();
    return 0;
}

/* Nothing to score is success, not a failure and not a reason to load 639 MB.
 * `ask` reaching this with an empty result set is an ordinary outcome. */
TEST(scoring_nothing_is_ok_and_loads_nothing) {
    rrt_begin();
    char err[RRT_MSG] = "";
    float out[1];
    ASSERT_EQ((int)hyp_ask_rerank_score("q", "C++", NULL, 0, out, err, sizeof(err)),
              (int)HYP_ASK_RERANK_OK);
    ASSERT_EQ((int)err[0], 0);
    ASSERT_EQ(hyp_ask_rerank_last_truncated(), 0);
    rrt_end();
    return 0;
}

/* The depths §2.2 names, and the ceiling they carry. Not a style check: the
 * default is what an agent gets when it passes `rerank` without a number, and
 * the cap is what stops "rerank everything" from being spellable. */
TEST(the_rerank_depths_are_the_ones_the_spec_named) {
    ASSERT_EQ(HYP_ASK_RERANK_DEFAULT_N, 50);
    ASSERT(HYP_ASK_RERANK_MAX_N >= 100);
    /* A pair window smaller than the model's 40,960-token context is a
     * deliberate trade priced in KV at 112 KiB/token — see ask_rerank.h. It
     * must still be big enough that an ordinary declaration is never cut. */
    ASSERT(HYP_ASK_RERANK_PAIR_WINDOW >= 1024);
    ASSERT(HYP_ASK_RERANK_MAX_PAIRS >= 1);
    return 0;
}

SUITE(ask_rerank) {
    RUN_TEST(rerank_pin_names_one_immutable_revision);
    RUN_TEST(the_two_models_are_distinct_everywhere);
    RUN_TEST(each_model_is_probed_on_its_own);
    RUN_TEST(a_wrong_sized_reranker_is_not_present);
    RUN_TEST(the_reranker_absence_does_not_claim_nothing_was_searched);
    RUN_TEST(the_reranker_remedy_names_the_reranker_command);
#ifdef HYP_ENABLE_TEST_SEAMS
    RUN_TEST(fetching_the_reranker_uses_the_reranker_pin);
#endif
    RUN_TEST(rerank_availability_is_build_and_disk);
    RUN_TEST(scoring_without_weights_says_no_weights);
    RUN_TEST(scoring_nothing_is_ok_and_loads_nothing);
    RUN_TEST(the_rerank_depths_are_the_ones_the_spec_named);
}
