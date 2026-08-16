/*
 * test_workspace_resolve.c — NEXT-STEPS §4 Phase 1, units A1 + A6.
 *
 * The two assertion rows the plan pins on A6, plus A1's own:
 *
 *   1. deterministic workspace id — two machines, same repo set, no TOML →
 *      identical workspace id (and never "the first repo's slug").
 *   2. slug collision refused — a workspace offered /a/b and /a-b refuses,
 *      NAMING BOTH PATHS.
 *   3. migration is identity — an address computed before A1's migration
 *      equals the address computed after, on a real store, and the store is
 *      the byte-identical <cache>/<slug>.db file.
 *
 * Slugs come from the REAL slug function (hyp_project_name_from_path) and
 * addresses from the REAL C1 contract (identity.h) — asserting against
 * hand-written strings would only prove the literal was typed twice.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/constants.h"
#include "foundation/identity.h"
#include "pipeline/pipeline.h"
#include "store/store.h"
#include "store/workspace_resolve.h"

#include <stdlib.h>
#include <string.h>

/* ── Fixture: a scratch tree + a redirected cache ─────────────────
 * HYP_CACHE_DIR points into the fixture for the duration, so no test can
 * touch the user's real cache. Same pattern as test_cross_repo.c. */

typedef struct {
    char root[256];  /* mkdtemp scratch: fixture repos live under here */
    char cache[512]; /* <root>/cache — what HYP_CACHE_DIR points at */
    char *saved_cache;
} wsr_fix_t;

static bool wsr_fix_begin(wsr_fix_t *f) {
    memset(f, 0, sizeof(*f));
    const char *saved = getenv("HYP_CACHE_DIR");
    if (saved) {
        f->saved_cache = strdup(saved);
        if (!f->saved_cache) {
            return false;
        }
    }
    snprintf(f->root, sizeof(f->root), "/tmp/hyp-wsr-XXXXXX");
    if (hyp_mkdtemp(f->root) == NULL) {
        return false;
    }
    snprintf(f->cache, sizeof(f->cache), "%s/cache", f->root);
    return th_mkdir_p(f->cache) == 0 && hyp_setenv("HYP_CACHE_DIR", f->cache, 1) == 0;
}

