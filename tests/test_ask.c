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

SUITE(ask) {
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
    RUN_TEST(ask_schema_declares_what_the_help_promises);
    RUN_TEST(ask_is_a_separate_tool_and_semantic_query_is_unchanged);
}
