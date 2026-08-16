/*
 * test_generation_carry.c — unit A12: multi-member index assembly, and what
 * must survive a generation boundary.
 *
 * Publication builds a fresh database from ONE member's graph buffer and
 * renames it over the destination. Two things were confirmed lost that way,
 * from two directions: the workspace registry (so "all members share one
 * database" was true of no code path), and an authored decision record's
 * timestamps (re-stamped by the publishing clock, which mints a second record
 * for one document under an append-only union).
 *
 * So the tests here are about the RULE, not the two instances:
 *
 *   1. the judgement is COMPLETE against a real database — add a durable
 *      table and forget to teach publication about it, and this fails naming
 *      your table;
 *   2. publication REFUSES a table it cannot judge, rather than guessing;
 *   3. the registry survives a full index of a member (the plan's row);
 *   4. member 1's nodes and edges survive indexing member 2 (the same row's
 *      second half, and the whole point of a workspace store);
 *   5. a carried row keeps ITS OWN timestamps. Asserted against a planted,
 *      distinctive instant rather than against wall-clock movement, because a
 *      re-stamp inside the same second is indistinguishable from no re-stamp
 *      and would make the test pass by luck.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat.h"
#include "foundation/constants.h"
#include "pipeline/pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "store/generation_carry.h"
#include "store/store.h"
#include "store/workspace_resolve.h"

#include <sqlite3/sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* ── Fixture: a scratch tree and a redirected cache ──────────────── */

typedef struct {
    char root[256];
    char cache[512];
    char *saved_cache;
} gc_fix_t;

static bool gc_fix_begin(gc_fix_t *f) {
    memset(f, 0, sizeof(*f));
    const char *saved = getenv("HYP_CACHE_DIR");
    if (saved) {
        f->saved_cache = strdup(saved);
        if (!f->saved_cache) {
            return false;
        }
    }
    snprintf(f->root, sizeof(f->root), "/tmp/hyp-a12-carry-XXXXXX");
    if (hyp_mkdtemp(f->root) == NULL) {
        return false;
    }
    snprintf(f->cache, sizeof(f->cache), "%s/cache", f->root);
    return th_mkdir_p(f->cache) == 0 && hyp_setenv("HYP_CACHE_DIR", f->cache, 1) == 0;
}

static void gc_fix_end(gc_fix_t *f) {
    if (f->saved_cache) {
        (void)hyp_setenv("HYP_CACHE_DIR", f->saved_cache, 1);
    } else {
        (void)hyp_unsetenv("HYP_CACHE_DIR");
    }
    if (f->root[0]) {
        th_rmtree(f->root);
    }
    free(f->saved_cache);
    memset(f, 0, sizeof(*f));
}

/* A member repo with one source file, and its project slug. */
static bool gc_make_member(const gc_fix_t *f, const char *name, const char *body, char *root,
                           size_t root_sz, char *slug, size_t slug_sz) {
    snprintf(root, root_sz, "%s/%s", f->root, name);
    if (th_mkdir_p(root) != 0) {
        return false;
    }
    char file[HYP_SZ_1K];
    snprintf(file, sizeof(file), "%s/mod.py", root);
    if (th_write_file(file, body) != 0) {
        return false;
    }
    char *derived = hyp_project_name_from_path(root);
    if (!derived) {
        return false;
    }
    bool ok = (size_t)snprintf(slug, slug_sz, "%s", derived) < slug_sz;
    free(derived);
    return ok;
}

static int gc_index(const char *repo_root, const char *db_path) {
    hyp_pipeline_t *p = hyp_pipeline_new(repo_root, db_path, HYP_MODE_FULL);
    if (!p) {
        return HYP_NOT_FOUND;
    }
    int rc = hyp_pipeline_run(p);
    hyp_pipeline_free(p);
    return rc;
}

/* Run one statement against a database file, outside any store handle. */
static bool gc_exec(const char *db_path, const char *sql) {
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    bool ok = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

/* Route the next run down the path that REBUILDS the file rather than cloning
 * it. Without the manifest an incremental run has nothing to diff against, so
 * it forces a full rebuild — which is the only route that erases, and so the
 * only one worth asserting against. The delta route clones the previous
 * generation byte for byte and would make every test below pass without the
 * carry existing at all. */
static bool gc_force_full_rebuild(const char *db_path) {
    return gc_exec(db_path, "DELETE FROM file_hashes;");
}

/* One integer out of a database file, or -1 when the query cannot run. */
static int gc_scalar(const char *db_path, const char *sql) {
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return HYP_NOT_FOUND;
    }
    sqlite3_stmt *st = NULL;
    int value = HYP_NOT_FOUND;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        value = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return value;
}

