/*
 * test_ask.c — the `ask` tool: schema, availability, language, ranking, shape.
 *
 * Run by `make -f Makefile.hyp test` and `test-par` (suite name `ask`).
 * NOT by `test-foundation`, which links only the 13 foundation suites.
 *
 * The tests that matter most here are the ones about what `ask` says when it
 * CANNOT answer. A ranking bug costs one bad result; reporting "no index" as
 * "no matches" costs a wrong belief about the caller's codebase, and the
 * caller has no way to tell the two apart from the outside. So the unavailable
 * paths are pinned by content, not just by a flag.
 *
 * The encoder is a deterministic in-process double. That is the whole point of
 * the backend seam: every line of ranking, shaping and disclosure is exercised
 * with no model on disk, no GPU and no network.
 */
#include "../src/foundation/compat.h" /* hyp_strdup */
#include "../src/foundation/constants.h"
#include "test_framework.h"
#include "test_helpers.h"

#include <ask/ask_vectors.h>
#include <ask/ask_view.h>
#include <cli/cli.h>              /* hyp_config_*, HYP_CONFIG_ASK_ESC_* */
#include <foundation/compat_fs.h> /* hyp_file_exists */
#include <mcp/mcp.h>
#include <semantic/ask_embed.h>
#include <semantic/ask_lang.h>
#include <store/ask_index.h>
#include <store/store.h>
#include <yyjson/yyjson.h>

#include <math.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── A deterministic encoder double ──────────────────────────────── */

/* Every vector is a unit vector on one axis, chosen by a hash of the text.
 * Two texts that hash to the same axis score 1.0 against each other and 0.0
 * against everything else, which makes the expected ranking exact rather than
 * approximate — a test that asserted "roughly first" would not notice a
 * comparator inverted on ties. */
static void fake_axis_vector(const char *text, float *out) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h = h * 33u + *p;
    }
    memset(out, 0, (size_t)HYP_ASK_DIM * sizeof(float));
    out[h % (unsigned long)HYP_ASK_DIM] = 1.0f;
}

static HYPLanguage g_fake_last_lang;
static int g_fake_query_calls;
static bool g_fake_fail;

static int fake_encode_query(HYPLanguage lang, const char *text, float *out, char *err,
                             size_t errlen) {
    g_fake_last_lang = lang;
    g_fake_query_calls++;
    if (g_fake_fail) {
        snprintf(err, errlen, "the double was told to fail");
        return -1;
    }
    fake_axis_vector(text, out);
    return 0;
}

static int fake_encode_documents(const char *const *texts, int n, float *out, char *err,
                                 size_t errlen) {
    (void)err;
    (void)errlen;
    for (int i = 0; i < n; i++) {
        fake_axis_vector(texts[i], out + (size_t)i * HYP_ASK_DIM);
    }
    return 0;
}

static const hyp_ask_backend_t g_fake_backend = {
    .model_id = "test-double/axis-1024",
    .dim = HYP_ASK_DIM,
    .window_tokens = 32768,
    .encode_query = fake_encode_query,
    .encode_documents = fake_encode_documents,
    .truncates = NULL,
};

static void fake_reset(void) {
    g_fake_last_lang = HYP_LANG_COUNT;
    g_fake_query_calls = 0;
    g_fake_fail = false;
}

/* ── Fixture: a project with nodes and a hand-built vector index ─── */

/* The write half of the vector store is T4's. These helpers create exactly
 * the shape ask_index.h documents, so when T4 lands, the read path under test
 * is already pinned against the contract rather than against T4's code. */
static void ask_fixture_create_tables(sqlite3 *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db,
                          "CREATE TABLE IF NOT EXISTS ask_vectors ("
                          "  project TEXT NOT NULL, node_id INTEGER NOT NULL, vec BLOB NOT NULL,"
                          "  content_hash TEXT NOT NULL DEFAULT '', truncated INTEGER NOT NULL"
                          "  DEFAULT 0, PRIMARY KEY (project, node_id));"
                          "CREATE TABLE IF NOT EXISTS ask_index_meta ("
                          "  project TEXT PRIMARY KEY, model_id TEXT NOT NULL, dim INTEGER NOT"
                          "  NULL, window_tokens INTEGER NOT NULL DEFAULT 0, extraction TEXT NOT"
                          "  NULL DEFAULT '', watermark TEXT NOT NULL DEFAULT '', trunc_state"
                          "  TEXT NOT NULL DEFAULT 'unknown', trunc_count INTEGER NOT NULL"
                          "  DEFAULT 0, n_rows INTEGER NOT NULL DEFAULT 0, built_at TEXT NOT NULL"
                          "  DEFAULT '');",
                          NULL, NULL, &err);
    (void)rc;
    sqlite3_free(err);
}

static void ask_fixture_put_meta(sqlite3 *db, const char *project, const char *model_id, int dim,
                                 const char *trunc_state, int trunc_count, int n_rows) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT OR REPLACE INTO ask_index_meta"
                       "(project,model_id,dim,trunc_state,trunc_count,n_rows)"
                       " VALUES(?1,?2,?3,?4,?5,?6)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, model_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, dim);
    sqlite3_bind_text(st, 4, trunc_state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, trunc_count);
    sqlite3_bind_int(st, 6, n_rows);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void ask_fixture_put_vector(sqlite3 *db, const char *project, int64_t node_id,
                                   const char *doc_text, bool truncated) {
    float vec[HYP_ASK_DIM];
    fake_axis_vector(doc_text, vec);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT OR REPLACE INTO ask_vectors(project,node_id,vec,truncated)"
                       " VALUES(?1,?2,?3,?4)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, node_id);
    sqlite3_bind_blob(st, 3, vec, (int)sizeof(vec), SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, truncated ? 1 : 0);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Pull the tool payload out of a JSON-RPC envelope: result.content[0].text. */
static char *ask_inner_text(const char *response) {
    yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *result = root ? yyjson_obj_get(root, "result") : NULL;
    yyjson_val *content = result ? yyjson_obj_get(result, "content") : NULL;
    yyjson_val *first = content ? yyjson_arr_get(content, 0) : NULL;
    yyjson_val *text = first ? yyjson_obj_get(first, "text") : NULL;
    char *out = (text && yyjson_is_str(text)) ? hyp_strdup(yyjson_get_str(text)) : NULL;
    yyjson_doc_free(doc);
    return out;
}

static bool ask_is_error(const char *response) {
    yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *result = root ? yyjson_obj_get(root, "result") : NULL;
    yyjson_val *is_err = result ? yyjson_obj_get(result, "isError") : NULL;
    bool v = is_err && yyjson_is_bool(is_err) && yyjson_get_bool(is_err);
    yyjson_doc_free(doc);
    return v;
}

static char *ask_call(hyp_mcp_server_t *srv, const char *arguments_json) {
    char req[HYP_SZ_2K];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{"
             "\"name\":\"ask\",\"arguments\":%s}}",
             arguments_json);
    return hyp_mcp_server_handle(srv, req);
}

/* ── Backend registry ────────────────────────────────────────────── */

TEST(ask_backend_none_by_default) {
    hyp_ask_backend_install(NULL);
    ASSERT_NULL(hyp_ask_backend());
    PASS();
}

TEST(ask_backend_install_and_uninstall) {
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    ASSERT_NOT_NULL(hyp_ask_backend());
    ASSERT_STR_EQ(hyp_ask_backend()->model_id, "test-double/axis-1024");
    hyp_ask_backend_install(NULL);
    ASSERT_NULL(hyp_ask_backend());
    PASS();
}

/* Each rejection is a mismatch that would otherwise produce plausible
 * numbers rather than an error. A wrong dim in particular reads 768 floats
 * out of a 1024-float question and calls the result a cosine. */
TEST(ask_backend_rejects_malformed) {
    hyp_ask_backend_t bad = g_fake_backend;
    bad.dim = 768;
    ASSERT_EQ(hyp_ask_backend_install(&bad), HYP_NOT_FOUND);
    ASSERT_NULL(hyp_ask_backend());

    bad = g_fake_backend;
    bad.model_id = NULL;
    ASSERT_EQ(hyp_ask_backend_install(&bad), HYP_NOT_FOUND);

    bad = g_fake_backend;
    bad.encode_query = NULL;
    ASSERT_EQ(hyp_ask_backend_install(&bad), HYP_NOT_FOUND);
    ASSERT_NULL(hyp_ask_backend());
    PASS();
}

/* ── Language resolution ─────────────────────────────────────────── */

/* The measured prefix is byte-identical only when {language} renders exactly
 * `C++`. Every other entry in the map is unmeasured by construction; this one
 * is not, so it is pinned. */
TEST(ask_lang_cpp_renders_the_measured_display_name) {
    HYPLanguage l = hyp_ask_resolve_language("cpp");
    ASSERT_TRUE(l != HYP_LANG_COUNT);
    ASSERT_STR_EQ(hyp_ask_language_display(l), "C++");
    PASS();
}

TEST(ask_lang_accepts_extension_and_display_name) {
    ASSERT_TRUE(hyp_ask_resolve_language("rs") == hyp_ask_resolve_language("Rust"));
    ASSERT_TRUE(hyp_ask_resolve_language("py") == hyp_ask_resolve_language("Python"));
    ASSERT_TRUE(hyp_ask_resolve_language(".cpp") == hyp_ask_resolve_language("cpp"));
    /* Case-insensitive on the display name, so a caller can echo the
     * disclosed language back without matching its capitalisation. */
    ASSERT_TRUE(hyp_ask_resolve_language("c++") == hyp_ask_resolve_language("C++"));
    PASS();
}

TEST(ask_lang_refuses_rather_than_defaults) {
    ASSERT_TRUE(hyp_ask_resolve_language(NULL) == HYP_LANG_COUNT);
    ASSERT_TRUE(hyp_ask_resolve_language("") == HYP_LANG_COUNT);
    ASSERT_TRUE(hyp_ask_resolve_language("klingon") == HYP_LANG_COUNT);
    ASSERT_NULL(hyp_ask_language_display(HYP_LANG_COUNT));
    ASSERT_NULL(hyp_ask_language_display((HYPLanguage)-1));
    PASS();
}

TEST(ask_lang_dominant_counts_files) {
    static const char *const paths[] = {"src/a.c",  "src/b.c",       "src/c.c",
                                        "web/x.ts", "docs/notes.md", "src/d.h"};
    HYPLanguage dom = hyp_ask_dominant_language(paths, 6);
    ASSERT_TRUE(dom != HYP_LANG_COUNT);
    /* .c and .h both map to the C grammar, so C wins 4-1 over TypeScript.
     * The point is the count, not the identity of the winner. */
    ASSERT_TRUE(dom == hyp_ask_resolve_language("c"));
    PASS();
}

TEST(ask_lang_dominant_refuses_on_nothing_recognisable) {
    static const char *const paths[] = {"a.qqq", "b.zzz"};
    ASSERT_TRUE(hyp_ask_dominant_language(paths, 2) == HYP_LANG_COUNT);
    ASSERT_TRUE(hyp_ask_dominant_language(NULL, 0) == HYP_LANG_COUNT);
    PASS();
}

/* ── Index status: three unavailable states, three remedies ──────── */

TEST(ask_status_no_backend_wins_over_everything) {
    hyp_ask_backend_install(NULL);
    hyp_store_t *st = hyp_store_open_memory();
    ASSERT_NOT_NULL(st);
    hyp_ask_status_t status;
    hyp_ask_index_status(st, "p", &status);
    ASSERT_TRUE(status.avail == HYP_ASK_NO_BACKEND);
    hyp_store_close(st);
    PASS();
}

TEST(ask_status_no_index_when_tables_absent) {
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_store_t *st = hyp_store_open_memory();
    ASSERT_NOT_NULL(st);
    hyp_ask_status_t status;
    hyp_ask_index_status(st, "p", &status);
    ASSERT_TRUE(status.avail == HYP_ASK_NO_INDEX);
    /* And the read path must not have CREATEd them on the way past: an
     * absent table is the answer, not a condition to repair. */
    sqlite3_stmt *q = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(hyp_store_get_db(st),
                                 "SELECT count(*) FROM sqlite_master WHERE name='ask_vectors'", -1,
                                 &q, NULL),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(q), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(q, 0), 0);
    sqlite3_finalize(q);
    hyp_store_close(st);
    hyp_ask_backend_install(NULL);
    PASS();
}

