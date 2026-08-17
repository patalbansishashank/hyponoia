/*
 * test_comment_migrate.c — relocating comment prose into the decision store
 * (Track E, unit E2).
 *
 * The organising claim is this unit's assertion row: MIGRATE TWICE, into two
 * fresh stores, then merge — the union must hold exactly what one run holds,
 * with zero duplicates, and each record's author and timestamp must be the
 * ones git blame gives, on a fixture whose blame answers are known because the
 * test wrote them.
 *
 * Zero duplicates is not a tidiness property. The record id commits to author
 * and timestamp, so attribution decides identity: a migrator that authored the
 * corpus as itself at its own clock would mint fresh ids on every machine, and
 * a union that cannot recognise two observations of one comment holds N copies
 * of it with no way to tell them from N different comments — permanently,
 * because records are never mutated. The `now()` attribution is therefore kept
 * reachable behind a marked seam in the script and MUTATED HERE, so the
 * duplicate is observed rather than argued about.
 *
 * The other half is "relocate, do not delete": every sentence the code gives
 * up must be retrievable afterwards. That is asserted from the store's own
 * read path, against prose the fixture put in a comment, and the same test is
 * what fails if a future edit deletes a comment instead of moving it.
 *
 * Fixtures are a throwaway git repo built at run time with fixed authors and
 * fixed commit dates. Committing files full of banned comment forms to the
 * real tree would seed the corpus this unit exists to drain.
 *
 * These tests shell out to git and python3, so they SKIP_PLATFORM on Windows
 * for the same reason test_git_context.c does. A missing tool elsewhere is a
 * FAIL: a test that cannot establish its preconditions has not been skipped.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include <foundation/constants.h>
#include <foundation/identity.h>
#include <memory/comment_migrate.h>
#include <store/record_store.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>

/* Two authors, two fixed commit times. Fixed because the assertion is
 * "timestamp == the commit's time", and a test that reads a clock to decide
 * what it expects is asserting its own arithmetic. */
#define CM_A_NAME "Ada Known"
#define CM_A_MAIL "ada@known.invalid"
#define CM_A_EPOCH "1700000000"
#define CM_A_MS "1700000000000"

#define CM_B_NAME "Bo Later"
#define CM_B_MAIL "bo@later.invalid"
#define CM_B_EPOCH "1750000000"
#define CM_B_MS "1750000000000"

/* The sentence commit two rewrites, so blame has two answers inside one block
 * and the migration has to choose the later one. */
#define CM_EDITED_MARK "REWRITTEN BY THE SECOND COMMIT"

/* The exemplar: prose that says which measurement and when, which is exactly
 * what the lint refuses and exactly what must survive the move. */
#define CM_EXEMPLAR "When this was written NO CODE PATH READ dim_mode"

static const char *CM_FIXTURE_C =
    "/*\n"
    " * fixture.c — the provider's dimension mode.\n"
    " *\n"
    " * " CM_EXEMPLAR ", and the wrong value survived three days. See §2.7 for\n"
    " * the measurement that settled it.\n"
    " *\n"
    " * PLACEHOLDER LINE\n"
    " */\n"
    "const char *fixture_note = \"shipped 2026-01-02 per §9.9\"; /* a literal */\n"
    "int fixture_dim_mode(void) {\n"
    "    return 0;\n"
    "}\n";

static const char *CM_SECOND_C =
    "/*\n"
    " * second.c — the second file, so attribution is a per-block answer.\n"
    " *\n"
    " * Frozen 2026-08-16, which is a date and therefore not the comment's to\n"
    " * keep.\n"
    " */\n"
    "int second_thing(void) {\n"
    "    return 1;\n"
    "}\n";

/* ── Shelling out ───────────────────────────────────────────────────────── */

static int cm_run(const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return system(cmd);
}

/* git with identity pinned on both sides: the author fields are the assertion,
 * and an unset committer identity makes `git commit` refuse outright. */
static int cm_commit(const char *dir, const char *name, const char *mail, const char *epoch,
                     const char *message) {
    return cm_run("GIT_AUTHOR_NAME='%s' GIT_AUTHOR_EMAIL='%s' GIT_AUTHOR_DATE='@%s +0000' "
                  "GIT_COMMITTER_NAME='%s' GIT_COMMITTER_EMAIL='%s' "
                  "GIT_COMMITTER_DATE='@%s +0000' "
                  "git -C '%s' -c commit.gpgsign=false commit -q -m '%s' >/dev/null 2>&1",
                  name, mail, epoch, name, mail, epoch, dir, message);
}

