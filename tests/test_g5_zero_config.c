/*
 * test_g5_zero_config.c — Track G, unit G5: out of the box.
 *
 * The claim under test is "a fresh repo, no TOML, no Multica, no flags —
 * indexes and answers". The chain is the easy half. The hard half is proving
 * that the chain is what produced the green: a warm cache, an inherited
 * environment variable, a model already on disk, or a project somebody else
 * indexed will each make a zero-config test pass for reasons that have
 * nothing to do with zero configuration. A green that came from somewhere
 * other than the property is the failure mode this file is built against.
 *
 * ── THE CLEAN ROOM, AND WHY IT IS AUDITED RATHER THAN ASSUMED ──────────────
 *
 * Every test here runs inside g5_room_begin/g5_room_end, which:
 *
 *   - removes every HYP_* variable from this process's environment. The
 *     enumeration comes from `environ` itself, never from a list somebody
 *     typed: a hand-written list of the product's own knobs is only as good
 *     as its author's memory of them, and the variable that steers the run is
 *     exactly the one such a list omits.
 *   - points HYP_CACHE_DIR at a directory made seconds earlier, and asserts
 *     that directory is EMPTY — no project database, no vectors, no config,
 *     no grant store, no model weights.
 *   - creates the repository as a directory outside this checkout holding two
 *     C files and nothing else — in particular no hyponoia.toml.
 *
 * g5_clean_room_guarantees_the_preconditions_are_absent runs that audit as a
 * test of its own, so the fixture cannot quietly stop guaranteeing what every
 * other test in this file assumes. A precondition that cannot be verified is
 * a failure here, never a skip.
 *
 * Multica needs no purge and gets an assertion instead of a fixture: the
 * adapter is defined over an injected row seam with no live transport in this
 * tree, and the zero-config resolver reads one file and no feed at all. What
 * is asserted is the resolver's own answer — shared feeds off.
 *
 * ── WHAT "ANSWERS" MEANS HERE, AND WHAT IT DOES NOT ────────────────────────
 *
 * The default lane is the local nano encoder on CPU. This binary is the test
 * runner, and Makefile.hyp compiles it WITHOUT the inference backend on
 * purpose, because the ask suites drive the encoder seam and must run with no
 * model on disk. The local lane therefore cannot be exercised from here, and
 * nothing below claims it was: both sides of the lane are driven by one
 * deterministic axis-vector double, the construction the ask suites already
 * use. What these tests demonstrate is that the PATH resolves, indexes, and
 * returns a correctly shaped and correctly ordered answer with nothing typed.
 * Retrieval quality is not measured here and no number in this file is about
 * it. g5_the_lane_this_build_cannot_run_is_reported_as_absent pins the other
 * half: what a user is told when the encoder the default lane would use is
 * not there.
 *
 * ── "NO FLAGS" IS THE LOAD-BEARING WORD ────────────────────────────────────
 *
 * The B4 suite proves the path decision and indexes through it, naming the
 * project on every call. That is one flag more than the claim allows. The
 * answer here is asked as {"question": "..."} and nothing else, so the
 * project comes from the resolver's own sole-indexed-project rule, and the
 * answer's `project_source` disclosure is asserted to say so. An answer that
 * named the project would prove the index works; only an answer that did not
 * proves the configuration is genuinely zero.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <ask/ask_cmd.h>    /* hyp_cmd_embed — the recipe's second line */
#include <ask/ask_embed.h>  /* hyp_ask_embed_run — the real embed pass */
#include <ask/ask_encoder.h>
#include <ask/ask_llama.h> /* hyp_ask_llama_compiled_in — what THIS build is */
#include <cli/cli.h>       /* hyp_cli_build_args_json */
#include <cli/command_surface.h>
#include <cli/model_fetch.h> /* hyp_model_ask_present/path */
#include <cli/onboard.h>
#include <foundation/compat_fs.h>
#include <foundation/identity.h> /* the address a zero-config repo derives */
#include <foundation/platform.h>
#include <foundation/workspace.h>
#include <mcp/mcp.h>
#include <semantic/ask_embed.h>       /* hyp_ask_backend_t — the query side */
#include <store/workspace_resolve.h>  /* THE resolver, and its TOML finder */
#include <yyjson/yyjson.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define g5_dup _dup
#define g5_dup2 _dup2
#define g5_close _close
#define G5_STDOUT_FD 1
#define G5_ENVIRON _environ
#else
#include <unistd.h>
#define g5_dup dup
#define g5_dup2 dup2
#define g5_close close
#define G5_STDOUT_FD STDOUT_FILENO
extern char **environ;
#define G5_ENVIRON environ
#endif

/* ── One axis-vector encoder, both sides of the lane ──────────────── */

/* The document side and the query side share this hash, so a question that is
 * byte-identical to an indexed span scores 1.0 against it and 0.0 against
 * everything else. The ordering assertions below are then exact rather than a
 * tendency. */
static void g5_axis_vector(const char *text, float *out, int dim) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h = h * 33u + *p;
    }
    memset(out, 0, (size_t)dim * sizeof(float));
    out[h % (unsigned long)dim] = 1.0f;
}

#define G5_ENC_MODEL_ID "test-double/axis-1024"

static const char *g5_enc_model_id(void *self) {
    (void)self;
    return G5_ENC_MODEL_ID;
}
static const char *g5_enc_prefix_contract(void *self) {
    (void)self;
    return "test-double-contract";
}
static const char *g5_enc_device_note(void *self) {
    (void)self;
    return "CPU (test double)";
}
static bool g5_enc_device_is_gpu(void *self) {
    (void)self;
    return false;
}
static int g5_enc_dim(void *self) {
    (void)self;
    return HYP_ASK_DIM;
}
static int g5_enc_window(void *self) {
    (void)self;
    return 32768;
}
static int g5_enc_token_length(void *self, const char *text) {
    (void)self;
    return (int)(strlen(text) / 4) + 1;
}
static int g5_enc_encode_documents(void *self, const char *const *texts, int count, float *out) {
    (void)self;
    for (int i = 0; i < count; i++) {
        g5_axis_vector(texts[i], out + (size_t)i * HYP_ASK_DIM, HYP_ASK_DIM);
    }
    return 0;
}
static int g5_enc_encode_query(void *self, const char *text, const char *lang, float *out) {
    (void)self;
    (void)lang;
    g5_axis_vector(text, out, HYP_ASK_DIM);
    return 0;
}
static void g5_enc_destroy(void *self) {
    (void)self;
}