TEST(ask_status_model_mismatch_is_refused_not_mixed) {
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_store_t *st = hyp_store_open_memory();
    ASSERT_NOT_NULL(st);
    sqlite3 *db = hyp_store_get_db(st);
    ask_fixture_create_tables(db);
    ask_fixture_put_meta(db, "p", "some-other-model", HYP_ASK_DIM, "none", 0, 3);
    ask_fixture_put_vector(db, "p", 1, "doc", false);

    hyp_ask_status_t status;
    hyp_ask_index_status(st, "p", &status);
    ASSERT_TRUE(status.avail == HYP_ASK_MODEL_MISMATCH);
    ASSERT_STR_EQ(status.model_id, "some-other-model");
    ASSERT_STR_EQ(status.backend_id, "test-double/axis-1024");

    /* Same model, wrong dimension: also a refusal. */
    ask_fixture_put_meta(db, "p", "test-double/axis-1024", 768, "none", 0, 3);
    hyp_ask_index_status(st, "p", &status);
    ASSERT_TRUE(status.avail == HYP_ASK_MODEL_MISMATCH);

    hyp_store_close(st);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* `unknown` and `none` are different claims and must not collapse. An
 * unrecognised word from a future writer degrades to `unknown`, never to
 * `none` — reading silence as "nothing was cut" is how an unattested set
 * becomes a confident disclosure. */
TEST(ask_status_truncation_has_three_states) {
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_store_t *st = hyp_store_open_memory();
    sqlite3 *db = hyp_store_get_db(st);
    ask_fixture_create_tables(db);
    ask_fixture_put_vector(db, "p", 1, "doc", false);
    hyp_ask_status_t status;

    ask_fixture_put_meta(db, "p", "test-double/axis-1024", HYP_ASK_DIM, "none", 0, 1);
    hyp_ask_index_status(st, "p", &status);
    ASSERT_TRUE(status.trunc == HYP_ASK_TRUNC_NONE);

    ask_fixture_put_meta(db, "p", "test-double/axis-1024", HYP_ASK_DIM, "unknown", 0, 1);
    hyp_ask_index_status(st, "p", &status);
    ASSERT_TRUE(status.trunc == HYP_ASK_TRUNC_UNKNOWN);

    ask_fixture_put_meta(db, "p", "test-double/axis-1024", HYP_ASK_DIM, "some", 4, 1);
    hyp_ask_index_status(st, "p", &status);
    ASSERT_TRUE(status.trunc == HYP_ASK_TRUNC_SOME);
    ASSERT_EQ(status.trunc_count, 4);

    ask_fixture_put_meta(db, "p", "test-double/axis-1024", HYP_ASK_DIM, "partially-maybe", 0, 1);
    hyp_ask_index_status(st, "p", &status);
    ASSERT_TRUE(status.trunc == HYP_ASK_TRUNC_UNKNOWN);

    hyp_store_close(st);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* ── The tool, end to end ────────────────────────────────────────── */

static hyp_mcp_server_t *ask_srv_with_nodes(const char *project) {
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    if (!srv) {
        return NULL;
    }
    hyp_store_t *st = hyp_mcp_server_store(srv);
    hyp_mcp_server_set_project(srv, project);
    hyp_store_upsert_project(st, project, "/tmp/askproj");

    hyp_node_t a = {.project = project,
                    .label = "Function",
                    .name = "orderSections",
                    .qualified_name = "askproj.writer.orderSections",
                    .file_path = "src/writer.c",
                    .start_line = 120,
                    .end_line = 168};
    hyp_node_t b = {.project = project,
                    .label = "Method",
                    .name = "emit",
                    .qualified_name = "askproj.writer.emit",
                    .file_path = "src/writer.c",
                    .start_line = 200,
                    .end_line = 240};
    hyp_node_t c = {.project = project,
                    .label = "Class",
                    .name = "Reader",
                    .qualified_name = "askproj.reader.Reader",
                    .file_path = "src/reader.c",
                    .start_line = 10,
                    .end_line = 90};
    int64_t ida = hyp_store_upsert_node(st, &a);
    int64_t idb = hyp_store_upsert_node(st, &b);
    int64_t idc = hyp_store_upsert_node(st, &c);

    sqlite3 *db = hyp_store_get_db(st);
    ask_fixture_create_tables(db);
    /* The question below hashes to the same axis as "GOLD", so node a scores
     * 1.0 and the other two score 0.0 — an exact expected order. */
    ask_fixture_put_vector(db, project, ida, "GOLD", false);
    ask_fixture_put_vector(db, project, idb, "other-1", false);
    ask_fixture_put_vector(db, project, idc, "other-2", false);
    ask_fixture_put_meta(db, project, "test-double/axis-1024", HYP_ASK_DIM, "none", 0, 3);
    return srv;
}

/* THE test this file exists for. With no encoder, `ask` must not answer with
 * an empty result set — that reads as "your codebase has nothing like that",
 * which is a claim about the caller's code the tool has no basis to make. */
TEST(ask_unavailable_says_so_and_emits_no_rows) {
    fake_reset();
    hyp_ask_backend_install(NULL);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    char *resp =
        ask_call(srv, "{\"project\":\"askproj\",\"question\":\"how are sections ordered\"}");
    ASSERT_NOT_NULL(resp);
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);

    ASSERT_NOT_NULL(strstr(inner, "available: false"));
    ASSERT_NOT_NULL(strstr(inner, "reason: no_encoder"));
    ASSERT_NOT_NULL(strstr(inner, "searched: false"));
    /* The remedy must name something the caller can actually run. */
    ASSERT_NOT_NULL(strstr(inner, "search_graph"));
    /* And there must be NO results table at all — not an empty one. */
    ASSERT_NULL(strstr(inner, "results:"));
    /* A first-class outcome, not an error path. */
    ASSERT_FALSE(ask_is_error(resp));

    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    PASS();
}

TEST(ask_unavailable_no_index_names_the_project_and_the_pass) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    hyp_store_t *st = hyp_mcp_server_store(srv);
    hyp_mcp_server_set_project(srv, "bare");
    hyp_store_upsert_project(st, "bare", "/tmp/bare");
    hyp_node_t n = {.project = "bare",
                    .label = "Function",
                    .name = "f",
                    .qualified_name = "bare.f",
                    .file_path = "a.c",
                    .start_line = 1,
                    .end_line = 2};
    hyp_store_upsert_node(st, &n);

    char *resp = ask_call(srv, "{\"project\":\"bare\",\"question\":\"anything at all\"}");
    ASSERT_NOT_NULL(resp);
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "reason: no_semantic_index"));
    ASSERT_NOT_NULL(strstr(inner, "bare"));
    ASSERT_NOT_NULL(strstr(inner, "NOTHING was searched"));
    ASSERT_NULL(strstr(inner, "results:"));
    /* The encoder must not have been called: there was nothing to search. */
    ASSERT_EQ(g_fake_query_calls, 0);
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    PASS();
}

TEST(ask_returns_ranked_spans_with_line_numbers) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}");
    ASSERT_NOT_NULL(resp);
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);

    ASSERT_NOT_NULL(strstr(inner, "available: true"));
    ASSERT_NOT_NULL(strstr(inner, "population: 3"));
    /* Same columns search_graph's BM25 path returns, plus the score. */
    ASSERT_NOT_NULL(strstr(inner, "(cols: qn label file lines score)"));
    /* A SPAN, not just a file: this is what lets an agent aim an edit. */
    ASSERT_NOT_NULL(strstr(inner, "askproj.writer.orderSections"));
    ASSERT_NOT_NULL(strstr(inner, "src/writer.c"));
    ASSERT_NOT_NULL(strstr(inner, "120-168"));
    /* Gold ranks first: it is the only row on the question's axis. */
    const char *gold = strstr(inner, "askproj.writer.orderSections");
    const char *other = strstr(inner, "askproj.reader.Reader");
    ASSERT_NOT_NULL(gold);
    ASSERT_NOT_NULL(other);
    ASSERT_TRUE(gold < other);
    /* Truncation is disclosed on EVERY answer, not only when a cut row ranks. */
    ASSERT_NOT_NULL(strstr(inner, "truncation:"));
    /* No `cut` column when the index attests none were cut. */
    ASSERT_NULL(strstr(inner, " cut)"));
    /* And so is what was left OUT of the population — the caller can see the
     * rows that came back and not the ones that could not be there. This
     * fixture is an in-graph table with no policy recorded, which is exactly
     * what an index built before the policy existed looks like: `kept`. */
    ASSERT_NOT_NULL(strstr(inner, "whole_file_spans: \"kept"));

    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* The prefix is rendered from a language, and which language it was must
 * reach the caller — a wrong derivation has no other signal anywhere. */
TEST(ask_discloses_the_language_it_rendered) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    /* Derived: every file in the fixture is .c. */
    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}");
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "language_source: derived"));
    ASSERT_NOT_NULL(strstr(inner, "language: C"));
    ASSERT_TRUE(g_fake_last_lang == hyp_ask_resolve_language("c"));
    free(inner);
    free(resp);

    /* Explicit overrides, and reaches the encoder. */
    resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"language\":\"cpp\"}");
    inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "language_source: explicit"));
    ASSERT_NOT_NULL(strstr(inner, "C++"));
    ASSERT_TRUE(g_fake_last_lang == hyp_ask_resolve_language("cpp"));
    free(inner);
    free(resp);

    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    PASS();
}

