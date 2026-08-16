/*
 * test_anchor_reindex.c — unit C5u: an anchor survives a re-index.
 *
 * `foo` moves 200 lines and the decision is still attached. That is the whole
 * unit, and this file proves it twice, in two different ways, because the two
 * failure modes are different:
 *
 *   REPLAY, over history nobody authored. The Track G corpus mined 2,103
 *   commits for symbols that actually moved and actually changed, and
 *   tests/fixtures/g1_anchors materialises the sixteen this unit consumes —
 *   eight moved-within-file (real shifts of +275 to +996 lines) and eight
 *   body-changed. Every span in that directory was re-derived from the commit
 *   the corpus names and its hash re-checked before it was written; nothing
 *   here was invented to make a point.
 *
 *   FIRST REAL USE, end to end. A decision written through the real record
 *   store against a symbol the real indexer found, then the symbol moved, the
 *   tree RE-INDEXED by the real pipeline, and the decision read back. A unit
 *   test can only prove the anchor agrees with itself; only this can prove the
 *   store, the anchor and the index agree with each other.
 *
 * ── The parser caveat, and how this file discharges it ───────────────────────
 *
 * The corpus parses spans by brace-matching; the product uses tree-sitter, and
 * a boundary disagreement is a hash mismatch with nothing to do with
 * anchoring. Both remedies the corpus offers are used, one per half:
 *
 *   replay PINS THE RANGE — the node is upserted at the exact lines the
 *   fixture states, so tree-sitter never gets a vote on the boundary, and the
 *   hash the product computes over that range is asserted equal to the
 *   corpus's own. A disagreement there is a finding about the hash, not about
 *   a parser.
 *
 *   the end-to-end half RECOMPUTES FROM THE PRODUCT'S OWN SPAN — no fixture
 *   hash appears in it at all. The anchor is written from whatever tree-sitter
 *   said the symbol was, and the assertion is on the RELATION afterwards.
 *
 * ── What "still attached" is, and what it is not ─────────────────────────────
 *
 * A moved symbol is RESOLVED; an edited one is RESOLVED_EDITED. Both are still
 * attached, and collapsing them is a defect in either direction: the span
 * includes the declaration line (I4), so a rename changes the qualified name
 * AND the hash, while an edit changes only the hash — the two states are how a
 * reader learns whether the reasoning it just retrieved describes the code in
 * front of it. Three fixtures slide ZERO lines while their content changes and
 * one slides EXACTLY ONE, which is the sharpest discriminator history offers
 * between "moved" and "edited": an implementation that decided by line number
 * gets both classes exactly backwards.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "../src/ask/ask_embed.h"
#include "../src/foundation/identity.h"
#include "../src/foundation/record.h"
#include "../src/memory/anchor.h"
#include "../src/pipeline/pipeline.h"
#include "../src/store/record_store.h"
#include "../src/store/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The corpus's own assumptions, restated where they are consumed: the project
 * slug it computed qualified names under, and where the spans it names live. */
#define C5U_PROJECT "hyponoia"
#define C5U_FIXTURE_DIR "tests/fixtures/g1_anchors"
#define C5U_MANIFEST C5U_FIXTURE_DIR "/manifest.tsv"

enum {
    C5U_MAX_FIXTURES = 64,
    C5U_MOVED_EXPECTED = 8,
    C5U_EDITED_EXPECTED = 8,
    /* The end-to-end move, in lines. The unit says 200; this pads past it so
     * "at least 200" is a measurement with room in it, not a coincidence. */
    C5U_E2E_PAD_FUNCS = 60,
};

/* ── The manifest ────────────────────────────────────────────────────────────
 *
 * One row per fixture, tab separated, generated beside the spans. Parsed here
 * rather than compiled in because 126KB of real source belongs in a data file,
 * and because a fixture nobody can read is a fixture nobody can check. */

typedef struct {
    char id[32];
    char category[32];
    char symbol[128];
    char before_path[256];
    int before_start;
    int before_end;
    char before_hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    char after_path[256];
    int after_start;
    int after_end;
    char after_hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    char qn[HYP_ADDR_QN_MAX + 1];
    char expect_status[40];
    char expect_relation[40];
    int expect_candidates;
    char commit_before[24];
    char commit_after[24];
} c5u_fix_t;

/* Read a whole file. Caller frees. NULL on any failure — a fixture that cannot
 * be read is a failed test, never a skipped one. */
static char *c5u_slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    (void)fclose(f);
    buf[got] = '\0';
    if (out_len) {
        *out_len = got;
    }
    return buf;
}

/* Copy field `n` of a tab-separated row into `out`. Returns false when the row
 * has no such field — a short row is a corrupt manifest, not a default. */