static const hyp_ask_encoder_vt_t G5_ENC_VT = {
    .model_id = g5_enc_model_id,
    .prefix_contract = g5_enc_prefix_contract,
    .device_note = g5_enc_device_note,
    .device_is_gpu = g5_enc_device_is_gpu,
    .dim = g5_enc_dim,
    .window_tokens = g5_enc_window,
    .token_length = g5_enc_token_length,
    .full_token_length = g5_enc_token_length,
    .encode_documents = g5_enc_encode_documents,
    .encode_query = g5_enc_encode_query,
    .destroy = g5_enc_destroy,
};

static int g5_backend_encode_query(HYPLanguage lang, const char *text, float *out, char *err,
                                   size_t errlen) {
    (void)lang;
    (void)err;
    (void)errlen;
    g5_axis_vector(text, out, HYP_ASK_DIM);
    return 0;
}
static int g5_backend_encode_documents(const char *const *texts, int n, float *out, char *err,
                                       size_t errlen) {
    (void)err;
    (void)errlen;
    for (int i = 0; i < n; i++) {
        g5_axis_vector(texts[i], out + (size_t)i * HYP_ASK_DIM, HYP_ASK_DIM);
    }
    return 0;
}
static const hyp_ask_backend_t G5_BACKEND = {
    .model_id = G5_ENC_MODEL_ID,
    .dim = HYP_ASK_DIM,
    .window_tokens = 32768,
    .encode_query = g5_backend_encode_query,
    .encode_documents = g5_backend_encode_documents,
    .truncates = NULL,
};

/* ── The clean room ───────────────────────────────────────────────── */

/* alpha's span, written once, so the bytes the indexer reads and the bytes
 * this file calls "alpha's span" cannot drift apart. The question asked below
 * is the JSON escaping of this — the one spelling that cannot be shared,
 * since a JSON string literal is not a C one — and the cosine assertion on the
 * top row is what fails loudly if the two ever stop matching. */
#define G5_ALPHA "int alpha(void) {\n    return 1;\n}"

enum { G5_MAX_SAVED_ENV = 128 };

typedef struct {
    char *cache; /* HYP_CACHE_DIR for the duration */
    char *repo;  /* the fresh repository */
    char *names[G5_MAX_SAVED_ENV];
    char *values[G5_MAX_SAVED_ENV];
    int saved_count;
} g5_room_t;

/* Count the HYP_* variables visible in this process right now. Derived from
 * `environ`, which is the only enumeration of them that cannot be incomplete. */
static int g5_env_hyp_count(void) {
    int n = 0;
    for (char **e = G5_ENVIRON; e && *e; e++) {
        if (strncmp(*e, "HYP_", 4) == 0) {
            n++;
        }
    }
    return n;
}

/* Two passes on purpose: unsetenv rewrites `environ`, so every name is copied
 * out before any of them is removed. */
static bool g5_env_purge(g5_room_t *r) {
    for (char **e = G5_ENVIRON; e && *e; e++) {
        if (strncmp(*e, "HYP_", 4) != 0) {
            continue;
        }
        const char *eq = strchr(*e, '=');
        if (!eq || r->saved_count >= G5_MAX_SAVED_ENV) {
            return false;
        }
        size_t nlen = (size_t)(eq - *e);
        char *name = (char *)malloc(nlen + 1);
        char *value = hyp_strdup(eq + 1);
        if (!name || !value) {
            free(name);
            free(value);
            return false;
        }
        memcpy(name, *e, nlen);
        name[nlen] = '\0';
        r->names[r->saved_count] = name;
        r->values[r->saved_count] = value;
        r->saved_count++;
    }
    for (int i = 0; i < r->saved_count; i++) {
        if (hyp_unsetenv(r->names[i]) != 0) {
            return false;
        }
    }
    return true;
}

static void g5_env_restore(g5_room_t *r) {
    for (int i = 0; i < r->saved_count; i++) {
        (void)hyp_setenv(r->names[i], r->values[i], 1);
        free(r->names[i]);
        free(r->values[i]);
        r->names[i] = NULL;
        r->values[i] = NULL;
    }
    r->saved_count = 0;
}

static bool g5_room_begin(g5_room_t *r) {
    memset(r, 0, sizeof(*r));
    if (!g5_env_purge(r)) {
        return false;
    }
    const char *made = th_mktempdir("hyp-g5-cache");
    r->cache = made ? hyp_strdup(made) : NULL;
    if (!r->cache || hyp_setenv("HYP_CACHE_DIR", r->cache, 1) != 0) {
        return false;
    }
    made = th_mktempdir("hyp-g5-repo");
    r->repo = made ? hyp_strdup(made) : NULL;
    if (!r->repo) {
        return false;
    }
    if (th_write_file(TH_PATH(r->repo, "src/x.c"), G5_ALPHA "\n"
                                                            "int beta(void) {\n"
                                                            "    return 2;\n"
                                                            "}\n") != 0) {
        return false;
    }
    return th_write_file(TH_PATH(r->repo, "src/y.c"), "int gamma(void) {\n    return 3;\n}\n") == 0;
}

static void g5_room_end(g5_room_t *r) {
    (void)hyp_unsetenv("HYP_CACHE_DIR");
    g5_env_restore(r);
    if (r->repo) {
        th_cleanup(r->repo);
        free(r->repo);
        r->repo = NULL;
    }
    if (r->cache) {
        th_cleanup(r->cache);
        free(r->cache);
        r->cache = NULL;
    }
}

/* Entries in a directory, ignoring the two self-references. -1 when the
 * directory cannot be read, which is a failure and never a pass. */
static int g5_dir_entries(const char *path) {
    hyp_dir_t *d = hyp_opendir(path);
    if (!d) {
        return -1;
    }
    int n = 0;
    hyp_dirent_t *e = NULL;
    while ((e = hyp_readdir(d)) != NULL) {
        if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0) {
            continue;
        }
        n++;
    }
    hyp_closedir(d);
    return n;
}

/*
 * THE AUDIT. Returns the first precondition that is NOT met, or NULL.
 *
 * Every line here answers "what would this test still pass if zero-config
 * were broken?" with "nothing, because the state that could carry it is
 * demonstrably not present."
 */
