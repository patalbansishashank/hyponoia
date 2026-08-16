/*
 * test_g3_controls.c — Track G, unit G3: the negative controls.
 *
 * A suite that only ever asks "does it find the right thing?" cannot tell a
 * working detector from one that says yes to everything. Every case below has
 * a right answer of NOTHING — plus one, PC-RENDEZVOUS-001, whose right answer
 * is YES, because a suite that only says NO has stopped discriminating too.
 *
 * ── THE CORPUS IS DERIVED, NOT AUTHORED ────────────────────────────────────
 *
 * Nothing in the expectation column was written for the occasion. Every
 * control is a real commit pair from this repository's own history: the
 * anchors, the span hashes, the addresses and the named wrong answers are
 * copied verbatim from the mined corpus, and the span TEXT lives in
 * tests/fixtures/g3/g3_corpus.txt, extracted from the blobs those commits
 * hold. g3_fixture_reproduces_every_recorded_span_hash re-derives every hash
 * with the product's own hasher and refuses to let the suite run on a fixture
 * that has drifted.
 *
 * ── THE PARSER CAVEAT, AND WHAT THIS SUITE DOES ABOUT IT ───────────────────
 *
 * The corpus parses spans by brace-matching; the product parses with
 * tree-sitter. A boundary disagreement between the two is a hash mismatch that
 * has nothing to do with anchoring. So this suite PINS the range: it
 * materialises the exact bytes the corpus hashed and registers the node over
 * exactly those lines. Nothing here asks the indexer where a function starts,
 * and therefore nothing here can be reddened by a parser difference.
 *
 * Line numbers inside a materialised file are NOT the line numbers in the
 * original blob — a tree is a concatenation of the spans a control needs, so
 * `src/store/store.c` here is ten lines long rather than six thousand. That is
 * sound because resolution never reads a line number (anchor.h): the QN comes
 * from the path and the symbol, the hash comes from the bytes, and both are
 * preserved exactly. The original ranges are in the corpus, beside the
 * commits, for re-derivation.
 *
 * ── THE READ PATH ──────────────────────────────────────────────────────────
 *
 * g3_read is the composition the orphaning unit will own: resolve the address,
 * then decide which stored records are attached to it. It lives here because
 * that unit has not landed — but every DECISION inside it is made by product
 * code (hyp_anchor_resolve classifies; hyp_addr_relate and hyp_addr_equal
 * decide attachment), so a defect planted in the contract is visible through
 * it. It answers in the tool surface's vocabulary: resolved + an empty record
 * list means "the symbol is there and nothing is attached"; orphaned means the
 * record list is ABSENT. Absent means look elsewhere; empty means there is
 * nothing, and only one of them is ever true.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/identity.h"
#include "foundation/record.h"
#include "memory/anchor.h"
#include "pipeline/pipeline.h"
#include "store/record_store.h"
#include "store/store.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── The tables, generated from the corpus ───────────────────────── */

/* One node the fixture materialises. `span_key` names its text in
 * tests/fixtures/g3/g3_corpus.txt; an empty key is a node the indexer itself
 * builds without a file or a span — an env key is upserted with an empty path
 * and lines 0,0 (pass_definitions.c), and the fixture mirrors that exactly. */
typedef struct {
    const char *tree;
    const char *project;
    const char *rel_path;
    const char *label;
    const char *symbol;
    const char *span_key;
} g3_node_spec_t;

/* A record already in the store when a control asks its question. Content is
 * the corpus's own text for that control; the assertion is about the ADDRESS a
 * record is attached to, never about what it says. */
typedef struct {
    const char *anchor;
    const char *content;
} g3_record_spec_t;

#include "g3_tables.h"

/* ── Fixture loading ─────────────────────────────────────────────── */

enum {
    G3_MAX_SPANS = 64,
    G3_MAX_LIVE = 64,
    G3_MAX_FILES = 32,
    G3_LINE_MAX = 16384,
    G3_MATCH_MAX = 8
};

typedef struct {
    char key[160];
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    int nlines;
    char *text; /* lines joined by '\n', no trailing newline */
} g3_span_t;

static g3_span_t g3_spans[G3_MAX_SPANS];
static int g3_span_count = -1; /* -1 = not loaded, -2 = load failed */

#define G3_FIXTURE_PATH "tests/fixtures/g3/g3_corpus.txt"

/* Load the span text once. Returns false when the fixture cannot be read or
 * does not parse — a control that cannot run is reported, never skipped. */
static bool g3_spans_load(void) {
    if (g3_span_count >= 0) {
        return true;
    }
    if (g3_span_count == -2) {
        return false;
    }
    FILE *fp = fopen(G3_FIXTURE_PATH, "rb");
    if (!fp) {
        g3_span_count = -2;
        return false;
    }
    g3_span_count = 0;
    char *line = malloc(G3_LINE_MAX);
    if (!line) {
        (void)fclose(fp);
        g3_span_count = -2;
        return false;
    }
    while (fgets(line, G3_LINE_MAX, fp)) {
        if (strncmp(line, "SPAN ", 5) != 0) {
            continue;
        }
        char key[160];
        char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
        int nlines = 0;
        if (sscanf(line + 5, "%159s %32s %d", key, hash, &nlines) != 3 || nlines <= 0 ||
            g3_span_count >= G3_MAX_SPANS) {
            g3_span_count = -2;
            break;
        }
        size_t cap = 1024;
        size_t used = 0;
        char *text = malloc(cap);
        if (!text) {
            g3_span_count = -2;
            break;
        }
        text[0] = '\0';
        bool ok = true;
        for (int i = 0; i < nlines; i++) {
            if (!fgets(line, G3_LINE_MAX, fp)) {
                ok = false;
                break;
            }
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[--len] = '\0';
            }
            while (used + len + 2 > cap) {
                cap *= 2;
                char *grown = realloc(text, cap);
                if (!grown) {
                    ok = false;
                    break;
                }
                text = grown;
            }
            if (!ok) {
                break;
            }
            if (i > 0) {
                text[used++] = '\n';
            }
            memcpy(text + used, line, len);
            used += len;
            text[used] = '\0';
        }
        if (!ok) {
            free(text);
            g3_span_count = -2;
            break;
        }
        g3_span_t *s = &g3_spans[g3_span_count++];
        snprintf(s->key, sizeof(s->key), "%s", key);
        snprintf(s->hash, sizeof(s->hash), "%s", hash);
        s->nlines = nlines;
        s->text = text;
    }
    free(line);
    (void)fclose(fp);
    return g3_span_count >= 0;
}

