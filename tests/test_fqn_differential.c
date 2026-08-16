/*
 * test_fqn_differential.c — Differential test over the TWO FQN implementations.
 *
 * src/pipeline/fqn.c (malloc-allocated: hyp_pipeline_fqn_compute/_module/
 * _module_dir/_folder) and internal/hyp/helpers.c (arena-allocated:
 * hyp_fqn_compute/_module/_module_source_lang/_compute_source_lang/_folder)
 * both derive qualified names for the same entities. Every anchor rests on the
 * QN: extraction writes def/module QNs with one implementation, pipeline passes
 * and lookups derive QNs with the other, and the two meet in the registry
 * (src/pipeline/registry.c:678-680 concatenates a PIPELINE-derived module QN
 * with a callee name and looks it up against EXTRACTION-written keys). Two
 * derivations that can drift is therefore a silent-orphaning machine.
 *
 * ── WHAT THIS FILE FOUND ─────────────────────────────────────────────
 *
 * The two implementations DISAGREE on six axes. They are recorded below as
 * characterization tests that pin BOTH sides' current output verbatim. THESE
 * ARE DEFECTS, NOT SPECIFICATIONS. Nothing below describes behavior anyone
 * designed; each one is a bug that is pinned so it cannot get worse silently.
 *
 * The headline: one implementation already carries the fix the other never
 * got. helpers.c strip_ext_len() guards the dotfile case in so many words —
 * "A dot at the very start of a filename segment is a DOTFILE marker, NOT an
 * extension separator. Stripping there leaves an empty stem whose module QN
 * collides with the parent directory/project root." — and append_path_segments()
 * drops the leading '.' to avoid "a malformed proj..env double-dot". fqn.c has
 * NEITHER half, so it reproduces both bugs that comment describes as fixed.
 * fqn.c:119-127 shows its authors hit the same wall and fixed it for the
 * name=="__file__" case ONLY (#1077/#964), naming .env/.env.local as the
 * motivating example, and left the MODULE/symbol path unguarded.
 *
 * Two of the six are LIVE — real repository contents reach them today:
 *   AXIS 1 (dotfile stems) and AXIS 2 (unresolved relative specifiers, which
 *   ride the same code path). The remaining four are latent: they need inputs
 *   the product does not currently produce.
 *
 * ── EXIT CONDITION ───────────────────────────────────────────────────
 *
 * These characterization tests are a TEMPORARY RECORD of a known defect, not a
 * permanent contract. When the two derivations are reconciled (tracked as unit
 * A11), every fqn_differential_DEFECT_* test below converts back into a plain
 * equality assertion — move its rows into the agreement tests above it — and
 * fqn_differential_DEFECT_divergence_count_pinned_at_six drops its expected
 * count toward 0. Each pinned row also fails if the defect is FIXED (the
 * checker reports "defect appears FIXED"), so the reconciliation cannot land
 * without someone being told to come back here and delete these tests.
 *
 * A disagreement reported by this file is a FINDING, not a flake. Do not
 * weaken an assertion to green the suite, and do not "fix" either
 * implementation to match the other without deciding which one is right.
 *
 * NULL-project note: hyp_pipeline_fqn_folder(NULL, dir) returns "" while
 * hyp_fqn_folder(a, NULL, dir) calls strlen(NULL) — undefined behavior, since
 * hyp_fqn_compute guards a NULL project and hyp_fqn_folder does not — so that
 * single pair is NOT exercised (a differential cannot compare against a
 * crash); the compute pair covers the NULL-project axis instead. That missing
 * guard is itself worth fixing alongside A11.
 */
#include "test_framework.h"
#include "../src/pipeline/pipeline.h"
#include "helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The six known divergence axes. Pinned as a count below so that a SEVENTH
 * axis cannot hide among the known six — it must arrive as a failure. */
#define FQD_KNOWN_DIVERGENCE_AXES 6