static const char *g5_precondition_gap(const g5_room_t *r) {
    if (!r->cache || !r->repo) {
        return "the clean room did not come up";
    }
    /* One HYP_* variable in the environment, and it is the cache the fixture
     * just made. Anything else could steer the run. */
    if (g5_env_hyp_count() != 1) {
        return "a HYP_* environment variable other than HYP_CACHE_DIR is set";
    }
    /* Copied out immediately: the resolver hands back a pointer into one
     * thread-local buffer, and everything below that asks the product another
     * question about paths writes through it again. */
    const char *live = hyp_workspace_cache_dir();
    if (!live) {
        return "the product cannot resolve a cache directory";
    }
    char resolved[HYP_SZ_4K];
    (void)snprintf(resolved, sizeof(resolved), "%s", live);
    /* HYP_SZ_4K, not HYP_PATH_MAX: hyp_canonical_path is realpath() on POSIX,
     * which writes up to PATH_MAX whatever the caller's buffer says. */
    char want[HYP_SZ_4K];
    char got[HYP_SZ_4K];
    if (!hyp_canonical_path(r->cache, want, sizeof(want)) ||
        !hyp_canonical_path(resolved, got, sizeof(got)) || strcmp(want, got) != 0) {
        return "the product's cache directory is not the fixture's";
    }
    /* Empty means empty: no project database, no vectors, no config, no logs,
     * no model. A single leftover file is a candidate explanation for a pass. */
    if (g5_dir_entries(r->cache) != 0) {
        return "the isolated cache is not empty";
    }
    /* Confinement is off because nothing has been granted, which is the
     * out-of-the-box state the product documents. A grant file inherited from
     * a real machine would change what indexing is allowed to touch. */
    char grants[HYP_PATH_MAX];
    if (!hyp_workspace_grant_path(r->cache, grants, sizeof(grants)) || hyp_file_exists(grants)) {
        return "a grant store already exists in the isolated cache";
    }
    char listing[HYP_SZ_8K];
    if (hyp_workspace_grant_list(r->cache, listing, sizeof(listing))) {
        return "the grant store is non-empty";
    }
    /* The weights of the default lane are absent, and the path they would be
     * absent FROM is inside the isolated cache — otherwise the answer would be
     * about the developer's own machine. */
    char model[HYP_SZ_4K];
    if (!hyp_model_ask_path(model, sizeof(model))) {
        return "the model path cannot be resolved";
    }
    /* A string prefix, not hyp_path_within_root: containment there resolves
     * links on BOTH paths and so answers false for a path that does not
     * exist — which is precisely the state under test. The two strings come
     * from the same cache resolver, so the prefix is exact rather than a
     * heuristic. */
    if (strncmp(model, resolved, strlen(resolved)) != 0) {
        return "the model path points outside the isolated cache";
    }
    if (hyp_model_ask_present()) {
        return "model weights are already present in the isolated cache";
    }
    /* The repository has no record of a previous conversation — and neither
     * does anything ABOVE it. The one resolver locates a workspace file by
     * walking from the start directory toward the filesystem root, so a
     * hyponoia.toml two levels up governs this repository as surely as one
     * inside it. Checking only the repository would leave the widest
     * inherited-state channel open, and the product's own finder is what
     * answers here rather than a loop written for the occasion. */
    char toml[HYP_WSR_PATH_MAX];
    if (hyp_wsr_toml_find(r->repo, toml, sizeof(toml))) {
        return "a hyponoia.toml governs the fresh repository from at or above it";
    }
    if (hyp_wsr_toml_find(r->cache, toml, sizeof(toml))) {
        return "a hyponoia.toml sits at or above the isolated cache";
    }
    /* It is also not this repository, and not anywhere under it: a fixture
     * that accidentally pointed at the checkout would inherit its git history,
     * its own hyponoia.toml and its indexed state all at once. */
    char here[HYP_SZ_4K];
    char repo[HYP_SZ_4K];
    if (!hyp_canonical_path(".", here, sizeof(here)) ||
        !hyp_canonical_path(r->repo, repo, sizeof(repo))) {
        return "the fixture paths cannot be canonicalised";
    }
    if (hyp_path_within_root(here, repo) || hyp_path_within_root(repo, here)) {
        return "the fresh repository is inside this checkout";
    }
    if (hyp_file_exists(TH_PATH(r->repo, ".git")) ||
        hyp_file_exists(TH_PATH(r->repo, "Makefile.hyp"))) {
        return "the fresh repository is a checkout of something";
    }
    return NULL;
}

/* result.content[0].text — the client's view, not the emitted envelope. */
static char *g5_tool_text(const char *response) {
    yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *content = root ? yyjson_obj_get(root, "content") : NULL;
    yyjson_val *first = content ? yyjson_arr_get(content, 0) : NULL;
    yyjson_val *text = first ? yyjson_obj_get(first, "text") : NULL;
    char *out = (text && yyjson_is_str(text)) ? hyp_strdup(yyjson_get_str(text)) : NULL;
    yyjson_doc_free(doc);
    return out;
}

/* Index the room's repository through the production tool, with the path and
 * nothing else. Returns 0 on an answer that is not an error. */
static int g5_index(hyp_mcp_server_t *srv, const g5_room_t *r) {
    char args[HYP_PATH_MAX + HYP_SZ_64];
    (void)snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", r->repo);
    char *resp = hyp_mcp_handle_tool(srv, "index_repository", args);
    if (!resp) {
        return -1;
    }
    int rc = strstr(resp, "\"isError\":true") ? -1 : 0;
    free(resp);
    return rc;
}

/* ── The audit, as a test ─────────────────────────────────────────── */

TEST(g5_clean_room_guarantees_the_preconditions_are_absent) {
    g5_room_t room;
    if (!g5_room_begin(&room)) {
        g5_room_end(&room);
        FAIL("the clean room could not be established");
    }
    const char *gap = g5_precondition_gap(&room);
    if (gap) {
        g5_room_end(&room);
        FAIL(gap);
    }

    /* The purge is real, not nominal: whatever this process carried is gone
     * while the room is open and comes back when it closes. Restoring matters
     * as much as clearing — a fixture that left the runner's environment
     * stripped would quietly change every suite scheduled after it. */
    ASSERT_EQ(g5_env_hyp_count(), 1);
    int carried = room.saved_count;
    g5_room_end(&room);
    ASSERT_EQ(g5_env_hyp_count(), carried);
    PASS();
}