static const g3_span_t *g3_span_find(const char *key) {
    for (int i = 0; i < g3_span_count; i++) {
        if (strcmp(g3_spans[i].key, key) == 0) {
            return &g3_spans[i];
        }
    }
    return NULL;
}

/* ── A world: one store, one record store, N materialised repos ──── */

typedef struct {
    char address[HYP_ADDR_MAX + 1];
    char qn[HYP_ADDR_QN_MAX + 1];
    const char *project;
    const char *rel_path;
    int start_line;
    int end_line;
    const char *corpus_hash; /* "" when the span carries no recorded hash */
} g3_live_t;

typedef struct {
    char root[512];
    const char *workspace; /* NULL = workspace of one */
    hyp_store_t *store;
    hyp_record_store_t *records;
    g3_live_t live[G3_MAX_LIVE];
    int live_count;
    char failure[512];
} g3_world_t;

typedef struct {
    char tree[64];
    char rel[512];
    char *buf;
    size_t used;
    size_t cap;
    int lines;
} g3_file_t;

static g3_file_t *g3_file_for(g3_file_t *files, int *count, const char *tree, const char *rel) {
    for (int i = 0; i < *count; i++) {
        if (strcmp(files[i].tree, tree) == 0 && strcmp(files[i].rel, rel) == 0) {
            return &files[i];
        }
    }
    if (*count >= G3_MAX_FILES) {
        return NULL;
    }
    g3_file_t *f = &files[(*count)++];
    memset(f, 0, sizeof(*f));
    snprintf(f->tree, sizeof(f->tree), "%s", tree);
    snprintf(f->rel, sizeof(f->rel), "%s", rel);
    f->cap = 4096;
    f->buf = malloc(f->cap);
    if (!f->buf) {
        return NULL;
    }
    f->buf[0] = '\0';
    return f;
}

static bool g3_file_append(g3_file_t *f, const char *text) {
    size_t len = strlen(text);
    while (f->used + len + 2 > f->cap) {
        f->cap *= 2;
        char *grown = realloc(f->buf, f->cap);
        if (!grown) {
            return false;
        }
        f->buf = grown;
    }
    memcpy(f->buf + f->used, text, len);
    f->used += len;
    f->buf[f->used++] = '\n';
    f->buf[f->used] = '\0';
    return true;
}

static bool g3_tree_wanted(const char *const *trees, int ntrees, const char *tree) {
    for (int i = 0; i < ntrees; i++) {
        if (strcmp(trees[i], tree) == 0) {
            return true;
        }
    }
    return false;
}

static void g3_world_close(g3_world_t *w) {
    if (!w) {
        return;
    }
    if (w->records) {
        hyp_record_store_close(w->records);
        w->records = NULL;
    }
    if (w->store) {
        hyp_store_close(w->store);
        w->store = NULL;
    }
    if (w->root[0]) {
        th_cleanup(w->root);
        w->root[0] = '\0';
    }
}

/* Build a world: materialise every tree named, register its nodes with the
 * REAL qualified-name builder, and seed the record store with every corpus
 * record. The record set is the same in every world on purpose — a control
 * that returns nothing must be seen to return nothing FROM A STORE THAT HAS
 * SOMETHING, or it is only proving the store is empty. */
