/*
 * test_identity.c — NEXT-STEPS §4 Phase 0, contract C1.
 *
 * The two acceptance tests the plan names, plus the properties everything in
 * Phase 1 will be written against.
 *
 *   1. the same symbol in two repos gets TWO DISTINCT addresses
 *   2. the same symbol after a move gets the SAME address
 *
 * Both are asserted over qualified names produced by the REAL builders —
 * hyp_pipeline_fqn_compute for (1), hyp_extract_file for the span half — not
 * over hand-written strings. A contract asserted against a literal only proves
 * the literal was typed twice.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "../src/foundation/identity.h"
#include "../src/pipeline/pipeline.h"
#include "../src/ask/ask_embed.h"
#include "../src/ask/ask_vectors.h"
#include "hyp.h"

#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Compose an address from a repo and a real qualified name, and render it.
 * `out` must hold HYP_ADDR_MAX + 1. Returns the refusal reason. */
static hyp_addr_err_t addr_of(const char *workspace, const char *repo, const char *qn, char *out) {
    hyp_addr_t a;
    hyp_addr_err_t err = hyp_addr_init(&a, workspace, repo, qn);
    out[0] = '\0';
    if (err != HYP_ADDR_OK) {
        return err;
    }
    return hyp_addr_format(&a, out, (size_t)HYP_ADDR_MAX + 1) ? HYP_ADDR_OK : HYP_ADDR_ERR_NULL;
}

/* The address of `name` declared in `rel_path` of `repo`, via the real FQN
 * builder. */
static hyp_addr_err_t addr_of_symbol(const char *repo, const char *rel_path, const char *name,
                                     char *out) {
    char *qn = hyp_pipeline_fqn_compute(repo, rel_path, name);
    if (!qn) {
        out[0] = '\0';
        return HYP_ADDR_ERR_NULL;
    }
    hyp_addr_err_t err = addr_of(NULL, repo, qn, out);
    free(qn);
    return err;
}

/* ════════════════════════════════════════════════════════════════════
 * ACCEPTANCE 1 — the same symbol in two repos gets two distinct addresses
 * ════════════════════════════════════════════════════════════════════ */

TEST(identity_same_symbol_two_repos_two_addresses) {
    char a[HYP_ADDR_MAX + 1];
    char b[HYP_ADDR_MAX + 1];

    /* Byte-identical file, byte-identical symbol, two repositories. This is the
     * vendored-code case as much as the coincidence case: this very repository
     * vendors llama.cpp, so identical trees under two slugs is normal. */
    ASSERT_EQ(addr_of_symbol("repo-one", "src/core/util.c", "parse", a), HYP_ADDR_OK);
    ASSERT_EQ(addr_of_symbol("repo-two", "src/core/util.c", "parse", b), HYP_ADDR_OK);
    ASSERT_STR_NEQ(a, b);

    /* And they are distinct for the stated reason — the repo field — not by
     * accident of some other component. */
    hyp_addr_t pa;
    hyp_addr_t pb;
    ASSERT_EQ(hyp_addr_parse(a, &pa), HYP_ADDR_OK);
    ASSERT_EQ(hyp_addr_parse(b, &pb), HYP_ADDR_OK);
    ASSERT_STR_NEQ(pa.repo, pb.repo);
    ASSERT_FALSE(hyp_addr_same_repo(&pa, &pb));
    ASSERT_FALSE(hyp_addr_equal(&pa, &pb));
    PASS();
}

/* Within one repository the distinctness still has to come from somewhere real:
 * two different files holding the same symbol name are two addresses. */
TEST(identity_same_name_two_files_two_addresses) {
    char a[HYP_ADDR_MAX + 1];
    char b[HYP_ADDR_MAX + 1];
    ASSERT_EQ(addr_of_symbol("repo-one", "src/a/foo.c", "run", a), HYP_ADDR_OK);
    ASSERT_EQ(addr_of_symbol("repo-one", "src/b/foo.c", "run", b), HYP_ADDR_OK);
    ASSERT_STR_NEQ(a, b);
    PASS();
}

/*
 * THE DESIGNED EXCEPTION. Cross-repo rendezvous names are workspace-scoped and
 * the same route in two repositories is deliberately the SAME address — that
 * flatness is the mechanism pass_cross_repo.c matches on, because the store's
 * authorizer denies SQLITE_ATTACH and there is no cross-database join key.
 * Scoping these per repo would silently disable every CROSS_* edge.
 */