static bool c5u_field(const char *row, int n, char *out, size_t out_sz) {
    const char *p = row;
    for (int i = 0; i < n; i++) {
        const char *tab = strchr(p, '\t');
        if (!tab) {
            return false;
        }
        p = tab + 1;
    }
    const char *tab = strchr(p, '\t');
    size_t len = tab ? (size_t)(tab - p) : strlen(p);
    if (len + 1 > out_sz) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool c5u_field_int(const char *row, int n, int *out) {
    char buf[32];
    if (!c5u_field(row, n, buf, sizeof(buf))) {
        return false;
    }
    *out = atoi(buf);
    return true;
}

/* Load the manifest. Fails closed: a missing file, a short row or an
 * unparseable number all return false rather than a partial corpus, because a
 * replay over four of eight fixtures passes while proving a quarter of the
 * claim. */
static bool c5u_load(c5u_fix_t *out, int max, int *out_count) {
    *out_count = 0;
    char *text = c5u_slurp(C5U_MANIFEST, NULL);
    if (!text) {
        return false;
    }
    bool ok = true;
    /* Split on '\n' in place. strtok_r is not portable to every venue this
     * suite builds in, and a row splitter is four lines. */
    char *row = text;
    bool first = true;
    while (row && *row && ok) {
        char *nl = strchr(row, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *next = nl ? nl + 1 : NULL;
        if (first) { /* the header names the columns; it is not a fixture */
            first = false;
            row = next;
            continue;
        }
        if (*out_count >= max) {
            ok = false;
            break;
        }
        c5u_fix_t *f = &out[*out_count];
        memset(f, 0, sizeof(*f));
        ok = c5u_field(row, 0, f->id, sizeof(f->id)) &&
             c5u_field(row, 1, f->category, sizeof(f->category)) &&
             c5u_field(row, 2, f->symbol, sizeof(f->symbol)) &&
             c5u_field(row, 3, f->before_path, sizeof(f->before_path)) &&
             c5u_field_int(row, 4, &f->before_start) && c5u_field_int(row, 5, &f->before_end) &&
             c5u_field(row, 6, f->before_hash, sizeof(f->before_hash)) &&
             c5u_field(row, 7, f->after_path, sizeof(f->after_path)) &&
             c5u_field_int(row, 8, &f->after_start) && c5u_field_int(row, 9, &f->after_end) &&
             c5u_field(row, 10, f->after_hash, sizeof(f->after_hash)) &&
             c5u_field(row, 11, f->qn, sizeof(f->qn)) &&
             c5u_field(row, 12, f->expect_status, sizeof(f->expect_status)) &&
             c5u_field(row, 13, f->expect_relation, sizeof(f->expect_relation)) &&
             c5u_field_int(row, 14, &f->expect_candidates) &&
             c5u_field(row, 15, f->commit_before, sizeof(f->commit_before)) &&
             c5u_field(row, 16, f->commit_after, sizeof(f->commit_after));
        if (ok) {
            (*out_count)++;
        }
        row = next;
    }
    free(text);
    return ok && *out_count > 0;
}

/* The exact span bytes for one side of one fixture. Caller frees. */
static char *c5u_span(const c5u_fix_t *f, const char *side) {
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s.%s.span", C5U_FIXTURE_DIR, f->id, side) < 0) {
        return NULL;
    }
    return c5u_slurp(path, NULL);
}

/* The corpus's status vocabulary, mapped to the contract's enum. Fails closed:
 * a word this file does not know is refused, so a corpus that grows a new
 * state cannot be silently replayed as one of the old ones. */
static bool c5u_status_of(const char *name, hyp_anchor_status_t *out) {
    static const struct {
        const char *name;
        hyp_anchor_status_t status;
    } table[] = {
        {"HYP_ANCHOR_RESOLVED", HYP_ANCHOR_RESOLVED},
        {"HYP_ANCHOR_RESOLVED_EDITED", HYP_ANCHOR_RESOLVED_EDITED},
        {"HYP_ANCHOR_ORPHANED", HYP_ANCHOR_ORPHANED},
        {"HYP_ANCHOR_UNKNOWN_WORKSPACE", HYP_ANCHOR_UNKNOWN_WORKSPACE},
        {"HYP_ANCHOR_ERROR", HYP_ANCHOR_ERROR},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *out = table[i].status;
            return true;
        }
    }
    return false;
}

static bool c5u_relation_of(const char *name, hyp_addr_rel_t *out) {
    static const struct {
        const char *name;
        hyp_addr_rel_t rel;
    } table[] = {
        {"HYP_ADDR_REL_SAME", HYP_ADDR_REL_SAME},
        {"HYP_ADDR_REL_EDITED", HYP_ADDR_REL_EDITED},
        {"HYP_ADDR_REL_CONTENT_ONLY", HYP_ADDR_REL_CONTENT_ONLY},
        {"HYP_ADDR_REL_UNRELATED", HYP_ADDR_REL_UNRELATED},
        {"HYP_ADDR_REL_INVALID", HYP_ADDR_REL_INVALID},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0) {
            *out = table[i].rel;
            return true;
        }
    }
    return false;
}