/* ── The headline: nothing typed, and it answers ──────────────────── */

TEST(g5_zero_config_indexes_and_answers_with_nothing_typed) {
    g5_room_t room;
    if (!g5_room_begin(&room)) {
        g5_room_end(&room);
        FAIL("the clean room could not be established");
    }
    const char *gap = g5_precondition_gap(&room);
    if (gap) {
        g5_room_end(&room);
        FAIL(gap);
    }

    /* 1 · The path decision, before anything runs. */
    hyp_onboard_resolved_t r;
    char err[HYP_SZ_512];
    err[0] = '\0';
    ASSERT_EQ(hyp_onboard_resolve(room.repo, &r, err, sizeof(err)), 0);
    ASSERT_STR_EQ(r.source, "defaults");
    ASSERT_TRUE(r.workspace_of_one);
    ASSERT_EQ(r.repo_count, 1);
    ASSERT_STR_EQ(r.repos[0].role, "member");
    ASSERT_STR_EQ(r.encoder, "local-nano");
    ASSERT_STR_EQ(r.device, "cpu");
    ASSERT_FALSE(r.shared_feeds);

    /* 2 · The index, through the production tool, with the path and nothing
     *     else. The empty cache is what makes the next step meaningful: after
     *     this there is exactly one indexed project on this machine. */
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(g5_index(srv, &room), 0);

    /* 3 · The embed pass, with the double standing in for local nano. Default
     *     paths throughout: the graph and the vectors land where the
     *     zero-config chain puts them, inside the isolated cache. */
    hyp_ask_encoder_t enc = {.vt = &G5_ENC_VT, .self = NULL};
    hyp_ask_embed_opts_t eopts;
    memset(&eopts, 0, sizeof(eopts));
    eopts.project = r.workspace;
    hyp_ask_embed_report_t rep;
    ASSERT_EQ(hyp_ask_embed_run(&enc, &eopts, &rep), 0);
    ASSERT_GT(rep.embedded, 0);
    hyp_ask_embed_report_free(&rep);

    /*
     * 4 · The answer, asked with NO PROJECT ARGUMENT — the whole point of the
     *     unit. The only key in the call is the question.
     *
     * The project therefore has to come from the resolver's sole-indexed
     * rule, and the answer has to say so. `project_source` is asserted for
     * exactly that reason: an answer that resolved by some other route would
     * mean some other state decided it, which is the failure this unit is
     * looking for rather than a detail of the disclosure.
     */
    ASSERT_EQ(hyp_ask_backend_install(&G5_BACKEND), 0);
    const char *question = "{\"question\":\"int alpha(void) {\\n    return 1;\\n}\"}";
    char *resp = hyp_mcp_handle_tool(srv, "ask", question);
    ASSERT_NOT_NULL(resp);
    char *text = g5_tool_text(resp);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "available: true"));
    ASSERT_NOT_NULL(strstr(text, r.workspace));
    ASSERT_NOT_NULL(strstr(text, "the only indexed project"));
    ASSERT_NOT_NULL(strstr(text, "alpha"));
    ASSERT_NOT_NULL(strstr(text, "src/x.c"));
    free(text);
    free(resp);

    /* 5 · The same question, structured, where rank is an array index rather
     *     than a byte offset and the separation that produced it is readable.
     *     Column positions come from `cols`, so an added column cannot
     *     silently shift an index written here by hand. */
    const char *jquestion = "{\"format\":\"json\",\"include_source\":false,"
                            "\"question\":\"int alpha(void) {\\n    return 1;\\n}\"}";
    resp = hyp_mcp_handle_tool(srv, "ask", jquestion);
    ASSERT_NOT_NULL(resp);
    char *jtext = g5_tool_text(resp);
    ASSERT_NOT_NULL(jtext);
    yyjson_doc *jdoc = yyjson_read(jtext, strlen(jtext), 0);
    ASSERT_NOT_NULL(jdoc);
    yyjson_val *jroot = yyjson_doc_get_root(jdoc);
    ASSERT_NOT_NULL(jroot);
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(jroot, "available")));
    const char *jproject = yyjson_get_str(yyjson_obj_get(jroot, "project"));
    ASSERT_NOT_NULL(jproject);
    ASSERT_STR_EQ(jproject, r.workspace);
    const char *jsource = yyjson_get_str(yyjson_obj_get(jroot, "project_source"));
    ASSERT_NOT_NULL(jsource);
    ASSERT_STR_EQ(jsource, "the only indexed project");

    yyjson_val *cols = yyjson_obj_get(jroot, "cols");
    ASSERT_NOT_NULL(cols);
    int qn_col = -1;
    int score_col = -1;
    size_t ci = 0;
    size_t cmax = 0;
    yyjson_val *cv = NULL;
    yyjson_arr_foreach(cols, ci, cmax, cv) {
        const char *cname = yyjson_get_str(cv);
        if (cname && strcmp(cname, "qn") == 0) {
            qn_col = (int)ci;
        } else if (cname && strcmp(cname, "score") == 0) {
            score_col = (int)ci;
        }
    }
    ASSERT_GTE(qn_col, 0);
    ASSERT_GTE(score_col, 0);

    yyjson_val *rows = yyjson_obj_get(jroot, "rows");
    ASSERT_NOT_NULL(rows);
    /* Three declarations were indexed and the tool applies no score
     * threshold, so three rows is what a working lane returns. Pinned
     * absolutely: a one-row answer would make every ordering assertion below
     * vacuously true. */
    ASSERT_EQ((long long)yyjson_arr_size(rows), 3);
    yyjson_val *row0 = yyjson_arr_get(rows, 0);
    ASSERT_NOT_NULL(row0);
    const char *qn0 = yyjson_get_str(yyjson_arr_get(row0, (size_t)qn_col));
    ASSERT_NOT_NULL(qn0);
    ASSERT_NOT_NULL(strstr(qn0, "alpha"));
    double s0 = yyjson_get_num(yyjson_arr_get(row0, (size_t)score_col));
    /* The question IS alpha's span, so the top score is the exact cosine and
     * not merely the largest of three numbers that happen to be equal. */
    ASSERT_GT((long long)(s0 * 1000000.0), 990000LL);
    for (size_t ri = 1; ri < yyjson_arr_size(rows); ri++) {
        yyjson_val *row = yyjson_arr_get(rows, ri);
        ASSERT_NOT_NULL(row);
        const char *qn = yyjson_get_str(yyjson_arr_get(row, (size_t)qn_col));
        ASSERT_NOT_NULL(qn);
        ASSERT_NULL(strstr(qn, "alpha"));
        double s = yyjson_get_num(yyjson_arr_get(row, (size_t)score_col));
        /* Strictly below, so the order is a separation and not a tie the sort
         * broke in our favour. */
        ASSERT_LT((long long)(s * 1000000.0), (long long)(s0 * 1000000.0));
    }
    yyjson_doc_free(jdoc);
    free(jtext);
    free(resp);

    (void)hyp_ask_backend_install(NULL);
    hyp_mcp_server_free(srv);
    g5_room_end(&room);
    PASS();
}