/* ── Mismatch reporting ────────────────────────────────────────── */

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
    if (bad) {
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

/* ── Per-pair checkers (equality — the drift detector) ──────────── */

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
 * hyp_fqn_module_source_lang. fqn.c:154-178 documents that these MUST agree
 * (cross-file LSP caller_qn vs def-node QN). */
static int check_module_lang(HYPArena *a, const char *project, const char *rel_path,
                             HYPLanguage lang) {
    bool is_dir = (lang == HYP_LANG_JAVA || lang == HYP_LANG_GO);
    return fqd_diverges(is_dir ? "module_dir" : "module_lang", project, rel_path, NULL,
                        hyp_pipeline_fqn_module_dir(project, rel_path, is_dir),
                        hyp_fqn_module_source_lang(a, project, rel_path, lang));
}

/* Symbol QN for directory-module languages. The pipeline has no _source_lang
 * symbol variant; its documented composition (fqn.c:154-178, pass_lsp_cross)
 * is module_dir + "." + name, which is emulated here and compared against the
 * extraction side's hyp_fqn_compute_source_lang. */
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

/* ── Pinned-defect checker (characterization) ───────────────────── */

typedef enum {
    FQD_COMPUTE = 0,
    FQD_FOLDER,
    FQD_MODULE_DIR_GO,
} fqd_kind_t;

/* One recorded defect: the input, and BOTH sides' output as measured. */
typedef struct {
    fqd_kind_t kind;
    const char *project;
    const char *rel_path;
    const char *name; /* FQD_COMPUTE only */
    const char *want_pipeline;
    const char *want_extraction;
} fqd_pin_t;

/* Assert both sides still produce exactly what was recorded, AND that they
 * still disagree. Three ways to fail, all of them worth a human:
 *   - pipeline moved     → the defect changed shape
 *   - extraction moved   → the defect changed shape
 *   - they now AGREE     → the defect was fixed; delete this row and move the
 *                          input into the equality/agreement tests (see the
 *                          EXIT CONDITION in the file header).
 * Returns 1 if the row needs attention. */
static int fqd_check_pin(HYPArena *a, const fqd_pin_t *p) {
    char *pipeline = NULL;
    const char *extraction = NULL;
    const char *what = "compute";

    switch (p->kind) {
    case FQD_FOLDER:
        what = "folder";
        pipeline = hyp_pipeline_fqn_folder(p->project, p->rel_path);
        extraction = hyp_fqn_folder(a, p->project, p->rel_path);
        break;
    case FQD_MODULE_DIR_GO:
        what = "module_dir";
        pipeline = hyp_pipeline_fqn_module_dir(p->project, p->rel_path, true);
        extraction = hyp_fqn_module_source_lang(a, p->project, p->rel_path, HYP_LANG_GO);
        break;
    case FQD_COMPUTE:
    default:
        pipeline = hyp_pipeline_fqn_compute(p->project, p->rel_path, p->name);
        extraction = hyp_fqn_compute(a, p->project, p->rel_path, p->name);
        break;
    }

    int bad = 0;
    if (!pipeline || strcmp(pipeline, p->want_pipeline) != 0) {
        char label[64];
        snprintf(label, sizeof(label), "PIPELINE MOVED %s", what);
        fqd_print_input(label, p->project, p->rel_path, p->name);
        fqd_print_qn("recorded", p->want_pipeline);
        printf("  ");
        fqd_print_qn("now", pipeline);
        printf("\n");
        bad = 1;
    }
    if (!extraction || strcmp(extraction, p->want_extraction) != 0) {
        char label[64];
        snprintf(label, sizeof(label), "EXTRACTION MOVED %s", what);
        fqd_print_input(label, p->project, p->rel_path, p->name);
        fqd_print_qn("recorded", p->want_extraction);
        printf("  ");
        fqd_print_qn("now", extraction);
        printf("\n");
        bad = 1;
    }
    if (!bad && strcmp(pipeline, extraction) == 0) {
        char label[64];
        snprintf(label, sizeof(label), "defect appears FIXED %s", what);
        fqd_print_input(label, p->project, p->rel_path, p->name);
        printf("both sides now agree on \"%s\" — delete this pinned row and\n"
               "      assert equality instead (see EXIT CONDITION in the header)\n",
               pipeline);
        bad = 1;
    }

    free(pipeline);
    return bad;
}

static int fqd_check_pins(HYPArena *a, const fqd_pin_t *pins, size_t count) {
    int bad = 0;
    for (size_t i = 0; i < count; i++) {
        bad += fqd_check_pin(a, &pins[i]);
    }
    return bad;
}

/* ── Corpus builders ───────────────────────────────────────────── */

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

/* Does `s` end with `suffix`? */
static int fqd_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) {
        return 0;
    }
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return m <= n && strcmp(s + n - m, suffix) == 0;
}

/* ================================================================
 * Agreement surface — cases the two implementations must and (per
 * this test's baseline) currently do agree on. Any red here is NEW
 * drift between the implementations: a SEVENTH divergence axis.
 * ================================================================ */