/* ── Materialising one side of a fixture ─────────────────────────────────────
 *
 * The file is written with the span's real bytes AT THE LINE THE FIXTURE
 * NAMES, preceded by that many blank lines. The padding is POSITION, not
 * evidence: the span hash covers exactly start_line..end_line, so bytes
 * outside it cannot enter any hash this test compares — which is what makes
 * the +996-line shift reproducible without carrying an 11,000-line file. */
static bool c5u_write_positioned(const char *dir, const char *rel_path, int start_line,
                                 const char *span) {
    char *padded = malloc((size_t)start_line + strlen(span) + 2);
    if (!padded) {
        return false;
    }
    size_t n = 0;
    for (int i = 1; i < start_line; i++) {
        padded[n++] = '\n';
    }
    memcpy(padded + n, span, strlen(span));
    n += strlen(span);
    padded[n++] = '\n';
    padded[n] = '\0';
    int rc = th_write_file(TH_PATH(dir, rel_path), padded);
    free(padded);
    return rc == 0;
}

/* A fresh in-memory index holding one project rooted at `dir` and one Function
 * node at the given lines — what a re-index of that tree would leave behind.
 * Fresh, never updated in place: a store that still remembers the old row
 * cannot distinguish "the anchor survived" from "the old row survived". */
static hyp_store_t *c5u_index_of(const char *dir, const char *rel_path, const char *symbol,
                                 const char *qn, int start_line, int end_line) {
    hyp_store_t *s = hyp_store_open_memory();
    if (!s) {
        return NULL;
    }
    if (hyp_store_upsert_project(s, C5U_PROJECT, dir) != HYP_STORE_OK) {
        hyp_store_close(s);
        return NULL;
    }
    hyp_node_t n = {.project = C5U_PROJECT,
                    .label = "Function",
                    .name = symbol,
                    .qualified_name = qn,
                    .file_path = rel_path,
                    .start_line = start_line,
                    .end_line = end_line,
                    .properties_json = NULL};
    if (hyp_store_upsert_node(s, &n) <= 0) {
        hyp_store_close(s);
        return NULL;
    }
    return s;
}

/* The hash a WRITER would record: the product reads the span through its own
 * reader and hashes it through its own hash. No fixture value is used to
 * produce it — only to check it. */
static bool c5u_product_hash(const char *dir, const char *rel_path, int start_line, int end_line,
                             char *out) {
    out[0] = '\0';
    char abs[HYP_PATH_MAX];
    if (snprintf(abs, sizeof(abs), "%s/%s", dir, rel_path) < 0) {
        return false;
    }
    char *text = hyp_ask_read_span_lines(abs, start_line, end_line, NULL);
    if (!text) {
        return false;
    }
    hyp_addr_span_hash(text, out);
    free(text);
    return true;
}

static bool c5u_anchor_of(const char *qn, const char *hash, char *out, size_t out_sz) {
    hyp_addr_t a;
    if (hyp_addr_init(&a, NULL, C5U_PROJECT, qn) != HYP_ADDR_OK) {
        return false;
    }
    return hyp_anchor_format(&a, hash, out, out_sz);
}

/*
 * Replay one fixture end to end and report what came back.
 *
 * The shape is the same for every category, which is the point — nothing here
 * branches on what the answer is supposed to be:
 *
 *   1. write the tree as it stood BEFORE, index it, write an anchor;
 *   2. check the anchor was true when written (RESOLVED against that index);
 *   3. write the tree as it stands AFTER, index it FRESH — the re-index;
 *   4. resolve the same anchor against the new index.
 *
 * `note` receives a description of the first step that failed mechanically
 * (a missing span, an unwritable tree); the caller turns that into a FAIL
 * naming the fixture. Returns false only for those; a fixture that resolves
 * to the WRONG answer returns true and is judged by the caller.
 */
typedef struct {
    hyp_anchor_status_t status;
    hyp_addr_rel_t relation;
    int candidate_count;
    int scan_skipped;
    int start_line;
    int end_line;
    char file_path[HYP_PATH_MAX];
    char current_hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    char derived_qn[HYP_ADDR_QN_MAX + 1];
    char written_hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_anchor_status_t status_before;
} c5u_replay_t;

