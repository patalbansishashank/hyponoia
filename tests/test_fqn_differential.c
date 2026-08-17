/*
 * test_fqn_differential.c — Differential test over the TWO FQN entry points.
 *
 * src/pipeline/fqn.c (malloc-allocated: hyp_pipeline_fqn_compute/_module/
 * _module_dir/_folder) and internal/hyp/helpers.c (arena-allocated:
 * hyp_fqn_compute/_module/_module_source_lang/_compute_source_lang/_folder)
 * both hand out qualified names for the same entities. Every anchor rests on
 * the QN: extraction writes def/module QNs through one, pipeline passes and
 * lookups derive QNs through the other, and the two meet in the registry
 * (src/pipeline/registry.c concatenates a PIPELINE-derived module QN with a
 * callee name and looks it up against EXTRACTION-written keys). A byte of
 * disagreement between them is not an error anywhere — it is a missing edge,
 * or an anchor that resolves to the wrong file.
 *
 * ── WHAT THIS FILE FOUND, AND WHAT HAPPENED TO IT ────────────────────
 *
 * It found six divergence axes, two of them live, one of them an address
 * COLLISION: hyp_pipeline_fqn_module(proj, ".env") returned "proj", which is
 * byte-identical to the project-root QN.
 *
 * All six are closed, and closed in the strongest available way: the RULES now
 * live in exactly one place, src/foundation/fqn_core.h, and both sides are thin
 * allocator wrappers over it. Five of the six axes were disagreements about a
 * rule — where an extension ends, whether a backslash separates, how deep a
 * path may go, what a leading dot means, what a NULL project answers — so
 * moving the rules into one implementation makes those five UNREPRESENTABLE
 * rather than merely fixed. Two callers of one function cannot drift.
 *
 * ── WHY THIS FILE STILL EXISTS ───────────────────────────────────────
 *
 * Because "thin wrapper" is a property that rots, and because one layer above
 * the core is still genuinely two implementations:
 *
 *   - hyp_pipeline_fqn_module_dir() and hyp_fqn_module_source_lang() each carry
 *     their own directory-module language predicate, their own root-file
 *     branch, and their own basename slice. They agree on where to cut only
 *     because both call hyp_fqn_core_dir_len(); everything around that call is
 *     duplicated, and fqn.c states agreement with its opposite number as a
 *     requirement in so many words.
 *   - The pipeline has no _source_lang symbol variant at all. Its documented
 *     composition — module_dir + "." + name — is emulated here and compared
 *     against hyp_fqn_compute_source_lang().
 *   - Either wrapper could reintroduce a normalization or a cap of its own.
 *
 * So: divergence is possible, the ledger is zero, and a seventh axis has to
 * arrive as a failure.
 *
 * ── HOW THIS FILE IS BUILT TO FAIL ───────────────────────────────────
 *
 * A differential that cannot report a difference is indistinguishable from one
 * that finds none, and both look green. Two structural rules keep them apart:
 *
 *   1. THE CORPUS IS DERIVED, NOT LISTED. Inputs are the PRODUCT of directory
 *      shapes, filename shapes and symbol-name shapes, so the gate covers
 *      combinations nobody wrote down. A count pinned to a hand-written list of
 *      known-bad inputs can only ever re-measure the answer it was given; this
 *      one counts divergences over the whole product, so a seventh axis on an
 *      unlisted combination increments it.
 *   2. THE DETECTOR IS SHOWN TO FIRE. fqd_diverges() is otherwise only ever
 *      run by assertions expecting zero — `return 0` would green the entire
 *      suite. fqn_diff_detector_fires_on_a_planted_divergence hands it a pair
 *      that must differ and asserts it says so.
 *
 * A disagreement reported by this file is a FINDING, not a flake. Do not weaken
 * an assertion to green the suite, and do not "fix" either side to match the
 * other without deciding which one is right — the fix for five of the six axes
 * was to delete a rule, not to copy one.
 */
#include "test_framework.h"
#include "../src/pipeline/pipeline.h"
#include "helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ledger. Every divergence the derived corpus can produce is counted, and
 * the total must equal this. Zero known divergences remain: raise it only with
 * a written finding beside the row that raises it. */
