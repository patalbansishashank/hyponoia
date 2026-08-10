/*
 * ask_vectors.c — the per-declaration vector store.
 *
 * Schema (one database per project, at <cache>/vectors/<project>.db):
 *
 *   ask_index    one row. Provenance and the truncation state. Refusing on a
 *                mismatch here is what stops two models' vectors sharing a
 *                matrix, which nothing downstream could detect.
 *
 *   ask_vectors  one row per declaration, keyed on qualified_name.
 *                node_id is a per-generation HINT, never identity: a full
 *                re-index re-mints node ids 1..N.
 *
 * The 17 MB in §2.1 is exactly the matrix: 4,287 x 1024 x 4 = 17,559,552 bytes.
 * On Hyponoia's own lld/ELF span population it is 4,117 x 1024 x 4 =
 * 16,863,232 bytes, plus spans, hashes and names.
 */

#include "ask/ask_vectors.h"
#include "ask/ask_encoder.h"
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/log.h"
#include "foundation/platform.h"

#include <sqlite3.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    AV_ERRBUF = 512,
    AV_PATHBUF = 1024,
    AV_DIR_PERMS = 0700,
    AV_TIMEBUF = 32,
};

struct hyp_ask_vectors {
    sqlite3 *db;
    char *project;
    char *path;
    int dim; /* the dim this build is writing; 0 until begin_build */
    char errbuf[AV_ERRBUF];
};

/* SQLITE_TRANSIENT is ((void*)-1); constructing it through a memcpy keeps
 * clang-tidy's performance-no-int-to-ptr off every use site, the same trick
 * store.c uses. */
static sqlite3_destructor_type av_transient(void) {
    static const volatile intptr_t raw = -1;
    sqlite3_destructor_type dtor = NULL;
    memcpy(&dtor, (const void *)&raw, sizeof(dtor));
    return dtor;
}
#define AV_TRANSIENT (av_transient())

static void av_err(hyp_ask_vectors_t *v, const char *msg) {
    if (v) {
        snprintf(v->errbuf, sizeof(v->errbuf), "%s", msg ? msg : "unknown");
    }
}

static void av_err_sqlite(hyp_ask_vectors_t *v, const char *prefix) {
    if (v) {
        snprintf(v->errbuf, sizeof(v->errbuf), "%s: %s", prefix,
                 v->db ? sqlite3_errmsg(v->db) : "no db");
    }
}

static int av_exec(hyp_ask_vectors_t *v, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(v->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        snprintf(v->errbuf, sizeof(v->errbuf), "exec: %s", err ? err : "unknown");
        sqlite3_free(err);
        return HYP_ASK_VEC_ERR;
    }
    return HYP_ASK_VEC_OK;
}