static bool c5u_replay(const c5u_fix_t *f, c5u_replay_t *out, char *note, size_t note_sz) {
    memset(out, 0, sizeof(*out));
    char *before = c5u_span(f, "before");
    char *after = c5u_span(f, "after");
    char *dir = th_mktempdir("hyp-c5u");
    bool ok = false;
    hyp_store_t *s_before = NULL;
    hyp_store_t *s_after = NULL;
    char *qn = NULL;
    /* Declared before the first early exit: a jump past an initialisation is
     * legal C but noisy under -Wjump-misses-init, and this file is not the
     * place to argue about it. */
    char anchor[HYP_ANCHOR_MAX + 1] = {0};
    hyp_anchor_res_t r0 = {0};
    hyp_anchor_res_t r = {0};

    if (!before || !after) {
        snprintf(note, note_sz, "%s: span file missing under %s", f->id, C5U_FIXTURE_DIR);
        goto done;
    }
    if (!dir) {
        snprintf(note, note_sz, "%s: no temp dir", f->id);
        goto done;
    }
    /* The qualified name comes from the REAL builder, never the manifest — the
     * manifest's copy is then compared against it by the caller, which is the
     * two-ends check between the corpus's naming scheme and the product's. */
    qn = hyp_pipeline_fqn_compute(C5U_PROJECT, f->before_path, f->symbol);
    if (!qn) {
        snprintf(note, note_sz, "%s: fqn_compute refused %s", f->id, f->before_path);
        goto done;
    }
    snprintf(out->derived_qn, sizeof(out->derived_qn), "%s", qn);

    if (!c5u_write_positioned(dir, f->before_path, f->before_start, before)) {
        snprintf(note, note_sz, "%s: cannot write the before tree", f->id);
        goto done;
    }
    if (!c5u_product_hash(dir, f->before_path, f->before_start, f->before_end, out->written_hash)) {
        snprintf(note, note_sz, "%s: cannot read the before span", f->id);
        goto done;
    }
    if (!c5u_anchor_of(qn, out->written_hash, anchor, sizeof(anchor))) {
        snprintf(note, note_sz, "%s: cannot form an anchor", f->id);
        goto done;
    }

    s_before = c5u_index_of(dir, f->before_path, f->symbol, qn, f->before_start, f->before_end);
    if (!s_before) {
        snprintf(note, note_sz, "%s: cannot index the before tree", f->id);
        goto done;
    }
    out->status_before = hyp_anchor_resolve(s_before, NULL, anchor, &r0);
    hyp_anchor_res_free(&r0);

    /* The re-index. The file is rewritten as it stands AFTER — for a move that
     * is the same bytes at a different line, for an edit different bytes — and
     * the index is built again from nothing. */
    if (!c5u_write_positioned(dir, f->after_path, f->after_start, after)) {
        snprintf(note, note_sz, "%s: cannot write the after tree", f->id);
        goto done;
    }
    s_after = c5u_index_of(dir, f->after_path, f->symbol, qn, f->after_start, f->after_end);
    if (!s_after) {
        snprintf(note, note_sz, "%s: cannot index the after tree", f->id);
        goto done;
    }

    out->status = hyp_anchor_resolve(s_after, NULL, anchor, &r);
    out->relation = r.relation;
    out->candidate_count = r.candidate_count;
    out->scan_skipped = r.scan_skipped;
    out->start_line = r.start_line;
    out->end_line = r.end_line;
    snprintf(out->file_path, sizeof(out->file_path), "%s", r.file_path);
    snprintf(out->current_hash, sizeof(out->current_hash), "%s", r.current_hash);
    hyp_anchor_res_free(&r);
    ok = true;

done:
    hyp_store_close(s_before);
    hyp_store_close(s_after);
    free(qn);
    free(before);
    free(after);
    th_cleanup(dir); /* th_mktempdir hands back a static buffer — never freed */
    return ok;
}

/* ════════════════════════════════════════════════════════════════════
 * C5u's assertion row, replayed over history
 * ════════════════════════════════════════════════════════════════════ */