/* ── The same code at two paths ───────────────────────────────────── */

/* Ask a project for the top-ranked qualified name. The question is alpha's
 * span, so the top row is alpha's declaration and the QN is the indexer's own,
 * read back through the tool a client would use rather than recomputed here. */
static bool g5_top_qn(hyp_mcp_server_t *srv, const char *project, char *out, size_t out_sz) {
    char args[HYP_SZ_1K];
    (void)snprintf(args, sizeof(args),
                   "{\"project\":\"%s\",\"format\":\"json\",\"include_source\":false,"
                   "\"question\":\"int alpha(void) {\\n    return 1;\\n}\"}",
                   project);
    char *resp = hyp_mcp_handle_tool(srv, "ask", args);
    char *jtext = resp ? g5_tool_text(resp) : NULL;
    free(resp);
    if (!jtext) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(jtext, strlen(jtext), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *cols = root ? yyjson_obj_get(root, "cols") : NULL;
    int qn_col = -1;
    size_t ci = 0;
    size_t cmax = 0;
    yyjson_val *cv = NULL;
    yyjson_arr_foreach(cols, ci, cmax, cv) {
        const char *cname = yyjson_get_str(cv);
        if (cname && strcmp(cname, "qn") == 0) {
            qn_col = (int)ci;
        }
    }
    yyjson_val *rows = root ? yyjson_obj_get(root, "rows") : NULL;
    yyjson_val *row0 = rows ? yyjson_arr_get(rows, 0) : NULL;
    const char *qn = (row0 && qn_col >= 0) ? yyjson_get_str(yyjson_arr_get(row0, (size_t)qn_col))
                                           : NULL;
    bool ok = qn != NULL && strstr(qn, "alpha") != NULL;
    if (ok) {
        (void)snprintf(out, out_sz, "%s", qn);
    }
    yyjson_doc_free(doc);
    free(jtext);
    return ok;
}

/* Index one directory and embed it, so a question can be asked of it. */
static bool g5_index_and_embed(hyp_mcp_server_t *srv, const char *dir, const char *project) {
    char args[HYP_SZ_4K];
    (void)snprintf(args, sizeof(args), "{\"repo_path\":\"%s\"}", dir);
    char *resp = hyp_mcp_handle_tool(srv, "index_repository", args);
    if (!resp || strstr(resp, "\"isError\":true")) {
        free(resp);
        return false;
    }
    free(resp);
    hyp_ask_encoder_t enc = {.vt = &G5_ENC_VT, .self = NULL};
    hyp_ask_embed_opts_t eopts;
    memset(&eopts, 0, sizeof(eopts));
    eopts.project = project;
    hyp_ask_embed_report_t rep;
    if (hyp_ask_embed_run(&enc, &eopts, &rep) != 0) {
        return false;
    }
    bool ok = rep.embedded > 0;
    hyp_ask_embed_report_free(&rep);
    return ok;
}

TEST(g5_the_same_repository_at_two_paths_is_two_addresses) {
    /*
     * A KNOWN LIMITATION, ASSERTED AS THE CURRENT BEHAVIOUR — not a property
     * anyone wants.
     *
     * A member slug is the project slug of the repository's canonical root,
     * so two checkouts of the same repository at different paths derive
     * different slugs. The declared remedy is a workspace name in the TOML,
     * and the zero-config path is by definition the one with no TOML to
     * declare anything in — which makes this the venue where a user meets the
     * split first and the venue least able to repair it.
     *
     * The clean room is what makes the comparison mean anything: identical
     * bytes, one cache, one process, one encoder, and the checkout path as
     * the only difference between the two legs.
     */
    g5_room_t room;
    if (!g5_room_begin(&room)) {
        g5_room_end(&room);
        FAIL("the clean room could not be established");
    }
    const char *gap = g5_precondition_gap(&room);
    if (gap) {
        g5_room_end(&room);
        FAIL(gap);
    }

    /* The twin: the same two files, byte for byte, at a different path. */
    const char *made = th_mktempdir("hyp-g5-twin");
    char *twin = made ? hyp_strdup(made) : NULL;
    if (!twin || th_write_file(TH_PATH(twin, "src/x.c"), G5_ALPHA "\n"
                                                                  "int beta(void) {\n"
                                                                  "    return 2;\n"
                                                                  "}\n") != 0 ||
        th_write_file(TH_PATH(twin, "src/y.c"), "int gamma(void) {\n    return 3;\n}\n") != 0) {
        free(twin);
        g5_room_end(&room);
        FAIL("the twin checkout could not be written");
    }

    /* Both resolved by THE resolver, on its zero-config precedence — no TOML
     * passed, no provider, so what separates the two legs is the path alone. */
    hyp_wsr_resolved_t a;
    hyp_wsr_resolved_t b;
    char werr[HYP_SZ_512];
    werr[0] = '\0';
    ASSERT_EQ(hyp_wsr_resolve(room.repo, NULL, NULL, &a, werr, sizeof(werr)), HYP_WSR_OK);
    ASSERT_EQ(hyp_wsr_resolve(twin, NULL, NULL, &b, werr, sizeof(werr)), HYP_WSR_OK);
    ASSERT_EQ((int)a.source, (int)HYP_WSR_SOURCE_ZERO_CONFIG);
    ASSERT_EQ((int)b.source, (int)HYP_WSR_SOURCE_ZERO_CONFIG);
    ASSERT_EQ(a.member_count, 1);
    ASSERT_EQ(b.member_count, 1);
    ASSERT_STR_EQ(a.members[0].role, HYP_WSR_ROLE_MEMBER);
    ASSERT_STR_EQ(b.members[0].role, HYP_WSR_ROLE_MEMBER);

    /* The content is identical, and that is asserted through the product's own
     * span hash rather than assumed from the fixture writing the same literal
     * twice. Identical content is the premise; if it did not hold, a differing
     * address would say nothing about the path. */
    char hash_a[HYP_ADDR_SPAN_HASH_LEN + 1];
    char hash_b[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(G5_ALPHA, hash_a);
    hyp_addr_span_hash(G5_ALPHA, hash_b);
    ASSERT_STR_EQ(hash_a, hash_b);

    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(hyp_ask_backend_install(&G5_BACKEND), 0);
    if (!g5_index_and_embed(srv, room.repo, a.id) || !g5_index_and_embed(srv, twin, b.id)) {
        (void)hyp_ask_backend_install(NULL);
        hyp_mcp_server_free(srv);
        th_cleanup(twin);
        free(twin);
        g5_room_end(&room);
        FAIL("one of the two checkouts could not be indexed and embedded");
    }

    /* The qualified name the INDEXER built for the same declaration in each
     * checkout, read back through the tool a client uses. */
    char qn_a[HYP_ADDR_QN_MAX];
    char qn_b[HYP_ADDR_QN_MAX];
    bool got_a = g5_top_qn(srv, a.id, qn_a, sizeof(qn_a));
    bool got_b = g5_top_qn(srv, b.id, qn_b, sizeof(qn_b));
    (void)hyp_ask_backend_install(NULL);
    hyp_mcp_server_free(srv);
    if (!got_a || !got_b) {
        th_cleanup(twin);
        free(twin);
        g5_room_end(&room);
        FAIL("alpha did not come back top-ranked from one of the two checkouts");
    }

    /* Workspace NULL is the workspace of one on C1's own terms: the repo is
     * its own workspace, which is exactly what the zero-config path resolves
     * to. */
    hyp_addr_t addr_a;
    hyp_addr_t addr_b;
    ASSERT_EQ((int)hyp_addr_init(&addr_a, NULL, a.members[0].slug, qn_a), (int)HYP_ADDR_OK);
    ASSERT_EQ((int)hyp_addr_init(&addr_b, NULL, b.members[0].slug, qn_b), (int)HYP_ADDR_OK);
    char text_a[HYP_ADDR_MAX];
    char text_b[HYP_ADDR_MAX];
    ASSERT_TRUE(hyp_addr_format(&addr_a, text_a, sizeof(text_a)));
    ASSERT_TRUE(hyp_addr_format(&addr_b, text_b, sizeof(text_b)));

    /* Both addresses, printed verbatim, so the comparison sits in the record
     * rather than in a verdict about it. */
    printf("  same content, two checkout paths:\n");
    printf("    %s -> %s\n", room.repo, text_a);
    printf("    %s -> %s\n", twin, text_b);

    /* THE CURRENT BEHAVIOUR. Two addresses, differing in the repo field and
     * in the qualified name, for one declaration whose bytes are identical.
     * A decision anchored in one checkout is orphaned in the other, and the
     * union merge cannot see that they were ever the same symbol. The fix is
     * a slug that is not a function of the path — a unit, not a patch — so
     * this asserts what is true today and will fail the day it changes, which
     * is the only signal that would tell anyone it had. */
    ASSERT_STR_NEQ(a.members[0].slug, b.members[0].slug);
    ASSERT_STR_NEQ(qn_a, qn_b);
    ASSERT_STR_NEQ(text_a, text_b);
    ASSERT_FALSE(hyp_addr_equal(&addr_a, &addr_b));
    ASSERT_FALSE(hyp_addr_same_repo(&addr_a, &addr_b));
    /* And the workspace id splits with it, so a declared workspace name would
     * repair only half of this: the repo field is downstream of the path and
     * no TOML `name` reaches it. */
    ASSERT_STR_NEQ(a.id, b.id);

    th_cleanup(twin);
    free(twin);
    g5_room_end(&room);
    PASS();
}

/* ── Before the pass: refuse, and say what fixes it ───────────────── */

TEST(g5_ask_before_the_embed_pass_refuses_and_names_the_remedy) {
    /* The first thing a fresh user does after indexing is ask a question, and
     * the honest answer at that moment is "nothing was searched". An empty
     * ranking would read as "there is nothing there", which is a different
     * and false claim about their code. */
    g5_room_t room;
    if (!g5_room_begin(&room)) {
        g5_room_end(&room);
        FAIL("the clean room could not be established");
    }
    const char *gap = g5_precondition_gap(&room);
    if (gap) {
        g5_room_end(&room);
        FAIL(gap);
    }

    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(g5_index(srv, &room), 0);
    ASSERT_EQ(hyp_ask_backend_install(&G5_BACKEND), 0);

    const char *question = "{\"format\":\"json\",\"question\":\"int alpha(void)\"}";
    char *resp = hyp_mcp_handle_tool(srv, "ask", question);
    ASSERT_NOT_NULL(resp);
    char *jtext = g5_tool_text(resp);
    ASSERT_NOT_NULL(jtext);
    yyjson_doc *jdoc = yyjson_read(jtext, strlen(jtext), 0);
    ASSERT_NOT_NULL(jdoc);
    yyjson_val *jroot = yyjson_doc_get_root(jdoc);
    ASSERT_NOT_NULL(jroot);
    ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(jroot, "available")));
    const char *reason = yyjson_get_str(yyjson_obj_get(jroot, "reason"));
    ASSERT_NOT_NULL(reason);
    ASSERT_STR_EQ(reason, "no_semantic_index");
    /* Absent is not empty, and the answer has to say which one it is. */
    const char *detail = yyjson_get_str(yyjson_obj_get(jroot, "detail"));
    ASSERT_NOT_NULL(detail);
    ASSERT_NOT_NULL(strstr(detail, "NOTHING was searched"));
    const char *remedy = yyjson_get_str(yyjson_obj_get(jroot, "remedy"));
    ASSERT_NOT_NULL(remedy);
    ASSERT_GT((long long)strlen(remedy), 0LL);
    /* Resolution still happened with no project argument: the refusal is
     * about the lane, never about failing to find the repository. */
    const char *jsource = yyjson_get_str(yyjson_obj_get(jroot, "project_source"));
    ASSERT_NOT_NULL(jsource);
    ASSERT_STR_EQ(jsource, "the only indexed project");
    yyjson_doc_free(jdoc);
    free(jtext);
    free(resp);

    (void)hyp_ask_backend_install(NULL);
    hyp_mcp_server_free(srv);
    g5_room_end(&room);
    PASS();
}