static bool g3_world_open(g3_world_t *w, const char *const *trees, int ntrees,
                          const char *workspace) {
    memset(w, 0, sizeof(*w));
    w->workspace = workspace;
    if (!g3_spans_load()) {
        snprintf(w->failure, sizeof(w->failure), "cannot read %s", G3_FIXTURE_PATH);
        return false;
    }
    char *dir = th_mktempdir("hyp-g3");
    if (!dir) {
        snprintf(w->failure, sizeof(w->failure), "no temp directory");
        return false;
    }
    snprintf(w->root, sizeof(w->root), "%s", dir);

    g3_file_t files[G3_MAX_FILES];
    int file_count = 0;
    memset(files, 0, sizeof(files));

    w->store = hyp_store_open_memory();
    if (!w->store) {
        snprintf(w->failure, sizeof(w->failure), "store would not open");
        return false;
    }
    for (int i = 0; i < ntrees; i++) {
        const char *project = NULL;
        for (size_t n = 0; n < sizeof(G3_NODES) / sizeof(G3_NODES[0]); n++) {
            if (strcmp(G3_NODES[n].tree, trees[i]) == 0) {
                project = G3_NODES[n].project;
                break;
            }
        }
        if (!project) {
            snprintf(w->failure, sizeof(w->failure), "no nodes for tree %s", trees[i]);
            return false;
        }
        char root[768];
        snprintf(root, sizeof(root), "%s/%s", w->root, trees[i]);
        if (hyp_store_upsert_project(w->store, project, root) != HYP_STORE_OK) {
            snprintf(w->failure, sizeof(w->failure), "project %s refused", project);
            return false;
        }
    }

    /* Pass one: lay the span text out, remembering where each node landed. */
    for (size_t n = 0; n < sizeof(G3_NODES) / sizeof(G3_NODES[0]); n++) {
        const g3_node_spec_t *spec = &G3_NODES[n];
        if (!g3_tree_wanted(trees, ntrees, spec->tree)) {
            continue;
        }
        if (w->live_count >= G3_MAX_LIVE) {
            snprintf(w->failure, sizeof(w->failure), "too many nodes");
            return false;
        }
        g3_live_t *live = &w->live[w->live_count];
        memset(live, 0, sizeof(*live));
        live->project = spec->project;
        live->rel_path = spec->rel_path;
        live->corpus_hash = "";
        if (spec->span_key[0] == '\0') {
            /* An env key: no file, no span, exactly as the indexer builds it. */
            snprintf(live->qn, sizeof(live->qn), "__env__%s", spec->symbol);
            live->start_line = 0;
            live->end_line = 0;
        } else {
            const g3_span_t *span = g3_span_find(spec->span_key);
            if (!span) {
                snprintf(w->failure, sizeof(w->failure), "fixture has no span %s", spec->span_key);
                return false;
            }
            g3_file_t *f = g3_file_for(files, &file_count, spec->tree, spec->rel_path);
            if (!f || !g3_file_append(f, span->text)) {
                snprintf(w->failure, sizeof(w->failure), "cannot lay out %s", spec->span_key);
                return false;
            }
            live->start_line = f->lines + 1;
            live->end_line = f->lines + span->nlines;
            f->lines += span->nlines;
            live->corpus_hash = span->hash;
            char *qn = hyp_pipeline_fqn_compute(spec->project, spec->rel_path, spec->symbol);
            if (!qn) {
                snprintf(w->failure, sizeof(w->failure), "no QN for %s", spec->symbol);
                return false;
            }
            snprintf(live->qn, sizeof(live->qn), "%s", qn);
            free(qn);
        }
        hyp_addr_t addr;
        if (hyp_addr_init(&addr, workspace, spec->project, live->qn) != HYP_ADDR_OK ||
            !hyp_addr_format(&addr, live->address, sizeof(live->address))) {
            snprintf(w->failure, sizeof(w->failure), "no address for %s", live->qn);
            return false;
        }
        w->live_count++;
    }

    /* Pass two: write the files, then register the nodes over the exact lines
     * the layout produced. */
    for (int i = 0; i < file_count; i++) {
        char path[1400];
        snprintf(path, sizeof(path), "%s/%s/%s", w->root, files[i].tree, files[i].rel);
        if (th_write_file(path, files[i].buf) != 0) {
            snprintf(w->failure, sizeof(w->failure), "cannot write %s", files[i].rel);
            return false;
        }
    }
    for (int i = 0; i < file_count; i++) {
        free(files[i].buf);
        files[i].buf = NULL;
    }
    int li = 0;
    for (size_t n = 0; n < sizeof(G3_NODES) / sizeof(G3_NODES[0]); n++) {
        const g3_node_spec_t *spec = &G3_NODES[n];
        if (!g3_tree_wanted(trees, ntrees, spec->tree)) {
            continue;
        }
        const g3_live_t *live = &w->live[li++];
        hyp_node_t node = {.project = spec->project,
                           .label = spec->label,
                           .name = spec->symbol,
                           .qualified_name = live->qn,
                           .file_path = spec->rel_path,
                           .start_line = live->start_line,
                           .end_line = live->end_line,
                           .properties_json = NULL};
        if (hyp_store_upsert_node(w->store, &node) <= 0) {
            snprintf(w->failure, sizeof(w->failure), "node %s refused", live->qn);
            return false;
        }
    }

    /* The record store: every corpus record, in every world. */
    char mem[768];
    snprintf(mem, sizeof(mem), "%s/memory", w->root);
    if (hyp_record_store_open(mem, &w->records) != HYP_RECORD_STORE_OK) {
        snprintf(w->failure, sizeof(w->failure), "record store would not open");
        return false;
    }
    for (size_t r = 0; r < sizeof(G3_RECORDS) / sizeof(G3_RECORDS[0]); r++) {
        hyp_record_input_t in;
        memset(&in, 0, sizeof(in));
        in.kind = HYP_RECORD_DECISION;
        in.author = "corpus:track-g";
        /* Caller-supplied and fixed: this suite reads no clock, and neither
         * does the store (I6). */
        in.timestamp_ms = INT64_C(1770985600000);
        in.content = G3_RECORDS[r].content;
        in.anchor = G3_RECORDS[r].anchor;
        const hyp_record_t *rec = NULL;
        if (hyp_record_build(&in, &rec) != HYP_RECORD_OK || !rec) {
            snprintf(w->failure, sizeof(w->failure), "record %u refused", (unsigned)r);
            return false;
        }
        hyp_record_store_status_t st = hyp_record_store_append(w->records, rec, NULL);
        hyp_record_free(rec);
        if (st != HYP_RECORD_STORE_OK) {
            snprintf(w->failure, sizeof(w->failure), "record %u not stored", (unsigned)r);
            return false;
        }
    }
    return true;
}

/* ── The read path ───────────────────────────────────────────────── */

typedef struct {
    hyp_anchor_status_t status;
    /* false means the record list is ABSENT — the anchor did not resolve, so
     * "nothing is attached" would be a claim about the wrong universe. */
    bool records_present;
    int record_count;
    /* How many anchored records the read path examined. A control that says NO
     * while this is zero has only proven the store was empty. */
    int considered;
    char matched[G3_MATCH_MAX][HYP_ANCHOR_MAX + 1];
    int candidate_count;
    char candidates[G3_MATCH_MAX][HYP_ADDR_MAX + 1];
    /* Where the anchor resolved, when it did. */
    char project[HYP_ADDR_SLUG_MAX + 1];
    char file_path[HYP_PATH_MAX];
    int start_line;
    char reason[512];
} g3_answer_t;