TEST(ask_refuses_an_unknown_language_rather_than_defaulting) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    char *resp =
        ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"language\":\"klingon\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "klingon"));
    ASSERT_EQ(g_fake_query_calls, 0); /* nothing was encoded behind a guess */
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* The `cut` column appears only when there is something to say, and the
 * truncation sentence names the count either way. */
TEST(ask_adds_the_cut_column_only_when_rows_were_truncated) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);
    sqlite3 *db = hyp_store_get_db(hyp_mcp_server_store(srv));
    ask_fixture_put_meta(db, "askproj", "test-double/axis-1024", HYP_ASK_DIM, "some", 2, 3);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}");
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "(cols: qn label file lines score cut)"));
    ASSERT_NOT_NULL(strstr(inner, "2 declaration(s) exceeded"));
    free(inner);
    free(resp);

    /* And `unknown` says so in words rather than reading as `none`. */
    ask_fixture_put_meta(db, "askproj", "test-double/axis-1024", HYP_ASK_DIM, "unknown", 0, 3);
    resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}");
    inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "truncation: \"unknown"));
    free(inner);
    free(resp);

    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* format:"json" carries the SAME model, with rows as ARRAYS matching cols —
 * never per-row key envelopes, which measured 84% key overhead. */
TEST(ask_json_rows_are_arrays_matching_cols) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    char *resp =
        ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"format\":\"json\"}");
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);

    yyjson_doc *doc = yyjson_read(inner, strlen(inner), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *cols = yyjson_obj_get(root, "cols");
    yyjson_val *rows = yyjson_obj_get(root, "rows");
    ASSERT_NOT_NULL(cols);
    ASSERT_NOT_NULL(rows);
    ASSERT_TRUE(yyjson_is_arr(cols));
    ASSERT_TRUE(yyjson_is_arr(rows));
    ASSERT_EQ(yyjson_arr_size(cols), 5U);
    ASSERT_EQ(yyjson_arr_size(rows), 3U);
    yyjson_val *row0 = yyjson_arr_get(rows, 0);
    ASSERT_TRUE(yyjson_is_arr(row0)); /* array, NOT an object */
    ASSERT_EQ(yyjson_arr_size(row0), yyjson_arr_size(cols));
    ASSERT_STR_EQ(yyjson_get_str(yyjson_arr_get(row0, 0)), "askproj.writer.orderSections");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_arr_get(row0, 3)), "120-168");
    yyjson_doc_free(doc);

    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* A vector whose node was deleted is DROPPED, not returned: a row that can be
 * ranked but not cited is a citation to nothing. */
TEST(ask_drops_rows_whose_node_is_gone) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);
    sqlite3 *db = hyp_store_get_db(hyp_mcp_server_store(srv));
    /* A vector for a node id that never existed. */
    ask_fixture_put_vector(db, "askproj", 999999, "GOLD", false);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}");
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "999999"));
    ASSERT_NOT_NULL(strstr(inner, "askproj.writer.orderSections"));
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* A row of the wrong width is skipped rather than read as 1024 floats. */
TEST(ask_skips_vectors_of_the_wrong_width) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_store_t *st = hyp_store_open_memory();
    sqlite3 *db = hyp_store_get_db(st);
    ask_fixture_create_tables(db);
    sqlite3_stmt *q = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO ask_vectors(project,node_id,vec) VALUES('p',1,?1)", -1, &q,
                       NULL);
    float short_vec[8] = {0};
    sqlite3_bind_blob(q, 1, short_vec, (int)sizeof(short_vec), SQLITE_TRANSIENT);
    sqlite3_step(q);
    sqlite3_finalize(q);

    float qvec[HYP_ASK_DIM];
    fake_axis_vector("anything", qvec);
    hyp_ask_hit_t *hits = NULL;
    int n = -1;
    ASSERT_EQ(hyp_ask_index_search(st, "p", qvec, 10, &hits, &n), HYP_STORE_OK);
    ASSERT_EQ(n, 0);
    hyp_ask_free_hits(hits, n);
    hyp_store_close(st);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* ── Argument validation ─────────────────────────────────────────── */

TEST(ask_rejects_an_array_and_points_at_semantic_query) {
    fake_reset();
    hyp_ask_backend_install(NULL);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);
    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":[\"a\",\"b\"]}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "ONE natural-language string"));
    ASSERT_NOT_NULL(strstr(inner, "semantic_query"));
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    PASS();
}

TEST(ask_requires_a_question) {
    fake_reset();
    hyp_ask_backend_install(NULL);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);
    char *resp = ask_call(srv, "{\"project\":\"askproj\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(strstr(inner, "question is required"));
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ONE CALL, AND THE LINES NAMED (NEXT-STEPS.md §3.2 step 1)
 *
 *  Every assertion below reads THE CLIENT'S VIEW — the text a client would
 *  render out of result.content[0], or result.structuredContent when the tool
 *  answered in JSON. That rule is the whole reason 505ee69d exists: four tests
 *  once asserted what the server emitted and all four passed against three
 *  tools that rendered blank.
 * ══════════════════════════════════════════════════════════════════ */

/* result.structuredContent, or NULL when the key is ABSENT. Absent is a
 * first-class answer here — it is how the tool says "the payload is text, read
 * content" — so this must distinguish it from an empty object. */
static yyjson_doc *ask_structured(const char *response) {
    yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *result = root ? yyjson_obj_get(root, "result") : NULL;
    yyjson_val *sc = result ? yyjson_obj_get(result, "structuredContent") : NULL;
    yyjson_doc *out = NULL;
    if (sc) {
        char *text = yyjson_val_write(sc, 0, NULL);
        if (text) {
            out = yyjson_read(text, strlen(text), 0);
            free(text);
        }
    }
    yyjson_doc_free(doc);
    return out;
}

/* ask_call + ask_inner_text in one, freeing the envelope. Every test below
 * reads only the payload; nesting the two leaks the envelope, and LeakSanitizer
 * is part of this suite's gate. */
static char *ask_text(hyp_mcp_server_t *srv, const char *arguments_json) {
    char *resp = ask_call(srv, arguments_json);
    char *inner = ask_inner_text(resp);
    free(resp);
    return inner;
}

/* A vector at an EXACT cosine to the question's axis, so a fixture can pin a
 * ranking ORDER rather than only a winner. The one-hot double scores 1.0 or
 * 0.0 and nothing between, which leaves ties whose order no test should lean
 * on. cos*e_q + sqrt(1-cos^2)*e_other is a unit vector whose cosine with e_q
 * is exactly cos. */
static void ask_fixture_put_vector_cos(sqlite3 *db, const char *project, int64_t node_id,
                                       const char *query_text, double cos) {
    float qv[HYP_ASK_DIM];
    fake_axis_vector(query_text, qv);
    int qaxis = 0;
    for (int i = 0; i < HYP_ASK_DIM; i++) {
        if (qv[i] != 0.0f) {
            qaxis = i;
            break;
        }
    }
    float vec[HYP_ASK_DIM];
    memset(vec, 0, sizeof(vec));
    vec[qaxis] = (float)cos;
    vec[(qaxis + 1) % HYP_ASK_DIM] = (float)sqrt(1.0 - cos * cos);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT OR REPLACE INTO ask_vectors(project,node_id,vec,truncated)"
                       " VALUES(?1,?2,?3,0)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, node_id);
    sqlite3_bind_blob(st, 3, vec, (int)sizeof(vec), SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* `n` lines, each `width` bytes wide and self-identifying, so an assertion can
 * name the exact line it expects to see and the exact line it expects not to. */
static char *ask_fixture_make_source(int n, int width) {
    if (width < 24) {
        width = 24;
    }
    char *buf = (char *)malloc((size_t)n * (size_t)(width + 1) + 1);
    if (!buf) {
        return NULL;
    }
    size_t w = 0;
    for (int i = 1; i <= n; i++) {
        int k = snprintf(buf + w, (size_t)width + 1, "/* line %04d ", i);
        while (k < width - 2) {
            buf[w + k] = 'x';
            k++;
        }
        buf[w + k] = '*';
        buf[w + k + 1] = '/';
        w += (size_t)width;
        buf[w++] = '\n';
    }
    buf[w] = '\0';
    return buf;
}

/* Three declarations over two REAL files, at pinned cosines 1.0 / 0.9 / 0.8:
 *
 *   rank 1  writer.orderSections  src/writer.c   10-13   4 lines  — fits whole
 *   rank 2  writer.emit           src/writer.c   40-100  61 lines — over the cap
 *   rank 3  reader.Reader         src/reader.c   5-8     4 lines  — no text: the
 *                                                                   top 2 only
 *
 * `width` sets how wide every source line is, which is what decides whether
 * rank 2 is cut by the LINE cap (narrow) or the BYTE cap (wide).
 *
 * The double hashes the WHOLE question to one axis, so the vectors have to be
 * built against the exact string the test will ask. ask_real_rescore re-aims
 * them when a test asks more than one. */
static int64_t g_real_ids[3];

static void ask_real_rescore(sqlite3 *db, const char *project, const char *question) {
    ask_fixture_put_vector_cos(db, project, g_real_ids[0], question, 1.0);
    ask_fixture_put_vector_cos(db, project, g_real_ids[1], question, 0.9);
    ask_fixture_put_vector_cos(db, project, g_real_ids[2], question, 0.8);
}

static hyp_mcp_server_t *ask_srv_with_real_files(const char *project, const char *root, int width,
                                                 const char *question) {
    char *writer = ask_fixture_make_source(120, width);
    char *reader = ask_fixture_make_source(40, width);
    if (!writer || !reader) {
        free(writer);
        free(reader);
        return NULL;
    }
    int ok = th_write_file(TH_PATH(root, "src/writer.c"), writer) == 0 &&
             th_write_file(TH_PATH(root, "src/reader.c"), reader) == 0;
    free(writer);
    free(reader);
    if (!ok) {
        return NULL;
    }

    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    if (!srv) {
        return NULL;
    }
    hyp_store_t *st = hyp_mcp_server_store(srv);
    hyp_mcp_server_set_project(srv, project);
    hyp_store_upsert_project(st, project, root);

    hyp_node_t a = {.project = project,
                    .label = "Function",
                    .name = "orderSections",
                    .qualified_name = "askreal.writer.orderSections",
                    .file_path = "src/writer.c",
                    .start_line = 10,
                    .end_line = 13};
    hyp_node_t b = {.project = project,
                    .label = "Method",
                    .name = "emit",
                    .qualified_name = "askreal.writer.emit",
                    .file_path = "src/writer.c",
                    .start_line = 40,
                    .end_line = 100};
    hyp_node_t c = {.project = project,
                    .label = "Class",
                    .name = "Reader",
                    .qualified_name = "askreal.reader.Reader",
                    .file_path = "src/reader.c",
                    .start_line = 5,
                    .end_line = 8};
    g_real_ids[0] = hyp_store_upsert_node(st, &a);
    g_real_ids[1] = hyp_store_upsert_node(st, &b);
    g_real_ids[2] = hyp_store_upsert_node(st, &c);

    sqlite3 *db = hyp_store_get_db(st);
    ask_fixture_create_tables(db);
    ask_real_rescore(db, project, question);
    ask_fixture_put_meta(db, project, "test-double/axis-1024", HYP_ASK_DIM, "none", 0, 3);
    return srv;
}

/* THE test §3.2 step 1 exists for: the answer carries the code, so the agent
 * does not spend a second turn on get_code_snippet to see it. */
TEST(ask_carries_the_span_text_for_the_top_candidates) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *root = th_mktempdir("hyp-ask-span");
    ASSERT_NOT_NULL(root);
    char *root_copy = hyp_strdup(root);
    hyp_mcp_server_t *srv = ask_srv_with_real_files("askreal", root_copy, 32, "GOLD");
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\"}");
    ASSERT_NOT_NULL(resp);
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);

    /* The block, and the caps it declares. */
    ASSERT_NOT_NULL(strstr(inner, "\nsource: 2  ("));
    ASSERT_NOT_NULL(strstr(inner, "capped at 40 lines / 1600 bytes each"));

    /* Rank 1 arrives WHOLE, and the actual lines are there. */
    ASSERT_NOT_NULL(strstr(inner, "#1 askreal.writer.orderSections src/writer.c:10-13 — whole, "
                                  "4 lines"));
    ASSERT_NOT_NULL(strstr(inner, "/* line 0010 "));
    ASSERT_NOT_NULL(strstr(inner, "/* line 0013 "));
    ASSERT_NULL(strstr(inner, "/* line 0014 ")); /* and not one line more */
    ASSERT_NULL(strstr(inner, "/* line 0009 "));

    /* Rank 3 is a ROW but carries no text: rank 3 held no gold at all on the
     * pinned corpus, so it is not worth its bytes. */
    ASSERT_NOT_NULL(strstr(inner, "askreal.reader.Reader"));
    ASSERT_NULL(strstr(inner, "#3 "));

    /* TOON is not JSON, so a client is told to read `content` by the ABSENCE
     * of structuredContent — not by an empty object standing in for it. */
    ASSERT_NULL(ask_structured(resp));

    free(inner);
    free(resp);
    free(root_copy);
    hyp_mcp_server_free(srv);
    th_rmtree(root);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* A span over the cap is CUT, and the cut NAMES THE FULL RANGE. A silently
 * shortened declaration is the same failure as a silently empty result: the
 * caller cannot tell from the outside that it is holding half of something. */
TEST(ask_a_cut_span_names_its_full_range_and_the_call_that_completes_it) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *root = th_mktempdir("hyp-ask-cut");
    ASSERT_NOT_NULL(root);
    char *root_copy = hyp_strdup(root);
    hyp_mcp_server_t *srv = ask_srv_with_real_files("askreal", root_copy, 32, "GOLD");
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\"}");
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);

    /* 40-100 is 61 lines; the line cap is 40, so lines 40-79 are shown and the
     * header says BOTH ranges and how many of how many. */
    ASSERT_NOT_NULL(
        strstr(inner, "#2 askreal.writer.emit src/writer.c:40-79 of 40-100 — CUT at 40 of 61 "
                      "lines"));
    ASSERT_NOT_NULL(strstr(inner, "/* line 0040 "));
    ASSERT_NOT_NULL(strstr(inner, "/* line 0079 "));
    ASSERT_NULL(strstr(inner, "/* line 0080 "));
    /* And the answer names the call that gets the rest. */
    ASSERT_NOT_NULL(strstr(inner, "get_code_snippet"));
    /* The ROW still carries the declaration's true range, unchanged. */
    ASSERT_NOT_NULL(strstr(inner, "askreal.writer.emit Method src/writer.c 40-100"));

    free(inner);
    free(resp);
    free(root_copy);
    hyp_mcp_server_free(srv);
    th_rmtree(root);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* The point of the whole step is TOKENS, so the block is bounded by bytes and
 * not only by lines: 40 lines of 200-byte C++ is 8 KB, and two of them is a
 * whole-file answer wearing a cap. */