TEST(identity_rendezvous_names_are_shared_across_repos_on_purpose) {
    char a[HYP_ADDR_MAX + 1];
    char b[HYP_ADDR_MAX + 1];
    const char *route = "__route__GET__/v2/orders/{}";

    /* Same workspace, two member repos, one route. */
    ASSERT_EQ(addr_of("acme", "repo-one", route, a), HYP_ADDR_OK);
    ASSERT_EQ(addr_of("acme", "repo-two", route, b), HYP_ADDR_OK);
    ASSERT_STR_EQ(a, b);
    ASSERT_STR_EQ(a, "hyp1:acme#__route__GET__/v2/orders/{}");

    hyp_addr_t p;
    ASSERT_EQ(hyp_addr_parse(a, &p), HYP_ADDR_OK);
    ASSERT_EQ(p.scope, HYP_ADDR_SCOPE_WORKSPACE);
    /* The repo is ABSENT, and scope is what says so. A caller must not read the
     * empty string as "there is no repository" — it means "look across the
     * workspace". */
    ASSERT_STR_EQ(p.repo, "");

    /* A DIFFERENT workspace does not rendezvous with it. */
    char c[HYP_ADDR_MAX + 1];
    ASSERT_EQ(addr_of("other-workspace", "repo-one", route, c), HYP_ADDR_OK);
    ASSERT_STR_NEQ(a, c);
    PASS();
}

/* The rendezvous classification is derived from the shape the builders emit,
 * so a tag nobody has written yet is covered the day it is written. Each of
 * these is a real construction site in the tree. */
TEST(identity_rendezvous_shape_is_derived_not_enumerated) {
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__route__GET__/x"), HYP_ADDR_SCOPE_WORKSPACE);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__route__kafka__orders"), HYP_ADDR_SCOPE_WORKSPACE);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__grpc__Orders/Create"), HYP_ADDR_SCOPE_WORKSPACE);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__graphql__createOrder"), HYP_ADDR_SCOPE_WORKSPACE);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__trpc__orders.create"), HYP_ADDR_SCOPE_WORKSPACE);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__channel__redis__jobs"), HYP_ADDR_SCOPE_WORKSPACE);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__env__DATABASE_URL"), HYP_ADDR_SCOPE_WORKSPACE);
    /* A tag that does not exist yet, and is covered anyway. */
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__queue__sqs__billing"), HYP_ADDR_SCOPE_WORKSPACE);

    /* "<project>.__branch__.<slug>" IS project-rooted (git_context.c builds it
     * that way) and therefore stays repo-scoped. */
    ASSERT_EQ(hyp_addr_qn_scope("proj", "proj.__branch__.main"), HYP_ADDR_SCOPE_REPO);
    /* "__file__" is a terminal component, never a leading one. */
    ASSERT_EQ(hyp_addr_qn_scope("proj", "proj.core.c.__file__"), HYP_ADDR_SCOPE_REPO);
    /* Ordinary names, including ones that merely start with an underscore. */
    ASSERT_EQ(hyp_addr_qn_scope("proj", "proj.core.init"), HYP_ADDR_SCOPE_REPO);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "proj.core._private"), HYP_ADDR_SCOPE_REPO);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "proj.core.__dunder"), HYP_ADDR_SCOPE_REPO);

    /* A repository whose own slug looks like a tag keeps its own nodes. This is
     * why rooting is checked BEFORE the shape, and not the other way round. */
    ASSERT_EQ(hyp_addr_qn_scope("__foo__bar", "__foo__bar.core.init"), HYP_ADDR_SCOPE_REPO);
    /* Ambiguity resolves toward repo-scoped, which is the recoverable error. */
    ASSERT_EQ(hyp_addr_qn_scope("proj", "__notatag"), HYP_ADDR_SCOPE_REPO);
    ASSERT_EQ(hyp_addr_qn_scope("proj", "____"), HYP_ADDR_SCOPE_REPO);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * ACCEPTANCE 2 — the same symbol after a move gets the same address
 * ════════════════════════════════════════════════════════════════════ */

/*
 * "A move" is the within-file line move: C5u is "foo moves 200 lines, the
 * decision is still attached", and C3u's mechanism is "symbol identity +
 * content hash, NEVER line numbers". It passes by construction here because
 * there is no line number in an address to go stale — which is the design
 * working, not the test being weak. The test asserts the construction: the
 * address of a symbol is a pure function of (workspace, repo, qualified name)
 * and nothing else that a re-index can change.
 */