static void g3_read(g3_world_t *w, const char *address, g3_answer_t *out) {
    memset(out, 0, sizeof(*out));
    hyp_anchor_res_t res;
    out->status = hyp_anchor_resolve(w->store, w->workspace, address, &res);
    snprintf(out->reason, sizeof(out->reason), "%s", res.reason);
    snprintf(out->project, sizeof(out->project), "%s", res.project);
    snprintf(out->file_path, sizeof(out->file_path), "%s", res.file_path);
    out->start_line = res.start_line;
    for (int i = 0; i < res.candidate_count && out->candidate_count < G3_MATCH_MAX; i++) {
        snprintf(out->candidates[out->candidate_count++], HYP_ADDR_MAX + 1, "%s",
                 res.candidates[i].address);
    }
    if (out->status != HYP_ANCHOR_RESOLVED && out->status != HYP_ANCHOR_RESOLVED_EDITED) {
        hyp_anchor_res_free(&res);
        return; /* records ABSENT */
    }
    out->records_present = true;

    hyp_anchor_t queried;
    if (hyp_anchor_parse(address, &queried) != HYP_ADDR_OK) {
        hyp_anchor_res_free(&res);
        return;
    }
    hyp_record_store_query_t query;
    memset(&query, 0, sizeof(query));
    query.anchored_only = true;
    hyp_record_set_t *set = NULL;
    if (hyp_record_store_query(w->records, &query, &set) != HYP_RECORD_STORE_OK || !set) {
        hyp_anchor_res_free(&res);
        return;
    }
    size_t count = hyp_record_set_count(set);
    for (size_t i = 0; i < count; i++) {
        const hyp_record_t *rec = hyp_record_set_at(set, i);
        hyp_anchor_t held;
        if (!rec || !rec->anchor || hyp_anchor_parse(rec->anchor, &held) != HYP_ADDR_OK) {
            continue;
        }
        out->considered++;
        /* THE GUARD. A record is attached when the identity contract says the
         * two observations are the same node — SAME or EDITED. CONTENT_ONLY is
         * a candidate and never an attachment (I5), which is the whole of the
         * cross-repo isolation control: two byte-identical spans in two repos
         * are two addresses. */
        bool attach;
        if (held.has_hash && res.current_hash[0]) {
            hyp_addr_rel_t rel =
                hyp_addr_relate(&held.addr, held.span_hash, &queried.addr, res.current_hash);
            attach = (rel == HYP_ADDR_REL_SAME || rel == HYP_ADDR_REL_EDITED);
        } else {
            attach = hyp_addr_equal(&held.addr, &queried.addr);
        }
        if (attach) {
            /* The COUNT is never capped, only the sample of anchors kept beside
             * it: a count that silently saturates would understate exactly the
             * failure these controls exist to catch. */
            if (out->record_count < G3_MATCH_MAX) {
                snprintf(out->matched[out->record_count], HYP_ANCHOR_MAX + 1, "%s", rec->anchor);
            }
            out->record_count++;
        }
    }
    hyp_record_set_free(set);
    hyp_anchor_res_free(&res);
}

/* Does this answer name `address` anywhere a reader would take as an
 * attachment — the place it resolved, a candidate, or a returned record? */
static bool g3_answer_names(const g3_world_t *w, const g3_answer_t *ans, const char *address) {
    for (int i = 0; i < ans->candidate_count; i++) {
        if (strcmp(ans->candidates[i], address) == 0) {
            return true;
        }
    }
    for (int i = 0; i < ans->record_count; i++) {
        const char *at = strchr(ans->matched[i], '@');
        const char *addr_part = at ? at + 1 : ans->matched[i];
        if (strcmp(addr_part, address) == 0) {
            return true;
        }
    }
    if (ans->status == HYP_ANCHOR_RESOLVED || ans->status == HYP_ANCHOR_RESOLVED_EDITED) {
        for (int i = 0; i < w->live_count; i++) {
            const g3_live_t *live = &w->live[i];
            if (strcmp(live->address, address) != 0) {
                continue;
            }
            if (strcmp(live->project, ans->project) == 0 &&
                strcmp(live->rel_path, ans->file_path) == 0 &&
                live->start_line == ans->start_line) {
                return true;
            }
        }
    }
    return false;
}

/* ── Shared shapes ───────────────────────────────────────────────── */

/* Append one violation to a control's verdict. EVERY check in a control runs,
 * and every one that fails is reported: a control that stops at its first
 * failure hides the assertions behind it, and an assertion that has never been
 * seen to fire is the class this suite exists to rule out. */