#define FQD_KNOWN_DIVERGENCE_AXES 0

/* ── Mismatch reporting ────────────────────────────────────────── */

/* Set while the detector's own positive control runs, so its deliberate
 * mismatch does not read as a finding in the log. */
static int fqd_quiet = 0;

/* Print one side's QN; long values (deep-path corpus) are shown truncated
 * with their length so the log stays readable. */
static void fqd_print_qn(const char *label, const char *qn) {
    if (!qn) {
        printf("%s=(null)", label);
        return;
    }
    size_t n = strlen(qn);
    if (n <= 160) {
        printf("%s=\"%s\"", label, qn);
    } else {
        printf("%s=\"%.80s...%.40s\" (len=%zu)", label, qn, qn + n - 40, n);
    }
}

static void fqd_print_input(const char *what, const char *project, const char *rel_path,
                            const char *name) {
    printf("\n    %s: project=%s%s%s rel_path=%s%s%s name=%s%s%s\n      ", what,
           project ? "\"" : "", project ? project : "(null)", project ? "\"" : "",
           rel_path ? "\"" : "", rel_path ? rel_path : "(null)", rel_path ? "\"" : "",
           name ? "\"" : "", name ? name : "(null)", name ? "\"" : "");
}

/* Compare the two results; on mismatch print input + both outputs.
 * Takes ownership of the malloc'd pipeline string. Returns 1 on mismatch. */
static int fqd_diverges(const char *what, const char *project, const char *rel_path,
                        const char *name, char *pipeline_qn, const char *extraction_qn) {
    int bad = !pipeline_qn || !extraction_qn || strcmp(pipeline_qn, extraction_qn) != 0;
    if (bad && !fqd_quiet) {
        char label[64];
        snprintf(label, sizeof(label), "DIVERGE %s", what);
        fqd_print_input(label, project, rel_path, name);
        fqd_print_qn("pipeline", pipeline_qn);
        printf("  ");
        fqd_print_qn("extraction", extraction_qn);
        printf("\n");
    }
    free(pipeline_qn);
    return bad;
}

/* ── Per-pair checkers ──────────────────────────────────────────── */

static int check_compute(HYPArena *a, const char *project, const char *rel_path, const char *name) {
    return fqd_diverges("compute", project, rel_path, name,
                        hyp_pipeline_fqn_compute(project, rel_path, name),
                        hyp_fqn_compute(a, project, rel_path, name));
}

static int check_module(HYPArena *a, const char *project, const char *rel_path) {
    return fqd_diverges("module", project, rel_path, NULL,
                        hyp_pipeline_fqn_module(project, rel_path),
                        hyp_fqn_module(a, project, rel_path));
}

static int check_folder(HYPArena *a, const char *project, const char *rel_dir) {
    return fqd_diverges("folder", project, rel_dir, NULL, hyp_pipeline_fqn_folder(project, rel_dir),
                        hyp_fqn_folder(a, project, rel_dir));
}

/* Language-aware module QN: pipeline hyp_pipeline_fqn_module_dir vs extraction
 * hyp_fqn_module_source_lang. fqn.c names agreement with its opposite number as
 * a requirement (cross-file LSP caller_qn vs def-node QN); this is that
 * requirement, executed. */
static int check_module_lang(HYPArena *a, const char *project, const char *rel_path,
                             HYPLanguage lang) {
    bool is_dir = (lang == HYP_LANG_JAVA || lang == HYP_LANG_GO);
    return fqd_diverges(is_dir ? "module_dir" : "module_lang", project, rel_path, NULL,
                        hyp_pipeline_fqn_module_dir(project, rel_path, is_dir),
                        hyp_fqn_module_source_lang(a, project, rel_path, lang));
}

/* Symbol QN for directory-module languages. The pipeline has no _source_lang
 * symbol variant; its documented composition is module_dir + "." + name, which
 * is emulated here and compared against hyp_fqn_compute_source_lang. */