TEST(fqn_diff_compute_agreement) {
    /* Axis product: representative paths × name forms. Every language's
     * plain-file shape, __init__/index stripping (both directions), lookalike
     * names that must NOT strip, multi-dot stems, no-extension files,
     * slash forms, dotted directory-with-plain-name, and non-ASCII bytes. */
    static const char *const paths[] = {
        "main.go",
        "app.py",
        "server.ts",
        "util.js",
        "core.c",
        "lib.rs",
        "src/pkg/module.go",
        "a/b/c/file.py",
        "pkg/__init__.py",
        "__init__.py",
        "pkg/__init__",
        "src/index.ts",
        "pkg/index.js",
        "index.js",
        "pkg/index",
        "a/index/b.ts",
        "pkg/__init_data__.py",
        "pkg/indexer.ts",
        "foo.test.ts",
        "util.spec.js",
        "archive.tar.gz",
        "a.b/c.py",
        "Makefile",
        "/src/main.go",
        "src/",
        "a//b.go",
        "src/\xe6\xa8\xa1\xe5\x9d\x97.py", /* CJK basename */
        "caf\xc3\xa9/\xc3\xbc.py",         /* accented dir + basename */
        "",
    };
    static const char *const names[] = {NULL, "", "fn", "Class_\xe5\x90\x8d"};

    HYPArena a;
    hyp_arena_init(&a);
    int bad = 0;
    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
            bad += check_compute(&a, "proj", paths[p], names[n]);
        }
    }
    /* NULL rel_path and empty project (both sides treat as ""). */
    bad += check_compute(&a, "proj", NULL, "fn");
    bad += check_compute(&a, "", "a.py", "f");
    /* __file__ on an extension-less file: the only __file__ shape with no
     * extension-stripping asymmetry. */
    bad += check_compute(&a, "proj", "Makefile", "__file__");
    /* Depth at and below the pipeline cap (FQN_MAX_PATH_SEGS = 254). */
    char *deep250 = make_deep_path(250, ".py");
    char *deep254 = make_deep_path(254, ".py");
    bad += check_compute(&a, "proj", deep250, "fn");
    bad += check_compute(&a, "proj", deep254, "fn");
    bad += check_compute(&a, "proj", deep254, NULL);
    free(deep250);
    free(deep254);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

TEST(fqn_diff_module_wrapper_agreement) {
    static const char *const paths[] = {
        "src/app.py", "cmd/server.go",      "pkg/__init__.py", "components/index.js",
        "a/b/c/d.rs", "NodeController.cpp", /* module stem unification, both sides */
        "",
    };
    HYPArena a;
    hyp_arena_init(&a);
    int bad = 0;
    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        bad += check_module(&a, "proj", paths[p]);
    }
    bad += check_module(&a, "proj", NULL);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

TEST(fqn_diff_folder_agreement) {
    static const char *const dirs[] = {
        "",
        "src",
        "src/pkg/util",
        "src/pkg/",
        "/src/pkg",
        "a//b",
        ".github", /* extraction's folder path keeps the dot — both agree */
        "./src",
        "\xe6\xb7\xb1\xe3\x81\x84/\xe8\xb7\xaf\xe5\xbe\x84", /* non-ASCII dirs */
    };
    HYPArena a;
    hyp_arena_init(&a);
    int bad = 0;
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        bad += check_folder(&a, "proj", dirs[d]);
    }
    /* Depth at the pipeline folder cap (254 dir segments). */
    char *deep254 = make_deep_path(254, "");
    bad += check_folder(&a, "proj", deep254);
    free(deep254);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

TEST(fqn_diff_dirlang_agreement) {
    /* Directory-module languages (Java/Go): module = containing directory.
     * fqn.c:154-178 names agreement with hyp_fqn_module_source_lang as a
     * requirement; this is that requirement, executed. */
    HYPArena a;
    hyp_arena_init(&a);
    int bad = 0;

    static const struct {
        const char *rel;
        HYPLanguage lang;
    } mods[] = {
        {"Outer.java", HYP_LANG_JAVA},         {"main.go", HYP_LANG_GO},
        {"myapp/db/conn.go", HYP_LANG_GO},     {"src/main/java/com/acme/Outer.java", HYP_LANG_JAVA},
        {".config/tool.go", HYP_LANG_GO}, /* dotted dir goes through folder on both sides */
        {"src/app.py", HYP_LANG_PYTHON},  /* non-dir language must forward to plain module */
        {"src/index.ts", HYP_LANG_TYPESCRIPT}, {"", HYP_LANG_GO},
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        bad += check_module_lang(&a, "proj", mods[i].rel, mods[i].lang);
    }
    bad += check_module_lang(&a, "proj", NULL, HYP_LANG_JAVA);

    static const char *const names[] = {NULL, "", "Open"};
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        bad += check_symbol_lang(&a, "proj", "myapp/db/conn.go", names[n], HYP_LANG_GO);
        bad += check_symbol_lang(&a, "proj", "Outer.java", names[n], HYP_LANG_JAVA);
        bad += check_symbol_lang(&a, "proj", "src/main/java/com/acme/Outer.java", names[n],
                                 HYP_LANG_JAVA);
        bad += check_symbol_lang(&a, "proj", "src/app.py", names[n], HYP_LANG_PYTHON);
    }
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