TEST(ask_the_source_block_is_bounded_in_bytes_not_only_lines) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *root = th_mktempdir("hyp-ask-bound");
    ASSERT_NOT_NULL(root);
    char *root_copy = hyp_strdup(root);
    /* 200-byte lines: 40 of them is 8000 bytes, five times the per-span cap. */
    hyp_mcp_server_t *srv = ask_srv_with_real_files("askreal", root_copy, 200, "GOLD");
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\"}");
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);

    /* Total answer stays small enough to be one call's worth of context: the
     * disclosures are ~700 bytes and the pool is 3200. */
    size_t total = strlen(inner);
    ASSERT_TRUE(total < 5000);
    /* Rank 1 is 4 lines of 200 = 800 bytes, under the per-span cap, so it is
     * whole; rank 2 is cut by BYTES well before its 40th line. */
    ASSERT_NOT_NULL(strstr(inner, "#1 askreal.writer.orderSections src/writer.c:10-13 — whole"));
    ASSERT_NOT_NULL(strstr(inner, "#2 askreal.writer.emit src/writer.c:40-"));
    ASSERT_NOT_NULL(strstr(inner, " of 40-100 — CUT at "));
    ASSERT_NULL(strstr(inner, "/* line 0079 "));

    free(inner);
    free(resp);
    free(root_copy);
    hyp_mcp_server_free(srv);
    th_rmtree(root);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* include_source=false is the caller who wants coordinates only, and it must
 * actually cost less — the whole argument for the flag is bytes. */
TEST(ask_include_source_false_returns_coordinates_only_and_costs_less) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *root = th_mktempdir("hyp-ask-nosrc");
    ASSERT_NOT_NULL(root);
    char *root_copy = hyp_strdup(root);
    hyp_mcp_server_t *srv = ask_srv_with_real_files("askreal", root_copy, 32, "GOLD");
    ASSERT_NOT_NULL(srv);

    char *with = ask_text(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\"}");
    char *without =
        ask_text(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\",\"include_source\":false}");
    ASSERT_NOT_NULL(with);
    ASSERT_NOT_NULL(without);

    ASSERT_NULL(strstr(without, "\nsource:"));
    ASSERT_NULL(strstr(without, "/* line 0010 "));
    ASSERT_TRUE(strlen(without) < strlen(with));
    /* The rows and every disclosure are unchanged — only the text is gone. */
    ASSERT_NOT_NULL(strstr(without, "askreal.writer.orderSections Function src/writer.c 10-13"));
    ASSERT_NOT_NULL(strstr(without, "population: 3"));
    ASSERT_NOT_NULL(strstr(without, "whole_file_spans:"));
    ASSERT_NOT_NULL(strstr(without, "truncation:"));

    free(with);
    free(without);
    free(root_copy);
    hyp_mcp_server_free(srv);
    th_rmtree(root);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* A span that cannot be read says WHY, in the place the text would have been.
 * Omitting it silently would leave a caller unable to tell "this declaration
 * is short" from "this file is gone". */
TEST(ask_says_why_a_span_could_not_be_read_rather_than_omitting_it) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *root = th_mktempdir("hyp-ask-gone");
    ASSERT_NOT_NULL(root);
    char *root_copy = hyp_strdup(root);
    hyp_mcp_server_t *srv = ask_srv_with_real_files("askreal", root_copy, 32, "GOLD");
    ASSERT_NOT_NULL(srv);
    /* The index is now stale for writer.c, which is the ordinary case: an edit
     * landed and the pass has not re-run. */
    th_unlink_force(TH_PATH(root_copy, "src/writer.c"));

    char *resp = ask_call(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\"}");
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(
        strstr(inner, "#1 askreal.writer.orderSections src/writer.c:10-13 — NOT READ:"));
    ASSERT_NOT_NULL(strstr(inner, "the index is stale for this declaration"));
    /* Still an answer, not an error: the ranking is intact. */
    ASSERT_FALSE(ask_is_error(resp));
    ASSERT_NOT_NULL(strstr(inner, "available: true"));

    free(inner);
    free(resp);
    free(root_copy);
    hyp_mcp_server_free(srv);
    th_rmtree(root);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* format=json: the client reads structuredContent, and the source arrives as
 * structure rather than as prose it has to scrape. */
TEST(ask_json_source_reaches_the_client_as_structuredcontent) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *root = th_mktempdir("hyp-ask-json");
    ASSERT_NOT_NULL(root);
    char *root_copy = hyp_strdup(root);
    hyp_mcp_server_t *srv = ask_srv_with_real_files("askreal", root_copy, 32, "GOLD");
    ASSERT_NOT_NULL(srv);

    char *resp =
        ask_call(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\",\"format\":\"json\"}");
    ASSERT_NOT_NULL(resp);
    yyjson_doc *sc = ask_structured(resp);
    ASSERT_NOT_NULL(sc); /* present, because the payload IS a JSON object */
    yyjson_val *root_val = yyjson_doc_get_root(sc);

    yyjson_val *src = yyjson_obj_get(root_val, "source");
    ASSERT_NOT_NULL(src);
    ASSERT_TRUE(yyjson_is_arr(src));
    ASSERT_EQ(yyjson_arr_size(src), 2U);

    yyjson_val *e0 = yyjson_arr_get(src, 0);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(e0, "rank")), 1);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(e0, "qn")), "askreal.writer.orderSections");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(e0, "file")), "src/writer.c");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(e0, "lines")), "10-13");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(e0, "shown")), "10-13");
    ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(e0, "truncated")));
    const char *text = yyjson_get_str(yyjson_obj_get(e0, "text"));
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(strstr(text, "/* line 0010 "));
    ASSERT_NOT_NULL(strstr(text, "/* line 0013 "));
    ASSERT_NULL(strstr(text, "/* line 0014 "));

    yyjson_val *e1 = yyjson_arr_get(src, 1);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(e1, "lines")), "40-100");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(e1, "shown")), "40-79");
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(e1, "truncated")));

    /* The rows contract is untouched: arrays matching cols, not envelopes. */
    yyjson_val *cols = yyjson_obj_get(root_val, "cols");
    yyjson_val *rows = yyjson_obj_get(root_val, "rows");
    ASSERT_EQ(yyjson_arr_size(cols), 5U);
    ASSERT_EQ(yyjson_arr_size(rows), 3U);
    ASSERT_EQ(yyjson_arr_size(yyjson_arr_get(rows, 0)), 5U);

    yyjson_doc_free(sc);
    free(resp);
    free(root_copy);
    hyp_mcp_server_free(srv);
    th_rmtree(root);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* The exactness marker. It is a FACT ABOUT TWO STRINGS — the question spells
 * the declaration's name — and never a score band: §2.4 closed margin gates by
 * measurement and this step does not reopen them. */