TEST(identity_symbol_moved_within_a_file_keeps_its_address) {
    char *dir = th_mktempdir("hyp-identity");
    ASSERT_NOT_NULL(dir);
    char file[600];
    snprintf(file, sizeof(file), "%s/x.c", dir);

    /* Generation 1: alpha at line 1. */
    ASSERT_EQ(th_write_file(file, "int alpha(void) {\n"
                                  "    return 1;\n"
                                  "}\n"),
              0);
    char *src1 = NULL;
    long n1 = 0;
    {
        FILE *f = fopen(file, "rb");
        ASSERT_NOT_NULL(f);
        fseek(f, 0, SEEK_END);
        n1 = ftell(f);
        fseek(f, 0, SEEK_SET);
        src1 = malloc((size_t)n1 + 1);
        ASSERT_NOT_NULL(src1);
        ASSERT_EQ((long)fread(src1, 1, (size_t)n1, f), n1);
        src1[n1] = '\0';
        fclose(f);
    }
    HYPFileResult *r1 = hyp_extract_file(src1, (int)n1, HYP_LANG_C, "proj", "x.c", 0, NULL, NULL);
    ASSERT_NOT_NULL(r1);
    const HYPDefinition *d1 = NULL;
    for (int i = 0; i < r1->defs.count; i++) {
        if (strcmp(r1->defs.items[i].name, "alpha") == 0) {
            d1 = &r1->defs.items[i];
        }
    }
    ASSERT_NOT_NULL(d1);
    ASSERT_EQ((int)d1->start_line, 1);

    char addr1[HYP_ADDR_MAX + 1];
    ASSERT_EQ(addr_of(NULL, "proj", d1->qualified_name, addr1), HYP_ADDR_OK);
    char *span1 = hyp_ask_read_span_lines(file, (int)d1->start_line, (int)d1->end_line, NULL);
    ASSERT_NOT_NULL(span1);
    char hash1[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(span1, hash1);

    /* Generation 2: the SAME alpha, pushed down by 200 lines of new code above
     * it. Same file, same name, same body — only the line numbers moved. */
    {
        FILE *f = fopen(file, "wb");
        ASSERT_NOT_NULL(f);
        for (int i = 0; i < 100; i++) {
            fprintf(f, "int filler_%d(void) {\n    return %d;\n}\n", i, i);
        }
        fprintf(f, "int alpha(void) {\n    return 1;\n}\n");
        fclose(f);
    }
    char *src2 = NULL;
    long n2 = 0;
    {
        FILE *f = fopen(file, "rb");
        ASSERT_NOT_NULL(f);
        fseek(f, 0, SEEK_END);
        n2 = ftell(f);
        fseek(f, 0, SEEK_SET);
        src2 = malloc((size_t)n2 + 1);
        ASSERT_NOT_NULL(src2);
        ASSERT_EQ((long)fread(src2, 1, (size_t)n2, f), n2);
        src2[n2] = '\0';
        fclose(f);
    }
    HYPFileResult *r2 = hyp_extract_file(src2, (int)n2, HYP_LANG_C, "proj", "x.c", 0, NULL, NULL);
    ASSERT_NOT_NULL(r2);
    const HYPDefinition *d2 = NULL;
    for (int i = 0; i < r2->defs.count; i++) {
        if (strcmp(r2->defs.items[i].name, "alpha") == 0) {
            d2 = &r2->defs.items[i];
        }
    }
    ASSERT_NOT_NULL(d2);
    /* The premise: it really did move. A test where nothing moved proves
     * nothing about surviving a move. */
    ASSERT_GT((int)d2->start_line, (int)d1->start_line + 200);

    char addr2[HYP_ADDR_MAX + 1];
    ASSERT_EQ(addr_of(NULL, "proj", d2->qualified_name, addr2), HYP_ADDR_OK);
    char *span2 = hyp_ask_read_span_lines(file, (int)d2->start_line, (int)d2->end_line, NULL);
    ASSERT_NOT_NULL(span2);
    char hash2[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(span2, hash2);

    /* THE ACCEPTANCE: same address across a 200-line move. */
    ASSERT_STR_EQ(addr1, addr2);
    ASSERT_STR_EQ(hash1, hash2);

    hyp_addr_t pa;
    hyp_addr_t pb;
    ASSERT_EQ(hyp_addr_parse(addr1, &pa), HYP_ADDR_OK);
    ASSERT_EQ(hyp_addr_parse(addr2, &pb), HYP_ADDR_OK);
    ASSERT_EQ(hyp_addr_relate(&pa, hash1, &pb, hash2), HYP_ADDR_REL_SAME);

    free(span1);
    free(span2);
    free(src1);
    free(src2);
    hyp_free_result(r1);
    hyp_free_result(r2);
    th_cleanup(dir);
    PASS();
}

/*
 * THE SPAN INCLUDES THE DECLARATION LINE.
 *
 * C4u is built on symbol-rename being separable from a move, and that is only
 * true because the declaration line sits INSIDE the hashed span: renaming foo
 * changes the qualified name (so the address) AND the declaration text (so the
 * hash), while relocating it changes only the address. Today that holds by
 * accident of span extraction. This test makes it a contract — it fails if a
 * later change quietly starts the span at the body.
 */
TEST(identity_span_hash_covers_the_declaration_line) {
    char *dir = th_mktempdir("hyp-identity");
    ASSERT_NOT_NULL(dir);
    char file[600];
    snprintf(file, sizeof(file), "%s/x.c", dir);
    const char *src = "int alpha(void) {\n"
                      "    return 1;\n"
                      "}\n";
    ASSERT_EQ(th_write_file(file, src), 0);

    HYPFileResult *r =
        hyp_extract_file(src, (int)strlen(src), HYP_LANG_C, "proj", "x.c", 0, NULL, NULL);
    ASSERT_NOT_NULL(r);
    const HYPDefinition *d = NULL;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].name, "alpha") == 0) {
            d = &r->defs.items[i];
        }
    }
    ASSERT_NOT_NULL(d);

    char *span = hyp_ask_read_span_lines(file, (int)d->start_line, (int)d->end_line, NULL);
    ASSERT_NOT_NULL(span);
    /* The declaration, not just the body. If this ever fails, rename detection
     * has silently degraded to "different address, same hash" for every rename,
     * and C4u cannot tell a rename from a relocation any more. */
    ASSERT_NOT_NULL(strstr(span, "int alpha(void)"));

    free(span);
    hyp_free_result(r);
    th_cleanup(dir);
    PASS();
}