/* ════════════════════════════════════════════════════════════════════
 * 1 · The judgement is complete, checked against a real database
 * ════════════════════════════════════════════════════════════════════ */

TEST(a12_every_table_of_a_published_generation_is_judged) {
    gc_fix_t fix;
    if (!gc_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char root[HYP_SZ_512];
    char slug[HYP_SZ_256];
    ASSERT_TRUE(gc_make_member(&fix, "solo", "def alpha():\n    return 1\n", root, sizeof(root),
                               slug, sizeof(slug)));
    char db[HYP_SZ_1K];
    snprintf(db, sizeof(db), "%s/solo.db", fix.cache);
    ASSERT_EQ(gc_index(root, db), 0);

    /* The published file, as the product actually writes it. */
    char unjudged[HYP_SZ_1K] = "";
    int n = hyp_generation_unclassified_tables(db, unjudged, sizeof(unjudged));
    if (n != 0) {
        (void)fprintf(stderr, "unjudged in a published generation:\n%s", unjudged);
    }
    ASSERT_EQ(n, 0);

    /* And the file as the STORE creates it: a table can enter the schema from
     * either writer, and a judgement that covers only one of them is the
     * two-ends shape again. Opening plus one project upsert is what materializes
     * everything init_schema and the metadata writers own. */
    char fresh[HYP_SZ_1K];
    snprintf(fresh, sizeof(fresh), "%s/fresh.db", fix.cache);
    hyp_store_t *s = hyp_store_open_path(fresh);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_upsert_project(s, "fresh-proj", root), HYP_STORE_OK);
    hyp_store_close(s);

    unjudged[0] = '\0';
    n = hyp_generation_unclassified_tables(fresh, unjudged, sizeof(unjudged));
    if (n != 0) {
        (void)fprintf(stderr, "unjudged in a freshly opened store:\n%s", unjudged);
    }
    ASSERT_EQ(n, 0);

    gc_fix_end(&fix);
    PASS();
}