static void wsr_fix_end(wsr_fix_t *f) {
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

/* The slug the indexer would use for this root — the one source of truth. */
static bool wsr_slug_of(const char *root, char *out, size_t out_sz) {
    char *slug = hyp_project_name_from_path(root);
    if (!slug) {
        return false;
    }
    bool ok = (size_t)snprintf(out, out_sz, "%s", slug) < out_sz;
    free(slug);
    return ok;
}

/* ── Providers for the seam ──────────────────────────────────────── */

typedef struct {
    const char *paths[HYP_WSR_MAX_REPOS];
    int count;
} wsr_fake_feed_t;

static hyp_wsr_provider_rc_t wsr_feed_resolve(void *ctx, const char *start_dir,
                                              hyp_wsr_member_spec_t *out, int max, int *count) {
    (void)start_dir;
    const wsr_fake_feed_t *feed = ctx;
    if (feed->count > max) {
        return HYP_WSR_PROVIDER_ERROR;
    }
    for (int i = 0; i < feed->count; i++) {
        snprintf(out[i].path, sizeof(out[i].path), "%s", feed->paths[i]);
        out[i].role[0] = '\0';
    }
    *count = feed->count;
    return HYP_WSR_PROVIDER_FOUND;
}

static hyp_wsr_provider_rc_t wsr_none_resolve(void *ctx, const char *start_dir,
                                              hyp_wsr_member_spec_t *out, int max, int *count) {
    (void)ctx;
    (void)start_dir;
    (void)out;
    (void)max;
    *count = 0;
    return HYP_WSR_PROVIDER_NONE;
}

static hyp_wsr_provider_rc_t wsr_error_resolve(void *ctx, const char *start_dir,
                                               hyp_wsr_member_spec_t *out, int max, int *count) {
    (void)ctx;
    (void)start_dir;
    (void)out;
    (void)max;
    *count = 0;
    return HYP_WSR_PROVIDER_ERROR;
}

/* ════════════════════════════════════════════════════════════════════
 * Zero-config: this repo, a workspace of one, role member (B4's default)
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_zero_config_is_a_workspace_of_one) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    const char *repo = TH_PATH(fix.root, "repo");
    ASSERT_EQ(th_mkdir_p(repo), 0);

    hyp_wsr_resolved_t ws;
    char err[HYP_SZ_1K] = "";
    hyp_wsr_err_t rc = hyp_wsr_resolve(repo, NULL, NULL, &ws, err, sizeof(err));
    ASSERT_EQ(rc, HYP_WSR_OK);
    ASSERT_EQ(ws.source, HYP_WSR_SOURCE_ZERO_CONFIG);
    ASSERT_EQ(ws.member_count, 1);

    /* The member slug is THE project slug, from the one slug function. */
    char slug[HYP_ADDR_SLUG_MAX + 1];
    ASSERT_TRUE(wsr_slug_of(ws.members[0].root, slug, sizeof(slug)));
    ASSERT_STR_EQ(ws.members[0].slug, slug);

    /* The workspace of one is named by its only member — the id every
     * existing per-repo database already answers to. */
    ASSERT_STR_EQ(ws.id, slug);

    /* B4: the zero-config repo IS declared, role member. */
    ASSERT_STR_EQ(ws.members[0].role, HYP_WSR_ROLE_MEMBER);

    wsr_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Assertion row: two machines, same repo set, no TOML → identical id
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_two_machines_same_repo_set_no_toml_agree_on_the_id) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char alpha[512];
    char beta[512];
    char gamma[512];
    snprintf(alpha, sizeof(alpha), "%s/alpha", fix.root);
    snprintf(beta, sizeof(beta), "%s/beta", fix.root);
    snprintf(gamma, sizeof(gamma), "%s/gamma", fix.root);
    ASSERT_EQ(th_mkdir_p(alpha), 0);
    ASSERT_EQ(th_mkdir_p(beta), 0);
    ASSERT_EQ(th_mkdir_p(gamma), 0);

    /* Machine one enumerates its feed one way; machine two another. The SET
     * is identical; nothing else may matter. */
    wsr_fake_feed_t feed_one = {{alpha, beta, gamma}, 3};
    wsr_fake_feed_t feed_two = {{gamma, alpha, beta}, 3};
    hyp_wsr_provider_t machine_one = {"machine-one", wsr_feed_resolve, &feed_one};
    hyp_wsr_provider_t machine_two = {"machine-two", wsr_feed_resolve, &feed_two};

    hyp_wsr_resolved_t ws_one;
    hyp_wsr_resolved_t ws_two;
    ASSERT_EQ(hyp_wsr_resolve(alpha, NULL, &machine_one, &ws_one, NULL, 0), HYP_WSR_OK);
    ASSERT_EQ(hyp_wsr_resolve(alpha, NULL, &machine_two, &ws_two, NULL, 0), HYP_WSR_OK);
    ASSERT_EQ(ws_one.source, HYP_WSR_SOURCE_PROVIDER);
    ASSERT_EQ(ws_one.member_count, 3);
    ASSERT_EQ(ws_two.member_count, 3);

    /* The row itself. */
    ASSERT_STR_EQ(ws_one.id, ws_two.id);

    /* And the forbidden default: the id is a function of the SET, never any
     * single member's slug — least of all whichever came first. */
    for (int i = 0; i < ws_one.member_count; i++) {
        ASSERT_STR_NEQ(ws_one.id, ws_one.members[i].slug);
    }

    /* Deterministic enumeration too: both machines hold the members in the
     * same (slug-sorted) order. */
    for (int i = 0; i < ws_one.member_count; i++) {
        ASSERT_STR_EQ(ws_one.members[i].slug, ws_two.members[i].slug);
    }

    wsr_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Assertion row: /a/b and /a-b refuse, naming both paths
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_slug_collision_is_refused_naming_both_paths) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char nested[512];
    char dashed[512];
    snprintf(nested, sizeof(nested), "%s/a/b", fix.root);
    snprintf(dashed, sizeof(dashed), "%s/a-b", fix.root);
    ASSERT_EQ(th_mkdir_p(nested), 0);
    ASSERT_EQ(th_mkdir_p(dashed), 0);

    /* Both slug to the same name — prove it with the real slug function
     * before asserting the refusal, so this test cannot rot into asserting a
     * collision that no longer exists. */
    char slug_nested[HYP_ADDR_SLUG_MAX + 1];
    char slug_dashed[HYP_ADDR_SLUG_MAX + 1];
    char canon_nested[HYP_SZ_4K];
    char canon_dashed[HYP_SZ_4K];
    ASSERT_TRUE(hyp_canonical_path(nested, canon_nested, sizeof(canon_nested)));
    ASSERT_TRUE(hyp_canonical_path(dashed, canon_dashed, sizeof(canon_dashed)));
    hyp_normalize_path_sep(canon_nested);
    hyp_normalize_path_sep(canon_dashed);
    ASSERT_TRUE(wsr_slug_of(canon_nested, slug_nested, sizeof(slug_nested)));
    ASSERT_TRUE(wsr_slug_of(canon_dashed, slug_dashed, sizeof(slug_dashed)));
    ASSERT_STR_EQ(slug_nested, slug_dashed);

    wsr_fake_feed_t feed = {{nested, dashed}, 2};
    hyp_wsr_provider_t provider = {"collide", wsr_feed_resolve, &feed};
    hyp_wsr_resolved_t ws;
    char err[HYP_SZ_1K] = "";
    hyp_wsr_err_t rc = hyp_wsr_resolve(nested, NULL, &provider, &ws, err, sizeof(err));
    ASSERT_EQ(rc, HYP_WSR_ERR_SLUG_COLLISION);

    /* Naming BOTH paths — never "a collision occurred". */
    ASSERT_NOT_NULL(strstr(err, canon_nested));
    ASSERT_NOT_NULL(strstr(err, canon_dashed));
    ASSERT_NOT_NULL(strstr(err, slug_nested));

    /* On refusal the caller holds an empty workspace, not a half-built one. */
    ASSERT_EQ(ws.member_count, 0);
    ASSERT_EQ((int)ws.id[0], 0);

    wsr_fix_end(&fix);
    PASS();
}