TEST(ask_exact_marks_a_name_the_question_spelled_and_is_absent_otherwise) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *root = th_mktempdir("hyp-ask-exact");
    ASSERT_NOT_NULL(root);
    char *root_copy = hyp_strdup(root);
    hyp_mcp_server_t *srv = ask_srv_with_real_files("askreal", root_copy, 32, "GOLD");
    ASSERT_NOT_NULL(srv);

    /* Nothing in "GOLD" spells orderSections, emit or Reader: NO column at
     * all, and no prose about it either. A wall of `false` would read as a
     * verdict, and it would cost bytes to say nothing. */
    char *quiet = ask_text(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\"}");
    ASSERT_NOT_NULL(quiet);
    ASSERT_NOT_NULL(strstr(quiet, "(cols: qn label file lines score)"));
    ASSERT_NULL(strstr(quiet, "exact"));

    /* Spell the name and the column appears, true on exactly that row. The
     * double hashes the whole question, so the vectors are re-aimed at the new
     * string first — the ranking under test is the same one either way. */
    sqlite3 *db = hyp_store_get_db(hyp_mcp_server_store(srv));
    ask_real_rescore(db, "askreal", "GOLD orderSections");
    char *loud = ask_text(srv, "{\"project\":\"askreal\",\"question\":\"GOLD orderSections\"}");
    ASSERT_NOT_NULL(loud);
    ASSERT_NOT_NULL(strstr(loud, " score exact)"));
    ASSERT_NOT_NULL(strstr(loud, "exact_marker:"));
    ASSERT_NOT_NULL(strstr(loud, "not a confidence and not a score"));
    const char *row = strstr(loud, "askreal.writer.orderSections Function");
    ASSERT_NOT_NULL(row);
    const char *eol = strchr(row, '\n');
    ASSERT_NOT_NULL(eol);
    ASSERT_TRUE(strstr(row, " true") != NULL && strstr(row, " true") < eol);
    /* And the rows that were not spelled are marked false, not omitted. */
    const char *other = strstr(loud, "askreal.reader.Reader Class");
    ASSERT_NOT_NULL(other);
    const char *oeol = strchr(other, '\n');
    ASSERT_TRUE(strstr(other, " false") != NULL && strstr(other, " false") < oeol);

    /* A substring is NOT a naming: "Readers" must not mark Reader. */
    ask_real_rescore(db, "askreal", "GOLD Readers");
    char *sub = ask_text(srv, "{\"project\":\"askreal\",\"question\":\"GOLD Readers\"}");
    ASSERT_NOT_NULL(sub);
    ASSERT_NULL(strstr(sub, "exact"));

    free(quiet);
    free(loud);
    free(sub);
    free(root_copy);
    hyp_mcp_server_free(srv);
    th_rmtree(root);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* Source lives inside a backtick fence, so source that CONTAINS a backtick
 * fence must not close it early — a markdown file, a doc comment or a C++ raw
 * string would otherwise split the answer in half at the client, silently. The
 * fence grows past anything in the text, which is CommonMark's own rule. */
TEST(ask_a_fence_in_the_source_cannot_close_the_block_early) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *root = th_mktempdir("hyp-ask-fence");
    ASSERT_NOT_NULL(root);
    char *root_copy = hyp_strdup(root);
    hyp_mcp_server_t *srv = ask_srv_with_real_files("askreal", root_copy, 32, "GOLD");
    ASSERT_NOT_NULL(srv);
    /* Rewrite writer.c so the top hit's span (lines 10-13) is itself a fenced
     * markdown block. The node's range is unchanged. */
    ASSERT_EQ(th_write_file(TH_PATH(root_copy, "src/writer.c"),
                            "1\n2\n3\n4\n5\n6\n7\n8\n9\n"
                            "```c\nint x = 1;\n```\ntrailer\n"
                            "15\n16\n17\n18\n19\n20\n"),
              0);

    char *inner = ask_text(srv, "{\"project\":\"askreal\",\"question\":\"GOLD\"}");
    ASSERT_NOT_NULL(inner);
    /* A four-backtick fence, because the text holds a three-backtick run at a
     * line start. The inner ``` survives verbatim. */
    ASSERT_NOT_NULL(strstr(inner, "\n````\n```c\nint x = 1;\n```\ntrailer\n````\n"));

    free(inner);
    free(root_copy);
    hyp_mcp_server_free(srv);
    th_rmtree(root);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* The default is 3 rows, not 10 — rows 4-10 carried a fifth of the measured
 * answers and most of the bytes. `limit` stays honourable for a caller that
 * wants the tail, and the source block does NOT grow with it. */
TEST(ask_default_limit_is_three_and_limit_is_still_honoured) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);
    sqlite3 *db = hyp_store_get_db(hyp_mcp_server_store(srv));
    /* Six more declarations so "3" is a cap and not just the population. */
    for (int i = 0; i < 6; i++) {
        char qn[64];
        char name[32];
        snprintf(qn, sizeof(qn), "askproj.filler.f%d", i);
        snprintf(name, sizeof(name), "f%d", i);
        hyp_node_t n = {.project = "askproj",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "src/filler.c",
                        .start_line = 1 + i,
                        .end_line = 2 + i};
        int64_t id = hyp_store_upsert_node(hyp_mcp_server_store(srv), &n);
        ask_fixture_put_vector_cos(db, "askproj", id, "GOLD", 0.5);
    }
    ask_fixture_put_meta(db, "askproj", "test-double/axis-1024", HYP_ASK_DIM, "none", 0, 9);

    char *def = ask_text(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}");
    ASSERT_NOT_NULL(def);
    ASSERT_NOT_NULL(strstr(def, "results: 3  (cols:"));

    char *wide = ask_text(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"limit\":9}");
    ASSERT_NOT_NULL(wide);
    ASSERT_NOT_NULL(strstr(wide, "results: 9  (cols:"));
    /* Nine rows, still at most two spans: the source cap is not the row cap. */
    ASSERT_NULL(strstr(wide, "#3 "));

    free(def);
    free(wide);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    PASS();
}

/* ── Escalation: which lane answered, and the refusals (§3.1 step 3) ── */

/* A second double whose model id is IN the measured voyage-4 space. Same
 * vectors, same axis trick — only the identity differs, which is exactly the
 * thing the space gate reads. */
static const hyp_ask_backend_t g_fake_backend_voyage4 = {
    .model_id = "voyage-4-nano-Q8_0@test-double",
    .dim = HYP_ASK_DIM,
    .window_tokens = 32768,
    .encode_query = fake_encode_query,
    .encode_documents = fake_encode_documents,
    .truncates = NULL,
};

/* An isolated cache directory holding ONLY the config this test writes: the
 * handler reads ask.escalation.* from <cache>/_config.db and looks for vector
 * files under <cache>/vectors/, so pointing HYP_CACHE_DIR here keeps the real
 * machine's config (and its real key variable name) out of the test. */
typedef struct {
    char *dir;
    char *saved_cache;
} esc_cache_t;

static bool esc_cache_begin(esc_cache_t *c) {
    /* th_mktempdir hands back a static buffer; copy it, because TH_PATH and a
     * second th_mktempdir would overwrite it under us. */
    const char *made = th_mktempdir("hyp-ask-esc");
    c->dir = made ? hyp_strdup(made) : NULL;
    if (!c->dir) {
        return false;
    }
    const char *saved = getenv("HYP_CACHE_DIR");
    c->saved_cache = saved ? hyp_strdup(saved) : NULL;
    hyp_setenv("HYP_CACHE_DIR", c->dir, 1);
    return true;
}

static void esc_cache_end(esc_cache_t *c) {
    if (c->saved_cache) {
        hyp_setenv("HYP_CACHE_DIR", c->saved_cache, 1);
        free(c->saved_cache);
    } else {
        hyp_unsetenv("HYP_CACHE_DIR");
    }
    if (c->dir) {
        th_cleanup(c->dir);
        free(c->dir);
    }
    c->dir = NULL;
    c->saved_cache = NULL;
}

/* Write escalation config through the real config layer, mode included. NULL
 * leaves a key unset. `raw_mode` bypasses validation on purpose, for the test
 * that hands the handler a value `config set` would have refused. */
static void esc_cache_configure(const esc_cache_t *c, const char *provider, const char *model,
                                const char *key_env, const char *raw_mode) {
    hyp_config_t *cfg = hyp_config_open(c->dir);
    if (!cfg) {
        return;
    }
    if (provider) {
        hyp_config_set(cfg, HYP_CONFIG_ASK_ESC_PROVIDER, provider);
    }
    if (model) {
        hyp_config_set(cfg, HYP_CONFIG_ASK_ESC_MODEL, model);
    }
    if (key_env) {
        hyp_config_set(cfg, HYP_CONFIG_ASK_ESC_KEY_ENV, key_env);
    }
    if (raw_mode) {
        hyp_config_set(cfg, HYP_CONFIG_ASK_ESC_MODE, raw_mode);
    }
    hyp_config_close(cfg);
}

#define ESC_TEST_KEY_VAR "HYP_TEST_ASK_ESCALATION_KEY_VAR"

/* A project with one node and NO vector fixture at all — neither lane built. */
static hyp_mcp_server_t *ask_srv_bare(void) {
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    if (!srv) {
        return NULL;
    }
    hyp_store_t *st = hyp_mcp_server_store(srv);
    hyp_mcp_server_set_project(srv, "bare");
    hyp_store_upsert_project(st, "bare", "/tmp/bare");
    hyp_node_t n = {.project = "bare",
                    .label = "Function",
                    .name = "f",
                    .qualified_name = "bare.f",
                    .file_path = "a.c",
                    .start_line = 1,
                    .end_line = 2};
    hyp_store_upsert_node(st, &n);
    return srv;
}

/* Every answer names the lane that produced it and BOTH encoders, in both
 * encodings, so an agent holding the answer knows what it is holding. */
TEST(ask_local_answers_carry_lane_and_both_encoders) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}");
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "lane: local"));
    ASSERT_NOT_NULL(strstr(inner, "query_encoder: test-double/axis-1024"));
    ASSERT_NOT_NULL(strstr(inner, "index_encoder: test-double/axis-1024"));
    /* No space disclosure on the local lane: one model on both sides. */
    ASSERT_NULL(strstr(inner, "space:"));
    free(inner);
    free(resp);

    resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"format\":\"json\"}");
    inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    yyjson_doc *doc = yyjson_read(inner, strlen(inner), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(root, "lane")), "local");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(root, "query_encoder")), "test-double/axis-1024");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(root, "index_encoder")), "test-double/axis-1024");
    ASSERT_NULL(yyjson_obj_get(root, "space"));
    yyjson_doc_free(doc);
    free(inner);
    free(resp);

    /* And the unavailable answer says which lane could not answer. */
    hyp_ask_backend_install(NULL);
    resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}");
    inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "available: false"));
    ASSERT_NOT_NULL(strstr(inner, "lane: local"));
    free(inner);
    free(resp);

    hyp_mcp_server_free(srv);
    PASS();
}