static void g3_note(char *why, size_t cap, const char *fmt, ...) {
    size_t used = strlen(why);
    if (used + 8 >= cap) {
        return;
    }
    if (used > 0) {
        snprintf(why + used, cap - used, " | ");
        used = strlen(why);
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(why + used, cap - used, fmt, ap);
    va_end(ap);
}

/* Every renamed control asserts the same four things about the same shape, so
 * the shape is written once and the CASES are the data. `after_addr` is the
 * positive complement: the symbol the rename produced IS in this index, and it
 * resolves — without it, ORPHANED could be the answer to an empty tree. */
static int g3_check_renamed(const char *tree, const char *anchor, const char *after_addr,
                            const char *after_hash, const char *const *must_not, int must_not_count,
                            char *why, size_t why_sz) {
    const char *trees[1] = {tree};
    g3_world_t w;
    if (!g3_world_open(&w, trees, 1, NULL)) {
        snprintf(why, why_sz, "fixture: %s", w.failure);
        g3_world_close(&w);
        return -1;
    }
    g3_answer_t ans;
    g3_read(&w, anchor, &ans);
    if (ans.status != HYP_ANCHOR_ORPHANED) {
        g3_note(why, why_sz, "status is %s, expected orphaned (resolved at %s:%d)",
                hyp_anchor_status_str(ans.status), ans.file_path, ans.start_line);
    }
    if (ans.records_present) {
        g3_note(why, why_sz, "the record list was present, not absent");
    }
    if (ans.candidate_count != 0) {
        g3_note(why, why_sz, "candidate_count is %d, expected 0 (first: %s)", ans.candidate_count,
                ans.candidates[0]);
    }
    for (int i = 0; i < must_not_count; i++) {
        if (g3_answer_names(&w, &ans, must_not[i])) {
            g3_note(why, why_sz, "re-attached to the plausible neighbour %s", must_not[i]);
        }
    }
    /* The positive complement, in the same index and the same call. */
    char after_anchor[HYP_ANCHOR_MAX + 1];
    hyp_anchor_t parsed;
    if (hyp_anchor_parse(after_addr, &parsed) != HYP_ADDR_OK ||
        !hyp_anchor_format(&parsed.addr, after_hash, after_anchor, sizeof(after_anchor))) {
        g3_note(why, why_sz, "could not build the after anchor");
    } else {
        g3_answer_t live;
        g3_read(&w, after_anchor, &live);
        if (live.status != HYP_ANCHOR_RESOLVED) {
            g3_note(why, why_sz, "the renamed-to symbol does not resolve: %s (%s)",
                    hyp_anchor_status_str(live.status), live.reason);
        }
    }
    g3_world_close(&w);
    return why[0] ? 1 : 0;
}

#define G3_RENAMED(tree, anchor, after_addr, after_hash, must_not, n)                       \
    do {                                                                                    \
        char _why[1024] = "";                                                               \
        int _rc = g3_check_renamed(tree, anchor, after_addr, after_hash, must_not, n, _why, \
                                   sizeof(_why));                                           \
        if (_rc != 0) {                                                                     \
            FAIL(_why);                                                                     \
        }                                                                                   \
    } while (0)

static const char *const g3_no_neighbours[1] = {NULL};

/* ── The fixture is what it claims to be ─────────────────────────── */

TEST(g3_fixture_reproduces_every_recorded_span_hash) {
    if (!g3_spans_load()) {
        FAIL("tests/fixtures/g3/g3_corpus.txt could not be read or parsed");
    }
    ASSERT_GT(g3_span_count, 0);
    for (int i = 0; i < g3_span_count; i++) {
        char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
        hyp_addr_span_hash(g3_spans[i].text, hash);
        if (strcmp(hash, g3_spans[i].hash) != 0) {
            printf("  span %s: %s != %s\n", g3_spans[i].key, hash, g3_spans[i].hash);
            FAIL("a fixture span no longer hashes to the value the corpus recorded");
        }
    }
    PASS();
}

TEST(g3_fixture_materialises_the_spans_the_corpus_hashed) {
    /* The pinned range, end to end: the bytes on disk in a materialised tree
     * hash to the corpus value when read back through the same span reader the
     * resolver uses. This is the parser caveat closed — no tree-sitter boundary
     * is consulted anywhere in this suite. */
    const char *trees[2] = {"forkA", "forkB"};
    g3_world_t w;
    if (!g3_world_open(&w, trees, 2, "fork-pair")) {
        char fixture[512];
        snprintf(fixture, sizeof(fixture), "%s", w.failure);
        g3_world_close(&w);
        FAIL(fixture);
    }
    int checked = 0;
    int rc = 0;
    for (int i = 0; i < w.live_count; i++) {
        const g3_live_t *live = &w.live[i];
        if (!live->corpus_hash[0]) {
            continue;
        }
        char anchor[HYP_ANCHOR_MAX + 1];
        hyp_anchor_t parsed;
        if (hyp_anchor_parse(live->address, &parsed) != HYP_ADDR_OK ||
            !hyp_anchor_format(&parsed.addr, live->corpus_hash, anchor, sizeof(anchor))) {
            rc = 1;
            break;
        }
        hyp_anchor_res_t res;
        hyp_anchor_status_t st = hyp_anchor_resolve(w.store, "fork-pair", anchor, &res);
        if (st != HYP_ANCHOR_RESOLVED || strcmp(res.current_hash, live->corpus_hash) != 0) {
            printf("  %s: %s current=%s corpus=%s\n", live->address, hyp_anchor_status_str(st),
                   res.current_hash, live->corpus_hash);
            rc = 1;
        }
        hyp_anchor_res_free(&res);
        checked++;
    }
    g3_world_close(&w);
    if (rc != 0) {
        FAIL("a materialised span does not read back as the corpus hashed it");
    }
    ASSERT_EQ(checked, 8);
    PASS();
}

TEST(g3_an_anchor_carries_no_line_number) {
    /* The sharpest rename trap in the corpus keeps the same file AND the same
     * line numbers, so a line-number anchor re-attaches with total confidence.
     * It cannot: the grammar has nowhere to put one. Parsing the anchor back
     * yields an address and a hash and nothing else. */
    hyp_anchor_t parsed;
    ASSERT_EQ(hyp_anchor_parse(NC_RENAME_003_ANCHOR, &parsed), HYP_ADDR_OK);
    ASSERT_TRUE(parsed.has_hash);
    char round[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&parsed.addr, parsed.span_hash, round, sizeof(round)));
    ASSERT_STR_EQ(round, NC_RENAME_003_ANCHOR);
    PASS();
}