TEST(c5u_history_moved_within_file_stays_resolved) {
    /* THE UNIT. Eight symbols that really moved, by +275 to +996 lines, in
     * commits that were made for unrelated reasons. Same qualified name, same
     * bytes, a different place in the file — and the classification does not
     * move, while the ANSWER does. The second half is what proves lines are
     * output: a test that only checked the status would pass against an
     * implementation that reported the old position. */
    c5u_fix_t fx[C5U_MAX_FIXTURES];
    int n = 0;
    if (!c5u_load(fx, C5U_MAX_FIXTURES, &n)) {
        FAIL("cannot read " C5U_MANIFEST);
    }
    char note[512];
    int seen = 0;
    int largest_shift = 0;
    for (int i = 0; i < n; i++) {
        const c5u_fix_t *f = &fx[i];
        if (strcmp(f->category, "moved_within_file") != 0) {
            continue;
        }
        seen++;
        hyp_anchor_status_t want_status;
        hyp_addr_rel_t want_rel;
        if (!c5u_status_of(f->expect_status, &want_status) ||
            !c5u_relation_of(f->expect_relation, &want_rel)) {
            snprintf(note, sizeof(note), "%s: unknown expectation vocabulary", f->id);
            FAIL(note);
        }
        c5u_replay_t got;
        if (!c5u_replay(f, &got, note, sizeof(note))) {
            FAIL(note);
        }

        /* Two ends: the corpus computed this name and the product computes it
         * now. A11 repaired exactly this surface; a divergence here is an
         * address collision, not a fixture problem. */
        ASSERT_STR_EQ(got.derived_qn, f->qn);
        /* Two ends again, on the hash: the corpus brace-matched the span and
         * SHA-256'd it, the product read the pinned range and hashed it. */
        ASSERT_STR_EQ(got.written_hash, f->before_hash);
        /* The anchor was true when it was written. */
        ASSERT_EQ(got.status_before, HYP_ANCHOR_RESOLVED);

        /* The move is real, and this is the fixture asserting itself: a
         * "moved" case that did not move would make the whole row vacuous. */
        ASSERT_NEQ(f->after_start, f->before_start);
        ASSERT_STR_EQ(f->after_hash, f->before_hash);

        /* STILL ATTACHED, and not merely not-orphaned. */
        ASSERT_EQ(got.status, want_status);
        ASSERT_EQ(got.status, HYP_ANCHOR_RESOLVED);
        ASSERT_EQ(got.relation, want_rel);
        ASSERT_EQ(got.candidate_count, f->expect_candidates);
        ASSERT_EQ(got.candidate_count, 0);
        ASSERT_EQ(got.scan_skipped, 0);

        /* Where it is NOW — the line numbers in the answer are the new ones. */
        ASSERT_EQ(got.start_line, f->after_start);
        ASSERT_EQ(got.end_line, f->after_end);
        ASSERT_STR_EQ(got.file_path, f->after_path);
        ASSERT_STR_EQ(got.current_hash, f->after_hash);

        int shift = f->after_start - f->before_start;
        if (shift > largest_shift) {
            largest_shift = shift;
        }
    }
    /* The corpus is pinned, exactly as A10 pins its six: a manifest that lost
     * a fixture would otherwise pass this test by having less to prove. */
    ASSERT_EQ(seen, C5U_MOVED_EXPECTED);
    /* And the unit's own number: 200 lines is the claim, and history offers
     * far more than 200. */
    ASSERT_GTE(largest_shift, 200);
    PASS();
}

TEST(c5u_history_body_changed_stays_attached_as_edited) {
    /* The other half of "still attached". Eight symbols whose bodies really
     * changed: the qualified name is the identity, so they stay attached — as
     * RESOLVED_EDITED, which says the reasoning may be stale, a different and
     * much smaller problem than being lost. Collapsing this into RESOLVED
     * would hide a real staleness; collapsing it into ORPHANED would lose the
     * decision outright. */
    c5u_fix_t fx[C5U_MAX_FIXTURES];
    int n = 0;
    if (!c5u_load(fx, C5U_MAX_FIXTURES, &n)) {
        FAIL("cannot read " C5U_MANIFEST);
    }
    char note[512];
    int seen = 0;
    for (int i = 0; i < n; i++) {
        const c5u_fix_t *f = &fx[i];
        if (strcmp(f->category, "body_changed") != 0) {
            continue;
        }
        seen++;
        hyp_anchor_status_t want_status;
        hyp_addr_rel_t want_rel;
        if (!c5u_status_of(f->expect_status, &want_status) ||
            !c5u_relation_of(f->expect_relation, &want_rel)) {
            snprintf(note, sizeof(note), "%s: unknown expectation vocabulary", f->id);
            FAIL(note);
        }
        c5u_replay_t got;
        if (!c5u_replay(f, &got, note, sizeof(note))) {
            FAIL(note);
        }

        ASSERT_STR_EQ(got.derived_qn, f->qn);
        ASSERT_STR_EQ(got.written_hash, f->before_hash);
        ASSERT_EQ(got.status_before, HYP_ANCHOR_RESOLVED);
        /* The edit is real. */
        ASSERT_STR_NEQ(f->after_hash, f->before_hash);

        ASSERT_EQ(got.status, want_status);
        ASSERT_EQ(got.status, HYP_ANCHOR_RESOLVED_EDITED);
        ASSERT_EQ(got.relation, want_rel);
        ASSERT_EQ(got.relation, HYP_ADDR_REL_EDITED);
        /* Attached means the answer says WHERE, not merely that it is not an
         * orphan. An edited anchor that reported no location would be
         * useless to a reader and would still pass a status-only assertion. */
        ASSERT_STR_EQ(got.file_path, f->after_path);
        ASSERT_EQ(got.start_line, f->after_start);
        ASSERT_EQ(got.end_line, f->after_end);
        ASSERT_STR_EQ(got.current_hash, f->after_hash);
        /* An edited symbol is FOUND, so nothing was scanned for. */
        ASSERT_EQ(got.candidate_count, f->expect_candidates);
        ASSERT_EQ(got.candidate_count, 0);
    }
    ASSERT_EQ(seen, C5U_EDITED_EXPECTED);
    PASS();
}

/* Find one fixture by id, or NULL. */
static const c5u_fix_t *c5u_by_id(const c5u_fix_t *fx, int n, const char *id) {
    for (int i = 0; i < n; i++) {
        if (strcmp(fx[i].id, id) == 0) {
            return &fx[i];
        }
    }
    return NULL;
}