/* Build the throwaway repo. Returns 0 on success. */
static int cm_make_repo(const char *dir) {
    char file[HYP_PATH_MAX];
    if (th_mkdir_p(dir) != 0) {
        return -1;
    }
    if (cm_run("git -C '%s' init -q >/dev/null 2>&1", dir) != 0) {
        return -1;
    }
    (void)snprintf(file, sizeof(file), "%s/fixture.c", dir);
    if (th_write_file(file, CM_FIXTURE_C) != 0) {
        return -1;
    }
    (void)snprintf(file, sizeof(file), "%s/second.c", dir);
    if (th_write_file(file, CM_SECOND_C) != 0) {
        return -1;
    }
    if (cm_run("git -C '%s' add -A >/dev/null 2>&1", dir) != 0) {
        return -1;
    }
    if (cm_commit(dir, CM_A_NAME, CM_A_MAIL, CM_A_EPOCH, "first") != 0) {
        return -1;
    }
    /* One line inside the block, by the other author, later. Rewriting the
     * whole file would give blame a single answer and prove nothing. */
    if (cm_run("sed -i 's/PLACEHOLDER LINE/%s/' '%s/fixture.c'", CM_EDITED_MARK, dir) != 0) {
        return -1;
    }
    if (cm_run("git -C '%s' add -A >/dev/null 2>&1", dir) != 0) {
        return -1;
    }
    return cm_commit(dir, CM_B_NAME, CM_B_MAIL, CM_B_EPOCH, "second");
}

/* Run the migrator's extractor over `repo`, writing a manifest to `out`.
 * `script_dir` lets a control point at a mutated copy. */
static int cm_extract(const char *script_dir, const char *repo, const char *out) {
    return cm_run("cd '%s' && python3 '%s/migrate-comments.py' -o '%s' >/dev/null 2>&1", repo,
                  script_dir, out);
}

static char *cm_slurp(const char *path) {
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return NULL;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        (void)fclose(fh);
        return NULL;
    }
    long size = ftell(fh);
    if (size < 0 || fseek(fh, 0, SEEK_SET) != 0) {
        (void)fclose(fh);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        (void)fclose(fh);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, fh);
    (void)fclose(fh);
    buf[got] = '\0';
    return buf;
}

/* The repository's own scripts directory, absolute. Tests run from the repo
 * root; a relative path would break the moment the extractor chdirs into the
 * fixture. */
static bool cm_script_dir(char *out, size_t cap) {
    char cwd[HYP_PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        return false;
    }
    int n = snprintf(out, cap, "%s/scripts", cwd);
    return n > 0 && (size_t)n < cap;
}

/* Ingest `manifest` into a fresh store at `dir`. */
static hyp_comment_migrate_status_t cm_ingest(const char *dir, const char *manifest,
                                             hyp_comment_migrate_stats_t *stats) {
    hyp_record_store_t *store = NULL;
    if (hyp_record_store_open(dir, &store) != HYP_RECORD_STORE_OK) {
        return HYP_COMMENT_MIGRATE_ERR_STORE;
    }
    hyp_comment_migrate_opts_t opts = {.workspace = NULL, .repo = "fixture"};
    char err[512];
    hyp_comment_migrate_status_t st =
        hyp_comment_migrate_run(manifest, &opts, store, stats, err, sizeof(err));
    hyp_record_store_close(store);
    return st;
}

static bool cm_load(const char *dir, hyp_record_set_t **out) {
    hyp_record_store_t *store = NULL;
    if (hyp_record_store_open(dir, &store) != HYP_RECORD_STORE_OK) {
        return false;
    }
    bool ok = hyp_record_store_load(store, out) == HYP_RECORD_STORE_OK;
    hyp_record_store_close(store);
    return ok;
}

/* ════════════════════════════════════════════════════════════════════
 * Attribution: author == blame, timestamp == commit
 * ════════════════════════════════════════════════════════════════════ */