TEST(g3_every_control_names_the_commit_pair_it_came_from) {
    /* The corpus's best credential is that a reader can go and check it. Each
     * row carries the control id and the two trees it was mined from, so any
     * expectation in this file can be re-derived with git rather than trusted. */
    const int rows = (int)(sizeof(G3_PROVENANCE) / sizeof(G3_PROVENANCE[0]));
    ASSERT_EQ(rows, 20);
    for (int i = 0; i < rows; i++) {
        ASSERT_NOT_NULL(G3_PROVENANCE[i][0]);
        ASSERT_GT((int)strlen(G3_PROVENANCE[i][0]), 0);
        for (int side = 1; side <= 2; side++) {
            const char *ref = G3_PROVENANCE[i][side];
            ASSERT_EQ((int)strlen(ref), 40);
            for (const char *p = ref; *p; p++) {
                ASSERT((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
            }
        }
    }
    PASS();
}

/* ── 1. Renamed symbols orphan, and name no neighbour ────────────── */

TEST(g3_nc_rename_001_toon_scalar_str_orphans_past_three_new_siblings) {
    G3_RENAMED(G1_RENAME_006_TREE, G1_RENAME_006_ANCHOR, G1_RENAME_006_AFTER_ADDR,
               G1_RENAME_006_AFTER_HASH, NC_RENAME_001_MUST_NOT, 4);
    PASS();
}

TEST(g3_nc_rename_002_activation_refusal_note_orphans_on_the_mass_rename) {
    G3_RENAMED(G1_RENAME_007_TREE, G1_RENAME_007_ANCHOR, G1_RENAME_007_AFTER_ADDR,
               G1_RENAME_007_AFTER_HASH, NC_RENAME_002_MUST_NOT, 1);
    PASS();
}

TEST(g3_nc_rename_003_free_node_fields_orphans_same_file_same_lines) {
    G3_RENAMED(G1_RENAME_001_TREE, G1_RENAME_001_ANCHOR, G1_RENAME_001_AFTER_ADDR,
               G1_RENAME_001_AFTER_HASH, NC_RENAME_003_MUST_NOT, 1);
    PASS();
}

TEST(g3_nc_rename_004_louvain_free_adj_orphans_renamed_edited_and_moved) {
    G3_RENAMED(G1_RENAME_008_TREE, G1_RENAME_008_ANCHOR, G1_RENAME_008_AFTER_ADDR,
               G1_RENAME_008_AFTER_HASH, NC_RENAME_004_MUST_NOT, 2);
    PASS();
}

TEST(g3_nc_rename_005_ha_read_stdin_orphans_to_an_unrelated_name) {
    G3_RENAMED(G1_RENAME_005_TREE, G1_RENAME_005_ANCHOR, G1_RENAME_005_AFTER_ADDR,
               G1_RENAME_005_AFTER_HASH, NC_RENAME_005_MUST_NOT, 2);
    PASS();
}

TEST(g3_g1_rename_002_pxc_collect_all_defs_orphans) {
    G3_RENAMED(G1_RENAME_002_TREE, G1_RENAME_002_ANCHOR, G1_RENAME_002_AFTER_ADDR,
               G1_RENAME_002_AFTER_HASH, g3_no_neighbours, 0);
    PASS();
}

TEST(g3_g1_rename_003_resolve_func_name_orphans) {
    G3_RENAMED(G1_RENAME_003_TREE, G1_RENAME_003_ANCHOR, G1_RENAME_003_AFTER_ADDR,
               G1_RENAME_003_AFTER_HASH, g3_no_neighbours, 0);
    PASS();
}

TEST(g3_g1_rename_004_cpp_out_of_line_parent_class_orphans) {
    G3_RENAMED(G1_RENAME_004_TREE, G1_RENAME_004_ANCHOR, G1_RENAME_004_AFTER_ADDR,
               G1_RENAME_004_AFTER_HASH, g3_no_neighbours, 0);
    PASS();
}

/* ── 2. Cross-repo isolation, on repo-scoped addresses only ──────── */

/* The two trees are two real repository identities from this project's own
 * history, and the four spans are BYTE-IDENTICAL in both — so content cannot
 * rescue a resolver here and only the repo field can. Stated limitation: they
 * are two checkouts of one lineage, which makes the control harder than a pair
 * of unrelated repositories would be, and is not a test of isolation between
 * genuinely unrelated codebases. */
static int g3_check_isolation(const char *record_anchor, const char *queried, const char *hash,
                              char *why, size_t why_sz) {
    const char *trees[2] = {"forkA", "forkB"};
    g3_world_t w;
    if (!g3_world_open(&w, trees, 2, "fork-pair")) {
        snprintf(why, why_sz, "fixture: %s", w.failure);
        g3_world_close(&w);
        return -1;
    }
    /* The premise of the control, asserted rather than assumed: the queried
     * repo's span is byte-identical to the span the other repo's record was
     * written against, so content cannot rescue a resolver here and only the
     * repo field can. */
    char probe[HYP_ANCHOR_MAX + 1];
    hyp_anchor_t parsed;
    if (hyp_anchor_parse(queried, &parsed) != HYP_ADDR_OK ||
        !hyp_anchor_format(&parsed.addr, hash, probe, sizeof(probe))) {
        g3_note(why, why_sz, "could not build the byte-identity probe");
    } else {
        hyp_anchor_res_t res;
        hyp_anchor_status_t probed = hyp_anchor_resolve(w.store, "fork-pair", probe, &res);
        bool identical = (probed == HYP_ANCHOR_RESOLVED && res.relation == HYP_ADDR_REL_SAME);
        hyp_anchor_res_free(&res);
        if (!identical) {
            g3_note(why, why_sz,
                    "the two repos' spans are not byte-identical, so the control is "
                    "weaker than the corpus claims");
        }
    }
    g3_answer_t ans;
    g3_read(&w, queried, &ans);
    if (ans.status != HYP_ANCHOR_RESOLVED) {
        g3_note(why, why_sz, "the queried symbol does not resolve: %s (%s)",
                hyp_anchor_status_str(ans.status), ans.reason);
    }
    if (ans.status == HYP_ANCHOR_RESOLVED && !ans.records_present) {
        g3_note(why, why_sz, "resolved, but the record list was absent rather than empty");
    }
    if (ans.record_count != 0) {
        g3_note(why, why_sz, "returned %d record(s), first %s", ans.record_count, ans.matched[0]);
    }
    if (ans.records_present && ans.considered == 0) {
        g3_note(why, why_sz, "no anchored record was examined — the store was empty");
    }
    if (g3_answer_names(&w, &ans, record_anchor)) {
        g3_note(why, why_sz, "named the other repository's record");
    }
    /* The other end of the same fact: the record IS findable from its own
     * repository, so the empty answer above is isolation and not absence. */
    const char *at = strchr(record_anchor, '@');
    g3_answer_t home;
    g3_read(&w, at ? at + 1 : record_anchor, &home);
    if (home.record_count != 1) {
        g3_note(why, why_sz, "the record is not findable from its own repo: %d found",
                home.record_count);
    } else if (strcmp(home.matched[0], record_anchor) != 0) {
        g3_note(why, why_sz, "its own repo returned a different record");
    }
    g3_world_close(&w);
    return why[0] ? 1 : 0;
}

#define G3_ISOLATION(record_anchor, queried, hash)                                      \
    do {                                                                                \
        char _why[1024] = "";                                                           \
        int _rc = g3_check_isolation(record_anchor, queried, hash, _why, sizeof(_why)); \
        if (_rc != 0) {                                                                 \
            FAIL(_why);                                                                 \
        }                                                                               \
    } while (0)

TEST(g3_nc_iso_001_ha_sanitize_metadata_is_not_shared_across_repos) {
    G3_ISOLATION(NC_ISO_001_RECORD_ANCHOR, NC_ISO_001_QUERY, NC_ISO_001_HASH);
    PASS();
}

TEST(g3_nc_iso_002_ws_volume_prefix_len_is_not_shared_across_repos) {
    G3_ISOLATION(NC_ISO_002_RECORD_ANCHOR, NC_ISO_002_QUERY, NC_ISO_002_HASH);
    PASS();
}

TEST(g3_nc_iso_003_parse_comparison_op_is_not_shared_across_repos) {
    G3_ISOLATION(NC_ISO_003_RECORD_ANCHOR, NC_ISO_003_QUERY, NC_ISO_003_HASH);
    PASS();
}

TEST(g3_nc_iso_004_slab_map_set_is_not_shared_across_repos) {
    G3_ISOLATION(NC_ISO_004_RECORD_ANCHOR, NC_ISO_004_QUERY, NC_ISO_004_HASH);
    PASS();
}

/* ── 3. The rendezvous pair: one YES, one NO ─────────────────────── */

TEST(g3_pc_rendezvous_001_env_home_is_found_from_the_other_repo) {
    /* THIS ONE MUST SAY YES. The address is workspace-scoped by SHAPE (I2):
     * leading __tag__, not project-rooted, so it carries no repo field at all
     * and two repositories sharing it is the point. Running the isolation
     * control here would assert the opposite of the contract. */
    const char *trees[2] = {"forkA", "forkB"};
    g3_world_t w;
    if (!g3_world_open(&w, trees, 2, "fork-pair")) {
        char fixture[512];
        snprintf(fixture, sizeof(fixture), "%s", w.failure);
        g3_world_close(&w);
        FAIL(fixture);
    }
    hyp_addr_t addr;
    char why[1024] = "";
    if (hyp_addr_parse(PC_RENDEZVOUS_001_ADDR, &addr) != HYP_ADDR_OK) {
        g3_note(why, sizeof(why), "the rendezvous address does not parse");
    } else if (addr.scope != HYP_ADDR_SCOPE_WORKSPACE) {
        g3_note(why, sizeof(why), "the rendezvous address is not workspace-scoped");
    }
    g3_answer_t ans;
    g3_read(&w, PC_RENDEZVOUS_001_ADDR, &ans);
    if (ans.status != HYP_ANCHOR_RESOLVED) {
        g3_note(why, sizeof(why), "status is %s, expected resolved (%s)",
                hyp_anchor_status_str(ans.status), ans.reason);
    }
    if (!ans.records_present || ans.record_count != 1) {
        g3_note(why, sizeof(why), "found %d record(s), expected exactly 1",
                ans.records_present ? ans.record_count : -1);
    }
    if (ans.record_count >= 1 && strcmp(ans.matched[0], PC_RENDEZVOUS_001_ADDR) != 0) {
        g3_note(why, sizeof(why), "found the wrong record: %s", ans.matched[0]);
    }
    g3_world_close(&w);
    if (why[0]) {
        FAIL(why);
    }
    PASS();
}

TEST(g3_nc_rendezvous_002_env_cbm_cache_dir_orphans_and_names_no_successor) {
    /* Workspace scope is not a licence to guess. The env key is gone from every
     * repo in this workspace, and the renamed variable that IS present must not
     * be substituted for it. */
    const char *trees[1] = {"forkB"};
    g3_world_t w;
    if (!g3_world_open(&w, trees, 1, "fork-pair")) {
        char fixture[512];
        snprintf(fixture, sizeof(fixture), "%s", w.failure);
        g3_world_close(&w);
        FAIL(fixture);
    }
    char why[1024] = "";
    g3_answer_t ans;
    g3_read(&w, NC_RENDEZVOUS_002_ADDR, &ans);
    if (ans.status != HYP_ANCHOR_ORPHANED) {
        g3_note(why, sizeof(why), "status is %s, expected orphaned",
                hyp_anchor_status_str(ans.status));
    }
    if (ans.records_present) {
        g3_note(why, sizeof(why), "the record list was present, not absent");
    }
    if (ans.candidate_count != 0) {
        g3_note(why, sizeof(why), "candidate_count is %d, expected 0", ans.candidate_count);
    }
    if (g3_answer_names(&w, &ans, NC_RENDEZVOUS_002_MUST_NOT)) {
        g3_note(why, sizeof(why), "substituted the renamed successor");
    }
    /* The successor is genuinely present in this index: the orphan verdict
     * above is a refusal to guess, not an empty workspace. */
    g3_answer_t successor;
    g3_read(&w, NC_RENDEZVOUS_002_MUST_NOT, &successor);
    if (successor.status != HYP_ANCHOR_RESOLVED) {
        g3_note(why, sizeof(why), "the successor env key is not in this index: %s",
                hyp_anchor_status_str(successor.status));
    }
    g3_world_close(&w);
    if (why[0]) {
        FAIL(why);
    }
    PASS();
}

/* ── 4. Never recorded: resolved, and the record list is EMPTY ───── */

/* The symbol's name occurs zero times across every commit message, the plan,
 * every tracked document and every comment in the tree. There is no decision
 * to find, so the honest answer is `resolved` with an empty record list — a
 * different sentence from `orphaned`, and both different from an error. The
 * claim these support is narrow and exact: no record in this corpus NAMES this
 * symbol; a function can be explained without being named. */
static int g3_check_unrecorded(const char *address, char *why, size_t why_sz) {
    const char *trees[1] = {"unrecorded"};
    g3_world_t w;
    if (!g3_world_open(&w, trees, 1, NULL)) {
        snprintf(why, why_sz, "fixture: %s", w.failure);
        g3_world_close(&w);
        return -1;
    }
    g3_answer_t ans;
    g3_read(&w, address, &ans);
    if (ans.status != HYP_ANCHOR_RESOLVED) {
        g3_note(why, why_sz, "status is %s, expected resolved (%s)",
                hyp_anchor_status_str(ans.status), ans.reason);
    }
    if (ans.status == HYP_ANCHOR_RESOLVED && !ans.records_present) {
        g3_note(why, why_sz,
                "the record list was ABSENT; for a resolved anchor it must be "
                "present and empty");
    }
    if (ans.record_count != 0) {
        g3_note(why, why_sz, "returned %d record(s), first %s", ans.record_count, ans.matched[0]);
    }
    if (ans.considered != (int)(sizeof(G3_RECORDS) / sizeof(G3_RECORDS[0]))) {
        g3_note(why, why_sz,
                "examined %d anchored records, expected %d — an empty store proves "
                "nothing",
                ans.considered, (int)(sizeof(G3_RECORDS) / sizeof(G3_RECORDS[0])));
    }
    g3_world_close(&w);
    return why[0] ? 1 : 0;
}

#define G3_UNRECORDED(address)                                      \
    do {                                                            \
        char _why[1024] = "";                                       \
        int _rc = g3_check_unrecorded(address, _why, sizeof(_why)); \
        if (_rc != 0) {                                             \
            FAIL(_why);                                             \
        }                                                           \
    } while (0)

TEST(g3_nc_unrecorded_001_run_closure_delta_has_nothing_attached) {
    G3_UNRECORDED(NC_UNRECORDED_001_ADDR);
    PASS();
}

TEST(g3_nc_unrecorded_002_hyp_layout_compute_has_nothing_attached) {
    G3_UNRECORDED(NC_UNRECORDED_002_ADDR);
    PASS();
}

TEST(g3_nc_unrecorded_003_kt_repair_bodyless_interface_methods_has_nothing_attached) {
    G3_UNRECORDED(NC_UNRECORDED_003_ADDR);
    PASS();
}

TEST(g3_nc_unrecorded_004_process_class_for_fields_has_nothing_attached) {
    G3_UNRECORDED(NC_UNRECORDED_004_ADDR);
    PASS();
}

TEST(g3_nc_unrecorded_005_rust_eval_expr_type_has_nothing_attached) {
    G3_UNRECORDED(NC_UNRECORDED_005_ADDR);
    PASS();
}

TEST(g3_nc_unrecorded_006_c_neg_memo_insert_has_nothing_attached) {
    G3_UNRECORDED(NC_UNRECORDED_006_ADDR);
    PASS();
}

/* ── Suite ───────────────────────────────────────────────────────── */

SUITE(g3_controls) {
    /* The fixture is what it claims to be */
    RUN_TEST(g3_fixture_reproduces_every_recorded_span_hash);
    RUN_TEST(g3_fixture_materialises_the_spans_the_corpus_hashed);
    RUN_TEST(g3_an_anchor_carries_no_line_number);
    RUN_TEST(g3_every_control_names_the_commit_pair_it_came_from);

    /* Renamed -> orphaned, never the plausible neighbour */
    RUN_TEST(g3_nc_rename_001_toon_scalar_str_orphans_past_three_new_siblings);
    RUN_TEST(g3_nc_rename_002_activation_refusal_note_orphans_on_the_mass_rename);
    RUN_TEST(g3_nc_rename_003_free_node_fields_orphans_same_file_same_lines);
    RUN_TEST(g3_nc_rename_004_louvain_free_adj_orphans_renamed_edited_and_moved);
    RUN_TEST(g3_nc_rename_005_ha_read_stdin_orphans_to_an_unrelated_name);
    RUN_TEST(g3_g1_rename_002_pxc_collect_all_defs_orphans);
    RUN_TEST(g3_g1_rename_003_resolve_func_name_orphans);
    RUN_TEST(g3_g1_rename_004_cpp_out_of_line_parent_class_orphans);

    /* Cross-repo isolation, repo-scoped addresses only */
    RUN_TEST(g3_nc_iso_001_ha_sanitize_metadata_is_not_shared_across_repos);
    RUN_TEST(g3_nc_iso_002_ws_volume_prefix_len_is_not_shared_across_repos);
    RUN_TEST(g3_nc_iso_003_parse_comparison_op_is_not_shared_across_repos);
    RUN_TEST(g3_nc_iso_004_slab_map_set_is_not_shared_across_repos);

    /* The rendezvous pair: the positive control, and its complement */
    RUN_TEST(g3_pc_rendezvous_001_env_home_is_found_from_the_other_repo);
    RUN_TEST(g3_nc_rendezvous_002_env_cbm_cache_dir_orphans_and_names_no_successor);

    /* Nothing was ever recorded about these */
    RUN_TEST(g3_nc_unrecorded_001_run_closure_delta_has_nothing_attached);
    RUN_TEST(g3_nc_unrecorded_002_hyp_layout_compute_has_nothing_attached);
    RUN_TEST(g3_nc_unrecorded_003_kt_repair_bodyless_interface_methods_has_nothing_attached);
    RUN_TEST(g3_nc_unrecorded_004_process_class_for_fields_has_nothing_attached);
    RUN_TEST(g3_nc_unrecorded_005_rust_eval_expr_type_has_nothing_attached);
    RUN_TEST(g3_nc_unrecorded_006_c_neg_memo_insert_has_nothing_attached);
}