TEST(a12_a_judgement_carries_its_reason_and_an_unknown_table_carries_none) {
    /* A judgement with no reason is a row somebody added to make a build
     * pass. Key on the property — there IS a stated reason — never on the
     * sentence, which would only measure whoever transcribed it. */
    static const char *const judged[] = {"nodes", "workspace_repos", "project_summaries",
                                         "store_meta"};
    for (size_t i = 0; i < sizeof(judged) / sizeof(judged[0]); i++) {
        ASSERT_NEQ((int)hyp_generation_table_class(judged[i]), (int)HYP_TABLE_UNCLASSIFIED);
        ASSERT_NOT_NULL(hyp_generation_table_reason(judged[i]));
        ASSERT_TRUE(strlen(hyp_generation_table_reason(judged[i])) > 16);
    }

    /* Absent means look elsewhere: an unjudged table has no class AND no
     * reason, so a caller cannot mistake a missing judgement for a lenient
     * one. */
    ASSERT_EQ((int)hyp_generation_table_class("a12_table_nobody_judged"),
              (int)HYP_TABLE_UNCLASSIFIED);
    ASSERT_NULL(hyp_generation_table_reason("a12_table_nobody_judged"));
    ASSERT_STR_EQ(hyp_generation_class_name(HYP_TABLE_UNCLASSIFIED), "unclassified");
    ASSERT_STR_EQ(hyp_generation_class_name(HYP_TABLE_DURABLE), "durable");
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 2 · Fail closed: an unjudgeable table stops the publish and is named
 * ════════════════════════════════════════════════════════════════════ */

TEST(a12_publication_refuses_a_table_it_cannot_judge) {
    gc_fix_t fix;
    if (!gc_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char root[HYP_SZ_512];
    char slug[HYP_SZ_256];
    ASSERT_TRUE(gc_make_member(&fix, "guarded", "def before():\n    return 1\n", root,
                               sizeof(root), slug, sizeof(slug)));
    char db[HYP_SZ_1K];
    snprintf(db, sizeof(db), "%s/guarded.db", fix.cache);
    ASSERT_EQ(gc_index(root, db), 0);

    /* A durable table nobody taught publication about. */
    ASSERT_TRUE(gc_exec(db, "CREATE TABLE a12_unjudged (k TEXT PRIMARY KEY, v TEXT);"
                            "INSERT INTO a12_unjudged VALUES ('keep', 'me');"));
    char named[HYP_SZ_1K] = "";
    ASSERT_EQ(hyp_generation_unclassified_tables(db, named, sizeof(named)), 1);
    ASSERT_NOT_NULL(strstr(named, "a12_unjudged"));

    /* Now index again, down the route that rebuilds the file. The publish
     * must refuse rather than quietly drop the table. */
    char file[HYP_SZ_1K];
    snprintf(file, sizeof(file), "%s/mod.py", root);
    ASSERT_EQ(th_write_file(file, "def after():\n    return 2\n"), 0);
    ASSERT_TRUE(gc_force_full_rebuild(db));
    hyp_pipeline_incremental_test_reset_faults();
    int rc = gc_index(root, db);
    ASSERT_EQ((int)hyp_pipeline_incremental_test_last_route(),
              (int)HYP_INCREMENTAL_ROUTE_FORCED_FULL);
    ASSERT_EQ(rc, HYP_PIPELINE_ABORT_PRESERVE_DB);

    /* The refusal preserved the previous generation, unjudged table included:
     * a refusal that damaged the store would be worse than the guess. */
    hyp_store_t *s = hyp_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(hyp_store_count_nodes(s, slug) > 0);
    hyp_store_close(s);
    named[0] = '\0';
    ASSERT_EQ(hyp_generation_unclassified_tables(db, named, sizeof(named)), 1);

    gc_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 3 · The registry survives a full index — the plan's assertion row
 * ════════════════════════════════════════════════════════════════════ */

TEST(a12_workspace_registry_survives_a_full_index_of_a_member) {
    gc_fix_t fix;
    if (!gc_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char one[HYP_SZ_512];
    char two[HYP_SZ_512];
    char slug_one[HYP_SZ_256];
    char slug_two[HYP_SZ_256];
    ASSERT_TRUE(gc_make_member(&fix, "alpha", "def alpha():\n    return 1\n", one, sizeof(one),
                               slug_one, sizeof(slug_one)));
    ASSERT_TRUE(gc_make_member(&fix, "beta", "def beta():\n    return 2\n", two, sizeof(two),
                               slug_two, sizeof(slug_two)));

    char db[HYP_SZ_1K];
    snprintf(db, sizeof(db), "%s/acme.db", fix.cache);

    /* Bind first — the order that broke: the registry writer and the index
     * publisher were each correct alone. */
    {
        hyp_store_t *s = hyp_store_open_path(db);
        ASSERT_NOT_NULL(s);
        hyp_workspace_repo_t rows[2] = {
            {slug_one, one, HYP_WSR_ROLE_MEMBER},
            {slug_two, two, HYP_WSR_ROLE_REFERENCE},
        };
        char err[HYP_SZ_1K] = "";
        ASSERT_EQ(hyp_store_workspace_bind(s, "acme", rows, 2, err, sizeof(err)), HYP_STORE_OK);
        hyp_store_close(s);
    }

    hyp_pipeline_incremental_test_reset_faults();
    ASSERT_EQ(gc_index(one, db), 0);

    hyp_store_t *s = hyp_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    char id[HYP_SZ_512] = "";
    ASSERT_EQ(hyp_store_workspace_id(s, id, sizeof(id)), HYP_STORE_OK);
    ASSERT_STR_EQ(id, "acme");
    hyp_workspace_repo_t *members = NULL;
    int count = 0;
    ASSERT_EQ(hyp_store_workspace_repos(s, &members, &count), HYP_STORE_OK);
    ASSERT_EQ(count, 2);
    /* Roles are part of the registry, not decoration: a carry that kept the
     * slugs and dropped the roles would make every member editable. */
    bool saw_reference = false;
    for (int i = 0; i < count; i++) {
        if (members[i].role && strcmp(members[i].role, HYP_WSR_ROLE_REFERENCE) == 0) {
            saw_reference = true;
        }
    }
    ASSERT_TRUE(saw_reference);
    hyp_store_free_workspace_repos(members, count);
    hyp_store_close(s);

    gc_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 4 · Members assemble: indexing member 2 does not erase member 1
 * ════════════════════════════════════════════════════════════════════ */

TEST(a12_three_members_assemble_into_one_workspace_store) {
    gc_fix_t fix;
    if (!gc_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    enum { MEMBERS = 3 };
    static const char *const names[MEMBERS] = {"m_one", "m_two", "m_three"};
    static const char *const bodies[MEMBERS] = {
        "def one_a():\n    return one_b()\n\n\ndef one_b():\n    return 1\n",
        "def two_a():\n    return two_b()\n\n\ndef two_b():\n    return 2\n",
        "def three_a():\n    return three_b()\n\n\ndef three_b():\n    return 3\n",
    };
    char roots[MEMBERS][HYP_SZ_512];
    char slugs[MEMBERS][HYP_SZ_256];
    for (int i = 0; i < MEMBERS; i++) {
        ASSERT_TRUE(gc_make_member(&fix, names[i], bodies[i], roots[i], sizeof(roots[i]), slugs[i],
                                   sizeof(slugs[i])));
    }

    char db[HYP_SZ_1K];
    snprintf(db, sizeof(db), "%s/trio.db", fix.cache);
    {
        hyp_store_t *s = hyp_store_open_path(db);
        ASSERT_NOT_NULL(s);
        hyp_workspace_repo_t rows[MEMBERS];
        for (int i = 0; i < MEMBERS; i++) {
            rows[i].slug = slugs[i];
            rows[i].root_path = roots[i];
            rows[i].role = HYP_WSR_ROLE_MEMBER;
        }
        char err[HYP_SZ_1K] = "";
        ASSERT_EQ(hyp_store_workspace_bind(s, "trio", rows, MEMBERS, err, sizeof(err)),
                  HYP_STORE_OK);
        hyp_store_close(s);
    }

    int nodes_after_own_index[MEMBERS] = {0, 0, 0};
    int edges_after_own_index[MEMBERS] = {0, 0, 0};
    for (int i = 0; i < MEMBERS; i++) {
        hyp_pipeline_incremental_test_reset_faults();
        ASSERT_EQ(gc_index(roots[i], db), 0);
        if (i > 0) {
            /* A member the store has never seen has no manifest, so its run
             * REBUILDS the file. That is the route that used to erase whoever
             * was indexed before it. */
            ASSERT_EQ((int)hyp_pipeline_incremental_test_last_route(),
                      (int)HYP_INCREMENTAL_ROUTE_FORCED_FULL);
        }
        hyp_store_t *s = hyp_store_open_path_query(db);
        ASSERT_NOT_NULL(s);
        nodes_after_own_index[i] = hyp_store_count_nodes(s, slugs[i]);
        edges_after_own_index[i] = hyp_store_count_edges(s, slugs[i]);
        hyp_store_close(s);
        ASSERT_TRUE(nodes_after_own_index[i] > 0);
        ASSERT_TRUE(edges_after_own_index[i] > 0);
    }

    /* All three, in one file, after the last one was indexed. Counts, not
     * "is anything there": a carry that dropped every edge whose endpoints it
     * could not translate would still leave nodes behind. */
    hyp_store_t *s = hyp_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    for (int i = 0; i < MEMBERS; i++) {
        ASSERT_EQ(hyp_store_count_nodes(s, slugs[i]), nodes_after_own_index[i]);
        ASSERT_EQ(hyp_store_count_edges(s, slugs[i]), edges_after_own_index[i]);
    }
    /* And the registry is still there under them. */
    char id[HYP_SZ_512] = "";
    ASSERT_EQ(hyp_store_workspace_id(s, id, sizeof(id)), HYP_STORE_OK);
    ASSERT_STR_EQ(id, "trio");
    hyp_store_close(s);

    gc_fix_end(&fix);
    PASS();
}

/* The same property through the entry point a caller actually has: a TOML on
 * disk, the one resolver, the one open path, three members into one file. The
 * first real USE of the mechanism is better evidence than the test written to
 * prove it — this one would also catch a resolver, a store path or a registry
 * bind that disagreed with the assembly, which the loop above cannot. */
TEST(a12_a_declared_workspace_indexes_every_member_from_one_call) {
    gc_fix_t fix;
    if (!gc_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    enum { MEMBERS = 3 };
    static const char *const names[MEMBERS] = {"declared_a", "declared_b", "declared_c"};
    char roots[MEMBERS][HYP_SZ_512];
    char slugs[MEMBERS][HYP_SZ_256];
    char body[HYP_SZ_256];
    for (int i = 0; i < MEMBERS; i++) {
        snprintf(body, sizeof(body), "def member_%d():\n    return %d\n", i, i);
        ASSERT_TRUE(gc_make_member(&fix, names[i], body, roots[i], sizeof(roots[i]), slugs[i],
                                   sizeof(slugs[i])));
    }

    char toml_path[HYP_SZ_512];
    snprintf(toml_path, sizeof(toml_path), "%s/%s", fix.root, HYP_WSR_TOML_NAME);
    char toml[HYP_SZ_1K];
    snprintf(toml, sizeof(toml),
             "name = \"declared\"\n"
             "[[repos]]\npath = \"%s\"\nrole = \"member\"\n"
             "[[repos]]\npath = \"%s\"\nrole = \"vendored\"\n"
             "[[repos]]\npath = \"%s\"\nrole = \"reference\"\n",
             roots[0], roots[1], roots[2]);
    ASSERT_EQ(th_write_file(toml_path, toml), 0);

    hyp_wsr_resolved_t ws;
    char err[HYP_SZ_1K] = "";
    ASSERT_EQ(hyp_wsr_resolve(roots[0], toml_path, NULL, &ws, err, sizeof(err)), HYP_WSR_OK);
    ASSERT_EQ(ws.member_count, MEMBERS);

    int indexed = 0;
    ASSERT_EQ(hyp_pipeline_index_workspace(&ws, HYP_MODE_FULL, &indexed, err, sizeof(err)), 0);
    ASSERT_EQ(indexed, MEMBERS);

    /* One file, named by the workspace, holding all three — including the
     * roles that say two of them must not be edited. */
    char db[HYP_SZ_1K];
    snprintf(db, sizeof(db), "%s/%s.db", fix.cache, ws.id);
    hyp_store_t *s = hyp_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    for (int i = 0; i < MEMBERS; i++) {
        ASSERT_TRUE(hyp_store_count_nodes(s, ws.members[i].slug) > 0);
    }
    char id[HYP_SZ_512] = "";
    ASSERT_EQ(hyp_store_workspace_id(s, id, sizeof(id)), HYP_STORE_OK);
    ASSERT_STR_EQ(id, "declared");
    hyp_workspace_repo_t *rows = NULL;
    int count = 0;
    ASSERT_EQ(hyp_store_workspace_repos(s, &rows, &count), HYP_STORE_OK);
    ASSERT_EQ(count, MEMBERS);
    hyp_store_free_workspace_repos(rows, count);
    hyp_store_close(s);

    gc_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * 5 · A carried row keeps its own provenance
 * ════════════════════════════════════════════════════════════════════ */

TEST(a12_a_carried_row_keeps_its_own_timestamps) {
    gc_fix_t fix;
    if (!gc_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char root[HYP_SZ_512];
    char slug[HYP_SZ_256];
    ASSERT_TRUE(gc_make_member(&fix, "recorded", "def before():\n    return 1\n", root,
                               sizeof(root), slug, sizeof(slug)));
    char db[HYP_SZ_1K];
    snprintf(db, sizeof(db), "%s/recorded.db", fix.cache);
    ASSERT_EQ(gc_index(root, db), 0);

    static const char adr_text[] = "# Decision\nCarried rows keep their own clock.";
    {
        hyp_store_t *s = hyp_store_open_path(db);
        ASSERT_NOT_NULL(s);
        ASSERT_EQ(hyp_store_adr_store(s, slug, adr_text), HYP_STORE_OK);
        hyp_store_close(s);
    }

    /* Plant a distinctive instant. Asserting that the timestamp does not MOVE
     * across a reindex would pass by luck whenever both writes land in the
     * same second; asserting it is still THIS instant cannot. */
    static const char planted_created[] = "2001-02-03T04:05:06Z";
    static const char planted_updated[] = "2002-03-04T05:06:07Z";
    char sql[HYP_SZ_1K];
    snprintf(sql, sizeof(sql),
             "UPDATE project_summaries SET created_at = '%s', updated_at = '%s';",
             planted_created, planted_updated);
    ASSERT_TRUE(gc_exec(db, sql));

    char file[HYP_SZ_1K];
    snprintf(file, sizeof(file), "%s/mod.py", root);
    ASSERT_EQ(th_write_file(file, "def after():\n    return 2\n"), 0);
    ASSERT_TRUE(gc_force_full_rebuild(db));
    hyp_pipeline_incremental_test_reset_faults();
    ASSERT_EQ(gc_index(root, db), 0);
    ASSERT_EQ((int)hyp_pipeline_incremental_test_last_route(),
              (int)HYP_INCREMENTAL_ROUTE_FORCED_FULL);

    hyp_store_t *s = hyp_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    hyp_adr_t adr = {0};
    ASSERT_EQ(hyp_store_adr_get(s, slug, &adr), HYP_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    ASSERT_STR_EQ(adr.content, adr_text);
    ASSERT_NOT_NULL(adr.created_at);
    ASSERT_NOT_NULL(adr.updated_at);
    ASSERT_STR_EQ(adr.created_at, planted_created);
    ASSERT_STR_EQ(adr.updated_at, planted_updated);
    hyp_store_adr_free(&adr);
    hyp_store_close(s);

    /* The graph really was rebuilt around it — otherwise this test proves
     * only that nothing happened. */
    ASSERT_TRUE(gc_scalar(db, "SELECT count(*) FROM nodes WHERE name = 'after';") > 0);
    ASSERT_EQ(gc_scalar(db, "SELECT count(*) FROM nodes WHERE name = 'before';"), 0);

    gc_fix_end(&fix);
    PASS();
}

/* Another member's indexed_at is provenance too: it records when THAT tree was
 * read, and a publish that re-stamped it would claim this run measured a
 * repository it never opened. */
TEST(a12_another_members_indexed_at_is_not_restamped) {
    gc_fix_t fix;
    if (!gc_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char one[HYP_SZ_512];
    char two[HYP_SZ_512];
    char slug_one[HYP_SZ_256];
    char slug_two[HYP_SZ_256];
    ASSERT_TRUE(gc_make_member(&fix, "early", "def early():\n    return 1\n", one, sizeof(one),
                               slug_one, sizeof(slug_one)));
    ASSERT_TRUE(gc_make_member(&fix, "later", "def later():\n    return 2\n", two, sizeof(two),
                               slug_two, sizeof(slug_two)));
    char db[HYP_SZ_1K];
    snprintf(db, sizeof(db), "%s/pair.db", fix.cache);
    ASSERT_EQ(gc_index(one, db), 0);

    static const char planted[] = "2003-04-05T06:07:08Z";
    char sql[HYP_SZ_1K];
    snprintf(sql, sizeof(sql), "UPDATE projects SET indexed_at = '%s' WHERE name = '%s';", planted,
             slug_one);
    ASSERT_TRUE(gc_exec(db, sql));

    hyp_pipeline_incremental_test_reset_faults();
    ASSERT_EQ(gc_index(two, db), 0);
    ASSERT_EQ((int)hyp_pipeline_incremental_test_last_route(),
              (int)HYP_INCREMENTAL_ROUTE_FORCED_FULL);

    hyp_store_t *s = hyp_store_open_path_query(db);
    ASSERT_NOT_NULL(s);
    hyp_project_t info = {0};
    ASSERT_EQ(hyp_store_get_project(s, slug_one, &info), HYP_STORE_OK);
    ASSERT_NOT_NULL(info.indexed_at);
    ASSERT_STR_EQ(info.indexed_at, planted);
    hyp_project_free_fields(&info);
    hyp_store_close(s);

    gc_fix_end(&fix);
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(generation_carry) {
    RUN_TEST(a12_every_table_of_a_published_generation_is_judged);
    RUN_TEST(a12_a_judgement_carries_its_reason_and_an_unknown_table_carries_none);
    RUN_TEST(a12_publication_refuses_a_table_it_cannot_judge);
    RUN_TEST(a12_workspace_registry_survives_a_full_index_of_a_member);
    RUN_TEST(a12_three_members_assemble_into_one_workspace_store);
    RUN_TEST(a12_a_declared_workspace_indexes_every_member_from_one_call);
    RUN_TEST(a12_a_carried_row_keeps_its_own_timestamps);
    RUN_TEST(a12_another_members_indexed_at_is_not_restamped);
}