/* ================================================================
 * DEFECTS. Each test below pins a KNOWN disagreement between the two
 * implementations, recording BOTH outputs verbatim. None of this is
 * designed behavior — see the file header. They convert back into
 * equality assertions when A11 reconciles the two derivations.
 * ================================================================ */

/* AXIS 1 — LIVE. Dot-leading path segments.
 *
 * helpers.c strip_ext_len() treats a dot at index 0 (or right after '/') as a
 * DOTFILE marker and keeps the whole name as the stem; append_path_segments()
 * then drops the leading '.'. fqn.c strip_file_extension() does neither: it
 * calls strrchr(start,'.'), finds the LEADING dot of ".env", and truncates the
 * segment to nothing, and tokenize_path() keeps a leading '.' on segments that
 * survive.
 *
 * Two distinct bugs on the pipeline side, both of which helpers.c documents as
 * already fixed on its side:
 *   (a) ADDRESS COLLISION. ".env" and ".gitignore" both produce the module QN
 *       "proj" — byte-identical to the project-root QN. Two different things
 *       resolve to one address, in a scheme whose entire job is that they do
 *       not. ".env.local" and ".env.production" both produce "proj..env",
 *       colliding with each other.
 *   (b) MALFORMED double dot. ".eslintrc.js" -> "proj..eslintrc".
 *
 * LIVE: the walker applies no dotfile filter — src/discover/discover.c
 * ALWAYS_SKIP_DIRS (:32-51) is exact-match and omits .github/.gitlab/.config,
 * and file_skip_reason (:646-685) has no leading-'.' rule at all. Worse,
 * language.c FILENAME_TABLE (:659-681) admits ".env"/".env.local"/
 * ".gitattributes"/".vimrc" BY NAME and language.c:988-990 admits any ".env.*"
 * by prefix. So these rel_paths reach hyp_pipeline_fqn_module at
 * pass_semantic.c:565 and pass_parallel.c:3024, while extraction writes the
 * def QNs via hyp.c:1238 — and registry.c:678-680 joins the two. Same-module
 * resolution can therefore never hit for any file with a dot-leading segment.
 * Reproducible in this repo right now: .gitattributes exists, is not ignored,
 * and its module QN is "proj". */