/* The store end of the same gate — the one A1's migration inherits. */
TEST(wsr_store_bind_refuses_the_same_collision) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    hyp_store_t *s = hyp_store_open_path(TH_PATH(fix.cache, "gate.db"));
    ASSERT_NOT_NULL(s);

    hyp_workspace_repo_t rows[2] = {
        {"a-b", "/fix/a/b", NULL},
        {"a-b", "/fix/a-b", NULL},
    };
    char err[HYP_SZ_1K] = "";
    ASSERT_EQ(hyp_store_workspace_bind(s, "acme", rows, 2, err, sizeof(err)), HYP_STORE_ERR);
    ASSERT_NOT_NULL(strstr(err, "/fix/a/b"));
    ASSERT_NOT_NULL(strstr(err, "/fix/a-b"));

    /* The refusal wrote nothing: the registry is still absent. */
    char id[HYP_SZ_512];
    ASSERT_EQ(hyp_store_workspace_id(s, id, sizeof(id)), HYP_STORE_NOT_FOUND);

    hyp_store_close(s);
    wsr_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Precedence: explicit TOML governs, names, and never consults the provider
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_toml_governs_names_and_short_circuits_the_provider) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char backend[512];
    char frontend[512];
    snprintf(backend, sizeof(backend), "%s/backend", fix.root);
    snprintf(frontend, sizeof(frontend), "%s/frontend", fix.root);
    ASSERT_EQ(th_mkdir_p(backend), 0);
    ASSERT_EQ(th_mkdir_p(frontend), 0);

    char toml_path[512];
    snprintf(toml_path, sizeof(toml_path), "%s/%s", fix.root, HYP_WSR_TOML_NAME);
    char toml[HYP_SZ_1K];
    snprintf(toml, sizeof(toml),
             "# workspace file\n"
             "name = \"acme\"\n"
             "\n"
             "[[repos]]\n"
             "path = \"%s\"   # absolute\n"
             "role = \"member\"\n"
             "[[repos]]\n"
             "path = \"frontend\"  # relative to this file's directory\n",
             backend);
    ASSERT_EQ(th_write_file(toml_path, toml), 0);

    /* A provider that would REFUSE if consulted: precedence 1 must never
     * reach precedence 2. */
    hyp_wsr_provider_t exploding = {"exploding", wsr_error_resolve, NULL};

    hyp_wsr_resolved_t ws;
    char err[HYP_SZ_1K] = "";
    hyp_wsr_err_t rc = hyp_wsr_resolve(backend, toml_path, &exploding, &ws, err, sizeof(err));
    ASSERT_EQ(rc, HYP_WSR_OK);
    ASSERT_EQ(ws.source, HYP_WSR_SOURCE_TOML);
    ASSERT_EQ(ws.member_count, 2);
    ASSERT_STR_EQ(ws.id, "acme");

    /* Members are slug-sorted; find each and check declared vs absent role. */
    for (int i = 0; i < 2; i++) {
        if (strstr(ws.members[i].root, "backend") != NULL) {
            ASSERT_STR_EQ(ws.members[i].role, HYP_WSR_ROLE_MEMBER);
        } else {
            ASSERT_NOT_NULL(strstr(ws.members[i].root, "frontend"));
            /* Undeclared role stays ABSENT for A2-A5, never defaulted here. */
            ASSERT_EQ((int)ws.members[i].role[0], 0);
        }
    }
    ASSERT_TRUE(strcmp(ws.members[0].slug, ws.members[1].slug) < 0);

    wsr_fix_end(&fix);
    PASS();
}