static int check_symbol_lang(HYPArena *a, const char *project, const char *rel_path,
                             const char *name, HYPLanguage lang) {
    bool is_dir = (lang == HYP_LANG_JAVA || lang == HYP_LANG_GO);
    char *pipeline_qn = NULL;
    if (!is_dir) {
        pipeline_qn = hyp_pipeline_fqn_compute(project, rel_path, name);
    } else {
        char *module = hyp_pipeline_fqn_module_dir(project, rel_path, true);
        if (module && name && name[0]) {
            size_t n = strlen(module) + 1 + strlen(name) + 1;
            pipeline_qn = (char *)malloc(n);
            if (pipeline_qn) {
                snprintf(pipeline_qn, n, "%s.%s", module, name);
            }
            free(module);
        } else {
            pipeline_qn = module; /* no name → the module QN itself */
        }
    }
    return fqd_diverges("symbol_lang", project, rel_path, name, pipeline_qn,
                        hyp_fqn_compute_source_lang(a, project, rel_path, name, lang));
}

/* ── The derived corpus ─────────────────────────────────────────── */

/* Directory shapes. Each one is a property of a path, not an example: root,
 * nested, hidden directory, dotted directory name, navigation segments,
 * doubled and absolute separators, backslash separators, non-ASCII bytes. */
static const char *const FQD_DIRS[] = {
    "",     "src",   "a/b/c",     "pkg",     ".github/workflows",
    "src/.config",   "a.b",       "./src",   "a//b",
    "/src", "src/",  "src\\pkg",  "a/b\\c",  ".",
    "caf\xc3\xa9",   "\xe6\xb7\xb1/\xe8\xb7\xaf",
};

/* Filename shapes. Plain extensions per language family, package-entry names
 * and their lookalikes, multi-dot stems, no extension at all, and the hidden
 * files whose stem has no extension to strip. */
static const char *const FQD_FILES[] = {
    "app.py",       "main.go",      "server.ts",       "util.js",
    "core.c",       "lib.rs",       "Outer.java",      "NodeController.h",
    "NodeController.cpp",           "__init__.py",     "__init__",
    "index.ts",     "index.js",     "index",           "__init_data__.py",
    "indexer.ts",   "foo.test.ts",  "archive.tar.gz",  "Makefile",
    ".env",         ".env.local",   ".env.production", ".gitignore",
    ".eslintrc.js", ".index.ts",    ".hidden.d.ts",    ".gitattributes",
    "\xe6\xa8\xa1\xe5\x9d\x97.py",  "\xc3\xbc.py",
};

/* Symbol-name shapes: absent, empty, ordinary, the File-node terminal, and a
 * non-ASCII identifier. */
static const char *const FQD_NAMES[] = {NULL, "", "fn", "__file__", "Class_\xe5\x90\x8d"};

/* Join a directory shape and a filename shape into one rel_path. Returns a
 * heap string the caller frees. */
static char *fqd_join(const char *dir, const char *file) {
    size_t dn = strlen(dir);
    size_t fn = strlen(file);
    char *buf = (char *)malloc(dn + fn + 2);
    if (!buf) {
        return NULL;
    }
    if (dn == 0) {
        memcpy(buf, file, fn + 1);
        return buf;
    }
    memcpy(buf, dir, dn);
    size_t off = dn;
    if (dir[dn - 1] != '/' && dir[dn - 1] != '\\') {
        buf[off++] = '/';
    }
    memcpy(buf + off, file, fn + 1);
    return buf;
}

/* Build "s0/s1/.../s<n-1><suffix>": n path segments, the last one carrying
 * `suffix` (e.g. ".py" to give the leaf an extension, "" for a dir path). */
static char *make_deep_path(int segs, const char *suffix) {
    size_t cap = (size_t)segs * 8 + strlen(suffix) + 1;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        return NULL;
    }
    size_t off = 0;
    for (int i = 0; i < segs; i++) {
        off += (size_t)snprintf(buf + off, cap - off, "%ss%d", i ? "/" : "", i);
    }
    snprintf(buf + off, cap - off, "%s", suffix);
    return buf;
}

/* ── Corpus runs. Each returns the number of divergences it saw, so the
 *    topical tests and the ledger below count the same events. ─────── */

#define FQD_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