TEST(fqn_differential_DEFECT_dotfile_module_qn_collides_with_project_root) {
    static const fqd_pin_t pins[] = {
        /* (a) collision with the project root QN — the severe case */
        {FQD_COMPUTE, "proj", ".env", NULL, "proj", "proj.env"},
        {FQD_COMPUTE, "proj", ".env", "", "proj", "proj.env"},
        {FQD_COMPUTE, "proj", ".env", "KEY", "proj.KEY", "proj.env.KEY"},
        {FQD_COMPUTE, "proj", ".gitignore", NULL, "proj", "proj.gitignore"},
        {FQD_COMPUTE, "proj", ".gitignore", "", "proj", "proj.gitignore"},
        {FQD_COMPUTE, "proj", ".gitignore", "KEY", "proj.KEY", "proj.gitignore.KEY"},
        /* (a) collision of two sibling dotfiles with each other */
        {FQD_COMPUTE, "proj", ".env.local", NULL, "proj..env", "proj.env"},
        {FQD_COMPUTE, "proj", ".env.local", "", "proj..env", "proj.env"},
        {FQD_COMPUTE, "proj", ".env.local", "KEY", "proj..env.KEY", "proj.env.KEY"},
        {FQD_COMPUTE, "proj", ".env.production", NULL, "proj..env", "proj.env"},
        {FQD_COMPUTE, "proj", ".env.production", "", "proj..env", "proj.env"},
        {FQD_COMPUTE, "proj", ".env.production", "KEY", "proj..env.KEY", "proj.env.KEY"},
        /* (b) malformed double dot */
        {FQD_COMPUTE, "proj", ".eslintrc.js", NULL, "proj..eslintrc", "proj.eslintrc"},
        {FQD_COMPUTE, "proj", ".eslintrc.js", "", "proj..eslintrc", "proj.eslintrc"},
        {FQD_COMPUTE, "proj", ".eslintrc.js", "KEY", "proj..eslintrc.KEY", "proj.eslintrc.KEY"},
        {FQD_COMPUTE, "proj", ".index.ts", NULL, "proj..index", "proj.index"},
        {FQD_COMPUTE, "proj", ".index.ts", "", "proj..index", "proj.index"},
        {FQD_COMPUTE, "proj", ".index.ts", "KEY", "proj..index.KEY", "proj.index.KEY"},
        {FQD_COMPUTE, "proj", ".hidden.d.ts", NULL, "proj..hidden.d", "proj.hidden.d"},
        {FQD_COMPUTE, "proj", ".hidden.d.ts", "", "proj..hidden.d", "proj.hidden.d"},
        {FQD_COMPUTE, "proj", ".hidden.d.ts", "KEY", "proj..hidden.d.KEY", "proj.hidden.d.KEY"},
        /* dot-DIRECTORY, not just dot-file */
        {FQD_COMPUTE, "proj", ".github/workflows/ci.yml", NULL, "proj..github.workflows.ci",
         "proj.github.workflows.ci"},
        {FQD_COMPUTE, "proj", ".github/workflows/ci.yml", "", "proj..github.workflows.ci",
         "proj.github.workflows.ci"},
        {FQD_COMPUTE, "proj", ".github/workflows/ci.yml", "KEY", "proj..github.workflows.ci.KEY",
         "proj.github.workflows.ci.KEY"},
        {FQD_COMPUTE, "proj", "src/.config/app.py", NULL, "proj.src..config.app",
         "proj.src.config.app"},
        {FQD_COMPUTE, "proj", "src/.config/app.py", "", "proj.src..config.app",
         "proj.src.config.app"},
        {FQD_COMPUTE, "proj", "src/.config/app.py", "KEY", "proj.src..config.app.KEY",
         "proj.src.config.app.KEY"},
    };
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_check_pins(&a, pins, sizeof(pins) / sizeof(pins[0]));
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

/* AXIS 2 — LIVE. Unresolved relative import specifiers.
 *
 * The same missing dotfile/dot-segment handling, reached by a different input:
 * a raw import specifier used directly as a path. pass_calls.c:128 and
 * pass_semantic.c:96 call hyp_pipeline_fqn_module(project, imp->module_path),
 * and module_path is the VERBATIM quote-stripped source specifier
 * (extract_imports.c:413-437) — "./config", not a resolved repo path.
 * hyp_pipeline_resolve_relative_import (fqn.c:334) exists but has exactly one
 * caller in the tree (pass_pkgmap.c:1398), and neither of those two sites is
 * on it.
 *
 * Consequence: every relative JS/TS import misses hyp_gbuf_find_by_qn and is
 * silently skipped, so the fast-path import map drops them. The failure mode
 * is a MISSING edge rather than a wrong one, which is why it has not
 * surfaced. pass_pkgmap.c:1748 has the narrow "../" variant (":1716-1718"
 * strips a leading "./" but not "../"). */
TEST(fqn_differential_DEFECT_unresolved_relative_specifier_dots) {
    static const fqd_pin_t pins[] = {
        {FQD_COMPUTE, "proj", "./a.py", NULL, "proj...a", "proj.a"},
        {FQD_COMPUTE, "proj", "./a.py", "", "proj...a", "proj.a"},
        {FQD_COMPUTE, "proj", "./a.py", "KEY", "proj...a.KEY", "proj.a.KEY"},
        {FQD_COMPUTE, "proj", "a/./b.py", NULL, "proj.a...b", "proj.a.b"},
        {FQD_COMPUTE, "proj", "a/./b.py", "", "proj.a...b", "proj.a.b"},
        {FQD_COMPUTE, "proj", "a/./b.py", "KEY", "proj.a...b.KEY", "proj.a.b.KEY"},
        /* Both sides are wrong here, in different ways — neither pops "a". */
        {FQD_COMPUTE, "proj", "a/../b.py", NULL, "proj.a....b", "proj.a...b"},
        {FQD_COMPUTE, "proj", "a/../b.py", "", "proj.a....b", "proj.a...b"},
        {FQD_COMPUTE, "proj", "a/../b.py", "KEY", "proj.a....b.KEY", "proj.a...b.KEY"},
    };
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_check_pins(&a, pins, sizeof(pins) / sizeof(pins[0]));
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

/* AXIS 3 — LATENT. name == "__file__" terminal.
 *
 * fqn.c:119-132 preserves the full filename for File-node QNs so that sibling
 * files sharing a stem get distinct nodes (#1077/#964). helpers.c has no
 * __file__ rule and strips the extension as usual, so it would collapse
 * exactly the files fqn.c went out of its way to separate: .env/.env.local/
 * .env.production all become "proj.env.__file__", and a C++ header and its
 * same-stem .cpp collide.
 *
 * LATENT: the literal "__file__" appears ZERO times under internal/hyp/, and
 * all 26 pipeline sites pass the inline string literal — no variable holding
 * "__file__" is ever passed to hyp_fqn_compute. This becomes live the instant
 * any extraction-side code mints a File QN. Note fqn.c's dot-preserving
 * __file__ output is separately pinned by tests/test_fqn.c:62-66 and
 * documented at identity.h:106-107, so an AXIS 1 fix must leave the
 * is_file_qn branch (fqn.c:129-132) alone. */
TEST(fqn_differential_DEFECT_file_terminal_extension_dropped_by_extraction) {
    static const fqd_pin_t pins[] = {
        {FQD_COMPUTE, "proj", "src/app.py", "__file__", "proj.src.app.py.__file__",
         "proj.src.app.__file__"},
        /* three siblings that fqn.c keeps distinct and helpers.c collapses */
        {FQD_COMPUTE, "proj", ".env", "__file__", "proj..env.__file__", "proj.env.__file__"},
        {FQD_COMPUTE, "proj", ".env.local", "__file__", "proj..env.local.__file__",
         "proj.env.__file__"},
        {FQD_COMPUTE, "proj", ".env.production", "__file__", "proj..env.production.__file__",
         "proj.env.__file__"},
        /* header vs same-stem implementation */
        {FQD_COMPUTE, "proj", "NodeController.h", "__file__", "proj.NodeController.h.__file__",
         "proj.NodeController.__file__"},
        {FQD_COMPUTE, "proj", "NodeController.cpp", "__file__", "proj.NodeController.cpp.__file__",
         "proj.NodeController.__file__"},
        {FQD_COMPUTE, "proj", "a/index.ts", "__file__", "proj.a.index.ts.__file__",
         "proj.a.__file__"},
    };
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_check_pins(&a, pins, sizeof(pins) / sizeof(pins[0]));
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

/* AXIS 4 — LATENT. Path-segment cap.
 *
 * fqn.c caps at FQN_MAX_PATH_SEGS = 254 path segments (FQN_MAX_DIR_SEGS = 255
 * for folders) and SILENTLY drops the remainder — tokenize_path (fqn.c:82)
 * just stops at max_segs and join_segments emits the shortened QN with no
 * warning. helpers.c has no cap at all. Beyond the cap the pipeline's QN is a
 * truncation of the extraction's.
 *
 * LATENT: the walker has no depth guard (WALK_STACK_CAP at discover.c:809 is
 * annotated as a discovery-failure bound, not a depth guard), so a 255-deep
 * tree is producible — but the real bound is rel_path[HYP_SZ_4K]
 * (discover.c:869), and 255 single-char segments is only ~509 bytes. No real
 * repository reaches this. Pinned structurally (length + tail) because the
 * full strings are ~1.2KB.
 *
 * The 254-segment side of this boundary — where the two AGREE — is asserted in
 * fqn_diff_compute_agreement and fqn_diff_folder_agreement above. */
TEST(fqn_differential_DEFECT_deep_path_silently_truncated_by_pipeline) {
    HYPArena a;
    hyp_arena_init(&a);
    int bad = 0;

    char *deep255 = make_deep_path(255, ".py");
    char *deep300 = make_deep_path(300, ".py");
    char *dirs255 = make_deep_path(255, "");

    struct {
        const char *what;
        char *pipeline;
        const char *extraction;
        size_t want_pipeline_len;
        size_t want_extraction_len;
        const char *want_pipeline_tail;
        const char *want_extraction_tail;
    } cases[] = {
        {"compute deep255", hyp_pipeline_fqn_compute("proj", deep255, "fn"),
         hyp_fqn_compute(&a, "proj", deep255, "fn"), 1167, 1172, ".s253.fn", ".s254.fn"},
        {"compute deep300", hyp_pipeline_fqn_compute("proj", deep300, "fn"),
         hyp_fqn_compute(&a, "proj", deep300, "fn"), 1167, 1397, ".s253.fn", ".s299.fn"},
        {"folder dirs255", hyp_pipeline_fqn_folder("proj", dirs255),
         hyp_fqn_folder(&a, "proj", dirs255), 1164, 1169, ".s253", ".s254"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *pl = cases[i].pipeline;
        const char *ex = cases[i].extraction;
        if (!pl || !ex || strlen(pl) != cases[i].want_pipeline_len ||
            strlen(ex) != cases[i].want_extraction_len ||
            !fqd_ends_with(pl, cases[i].want_pipeline_tail) ||
            !fqd_ends_with(ex, cases[i].want_extraction_tail)) {
            printf("\n    DEEP PATH MOVED %s: recorded pipeline len=%zu tail=\"%s\", "
                   "extraction len=%zu tail=\"%s\"\n      ",
                   cases[i].what, cases[i].want_pipeline_len, cases[i].want_pipeline_tail,
                   cases[i].want_extraction_len, cases[i].want_extraction_tail);
            fqd_print_qn("pipeline", pl);
            printf("  ");
            fqd_print_qn("extraction", ex);
            printf("\n");
            bad++;
        } else if (strcmp(pl, ex) == 0) {
            printf("\n    defect appears FIXED %s: both sides now agree — delete this\n"
                   "      pinned row (see EXIT CONDITION in the header)\n",
                   cases[i].what);
            bad++;
        }
        free(cases[i].pipeline);
    }

    free(deep255);
    free(deep300);
    free(dirs255);
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

/* AXIS 5 — LATENT. Backslash path separators.
 *
 * fqn.c normalizes '\' to '/' (hyp_normalize_path_sep, fqn.c:119/354) before
 * splitting. helpers.c hyp_fqn_compute/_folder split on '/' only and never
 * normalize, so a backslash survives INSIDE a QN segment. Worse,
 * hyp_pipeline_fqn_module_dir honors either separator (fqn.c:167-169) while
 * hyp_fqn_module_source_lang uses strrchr('/') only — so for "myapp\db\conn.go"
 * the pipeline strips the basename and the extraction side sees no separator
 * at all and returns the bare project QN.
 *
 * That last pair is a broken CONTRACT, not merely a difference: fqn.c:159-163
 * states in so many words that hyp_pipeline_fqn_module_dir "MUST agree with
 * the extraction-side hyp_fqn_module_source_lang() ... so the cross-file LSP
 * caller_qn matches the def-node QN". One implementation is violating an
 * agreement the other one writes down.
 *
 * LATENT: every rel_path is '/'-joined by construction at discover.c:873-878
 * ("%s/%s") and readdir components cannot contain '\' on Win32, so the
 * contract holds today only because the input is pre-normalized upstream —
 * not because the two functions agree. Any future feature that accepts a
 * user-supplied or compile_commands.json-derived path as rel_path makes this
 * live immediately. */
TEST(fqn_differential_DEFECT_backslash_not_normalized_by_extraction) {
    static const fqd_pin_t pins[] = {
        {FQD_COMPUTE, "proj", "src\\main.go", "run", "proj.src.main.run", "proj.src\\main.run"},
        {FQD_COMPUTE, "proj", "a\\b\\c\\file.py", "X", "proj.a.b.c.file.X", "proj.a\\b\\c\\file.X"},
        {FQD_COMPUTE, "proj", "a/b\\c/d.ts", "fn", "proj.a.b.c.d.fn", "proj.a.b\\c.d.fn"},
        {FQD_FOLDER, "proj", "src\\pkg\\util", NULL, "proj.src.pkg.util", "proj.src\\pkg\\util"},
        {FQD_FOLDER, "proj", "src/pkg\\util", NULL, "proj.src.pkg.util", "proj.src.pkg\\util"},
        /* the documented-MUST-agree pair, disagreeing */
        {FQD_MODULE_DIR_GO, "proj", "myapp\\db\\conn.go", NULL, "proj.myapp.db", "proj"},
        {FQD_MODULE_DIR_GO, "proj", "a/b\\c.go", NULL, "proj.a.b", "proj.a"},
    };
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_check_pins(&a, pins, sizeof(pins) / sizeof(pins[0]));
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

/* AXIS 6 — LATENT. rel_dir == "." and a NULL project.
 *
 * helpers.c hyp_fqn_folder special-cases rel_dir == "." to the bare project QN
 * (helpers.c:1534); fqn.c keeps the "." as a literal segment and emits the
 * malformed "proj..". Separately, fqn.c returns strdup("") for a NULL project
 * without looking at the path, while helpers.c substitutes "" and still emits
 * path + name.
 *
 * LATENT on both counts: project_name is non-NULL by construction
 * (pipeline.c:268 hyp_project_name_from_path returns "root" for empty input,
 * and pipeline.c:304/314 reject NULL/empty), and pass_pkgmap.c:1394's
 * "ctx ? ctx->project_name : NULL" is dead defensive code — both callers
 * dereference ctx on the next line (pass_pkgmap.c:1790, :2024). Recorded
 * because a NULL project cannot anchor a stored QN yet the two ends still
 * answer the same call differently. */
TEST(fqn_differential_DEFECT_dot_folder_and_null_project) {
    static const fqd_pin_t pins[] = {
        {FQD_FOLDER, "proj", ".", NULL, "proj..", "proj"},
        {FQD_COMPUTE, NULL, "foo.go", "bar", "", ".foo.bar"},
    };
    HYPArena a;
    hyp_arena_init(&a);
    int bad = fqd_check_pins(&a, pins, sizeof(pins) / sizeof(pins[0]));
    hyp_arena_destroy(&a);
    ASSERT_EQ(bad, 0);
    PASS();
}

/* The count gate. Exactly FQD_KNOWN_DIVERGENCE_AXES axes are known to diverge;
 * one representative probe per axis. A SEVENTH axis must arrive as a failure
 * in the agreement tests above rather than hide among the known six, and an
 * axis that stops diverging must arrive as a failure here rather than pass
 * unnoticed — that is the signal to convert its pinned rows back into equality
 * assertions and lower this count (see EXIT CONDITION in the file header). */
TEST(fqn_differential_DEFECT_divergence_count_pinned_at_six) {
    HYPArena a;
    hyp_arena_init(&a);

    char *deep255 = make_deep_path(255, ".py");
    char *pl_deep = hyp_pipeline_fqn_compute("proj", deep255, "fn");
    const char *ex_deep = hyp_fqn_compute(&a, "proj", deep255, "fn");

    char *pl_dotfile = hyp_pipeline_fqn_compute("proj", ".env", NULL);
    char *pl_relspec = hyp_pipeline_fqn_compute("proj", "./a.py", NULL);
    char *pl_fileterm = hyp_pipeline_fqn_compute("proj", "src/app.py", "__file__");
    char *pl_backslash = hyp_pipeline_fqn_compute("proj", "src\\main.go", "run");
    char *pl_dotdir = hyp_pipeline_fqn_folder("proj", ".");

    const struct {
        const char *axis;
        int diverges;
    } axes[] = {
        {"1 dotfile stems (LIVE)",
         strcmp(pl_dotfile, hyp_fqn_compute(&a, "proj", ".env", NULL)) != 0},
        {"2 unresolved relative specifiers (LIVE)",
         strcmp(pl_relspec, hyp_fqn_compute(&a, "proj", "./a.py", NULL)) != 0},
        {"3 __file__ terminal extension",
         strcmp(pl_fileterm, hyp_fqn_compute(&a, "proj", "src/app.py", "__file__")) != 0},
        {"4 deep-path segment cap", strcmp(pl_deep, ex_deep) != 0},
        {"5 backslash separators",
         strcmp(pl_backslash, hyp_fqn_compute(&a, "proj", "src\\main.go", "run")) != 0},
        {"6 dot folder / NULL project", strcmp(pl_dotdir, hyp_fqn_folder(&a, "proj", ".")) != 0},
    };

    int diverging = 0;
    for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
        if (axes[i].diverges) {
            diverging++;
        } else {
            printf("\n    axis no longer diverges: %s\n"
                   "      convert its pinned rows back to equality assertions and\n"
                   "      lower FQD_KNOWN_DIVERGENCE_AXES (see EXIT CONDITION)\n",
                   axes[i].axis);
        }
    }

    free(pl_deep);
    free(pl_dotfile);
    free(pl_relspec);
    free(pl_fileterm);
    free(pl_backslash);
    free(pl_dotdir);
    free(deep255);
    hyp_arena_destroy(&a);

    ASSERT_EQ((size_t)(sizeof(axes) / sizeof(axes[0])), (size_t)FQD_KNOWN_DIVERGENCE_AXES);
    ASSERT_EQ(diverging, FQD_KNOWN_DIVERGENCE_AXES);
    PASS();
}

/* ================================================================
 * Suite
 * ================================================================ */

SUITE(fqn_differential) {
    /* Equality — red here is NEW drift, i.e. a seventh axis. */
    RUN_TEST(fqn_diff_compute_agreement);
    RUN_TEST(fqn_diff_module_wrapper_agreement);
    RUN_TEST(fqn_diff_folder_agreement);
    RUN_TEST(fqn_diff_dirlang_agreement);
    /* Pinned defects — red here means a known bug changed shape or was fixed. */
    RUN_TEST(fqn_differential_DEFECT_dotfile_module_qn_collides_with_project_root);
    RUN_TEST(fqn_differential_DEFECT_unresolved_relative_specifier_dots);
    RUN_TEST(fqn_differential_DEFECT_file_terminal_extension_dropped_by_extraction);
    RUN_TEST(fqn_differential_DEFECT_deep_path_silently_truncated_by_pipeline);
    RUN_TEST(fqn_differential_DEFECT_backslash_not_normalized_by_extraction);
    RUN_TEST(fqn_differential_DEFECT_dot_folder_and_null_project);
    RUN_TEST(fqn_differential_DEFECT_divergence_count_pinned_at_six);
}