TEST(wsr_toml_without_a_name_derives_the_id_from_the_member_set) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char one[512];
    char two[512];
    snprintf(one, sizeof(one), "%s/one", fix.root);
    snprintf(two, sizeof(two), "%s/two", fix.root);
    ASSERT_EQ(th_mkdir_p(one), 0);
    ASSERT_EQ(th_mkdir_p(two), 0);

    /* The same member set declared in opposite orders must produce the same
     * id — declaration order is machine A vs machine B in file form. */
    char fwd[512];
    char rev[512];
    snprintf(fwd, sizeof(fwd), "%s/fwd.toml", fix.root);
    snprintf(rev, sizeof(rev), "%s/rev.toml", fix.root);
    char body[HYP_SZ_1K];
    snprintf(body, sizeof(body), "[[repos]]\npath = \"%s\"\n[[repos]]\npath = \"%s\"\n", one, two);
    ASSERT_EQ(th_write_file(fwd, body), 0);
    snprintf(body, sizeof(body), "[[repos]]\npath = \"%s\"\n[[repos]]\npath = \"%s\"\n", two, one);
    ASSERT_EQ(th_write_file(rev, body), 0);

    hyp_wsr_resolved_t ws_fwd;
    hyp_wsr_resolved_t ws_rev;
    ASSERT_EQ(hyp_wsr_resolve(one, fwd, NULL, &ws_fwd, NULL, 0), HYP_WSR_OK);
    ASSERT_EQ(hyp_wsr_resolve(one, rev, NULL, &ws_rev, NULL, 0), HYP_WSR_OK);
    ASSERT_STR_EQ(ws_fwd.id, ws_rev.id);
    ASSERT_EQ(strncmp(ws_fwd.id, "ws-", 3), 0);

    /* One member and no name: the workspace of one is named by its member,
     * the same rule the zero-config path uses. */
    char solo[512];
    snprintf(solo, sizeof(solo), "%s/solo.toml", fix.root);
    snprintf(body, sizeof(body), "[[repos]]\npath = \"%s\"\n", one);
    ASSERT_EQ(th_write_file(solo, body), 0);
    hyp_wsr_resolved_t ws_solo;
    ASSERT_EQ(hyp_wsr_resolve(one, solo, NULL, &ws_solo, NULL, 0), HYP_WSR_OK);
    ASSERT_STR_EQ(ws_solo.id, ws_solo.members[0].slug);

    wsr_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Fail closed: a governing TOML that cannot be honoured REFUSES — it never
 * falls through and quietly indexes a declared workspace as a repo of one
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_toml_refuses_rather_than_guessing) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char repo[512];
    snprintf(repo, sizeof(repo), "%s/repo", fix.root);
    ASSERT_EQ(th_mkdir_p(repo), 0);

    char path[512];
    char body[HYP_SZ_1K];
    hyp_wsr_resolved_t ws;
    char err[HYP_SZ_1K];

    /* A named-but-missing file is a refusal, not a fall-through. */
    snprintf(path, sizeof(path), "%s/absent.toml", fix.root);
    ASSERT_EQ(hyp_wsr_resolve(repo, path, NULL, &ws, err, sizeof(err)), HYP_WSR_ERR_TOML_READ);
    ASSERT_EQ(ws.member_count, 0);

    /* An unknown key is a typo'd repo waiting to be dropped — refused,
     * naming the line. */
    snprintf(path, sizeof(path), "%s/badkey.toml", fix.root);
    snprintf(body, sizeof(body), "[[repos]]\npath = \"%s\"\nrolle = \"member\"\n", repo);
    ASSERT_EQ(th_write_file(path, body), 0);
    ASSERT_EQ(hyp_wsr_resolve(repo, path, NULL, &ws, err, sizeof(err)), HYP_WSR_ERR_TOML_PARSE);
    ASSERT_NOT_NULL(strstr(err, "line 3"));
    ASSERT_NOT_NULL(strstr(err, "rolle"));

    /* A role outside the vocabulary. */
    snprintf(path, sizeof(path), "%s/badrole.toml", fix.root);
    snprintf(body, sizeof(body), "[[repos]]\npath = \"%s\"\nrole = \"banana\"\n", repo);
    ASSERT_EQ(th_write_file(path, body), 0);
    ASSERT_EQ(hyp_wsr_resolve(repo, path, NULL, &ws, err, sizeof(err)), HYP_WSR_ERR_ROLE);
    ASSERT_NOT_NULL(strstr(err, "banana"));

    /* A [[repos]] with no path. */
    snprintf(path, sizeof(path), "%s/nopath.toml", fix.root);
    ASSERT_EQ(th_write_file(path, "[[repos]]\nrole = \"member\"\n"), 0);
    ASSERT_EQ(hyp_wsr_resolve(repo, path, NULL, &ws, err, sizeof(err)), HYP_WSR_ERR_TOML_PARSE);

    /* A file with no repos at all. */
    snprintf(path, sizeof(path), "%s/empty.toml", fix.root);
    ASSERT_EQ(th_write_file(path, "name = \"acme\"\n"), 0);
    ASSERT_EQ(hyp_wsr_resolve(repo, path, NULL, &ws, err, sizeof(err)), HYP_WSR_ERR_NO_REPOS);

    /* A name that is not a valid slug (the id is a database filename). */
    snprintf(path, sizeof(path), "%s/badname.toml", fix.root);
    snprintf(body, sizeof(body), "name = \"a/b\"\n[[repos]]\npath = \"%s\"\n", repo);
    ASSERT_EQ(th_write_file(path, body), 0);
    ASSERT_EQ(hyp_wsr_resolve(repo, path, NULL, &ws, err, sizeof(err)), HYP_WSR_ERR_NAME);

    /* A member that does not exist on disk. */
    snprintf(path, sizeof(path), "%s/ghost.toml", fix.root);
    snprintf(body, sizeof(body), "[[repos]]\npath = \"%s/no-such-repo\"\n", fix.root);
    ASSERT_EQ(th_write_file(path, body), 0);
    ASSERT_EQ(hyp_wsr_resolve(repo, path, NULL, &ws, err, sizeof(err)), HYP_WSR_ERR_REPO_PATH);

    wsr_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The provider seam: absent falls through, error refuses
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_provider_absence_falls_through_error_refuses) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char repo[512];
    snprintf(repo, sizeof(repo), "%s/repo", fix.root);
    ASSERT_EQ(th_mkdir_p(repo), 0);

    /* NONE = "no workspace known here": zero-config proceeds. */
    hyp_wsr_provider_t none = {"multica", wsr_none_resolve, NULL};
    hyp_wsr_resolved_t ws;
    ASSERT_EQ(hyp_wsr_resolve(repo, NULL, &none, &ws, NULL, 0), HYP_WSR_OK);
    ASSERT_EQ(ws.source, HYP_WSR_SOURCE_ZERO_CONFIG);
    ASSERT_EQ(ws.member_count, 1);

    /* ERROR = "could not tell": refused, never read as "no workspace". */
    hyp_wsr_provider_t broken = {"multica", wsr_error_resolve, NULL};
    char err[HYP_SZ_1K] = "";
    ASSERT_EQ(hyp_wsr_resolve(repo, NULL, &broken, &ws, err, sizeof(err)), HYP_WSR_ERR_PROVIDER);
    ASSERT_NOT_NULL(strstr(err, "multica"));
    ASSERT_EQ(ws.member_count, 0);

    wsr_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * A1's own row: migration is the identity function on every existing
 * address — same file, same addresses, on a real store
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_migration_is_identity_on_an_existing_per_repo_store) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char repo[512];
    snprintf(repo, sizeof(repo), "%s/repo", fix.root);
    ASSERT_EQ(th_mkdir_p(repo), 0);
    char canon[HYP_SZ_4K];
    ASSERT_TRUE(hyp_canonical_path(repo, canon, sizeof(canon)));
    hyp_normalize_path_sep(canon);

    char slug[HYP_ADDR_SLUG_MAX + 1];
    ASSERT_TRUE(wsr_slug_of(canon, slug, sizeof(slug)));

    /* BEFORE: the store exactly as today's per-repo path creates it, with a
     * real node under a real qualified name, and its C1 address. */
    char *qn = hyp_pipeline_fqn_compute(slug, "src/core.c", "init");
    ASSERT_NOT_NULL(qn);
    char addr_before[HYP_ADDR_MAX + 1];
    {
        hyp_store_t *s = hyp_store_open(slug);
        ASSERT_NOT_NULL(s);
        ASSERT_EQ(hyp_store_upsert_project(s, slug, canon), HYP_STORE_OK);
        hyp_node_t node = {0};
        node.project = slug;
        node.label = "Function";
        node.name = "init";
        node.qualified_name = qn;
        node.file_path = "src/core.c";
        node.start_line = 3;
        node.end_line = 9;
        ASSERT_GT(hyp_store_upsert_node(s, &node), 0);

        hyp_addr_t a;
        /* workspace == NULL: a workspace of one on the SAME code path. */
        ASSERT_EQ(hyp_addr_init(&a, NULL, slug, qn), HYP_ADDR_OK);
        ASSERT_TRUE(hyp_addr_format(&a, addr_before, sizeof(addr_before)));
        hyp_store_close(s);
    }

    /* One file, and it is the file today's naming produces. */
    char expected_db[512];
    snprintf(expected_db, sizeof(expected_db), "%s/%s.db", fix.cache, slug);
    ASSERT_TRUE(hyp_file_exists(expected_db));

    /* AFTER: resolve zero-config and open through A1's workspace path — the
     * migration happens here, and it must change nothing but the registry. */
    hyp_wsr_resolved_t ws;
    char err[HYP_SZ_1K] = "";
    ASSERT_EQ(hyp_wsr_resolve(repo, NULL, NULL, &ws, err, sizeof(err)), HYP_WSR_OK);
    hyp_store_t *s = hyp_wsr_store_open(&ws, err, sizeof(err));
    ASSERT_NOT_NULL(s);

    /* Same file — one layout, not a second one beside it. */
    ASSERT_STR_EQ(hyp_store_db_path(s), expected_db);

    /* The node is still there under the same project and QN. */
    hyp_node_t found = {0};
    ASSERT_EQ(hyp_store_find_node_by_qn(s, slug, qn, &found), HYP_STORE_OK);
    hyp_node_free_fields(&found);

    /* The address computed after migration equals the address computed
     * before: hyp1:<slug>/<slug>#<qn>, byte for byte. */
    char addr_after[HYP_ADDR_MAX + 1];
    hyp_addr_t a;
    ASSERT_EQ(hyp_addr_init(&a, ws.id, slug, qn), HYP_ADDR_OK);
    ASSERT_TRUE(hyp_addr_format(&a, addr_after, sizeof(addr_after)));
    ASSERT_STR_EQ(addr_before, addr_after);

    /* And the migration wrote exactly the registry: id + one member row. */
    char id[HYP_SZ_512];
    ASSERT_EQ(hyp_store_workspace_id(s, id, sizeof(id)), HYP_STORE_OK);
    ASSERT_STR_EQ(id, slug);
    hyp_workspace_repo_t *rows = NULL;
    int row_count = 0;
    ASSERT_EQ(hyp_store_workspace_repos(s, &rows, &row_count), HYP_STORE_OK);
    ASSERT_EQ(row_count, 1);
    ASSERT_STR_EQ(rows[0].slug, slug);
    ASSERT_STR_EQ(rows[0].root_path, canon);
    ASSERT_NOT_NULL(rows[0].role);
    ASSERT_STR_EQ(rows[0].role, HYP_WSR_ROLE_MEMBER);
    hyp_store_free_workspace_repos(rows, row_count);

    hyp_store_close(s);
    free(qn);
    wsr_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Registry semantics: absent derives, a different id refuses, and the
 * integrity ceiling follows the registry
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_registry_absent_is_a_per_repo_store_not_an_error) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    hyp_store_t *s = hyp_store_open_path(TH_PATH(fix.cache, "legacy.db"));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(hyp_store_upsert_project(s, "legacy", "/fix/legacy"), HYP_STORE_OK);

    /* ABSENT means "a pre-A1 per-repo store" — NOT_FOUND, never an error and
     * never an empty id. */
    char id[HYP_SZ_512];
    ASSERT_EQ(hyp_store_workspace_id(s, id, sizeof(id)), HYP_STORE_NOT_FOUND);
    hyp_workspace_repo_t *rows = NULL;
    int count = 0;
    ASSERT_EQ(hyp_store_workspace_repos(s, &rows, &count), HYP_STORE_NOT_FOUND);

    /* Binding is the migration; after it the registry answers. */
    hyp_workspace_repo_t member = {"legacy", "/fix/legacy", HYP_WSR_ROLE_MEMBER};
    ASSERT_EQ(hyp_store_workspace_bind(s, "legacy", &member, 1, NULL, 0), HYP_STORE_OK);
    ASSERT_EQ(hyp_store_workspace_id(s, id, sizeof(id)), HYP_STORE_OK);
    ASSERT_STR_EQ(id, "legacy");

    /* Idempotent: binding again changes nothing and refuses nothing. */
    ASSERT_EQ(hyp_store_workspace_bind(s, "legacy", &member, 1, NULL, 0), HYP_STORE_OK);

    hyp_store_close(s);
    wsr_fix_end(&fix);
    PASS();
}