static char *av_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static void av_iso_now(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tm;
    hyp_gmtime_r(&t, &tm);
    (void)strftime(buf, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* ── Paths ─────────────────────────────────────────────────────── */

bool hyp_ask_vectors_path(const char *project, char *buf, size_t bufsz) {
    if (!project || !project[0] || !buf || bufsz == 0) {
        return false;
    }
    const char *cache = hyp_resolve_cache_dir();
    if (!cache) {
        cache = hyp_tmpdir();
    }
    if (!cache) {
        return false;
    }
    int n = snprintf(buf, bufsz, "%s/vectors/%s.db", cache, project);
    return n > 0 && (size_t)n < bufsz;
}

static bool av_ensure_parent_dir(const char *path) {
    char dir[AV_PATHBUF];
    int n = snprintf(dir, sizeof(dir), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        return false;
    }
    char *slash = strrchr(dir, '/');
    if (!slash) {
        return true; /* relative name in cwd */
    }
    *slash = '\0';
    if (dir[0] == '\0') {
        return true;
    }
    return hyp_mkdir_p(dir, AV_DIR_PERMS);
}

/* ── Schema ────────────────────────────────────────────────────── */

static int av_init_schema(hyp_ask_vectors_t *v) {
    /* No REFERENCES to the graph's nodes table: it lives in a different file.
     * The link is (project, qualified_name), which is what survives a
     * re-index; node_id is carried alongside as a hint for the current
     * generation and is deliberately not a key. */
    const char *ddl =
        "CREATE TABLE IF NOT EXISTS ask_index ("
        "  singleton        INTEGER PRIMARY KEY CHECK (singleton = 1),"
        "  format           INTEGER NOT NULL,"
        "  project          TEXT NOT NULL,"
        "  model_id         TEXT NOT NULL,"
        "  dim              INTEGER NOT NULL,"
        "  window_tokens    INTEGER NOT NULL,"
        "  graph_generation TEXT NOT NULL DEFAULT '',"
        /* Which device built it. A 45-minute CPU build and a 3-minute GPU
         * build produce indistinguishable files; this is the only place the
         * difference survives. */
        "  device_note      TEXT NOT NULL DEFAULT '',"
        "  built_at         TEXT NOT NULL DEFAULT '',"
        "  row_count        INTEGER NOT NULL DEFAULT 0,"
        /* 0 = the encoder could not say. NOT the same claim as
         * truncated_count = 0, and the two must never collapse. */
        "  truncation_known INTEGER NOT NULL DEFAULT 0,"
        "  truncated_count  INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS ask_vectors ("
        "  qualified_name TEXT PRIMARY KEY,"
        "  node_id        INTEGER NOT NULL DEFAULT 0,"
        "  label          TEXT NOT NULL DEFAULT '',"
        "  file_path      TEXT NOT NULL DEFAULT '',"
        "  start_line     INTEGER NOT NULL DEFAULT 0,"
        "  end_line       INTEGER NOT NULL DEFAULT 0,"
        "  lang           TEXT NOT NULL DEFAULT '',"
        "  content_hash   TEXT NOT NULL,"
        "  truncated      INTEGER NOT NULL DEFAULT 0,"
        "  vector         BLOB NOT NULL"
        ");"
        /* The only index worth having. Truncated rows are a small set that has
         * to be enumerated on every disclosure; everything else about this
         * table is a full scan by design. */
        "CREATE INDEX IF NOT EXISTS idx_ask_truncated ON ask_vectors(truncated)"
        " WHERE truncated = 1;";
    return av_exec(v, ddl);
}

static int av_configure(hyp_ask_vectors_t *v) {
    int rc = av_exec(v, "PRAGMA temp_store = MEMORY;");
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    rc = av_exec(v, "PRAGMA busy_timeout = 10000;");
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    rc = av_exec(v, "PRAGMA journal_mode = WAL;");
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    /* NORMAL, matching the graph store: a torn build is re-runnable, and this
     * file is a cache of work that can always be recomputed from source. */
    return av_exec(v, "PRAGMA synchronous = NORMAL;");
}

/* ── Lifecycle ─────────────────────────────────────────────────── */

hyp_ask_vectors_t *hyp_ask_vectors_open_path(const char *project, const char *db_path) {
    if (!project || !project[0] || !db_path || !db_path[0]) {
        return NULL;
    }
    if (!av_ensure_parent_dir(db_path)) {
        return NULL;
    }
    hyp_ask_vectors_t *v = calloc(1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    v->project = av_strdup(project);
    v->path = av_strdup(db_path);
    if (!v->project || !v->path) {
        hyp_ask_vectors_close(v);
        return NULL;
    }
    if (sqlite3_open_v2(db_path, &v->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        hyp_ask_vectors_close(v);
        return NULL;
    }
    if (av_configure(v) != HYP_ASK_VEC_OK || av_init_schema(v) != HYP_ASK_VEC_OK) {
        hyp_ask_vectors_close(v);
        return NULL;
    }
    return v;
}

hyp_ask_vectors_t *hyp_ask_vectors_open(const char *project) {
    char path[AV_PATHBUF];
    if (!hyp_ask_vectors_path(project, path, sizeof(path))) {
        return NULL;
    }
    return hyp_ask_vectors_open_path(project, path);
}

void hyp_ask_vectors_close(hyp_ask_vectors_t *v) {
    if (!v) {
        return;
    }
    if (v->db) {
        sqlite3_close(v->db);
    }
    free(v->project);
    free(v->path);
    free(v);
}

const char *hyp_ask_vectors_error(const hyp_ask_vectors_t *v) {
    if (!v) {
        return "no store";
    }
    return v->errbuf[0] ? v->errbuf : "ok";
}

int hyp_ask_vectors_drop(const char *project) {
    char path[AV_PATHBUF];
    if (!hyp_ask_vectors_path(project, path, sizeof(path))) {
        return HYP_ASK_VEC_ERR;
    }
    /* Sidecars first: a surviving -wal against a deleted database is how a
     * later open resurrects rows nobody expected. */
    char side[AV_PATHBUF + 8];
    snprintf(side, sizeof(side), "%s-wal", path);
    (void)hyp_unlink(side);
    snprintf(side, sizeof(side), "%s-shm", path);
    (void)hyp_unlink(side);
    if (!hyp_file_exists(path)) {
        return HYP_ASK_VEC_NOT_FOUND;
    }
    return hyp_unlink(path) == 0 ? HYP_ASK_VEC_OK : HYP_ASK_VEC_ERR;
}

/* ── Provenance ────────────────────────────────────────────────── */

int hyp_ask_vectors_get_meta(hyp_ask_vectors_t *v, hyp_ask_vec_meta_t *out) {
    if (!v || !out) {
        return HYP_ASK_VEC_ERR;
    }
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT format, project, model_id, dim, window_tokens, graph_generation,"
                      "       built_at, row_count, truncation_known, truncated_count, device_note"
                      " FROM ask_index WHERE singleton = 1";
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        av_err_sqlite(v, "meta prepare");
        return HYP_ASK_VEC_ERR;
    }
    int rc = HYP_ASK_VEC_NOT_FOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->format = sqlite3_column_int(st, 0);
        out->project = av_strdup((const char *)sqlite3_column_text(st, 1));
        out->model_id = av_strdup((const char *)sqlite3_column_text(st, 2));
        out->dim = sqlite3_column_int(st, 3);
        out->window_tokens = sqlite3_column_int(st, 4);
        out->graph_generation = av_strdup((const char *)sqlite3_column_text(st, 5));
        out->built_at = av_strdup((const char *)sqlite3_column_text(st, 6));
        out->row_count = sqlite3_column_int64(st, 7);
        out->truncation_known = sqlite3_column_int(st, 8) != 0;
        out->truncated_count = sqlite3_column_int64(st, 9);
        out->device_note = av_strdup((const char *)sqlite3_column_text(st, 10));
        rc = HYP_ASK_VEC_OK;
    }
    sqlite3_finalize(st);
    return rc;
}

void hyp_ask_vec_meta_free(hyp_ask_vec_meta_t *m) {
    if (!m) {
        return;
    }
    free(m->project);
    free(m->model_id);
    free(m->graph_generation);
    free(m->device_note);
    free(m->built_at);
    memset(m, 0, sizeof(*m));
}

int hyp_ask_vectors_check_compatible(hyp_ask_vectors_t *v, const char *model_id, int dim,
                                     int window_tokens) {
    hyp_ask_vec_meta_t m;
    int rc = hyp_ask_vectors_get_meta(v, &m);
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    int verdict = HYP_ASK_VEC_OK;
    if (m.format != HYP_ASK_VEC_FORMAT) {
        snprintf(v->errbuf, sizeof(v->errbuf),
                 "stored format %d against this binary's %d — refused rather than read",
                 m.format, HYP_ASK_VEC_FORMAT);
        verdict = HYP_ASK_VEC_INCOMPATIBLE;
    } else if (model_id && m.model_id && strcmp(model_id, m.model_id) != 0) {
        snprintf(v->errbuf, sizeof(v->errbuf),
                 "stored vectors are from model '%s', this run uses '%s' — two models' vectors "
                 "are not comparable and nothing downstream can tell them apart",
                 m.model_id, model_id);
        verdict = HYP_ASK_VEC_INCOMPATIBLE;
    } else if (dim > 0 && m.dim != dim) {
        snprintf(v->errbuf, sizeof(v->errbuf), "stored dim %d against encoder dim %d", m.dim, dim);
        verdict = HYP_ASK_VEC_INCOMPATIBLE;
    } else if (window_tokens > 0 && m.window_tokens != window_tokens) {
        /* A different window means the truncation set describes a different
         * question. It is not a vector mismatch, but it is a disclosure
         * mismatch, and the disclosure is the reason the counter exists. */
        snprintf(v->errbuf, sizeof(v->errbuf),
                 "stored window %d against encoder window %d — the truncation set was "
                 "denominated in a different number",
                 m.window_tokens, window_tokens);
        verdict = HYP_ASK_VEC_INCOMPATIBLE;
    }
    hyp_ask_vec_meta_free(&m);
    return verdict;
}

/* ── Writing ───────────────────────────────────────────────────── */

int hyp_ask_vectors_begin_build(hyp_ask_vectors_t *v, const char *model_id, int dim,
                                int window_tokens, const char *graph_generation,
                                const char *device_note, bool wipe_incompatible) {
    if (!v || !model_id || dim <= 0 || window_tokens <= 0) {
        av_err(v, "begin_build: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    int compat = hyp_ask_vectors_check_compatible(v, model_id, dim, window_tokens);
    if (compat == HYP_ASK_VEC_INCOMPATIBLE) {
        if (!wipe_incompatible) {
            return HYP_ASK_VEC_INCOMPATIBLE;
        }
        hyp_log_warn("ask.vectors.wipe", "reason", v->errbuf);
        if (av_exec(v, "DELETE FROM ask_vectors;") != HYP_ASK_VEC_OK) {
            return HYP_ASK_VEC_ERR;
        }
    } else if (compat == HYP_ASK_VEC_ERR) {
        return HYP_ASK_VEC_ERR;
    }

    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO ask_index (singleton, format, project, model_id, dim, window_tokens,"
        "                       graph_generation, device_note, built_at, row_count,"
        "                       truncation_known, truncated_count)"
        " VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6, ?7, '', 0, 0, 0)"
        " ON CONFLICT(singleton) DO UPDATE SET"
        "   format = excluded.format, project = excluded.project,"
        "   model_id = excluded.model_id, dim = excluded.dim,"
        "   window_tokens = excluded.window_tokens,"
        "   graph_generation = excluded.graph_generation,"
        "   device_note = excluded.device_note";
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        av_err_sqlite(v, "begin_build prepare");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_bind_int(st, 1, HYP_ASK_VEC_FORMAT);
    sqlite3_bind_text(st, 2, v->project, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, model_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, dim);
    sqlite3_bind_int(st, 5, window_tokens);
    sqlite3_bind_text(st, 6, graph_generation ? graph_generation : "", -1, AV_TRANSIENT);
    sqlite3_bind_text(st, 7, device_note ? device_note : "", -1, AV_TRANSIENT);
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) {
        av_err_sqlite(v, "begin_build");
        return HYP_ASK_VEC_ERR;
    }
    v->dim = dim;
    return HYP_ASK_VEC_OK;
}

int hyp_ask_vectors_put(hyp_ask_vectors_t *v, const hyp_ask_vec_row_t *rows, int count) {
    if (!v || (count > 0 && !rows)) {
        av_err(v, "put: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    if (count <= 0) {
        return HYP_ASK_VEC_OK;
    }
    if (v->dim <= 0) {
        av_err(v, "put: begin_build has not run, so the row width is unknown");
        return HYP_ASK_VEC_ERR;
    }
    /* Every row is checked BEFORE anything is written. Cosine is a dot product
     * only when both sides are unit vectors; a row that is not would score
     * silently wrong rather than loudly absent, so it must never reach disk. */
    for (int i = 0; i < count; i++) {
        if (!rows[i].qualified_name || !rows[i].content_hash || !rows[i].vector) {
            av_err(v, "put: row is missing its key, hash or vector");
            return HYP_ASK_VEC_ERR;
        }
        int bad = -1;
        if (!hyp_ask_vectors_are_unit(rows[i].vector, 1, v->dim, &bad)) {
            snprintf(v->errbuf, sizeof(v->errbuf),
                     "put: '%s' is not unit-normalised within %g — the caller normalised with a "
                     "different convention, or not at all",
                     rows[i].qualified_name, (double)HYP_ASK_NORM_TOLERANCE);
            return HYP_ASK_VEC_ERR;
        }
    }

    if (av_exec(v, "BEGIN IMMEDIATE;") != HYP_ASK_VEC_OK) {
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO ask_vectors (qualified_name, node_id, label, file_path,"
                      "                          start_line, end_line, lang, content_hash,"
                      "                          truncated, vector)"
                      " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)"
                      " ON CONFLICT(qualified_name) DO UPDATE SET"
                      "   node_id = excluded.node_id, label = excluded.label,"
                      "   file_path = excluded.file_path, start_line = excluded.start_line,"
                      "   end_line = excluded.end_line, lang = excluded.lang,"
                      "   content_hash = excluded.content_hash,"
                      "   truncated = excluded.truncated, vector = excluded.vector";
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        av_err_sqlite(v, "put prepare");
        (void)av_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    int rc = HYP_ASK_VEC_OK;
    for (int i = 0; i < count && rc == HYP_ASK_VEC_OK; i++) {
        const hyp_ask_vec_row_t *r = &rows[i];
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, r->qualified_name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, r->node_id);
        sqlite3_bind_text(st, 3, r->label ? r->label : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, r->file_path ? r->file_path : "", -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 5, r->start_line);
        sqlite3_bind_int(st, 6, r->end_line);
        sqlite3_bind_text(st, 7, r->lang ? r->lang : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 8, r->content_hash, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 9, r->truncated ? 1 : 0);
        sqlite3_bind_blob(st, 10, r->vector, (int)((size_t)v->dim * sizeof(float)), SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE) {
            av_err_sqlite(v, "put step");
            rc = HYP_ASK_VEC_ERR;
        }
    }
    sqlite3_finalize(st);
    if (rc != HYP_ASK_VEC_OK) {
        (void)av_exec(v, "ROLLBACK;");
        return rc;
    }
    return av_exec(v, "COMMIT;");
}

int hyp_ask_vectors_prune(hyp_ask_vectors_t *v, const char *const *keep, int keep_count,
                          int64_t *out_removed) {
    if (out_removed) {
        *out_removed = 0;
    }
    if (!v || (keep_count > 0 && !keep)) {
        av_err(v, "prune: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    if (av_exec(v, "BEGIN IMMEDIATE;") != HYP_ASK_VEC_OK) {
        return HYP_ASK_VEC_ERR;
    }
    /* A temp table rather than a giant IN list: the keep set is the whole
     * corpus, and SQLITE_MAX_VARIABLE_NUMBER is not a number a corpus should
     * have to respect. */
    int rc = av_exec(v, "CREATE TEMP TABLE IF NOT EXISTS ask_keep (qn TEXT PRIMARY KEY);"
                        "DELETE FROM ask_keep;");
    if (rc != HYP_ASK_VEC_OK) {
        (void)av_exec(v, "ROLLBACK;");
        return rc;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(v->db, "INSERT OR IGNORE INTO ask_keep(qn) VALUES (?1)", -1, &st,
                           NULL) != SQLITE_OK) {
        av_err_sqlite(v, "prune prepare");
        (void)av_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    for (int i = 0; i < keep_count; i++) {
        if (!keep[i]) {
            continue;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, keep[i], -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE) {
            av_err_sqlite(v, "prune insert");
            rc = HYP_ASK_VEC_ERR;
            break;
        }
    }
    sqlite3_finalize(st);
    if (rc != HYP_ASK_VEC_OK) {
        (void)av_exec(v, "ROLLBACK;");
        return rc;
    }
    rc = av_exec(v, "DELETE FROM ask_vectors WHERE qualified_name NOT IN (SELECT qn FROM ask_keep);");
    if (rc != HYP_ASK_VEC_OK) {
        (void)av_exec(v, "ROLLBACK;");
        return rc;
    }
    int removed = sqlite3_changes(v->db);
    (void)av_exec(v, "DROP TABLE IF EXISTS ask_keep;");
    rc = av_exec(v, "COMMIT;");
    if (rc == HYP_ASK_VEC_OK && out_removed) {
        *out_removed = removed;
    }
    return rc;
}

int hyp_ask_vectors_finish_build(hyp_ask_vectors_t *v, bool truncation_known) {
    if (!v) {
        return HYP_ASK_VEC_ERR;
    }
    char now[AV_TIMEBUF];
    av_iso_now(now, sizeof(now));
    if (!truncation_known) {
        /* Nothing may be left holding an attested-looking flag under an
         * unattested index. Clearing them costs one indexed delete-shaped
         * update and removes the only way a caller could read a stale `true`
         * out of a store that has just said it does not know. */
        if (av_exec(v, "UPDATE ask_vectors SET truncated = 0 WHERE truncated = 1;") !=
            HYP_ASK_VEC_OK) {
            return HYP_ASK_VEC_ERR;
        }
    }
    sqlite3_stmt *st = NULL;
    /* truncated_count is derived from the rows rather than counted by the
     * caller, so the number disclosed and the set enumerated can never
     * disagree. When the state is UNKNOWN it is forced to zero and the
     * truncation_known flag is the only thing a reader is allowed to act on. */
    const char *sql =
        "UPDATE ask_index SET"
        "  built_at = ?1,"
        "  row_count = (SELECT count(*) FROM ask_vectors),"
        "  truncation_known = ?2,"
        "  truncated_count = CASE WHEN ?2 = 0 THEN 0"
        "                    ELSE (SELECT count(*) FROM ask_vectors WHERE truncated = 1) END"
        " WHERE singleton = 1";
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        av_err_sqlite(v, "finish prepare");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_bind_text(st, 1, now, -1, AV_TRANSIENT);
    sqlite3_bind_int(st, 2, truncation_known ? 1 : 0);
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) {
        av_err_sqlite(v, "finish");
        return HYP_ASK_VEC_ERR;
    }
    return HYP_ASK_VEC_OK;
}

/* ── Reuse ─────────────────────────────────────────────────────── */

int hyp_ask_vectors_stored_hash(hyp_ask_vectors_t *v, const char *qualified_name, char *out_hash,
                                bool *out_truncated) {
    if (!v || !qualified_name || !out_hash) {
        return HYP_ASK_VEC_ERR;
    }
    out_hash[0] = '\0';
    if (out_truncated) {
        *out_truncated = false;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(v->db,
                           "SELECT content_hash, truncated FROM ask_vectors"
                           " WHERE qualified_name = ?1",
                           -1, &st, NULL) != SQLITE_OK) {
        av_err_sqlite(v, "stored_hash prepare");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_bind_text(st, 1, qualified_name, -1, SQLITE_STATIC);
    int rc = HYP_ASK_VEC_NOT_FOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *h = (const char *)sqlite3_column_text(st, 0);
        snprintf(out_hash, HYP_ASK_VEC_HASH_LEN + 1, "%s", h ? h : "");
        if (out_truncated) {
            *out_truncated = sqlite3_column_int(st, 1) != 0;
        }
        rc = HYP_ASK_VEC_OK;
    }
    sqlite3_finalize(st);
    return rc;
}

int hyp_ask_vectors_set_truncated_batch(hyp_ask_vectors_t *v, const char *const *qualified_names,
                                        const bool *flags, int count) {
    if (!v || (count > 0 && (!qualified_names || !flags))) {
        av_err(v, "set_truncated: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    if (count <= 0) {
        return HYP_ASK_VEC_OK;
    }
    if (av_exec(v, "BEGIN IMMEDIATE;") != HYP_ASK_VEC_OK) {
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(v->db, "UPDATE ask_vectors SET truncated = ?2 WHERE qualified_name = ?1",
                           -1, &st, NULL) != SQLITE_OK) {
        av_err_sqlite(v, "set_truncated prepare");
        (void)av_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    int rc = HYP_ASK_VEC_OK;
    for (int i = 0; i < count; i++) {
        if (!qualified_names[i]) {
            continue;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, qualified_names[i], -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, flags[i] ? 1 : 0);
        if (sqlite3_step(st) != SQLITE_DONE) {
            av_err_sqlite(v, "set_truncated step");
            rc = HYP_ASK_VEC_ERR;
            break;
        }
    }
    sqlite3_finalize(st);
    if (rc != HYP_ASK_VEC_OK) {
        (void)av_exec(v, "ROLLBACK;");
        return rc;
    }
    return av_exec(v, "COMMIT;");
}

/* ── Reading ───────────────────────────────────────────────────── */

int64_t hyp_ask_vectors_count(hyp_ask_vectors_t *v) {
    if (!v) {
        return 0;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(v->db, "SELECT count(*) FROM ask_vectors", -1, &st, NULL) !=
        SQLITE_OK) {
        return 0;
    }
    int64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

/* Insertion into a k-sized min-ordered array. k is 10 in practice, so a heap
 * would be more code than the linear insert costs. */
static void av_topk_insert(hyp_ask_vec_hit_t *heap, int *filled, int k, hyp_ask_vec_hit_t cand) {
    if (*filled < k) {
        int i = (*filled)++;
        while (i > 0 && heap[i - 1].score < cand.score) {
            heap[i] = heap[i - 1];
            i--;
        }
        heap[i] = cand;
        return;
    }
    if (cand.score <= heap[k - 1].score) {
        free(cand.qualified_name);
        free(cand.label);
        free(cand.file_path);
        free(cand.lang);
        return;
    }
    free(heap[k - 1].qualified_name);
    free(heap[k - 1].label);
    free(heap[k - 1].file_path);
    free(heap[k - 1].lang);
    int i = k - 1;
    while (i > 0 && heap[i - 1].score < cand.score) {
        heap[i] = heap[i - 1];
        i--;
    }
    heap[i] = cand;
}

int hyp_ask_vectors_search(hyp_ask_vectors_t *v, const float *query, int dim, int k,
                           hyp_ask_vec_hit_t **out, int *out_count) {
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!v || !query || dim <= 0 || k <= 0 || !out || !out_count) {
        av_err(v, "search: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    hyp_ask_vec_meta_t m;
    int rc = hyp_ask_vectors_get_meta(v, &m);
    if (rc != HYP_ASK_VEC_OK) {
        return rc; /* NOT_FOUND: no index built — the lane is unavailable */
    }
    int stored_dim = m.dim;
    hyp_ask_vec_meta_free(&m);
    if (stored_dim != dim) {
        snprintf(v->errbuf, sizeof(v->errbuf),
                 "question is %d-D against a %d-D index — two different models, or one model "
                 "with two configurations",
                 dim, stored_dim);
        return HYP_ASK_VEC_INCOMPATIBLE;
    }
    int bad = -1;
    if (!hyp_ask_vectors_are_unit(query, 1, dim, &bad)) {
        av_err(v, "search: the question vector is not unit-normalised, so its 'cosine' scores "
                  "would be silently wrong rather than loudly absent");
        return HYP_ASK_VEC_ERR;
    }

    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT qualified_name, label, file_path, start_line, end_line, lang,"
                      "       node_id, truncated, vector FROM ask_vectors";
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        av_err_sqlite(v, "search prepare");
        return HYP_ASK_VEC_ERR;
    }
    hyp_ask_vec_hit_t *heap = calloc((size_t)k, sizeof(*heap));
    if (!heap) {
        sqlite3_finalize(st);
        av_err(v, "search: out of memory");
        return HYP_ASK_VEC_ERR;
    }
    int filled = 0;
    size_t want_bytes = (size_t)dim * sizeof(float);
    int step_rc = 0;
    while ((step_rc = sqlite3_step(st)) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 8);
        int blob_len = sqlite3_column_bytes(st, 8);
        if (!blob || (size_t)blob_len != want_bytes) {
            /* A mis-sized row is a corrupt row. Skipping it silently would let
             * one bad write quietly shrink the haystack, so say so. */
            hyp_log_warn("ask.vectors.bad_row", "qn",
                         (const char *)sqlite3_column_text(st, 0), "bytes",
                         blob_len == 0 ? "0" : "mismatch");
            continue;
        }
        /* Both sides are unit vectors, so the dot product IS the cosine — no
         * division, and none should be added. */
        const float *row = (const float *)blob;
        double dot = 0.0;
        for (int d = 0; d < dim; d++) {
            dot += (double)row[d] * (double)query[d];
        }
        hyp_ask_vec_hit_t cand = {0};
        cand.qualified_name = av_strdup((const char *)sqlite3_column_text(st, 0));
        cand.label = av_strdup((const char *)sqlite3_column_text(st, 1));
        cand.file_path = av_strdup((const char *)sqlite3_column_text(st, 2));
        cand.start_line = sqlite3_column_int(st, 3);
        cand.end_line = sqlite3_column_int(st, 4);
        cand.lang = av_strdup((const char *)sqlite3_column_text(st, 5));
        cand.node_id = sqlite3_column_int64(st, 6);
        cand.truncated = sqlite3_column_int(st, 7) != 0;
        cand.score = (float)dot;
        av_topk_insert(heap, &filled, k, cand);
    }
    sqlite3_finalize(st);
    if (step_rc != SQLITE_DONE) {
        av_err_sqlite(v, "search scan");
        hyp_ask_vec_hits_free(heap, filled);
        return HYP_ASK_VEC_ERR;
    }
    *out = heap;
    *out_count = filled;
    return HYP_ASK_VEC_OK;
}

void hyp_ask_vec_hits_free(hyp_ask_vec_hit_t *hits, int count) {
    if (!hits) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(hits[i].qualified_name);
        free(hits[i].label);
        free(hits[i].file_path);
        free(hits[i].lang);
    }
    free(hits);
}

/* ── The truncation counter ────────────────────────────────────── */

int hyp_ask_vectors_truncation(hyp_ask_vectors_t *v, hyp_ask_trunc_t *out) {
    if (!v || !out) {
        return HYP_ASK_VEC_ERR;
    }
    memset(out, 0, sizeof(*out));
    out->state = HYP_ASK_TRUNC_UNKNOWN;
    hyp_ask_vec_meta_t m;
    int rc = hyp_ask_vectors_get_meta(v, &m);
    if (rc != HYP_ASK_VEC_OK) {
        /* No index at all. UNKNOWN is the right answer: nobody has measured
         * anything, which is a different claim from "nothing was truncated". */
        return rc;
    }
    out->window_tokens = m.window_tokens;
    if (!m.truncation_known) {
        out->state = HYP_ASK_TRUNC_UNKNOWN;
        hyp_ask_vec_meta_free(&m);
        return HYP_ASK_VEC_OK;
    }
    out->count = m.truncated_count;
    out->state = m.truncated_count > 0 ? HYP_ASK_TRUNC_SOME : HYP_ASK_TRUNC_NONE;
    hyp_ask_vec_meta_free(&m);

    if (out->state != HYP_ASK_TRUNC_SOME) {
        return HYP_ASK_VEC_OK;
    }
    /* Name the largest offender, by span line count — the reference
     * implementation names the largest by text length; lines are the same
     * ordering without re-reading every file. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(v->db,
                           "SELECT qualified_name, file_path, start_line, end_line"
                           " FROM ask_vectors WHERE truncated = 1"
                           " ORDER BY (end_line - start_line) DESC, qualified_name ASC LIMIT 1",
                           -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            out->worst_qualified_name = av_strdup((const char *)sqlite3_column_text(st, 0));
            out->worst_file_path = av_strdup((const char *)sqlite3_column_text(st, 1));
            out->worst_start_line = sqlite3_column_int(st, 2);
            out->worst_end_line = sqlite3_column_int(st, 3);
        }
        sqlite3_finalize(st);
    }
    return HYP_ASK_VEC_OK;
}

void hyp_ask_trunc_free(hyp_ask_trunc_t *t) {
    if (!t) {
        return;
    }
    free(t->worst_qualified_name);
    free(t->worst_file_path);
    memset(t, 0, sizeof(*t));
}

void hyp_ask_trunc_describe(const hyp_ask_trunc_t *t, char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) {
        return;
    }
    if (!t) {
        snprintf(buf, bufsz, "truncation: UNKNOWN — no vector index has been built, so whether "
                             "any declaration was encoded from its head alone cannot be said.");
        return;
    }
    switch (t->state) {
    case HYP_ASK_TRUNC_NONE:
        snprintf(buf, bufsz, "truncation: none — every declaration fitted the model's %d-token "
                             "window.",
                 t->window_tokens);
        break;
    case HYP_ASK_TRUNC_SOME:
        snprintf(buf, bufsz,
                 "truncation: %lld declaration(s) exceeded the model's %d-token window and were "
                 "encoded FROM THEIR FIRST TOKENS ONLY — largest `%s` at %s:%d-%d. This is "
                 "disclosed on every answer, not only ones a truncated row reaches.",
                 (long long)t->count, t->window_tokens,
                 t->worst_qualified_name ? t->worst_qualified_name : "?",
                 t->worst_file_path ? t->worst_file_path : "?", t->worst_start_line,
                 t->worst_end_line);
        break;
    case HYP_ASK_TRUNC_UNKNOWN:
    default:
        snprintf(buf, bufsz,
                 "truncation: UNKNOWN — this encoder does not report token counts, so whether any "
                 "declaration was encoded from its head alone cannot be said here. This is not a "
                 "claim that none was.");
        break;
    }
}