/* escalate=true with nothing configured: an ERROR that says it did not fall
 * back, points at the config keys, and — because the default mode is query —
 * does NOT send the caller off to build an escalation index. The local
 * encoder must not have been asked anything. */
TEST(ask_escalate_unconfigured_refuses_and_does_not_fall_back) {
    fake_reset();
    esc_cache_t cache;
    ASSERT_TRUE(esc_cache_begin(&cache));
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"escalate\":true}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "did NOT fall back"));
    ASSERT_NOT_NULL(strstr(inner, "ask.escalation.provider"));
    ASSERT_NOT_NULL(strstr(inner, "ask.escalation.mode=query"));
    /* Query mode needs no escalation index; telling someone to build one
     * would send them to spend money on the wrong thing. */
    ASSERT_NULL(strstr(inner, "embed --escalation"));
    ASSERT_NULL(strstr(inner, "results:"));
    ASSERT_EQ(g_fake_query_calls, 0);
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    esc_cache_end(&cache);
    PASS();
}

/* Configured, key variable unset: the refusal names THE VARIABLE — never a
 * value — and did not fall back. */
TEST(ask_escalate_query_mode_refuses_without_key_naming_the_variable) {
    fake_reset();
    esc_cache_t cache;
    ASSERT_TRUE(esc_cache_begin(&cache));
    hyp_unsetenv(ESC_TEST_KEY_VAR);
    esc_cache_configure(&cache, "voyage", "voyage-4-large", ESC_TEST_KEY_VAR, NULL);
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend_voyage4), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);
    sqlite3 *db = hyp_store_get_db(hyp_mcp_server_store(srv));
    ask_fixture_put_meta(db, "askproj", "voyage-4-nano-Q8_0@test-double", HYP_ASK_DIM, "none", 0,
                         3);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"escalate\":true}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, ESC_TEST_KEY_VAR));
    ASSERT_NOT_NULL(strstr(inner, "did NOT fall back"));
    ASSERT_NOT_NULL(strstr(inner, "ONLY the question"));
    ASSERT_NULL(strstr(inner, "results:"));
    ASSERT_EQ(g_fake_query_calls, 0);
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    esc_cache_end(&cache);
    PASS();
}

/* THE SPACE GATE. The local index was built by a model whose space is
 * unmeasured; the escalation model is voyage-4-large. Nothing is scored, the
 * refusal names both model ids and both spaces and says why, and no request
 * left the machine (the key is a placeholder — a leak here would be a curl to
 * the real endpoint, which the test would notice as a provider error). */
TEST(ask_escalate_query_mode_refuses_when_the_index_space_is_unknown) {
    fake_reset();
    esc_cache_t cache;
    ASSERT_TRUE(esc_cache_begin(&cache));
    hyp_setenv(ESC_TEST_KEY_VAR, "not-a-real-key-placeholder", 1);
    esc_cache_configure(&cache, "voyage", "voyage-4-large", ESC_TEST_KEY_VAR, NULL);
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"escalate\":true}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "test-double/axis-1024"));     /* the index's model */
    ASSERT_NOT_NULL(strstr(inner, "voyage/voyage-4-large"));     /* the escalation model */
    ASSERT_NOT_NULL(strstr(inner, "unknown/unmeasured"));        /* the index's space */
    ASSERT_NOT_NULL(strstr(inner, "embedding space: voyage-4")); /* the query's space */
    ASSERT_NOT_NULL(strstr(inner, "confidently-ranked garbage"));
    ASSERT_NOT_NULL(strstr(inner, "did NOT fall back"));
    /* The in-graph fixture carries no stamp, so the space was DERIVED and the
     * refusal says so rather than passing it off as a labelled index. */
    ASSERT_NOT_NULL(strstr(inner, "derived from the index's model id"));
    /* The placeholder must never be echoed. */
    ASSERT_NULL(strstr(inner, "not-a-real-key-placeholder"));
    ASSERT_NULL(strstr(inner, "results:"));
    ASSERT_EQ(g_fake_query_calls, 0);
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    hyp_unsetenv(ESC_TEST_KEY_VAR);
    esc_cache_end(&cache);
    PASS();
}

/* The other direction: the index IS in voyage-4, the escalation model is not
 * (voyage-code-3 — same vendor, one generation back, measured NOT shared).
 * Refused, naming both. */
TEST(ask_escalate_query_mode_refuses_a_space_mismatch_naming_both_spaces) {
    fake_reset();
    esc_cache_t cache;
    ASSERT_TRUE(esc_cache_begin(&cache));
    hyp_setenv(ESC_TEST_KEY_VAR, "not-a-real-key-placeholder", 1);
    esc_cache_configure(&cache, "voyage", "voyage-code-3", ESC_TEST_KEY_VAR, "query");
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend_voyage4), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);
    sqlite3 *db = hyp_store_get_db(hyp_mcp_server_store(srv));
    ask_fixture_put_meta(db, "askproj", "voyage-4-nano-Q8_0@test-double", HYP_ASK_DIM, "none", 0,
                         3);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"escalate\":true}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "voyage-4-nano-Q8_0@test-double"));
    ASSERT_NOT_NULL(strstr(inner, "embedding space: voyage-4"));
    ASSERT_NOT_NULL(strstr(inner, "voyage/voyage-code-3"));
    ASSERT_NOT_NULL(strstr(inner, "embedding space: unknown/unmeasured"));
    ASSERT_NOT_NULL(strstr(inner, "did NOT fall back"));
    ASSERT_NULL(strstr(inner, "results:"));
    ASSERT_EQ(g_fake_query_calls, 0);
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    hyp_unsetenv(ESC_TEST_KEY_VAR);
    esc_cache_end(&cache);
    PASS();
}

/* Query mode scores the LOCAL index. When there is none, that is said as
 * itself with the local lane's remedy — not as an empty ranking, and not as
 * "build the escalation index", which query mode would never read. */
TEST(ask_escalate_query_mode_without_a_local_index_says_so) {
    fake_reset();
    esc_cache_t cache;
    ASSERT_TRUE(esc_cache_begin(&cache));
    hyp_setenv(ESC_TEST_KEY_VAR, "not-a-real-key-placeholder", 1);
    esc_cache_configure(&cache, "voyage", "voyage-4-large", ESC_TEST_KEY_VAR, NULL);
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend_voyage4), 0);
    hyp_mcp_server_t *srv = ask_srv_bare();
    ASSERT_NOT_NULL(srv);

    char *resp =
        ask_call(srv, "{\"project\":\"bare\",\"question\":\"anything\",\"escalate\":true}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "no_semantic_index"));
    ASSERT_NOT_NULL(strstr(inner, "LOCAL index"));
    ASSERT_NOT_NULL(strstr(inner, "did NOT fall back"));
    ASSERT_NULL(strstr(inner, "embed --escalation"));
    ASSERT_NULL(strstr(inner, "results:"));
    ASSERT_EQ(g_fake_query_calls, 0);
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    hyp_unsetenv(ESC_TEST_KEY_VAR);
    esc_cache_end(&cache);
    PASS();
}

/* Index mode is the pre-existing lane and behaves as it did: an unbuilt
 * escalation index is refused with the build command, never served locally.
 * The space gate does not apply — both sides of that index are one model. */
TEST(ask_escalate_index_mode_still_refuses_an_unbuilt_escalation_index) {
    fake_reset();
    esc_cache_t cache;
    ASSERT_TRUE(esc_cache_begin(&cache));
    hyp_setenv(ESC_TEST_KEY_VAR, "not-a-real-key-placeholder", 1);
    esc_cache_configure(&cache, "voyage", "voyage-code-3", ESC_TEST_KEY_VAR, "index");
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_bare();
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"bare\",\"question\":\"GOLD\",\"escalate\":true}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "escalation index is not built for this project"));
    ASSERT_NOT_NULL(strstr(inner, "embed --escalation --project bare"));
    ASSERT_NOT_NULL(strstr(inner, "did NOT fall back"));
    ASSERT_NULL(strstr(inner, "embedding space"));
    ASSERT_NULL(strstr(inner, "results:"));
    ASSERT_EQ(g_fake_query_calls, 0);
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    hyp_unsetenv(ESC_TEST_KEY_VAR);
    esc_cache_end(&cache);
    PASS();
}

/* A mode the validator never saw (written by hand or by an older binary) is
 * refused naming both legal values — it decides what leaves the machine, so it
 * is not guessed. */
TEST(ask_escalate_refuses_an_unrecognised_mode) {
    fake_reset();
    esc_cache_t cache;
    ASSERT_TRUE(esc_cache_begin(&cache));
    esc_cache_configure(&cache, "voyage", "voyage-4-large", ESC_TEST_KEY_VAR, "both");
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    ASSERT_NOT_NULL(srv);

    char *resp = ask_call(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\",\"escalate\":true}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(ask_is_error(resp));
    char *inner = ask_inner_text(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "'both'"));
    ASSERT_NOT_NULL(strstr(inner, "'query'"));
    ASSERT_NOT_NULL(strstr(inner, "'index'"));
    ASSERT_NOT_NULL(strstr(inner, "did NOT fall back"));
    ASSERT_EQ(g_fake_query_calls, 0);
    free(inner);
    free(resp);
    hyp_mcp_server_free(srv);
    hyp_ask_backend_install(NULL);
    esc_cache_end(&cache);
    PASS();
}

/* THE TWO ENDS. The tool description an agent reads must name both modes, the
 * default, and the three lane values the answer carries — otherwise the
 * server can do something the client was never told exists. */
TEST(ask_schema_describes_both_escalation_modes_and_the_lane_values) {
    const char *schema = hyp_mcp_tool_input_schema("ask");
    ASSERT_NOT_NULL(schema);
    yyjson_doc *doc = yyjson_read(schema, strlen(schema), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *props = yyjson_obj_get(yyjson_doc_get_root(doc), "properties");
    yyjson_val *esc = props ? yyjson_obj_get(props, "escalate") : NULL;
    ASSERT_NOT_NULL(esc);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(esc, "type")), "boolean");
    ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(esc, "default")));
    const char *d = yyjson_get_str(yyjson_obj_get(esc, "description"));
    ASSERT_NOT_NULL(d);
    ASSERT_NOT_NULL(strstr(d, "ask.escalation.mode"));
    ASSERT_NOT_NULL(strstr(d, "`query` (the DEFAULT)"));
    ASSERT_NOT_NULL(strstr(d, "`index`"));
    ASSERT_NOT_NULL(strstr(d, "ONLY the ~30-token question"));
    ASSERT_NOT_NULL(strstr(d, "voyage-4"));
    ASSERT_NOT_NULL(strstr(d, "escalation-query"));
    ASSERT_NOT_NULL(strstr(d, "escalation-index"));
    ASSERT_NOT_NULL(strstr(d, "query_encoder"));
    ASSERT_NOT_NULL(strstr(d, "index_encoder"));
    yyjson_doc_free(doc);
    PASS();
}