TEST(wsr_store_bind_refuses_a_different_workspace_id) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    hyp_store_t *s = hyp_store_open_path(TH_PATH(fix.cache, "bound.db"));
    ASSERT_NOT_NULL(s);

    hyp_workspace_repo_t member = {"repo-one", "/fix/repo-one", NULL};
    ASSERT_EQ(hyp_store_workspace_bind(s, "acme", &member, 1, NULL, 0), HYP_STORE_OK);

    /* Rebinding under another name would silently re-home every address in
     * the file — refused, naming both ids. */
    char err[HYP_SZ_1K] = "";
    ASSERT_EQ(hyp_store_workspace_bind(s, "other", &member, 1, err, sizeof(err)), HYP_STORE_ERR);
    ASSERT_NOT_NULL(strstr(err, "acme"));
    ASSERT_NOT_NULL(strstr(err, "other"));

    char id[HYP_SZ_512];
    ASSERT_EQ(hyp_store_workspace_id(s, id, sizeof(id)), HYP_STORE_OK);
    ASSERT_STR_EQ(id, "acme");

    /* A slug re-registered from a DIFFERENT root is the collision gate
     * separated in time. */
    hyp_workspace_repo_t moved = {"repo-one", "/fix/elsewhere/repo-one", NULL};
    err[0] = '\0';
    ASSERT_EQ(hyp_store_workspace_bind(s, "acme", &moved, 1, err, sizeof(err)), HYP_STORE_ERR);
    ASSERT_NOT_NULL(strstr(err, "/fix/repo-one"));
    ASSERT_NOT_NULL(strstr(err, "/fix/elsewhere/repo-one"));

    hyp_store_close(s);
    wsr_fix_end(&fix);
    PASS();
}

