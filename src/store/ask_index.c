/*
 * ask_index.c — read half of the `ask` vector index. See ask_index.h.
 *
 * Read-only by construction: no CREATE, no INSERT, no migration. An absent
 * table is an answer here, not a condition to repair, and a read path that
 * quietly created one would convert "never built" into "built and empty" —
 * which is the zero-results lie the whole lane is arranged to avoid.
 */
#include "store/ask_index.h"

#include "ask/ask_vectors.h"

#include "foundation/compat.h"
#include "foundation/platform.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ASK_COL_1 = 1,
    ASK_COL_2 = 2,
    ASK_SQL_AUTO_LEN = -1,
    /* Scored candidates held while selecting the top-k. The scan is over
     * every row regardless — this only bounds what is kept. */
    ASK_MAX_LIMIT = 500,
};

/* Module-local SQLITE_TRANSIENT wrapper to dodge performance-no-int-to-ptr.
 * Same pattern as src/store/store.c and src/mcp/mcp.c. */
static sqlite3_destructor_type ask_sqlite_transient(void) {
    static const volatile intptr_t raw = -1;
    sqlite3_destructor_type dtor = NULL;
    memcpy(&dtor, (const void *)&raw, sizeof(dtor));
    return dtor;
}

/* ── Status ─────────────────────────────────────────────────────── */

static bool ask_table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
                           ASK_SQL_AUTO_LEN, &st, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st, ASK_COL_1, name, ASK_SQL_AUTO_LEN, ask_sqlite_transient());
    bool found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

static void ask_copy(char *dst, size_t dstlen, const char *src) {
    snprintf(dst, dstlen, "%s", src ? src : "");
}

/* ── The per-project vector file, which is where the vectors really are ── */

/* Fill `out` from <cache>/vectors/<project>.db. Returns false when that file
 * does not exist or holds no rows for this project, which is the caller's cue
 * to try the in-graph tables the tests build by hand. */
static bool ask_status_from_vector_file(const char *project, hyp_ask_lane_t lane,
                                        const hyp_ask_backend_t *backend, const char *want_model,
                                        const char *want_contract, hyp_ask_status_t *out) {
    if (!project || !project[0]) {
        return false;
    }
    char path[HYP_SZ_4K];
    if (!hyp_ask_vectors_path_lane(project, lane, path, sizeof(path))) {
        return false;
    }
    if (!hyp_file_exists(path)) {
        return false;
    }
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_lane(project, lane);
    if (!v) {
        return false;
    }
    hyp_ask_vec_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    if (hyp_ask_vectors_get_meta(v, &meta) != HYP_ASK_VEC_OK || meta.row_count <= 0) {
        hyp_ask_vec_meta_free(&meta);
        hyp_ask_vectors_close(v);
        return false;
    }

    ask_copy(out->model_id, sizeof(out->model_id), meta.model_id);
    out->dim = meta.dim;
    out->n_vectors = (int)meta.row_count;
    /* Only the exact word counts as "dropped". An unrecognised value — a
     * writer from a future version that learned a third policy — reads as
     * "not dropped", which is the claim that promises the caller LESS. */
    out->whole_file_spans_dropped =
        meta.whole_file_spans && strcmp(meta.whole_file_spans, "drop") == 0;

    /* PROVENANCE IS A HARD GATE. Two models' vectors are not comparable and
     * nothing downstream can detect the mix, so a disagreement is REFUSED
     * rather than served with ordinary-looking scores. */
    /* The contract joins the gate on the same terms as the model id, with one
     * asymmetry: a stored contract of "" means an index written before the
     * field existed and is NOT compared, because "unrecorded" is not the same
     * claim as "different" and must not condemn an index that is actually fine. */
    /* WHICH identity to hold the index to. The process-wide backend is the
     * right answer for the local lane and the WRONG one for the escalation
     * lane, whose vectors were made by a hosted model this binary does not
     * link — comparing those against the local encoder would condemn a
     * perfectly good index on every query. The caller passes what it expects
     * when it knows; NULL keeps the backend's. */
    const char *exp_model = (want_model && want_model[0]) ? want_model : backend->model_id;
    const char *exp_contract =
        (want_contract && want_contract[0]) ? want_contract : backend->prefix_contract;
    bool contract_conflict = exp_contract && exp_contract[0] && meta.prefix_contract &&
                             meta.prefix_contract[0] &&
                             strcmp(meta.prefix_contract, exp_contract) != 0;
    if (!exp_model || strcmp(meta.model_id ? meta.model_id : "", exp_model) != 0 ||
        meta.dim != backend->dim || contract_conflict) {
        out->avail = HYP_ASK_MODEL_MISMATCH;
        hyp_ask_vec_meta_free(&meta);
        hyp_ask_vectors_close(v);
        return true;
    }

    hyp_ask_trunc_t tr;
    memset(&tr, 0, sizeof(tr));
    if (hyp_ask_vectors_truncation(v, &tr) == HYP_ASK_VEC_OK) {
        switch (tr.state) {
        case HYP_ASK_TRUNC_SOME:
            out->trunc = HYP_ASK_TRUNC_SOME;
            out->trunc_count = (int)tr.count;
            break;
        case HYP_ASK_TRUNC_NONE:
            out->trunc = HYP_ASK_TRUNC_NONE;
            break;
        default:
            out->trunc = HYP_ASK_TRUNC_UNKNOWN;
            break;
        }
    }
    hyp_ask_trunc_free(&tr);
    out->avail = HYP_ASK_AVAILABLE;
    hyp_ask_vec_meta_free(&meta);
    hyp_ask_vectors_close(v);
    return true;
}

