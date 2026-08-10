/*
 * ask_embed.c — the opt-in embed pass.
 *
 * Reads the graph read-only, reads the source off disk, groups by the linear
 * padded-slot rule, encodes, and writes vectors to a database of its own.
 */

#include "ask/ask_embed.h"
#include "ask/ask_batch.h"
#include "ask/ask_doctext.h"
#include "ask/ask_vectors.h"
#include "discover/discover.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/sha256.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    AE_PATHBUF = 1024,
    AE_NUMBUF = 32,
};

/* One pending document: everything the encode step and the store need. */
typedef struct {
    char *qualified_name;
    char *label;
    char *file_path;
    char *lang;
    int64_t node_id;
    int start_line;
    int end_line;
    char *text; /* owned; freed as soon as the batch is encoded */
    char hash[HYP_ASK_VEC_HASH_LEN + 1];
    int token_len;  /* padded slots this document occupies */
    int full_len;   /* untruncated token length, or < 0 for "cannot say" */
    bool truncated; /* full_len > window */
} ae_doc_t;

static char *ae_strdup(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static void ae_doc_free(ae_doc_t *d) {
    free(d->qualified_name);
    free(d->label);
    free(d->file_path);
    free(d->lang);
    free(d->text);
    memset(d, 0, sizeof(*d));
}

static double ae_elapsed_ms(struct timespec start) {
    struct timespec now;
    hyp_clock_gettime(CLOCK_MONOTONIC, &now);
    return ((double)(now.tv_sec - start.tv_sec) * 1000.0) +
           ((double)(now.tv_nsec - start.tv_nsec) / 1000000.0);
}

static const char *ae_itoa(int64_t v, char *buf, size_t sz) {
    snprintf(buf, sz, "%lld", (long long)v);
    return buf;
}

void hyp_ask_embed_report_free(hyp_ask_embed_report_t *r) {
    if (!r) {
        return;
    }
    free(r->device_note);
    r->device_note = NULL;
}

void hyp_ask_content_hash(const char *text, char *out) {
    char hex[HYP_SHA256_HEX_LEN + 1];
    hyp_sha256_hex(text ? text : "", text ? strlen(text) : 0, hex);
    memcpy(out, hex, HYP_ASK_VEC_HASH_LEN);
    out[HYP_ASK_VEC_HASH_LEN] = '\0';
}

/* ── Reading the span ──────────────────────────────────────────── */

/* The whole file, NUL-terminated. `*out_len` is the byte count. */
static char *ae_slurp(const char *abs_path, size_t *out_len) {
    FILE *fp = hyp_fopen(abs_path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        (void)fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        (void)fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        (void)fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, fp);
    (void)fclose(fp);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static char *ae_slice(const char *buf, size_t got, int start, int end) {
    /* Walk to the first byte of `start`. Lines are separated by '\n' — which is
     * how tree-sitter counted them when it produced start_line/end_line, so it
     * is the only split that keeps the slice and the line numbers in step. */
    size_t i = 0;
    int line = 1;
    while (line < start && i < got) {
        if (buf[i] == '\n') {
            line++;
        }
        i++;
    }
    if (line < start) {
        return NULL; /* the span starts past the end of the file */
    }
    size_t from = i;
    /* Walk to the byte after the last byte of `end`. */
    while (line <= end && i < got) {
        if (buf[i] == '\n') {
            line++;
            if (line > end) {
                break;
            }
        }
        i++;
    }
    size_t to = i; /* exclusive; sits on the '\n' that ends `end`, or at EOF */
    size_t span = to - from;
    char *out = malloc(span + 1);
    if (!out) {
        return NULL;
    }
    /* Copy, dropping a '\r' that immediately precedes a '\n'. Joining with
     * '\n' and emitting no trailing newline is what the reference implementation
     * produces, and the embedded text unit is part of the measurement. */
    size_t w = 0;
    for (size_t r = from; r < to; r++) {
        if (buf[r] == '\r' && r + 1 < to && buf[r + 1] == '\n') {
            continue;
        }
        out[w++] = buf[r];
    }
    /* A span that ends at EOF may still carry its own final '\r'. */
    if (w > 0 && out[w - 1] == '\r') {
        w--;
    }
    out[w] = '\0';
    return out;
}

char *hyp_ask_read_span(const char *abs_path, int start, int end) {
    if (!abs_path || start < 1 || end < start) {
        return NULL;
    }
    size_t got = 0;
    char *buf = ae_slurp(abs_path, &got);
    if (!buf) {
        return NULL;
    }
    char *out = ae_slice(buf, got, start, end);
    free(buf);
    return out;
}

char *hyp_ask_document_text(const char *abs_path, HYPLanguage lang, hyp_ask_compose_t compose,
                            const char *label, const char *qualified_name, const char *project,
                            const char *rel_path, int start, int end, int *comment_lines_out) {
    if (comment_lines_out) {
        *comment_lines_out = 0;
    }
    if (!abs_path || start < 1 || end < start) {
        return NULL;
    }
    size_t got = 0;
    char *buf = ae_slurp(abs_path, &got);
    if (!buf) {
        return NULL;
    }
    /* The comment is taken by EXTENDING THE SPAN UPWARD, not by splicing a
     * second string in. Everything below the header line is therefore still
     * one contiguous slice of the file, which is what makes this a bigger
     * span rather than a synthesised document. */
    int first = start;
    if (compose & HYP_ASK_COMPOSE_COMMENT) {
        first = hyp_ask_leading_comment_start(buf, lang, start);
        if (first < 1 || first > start) {
            first = start; /* defensive: the scan may only ever go up */
        }
    }
    char *src = ae_slice(buf, got, first, end);
    free(buf);
    if (!src) {
        return NULL;
    }
    if (comment_lines_out) {
        *comment_lines_out = start - first;
    }
    if (!(compose & HYP_ASK_COMPOSE_HEADER)) {
        return src;
    }
    char head[HYP_ASK_DOC_HEADER_MAX];
    size_t hn = hyp_ask_header_line(head, sizeof(head), lang, label, qualified_name, project,
                                    rel_path);
    if (hn == 0) {
        return src;
    }
    size_t sn = strlen(src);
    char *out = malloc(hn + 1 + sn + 1);
    if (!out) {
        free(src);
        return NULL;
    }
    memcpy(out, head, hn);
    out[hn] = '\n';
    memcpy(out + hn + 1, src, sn + 1);
    free(src);
    return out;
}

/* ── Graph access ──────────────────────────────────────────────── */

/* The graph database is opened READ-ONLY through a private connection rather
 * than through hyp_store_t. Two reasons and both are about blast radius: this
 * pass must be unable to write to the structural index under any failure, and
 * it must add no symbol to src/store so that "the structural path is unchanged"
 * stays checkable by reading the diff. Nothing user-supplied reaches SQL here —
 * the only bound parameter is the project name. */
typedef struct {
    sqlite3 *db;
    char *root_path;
    char *generation;
} ae_graph_t;

static void ae_graph_close(ae_graph_t *g) {
    if (!g) {
        return;
    }
    if (g->db) {
        sqlite3_close(g->db);
    }
    free(g->root_path);
    free(g->generation);
    memset(g, 0, sizeof(*g));
}

static bool ae_graph_open(ae_graph_t *g, const char *db_path, const char *project) {
    memset(g, 0, sizeof(*g));
    if (sqlite3_open_v2(db_path, &g->db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        hyp_log_error("ask.embed.graph_open", "path", db_path, "err",
                      g->db ? sqlite3_errmsg(g->db) : "open failed");
        ae_graph_close(g);
        return false;
    }
    (void)sqlite3_exec(g->db, "PRAGMA busy_timeout = 10000;", NULL, NULL, NULL);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g->db, "SELECT root_path FROM projects WHERE name = ?1", -1, &st,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, project, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            g->root_path = ae_strdup((const char *)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
    }
    /* The graph's own generation marker, recorded beside the vectors so the
     * lane can say it is looking at an older corpus rather than answering
     * confidently from one. Absent on databases predating it — "legacy" is the
     * store's own word for that and is carried through unchanged. */
    st = NULL;
    if (sqlite3_prepare_v2(g->db, "SELECT v FROM store_meta WHERE k = 'mutation_gen'", -1, &st,
                           NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            g->generation = ae_strdup((const char *)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
    }
    if (!g->generation) {
        g->generation = ae_strdup("legacy");
    }
    return true;
}

/* ── The pass ──────────────────────────────────────────────────── */

typedef struct {
    ae_doc_t *docs;
    int count;
    int cap;
    size_t bytes;
} ae_window_t;

static bool ae_window_push(ae_window_t *w, ae_doc_t doc) {
    if (w->count >= w->cap) {
        int nc = w->cap ? w->cap * 2 : 256;
        ae_doc_t *grown = realloc(w->docs, (size_t)nc * sizeof(*grown));
        if (!grown) {
            return false;
        }
        w->docs = grown;
        w->cap = nc;
    }
    w->bytes += doc.text ? strlen(doc.text) : 0;
    w->docs[w->count++] = doc;
    return true;
}

static void ae_window_clear(ae_window_t *w) {
    for (int i = 0; i < w->count; i++) {
        ae_doc_free(&w->docs[i]);
    }
    w->count = 0;
    w->bytes = 0;
}

/* Group the window, encode each group, write the rows. */
static int ae_flush_window(ae_window_t *w, const hyp_ask_encoder_t *enc, hyp_ask_vectors_t *store,
                           int dim, int budget, int max_docs, hyp_ask_embed_report_t *rep) {
    if (w->count == 0) {
        return 0;
    }
    int *lengths = malloc((size_t)w->count * sizeof(*lengths));
    if (!lengths) {
        return -1;
    }
    for (int i = 0; i < w->count; i++) {
        lengths[i] = w->docs[i].token_len > 0 ? w->docs[i].token_len : 1;
    }
    hyp_ask_batches_t batches;
    if (hyp_ask_group_by_token_budget(lengths, w->count, budget, max_docs, &batches) != 0) {
        free(lengths);
        return -1;
    }
    int64_t slots = 0;
    int64_t max_rect = 0;
    hyp_ask_batches_cost(&batches, lengths, &slots, &max_rect);
    rep->forward_passes += batches.group_count;
    rep->padded_slots += slots;
    if (max_rect > rep->max_rect) {
        rep->max_rect = max_rect;
    }

    int rc = 0;
    for (int g = 0; g < batches.group_count && rc == 0; g++) {
        int from = batches.group_start[g];
        int to = batches.group_start[g + 1];
        int n = to - from;
        const char **texts = malloc((size_t)n * sizeof(*texts));
        float *vecs = malloc((size_t)n * (size_t)dim * sizeof(*vecs));
        hyp_ask_vec_row_t *rows = calloc((size_t)n, sizeof(*rows));
        if (!texts || !vecs || !rows) {
            free(texts);
            free(vecs);
            free(rows);
            rc = -1;
            break;
        }
        for (int j = 0; j < n; j++) {
            texts[j] = w->docs[batches.order[from + j]].text;
        }
        if (hyp_ask_encode_documents(enc, texts, n, vecs) != 0) {
            hyp_log_error("ask.embed.encode_failed", "batch", "documents");
            rc = -1;
        } else {
            int bad = -1;
            if (!hyp_ask_vectors_are_unit(vecs, n, dim, &bad)) {
                /* Loudly absent, not silently wrong: an encoder that returns
                 * un-normalised rows has a different normalisation convention,
                 * and a "cosine" computed against it is not a cosine. */
                char b[AE_NUMBUF];
                hyp_log_error("ask.embed.not_unit", "row", ae_itoa(bad, b, sizeof(b)));
                rc = -1;
            }
        }
        if (rc == 0) {
            for (int j = 0; j < n; j++) {
                const ae_doc_t *d = &w->docs[batches.order[from + j]];
                rows[j].qualified_name = d->qualified_name;
                rows[j].node_id = d->node_id;
                rows[j].label = d->label;
                rows[j].file_path = d->file_path;
                rows[j].start_line = d->start_line;
                rows[j].end_line = d->end_line;
                rows[j].lang = d->lang;
                rows[j].content_hash = d->hash;
                rows[j].truncated = d->truncated;
                rows[j].vector = vecs + (size_t)j * (size_t)dim;
            }
            if (hyp_ask_vectors_put(store, rows, n) != HYP_ASK_VEC_OK) {
                hyp_log_error("ask.embed.put_failed", "err", hyp_ask_vectors_error(store));
                rc = -1;
            } else {
                rep->embedded += n;
            }
        }
        /* The text has served its purpose; drop it before the next group so
         * peak memory is one window, not one corpus. */
        for (int j = 0; j < n; j++) {
            ae_doc_t *d = &w->docs[batches.order[from + j]];
            free(d->text);
            d->text = NULL;
        }
        free(texts);
        free(vecs);
        free(rows);
    }
    hyp_ask_batches_free(&batches);
    free(lengths);
    ae_window_clear(w);
    return rc;
}

int hyp_ask_embed_run(const hyp_ask_encoder_t *enc, const hyp_ask_embed_opts_t *opts,
                      hyp_ask_embed_report_t *out) {
    hyp_ask_embed_report_t rep;
    memset(&rep, 0, sizeof(rep));
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!enc || !opts || !opts->project || !opts->project[0]) {
        return -1;
    }
    struct timespec t0;
    hyp_clock_gettime(CLOCK_MONOTONIC, &t0);

    const char *model_id = hyp_ask_encoder_model_id(enc);
    /* WHICH DEVICE ACTUALLY RAN — asked of the encoder, never inferred from
     * what the caller requested. A run that asked for a GPU and quietly got a
     * CPU takes 45 minutes instead of 3 and looks identical afterwards; the
     * only defence is to write down what happened. */
    const char *device_note = hyp_ask_encoder_device_note(enc);
    bool device_is_gpu = hyp_ask_encoder_device_is_gpu(enc);
    rep.device_note = ae_strdup(device_note);
    rep.device_was_gpu = device_is_gpu;
    rep.device_downgraded = (opts->device_pref == HYP_ASK_DEVICE_GPU && !device_is_gpu);
    if (rep.device_downgraded) {
        hyp_log_warn("ask.embed.device_downgraded", "requested", "gpu", "got", device_note,
                     "cost", "~15x slower than the device that was asked for");
    } else {
        hyp_log_info("ask.embed.device", "requested",
                     hyp_ask_device_pref_name(opts->device_pref), "using", device_note);
    }
    int dim = hyp_ask_encoder_dim(enc);
    int window = hyp_ask_encoder_window(enc);
    if (!model_id || dim <= 0 || window <= 0) {
        hyp_log_error("ask.embed.encoder", "err", "encoder did not report model/dim/window");
        free(rep.device_note);
        return -1;
    }

    char graph_path[AE_PATHBUF];
    if (opts->graph_db_path) {
        snprintf(graph_path, sizeof(graph_path), "%s", opts->graph_db_path);
    } else {
        const char *cache = hyp_resolve_cache_dir();
        cache = cache ? cache : hyp_tmpdir();
        if (!cache) {
            free(rep.device_note);
            return -1;
        }
        int n = snprintf(graph_path, sizeof(graph_path), "%s/%s.db", cache, opts->project);
        if (n < 0 || (size_t)n >= sizeof(graph_path)) {
            free(rep.device_note);
            return -1;
        }
    }
    if (!hyp_file_exists(graph_path)) {
        hyp_log_error("ask.embed.no_graph", "path", graph_path, "hint",
                      "index_repository has not run for this project");
        free(rep.device_note);
        return -1;
    }

    ae_graph_t graph;
    if (!ae_graph_open(&graph, graph_path, opts->project)) {
        free(rep.device_note);
        return -1;
    }
    const char *root = opts->repo_path ? opts->repo_path : graph.root_path;
    if (!root || !root[0]) {
        hyp_log_error("ask.embed.no_root", "project", opts->project);
        ae_graph_close(&graph);
        free(rep.device_note);
        return -1;
    }

    hyp_ask_vectors_t *store = opts->vectors_db_path
                                   ? hyp_ask_vectors_open_path(opts->project,
                                                               opts->vectors_db_path)
                                   : hyp_ask_vectors_open(opts->project);
    if (!store) {
        hyp_log_error("ask.embed.vectors_open", "project", opts->project);
        ae_graph_close(&graph);
        free(rep.device_note);
        return -1;
    }
    /* Whether the PREVIOUS index's truncation flags attest anything. Read
     * before begin_build, because begin_build may wipe the index. If the
     * previous state was UNKNOWN, a reused row's stored flag is unattested and
     * has to be re-probed before this run may publish it as attested. */
    bool prev_truncation_known = false;
    {
        hyp_ask_vec_meta_t prev;
        if (hyp_ask_vectors_get_meta(store, &prev) == HYP_ASK_VEC_OK) {
            prev_truncation_known = prev.truncation_known;
            hyp_ask_vec_meta_free(&prev);
        }
    }
    int begin = hyp_ask_vectors_begin_build(store, model_id, dim, window, graph.generation,
                                            device_note, opts->allow_model_change);
    if (begin == HYP_ASK_VEC_INCOMPATIBLE) {
        hyp_log_error("ask.embed.incompatible", "err", hyp_ask_vectors_error(store), "hint",
                      "re-run with --allow-model-change to discard and rebuild");
        hyp_ask_vectors_close(store);
        ae_graph_close(&graph);
        free(rep.device_note);
        return -1;
    }
    if (begin != HYP_ASK_VEC_OK) {
        hyp_log_error("ask.embed.begin", "err", hyp_ask_vectors_error(store));
        hyp_ask_vectors_close(store);
        ae_graph_close(&graph);
        free(rep.device_note);
        return -1;
    }

    /* EVERY node with a span. The ORDER BY is not cosmetic: the scan must be
     * deterministic so two runs over the same corpus produce the same batches
     * and the same reuse decisions, otherwise a throughput measurement is not
     * repeatable. */
    const char *sql = "SELECT id, qualified_name, label, file_path, start_line, end_line"
                      " FROM nodes"
                      " WHERE project = ?1 AND start_line > 0 AND end_line >= start_line"
                      "   AND file_path <> ''"
                      " ORDER BY qualified_name";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(graph.db, sql, -1, &st, NULL) != SQLITE_OK) {
        hyp_log_error("ask.embed.scan_prepare", "err", sqlite3_errmsg(graph.db));
        hyp_ask_vectors_close(store);
        ae_graph_close(&graph);
        free(rep.device_note);
        return -1;
    }
    sqlite3_bind_text(st, 1, opts->project, -1, SQLITE_STATIC);

    int budget = opts->token_budget > 0 ? opts->token_budget : HYP_ASK_TOKEN_BUDGET;
    int max_docs = opts->max_docs > 0 ? opts->max_docs : HYP_ASK_MAX_DOCS;
    hyp_ask_compose_t compose = opts->compose;
    rep.compose = compose;

    ae_window_t win;
    memset(&win, 0, sizeof(win));
    /* The keep set for pruning: every declaration this run saw, whether it was
     * encoded or reused. Rows outside it are declarations that no longer
     * exist. */
    char **keep = NULL;
    int keep_count = 0;
    int keep_cap = 0;

    /* Reused rows whose stored truncation flag turned out to be wrong. Only
     * ever populated when the PREVIOUS index could not attest its flags. */
    char **reprobe_qn = NULL;
    bool *reprobe_flag = NULL;
    int reprobe_count = 0;
    int reprobe_cap = 0;

    /* THE TRUNCATION COUNTER'S THIRD STATE. It starts as "the encoder can say"
     * and is demoted the first time the encoder answers "I cannot". A single
     * document the encoder could not measure makes the whole set unattested,
     * because a set that is missing an unknown number of members is not a set
     * anybody can act on. */
    bool truncation_known = true;
    int rc = 0;
    int step_rc = 0;
    while (rc == 0 && (step_rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (opts->limit > 0 && rep.declarations_seen >= opts->limit) {
            rep.partial = true;
            break;
        }
        rep.declarations_seen++;
        int64_t node_id = sqlite3_column_int64(st, 0);
        const char *qn = (const char *)sqlite3_column_text(st, 1);
        const char *label = (const char *)sqlite3_column_text(st, 2);
        const char *rel = (const char *)sqlite3_column_text(st, 3);
        int start_line = sqlite3_column_int(st, 4);
        int end_line = sqlite3_column_int(st, 5);
        if (!qn || !rel) {
            rep.skipped_unreadable++;
            continue;
        }

        char abs_path[AE_PATHBUF];
        int pn = snprintf(abs_path, sizeof(abs_path), "%s/%s", root, rel);
        if (pn < 0 || (size_t)pn >= sizeof(abs_path)) {
            rep.skipped_unreadable++;
            continue;
        }
        /* The grammar id, derived from the path. Needed BEFORE the text is
         * built now, because the comment scan is language-specific — a
         * language whose comment syntax is not in the table contributes no
         * leading comment, which is exactly the pre-§2.2 behaviour. */
        const char *base = strrchr(rel, '/');
        HYPLanguage lang = hyp_language_for_filename(base ? base + 1 : rel);

        int comment_lines = 0;
        char *text = hyp_ask_document_text(abs_path, lang, compose, label, qn, opts->project, rel,
                                           start_line, end_line, &comment_lines);
        if (!text) {
            rep.skipped_unreadable++;
            continue;
        }
        if (comment_lines > 0) {
            rep.with_leading_comment++;
            rep.comment_lines += comment_lines;
        }

        if (keep_count >= keep_cap) {
            int nc = keep_cap ? keep_cap * 2 : 1024;
            char **grown = realloc(keep, (size_t)nc * sizeof(*grown));
            if (!grown) {
                free(text);
                rc = -1;
                break;
            }
            keep = grown;
            keep_cap = nc;
        }
        keep[keep_count++] = ae_strdup(qn);

        ae_doc_t doc;
        memset(&doc, 0, sizeof(doc));
        hyp_ask_content_hash(text, doc.hash);

        /* Reuse. Licensed by a byte-identical text under an index whose
         * provenance begin_build has already matched — same model, same dim,
         * same window. */
        if (!opts->force) {
            char stored[HYP_ASK_VEC_HASH_LEN + 1];
            bool stored_trunc = false;
            if (hyp_ask_vectors_stored_hash(store, qn, stored, &stored_trunc) ==
                    HYP_ASK_VEC_OK &&
                strcmp(stored, doc.hash) == 0) {
                rep.reused++;
                /* The vector is licensed for reuse; the DISCLOSURE is not.
                 * Re-probe the truncation flag when the stored index could not
                 * say — otherwise an unattested zero would be republished as an
                 * attested one. One tokenisation, no forward pass; the
                 * reference pays the same price for the same reason. */
                if (!prev_truncation_known) {
                    int fl = hyp_ask_encoder_full_token_length(enc, text);
                    if (fl < 0) {
                        truncation_known = false;
                    } else {
                        bool now_trunc = fl > window;
                        if (now_trunc != stored_trunc) {
                            if (reprobe_count >= reprobe_cap) {
                                int nc = reprobe_cap ? reprobe_cap * 2 : 64;
                                char **gq = realloc(reprobe_qn, (size_t)nc * sizeof(*gq));
                                bool *gf = realloc(reprobe_flag, (size_t)nc * sizeof(*gf));
                                if (gq) {
                                    reprobe_qn = gq;
                                }
                                if (gf) {
                                    reprobe_flag = gf;
                                }
                                if (!gq || !gf) {
                                    free(text);
                                    rc = -1;
                                    break;
                                }
                                reprobe_cap = nc;
                            }
                            reprobe_qn[reprobe_count] = ae_strdup(qn);
                            reprobe_flag[reprobe_count] = now_trunc;
                            reprobe_count++;
                        }
                    }
                }
                free(text);
                continue;
            }
        }

        doc.qualified_name = ae_strdup(qn);
        doc.label = ae_strdup(label);
        doc.file_path = ae_strdup(rel);
        /* The grammar id, derived from the path. The grammar-id -> display-name
         * map the query prefix needs is track 5's and is deliberately not
         * duplicated here; the document side is encoded BARE and has no use for
         * it. */
        doc.lang = ae_strdup(hyp_language_name(lang));
        doc.node_id = node_id;
        doc.start_line = start_line;
        doc.end_line = end_line;
        doc.text = text;

        int tl = hyp_ask_encoder_token_length(enc, text);
        doc.token_len = tl > 0 ? tl : 1;
        /* Counted with a SEPARATE call, on purpose. `token_length` answers
         * "how many slots will this occupy" and is clipped at the window;
         * `full_token_length` answers "did this overflow" and is not. Deriving
         * one from the other cannot tell a document of exactly the window
         * length from one cut off at it. */
        doc.full_len = hyp_ask_encoder_full_token_length(enc, text);
        if (doc.full_len < 0) {
            truncation_known = false;
            doc.truncated = false;
        } else {
            doc.truncated = doc.full_len > window;
        }

        if (!ae_window_push(&win, doc)) {
            ae_doc_free(&doc);
            rc = -1;
            break;
        }
        if (win.count >= HYP_ASK_SORT_WINDOW || win.bytes >= HYP_ASK_SORT_WINDOW_BYTES) {
            rc = ae_flush_window(&win, enc, store, dim, budget, max_docs, &rep);
        }
    }
    sqlite3_finalize(st);
    if (rc == 0 && step_rc != SQLITE_DONE && step_rc != SQLITE_ROW) {
        hyp_log_error("ask.embed.scan", "err", sqlite3_errmsg(graph.db));
        rc = -1;
    }
    if (rc == 0) {
        rc = ae_flush_window(&win, enc, store, dim, budget, max_docs, &rep);
    }
    ae_window_clear(&win);
    free(win.docs);

    if (rc == 0 && reprobe_count > 0) {
        if (hyp_ask_vectors_set_truncated_batch(store, (const char *const *)reprobe_qn,
                                                reprobe_flag, reprobe_count) != HYP_ASK_VEC_OK) {
            hyp_log_error("ask.embed.reprobe", "err", hyp_ask_vectors_error(store));
            rc = -1;
        } else {
            char b[AE_NUMBUF];
            hyp_log_info("ask.embed.reprobed", "rows", ae_itoa(reprobe_count, b, sizeof(b)),
                         "reason", "previous index could not attest its truncation flags");
        }
    }
    for (int i = 0; i < reprobe_count; i++) {
        free(reprobe_qn[i]);
    }
    free(reprobe_qn);
    free(reprobe_flag);

    if (rc == 0 && !rep.partial) {
        /* Only a run that saw the WHOLE corpus may prune: a limited run has no
         * opinion about the declarations it never reached. */
        int64_t removed = 0;
        if (hyp_ask_vectors_prune(store, (const char *const *)keep, keep_count, &removed) ==
            HYP_ASK_VEC_OK) {
            rep.pruned = removed;
        }
    }
    for (int i = 0; i < keep_count; i++) {
        free(keep[i]);
    }
    free(keep);

    if (rc == 0) {
        if (hyp_ask_vectors_finish_build(store, truncation_known) != HYP_ASK_VEC_OK) {
            hyp_log_error("ask.embed.finish", "err", hyp_ask_vectors_error(store));
            rc = -1;
        }
    }
    if (rc == 0) {
        hyp_ask_trunc_t tr;
        if (hyp_ask_vectors_truncation(store, &tr) == HYP_ASK_VEC_OK) {
            rep.truncation_known = tr.state != HYP_ASK_TRUNC_UNKNOWN;
            rep.truncated = tr.count;
            char line[512];
            hyp_ask_trunc_describe(&tr, line, sizeof(line));
            hyp_log_info("ask.embed.truncation", "disclosure", line);
            hyp_ask_trunc_free(&tr);
        }
    }

    hyp_ask_vectors_close(store);
    ae_graph_close(&graph);
    rep.elapsed_ms = ae_elapsed_ms(t0);
    if (out) {
        *out = rep;
    } else {
        free(rep.device_note);
        rep.device_note = NULL;
    }
    if (rc != 0) {
        return rc;
    }
    /* Zero work is not success. A scan that found no declaration completed
     * everything it attempted and indexed nothing — reporting that as 0 would
     * make "measured nothing" indistinguishable from "measured fine", which is
     * how a silent no-op survives a green run. */
    if (rep.declarations_seen == 0) {
        hyp_log_warn("ask.embed.no_declarations", "project", opts->project, "graph", graph_path,
                     "hint",
                     "the project has no node with a source span — check the project name");
        return HYP_ASK_EMBED_NO_WORK;
    }
    return 0;
}