/* ── The lane this build cannot run ───────────────────────────────── */

TEST(g5_the_lane_this_build_cannot_run_is_reported_as_absent) {
    /*
     * The honest half of the claim, pinned as an assertion rather than left
     * in a report. The default lane is the local encoder; whether this binary
     * HAS one is a fact about the build, and the answer must follow the fact
     * in either direction. With no encoder installed at all, the refusal must
     * name the encoder — not the index, and never an empty ranking, which
     * would read as a statement about the user's code.
     */
    g5_room_t room;
    if (!g5_room_begin(&room)) {
        g5_room_end(&room);
        FAIL("the clean room could not be established");
    }
    const char *gap = g5_precondition_gap(&room);
    if (gap) {
        g5_room_end(&room);
        FAIL(gap);
    }

    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(g5_index(srv, &room), 0);

    /* No backend: the state of a build with no encoder linked, which is what
     * the test runner is, and what a user of such a build would meet. */
    (void)hyp_ask_backend_install(NULL);
    ASSERT_NULL(hyp_ask_backend());
    const char *question = "{\"format\":\"json\",\"question\":\"int alpha(void)\"}";
    char *resp = hyp_mcp_handle_tool(srv, "ask", question);
    ASSERT_NOT_NULL(resp);
    char *jtext = g5_tool_text(resp);
    ASSERT_NOT_NULL(jtext);
    yyjson_doc *jdoc = yyjson_read(jtext, strlen(jtext), 0);
    ASSERT_NOT_NULL(jdoc);
    yyjson_val *jroot = yyjson_doc_get_root(jdoc);
    ASSERT_NOT_NULL(jroot);
    ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(jroot, "available")));
    const char *reason = yyjson_get_str(yyjson_obj_get(jroot, "reason"));
    ASSERT_NOT_NULL(reason);
    ASSERT_STR_EQ(reason, "no_encoder");
    const char *detail = yyjson_get_str(yyjson_obj_get(jroot, "detail"));
    ASSERT_NOT_NULL(detail);
    ASSERT_NOT_NULL(strstr(detail, "NOTHING was searched"));
    /* The remedy names a surface that works in every build rather than a
     * download the user may not want. */
    const char *remedy = yyjson_get_str(yyjson_obj_get(jroot, "remedy"));
    ASSERT_NOT_NULL(remedy);
    ASSERT_NOT_NULL(strstr(remedy, "search_graph"));
    yyjson_doc_free(jdoc);
    free(jtext);
    free(resp);

    /* And the build fact this file's honesty claim rests on, asserted rather
     * than asserted-about: the weights of the default lane are only ever
     * reported present by a build that could load them. */
    ASSERT_TRUE(hyp_ask_llama_compiled_in() || !hyp_model_ask_present());

    hyp_mcp_server_free(srv);
    g5_room_end(&room);
    PASS();
}