/* Top-k out of the vector file, converted to this header's hit shape. Returns
 * false when there is no such file, so the caller can fall through. */
static bool ask_search_vector_file(const char *project, hyp_ask_lane_t lane, const float *qvec,
                                   int limit, hyp_ask_hit_t **out, int *out_count) {
    char path[HYP_SZ_4K];
    if (!hyp_ask_vectors_path_lane(project, lane, path, sizeof(path)) || !hyp_file_exists(path)) {
        return false;
    }
    hyp_ask_vectors_t *v = hyp_ask_vectors_open_lane(project, lane);
    if (!v) {
        return false;
    }
    hyp_ask_vec_hit_t *hits = NULL;
    int n = 0;
    if (hyp_ask_vectors_search(v, qvec, HYP_ASK_DIM, limit, &hits, &n) != HYP_ASK_VEC_OK) {
        hyp_ask_vectors_close(v);
        return false;
    }
    hyp_ask_hit_t *conv = n > 0 ? (hyp_ask_hit_t *)calloc((size_t)n, sizeof(*conv)) : NULL;
    if (n > 0 && !conv) {
        hyp_ask_vec_hits_free(hits, n);
        hyp_ask_vectors_close(v);
        return false;
    }
    for (int i = 0; i < n; i++) {
        conv[i].node_id = hits[i].node_id;
        conv[i].qualified_name = hits[i].qualified_name ? hyp_strdup(hits[i].qualified_name) : NULL;
        /* The lane cites spans, and the leaf name is the last dotted segment of
         * the qualified name — the vector store keeps no separate `name`
         * column because everything it needs to cite is in the row already. */
        conv[i].name = NULL;
        if (conv[i].qualified_name) {
            const char *dot = strrchr(conv[i].qualified_name, '.');
            conv[i].name = hyp_strdup(dot ? dot + 1 : conv[i].qualified_name);
        }
        conv[i].label = hits[i].label ? hyp_strdup(hits[i].label) : NULL;
        conv[i].file_path = hits[i].file_path ? hyp_strdup(hits[i].file_path) : NULL;
        conv[i].start_line = hits[i].start_line;
        conv[i].end_line = hits[i].end_line;
        conv[i].score = (double)hits[i].score;
        conv[i].truncated = hits[i].truncated;
    }
    hyp_ask_vec_hits_free(hits, n);
    hyp_ask_vectors_close(v);
    *out = conv;
    *out_count = n;
    return true;
}