TEST(wsr_integrity_ceiling_derives_from_the_registry) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }

    /* A registered workspace of 4 legitimately holds 8 project rows (each
     * member plus its ::missed shadow) — that must PASS integrity. */
    hyp_store_t *s = hyp_store_open_path(TH_PATH(fix.cache, "wide.db"));
    ASSERT_NOT_NULL(s);
    hyp_workspace_repo_t members[4] = {
        {"m-one", "/fix/m-one", NULL},
        {"m-two", "/fix/m-two", NULL},
        {"m-three", "/fix/m-three", NULL},
        {"m-four", "/fix/m-four", NULL},
    };
    ASSERT_EQ(hyp_store_workspace_bind(s, "wide", members, 4, NULL, 0), HYP_STORE_OK);
    for (int i = 0; i < 8; i++) {
        char name[HYP_SZ_64];
        snprintf(name, sizeof(name), "proj-%d", i);
        ASSERT_EQ(hyp_store_upsert_project(s, name, "/fix/p"), HYP_STORE_OK);
    }
    ASSERT_TRUE(hyp_store_check_integrity(s));
    hyp_store_close(s);

    /* The same 8 rows WITHOUT a registry is still what it always was: a
     * corrupt per-repo store. The old ceiling did not move. */
    hyp_store_t *legacy = hyp_store_open_path(TH_PATH(fix.cache, "narrow.db"));
    ASSERT_NOT_NULL(legacy);
    for (int i = 0; i < 8; i++) {
        char name[HYP_SZ_64];
        snprintf(name, sizeof(name), "proj-%d", i);
        ASSERT_EQ(hyp_store_upsert_project(legacy, name, "/fix/p"), HYP_STORE_OK);
    }
    ASSERT_FALSE(hyp_store_check_integrity(legacy));
    hyp_store_close(legacy);

    wsr_fix_end(&fix);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * TOML discovery: the nearest file governs
 * ════════════════════════════════════════════════════════════════════ */