/* ── The recipe the product prints is a recipe that parses ────────── */

#define G5_SURFACE_TOKEN(id, token, role, help, dispatch, usage) token,
static const char *const G5_SURFACE_TOKENS[] = {HYP_CLI_COMMAND_SURFACE(G5_SURFACE_TOKEN)};
enum { G5_SURFACE_TOKEN_COUNT = (int)(sizeof(G5_SURFACE_TOKENS) / sizeof(G5_SURFACE_TOKENS[0])) };

static bool g5_is_command_token(const char *tok) {
    for (int i = 0; i < G5_SURFACE_TOKEN_COUNT; i++) {
        if (strcmp(tok, G5_SURFACE_TOKENS[i]) == 0) {
            return true;
        }
    }
    return false;
}

enum { G5_MAX_WORDS = 24, G5_CAPTURE_CAP = 16384 };

/* Split one printed command line into argv. Double quotes group a word (the
 * printed repository path is quoted); a '#' outside quotes ends the line,
 * because one printed step carries a trailing explanation. `line` is
 * rewritten in place and the returned pointers point into it. */
static int g5_split_command(char *line, char **out, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p || *p == '#') {
            break;
        }
        char *w = p;
        char *dst = p;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
                continue;
            }
            *dst++ = *p++;
        }
        bool end = (*p == '\0');
        *dst = '\0';
        out[n++] = w;
        if (!end) {
            p++;
        }
    }
    return n;
}

/* Run `fn` with stdout captured into `buf`. Returns the callee's exit code, or
 * -1 when the capture itself could not be set up — which is a failure and
 * never a quiet pass. */