/* And the consequence, asserted end to end: a symbol rename moves BOTH halves,
 * so it is never mistaken for a relocation. */
TEST(identity_symbol_rename_changes_both_address_and_hash) {
    char *dir = th_mktempdir("hyp-identity");
    ASSERT_NOT_NULL(dir);
    char file[600];
    snprintf(file, sizeof(file), "%s/x.c", dir);

    const char *before_src = "int alpha(void) {\n    return 1;\n}\n";
    const char *after_src = "int alpha_renamed(void) {\n    return 1;\n}\n";

    char addr_before[HYP_ADDR_MAX + 1];
    char addr_after[HYP_ADDR_MAX + 1];
    char hash_before[HYP_ADDR_SPAN_HASH_LEN + 1];
    char hash_after[HYP_ADDR_SPAN_HASH_LEN + 1];

    const char *names[] = {"alpha", "alpha_renamed"};
    const char *sources[] = {before_src, after_src};
    char *addrs[] = {addr_before, addr_after};
    char *hashes[] = {hash_before, hash_after};

    for (int gen = 0; gen < 2; gen++) {
        ASSERT_EQ(th_write_file(file, sources[gen]), 0);
        HYPFileResult *r = hyp_extract_file(sources[gen], (int)strlen(sources[gen]), HYP_LANG_C,
                                            "proj", "x.c", 0, NULL, NULL);
        ASSERT_NOT_NULL(r);
        const HYPDefinition *d = NULL;
        for (int i = 0; i < r->defs.count; i++) {
            if (strcmp(r->defs.items[i].name, names[gen]) == 0) {
                d = &r->defs.items[i];
            }
        }
        ASSERT_NOT_NULL(d);
        ASSERT_EQ(addr_of(NULL, "proj", d->qualified_name, addrs[gen]), HYP_ADDR_OK);
        char *span = hyp_ask_read_span_lines(file, (int)d->start_line, (int)d->end_line, NULL);
        ASSERT_NOT_NULL(span);
        hyp_addr_span_hash(span, hashes[gen]);
        free(span);
        hyp_free_result(r);
    }

    ASSERT_STR_NEQ(addr_before, addr_after);
    ASSERT_STR_NEQ(hash_before, hash_after);

    hyp_addr_t pa;
    hyp_addr_t pb;
    ASSERT_EQ(hyp_addr_parse(addr_before, &pa), HYP_ADDR_OK);
    ASSERT_EQ(hyp_addr_parse(addr_after, &pb), HYP_ADDR_OK);
    /* Nothing may be inferred — and the enum says exactly that, rather than
     * offering a plausible neighbour. */
    ASSERT_EQ(hyp_addr_relate(&pa, hash_before, &pb, hash_after), HYP_ADDR_REL_UNRELATED);

    th_cleanup(dir);
    PASS();
}