/* ── Registration and schema ─────────────────────────────────────── */

/* §2 cost a measurement round to `search_graph --help` documenting an
 * array-flag spelling that did not work. So the schema is pinned against the
 * invocation the description tells a caller to type: both required flags are
 * declared required, and `question` is declared a STRING (which is what makes
 * the CLI pass it through as one rather than splicing it into an array). */
TEST(ask_schema_declares_what_the_help_promises) {
    const char *schema = hyp_mcp_tool_input_schema("ask");
    ASSERT_NOT_NULL(schema);
    yyjson_doc *doc = yyjson_read(schema, strlen(schema), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *props = yyjson_obj_get(root, "properties");
    ASSERT_NOT_NULL(props);

    yyjson_val *question = yyjson_obj_get(props, "question");
    ASSERT_NOT_NULL(question);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(question, "type")), "string");
    ASSERT_NOT_NULL(yyjson_obj_get(props, "project"));
    ASSERT_NOT_NULL(yyjson_obj_get(props, "language"));
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_obj_get(props, "language"), "type")),
                  "string");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_obj_get(props, "limit"), "type")),
                  "integer");

    yyjson_val *required = yyjson_obj_get(root, "required");
    ASSERT_NOT_NULL(required);
    ASSERT_EQ(yyjson_arr_size(required), 2U);
    bool has_q = false;
    bool has_p = false;
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(required, i, max, v) {
        const char *s = yyjson_get_str(v);
        has_q = has_q || (s && strcmp(s, "question") == 0);
        has_p = has_p || (s && strcmp(s, "project") == 0);
    }
    ASSERT_TRUE(has_q);
    ASSERT_TRUE(has_p);
    yyjson_doc_free(doc);
    PASS();
}

/* The two ends must agree about what the tool now returns. A client picks
 * `include_source` and `limit` out of the SCHEMA, and reads what the answer
 * contains out of the DESCRIPTION — so an implementation that ships the source
 * block behind a schema that never mentions it is §2.14's failure again. */
TEST(ask_schema_documents_the_source_block_and_the_new_default) {
    const char *schema = hyp_mcp_tool_input_schema("ask");
    ASSERT_NOT_NULL(schema);
    yyjson_doc *doc = yyjson_read(schema, strlen(schema), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *props = yyjson_obj_get(yyjson_doc_get_root(doc), "properties");
    ASSERT_NOT_NULL(props);

    yyjson_val *inc = yyjson_obj_get(props, "include_source");
    ASSERT_NOT_NULL(inc);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(inc, "type")), "boolean");
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(inc, "default"))); /* ON by default */
    const char *incd = yyjson_get_str(yyjson_obj_get(inc, "description"));
    ASSERT_NOT_NULL(incd);
    ASSERT_NOT_NULL(strstr(incd, "40 lines / 1600"));
    ASSERT_NOT_NULL(strstr(incd, "CUT"));

    yyjson_val *lim = yyjson_obj_get(props, "limit");
    ASSERT_NOT_NULL(lim);
    ASSERT_EQ(yyjson_get_int(yyjson_obj_get(lim, "default")), 3);
    yyjson_doc_free(doc);

    /* And the description a client actually receives — out of tools/list, the
     * only place it ever sees one — tells it what is about to arrive. */
    char *listed = hyp_mcp_tools_list();
    ASSERT_NOT_NULL(listed);
    ASSERT_NOT_NULL(strstr(listed, "CARRIES THE SOURCE"));
    ASSERT_NOT_NULL(strstr(listed, "include_source=false"));
    ASSERT_NOT_NULL(strstr(listed, "NAMES ITS FULL RANGE"));
    ASSERT_NOT_NULL(strstr(listed, "NOT a confidence"));
    free(listed);
    PASS();
}

/* `ask` is registered as its own tool, and search_graph's semantic_query is
 * untouched beside it — §2.1's "semantic_query stays exactly as it is". */
TEST(ask_is_a_separate_tool_and_semantic_query_is_unchanged) {
    char *json = hyp_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"ask\""));
    ASSERT_NOT_NULL(strstr(json, "\"semantic_query\""));
    /* semantic_query is still an ARRAY of keyword strings on search_graph. */
    ASSERT_NOT_NULL(strstr(json, "MUST be an ARRAY of keyword strings"));
    free(json);

    /* And it shows up in the registry-rendered --help block. */
    char *help = hyp_mcp_tools_help_list();
    ASSERT_NOT_NULL(help);
    ASSERT_NOT_NULL(strstr(help, "ask"));
    free(help);
    PASS();
}

/* ── The 3-D view's query overlay: TWO ENDS MUST AGREE ──────────────
 *
 * The overlay's neighbours must be the `ask` tool's own ranked rows — the
 * same list, in the same order — not a recomputation that could disagree.
 * This test builds a REAL vector file (the production store, under an
 * isolated cache dir), fits the view, calls the tool through the MCP envelope
 * as a client would, calls the overlay as the UI would, and asserts the two
 * agree row for row. It also pins the caveat: the sentence that says the
 * distances are not real must be in the JSON, verbatim. */
static const char *json_str_at(yyjson_val *obj, const char *key) {
    yyjson_val *v = obj ? yyjson_obj_get(obj, key) : NULL;
    return v && yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
}

TEST(ask_view_overlay_neighbours_are_the_tools_own_rows) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *dir = th_mktempdir("hyp-askview-overlay");
    ASSERT_NOT_NULL(dir);
    char *old_cache = getenv("HYP_CACHE_DIR") ? hyp_strdup(getenv("HYP_CACHE_DIR")) : NULL;
    ASSERT_EQ(hyp_setenv("HYP_CACHE_DIR", dir, 1), 0);

    const char *project = "askview";
    hyp_mcp_server_t *srv = ask_srv_with_nodes(project);
    /* Everything below the fixture is captured first and asserted after the
     * environment is restored, so a failing assert cannot leak HYP_CACHE_DIR
     * into every later suite in this process. */
    bool built = false;
    bool fitted = false;
    char *tool_json = NULL;
    char *overlay = NULL;
    char *cloud = NULL;
    if (srv) {
        hyp_ask_vectors_t *v = hyp_ask_vectors_open(project);
        if (v && hyp_ask_vectors_begin_build(v, g_fake_backend.model_id, "", HYP_ASK_DIM,
                                             g_fake_backend.window_tokens, "g1", "test double",
                                             false) == HYP_ASK_VEC_OK) {
            static float vecs[3][HYP_ASK_DIM];
            fake_axis_vector("GOLD", vecs[0]);
            fake_axis_vector("other-1", vecs[1]);
            fake_axis_vector("other-2", vecs[2]);
            hyp_ask_vec_row_t rows[3];
            memset(rows, 0, sizeof(rows));
            rows[0].qualified_name = "askview.writer.orderSections";
            rows[0].node_id = 1;
            rows[0].label = "Function";
            rows[0].file_path = "src/writer.c";
            rows[0].start_line = 120;
            rows[0].end_line = 168;
            rows[0].lang = "C";
            rows[0].content_hash = "h-gold";
            rows[0].vector = vecs[0];
            rows[1] = rows[0];
            rows[1].qualified_name = "askview.writer.emit";
            rows[1].node_id = 2;
            rows[1].label = "Method";
            rows[1].start_line = 200;
            rows[1].end_line = 240;
            rows[1].content_hash = "h-1";
            rows[1].vector = vecs[1];
            rows[2] = rows[0];
            rows[2].qualified_name = "askview.reader.Reader";
            rows[2].node_id = 3;
            rows[2].label = "Class";
            rows[2].file_path = "src/reader.c";
            rows[2].start_line = 10;
            rows[2].end_line = 90;
            rows[2].content_hash = "h-2";
            rows[2].vector = vecs[2];
            built = hyp_ask_vectors_put(v, rows, 3) == HYP_ASK_VEC_OK &&
                    hyp_ask_vectors_finish_build(v, true) == HYP_ASK_VEC_OK;
            fitted = built && hyp_ask_view_fit(v, NULL) == HYP_ASK_VEC_OK;
        }
        if (v) {
            hyp_ask_vectors_close(v);
        }
        if (fitted) {
            /* END ONE: the tool, through the MCP envelope, as a client sees it. */
            char *resp = ask_call(
                srv,
                "{\"project\":\"askview\",\"question\":\"GOLD\",\"limit\":3,\"format\":\"json\"}");
            tool_json = ask_inner_text(resp);
            free(resp);
            /* END TWO: the overlay, as the UI's /api/embed-view/ask serves it. */
            overlay =
                hyp_mcp_ask_view_overlay(srv, "{\"project\":\"askview\",\"question\":\"GOLD\","
                                              "\"limit\":3}");
            cloud = hyp_mcp_ask_view_points_json(project, false);
        }
        hyp_mcp_server_free(srv);
    }
    hyp_ask_backend_install(NULL);
    if (old_cache) {
        (void)hyp_setenv("HYP_CACHE_DIR", old_cache, 1);
    } else {
        (void)hyp_unsetenv("HYP_CACHE_DIR");
    }
    free(old_cache);
    th_cleanup(dir);

    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(built);
    ASSERT_TRUE(fitted);
    ASSERT_NOT_NULL(tool_json);
    ASSERT_NOT_NULL(overlay);
    ASSERT_NOT_NULL(cloud);

    yyjson_doc *td = yyjson_read(tool_json, strlen(tool_json), 0);
    yyjson_doc *od = yyjson_read(overlay, strlen(overlay), 0);
    yyjson_doc *cd = yyjson_read(cloud, strlen(cloud), 0);
    ASSERT_NOT_NULL(td);
    ASSERT_NOT_NULL(od);
    ASSERT_NOT_NULL(cd);
    yyjson_val *troot = yyjson_doc_get_root(td);
    yyjson_val *oroot = yyjson_doc_get_root(od);
    yyjson_val *croot = yyjson_doc_get_root(cd);

    /* The tool answered from the REAL vector file (population 3, available). */
    yyjson_val *tavail = yyjson_obj_get(troot, "available");
    ASSERT_TRUE(tavail && yyjson_get_bool(tavail));
    yyjson_val *trows = yyjson_obj_get(troot, "rows");
    ASSERT_NOT_NULL(trows);
    ASSERT_EQ((int)yyjson_arr_size(trows), 3);
    /* Gold first — the only row on the question's axis. */
    yyjson_val *r0 = yyjson_arr_get(trows, 0);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_arr_get(r0, 0)), "askview.writer.orderSections");

    /* The overlay carries the tool's own answer verbatim... */
    yyjson_val *oask = yyjson_obj_get(oroot, "ask");
    ASSERT_NOT_NULL(oask);
    yyjson_val *oask_rows = yyjson_obj_get(oask, "rows");
    ASSERT_NOT_NULL(oask_rows);
    ASSERT_EQ((int)yyjson_arr_size(oask_rows), 3);
    /* ...a fitted view... */
    yyjson_val *oview = yyjson_obj_get(oroot, "view");
    ASSERT_NOT_NULL(oview);
    yyjson_val *oview_avail = yyjson_obj_get(oview, "available");
    ASSERT_TRUE(oview_avail && yyjson_get_bool(oview_avail));
    ASSERT_STR_EQ(json_str_at(oview, "method"), HYP_ASK_VIEW_METHOD);
    yyjson_val *ostale = yyjson_obj_get(oview, "stale");
    ASSERT_TRUE(ostale && !yyjson_get_bool(ostale));
    /* ...the question placed in it... */
    yyjson_val *oq = yyjson_obj_get(oroot, "query");
    ASSERT_NOT_NULL(oq);
    ASSERT_TRUE(yyjson_is_num(yyjson_obj_get(oq, "x")));
    ASSERT_TRUE(yyjson_is_num(yyjson_obj_get(oq, "y")));
    ASSERT_TRUE(yyjson_is_num(yyjson_obj_get(oq, "z")));
    /* ...and hits that ARE the tool's rows, rank for rank, both against the
     * embedded copy and against the SEPARATE tool call a client made. */
    yyjson_val *hits = yyjson_obj_get(oroot, "hits");
    ASSERT_NOT_NULL(hits);
    ASSERT_EQ((int)yyjson_arr_size(hits), 3);
    for (int i = 0; i < 3; i++) {
        yyjson_val *h = yyjson_arr_get(hits, (size_t)i);
        yyjson_val *client_row = yyjson_arr_get(trows, (size_t)i);
        yyjson_val *embedded_row = yyjson_arr_get(oask_rows, (size_t)i);
        const char *hqn = json_str_at(h, "qualified_name");
        ASSERT_NOT_NULL(hqn);
        ASSERT_STR_EQ(hqn, yyjson_get_str(yyjson_arr_get(client_row, 0)));
        ASSERT_STR_EQ(hqn, yyjson_get_str(yyjson_arr_get(embedded_row, 0)));
        ASSERT_EQ((int)yyjson_get_int(yyjson_obj_get(h, "rank")), i + 1);
        ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(h, "projected")));
        ASSERT_TRUE(yyjson_is_num(yyjson_obj_get(h, "x")));
        /* Same score the client saw. */
        ASSERT_FLOAT_EQ((float)yyjson_get_num(yyjson_obj_get(h, "score")),
                        (float)yyjson_get_num(yyjson_arr_get(client_row, 4)), 1e-6F);
    }
    /* A hit's coordinates are the cloud's coordinates for the same row: one
     * projection on disk, read from two routes. */
    yyjson_val *h0 = yyjson_arr_get(hits, 0);
    yyjson_val *points = yyjson_obj_get(croot, "points");
    ASSERT_NOT_NULL(points);
    ASSERT_EQ((int)yyjson_arr_size(points), 3);
    bool matched = false;
    size_t pi = 0;
    size_t pmax = 0;
    yyjson_val *pt = NULL;
    yyjson_arr_foreach(points, pi, pmax, pt) {
        const char *pqn = json_str_at(pt, "qn");
        if (pqn && strcmp(pqn, "askview.writer.orderSections") == 0) {
            matched = true;
            ASSERT_FLOAT_EQ((float)yyjson_get_num(yyjson_obj_get(pt, "x")),
                            (float)yyjson_get_num(yyjson_obj_get(h0, "x")), 0.0F);
            ASSERT_FLOAT_EQ((float)yyjson_get_num(yyjson_obj_get(pt, "y")),
                            (float)yyjson_get_num(yyjson_obj_get(h0, "y")), 0.0F);
            ASSERT_FLOAT_EQ((float)yyjson_get_num(yyjson_obj_get(pt, "z")),
                            (float)yyjson_get_num(yyjson_obj_get(h0, "z")), 0.0F);
        }
    }
    ASSERT_TRUE(matched);
    /* THE CAVEAT, on both payloads, verbatim. */
    ASSERT_STR_EQ(json_str_at(oroot, "caveat"), HYP_ASK_VIEW_CAVEAT);
    ASSERT_STR_EQ(json_str_at(croot, "caveat"), HYP_ASK_VIEW_CAVEAT);
    ASSERT_NOT_NULL(strstr(HYP_ASK_VIEW_CAVEAT, "not real"));

    yyjson_doc_free(td);
    yyjson_doc_free(od);
    yyjson_doc_free(cd);
    free(tool_json);
    free(overlay);
    free(cloud);
    PASS();
}