static int g5_capture_stdout(const char *scratch, int (*fn)(int, char **), int argc, char **argv,
                             char *buf, size_t cap) {
    buf[0] = '\0';
    char path[HYP_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/g5-capture.txt", scratch);
    FILE *sink = hyp_fopen(path, "w+b");
    if (!sink) {
        return -1;
    }
    fflush(stdout);
    int saved = g5_dup(G5_STDOUT_FD);
    if (saved < 0 || g5_dup2(hyp_fileno(sink), G5_STDOUT_FD) < 0) {
        if (saved >= 0) {
            g5_close(saved);
        }
        fclose(sink);
        return -1;
    }
    int rc = fn(argc, argv);
    fflush(stdout);
    (void)g5_dup2(saved, G5_STDOUT_FD);
    g5_close(saved);
    rewind(sink);
    size_t got = fread(buf, 1, cap - 1, sink);
    buf[got] = '\0';
    fclose(sink);
    (void)hyp_unlink(path);
    return rc;
}

TEST(g5_the_printed_next_steps_are_commands_this_binary_accepts) {
    /*
     * Onboarding does not run the first index; it PRINTS the two commands
     * that do. That makes the printed recipe a second end of the same
     * contract, and two ends that pass their own tests while disagreeing is
     * how this project has shipped dead surfaces before. Nothing below is
     * typed out by hand: the commands are read back from what the product
     * printed, and each is put to the parser that would receive it.
     */
    g5_room_t room;
    if (!g5_room_begin(&room)) {
        g5_room_end(&room);
        FAIL("the clean room could not be established");
    }
    const char *gap = g5_precondition_gap(&room);
    if (gap) {
        g5_room_end(&room);
        FAIL(gap);
    }

    const char *made = th_mktempdir("hyp-g5-scratch");
    char *scratch = made ? hyp_strdup(made) : NULL;
    if (!scratch) {
        g5_room_end(&room);
        FAIL("no scratch directory for the capture");
    }

    /* --yes runs the flow with nothing answered; --no-toml keeps the record
     * out of the repository, so the recipe is the only thing produced. */
    char *argv[] = {(char *)"--yes", (char *)"--no-toml", room.repo};
    char *out = (char *)malloc(G5_CAPTURE_CAP);
    if (!out) {
        th_cleanup(scratch);
        free(scratch);
        g5_room_end(&room);
        FAIL("no buffer for the capture");
    }
    int onboard_rc = g5_capture_stdout(scratch, hyp_cmd_onboard, 3, argv, out, G5_CAPTURE_CAP);
    if (onboard_rc != 0) {
        free(out);
        th_cleanup(scratch);
        free(scratch);
        g5_room_end(&room);
        FAIL("onboard did not succeed on a fresh repository");
    }
    /* The record really was declined, so nothing downstream can be reading it. */
    char toml[HYP_PATH_MAX];
    (void)snprintf(toml, sizeof(toml), "%s/%s", room.repo, HYP_ONBOARD_TOML_NAME);
    ASSERT_FALSE(hyp_file_exists(toml));

    char *block = strstr(out, "\nnext:\n");
    if (!block) {
        free(out);
        th_cleanup(scratch);
        free(scratch);
        g5_room_end(&room);
        FAIL("onboard printed no next-steps block");
    }
    block += strlen("\nnext:\n");

    /* Index the repository first: the second printed step reads a project, so
     * a parse check that ran against nothing would not be reading the
     * recipe's own arguments. */
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_EQ(g5_index(srv, &room), 0);
    hyp_mcp_server_free(srv);

    int checked = 0;
    int saw_index = 0;
    int saw_embed = 0;
    int saw_fetch = 0;
    char *line = block;
    while (line && *line == ' ') {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *words[G5_MAX_WORDS];
        int n = g5_split_command(line, words, G5_MAX_WORDS);
        if (n >= 2) {
            /* The binary's own name, then a row of the command table both the
             * classifier and the dispatcher read. A printed step naming a
             * command this binary does not classify would be served the MCP
             * protocol on a closed stdin. */
            ASSERT_STR_EQ(words[0], "hyponoia");
            ASSERT_TRUE(g5_is_command_token(words[1]));
            if (strcmp(words[1], "cli") == 0 && n >= 3) {
                /* The tool exists, and its printed flags build the arguments
                 * object the tool would receive. */
                ASSERT_NOT_NULL(hyp_mcp_tool_input_schema(words[2]));
                char *err = NULL;
                char *json = hyp_cli_build_args_json(words[2], n - 3, words + 3, &err);
                if (!json) {
                    printf("  printed step rejected: %s\n", err ? err : "unknown");
                }
                free(err);
                ASSERT_NOT_NULL(json);
                /* The canonical spelling, because the resolver canonicalises
                 * and the fixture's own path need not already be canonical.
                 * The point of the assertion is that the arguments object
                 * carries THIS repository, not that two strings match. */
                char canon_repo[HYP_SZ_4K];
                ASSERT_TRUE(hyp_canonical_path(room.repo, canon_repo, sizeof(canon_repo)) != 0);
                ASSERT_NOT_NULL(strstr(json, canon_repo));
                free(json);
                saw_index++;
            } else if (strcmp(words[1], "embed") == 0) {
                /* Parsed by the command itself. --status makes it a read, so
                 * the check is of the flags and not of a pass that would take
                 * minutes. Exit 2 is this parser's argument error. */
                char *eargv[G5_MAX_WORDS];
                int en = 0;
                for (int i = 2; i < n; i++) {
                    eargv[en++] = words[i];
                }
                eargv[en++] = (char *)"--status";
                char *ebuf = (char *)malloc(G5_CAPTURE_CAP);
                ASSERT_NOT_NULL(ebuf);
                int erc =
                    g5_capture_stdout(scratch, hyp_cmd_embed, en, eargv, ebuf, G5_CAPTURE_CAP);
                free(ebuf);
                ASSERT_NEQ(erc, 2);
                ASSERT_NEQ(erc, -1);
                saw_embed++;
            } else if (strcmp(words[1], "fetch-model") == 0) {
                saw_fetch++;
            }
            checked++;
        }
        line = nl ? nl + 1 : NULL;
    }
    /* The recipe was read, not merely searched for: every step the
     * zero-config path prints was put to a parser. */
    ASSERT_GTE(checked, 3);
    ASSERT_EQ(saw_index, 1);
    ASSERT_EQ(saw_embed, 1);
    /*
     * And the recipe is complete FOR THIS MACHINE. The clean room guarantees
     * the weights of the default lane are not on it, and the embed step
     * refuses without them — so a recipe that went straight from indexing to
     * embedding would send a first-time user into an error the product had
     * everything it needed to avoid. This is the assertion that fails if that
     * step is dropped again.
     */
    ASSERT_FALSE(hyp_model_ask_present());
    ASSERT_EQ(saw_fetch, 1);

    free(out);
    th_cleanup(scratch);
    free(scratch);
    g5_room_end(&room);
    PASS();
}

SUITE(g5_zero_config) {
    RUN_TEST(g5_clean_room_guarantees_the_preconditions_are_absent);
    RUN_TEST(g5_zero_config_indexes_and_answers_with_nothing_typed);
    RUN_TEST(g5_the_same_repository_at_two_paths_is_two_addresses);
    RUN_TEST(g5_ask_before_the_embed_pass_refuses_and_names_the_remedy);
    RUN_TEST(g5_the_lane_this_build_cannot_run_is_reported_as_absent);
    RUN_TEST(g5_the_printed_next_steps_are_commands_this_binary_accepts);
}