static int fqd_run_compute_corpus(HYPArena *a) {
    int bad = 0;
    for (size_t d = 0; d < FQD_COUNT(FQD_DIRS); d++) {
        for (size_t f = 0; f < FQD_COUNT(FQD_FILES); f++) {
            char *rel = fqd_join(FQD_DIRS[d], FQD_FILES[f]);
            if (!rel) {
                continue;
            }
            for (size_t n = 0; n < FQD_COUNT(FQD_NAMES); n++) {
                bad += check_compute(a, "proj", rel, FQD_NAMES[n]);
            }
            free(rel);
        }
    }
    /* Degenerate inputs the product cannot express. */
    bad += check_compute(a, "proj", NULL, "fn");
    bad += check_compute(a, "proj", "", NULL);
    bad += check_compute(a, "", "a.py", "f");
    bad += check_compute(a, NULL, "foo.go", "bar");
    return bad;
}

static int fqd_run_module_corpus(HYPArena *a) {
    int bad = 0;
    for (size_t d = 0; d < FQD_COUNT(FQD_DIRS); d++) {
        for (size_t f = 0; f < FQD_COUNT(FQD_FILES); f++) {
            char *rel = fqd_join(FQD_DIRS[d], FQD_FILES[f]);
            if (!rel) {
                continue;
            }
            bad += check_module(a, "proj", rel);
            free(rel);
        }
    }
    bad += check_module(a, "proj", NULL);
    bad += check_module(a, "proj", "");
    return bad;
}

static int fqd_run_folder_corpus(HYPArena *a) {
    int bad = 0;
    for (size_t d = 0; d < FQD_COUNT(FQD_DIRS); d++) {
        bad += check_folder(a, "proj", FQD_DIRS[d]);
    }
    bad += check_folder(a, "proj", NULL);
    /* hyp_fqn_folder(a, NULL, dir) took strlen(NULL) before the derivation was
     * unified — the one pair a differential could not compare, because it
     * cannot compare against a crash. */
    bad += check_folder(a, NULL, "src");
    return bad;
}

/* Depth: at, either side of, and far past the segment cap the pipeline used to
 * impose and the extraction side never had. Neither has one, so both sides must
 * carry every segment. */
static int fqd_run_depth_corpus(HYPArena *a) {
    int bad = 0;
    static const int depths[] = {1, 2, 250, 254, 255, 300};
    for (size_t i = 0; i < FQD_COUNT(depths); i++) {
        char *file_path = make_deep_path(depths[i], ".py");
        char *dir_path = make_deep_path(depths[i], "");
        if (file_path) {
            bad += check_compute(a, "proj", file_path, "fn");
            bad += check_compute(a, "proj", file_path, NULL);
            free(file_path);
        }
        if (dir_path) {
            bad += check_folder(a, "proj", dir_path);
            free(dir_path);
        }
    }
    return bad;
}

/* The language-variant layer, which is still two implementations. */
static int fqd_run_dirlang_corpus(HYPArena *a) {
    static const HYPLanguage langs[] = {HYP_LANG_JAVA, HYP_LANG_GO, HYP_LANG_PYTHON,
                                        HYP_LANG_TYPESCRIPT};
    static const char *const names[] = {NULL, "", "Open"};
    int bad = 0;
    for (size_t d = 0; d < FQD_COUNT(FQD_DIRS); d++) {
        for (size_t f = 0; f < FQD_COUNT(FQD_FILES); f++) {
            char *rel = fqd_join(FQD_DIRS[d], FQD_FILES[f]);
            if (!rel) {
                continue;
            }
            for (size_t l = 0; l < FQD_COUNT(langs); l++) {
                bad += check_module_lang(a, "proj", rel, langs[l]);
                for (size_t n = 0; n < FQD_COUNT(names); n++) {
                    bad += check_symbol_lang(a, "proj", rel, names[n], langs[l]);
                }
            }
            free(rel);
        }
    }
    for (size_t l = 0; l < FQD_COUNT(langs); l++) {
        bad += check_module_lang(a, "proj", NULL, langs[l]);
        bad += check_module_lang(a, "proj", "", langs[l]);
    }
    return bad;
}

/* ================================================================
 * The detector's own positive control. Everything else in this file
 * asserts zero, so a fqd_diverges() that always returned zero would
 * green the whole suite — including the ledger.
 * ================================================================ */