void hyp_ask_index_status_lane(hyp_store_t *s, const char *project, hyp_ask_lane_t lane,
                               const char *want_model, const char *want_contract,
                               hyp_ask_status_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->trunc = HYP_ASK_TRUNC_UNKNOWN;

    const hyp_ask_backend_t *backend = hyp_ask_backend();
    if (backend) {
        ask_copy(out->backend_id, sizeof(out->backend_id), backend->model_id);
    }

    /* Backend first, and it wins. Without an encoder the question cannot be
     * turned into a vector at all, so what the store holds is not the
     * caller's problem yet and telling them to run an embed pass would send
     * them somewhere that cannot help. */
    if (!backend) {
        out->avail = HYP_ASK_NO_BACKEND;
        return;
    }

    /* ── The real store comes first ──────────────────────────────
     *
     * This file was written before the write half existed, against the schema
     * ask_index.h documents: two tables inside the GRAPH database. The write
     * half that actually landed (src/ask/ask_vectors.c) keeps the vectors in a
     * SEPARATE per-project file, <cache>/vectors/<project>.db, deliberately —
     * surviving a re-index is the whole reason it is its own file.
     *
     * Nothing caught the divergence, because until an encoder existed no index
     * could be built and every test built the fixture by hand. The first real
     * `hyponoia embed` run produced 4,117 vectors and `ask` answered
     * `no_semantic_index`, which is the worst shape a bug can take here: a
     * confident claim that the tool has nothing, about a corpus it had just
     * finished embedding.
     *
     * So: ask the real store, and fall back to the in-graph tables only when
     * it has nothing. The fallback is what the 25 response-shaping tests drive,
     * and it is deliberately SECOND — production data must win. */
    if (ask_status_from_vector_file(project, lane, backend, want_model, want_contract, out)) {
        return;
    }

    sqlite3 *db = s ? hyp_store_get_db(s) : NULL;
    if (!db || !project || !project[0] || !ask_table_exists(db, HYP_ASK_META_TABLE) ||
        !ask_table_exists(db, HYP_ASK_VECTORS_TABLE)) {
        out->avail = HYP_ASK_NO_INDEX;
        return;
    }

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT model_id, dim, trunc_state, trunc_count, n_rows FROM " HYP_ASK_META_TABLE
        " WHERE project = ?1";
    if (sqlite3_prepare_v2(db, sql, ASK_SQL_AUTO_LEN, &st, NULL) != SQLITE_OK) {
        out->avail = HYP_ASK_NO_INDEX;
        return;
    }
    sqlite3_bind_text(st, ASK_COL_1, project, ASK_SQL_AUTO_LEN, ask_sqlite_transient());
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        out->avail = HYP_ASK_NO_INDEX;
        return;
    }

    ask_copy(out->model_id, sizeof(out->model_id), (const char *)sqlite3_column_text(st, 0));
    out->dim = sqlite3_column_int(st, 1);
    /* Copied, not aliased: sqlite3_column_text hands back memory owned by the
     * statement, which sqlite3_finalize below invalidates. Reading it after is
     * a use-after-free that happens to work often enough to ship. */
    char trunc_state[HYP_SZ_32];
    ask_copy(trunc_state, sizeof(trunc_state), (const char *)sqlite3_column_text(st, 2));
    out->trunc_count = sqlite3_column_int(st, 3);
    out->n_vectors = sqlite3_column_int(st, 4);
    sqlite3_finalize(st);

    /* An UNRECOGNISED trunc_state reads as UNKNOWN, never as NONE. A writer
     * from a future version that learned a fourth word must degrade into "I
     * cannot say", not into "nothing was cut". */
    if (strcmp(trunc_state, "some") == 0) {
        out->trunc = HYP_ASK_TRUNC_SOME;
    } else if (strcmp(trunc_state, "none") == 0) {
        out->trunc = HYP_ASK_TRUNC_NONE;
    } else {
        out->trunc = HYP_ASK_TRUNC_UNKNOWN;
    }
    /* "some" with a zero count is a writer bug, and reading it as `some`
     * without a number is more honest than reading it as `none`. */
    if (out->trunc == HYP_ASK_TRUNC_SOME && out->trunc_count <= 0) {
        out->trunc_count = 0;
    }

    if (out->n_vectors <= 0) {
        out->avail = HYP_ASK_NO_INDEX;
        return;
    }
    /* Model or dimension mismatch is a REFUSAL, not a warning. Two models'
     * vectors are not comparable, the scores they produce are ordinary
     * floats, and nothing downstream can tell the mix from a bad day. */
    if (out->dim != backend->dim || strcmp(out->model_id, backend->model_id) != 0) {
        out->avail = HYP_ASK_MODEL_MISMATCH;
        return;
    }
    out->avail = HYP_ASK_AVAILABLE;
}