TEST(manifest_attributes_each_block_to_the_commit_that_last_touched_it) {
    char scripts[HYP_PATH_MAX];
    if (!cm_script_dir(scripts, sizeof(scripts))) {
        FAIL("cannot resolve the scripts directory");
    }
    char *base = th_mktempdir("cm_blame");
    if (!base) {
        FAIL("cannot create a temp dir");
    }
    char repo_dir[HYP_PATH_MAX];
    (void)snprintf(repo_dir, sizeof(repo_dir), "%s/repo", base);
    if (cm_make_repo(repo_dir) != 0) {
        th_cleanup(base);
        FAIL("cannot build the fixture repository (git missing?)");
    }
    char manifest[HYP_PATH_MAX];
    (void)snprintf(manifest, sizeof(manifest), "%s/manifest", base);
    if (cm_extract(scripts, repo_dir, manifest) != 0) {
        th_cleanup(base);
        FAIL("the extractor failed (python3 missing?)");
    }
    char *text = cm_slurp(manifest);
    if (!text) {
        th_cleanup(base);
        FAIL("no manifest was written");
    }

    /* fixture.c's block spans two commits; the later one owns it. */
    ASSERT_NOT_NULL(strstr(text, "\n" CM_B_NAME " <" CM_B_MAIL ">\n"));
    ASSERT_NOT_NULL(strstr(text, "timestamp_ms 13\n" CM_B_MS "\n"));
    /* second.c was never re-touched, so it keeps the first author. A single
     * author for both blocks would pass "author == blame" by accident. */
    ASSERT_NOT_NULL(strstr(text, "\n" CM_A_NAME " <" CM_A_MAIL ">\n"));
    ASSERT_NOT_NULL(strstr(text, "timestamp_ms 13\n" CM_A_MS "\n"));

    /* Neither the migrator nor a clock authored anything. */
    ASSERT_NULL(strstr(text, "agent:comment-migrator"));

    /* The prose came out; the string literal's date and section did not. Both
     * previous mechanical passes over this tree failed exactly here. */
    ASSERT_NOT_NULL(strstr(text, CM_EXEMPLAR));
    ASSERT_NULL(strstr(text, "shipped 2026-01-02"));

    free(text);
    th_cleanup(base);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The assertion row: twice, fresh stores, merge, zero duplicates
 * ════════════════════════════════════════════════════════════════════ */

TEST(two_runs_into_fresh_stores_union_to_exactly_one_run) {
    char scripts[HYP_PATH_MAX];
    if (!cm_script_dir(scripts, sizeof(scripts))) {
        FAIL("cannot resolve the scripts directory");
    }
    char *base = th_mktempdir("cm_union");
    if (!base) {
        FAIL("cannot create a temp dir");
    }
    char repo_dir[HYP_PATH_MAX];
    (void)snprintf(repo_dir, sizeof(repo_dir), "%s/repo", base);
    if (cm_make_repo(repo_dir) != 0) {
        th_cleanup(base);
        FAIL("cannot build the fixture repository (git missing?)");
    }

    /* Two independent extractions, as two machines at one commit would do. */
    char m1[HYP_PATH_MAX];
    char m2[HYP_PATH_MAX];
    (void)snprintf(m1, sizeof(m1), "%s/m1", base);
    (void)snprintf(m2, sizeof(m2), "%s/m2", base);
    if (cm_extract(scripts, repo_dir, m1) != 0 || cm_extract(scripts, repo_dir, m2) != 0) {
        th_cleanup(base);
        FAIL("the extractor failed (python3 missing?)");
    }
    char *t1 = cm_slurp(m1);
    char *t2 = cm_slurp(m2);
    if (!t1 || !t2) {
        free(t1);
        free(t2);
        th_cleanup(base);
        FAIL("no manifest was written");
    }
    /* Byte-identical, because every field comes from git rather than from the
     * run. This is the property the union rests on. */
    ASSERT_STR_EQ(t1, t2);
    free(t1);
    free(t2);

    char store_a[HYP_PATH_MAX];
    char store_b[HYP_PATH_MAX];
    (void)snprintf(store_a, sizeof(store_a), "%s/store-a", base);
    (void)snprintf(store_b, sizeof(store_b), "%s/store-b", base);

    hyp_comment_migrate_stats_t sa;
    hyp_comment_migrate_stats_t sb;
    ASSERT_EQ(cm_ingest(store_a, m1, &sa), HYP_COMMENT_MIGRATE_OK);
    ASSERT_EQ(cm_ingest(store_b, m2, &sb), HYP_COMMENT_MIGRATE_OK);
    /* Two comment blocks carry findings; the third comment on the literal's
     * line carries none and is left where it is. */
    ASSERT_EQ((int)sa.items, 2);
    ASSERT_EQ((int)sa.appended, 2);
    ASSERT_EQ((int)sa.absorbed, 0);
    ASSERT_EQ((int)sb.appended, 2);

    hyp_record_set_t *set_a = NULL;
    hyp_record_set_t *set_b = NULL;
    if (!cm_load(store_a, &set_a) || !cm_load(store_b, &set_b)) {
        hyp_record_set_free(set_a);
        hyp_record_set_free(set_b);
        th_cleanup(base);
        FAIL("cannot read back a store just written");
    }
    size_t single = hyp_record_set_count(set_a);
    ASSERT_EQ((int)single, 2);

    char digest_a[HYP_RECORD_ID_LEN + 1];
    char digest_b[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(set_a, digest_a);
    hyp_record_set_digest(set_b, digest_b);
    ASSERT_STR_EQ(digest_a, digest_b);

    /* THE ROW: merging the second machine's store into the first adds nothing. */
    ASSERT_EQ(hyp_record_set_merge(set_a, set_b), HYP_RECORD_OK);
    ASSERT_EQ((int)hyp_record_set_count(set_a), (int)single);
    char digest_merged[HYP_RECORD_ID_LEN + 1];
    hyp_record_set_digest(set_a, digest_merged);
    ASSERT_STR_EQ(digest_merged, digest_a);

    hyp_record_set_free(set_a);
    hyp_record_set_free(set_b);

    /* And the same run into the SAME store is absorbed, not appended (I8). */
    hyp_comment_migrate_stats_t again;
    ASSERT_EQ(cm_ingest(store_a, m1, &again), HYP_COMMENT_MIGRATE_OK);
    ASSERT_EQ((int)again.appended, 0);
    ASSERT_EQ((int)again.absorbed, 2);

    th_cleanup(base);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The control the row exists for: attribution from the clock duplicates
 * ════════════════════════════════════════════════════════════════════ */

TEST(clock_attribution_duplicates_and_blame_attribution_does_not) {
    char scripts[HYP_PATH_MAX];
    if (!cm_script_dir(scripts, sizeof(scripts))) {
        FAIL("cannot resolve the scripts directory");
    }
    char *base = th_mktempdir("cm_control");
    if (!base) {
        FAIL("cannot create a temp dir");
    }
    char repo_dir[HYP_PATH_MAX];
    (void)snprintf(repo_dir, sizeof(repo_dir), "%s/repo", base);
    if (cm_make_repo(repo_dir) != 0) {
        th_cleanup(base);
        FAIL("cannot build the fixture repository (git missing?)");
    }

    /* A copy of the real scripts with the attribution seam mutated. Copying
     * both files matters: the extractor loads the lint's comment view from its
     * own directory, so a half-copy would silently test the wrong pair. */
    char mutant[HYP_PATH_MAX];
    (void)snprintf(mutant, sizeof(mutant), "%s/mutant", base);
    if (th_mkdir_p(mutant) != 0 ||
        cm_run("cp '%s/migrate-comments.py' '%s/lint-comments.py' '%s/'", scripts, scripts,
               mutant) != 0 ||
        cm_run("sed -i 's/^ATTRIBUTION = \"blame\"$/ATTRIBUTION = \"now\"/' "
               "'%s/migrate-comments.py'",
               mutant) != 0) {
        th_cleanup(base);
        FAIL("cannot build the mutated extractor");
    }
    /* A control that did not apply proves nothing. */
    if (cm_run("grep -q '^ATTRIBUTION = \"now\"$' '%s/migrate-comments.py'", mutant) != 0) {
        th_cleanup(base);
        FAIL("the attribution seam did not mutate — the control is inert");
    }

    char m1[HYP_PATH_MAX];
    char m2[HYP_PATH_MAX];
    (void)snprintf(m1, sizeof(m1), "%s/m1", base);
    (void)snprintf(m2, sizeof(m2), "%s/m2", base);
    if (cm_extract(mutant, repo_dir, m1) != 0) {
        th_cleanup(base);
        FAIL("the mutated extractor failed (python3 missing?)");
    }
    /* Far enough apart that two `now()` readings differ; the point is that
     * they CAN differ at all, which blame's answer never does. */
    (void)cm_run("sleep 0.05");
    if (cm_extract(mutant, repo_dir, m2) != 0) {
        th_cleanup(base);
        FAIL("the mutated extractor failed on the second run");
    }

    char *text = cm_slurp(m1);
    if (!text) {
        th_cleanup(base);
        FAIL("no manifest was written");
    }
    ASSERT_NOT_NULL(strstr(text, "agent:comment-migrator"));
    ASSERT_NULL(strstr(text, CM_B_NAME));
    free(text);

    char store_a[HYP_PATH_MAX];
    char store_b[HYP_PATH_MAX];
    (void)snprintf(store_a, sizeof(store_a), "%s/store-a", base);
    (void)snprintf(store_b, sizeof(store_b), "%s/store-b", base);
    hyp_comment_migrate_stats_t sa;
    hyp_comment_migrate_stats_t sb;
    ASSERT_EQ(cm_ingest(store_a, m1, &sa), HYP_COMMENT_MIGRATE_OK);
    ASSERT_EQ(cm_ingest(store_b, m2, &sb), HYP_COMMENT_MIGRATE_OK);

    hyp_record_set_t *set_a = NULL;
    hyp_record_set_t *set_b = NULL;
    if (!cm_load(store_a, &set_a) || !cm_load(store_b, &set_b)) {
        hyp_record_set_free(set_a);
        hyp_record_set_free(set_b);
        th_cleanup(base);
        FAIL("cannot read back a store just written");
    }
    size_t single = hyp_record_set_count(set_a);
    ASSERT_EQ(hyp_record_set_merge(set_a, set_b), HYP_RECORD_OK);
    /* The failure the shipped attribution prevents: one comment, two records,
     * and nothing in the store able to say they are the same comment. */
    ASSERT_GT((int)hyp_record_set_count(set_a), (int)single);
    hyp_record_set_free(set_a);
    hyp_record_set_free(set_b);

    th_cleanup(base);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * Relocate, do not delete
 * ════════════════════════════════════════════════════════════════════ */

TEST(migrated_prose_is_retrievable_by_anchor_afterwards) {
    char scripts[HYP_PATH_MAX];
    if (!cm_script_dir(scripts, sizeof(scripts))) {
        FAIL("cannot resolve the scripts directory");
    }
    char *base = th_mktempdir("cm_read");
    if (!base) {
        FAIL("cannot create a temp dir");
    }
    char repo_dir[HYP_PATH_MAX];
    (void)snprintf(repo_dir, sizeof(repo_dir), "%s/repo", base);
    char manifest[HYP_PATH_MAX];
    (void)snprintf(manifest, sizeof(manifest), "%s/manifest", base);
    char store_dir[HYP_PATH_MAX];
    (void)snprintf(store_dir, sizeof(store_dir), "%s/store", base);
    if (cm_make_repo(repo_dir) != 0) {
        th_cleanup(base);
        FAIL("cannot build the fixture repository (git missing?)");
    }
    if (cm_extract(scripts, repo_dir, manifest) != 0) {
        th_cleanup(base);
        FAIL("the extractor failed (python3 missing?)");
    }
    hyp_comment_migrate_stats_t stats;
    ASSERT_EQ(cm_ingest(store_dir, manifest, &stats), HYP_COMMENT_MIGRATE_OK);

    /* The reader's question is the one an agent asks: what is attached to this
     * file? Answered by the anchor the migration derived, not by a record id
     * the caller had to already know. */
    hyp_comment_migrate_opts_t opts = {.workspace = NULL, .repo = "fixture"};
    char anchor[HYP_ADDR_MAX + 1];
    ASSERT_EQ(hyp_comment_migrate_anchor(&opts, "fixture.c", anchor, sizeof(anchor)),
              HYP_COMMENT_MIGRATE_OK);
    ASSERT_STR_EQ(anchor, "hyp1:fixture/fixture#fixture.fixture");

    hyp_record_store_t *store = NULL;
    if (hyp_record_store_open(store_dir, &store) != HYP_RECORD_STORE_OK) {
        th_cleanup(base);
        FAIL("cannot reopen the store");
    }
    hyp_record_store_query_t q;
    memset(&q, 0, sizeof(q));
    q.anchor = anchor;
    hyp_record_set_t *found = NULL;
    ASSERT_EQ(hyp_record_store_query(store, &q, &found), HYP_RECORD_STORE_OK);
    ASSERT_EQ((int)hyp_record_set_count(found), 1);

    const hyp_record_t *rec = hyp_record_set_at(found, 0);
    ASSERT_NOT_NULL(rec);
    /* The whole point of the unit: the sentence the code gives up is still
     * here, with the provenance the code is no longer allowed to carry. */
    ASSERT_NOT_NULL(strstr(rec->content, CM_EXEMPLAR));
    ASSERT_NOT_NULL(strstr(rec->content, "§2.7"));
    ASSERT_EQ(rec->kind, HYP_RECORD_DECISION);
    ASSERT_STR_EQ(rec->author, CM_B_NAME " <" CM_B_MAIL ">");
    ASSERT_EQ((long long)rec->timestamp_ms, 1750000000000LL);
    ASSERT_NOT_NULL(rec->origin);
    ASSERT_EQ(strncmp(rec->origin, "comment:", 8), 0);
    ASSERT_NOT_NULL(strstr(rec->origin, ":fixture.c:"));

    hyp_record_set_free(found);
    hyp_record_store_close(store);
    th_cleanup(base);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The derived strings, and the refusals
 * ════════════════════════════════════════════════════════════════════ */

TEST(origin_names_blob_path_and_range_and_refuses_the_rest) {
    const char *blob = "0123456789abcdef0123456789abcdef01234567";
    char out[HYP_RECORD_MAX_ORIGIN + 1];
    ASSERT_TRUE(hyp_comment_migrate_origin(blob, "src/a.c", 12, 40, out, sizeof(out)));
    ASSERT_STR_EQ(out, "comment:0123456789abcdef0123456789abcdef01234567:src/a.c:12-40");

    /* Not a blob; an uppercase or short hex is a different string that would
     * produce a different id for the same content. */
    ASSERT_FALSE(hyp_comment_migrate_origin("0123456789", "src/a.c", 1, 2, out, sizeof(out)));
    ASSERT_STR_EQ(out, "");
    ASSERT_FALSE(hyp_comment_migrate_origin(blob, "", 1, 2, out, sizeof(out)));
    /* A range that runs backwards, or starts before the first line. */
    ASSERT_FALSE(hyp_comment_migrate_origin(blob, "src/a.c", 40, 12, out, sizeof(out)));
    ASSERT_FALSE(hyp_comment_migrate_origin(blob, "src/a.c", 0, 12, out, sizeof(out)));
    /* Refusal, never truncation: a shortened origin names a different range. */
    char tiny[16];
    ASSERT_FALSE(hyp_comment_migrate_origin(blob, "src/a.c", 12, 40, tiny, sizeof(tiny)));
    ASSERT_STR_EQ(tiny, "");
    PASS();
}

TEST(anchor_is_the_module_address_and_carries_no_line_numbers) {
    hyp_comment_migrate_opts_t solo = {.workspace = NULL, .repo = "hyponoia"};
    char out[HYP_ADDR_MAX + 1];
    ASSERT_EQ(hyp_comment_migrate_anchor(&solo, "src/mcp/tool_surface.h", out, sizeof(out)),
              HYP_COMMENT_MIGRATE_OK);
    ASSERT_STR_EQ(out, "hyp1:hyponoia/hyponoia#hyponoia.src.mcp.tool_surface");
    /* No digits from a line range anywhere in it: resolution consults symbol
     * identity and content, never lines, so an anchor must not smuggle one in. */
    ASSERT_NULL(strchr(out, '-'));

    hyp_comment_migrate_opts_t ws = {.workspace = "team", .repo = "hyponoia"};
    ASSERT_EQ(hyp_comment_migrate_anchor(&ws, "src/mcp/tool_surface.h", out, sizeof(out)),
              HYP_COMMENT_MIGRATE_OK);
    ASSERT_STR_EQ(out, "hyp1:team/hyponoia#hyponoia.src.mcp.tool_surface");

    /* A missing repo is a missing address, not a default one. */
    hyp_comment_migrate_opts_t none = {.workspace = NULL, .repo = NULL};
    ASSERT_EQ(hyp_comment_migrate_anchor(&none, "src/a.c", out, sizeof(out)),
              HYP_COMMENT_MIGRATE_ERR_NULL);
    ASSERT_STR_EQ(out, "");
    PASS();
}

TEST(manifest_refuses_anything_it_cannot_read_exactly) {
    char *base = th_mktempdir("cm_refuse");
    if (!base) {
        FAIL("cannot create a temp dir");
    }
    char store_dir[HYP_PATH_MAX];
    char path[HYP_PATH_MAX];
    (void)snprintf(store_dir, sizeof(store_dir), "%s/store", base);
    (void)snprintf(path, sizeof(path), "%s/manifest", base);

    hyp_comment_migrate_stats_t stats;

    /* Not this format at all. */
    ASSERT_EQ(th_write_file(path, "some other file\n"), 0);
    ASSERT_EQ(cm_ingest(store_dir, path, &stats), HYP_COMMENT_MIGRATE_ERR_FORMAT);

    /* Right magic, fields out of order: an ordered format has no place for a
     * field one end forgot and the other defaulted. */
    ASSERT_EQ(th_write_file(path, "hyp-comment-manifest-v1\n"
                                  "blob 40\n0123456789abcdef0123456789abcdef01234567\n"
                                  "path 7\nsrc/a.c\n"),
              0);
    ASSERT_EQ(cm_ingest(store_dir, path, &stats), HYP_COMMENT_MIGRATE_ERR_FORMAT);

    /* A declared length that does not match the bytes that follow. */
    ASSERT_EQ(th_write_file(path, "hyp-comment-manifest-v1\n"
                                  "path 99\nsrc/a.c\n"),
              0);
    ASSERT_EQ(cm_ingest(store_dir, path, &stats), HYP_COMMENT_MIGRATE_ERR_FORMAT);

    /* A timestamp of zero is what a zeroed struct hands you; "unset" must not
     * be indistinguishable from 1970. */
    ASSERT_EQ(th_write_file(path, "hyp-comment-manifest-v1\n"
                                  "path 7\nsrc/a.c\n"
                                  "blob 40\n0123456789abcdef0123456789abcdef01234567\n"
                                  "lines 4\n1-40\n"
                                  "author 3\nada\n"
                                  "timestamp_ms 1\n0\n"
                                  "content 4\nwhy?\n"),
              0);
    ASSERT_EQ(cm_ingest(store_dir, path, &stats), HYP_COMMENT_MIGRATE_ERR_FIELD);
    ASSERT_EQ((int)stats.items, 0);

    /* The same item with a usable timestamp lands, so the refusals above are
     * about the field and not about the fixture being unusable. */
    ASSERT_EQ(th_write_file(path, "hyp-comment-manifest-v1\n"
                                  "path 7\nsrc/a.c\n"
                                  "blob 40\n0123456789abcdef0123456789abcdef01234567\n"
                                  "lines 4\n1-40\n"
                                  "author 3\nada\n"
                                  "timestamp_ms 13\n1700000000000\n"
                                  "content 4\nwhy?\n"),
              0);
    ASSERT_EQ(cm_ingest(store_dir, path, &stats), HYP_COMMENT_MIGRATE_OK);
    ASSERT_EQ((int)stats.appended, 1);

    /* A store that cannot be opened is a refusal that names itself. */
    ASSERT_EQ(cm_ingest(store_dir, "/nonexistent/manifest", &stats), HYP_COMMENT_MIGRATE_ERR_IO);

    th_cleanup(base);
    PASS();
}

TEST(migrate_status_reasons_are_present_and_distinct) {
    hyp_comment_migrate_status_t all[] = {
        HYP_COMMENT_MIGRATE_OK,      HYP_COMMENT_MIGRATE_ERR_NULL,
        HYP_COMMENT_MIGRATE_ERR_IO,  HYP_COMMENT_MIGRATE_ERR_FORMAT,
        HYP_COMMENT_MIGRATE_ERR_FIELD, HYP_COMMENT_MIGRATE_ERR_ADDRESS,
        HYP_COMMENT_MIGRATE_ERR_RECORD, HYP_COMMENT_MIGRATE_ERR_STORE,
        HYP_COMMENT_MIGRATE_ERR_ALLOC};
    size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const char *reason = hyp_comment_migrate_status_reason(all[i]);
        ASSERT_NOT_NULL(reason);
        ASSERT_TRUE(reason[0] != '\0');
        for (size_t j = i + 1; j < n; j++) {
            ASSERT_STR_NEQ(reason, hyp_comment_migrate_status_reason(all[j]));
        }
    }
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The in-place half: keep the why, strip the when
 * ════════════════════════════════════════════════════════════════════ */

/* Sentences a reader with NO ACCESS TO THE STORE must still find in the
 * header. Asserting the absence of section references alone would be satisfied
 * by deleting the paragraph, which is the failure this unit is named for — so
 * the presence of the reasoning is asserted too, and it is the half that
 * fails if a future edit "simplifies" the header. */
/* Each row is a CLAIM a cold reader must still be able to make, keyed on a
 * durable anchor plus a set of alternative phrasings — any one of which
 * satisfies it, and the match must land in the prose that FOLLOWS the anchor.
 *
 * Not a single pinned sentence, deliberately. A test that names one sentence
 * is only as good as whoever transcribed it: reword the paragraph for clarity
 * and the check fails while the property holds, or paraphrase it in a brief
 * and the check "fails" against a sentence that was never in the file. What is
 * being asserted is that the reasoning is still ATTACHED TO ITS TOPIC, which
 * survives an edit that a literal does not. */
typedef struct {
    const char *claim;           /* what a reader must be able to state */
    const char *anchor;          /* durable topic marker in the header */
    const char *alternatives[6]; /* NULL-terminated; any one satisfies */
} cm_cold_read_t;

static const cm_cold_read_t CM_COLD_READ[] = {
    {"why reserved rows exist rather than advertised-but-unimplemented ones",
     "Reserved rows",
     {"pays for the call", "charges a turn", "costs every session",
      "not a tool the agent uses", NULL}},
    {"why going live is a one-token change",
     "reserved row publishes a signature",
     {"one token in one row", "the flip is one token", NULL}},
    {"why an alias is declared but never advertised",
     "alias",
     {"compatibility shim", "not surface", "no end declares", "no client can discover", NULL}},
};

/* The window after an anchor in which the reasoning must appear. Wide enough
 * for a rewritten paragraph, narrow enough that a match under an unrelated
 * heading cannot satisfy the claim. */
enum { CM_COLD_READ_WINDOW = 2000 };

/* The file as a READER sees it: comment leaders folded away, whitespace runs
 * collapsed to one space. A sentence that wraps across three lines is one
 * sentence to a person and three to strstr, and asserting against the raw
 * bytes would make the check pass or fail on where the paragraph happened to
 * be rewrapped. */
static char *cm_flow(const char *text) {
    size_t len = strlen(text);
    char *out = (char *)malloc(len + 2);
    if (!out) {
        return NULL;
    }
    size_t w = 0;
    size_t i = 0;
    while (i < len) {
        if (text[i] == '\n') {
            i++;
            while (i < len && (text[i] == ' ' || text[i] == '\t')) {
                i++;
            }
            if (i < len && text[i] == '*' && (i + 1 >= len || text[i + 1] != '/')) {
                i++;
            }
            while (i < len && (text[i] == ' ' || text[i] == '\t')) {
                i++;
            }
            if (w > 0 && out[w - 1] != ' ') {
                out[w++] = ' ';
            }
            continue;
        }
        if (text[i] == ' ' || text[i] == '\t') {
            if (w > 0 && out[w - 1] != ' ') {
                out[w++] = ' ';
            }
            i++;
            continue;
        }
        out[w++] = text[i++];
    }
    out[w] = '\0';
    return out;
}

TEST(cold_read_of_tool_surface_still_states_why) {
    char *text = cm_slurp("src/mcp/tool_surface.h");
    if (!text) {
        FAIL("cannot read src/mcp/tool_surface.h from the repository root");
    }
    char *flow = cm_flow(text);
    free(text);
    if (!flow) {
        FAIL("out of memory");
    }
    size_t n = sizeof(CM_COLD_READ) / sizeof(CM_COLD_READ[0]);
    for (size_t i = 0; i < n; i++) {
        const cm_cold_read_t *row = &CM_COLD_READ[i];
        const char *at = strstr(flow, row->anchor);
        if (!at) {
            (void)fprintf(stderr, "\n  topic gone from tool_surface.h: \"%s\"\n", row->anchor);
            free(flow);
            FAIL("a rationale block left the header entirely");
        }
        size_t remaining = strlen(at);
        size_t window = remaining < CM_COLD_READ_WINDOW ? remaining : CM_COLD_READ_WINDOW;
        char saved = at[window];
        ((char *)at)[window] = '\0';
        bool found = false;
        for (size_t k = 0; row->alternatives[k] != NULL; k++) {
            if (strstr(at, row->alternatives[k])) {
                found = true;
                break;
            }
        }
        ((char *)at)[window] = saved;
        if (!found) {
            (void)fprintf(stderr, "\n  tool_surface.h no longer says %s\n", row->claim);
            free(flow);
            FAIL("the header kept the topic and lost the reasoning");
        }
    }
    free(flow);
    PASS();
}

TEST(migrated_headers_pass_the_comment_lint) {
    /* The lint is the arbiter — there is no exemption list, so "done" for a
     * migrated file is measured by the same eight patterns that gate every new
     * comment. Files listed here have been through E2 and must stay clean. */
    static const char *migrated[] = {"src/mcp/tool_surface.h", "src/memory/anchor.h",
                                     "src/memory/comment_migrate.h", "src/store/record_store.h"};
    char *base = th_mktempdir("cm_lint");
    if (!base) {
        FAIL("cannot create a temp dir");
    }
    char report[HYP_PATH_MAX];
    (void)snprintf(report, sizeof(report), "%s/report", base);
    if (cm_run("python3 scripts/lint-comments.py --all > '%s' 2>/dev/null", report) != 0) {
        th_cleanup(base);
        FAIL("the comment lint could not run (python3 missing?)");
    }
    char *text = cm_slurp(report);
    if (!text) {
        th_cleanup(base);
        FAIL("the comment lint wrote no report");
    }
    size_t n = sizeof(migrated) / sizeof(migrated[0]);
    for (size_t i = 0; i < n; i++) {
        char needle[HYP_PATH_MAX];
        (void)snprintf(needle, sizeof(needle), "\n%s:", migrated[i]);
        if (strstr(text, needle)) {
            (void)fprintf(stderr, "\n  still history-shaped: %s\n", migrated[i]);
            free(text);
            th_cleanup(base);
            FAIL("a migrated file still carries findings");
        }
    }
    free(text);
    th_cleanup(base);
    PASS();
}

#endif /* !_WIN32 */

/* ── Suite ──────────────────────────────────────────────────────────────── */

SUITE(comment_migrate) {
#ifdef _WIN32
    SKIP_PLATFORM("comment migration shells out to git and python3");
#else
    /* attribution */
    RUN_TEST(manifest_attributes_each_block_to_the_commit_that_last_touched_it);
    /* the assertion row, and the control it exists for */
    RUN_TEST(two_runs_into_fresh_stores_union_to_exactly_one_run);
    RUN_TEST(clock_attribution_duplicates_and_blame_attribution_does_not);
    /* relocate, do not delete */
    RUN_TEST(migrated_prose_is_retrievable_by_anchor_afterwards);
    /* the derived strings and the refusals */
    RUN_TEST(origin_names_blob_path_and_range_and_refuses_the_rest);
    RUN_TEST(anchor_is_the_module_address_and_carries_no_line_numbers);
    RUN_TEST(manifest_refuses_anything_it_cannot_read_exactly);
    RUN_TEST(migrate_status_reasons_are_present_and_distinct);
    /* the in-place half */
    RUN_TEST(cold_read_of_tool_surface_still_states_why);
    RUN_TEST(migrated_headers_pass_the_comment_lint);
#endif
}