TEST(fqn_diff_detector_fires_on_a_planted_divergence) {
    HYPArena a;
    hyp_arena_init(&a);
    fqd_quiet = 1;
    /* Two derivations of DIFFERENT inputs. ".env" and "env.py" are exactly the
     * pair that a leading-dot rule which DROPPED the dot would collapse onto
     * one address, so this control also pins that they stay apart. */
    int fires = fqd_diverges("planted", "proj", ".env", NULL,
                             hyp_pipeline_fqn_compute("proj", ".env", NULL),
                             hyp_fqn_compute(&a, "proj", "env.py", NULL));
    /* And the same call must stay silent when the two agree, or "it fires" is
     * just "it always returns 1". */
    int silent = fqd_diverges("matched", "proj", ".env", NULL,
                              hyp_pipeline_fqn_compute("proj", ".env", NULL),
                              hyp_fqn_compute(&a, "proj", ".env", NULL));
    fqd_quiet = 0;
    hyp_arena_destroy(&a);
    ASSERT_EQ(fires, 1);
    ASSERT_EQ(silent, 0);
    PASS();
}

/* ================================================================
 * Agreement surface. Red here is drift between the two entry points.
 * ================================================================ */

TEST(fqn_diff_compute_agreement) {
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_run_compute_corpus(&a);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

TEST(fqn_diff_module_wrapper_agreement) {
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_run_module_corpus(&a);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

TEST(fqn_diff_folder_agreement) {
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_run_folder_corpus(&a);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

TEST(fqn_diff_depth_agreement) {
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_run_depth_corpus(&a);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

TEST(fqn_diff_dirlang_agreement) {
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_run_dirlang_corpus(&a);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

/* ================================================================
 * The addresses this unit exists to keep apart. These are not
 * differential rows — both sides could agree on a collision — so
 * they assert the property directly, on the extraction side and on
 * the pipeline side alike.
 * ================================================================ */

TEST(fqn_diff_dotfile_qn_never_equals_project_qn) {
    static const char *const dotfiles[] = {".env",      ".env.local",    ".env.production",
                                           ".gitignore", ".gitattributes", ".eslintrc.js"};
    HYPArena a;
    hyp_arena_init(&a);

    char *root = hyp_pipeline_fqn_module("proj", "");
    const char *root_ex = hyp_fqn_module(&a, "proj", "");
    ASSERT_STR_EQ(root, "proj");
    ASSERT_STR_EQ(root_ex, "proj");

    for (size_t i = 0; i < FQD_COUNT(dotfiles); i++) {
        char *pl = hyp_pipeline_fqn_module("proj", dotfiles[i]);
        const char *ex = hyp_fqn_module(&a, "proj", dotfiles[i]);
        ASSERT_NOT_NULL(pl);
        ASSERT_NOT_NULL(ex);
        /* A dotfile's module QN is never the project QN: the collision that
         * makes an anchor resolve, confidently, to the repository root. */
        ASSERT_TRUE(strcmp(pl, root) != 0);
        ASSERT_TRUE(strcmp(ex, root) != 0);
        /* And each dotfile is distinct from every other one. */
        for (size_t j = 0; j < i; j++) {
            char *prev = hyp_pipeline_fqn_module("proj", dotfiles[j]);
            ASSERT_TRUE(strcmp(pl, prev) != 0);
            free(prev);
        }
        free(pl);
    }
    free(root);
    hyp_arena_destroy(&a);
    PASS();
}

TEST(fqn_diff_dotfile_does_not_collide_with_its_undotted_neighbour) {
    /* Dropping the leading dot to spell ".env" as "proj.env" reads better and
     * puts it on the same address as env.py — trading a fixed collision for a
     * new one. Both sides keep the dot. */
    static const struct {
        const char *hidden;
        const char *plain;
    } pairs[] = {
        {".env", "env.py"},
        {".gitignore", "gitignore.py"},
        {".github/workflows/ci.yml", "github/workflows/ci.yml"},
        {"src/.config/app.py", "src/config/app.py"},
    };
    HYPArena a;
    hyp_arena_init(&a);
    for (size_t i = 0; i < FQD_COUNT(pairs); i++) {
        char *ph = hyp_pipeline_fqn_module("proj", pairs[i].hidden);
        char *pp = hyp_pipeline_fqn_module("proj", pairs[i].plain);
        const char *eh = hyp_fqn_module(&a, "proj", pairs[i].hidden);
        const char *ep = hyp_fqn_module(&a, "proj", pairs[i].plain);
        ASSERT_TRUE(strcmp(ph, pp) != 0);
        ASSERT_TRUE(strcmp(eh, ep) != 0);
        free(ph);
        free(pp);
    }
    hyp_arena_destroy(&a);
    PASS();
}

TEST(fqn_diff_unresolved_relative_specifier_has_no_address) {
    /* A raw import specifier is not a repository path. "./a.py" normalizes to
     * the file it names, but "a/../b.py" would have to be addressed either as
     * proj.a.b or as proj.b — and both are the address of a DIFFERENT real
     * file. The derivation refuses instead: the empty string matches nothing,
     * where a plausible name matches the wrong thing. Callers resolve the
     * specifier against the importing file first. */
    HYPArena a;
    hyp_arena_init(&a);
    static const char *const refused[] = {"a/../b.py", "../b.py", "a/b/../../c.py", "..\\b.py"};
    for (size_t i = 0; i < FQD_COUNT(refused); i++) {
        char *pl = hyp_pipeline_fqn_module("proj", refused[i]);
        const char *ex = hyp_fqn_module(&a, "proj", refused[i]);
        ASSERT_STR_EQ(pl, "");
        ASSERT_STR_EQ(ex, "");
        free(pl);
    }
    /* "." is this directory, so it contributes nothing rather than refusing —
     * a folder QN for "." is the project root, which is a real address. */
    char *dot = hyp_pipeline_fqn_folder("proj", ".");
    ASSERT_STR_EQ(dot, "proj");
    ASSERT_STR_EQ(hyp_fqn_folder(&a, "proj", "."), "proj");
    free(dot);
    hyp_arena_destroy(&a);
    PASS();
}

/* ================================================================
 * The ledger. Counts divergences over the WHOLE derived corpus, not
 * over a list of known-bad inputs — a list can only re-measure the
 * answer it was handed, and a seventh axis on an unlisted
 * combination would hide behind it.
 * ================================================================ */

TEST(fqn_differential_divergence_ledger) {
    HYPArena a;
    hyp_arena_init(&a);
    int diverging = 0;
    diverging += fqd_run_compute_corpus(&a);
    diverging += fqd_run_module_corpus(&a);
    diverging += fqd_run_folder_corpus(&a);
    diverging += fqd_run_depth_corpus(&a);
    diverging += fqd_run_dirlang_corpus(&a);
    hyp_arena_destroy(&a);

    if (diverging != FQD_KNOWN_DIVERGENCE_AXES) {
        printf("\n    ledger: %d divergences over the derived corpus, expected %d.\n"
               "      Each one is printed above with both sides' output. A NEW one is a\n"
               "      finding: decide which side is right before touching either.\n",
               diverging, FQD_KNOWN_DIVERGENCE_AXES);
    }
    ASSERT_EQ(diverging, FQD_KNOWN_DIVERGENCE_AXES);
    PASS();
}

/* ================================================================
 * Suite
 * ================================================================ */

SUITE(fqn_differential) {
    /* The detector, before anything that trusts it. */
    RUN_TEST(fqn_diff_detector_fires_on_a_planted_divergence);
    /* Agreement — red here is drift between the two entry points. */
    RUN_TEST(fqn_diff_compute_agreement);
    RUN_TEST(fqn_diff_module_wrapper_agreement);
    RUN_TEST(fqn_diff_folder_agreement);
    RUN_TEST(fqn_diff_depth_agreement);
    RUN_TEST(fqn_diff_dirlang_agreement);
    /* The addresses that must stay apart, on both sides. */
    RUN_TEST(fqn_diff_dotfile_qn_never_equals_project_qn);
    RUN_TEST(fqn_diff_dotfile_does_not_collide_with_its_undotted_neighbour);
    RUN_TEST(fqn_diff_unresolved_relative_specifier_has_no_address);
    /* The ledger, over the whole derived corpus. */
    RUN_TEST(fqn_differential_divergence_ledger);
}