/* ── Search ─────────────────────────────────────────────────────── */

typedef struct {
    int64_t node_id;
    double score;
    bool truncated;
} ask_scored_t;

static int ask_cmp_scored(const void *pa, const void *pb) {
    const ask_scored_t *a = (const ask_scored_t *)pa;
    const ask_scored_t *b = (const ask_scored_t *)pb;
    if (a->score < b->score) {
        return 1; /* descending */
    }
    if (a->score > b->score) {
        return -1;
    }
    /* Ties broken by node_id so paging and repeated calls agree. An unstable
     * order here would make two identical questions disagree about which of
     * two equally-scored declarations is "the" answer. */
    if (a->node_id < b->node_id) {
        return -1;
    }
    return a->node_id > b->node_id ? 1 : 0;
}

static char *ask_dup(const unsigned char *s) {
    const char *src = s ? (const char *)s : "";
    size_t n = strlen(src) + 1;
    char *out = (char *)malloc(n);
    if (out) {
        memcpy(out, src, n);
    }
    return out;
}

int hyp_ask_index_search_lane(hyp_store_t *s, const char *project, hyp_ask_lane_t lane,
                              const float *qvec, int limit, hyp_ask_hit_t **out, int *out_count) {
    if (!out || !out_count) {
        return HYP_STORE_ERR;
    }
    *out = NULL;
    *out_count = 0;
    if (!s || !project || !project[0] || !qvec) {
        return HYP_STORE_ERR;
    }
    if (limit <= 0) {
        limit = HYP_DEFAULT_SEARCH_LIMIT;
    }
    if (limit > ASK_MAX_LIMIT) {
        limit = ASK_MAX_LIMIT;
    }
    /* The real store first, for the reason spelled out in
     * hyp_ask_index_status. The in-graph tables below are the fixture path. */
    if (ask_search_vector_file(project, lane, qvec, limit, out, out_count)) {
        return HYP_STORE_OK;
    }
    sqlite3 *db = hyp_store_get_db(s);
    if (!db || !ask_table_exists(db, HYP_ASK_VECTORS_TABLE)) {
        return HYP_STORE_ERR;
    }
    if (limit <= 0) {
        limit = HYP_DEFAULT_SEARCH_LIMIT;
    }
    if (limit > ASK_MAX_LIMIT) {
        limit = ASK_MAX_LIMIT;
    }

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT node_id, vec, truncated FROM " HYP_ASK_VECTORS_TABLE " WHERE project = ?1";
    if (sqlite3_prepare_v2(db, sql, ASK_SQL_AUTO_LEN, &st, NULL) != SQLITE_OK) {
        return HYP_STORE_ERR;
    }
    sqlite3_bind_text(st, ASK_COL_1, project, ASK_SQL_AUTO_LEN, ask_sqlite_transient());

    size_t cap = (size_t)limit * 4 + 64;
    ask_scored_t *scored = (ask_scored_t *)malloc(cap * sizeof(*scored));
    if (!scored) {
        sqlite3_finalize(st);
        return HYP_STORE_ERR;
    }
    size_t n = 0;
    const size_t vec_bytes = (size_t)HYP_ASK_DIM * sizeof(float);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 1);
        int blob_len = sqlite3_column_bytes(st, 1);
        /* A row of the wrong width is SKIPPED, not truncated into a score.
         * Reading 1024 floats out of a 768-float blob is how a dimension
         * mismatch becomes a plausible ranking instead of a refusal. */
        if (!blob || (size_t)blob_len != vec_bytes) {
            continue;
        }
        /* memcpy rather than a cast: a BLOB has no alignment guarantee, and
         * dereferencing it as float* is undefined on the platforms that care. */
        float row[HYP_ASK_DIM];
        memcpy(row, blob, vec_bytes);
        double dot = 0.0;
        for (int d = 0; d < HYP_ASK_DIM; d++) {
            dot += (double)row[d] * (double)qvec[d];
        }
        if (n == cap) {
            /* Keep only the best `limit` so far, then carry on. Bounded
             * memory over a corpus of any size; the scan is exact either
             * way because nothing is discarded before it is scored. */
            qsort(scored, n, sizeof(*scored), ask_cmp_scored);
            n = (size_t)limit < n ? (size_t)limit : n;
        }
        scored[n].node_id = sqlite3_column_int64(st, 0);
        scored[n].score = dot;
        scored[n].truncated = sqlite3_column_int(st, 2) != 0;
        n++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        free(scored);
        return HYP_STORE_ERR;
    }
    if (n == 0) {
        free(scored);
        return HYP_STORE_OK;
    }

    qsort(scored, n, sizeof(*scored), ask_cmp_scored);
    if (n > (size_t)limit) {
        n = (size_t)limit;
    }

    hyp_ask_hit_t *hits = (hyp_ask_hit_t *)calloc(n, sizeof(*hits));
    if (!hits) {
        free(scored);
        return HYP_STORE_ERR;
    }

    sqlite3_stmt *nq = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT qualified_name, name, label, file_path, start_line, end_line "
                           "FROM nodes WHERE id = ?1 AND project = ?2",
                           ASK_SQL_AUTO_LEN, &nq, NULL) != SQLITE_OK) {
        free(scored);
        free(hits);
        return HYP_STORE_ERR;
    }

    int emitted = 0;
    for (size_t i = 0; i < n; i++) {
        sqlite3_reset(nq);
        sqlite3_bind_int64(nq, ASK_COL_1, scored[i].node_id);
        sqlite3_bind_text(nq, ASK_COL_2, project, ASK_SQL_AUTO_LEN, ask_sqlite_transient());
        if (sqlite3_step(nq) != SQLITE_ROW) {
            /* The node is gone — a vector whose declaration was deleted
             * between the embed pass and now. DROPPED rather than returned:
             * a row that can be ranked but not cited is a citation to
             * nothing, which is the one failure this engine exists to
             * remove. It costs a result, not a wrong answer. */
            continue;
        }
        hyp_ask_hit_t *h = &hits[emitted];
        h->node_id = scored[i].node_id;
        h->qualified_name = ask_dup(sqlite3_column_text(nq, 0));
        h->name = ask_dup(sqlite3_column_text(nq, 1));
        h->label = ask_dup(sqlite3_column_text(nq, 2));
        h->file_path = ask_dup(sqlite3_column_text(nq, 3));
        h->start_line = sqlite3_column_int(nq, 4);
        h->end_line = sqlite3_column_int(nq, 5);
        h->score = scored[i].score;
        h->truncated = scored[i].truncated;
        emitted++;
    }
    sqlite3_finalize(nq);
    free(scored);

    *out = hits;
    *out_count = emitted;
    return HYP_STORE_OK;
}

void hyp_ask_free_hits(hyp_ask_hit_t *hits, int count) {
    if (!hits) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(hits[i].qualified_name);
        free(hits[i].name);
        free(hits[i].label);
        free(hits[i].file_path);
    }
    free(hits);
}

/* ── The historical, local-lane entry points ───────────────────────
 *
 * Kept as thin forwarders rather than rewritten call sites: everything that
 * existed before the escalation lane means the local one, and saying so once
 * here is safer than editing dozens of callers to pass a constant. */
void hyp_ask_index_status(hyp_store_t *s, const char *project, hyp_ask_status_t *out) {
    hyp_ask_index_status_lane(s, project, HYP_ASK_LANE_LOCAL, NULL, NULL, out);
}

int hyp_ask_index_search(hyp_store_t *s, const char *project, const float *qvec, int limit,
                         hyp_ask_hit_t **out, int *out_count) {
    return hyp_ask_index_search_lane(s, project, HYP_ASK_LANE_LOCAL, qvec, limit, out, out_count);
}