TEST(c5u_largest_history_shift_996_lines_survives) {
    /* The biggest move the corpus found, named so the number cannot quietly
     * shrink: cbm_run_c_lsp_cross slid 996 lines down internal/cbm/lsp/c_lsp.c
     * and stayed attached with byte-identical content. */
    c5u_fix_t fx[C5U_MAX_FIXTURES];
    int n = 0;
    if (!c5u_load(fx, C5U_MAX_FIXTURES, &n)) {
        FAIL("cannot read " C5U_MANIFEST);
    }
    const c5u_fix_t *f = c5u_by_id(fx, n, "G1-MOVE-007");
    if (!f) {
        FAIL("G1-MOVE-007 absent from the manifest");
    }
    ASSERT_EQ(f->after_start - f->before_start, 996);
    char note[512];
    c5u_replay_t got;
    if (!c5u_replay(f, &got, note, sizeof(note))) {
        FAIL(note);
    }
    ASSERT_EQ(got.status, HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(got.relation, HYP_ADDR_REL_SAME);
    ASSERT_EQ(got.candidate_count, 0);
    ASSERT_EQ(got.start_line, f->after_start);
    ASSERT_EQ(got.end_line, f->after_end);
    PASS();
}

TEST(c5u_one_line_slide_with_edit_is_edited_not_resolved) {
    /* The sharpest discriminator history offers. handle_ask slid EXACTLY ONE
     * line — 4069 to 4070 — while its body changed. Every line-number-shaped
     * shortcut gets this wrong in the same direction: "it barely moved, so it
     * is the same" reports RESOLVED and tells a reader the reasoning still
     * describes the code. The content hash is the only thing that can tell,
     * and it says EDITED. */
    c5u_fix_t fx[C5U_MAX_FIXTURES];
    int n = 0;
    if (!c5u_load(fx, C5U_MAX_FIXTURES, &n)) {
        FAIL("cannot read " C5U_MANIFEST);
    }
    const c5u_fix_t *f = c5u_by_id(fx, n, "G1-EDIT-008");
    if (!f) {
        FAIL("G1-EDIT-008 absent from the manifest");
    }
    ASSERT_EQ(f->after_start - f->before_start, 1);
    ASSERT_STR_NEQ(f->after_hash, f->before_hash);
    char note[512];
    c5u_replay_t got;
    if (!c5u_replay(f, &got, note, sizeof(note))) {
        FAIL(note);
    }
    ASSERT_EQ(got.status, HYP_ANCHOR_RESOLVED_EDITED);
    ASSERT_NEQ(got.status, HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(got.relation, HYP_ADDR_REL_EDITED);
    ASSERT_EQ(got.start_line, f->after_start);
    PASS();
}

TEST(c5u_zero_line_slide_with_edit_is_edited_not_resolved) {
    /* The same discriminator from the other side, and the one a line-number
     * implementation fails most loudly: three symbols whose declaration line
     * did not move AT ALL while their bodies changed. "Same line, therefore
     * same code" reports RESOLVED. Only the content hash says otherwise. */
    c5u_fix_t fx[C5U_MAX_FIXTURES];
    int n = 0;
    if (!c5u_load(fx, C5U_MAX_FIXTURES, &n)) {
        FAIL("cannot read " C5U_MANIFEST);
    }
    char note[512];
    int seen = 0;
    for (int i = 0; i < n; i++) {
        const c5u_fix_t *f = &fx[i];
        if (strcmp(f->category, "body_changed") != 0 || f->after_start != f->before_start) {
            continue;
        }
        seen++;
        ASSERT_STR_NEQ(f->after_hash, f->before_hash);
        c5u_replay_t got;
        if (!c5u_replay(f, &got, note, sizeof(note))) {
            FAIL(note);
        }
        ASSERT_EQ(got.start_line, f->before_start); /* it did not move */
        ASSERT_EQ(got.status, HYP_ANCHOR_RESOLVED_EDITED);
        ASSERT_NEQ(got.status, HYP_ANCHOR_RESOLVED);
        ASSERT_EQ(got.relation, HYP_ADDR_REL_EDITED);
    }
    ASSERT_GTE(seen, 3);
    PASS();
}

/* ════════════════════════════════════════════════════════════════════
 * End to end — write a decision, re-index, read it back
 * ════════════════════════════════════════════════════════════════════ */

/* A source file with `foo` in it, optionally preceded by `pad` real functions.
 * Real functions, not comments: the mover has to be something tree-sitter
 * parses, or the re-index is not doing the work the test claims it does. */
static char *c5u_e2e_source(int pad) {
    size_t cap = (size_t)pad * 96 + 512;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    int n = snprintf(buf, cap, "#include <stddef.h>\n\n");
    for (int i = 0; i < pad; i++) {
        n += snprintf(buf + n, cap - (size_t)n,
                      "int pad_%d(int x) {\n"
                      "    return x + %d;\n"
                      "}\n\n",
                      i, i);
    }
    snprintf(buf + n, cap - (size_t)n,
             "int foo(int a, int b) {\n"
             "    int total = a + b;\n"
             "    return total * 2;\n"
             "}\n");
    return buf;
}

/* Index `repo` into `db` with the real pipeline; copy out the project name. */
static bool c5u_index_tree(const char *repo, const char *db, char *out_project, size_t sz) {
    hyp_pipeline_t *p = hyp_pipeline_new(repo, db, HYP_MODE_FULL);
    if (!p) {
        return false;
    }
    bool ok = hyp_pipeline_run(p) == 0;
    if (ok && out_project) {
        const char *name = hyp_pipeline_project_name(p);
        snprintf(out_project, sz, "%s", name ? name : "");
    }
    hyp_pipeline_free(p);
    return ok;
}

TEST(c5u_end_to_end_decision_survives_a_real_reindex) {
    /*
     * THE FIRST REAL USE, which is stronger evidence than the replay above
     * because nothing in it is arranged. Every stage is the production one:
     *
     *   the real pipeline indexes a real tree (tree-sitter finds `foo` and
     *   decides its own span — no fixture range is pinned anywhere here);
     *   the real record store persists a kind=decision record whose anchor is
     *   built from what the indexer said;
     *   sixty new functions are written ABOVE `foo` and the real pipeline
     *   RE-INDEXES;
     *   the anchor resolves, and the decision is read back BY THAT ANCHOR.
     *
     * The last step is the promise a user experiences. A test that stopped at
     * hyp_anchor_resolve would prove the anchor agrees with itself and nothing
     * about whether the store can still find the decision.
     */
    char *base = th_mktempdir("hyp-c5u-e2e");
    ASSERT_NOT_NULL(base);
    char repo[512];
    char db[512];
    char recdir[512];
    snprintf(repo, sizeof(repo), "%s/repo", base);
    snprintf(db, sizeof(db), "%s/index.db", base);
    snprintf(recdir, sizeof(recdir), "%s/memory", base);

    /* ── 1. A tree with `foo` near the top, indexed for real ──────────── */
    char *src = c5u_e2e_source(0);
    ASSERT_NOT_NULL(src);
    ASSERT_EQ(th_write_file(TH_PATH(repo, "src/thing.c"), src), 0);
    free(src);

    char project[256] = "";
    ASSERT_TRUE(c5u_index_tree(repo, db, project, sizeof(project)));
    ASSERT_TRUE(project[0] != '\0');

    char *qn = hyp_pipeline_fqn_compute(project, "src/thing.c", "foo");
    ASSERT_NOT_NULL(qn);

    hyp_store_t *s1 = hyp_store_open_path(db);
    ASSERT_NOT_NULL(s1);
    hyp_node_t node = {0};
    ASSERT_EQ(hyp_store_find_node_by_qn(s1, project, qn, &node), HYP_STORE_OK);
    int line_before = node.start_line;
    int end_before = node.end_line;
    hyp_node_free_fields(&node);
    hyp_store_close(s1);
    ASSERT_GT(line_before, 0);

    /* The anchor a writer would record: the hash recomputed from the
     * product's OWN span, per the corpus caveat's second remedy. */
    char hash_before[HYP_ADDR_SPAN_HASH_LEN + 1];
    ASSERT_TRUE(c5u_product_hash(repo, "src/thing.c", line_before, end_before, hash_before));
    hyp_addr_t addr;
    ASSERT_EQ(hyp_addr_init(&addr, NULL, project, qn), HYP_ADDR_OK);
    char anchor[HYP_ANCHOR_MAX + 1];
    ASSERT_TRUE(hyp_anchor_format(&addr, hash_before, anchor, sizeof(anchor)));

    /* ── 2. The decision, through the real store ──────────────────────── */
    static const char *const decision =
        "foo doubles the sum rather than returning it, because every caller "
        "was already doubling it and two of them disagreed about when.";
    hyp_record_store_t *rs = NULL;
    ASSERT_EQ(hyp_record_store_open(recdir, &rs), HYP_RECORD_STORE_OK);
    hyp_record_input_t in = {.kind = HYP_RECORD_DECISION,
                             .author = "c5u-test",
                             .timestamp_ms = 1755000000000LL,
                             .content = decision,
                             .anchor = anchor,
                             .origin = NULL,
                             .thread = NULL,
                             .parent = NULL,
                             .redactions = 0};
    const hyp_record_t *rec = NULL;
    ASSERT_EQ(hyp_record_build(&in, &rec), HYP_RECORD_OK);
    ASSERT_NOT_NULL(rec);
    char rec_id[HYP_RECORD_ID_LEN + 1];
    snprintf(rec_id, sizeof(rec_id), "%s", rec->id);
    bool added = false;
    ASSERT_EQ(hyp_record_store_append(rs, rec, &added), HYP_RECORD_STORE_OK);
    ASSERT_TRUE(added);
    hyp_record_free(rec);

    /* ── 3. `foo` moves, and the tree is RE-INDEXED for real ──────────── */
    src = c5u_e2e_source(C5U_E2E_PAD_FUNCS);
    ASSERT_NOT_NULL(src);
    ASSERT_EQ(th_write_file(TH_PATH(repo, "src/thing.c"), src), 0);
    free(src);
    char project2[256] = "";
    ASSERT_TRUE(c5u_index_tree(repo, db, project2, sizeof(project2)));
    /* The address is stable across a re-index or nothing below means
     * anything — a changed project slug would be a new address space. */
    ASSERT_STR_EQ(project2, project);

    /* ── 4. The anchor resolves, at the new place ─────────────────────── */
    hyp_store_t *s2 = hyp_store_open_path(db);
    ASSERT_NOT_NULL(s2);
    hyp_anchor_res_t res;
    ASSERT_EQ(hyp_anchor_resolve(s2, NULL, anchor, &res), HYP_ANCHOR_RESOLVED);
    ASSERT_EQ(res.relation, HYP_ADDR_REL_SAME);
    ASSERT_EQ(res.candidate_count, 0);
    ASSERT_STR_EQ(res.file_path, "src/thing.c");
    ASSERT_STR_EQ(res.current_hash, hash_before);
    /* The unit's own number, MEASURED rather than assumed: the pad is sixty
     * four-line functions, so `foo` is at least 200 lines further down than
     * where the decision was written against it. */
    ASSERT_GTE(res.start_line - line_before, 200);
    int line_after = res.start_line;
    hyp_anchor_res_free(&res);

    /* ── 5. The decision is read back BY THE ANCHOR ───────────────────── */
    hyp_record_store_query_t q = {0};
    q.anchor = anchor;
    hyp_record_set_t *found = NULL;
    ASSERT_EQ(hyp_record_store_query(rs, &q, &found), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(found);
    ASSERT_EQ((long long)hyp_record_set_count(found), 1);
    const hyp_record_t *back = hyp_record_set_at(found, 0);
    ASSERT_NOT_NULL(back);
    ASSERT_STR_EQ(back->id, rec_id);
    ASSERT_STR_EQ(back->content, decision);
    ASSERT_STR_EQ(back->anchor, anchor);
    hyp_record_set_free(found);

    /* ── 6. And the reader's own path, which is the one a user takes ───
     *
     * A reader does not know the anchor string; it sweeps the anchored
     * records and resolves each one against the live index. This is the
     * assertion that binds all three modules together: the decision written
     * before the move is delivered, attached, at the line the symbol occupies
     * NOW. Assert the reader's view, not the emitted shape. */
    hyp_record_store_query_t sweep = {0};
    sweep.anchored_only = true;
    hyp_record_set_t *all = NULL;
    ASSERT_EQ(hyp_record_store_query(rs, &sweep, &all), HYP_RECORD_STORE_OK);
    ASSERT_NOT_NULL(all);
    int delivered = 0;
    for (size_t i = 0; i < hyp_record_set_count(all); i++) {
        const hyp_record_t *r = hyp_record_set_at(all, i);
        if (!r || !r->anchor) {
            continue;
        }
        hyp_anchor_res_t rr;
        if (hyp_anchor_resolve(s2, NULL, r->anchor, &rr) == HYP_ANCHOR_RESOLVED &&
            rr.start_line == line_after && strcmp(r->content, decision) == 0) {
            delivered++;
        }
        hyp_anchor_res_free(&rr);
    }
    hyp_record_set_free(all);
    ASSERT_EQ(delivered, 1);

    /* ── 7. Nothing about the move touched the store ──────────────────
     *
     * Orphan-ness and moved-ness are properties of (record, index state),
     * derived at read time. A re-index that had written to the record store
     * would have made them properties of the record, which C2 forbids. */
    size_t count = 0;
    ASSERT_EQ(hyp_record_store_count(rs, &count), HYP_RECORD_STORE_OK);
    ASSERT_EQ((long long)count, 1);

    hyp_store_close(s2);
    hyp_record_store_close(rs);
    free(qn);
    th_cleanup(base); /* static buffer from th_mktempdir — never freed */
    PASS();
}

SUITE(anchor_reindex) {
    RUN_TEST(c5u_history_moved_within_file_stays_resolved);
    RUN_TEST(c5u_history_body_changed_stays_attached_as_edited);
    RUN_TEST(c5u_largest_history_shift_996_lines_survives);
    RUN_TEST(c5u_one_line_slide_with_edit_is_edited_not_resolved);
    RUN_TEST(c5u_zero_line_slide_with_edit_is_edited_not_resolved);
    RUN_TEST(c5u_end_to_end_decision_survives_a_real_reindex);
}