/* Without a view, the overlay still carries the tool's answer and says why
 * there is no picture — an absent "query" means "look at ask", never "the
 * question landed nowhere". And it must not mint a vector file for a project
 * that has none. */
TEST(ask_view_overlay_without_a_view_says_so_and_creates_nothing) {
    fake_reset();
    ASSERT_EQ(hyp_ask_backend_install(&g_fake_backend), 0);
    char *dir = th_mktempdir("hyp-askview-none");
    ASSERT_NOT_NULL(dir);
    char *old_cache = getenv("HYP_CACHE_DIR") ? hyp_strdup(getenv("HYP_CACHE_DIR")) : NULL;
    ASSERT_EQ(hyp_setenv("HYP_CACHE_DIR", dir, 1), 0);
    hyp_mcp_server_t *srv = ask_srv_with_nodes("askproj");
    char *overlay =
        srv ? hyp_mcp_ask_view_overlay(srv, "{\"project\":\"askproj\",\"question\":\"GOLD\"}")
            : NULL;
    char vpath[HYP_SZ_4K];
    bool minted = hyp_ask_vectors_path("askproj", vpath, sizeof(vpath)) && hyp_file_exists(vpath);
    if (srv) {
        hyp_mcp_server_free(srv);
    }
    hyp_ask_backend_install(NULL);
    if (old_cache) {
        (void)hyp_setenv("HYP_CACHE_DIR", old_cache, 1);
    } else {
        (void)hyp_unsetenv("HYP_CACHE_DIR");
    }
    free(old_cache);
    th_cleanup(dir);
    ASSERT_NOT_NULL(overlay);
    ASSERT_FALSE(minted);
    yyjson_doc *od = yyjson_read(overlay, strlen(overlay), 0);
    ASSERT_NOT_NULL(od);
    yyjson_val *root = yyjson_doc_get_root(od);
    /* The tool answered (from the in-graph fixture) — its answer is here. */
    yyjson_val *ask = yyjson_obj_get(root, "ask");
    ASSERT_NOT_NULL(ask);
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(ask, "available")));
    /* No view: said, with the remedy; no query, no hits. */
    yyjson_val *view = yyjson_obj_get(root, "view");
    ASSERT_NOT_NULL(view);
    ASSERT_FALSE(yyjson_get_bool(yyjson_obj_get(view, "available")));
    ASSERT_NOT_NULL(strstr(json_str_at(view, "remedy"), "--fit-view"));
    ASSERT_NULL(yyjson_obj_get(root, "query"));
    ASSERT_NULL(yyjson_obj_get(root, "hits"));
    ASSERT_STR_EQ(json_str_at(root, "caveat"), HYP_ASK_VIEW_CAVEAT);
    yyjson_doc_free(od);
    free(overlay);
    PASS();
}

SUITE(ask) {
    RUN_TEST(ask_view_overlay_neighbours_are_the_tools_own_rows);
    RUN_TEST(ask_view_overlay_without_a_view_says_so_and_creates_nothing);
    RUN_TEST(ask_backend_none_by_default);
    RUN_TEST(ask_backend_install_and_uninstall);
    RUN_TEST(ask_backend_rejects_malformed);
    RUN_TEST(ask_lang_cpp_renders_the_measured_display_name);
    RUN_TEST(ask_lang_accepts_extension_and_display_name);
    RUN_TEST(ask_lang_refuses_rather_than_defaults);
    RUN_TEST(ask_lang_dominant_counts_files);
    RUN_TEST(ask_lang_dominant_refuses_on_nothing_recognisable);
    RUN_TEST(ask_status_no_backend_wins_over_everything);
    RUN_TEST(ask_status_no_index_when_tables_absent);
    RUN_TEST(ask_status_model_mismatch_is_refused_not_mixed);
    RUN_TEST(ask_status_truncation_has_three_states);
    RUN_TEST(ask_unavailable_says_so_and_emits_no_rows);
    RUN_TEST(ask_unavailable_no_index_names_the_project_and_the_pass);
    RUN_TEST(ask_returns_ranked_spans_with_line_numbers);
    RUN_TEST(ask_discloses_the_language_it_rendered);
    RUN_TEST(ask_refuses_an_unknown_language_rather_than_defaulting);
    RUN_TEST(ask_adds_the_cut_column_only_when_rows_were_truncated);
    RUN_TEST(ask_json_rows_are_arrays_matching_cols);
    RUN_TEST(ask_drops_rows_whose_node_is_gone);
    RUN_TEST(ask_skips_vectors_of_the_wrong_width);
    RUN_TEST(ask_rejects_an_array_and_points_at_semantic_query);
    RUN_TEST(ask_requires_a_question);
    RUN_TEST(ask_local_answers_carry_lane_and_both_encoders);
    RUN_TEST(ask_escalate_unconfigured_refuses_and_does_not_fall_back);
    RUN_TEST(ask_escalate_query_mode_refuses_without_key_naming_the_variable);
    RUN_TEST(ask_escalate_query_mode_refuses_when_the_index_space_is_unknown);
    RUN_TEST(ask_escalate_query_mode_refuses_a_space_mismatch_naming_both_spaces);
    RUN_TEST(ask_escalate_query_mode_without_a_local_index_says_so);
    RUN_TEST(ask_escalate_index_mode_still_refuses_an_unbuilt_escalation_index);
    RUN_TEST(ask_escalate_refuses_an_unrecognised_mode);
    RUN_TEST(ask_schema_describes_both_escalation_modes_and_the_lane_values);
    RUN_TEST(ask_schema_declares_what_the_help_promises);
    RUN_TEST(ask_is_a_separate_tool_and_semantic_query_is_unchanged);
    RUN_TEST(ask_carries_the_span_text_for_the_top_candidates);
    RUN_TEST(ask_a_cut_span_names_its_full_range_and_the_call_that_completes_it);
    RUN_TEST(ask_the_source_block_is_bounded_in_bytes_not_only_lines);
    RUN_TEST(ask_include_source_false_returns_coordinates_only_and_costs_less);
    RUN_TEST(ask_says_why_a_span_could_not_be_read_rather_than_omitting_it);
    RUN_TEST(ask_json_source_reaches_the_client_as_structuredcontent);
    RUN_TEST(ask_exact_marks_a_name_the_question_spelled_and_is_absent_otherwise);
    RUN_TEST(ask_a_fence_in_the_source_cannot_close_the_block_early);
    RUN_TEST(ask_default_limit_is_three_and_limit_is_still_honoured);
    RUN_TEST(ask_schema_documents_the_source_block_and_the_new_default);
}