/*
 * A CROSS-DIRECTORY MOVE IS A DIFFERENT ADDRESS, AND THAT IS THE CONTRACT.
 *
 * Symbol identity IS the qualified name and the qualified name embeds the
 * directory path, so relocating a file changes the address. Dropping the
 * directory to make relocation identity-preserving would break agreement with
 * the store's UNIQUE(project, qualified_name) and would make src/a/foo.c and
 * src/b/foo.c indistinguishable, which is worse than what it fixes.
 *
 * The content hash still matches, so the pair is a VISIBLE candidate for C4u —
 * never a silent re-attachment. This contract cannot separate a cross-directory
 * move from a file rename, and does not pretend to.
 */
TEST(identity_cross_directory_move_is_a_visible_candidate_not_a_match) {
    char before[HYP_ADDR_MAX + 1];
    char after[HYP_ADDR_MAX + 1];
    ASSERT_EQ(addr_of_symbol("proj", "src/a/foo.c", "run", before), HYP_ADDR_OK);
    ASSERT_EQ(addr_of_symbol("proj", "src/b/foo.c", "run", after), HYP_ADDR_OK);
    ASSERT_STR_NEQ(before, after);

    /* The body did not change, so the span hash did not either. */
    char h[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash("int run(void) {\n    return 1;\n}", h);

    hyp_addr_t pa;
    hyp_addr_t pb;
    ASSERT_EQ(hyp_addr_parse(before, &pa), HYP_ADDR_OK);
    ASSERT_EQ(hyp_addr_parse(after, &pb), HYP_ADDR_OK);
    /* NOT "same". A candidate with evidence, which C4u presents and does not
     * decide — a copy-paste of run() into a second file produces this exact
     * signature with the original still sitting untouched at `before`. */
    ASSERT_EQ(hyp_addr_relate(&pa, h, &pb, h), HYP_ADDR_REL_CONTENT_ONLY);
    ASSERT_STR_EQ(hyp_addr_rel_str(HYP_ADDR_REL_CONTENT_ONLY), "content-only");
    PASS();
}

/* An edit in place keeps the address and moves the hash: still attached, and
 * distinguishable from both "intact" and "gone". */
TEST(identity_edit_in_place_is_its_own_relation) {
    hyp_addr_t a;
    ASSERT_EQ(hyp_addr_init(&a, NULL, "proj", "proj.core.run"), HYP_ADDR_OK);
    char h1[HYP_ADDR_SPAN_HASH_LEN + 1];
    char h2[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash("int run(void) { return 1; }", h1);
    hyp_addr_span_hash("int run(void) { return 2; }", h2);
    ASSERT_STR_NEQ(h1, h2);
    ASSERT_EQ(hyp_addr_relate(&a, h1, &a, h2), HYP_ADDR_REL_EDITED);
    ASSERT_EQ(hyp_addr_relate(&a, h1, &a, h1), HYP_ADDR_REL_SAME);
    PASS();
}

/* "Could not compare" is not "unrelated". A caller that read one as the other
 * would drop an anchor on a malformed input and call it an orphan. */
TEST(identity_relate_refuses_rather_than_guessing) {
    hyp_addr_t a;
    ASSERT_EQ(hyp_addr_init(&a, NULL, "proj", "proj.core.run"), HYP_ADDR_OK);
    char h[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash("body", h);

    ASSERT_EQ(hyp_addr_relate(NULL, h, &a, h), HYP_ADDR_REL_INVALID);
    ASSERT_EQ(hyp_addr_relate(&a, h, NULL, h), HYP_ADDR_REL_INVALID);
    ASSERT_EQ(hyp_addr_relate(&a, NULL, &a, h), HYP_ADDR_REL_INVALID);
    ASSERT_EQ(hyp_addr_relate(&a, "", &a, h), HYP_ADDR_REL_INVALID);
    ASSERT_EQ(hyp_addr_relate(&a, "deadbeef", &a, h), HYP_ADDR_REL_INVALID); /* too short */
    ASSERT_EQ(hyp_addr_relate(&a, h, &a, "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"),
              HYP_ADDR_REL_INVALID); /* not hex */
    /* Uppercase hex is not what hyp_addr_span_hash emits, so it is refused
     * rather than silently case-folded into a match. */
    ASSERT_EQ(hyp_addr_relate(&a, h, &a, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), HYP_ADDR_REL_INVALID);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The string: round trip, and the refusals
 * ════════════════════════════════════════════════════════════════════ */

TEST(identity_round_trips_exactly) {
    const char *cases[][3] = {
        {"ws", "repo", "repo.core.init"},
        {NULL, "hyponoia", "hyponoia.src.pipeline.fqn.hyp_pipeline_fqn_compute"},
        {"ws", "repo", "__route__GET__/v2/orders/{}"},
        {"ws", "repo", "__grpc__Orders/Create"},
        /* The unescaped dot the FQN builder really emits, pinned at
         * tests/test_fqn.c: "proj..env.local.__file__". Ambiguous to SPLIT,
         * exact to CARRY — the qualified name is terminal and is never split. */
        {"ws", "proj", "proj..env.local.__file__"},
        /* A '#' inside the qualified name still round-trips, because the parse
         * splits at the FIRST '#' and a slug can never contain one. */
        {"ws", "repo", "repo.weird#name"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hyp_addr_t built;
        ASSERT_EQ(hyp_addr_init(&built, cases[i][0], cases[i][1], cases[i][2]), HYP_ADDR_OK);
        char s[HYP_ADDR_MAX + 1];
        ASSERT_TRUE(hyp_addr_format(&built, s, sizeof(s)));
        hyp_addr_t back;
        ASSERT_EQ(hyp_addr_parse(s, &back), HYP_ADDR_OK);
        ASSERT_TRUE(hyp_addr_equal(&built, &back));
        ASSERT_STR_EQ(back.qn, cases[i][2]);
        char s2[HYP_ADDR_MAX + 1];
        ASSERT_TRUE(hyp_addr_format(&back, s2, sizeof(s2)));
        ASSERT_STR_EQ(s, s2);
    }
    PASS();
}

/* A single repo is a workspace of one: one code path, and the default is
 * visible in the string rather than hidden in a mode flag. */
TEST(identity_a_single_repo_is_a_workspace_of_one) {
    char a[HYP_ADDR_MAX + 1];
    char b[HYP_ADDR_MAX + 1];
    ASSERT_EQ(addr_of(NULL, "proj", "proj.core.init", a), HYP_ADDR_OK);
    ASSERT_STR_EQ(a, "hyp1:proj/proj#proj.core.init");
    /* Naming the workspace explicitly as itself is the same address — there is
     * no second mode to disagree with the first. */
    ASSERT_EQ(addr_of("proj", "proj", "proj.core.init", b), HYP_ADDR_OK);
    ASSERT_STR_EQ(a, b);
    PASS();
}

/* Whatever hyp_project_name_from_path produces must fit what an address can
 * carry, and must pass the validator this module reuses. Asserted against the
 * producer's real output rather than a comment claiming the caps agree. */
TEST(identity_accepts_every_slug_the_project_namer_produces) {
    const char *paths[] = {
        "/home/u/my project",
        "/home/u/repos/hyponoia",
        "C:\\Users\\u\\repo",
        "/home/u/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
        "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z/aa/bb/cc/dd/ee/ff/gg/hh/ii/jj/kk/"
        "ll/mm/nn/oo/pp/qq/rr/ss/tt/uu/vv/ww/xx/yy/zz/aaa/bbb/ccc/ddd/eee/fff/ggg/hhh/iii/jjj/"
        "kkk/lll/mmm/nnn/ooo/ppp/qqq/rrr/sss/ttt/uuu/vvv/www/xxx/yyy/zzz/final",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char *slug = hyp_project_name_from_path(paths[i]);
        ASSERT_NOT_NULL(slug);
        char qn[HYP_ADDR_QN_MAX + 1];
        snprintf(qn, sizeof(qn), "%s.core.init", slug);
        hyp_addr_t a;
        hyp_addr_err_t err = hyp_addr_init(&a, NULL, slug, qn);
        if (err != HYP_ADDR_OK) {
            fprintf(stderr, "  slug %s refused: %s\n", slug, hyp_addr_err_str(err));
        }
        ASSERT_EQ(err, HYP_ADDR_OK);
        ASSERT_STR_EQ(a.repo, slug);
        free(slug);
    }
    PASS();
}

TEST(identity_refuses_rather_than_guessing) {
    hyp_addr_t a;

    /* A bare qualified name is not an address. Without the scheme this would be
     * read as a workspace with no node, or worse, silently accepted. */
    ASSERT_EQ(hyp_addr_parse("proj.core.init", &a), HYP_ADDR_ERR_SCHEME);
    ASSERT_EQ(hyp_addr_parse("hyp2:ws/repo#q", &a), HYP_ADDR_ERR_SCHEME);
    ASSERT_EQ(hyp_addr_parse("hyp1", &a), HYP_ADDR_ERR_SCHEME);
    ASSERT_EQ(hyp_addr_parse("hyp1:ws/repo", &a), HYP_ADDR_ERR_NO_NODE);
    ASSERT_EQ(hyp_addr_parse("hyp1:ws/repo#", &a), HYP_ADDR_ERR_QN_EMPTY);
    ASSERT_EQ(hyp_addr_parse(NULL, &a), HYP_ADDR_ERR_NULL);

    /* Slugs are project names, validated by the one validator. */
    ASSERT_EQ(hyp_addr_parse("hyp1:#q", &a), HYP_ADDR_ERR_WORKSPACE);
    ASSERT_EQ(hyp_addr_parse("hyp1:ws/#q", &a), HYP_ADDR_ERR_REPO);
    ASSERT_EQ(hyp_addr_parse("hyp1:../repo#repo.q", &a), HYP_ADDR_ERR_WORKSPACE);
    ASSERT_EQ(hyp_addr_parse("hyp1:ws/re po#re po.q", &a), HYP_ADDR_ERR_REPO);
    ASSERT_EQ(hyp_addr_init(&a, "ws", "re;po", "q"), HYP_ADDR_ERR_REPO);
    ASSERT_EQ(hyp_addr_init(&a, "w s", "repo", "q"), HYP_ADDR_ERR_WORKSPACE);

    /* Control bytes never enter an address: these travel through JSON, logs and
     * line-oriented files, and a newline inside one splits a record. */
    ASSERT_EQ(hyp_addr_init(&a, "ws", "repo", "repo.a\nb"), HYP_ADDR_ERR_QN_CONTROL);
    ASSERT_EQ(hyp_addr_parse("hyp1:ws/repo#repo.a\tb", &a), HYP_ADDR_ERR_QN_CONTROL);

    /* Refused, never truncated: a truncated qualified name is a valid address
     * for a DIFFERENT node. */
    char *huge = malloc((size_t)HYP_ADDR_QN_MAX + 2);
    ASSERT_NOT_NULL(huge);
    memset(huge, 'x', (size_t)HYP_ADDR_QN_MAX + 1);
    huge[HYP_ADDR_QN_MAX + 1] = '\0';
    ASSERT_EQ(hyp_addr_init(&a, "ws", "repo", huge), HYP_ADDR_ERR_QN_TOO_LONG);
    free(huge);

    /* Every reason has a message, including one that is not a reason. */
    ASSERT_NOT_NULL(hyp_addr_err_str(HYP_ADDR_ERR_SCOPE_MISMATCH));
    ASSERT_NOT_NULL(hyp_addr_err_str((hyp_addr_err_t)999));
    ASSERT_NOT_NULL(hyp_addr_rel_str((hyp_addr_rel_t)999));
    PASS();
}

/*
 * THE TWO ENDS. hyp_addr_parse accepts exactly what hyp_addr_format o
 * hyp_addr_init emits, and nothing else. Without this the parser would admit
 * strings the composer can never produce — a rendezvous name pinned to one
 * repo, or a repo-scoped name floating free of any repo — and the two halves of
 * the contract would have drifted apart on day one.
 */
TEST(identity_parse_accepts_exactly_what_format_emits) {
    hyp_addr_t a;
    /* A rendezvous name cannot be pinned to a repository. */
    ASSERT_EQ(hyp_addr_parse("hyp1:ws/repo#__route__GET__/x", &a), HYP_ADDR_ERR_SCOPE_MISMATCH);
    /* A repo-scoped name cannot float free of one. */
    ASSERT_EQ(hyp_addr_parse("hyp1:ws#proj.core.init", &a), HYP_ADDR_ERR_SCOPE_MISMATCH);

    /* And the composer never emits either shape. */
    hyp_addr_t built;
    char s[HYP_ADDR_MAX + 1];
    ASSERT_EQ(hyp_addr_init(&built, "ws", "repo", "__route__GET__/x"), HYP_ADDR_OK);
    ASSERT_TRUE(hyp_addr_format(&built, s, sizeof(s)));
    ASSERT_STR_EQ(s, "hyp1:ws#__route__GET__/x");
    ASSERT_EQ(hyp_addr_parse(s, &a), HYP_ADDR_OK);

    /* A hand-built struct whose scope contradicts its fields renders nothing
     * rather than an address that could not have been composed. */
    hyp_addr_t bogus;
    memset(&bogus, 0, sizeof(bogus));
    snprintf(bogus.workspace, sizeof(bogus.workspace), "ws");
    snprintf(bogus.repo, sizeof(bogus.repo), "repo");
    snprintf(bogus.qn, sizeof(bogus.qn), "repo.core.init");
    bogus.scope = HYP_ADDR_SCOPE_WORKSPACE; /* claims shared, carries a repo */
    ASSERT_FALSE(hyp_addr_format(&bogus, s, sizeof(s)));
    ASSERT_STR_EQ(s, "");
    PASS();
}

TEST(identity_format_refuses_a_buffer_it_cannot_fill) {
    hyp_addr_t a;
    ASSERT_EQ(hyp_addr_init(&a, "ws", "repo", "repo.core.init"), HYP_ADDR_OK);
    char small[8];
    ASSERT_FALSE(hyp_addr_format(&a, small, sizeof(small)));
    /* Empty, not truncated — a caller ignoring the return value gets nothing
     * rather than "hyp1:ws". */
    ASSERT_STR_EQ(small, "");
    char exact[HYP_ADDR_MAX + 1];
    ASSERT_TRUE(hyp_addr_format(&a, exact, sizeof(exact)));
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * The span hash is the vector store's hash, not a second one
 * ════════════════════════════════════════════════════════════════════ */

/* Asserted from the CLIENT's side — what the ask lane's own entry point
 * returns — not from the shared implementation both would trivially agree on. */
TEST(identity_span_hash_is_the_vector_stores_hash) {
    const char *texts[] = {
        "",
        "int alpha(void) {\n    return 1;\n}",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        char mine[HYP_ADDR_SPAN_HASH_LEN + 1];
        char theirs[HYP_ASK_VEC_HASH_LEN + 1];
        hyp_addr_span_hash(texts[i], mine);
        hyp_ask_content_hash(texts[i], theirs);
        ASSERT_STR_EQ(mine, theirs);
        ASSERT_EQ((int)strlen(mine), HYP_ADDR_SPAN_HASH_LEN);
    }
    /* NULL hashes the empty string, exactly as the ask lane always has. */
    char a[HYP_ADDR_SPAN_HASH_LEN + 1];
    char b[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(NULL, a);
    hyp_ask_content_hash(NULL, b);
    ASSERT_STR_EQ(a, b);
    ASSERT_EQ(HYP_ADDR_SPAN_HASH_LEN, HYP_ASK_VEC_HASH_LEN);
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(identity) {
    /* Acceptance 1 and its designed exception */
    RUN_TEST(identity_same_symbol_two_repos_two_addresses);
    RUN_TEST(identity_same_name_two_files_two_addresses);
    RUN_TEST(identity_rendezvous_names_are_shared_across_repos_on_purpose);
    RUN_TEST(identity_rendezvous_shape_is_derived_not_enumerated);

    /* Acceptance 2, and what a move is not */
    RUN_TEST(identity_symbol_moved_within_a_file_keeps_its_address);
    RUN_TEST(identity_span_hash_covers_the_declaration_line);
    RUN_TEST(identity_symbol_rename_changes_both_address_and_hash);
    RUN_TEST(identity_cross_directory_move_is_a_visible_candidate_not_a_match);
    RUN_TEST(identity_edit_in_place_is_its_own_relation);
    RUN_TEST(identity_relate_refuses_rather_than_guessing);

    /* The string */
    RUN_TEST(identity_round_trips_exactly);
    RUN_TEST(identity_a_single_repo_is_a_workspace_of_one);
    RUN_TEST(identity_accepts_every_slug_the_project_namer_produces);
    RUN_TEST(identity_refuses_rather_than_guessing);
    RUN_TEST(identity_parse_accepts_exactly_what_format_emits);
    RUN_TEST(identity_format_refuses_a_buffer_it_cannot_fill);

    /* One hash */
    RUN_TEST(identity_span_hash_is_the_vector_stores_hash);
}