TEST(wsr_toml_find_returns_the_nearest_ancestor) {
    wsr_fix_t fix;
    if (!wsr_fix_begin(&fix)) {
        FAIL("fixture setup");
    }
    char deep[512];
    snprintf(deep, sizeof(deep), "%s/ws/sub/repo", fix.root);
    ASSERT_EQ(th_mkdir_p(deep), 0);
    char upper[512];
    snprintf(upper, sizeof(upper), "%s/ws/%s", fix.root, HYP_WSR_TOML_NAME);
    ASSERT_EQ(th_write_file(upper, "[[repos]]\npath = \".\"\n"), 0);

    char found[HYP_SZ_4K];
    ASSERT_TRUE(hyp_wsr_toml_find(deep, found, sizeof(found)));
    ASSERT_NOT_NULL(strstr(found, "/ws/"));
    ASSERT_NOT_NULL(strstr(found, HYP_WSR_TOML_NAME));

    /* A nearer file wins over the ancestor. */
    char lower[512];
    snprintf(lower, sizeof(lower), "%s/ws/sub/%s", fix.root, HYP_WSR_TOML_NAME);
    ASSERT_EQ(th_write_file(lower, "[[repos]]\npath = \".\"\n"), 0);
    ASSERT_TRUE(hyp_wsr_toml_find(deep, found, sizeof(found)));
    ASSERT_NOT_NULL(strstr(found, "/ws/sub/"));

    wsr_fix_end(&fix);
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(workspace_resolve) {
    RUN_TEST(wsr_zero_config_is_a_workspace_of_one);
    RUN_TEST(wsr_two_machines_same_repo_set_no_toml_agree_on_the_id);
    RUN_TEST(wsr_slug_collision_is_refused_naming_both_paths);
    RUN_TEST(wsr_store_bind_refuses_the_same_collision);
    RUN_TEST(wsr_toml_governs_names_and_short_circuits_the_provider);
    RUN_TEST(wsr_toml_without_a_name_derives_the_id_from_the_member_set);
    RUN_TEST(wsr_toml_refuses_rather_than_guessing);
    RUN_TEST(wsr_provider_absence_falls_through_error_refuses);
    RUN_TEST(wsr_migration_is_identity_on_an_existing_per_repo_store);
    RUN_TEST(wsr_registry_absent_is_a_per_repo_store_not_an_error);
    RUN_TEST(wsr_store_bind_refuses_a_different_workspace_id);
    RUN_TEST(wsr_integrity_ceiling_derives_from_the_registry);
    RUN_TEST(wsr_toml_find_returns_the_nearest_ancestor);
}
