/*
 * record_vectors.c — the record-content vector store (F1).
 *
 * Schema (one database, default <cache>/memory/vectors.db):
 *
 *   record_vec_index    one row. The encoder stamp — model_id, prefix
 *                       contract, dim, window. Refusing on a mismatch here is
 *                       what stops two encoders' vectors sharing a matrix,
 *                       which nothing downstream could detect. The stamp
 *                       lives HERE and nowhere else: never in an address,
 *                       never on a record (invariant I3).
 *
 *   record_vectors      one row per distinct CONTENT, keyed on
 *                       hyp_addr_span_hash(content). N records sharing text
 *                       share the row; the record id never reaches this file.
 *
 *   record_vec_pending  shipped vectors whose hash matched no local record
 *                       content when they arrived. Visible and enumerable,
 *                       never dropped, never attached until claim() joins
 *                       them against a real record.
 */

#include "ask/record_vectors.h"
#include "ask/ask_encoder.h" /* hyp_ask_vectors_are_unit */
#include "foundation/compat.h"
#include "foundation/compat_fs.h"
#include "foundation/identity.h" /* hyp_addr_span_hash — THE hash, I1 */
#include "foundation/log.h"
#include "foundation/platform.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One hash, one width, everywhere in the lane. ask_embed.c asserts the same
 * pair; repeating it here means this file cannot even build against a world
 * where the two widths diverged. */
_Static_assert(HYP_ADDR_SPAN_HASH_LEN == HYP_ASK_VEC_HASH_LEN,
               "the record-content hash and the C1 span hash must be one hash");

enum {
    RV_ERRBUF = 512,
    RV_PATHBUF = 1024,
    RV_DIR_PERMS = 0700,
    RV_TIMEBUF = 32,
    /* Sanity cap on the two stamp strings in a ship file header. Longer is a
     * corrupt or hostile file, refused before any allocation trusts it. */
    RV_SHIP_STR_MAX = 4096,
};

struct hyp_record_vectors {
    sqlite3 *db;
    char *path;
    char errbuf[RV_ERRBUF];
};

/* SQLITE_TRANSIENT via memcpy — the same clang-tidy-quiet trick as
 * ask_vectors.c and store.c. */
static sqlite3_destructor_type rv_transient(void) {
    static const volatile intptr_t raw = -1;
    sqlite3_destructor_type dtor = NULL;
    memcpy(&dtor, (const void *)&raw, sizeof(dtor));
    return dtor;
}
#define RV_TRANSIENT (rv_transient())

static void rv_err(hyp_record_vectors_t *v, const char *msg) {
    if (v) {
        snprintf(v->errbuf, sizeof(v->errbuf), "%s", msg ? msg : "unknown");
    }
}

static void rv_err_sqlite(hyp_record_vectors_t *v, const char *prefix) {
    if (v) {
        snprintf(v->errbuf, sizeof(v->errbuf), "%s: %s", prefix,
                 v->db ? sqlite3_errmsg(v->db) : "no db");
    }
}

static int rv_exec(hyp_record_vectors_t *v, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(v->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        snprintf(v->errbuf, sizeof(v->errbuf), "exec: %s", err ? err : "unknown");
        sqlite3_free(err);
        return HYP_ASK_VEC_ERR;
    }
    return HYP_ASK_VEC_OK;
}

static char *rv_strdup(const char *s) {
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

static void rv_iso_now(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tm;
    hyp_gmtime_r(&t, &tm);
    (void)strftime(buf, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static bool rv_hash_valid(const char *h) {
    if (!h) {
        return false;
    }
    for (int i = 0; i < HYP_ADDR_SPAN_HASH_LEN; i++) {
        char c = h[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return false;
        }
    }
    return h[HYP_ADDR_SPAN_HASH_LEN] == '\0';
}

/* ── Paths and lifecycle ───────────────────────────────────────── */

bool hyp_record_vectors_default_path(char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) {
        return false;
    }
    const char *cache = hyp_resolve_cache_dir();
    if (!cache) {
        cache = hyp_tmpdir();
    }
    if (!cache) {
        return false;
    }
    /* memory/, not vectors/: the vectors/ namespace is "<project>.db" and a
     * fixed name there would collide with a project of the same name. The
     * subdirectory keeps it invisible to the three scanners that enumerate
     * every *.db directly under <cache>, for the same reason vectors/ is. */
    int n = snprintf(buf, bufsz, "%s/memory/vectors.db", cache);
    return n > 0 && (size_t)n < bufsz;
}

static bool rv_ensure_parent_dir(const char *path) {
    char dir[RV_PATHBUF];
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
    return hyp_mkdir_p(dir, RV_DIR_PERMS);
}

static int rv_init_schema(hyp_record_vectors_t *v) {
    /* No REFERENCES to any record store: records live wherever the record set
     * lives, and the join is by content hash at read time. Nothing here can
     * hold an id, an author, an origin, a thread, an anchor or a timestamp of
     * a record — address-free is a property of the schema, not a policy. */
    const char *ddl = "CREATE TABLE IF NOT EXISTS record_vec_index ("
                      "  singleton       INTEGER PRIMARY KEY CHECK (singleton = 1),"
                      "  format          INTEGER NOT NULL,"
                      "  model_id        TEXT NOT NULL,"
                      "  prefix_contract TEXT NOT NULL DEFAULT '',"
                      "  dim             INTEGER NOT NULL,"
                      "  window_tokens   INTEGER NOT NULL"
                      ");"
                      "CREATE TABLE IF NOT EXISTS record_vectors ("
                      "  content_hash TEXT PRIMARY KEY,"
                      "  vector       BLOB NOT NULL"
                      ");"
                      "CREATE TABLE IF NOT EXISTS record_vec_pending ("
                      "  content_hash TEXT PRIMARY KEY,"
                      "  vector       BLOB NOT NULL,"
                      "  received_at  TEXT NOT NULL DEFAULT ''"
                      ");";
    return rv_exec(v, ddl);
}

static int rv_configure(hyp_record_vectors_t *v) {
    int rc = rv_exec(v, "PRAGMA temp_store = MEMORY;");
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    rc = rv_exec(v, "PRAGMA busy_timeout = 10000;");
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    rc = rv_exec(v, "PRAGMA journal_mode = WAL;");
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    /* NORMAL, unlike the graph store's cache rationale: these vectors are
     * recomputable from record content, so a torn write costs a re-embed,
     * not a record. */
    return rv_exec(v, "PRAGMA synchronous = NORMAL;");
}

hyp_record_vectors_t *hyp_record_vectors_open_path(const char *db_path) {
    if (!db_path || !db_path[0]) {
        return NULL;
    }
    if (!rv_ensure_parent_dir(db_path)) {
        return NULL;
    }
    hyp_record_vectors_t *v = calloc(1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    v->path = rv_strdup(db_path);
    if (!v->path) {
        hyp_record_vectors_close(v);
        return NULL;
    }
    if (sqlite3_open_v2(db_path, &v->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        hyp_record_vectors_close(v);
        return NULL;
    }
    if (rv_configure(v) != HYP_ASK_VEC_OK || rv_init_schema(v) != HYP_ASK_VEC_OK) {
        hyp_record_vectors_close(v);
        return NULL;
    }
    return v;
}

hyp_record_vectors_t *hyp_record_vectors_open(void) {
    char path[RV_PATHBUF];
    if (!hyp_record_vectors_default_path(path, sizeof(path))) {
        return NULL;
    }
    return hyp_record_vectors_open_path(path);
}

void hyp_record_vectors_close(hyp_record_vectors_t *v) {
    if (!v) {
        return;
    }
    if (v->db) {
        sqlite3_close(v->db);
    }
    free(v->path);
    free(v);
}

const char *hyp_record_vectors_error(const hyp_record_vectors_t *v) {
    if (!v) {
        return "no store";
    }
    return v->errbuf[0] ? v->errbuf : "ok";
}

/* ── The stamp ─────────────────────────────────────────────────── */

static int64_t rv_count_table(hyp_record_vectors_t *v, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    int64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int hyp_record_vectors_get_meta(hyp_record_vectors_t *v, hyp_record_vec_meta_t *out) {
    if (!v || !out) {
        return HYP_ASK_VEC_ERR;
    }
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT format, model_id, prefix_contract, dim, window_tokens"
                      " FROM record_vec_index WHERE singleton = 1";
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        rv_err_sqlite(v, "meta prepare");
        return HYP_ASK_VEC_ERR;
    }
    int rc = HYP_ASK_VEC_NOT_FOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->format = sqlite3_column_int(st, 0);
        out->model_id = rv_strdup((const char *)sqlite3_column_text(st, 1));
        out->prefix_contract = rv_strdup((const char *)sqlite3_column_text(st, 2));
        out->dim = sqlite3_column_int(st, 3);
        out->window_tokens = sqlite3_column_int(st, 4);
        rc = HYP_ASK_VEC_OK;
    }
    sqlite3_finalize(st);
    if (rc == HYP_ASK_VEC_OK) {
        out->row_count = rv_count_table(v, "SELECT count(*) FROM record_vectors");
        out->pending_count = rv_count_table(v, "SELECT count(*) FROM record_vec_pending");
    }
    return rc;
}

void hyp_record_vec_meta_free(hyp_record_vec_meta_t *m) {
    if (!m) {
        return;
    }
    free(m->model_id);
    free(m->prefix_contract);
    memset(m, 0, sizeof(*m));
}

/* Load the stamp and gate it against this binary's schema format. The reason
 * every entry point goes through here rather than reading the row itself: a
 * newer file must be refused identically everywhere, not wherever someone
 * remembered. */
static int rv_load_stamp(hyp_record_vectors_t *v, hyp_record_vec_meta_t *m) {
    int rc = hyp_record_vectors_get_meta(v, m);
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    if (m->format != HYP_RECORD_VEC_FORMAT) {
        snprintf(v->errbuf, sizeof(v->errbuf),
                 "stored format %d against this binary's %d — refused rather than read", m->format,
                 HYP_RECORD_VEC_FORMAT);
        hyp_record_vec_meta_free(m);
        return HYP_ASK_VEC_INCOMPATIBLE;
    }
    return HYP_ASK_VEC_OK;
}

int hyp_record_vectors_begin(hyp_record_vectors_t *v, const char *model_id,
                             const char *prefix_contract, int dim, int window_tokens,
                             bool wipe_incompatible) {
    if (!v || !model_id || !model_id[0] || dim <= 0 || window_tokens <= 0) {
        rv_err(v, "begin: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    hyp_record_vec_meta_t m;
    int rc = rv_load_stamp(v, &m);
    if (rc == HYP_ASK_VEC_OK) {
        int verdict =
            hyp_ask_stamp_check(m.model_id, m.prefix_contract, m.dim, m.window_tokens, model_id,
                                prefix_contract, dim, window_tokens, v->errbuf, sizeof(v->errbuf));
        hyp_record_vec_meta_free(&m);
        rc = verdict;
    }
    if (rc == HYP_ASK_VEC_INCOMPATIBLE) {
        if (!wipe_incompatible) {
            return HYP_ASK_VEC_INCOMPATIBLE;
        }
        hyp_log_warn("record.vectors.wipe", "reason", v->errbuf);
        /* Pending rows go too: they passed the OLD stamp's gate, and under the
         * new stamp they would be exactly the silent mix the gate exists to
         * prevent. */
        if (rv_exec(v, "DELETE FROM record_vectors; DELETE FROM record_vec_pending;") !=
            HYP_ASK_VEC_OK) {
            return HYP_ASK_VEC_ERR;
        }
    } else if (rc == HYP_ASK_VEC_ERR) {
        return HYP_ASK_VEC_ERR;
    }

    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO record_vec_index (singleton, format, model_id, prefix_contract, dim,"
        "                              window_tokens)"
        " VALUES (1, ?1, ?2, ?3, ?4, ?5)"
        " ON CONFLICT(singleton) DO UPDATE SET"
        "   format = excluded.format, model_id = excluded.model_id,"
        "   prefix_contract = excluded.prefix_contract, dim = excluded.dim,"
        "   window_tokens = excluded.window_tokens";
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        rv_err_sqlite(v, "begin prepare");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_bind_int(st, 1, HYP_RECORD_VEC_FORMAT);
    sqlite3_bind_text(st, 2, model_id, -1, RV_TRANSIENT);
    sqlite3_bind_text(st, 3, prefix_contract ? prefix_contract : "", -1, RV_TRANSIENT);
    sqlite3_bind_int(st, 4, dim);
    sqlite3_bind_int(st, 5, window_tokens);
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) {
        rv_err_sqlite(v, "begin");
        return HYP_ASK_VEC_ERR;
    }
    return HYP_ASK_VEC_OK;
}

/* ── Writing ───────────────────────────────────────────────────── */

static bool rv_hash_in(hyp_record_vectors_t *v, const char *table, const char *hash, bool *out) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE content_hash = ?1", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        rv_err_sqlite(v, "lookup prepare");
        return false;
    }
    sqlite3_bind_text(st, 1, hash, -1, RV_TRANSIENT);
    *out = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return true;
}

int hyp_record_vectors_plan(hyp_record_vectors_t *v, const hyp_record_t *const *records, int count,
                            int *out_indices, int *out_job_count) {
    if (out_job_count) {
        *out_job_count = 0;
    }
    if (!v || (count > 0 && (!records || !out_indices)) || !out_job_count) {
        rv_err(v, "plan: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    if (count <= 0) {
        return HYP_ASK_VEC_OK;
    }
    /* The hashes already chosen for a job this batch, for in-batch dedup.
     * Content is the ONLY input to the hash — two records differing in every
     * other field land on one entry here, which is the entire point. */
    char (*chosen)[HYP_ADDR_SPAN_HASH_LEN + 1] = malloc((size_t)count * sizeof(*chosen));
    if (!chosen) {
        rv_err(v, "plan: out of memory");
        return HYP_ASK_VEC_ERR;
    }
    int jobs = 0;
    for (int i = 0; i < count; i++) {
        if (!records[i] || !records[i]->content) {
            free(chosen);
            rv_err(v, "plan: a record (or its content) is NULL");
            return HYP_ASK_VEC_ERR;
        }
        char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
        hyp_addr_span_hash(records[i]->content, hash);
        bool dup = false;
        for (int j = 0; j < jobs; j++) {
            if (strcmp(chosen[j], hash) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        bool stored = false;
        if (!rv_hash_in(v, "record_vectors", hash, &stored)) {
            free(chosen);
            return HYP_ASK_VEC_ERR;
        }
        if (stored) {
            continue; /* embedded once already — the reuse this store exists for */
        }
        memcpy(chosen[jobs], hash, sizeof(hash));
        out_indices[jobs] = i;
        jobs++;
    }
    free(chosen);
    *out_job_count = jobs;
    return HYP_ASK_VEC_OK;
}

int hyp_record_vectors_put(hyp_record_vectors_t *v, const hyp_record_t *rec, const float *vector) {
    if (!v || !rec || !rec->content || !vector) {
        rv_err(v, "put: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    hyp_record_vec_meta_t m;
    int rc = rv_load_stamp(v, &m);
    if (rc == HYP_ASK_VEC_NOT_FOUND) {
        rv_err(v, "put: no encoder stamp — call begin first, so the space these vectors live in "
                  "is recorded before any vector is");
        return HYP_ASK_VEC_ERR;
    }
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    int dim = m.dim;
    hyp_record_vec_meta_free(&m);
    int bad = -1;
    if (!hyp_ask_vectors_are_unit(vector, 1, dim, &bad)) {
        rv_err(v, "put: vector is not unit-normalised — the caller normalised with a different "
                  "convention, or not at all");
        return HYP_ASK_VEC_ERR;
    }
    /* THE KEY. content, and nothing else, reaches the hash: the id (which
     * covers author, timestamp, origin, thread) is not an input here, so
     * byte-identical text in two sessions lands on one row. */
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(rec->content, hash);

    if (rv_exec(v, "BEGIN IMMEDIATE;") != HYP_ASK_VEC_OK) {
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO record_vectors (content_hash, vector) VALUES (?1, ?2)"
                      " ON CONFLICT(content_hash) DO UPDATE SET vector = excluded.vector";
    if (sqlite3_prepare_v2(v->db, sql, -1, &st, NULL) != SQLITE_OK) {
        rv_err_sqlite(v, "put prepare");
        (void)rv_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_bind_text(st, 1, hash, -1, RV_TRANSIENT);
    sqlite3_bind_blob(st, 2, vector, (int)((size_t)dim * sizeof(float)), RV_TRANSIENT);
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) {
        rv_err_sqlite(v, "put");
        (void)rv_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    /* A pending vector for the same content is superseded: the content is now
     * locally embedded and attached, in the same stamped space. */
    st = NULL;
    if (sqlite3_prepare_v2(v->db, "DELETE FROM record_vec_pending WHERE content_hash = ?1", -1, &st,
                           NULL) != SQLITE_OK) {
        rv_err_sqlite(v, "put pending prepare");
        (void)rv_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_bind_text(st, 1, hash, -1, RV_TRANSIENT);
    step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) {
        rv_err_sqlite(v, "put pending");
        (void)rv_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    return rv_exec(v, "COMMIT;");
}

/* ── Reading ───────────────────────────────────────────────────── */

int64_t hyp_record_vectors_count(hyp_record_vectors_t *v) {
    if (!v) {
        return 0;
    }
    return rv_count_table(v, "SELECT count(*) FROM record_vectors");
}

int64_t hyp_record_vectors_pending_count(hyp_record_vectors_t *v) {
    if (!v) {
        return 0;
    }
    return rv_count_table(v, "SELECT count(*) FROM record_vec_pending");
}

int hyp_record_vectors_get(hyp_record_vectors_t *v, const hyp_record_t *rec, float *out, int dim) {
    if (!v || !rec || !rec->content || !out || dim <= 0) {
        rv_err(v, "get: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    hyp_record_vec_meta_t m;
    int rc = rv_load_stamp(v, &m);
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }
    int stored_dim = m.dim;
    hyp_record_vec_meta_free(&m);
    if (stored_dim != dim) {
        snprintf(v->errbuf, sizeof(v->errbuf),
                 "asked for %d-D against a %d-D store — two different models, or one model with "
                 "two configurations",
                 dim, stored_dim);
        return HYP_ASK_VEC_INCOMPATIBLE;
    }
    char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
    hyp_addr_span_hash(rec->content, hash);
    sqlite3_stmt *st = NULL;
    /* record_vectors ONLY: a held vector is visible through the pending
     * surface, and nowhere else — held is not attached. */
    if (sqlite3_prepare_v2(v->db, "SELECT vector FROM record_vectors WHERE content_hash = ?1", -1,
                           &st, NULL) != SQLITE_OK) {
        rv_err_sqlite(v, "get prepare");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_bind_text(st, 1, hash, -1, RV_TRANSIENT);
    rc = HYP_ASK_VEC_NOT_FOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int blob_len = sqlite3_column_bytes(st, 0);
        if (!blob || (size_t)blob_len != (size_t)dim * sizeof(float)) {
            rv_err(v, "get: stored vector has the wrong width — a corrupt row, refused");
            rc = HYP_ASK_VEC_ERR;
        } else {
            memcpy(out, blob, (size_t)dim * sizeof(float));
            rc = HYP_ASK_VEC_OK;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

/* ── The ship file ─────────────────────────────────────────────── */

/* Integers and IEEE-754 float bits are written little-endian EXPLICITLY, so
 * the file means the same thing on any host rather than on hosts that happen
 * to share the writer's byte order. */
static void rv_put_u32(unsigned char *p, uint32_t x) {
    p[0] = (unsigned char)(x & 0xFFU);
    p[1] = (unsigned char)((x >> 8) & 0xFFU);
    p[2] = (unsigned char)((x >> 16) & 0xFFU);
    p[3] = (unsigned char)((x >> 24) & 0xFFU);
}

static uint32_t rv_get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void rv_put_u64(unsigned char *p, uint64_t x) {
    rv_put_u32(p, (uint32_t)(x & 0xFFFFFFFFU));
    rv_put_u32(p + 4, (uint32_t)(x >> 32));
}

static uint64_t rv_get_u64(const unsigned char *p) {
    return (uint64_t)rv_get_u32(p) | ((uint64_t)rv_get_u32(p + 4) << 32);
}

static bool rv_write_u32(FILE *f, uint32_t x) {
    unsigned char b[4];
    rv_put_u32(b, x);
    return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static bool rv_write_u64(FILE *f, uint64_t x) {
    unsigned char b[8];
    rv_put_u64(b, x);
    return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static bool rv_write_str(FILE *f, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > RV_SHIP_STR_MAX) {
        return false;
    }
    if (!rv_write_u32(f, (uint32_t)n)) {
        return false;
    }
    return n == 0 || fwrite(s, 1, n, f) == n;
}

static bool rv_write_floats(FILE *f, const float *vec, int dim) {
    for (int i = 0; i < dim; i++) {
        uint32_t bits = 0;
        memcpy(&bits, &vec[i], sizeof(bits));
        if (!rv_write_u32(f, bits)) {
            return false;
        }
    }
    return true;
}

static bool rv_read_exact(FILE *f, void *buf, size_t n) {
    return fread(buf, 1, n, f) == n;
}

static bool rv_read_u32(FILE *f, uint32_t *out) {
    unsigned char b[4];
    if (!rv_read_exact(f, b, sizeof(b))) {
        return false;
    }
    *out = rv_get_u32(b);
    return true;
}

static bool rv_read_u64(FILE *f, uint64_t *out) {
    unsigned char b[8];
    if (!rv_read_exact(f, b, sizeof(b))) {
        return false;
    }
    *out = rv_get_u64(b);
    return true;
}

/* Reads a length-prefixed string into a malloc'd NUL-terminated buffer.
 * Refuses lengths over the cap before allocating anything sized by them. */
static bool rv_read_str(FILE *f, char **out) {
    *out = NULL;
    uint32_t n = 0;
    if (!rv_read_u32(f, &n) || n > RV_SHIP_STR_MAX) {
        return false;
    }
    char *s = malloc((size_t)n + 1);
    if (!s) {
        return false;
    }
    if (n > 0 && !rv_read_exact(f, s, n)) {
        free(s);
        return false;
    }
    s[n] = '\0';
    *out = s;
    return true;
}

static bool rv_read_floats(FILE *f, float *vec, int dim) {
    for (int i = 0; i < dim; i++) {
        uint32_t bits = 0;
        if (!rv_read_u32(f, &bits)) {
            return false;
        }
        memcpy(&vec[i], &bits, sizeof(bits));
    }
    return true;
}

int hyp_record_vectors_export(hyp_record_vectors_t *v, const char *file_path,
                              int64_t *out_exported) {
    if (out_exported) {
        *out_exported = 0;
    }
    if (!v || !file_path || !file_path[0]) {
        rv_err(v, "export: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    hyp_record_vec_meta_t m;
    int rc = rv_load_stamp(v, &m);
    if (rc == HYP_ASK_VEC_NOT_FOUND) {
        rv_err(v, "export: no encoder stamp — an unstamped store cannot say what space its "
                  "vectors live in, and a shipment that cannot say is a shipment nobody can "
                  "safely receive");
        return HYP_ASK_VEC_ERR;
    }
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }

    FILE *f = hyp_fopen(file_path, "wb");
    if (!f) {
        hyp_record_vec_meta_free(&m);
        rv_err(v, "export: cannot open the output file");
        return HYP_ASK_VEC_ERR;
    }
    bool ok = fwrite(HYP_RECORD_VEC_SHIP_MAGIC, 1, 8, f) == 8 &&
              rv_write_u32(f, HYP_RECORD_VEC_SHIP_FORMAT) && rv_write_u32(f, (uint32_t)m.dim) &&
              rv_write_u32(f, (uint32_t)m.window_tokens) && rv_write_str(f, m.model_id) &&
              rv_write_str(f, m.prefix_contract) && rv_write_u64(f, (uint64_t)m.row_count);
    int dim = m.dim;
    int64_t expect = m.row_count;
    hyp_record_vec_meta_free(&m);

    int64_t written = 0;
    sqlite3_stmt *st = NULL;
    if (ok) {
        /* Canonical order — ascending hash, the record set's own convention —
         * so two exports of equal stores are byte-identical files. */
        if (sqlite3_prepare_v2(v->db,
                               "SELECT content_hash, vector FROM record_vectors"
                               " ORDER BY content_hash",
                               -1, &st, NULL) != SQLITE_OK) {
            rv_err_sqlite(v, "export prepare");
            ok = false;
        }
    }
    while (ok && sqlite3_step(st) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(st, 0);
        const void *blob = sqlite3_column_blob(st, 1);
        int blob_len = sqlite3_column_bytes(st, 1);
        if (!rv_hash_valid(hash) || !blob || (size_t)blob_len != (size_t)dim * sizeof(float)) {
            rv_err(v, "export: a stored row is malformed — refused rather than shipped");
            ok = false;
            break;
        }
        if (fwrite(hash, 1, HYP_ADDR_SPAN_HASH_LEN, f) != HYP_ADDR_SPAN_HASH_LEN ||
            !rv_write_floats(f, (const float *)blob, dim)) {
            rv_err(v, "export: write failed");
            ok = false;
            break;
        }
        written++;
    }
    if (st) {
        sqlite3_finalize(st);
    }
    if (ok && written != expect) {
        /* The header promised a count; a file that under-delivers it would be
         * read as truncated, so it IS an error here, loudly. */
        rv_err(v, "export: row count changed under the export — refused");
        ok = false;
    }
    if (fclose(f) != 0) {
        rv_err(v, "export: close failed");
        ok = false;
    }
    if (!ok) {
        (void)hyp_unlink(file_path); /* never leave a half-written shipment */
        return HYP_ASK_VEC_ERR;
    }
    if (out_exported) {
        *out_exported = written;
    }
    return HYP_ASK_VEC_OK;
}

int hyp_record_vectors_import(hyp_record_vectors_t *v, const char *file_path,
                              const char *const *local_hashes, int local_count,
                              hyp_record_vec_import_report_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!v || !file_path || !file_path[0] || (local_count > 0 && !local_hashes) || !out) {
        rv_err(v, "import: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    /* The receiver's stamp first. A store with no stamp CANNOT verify the
     * space; "could not check" refuses, it does not wave through. */
    hyp_record_vec_meta_t m;
    int rc = rv_load_stamp(v, &m);
    if (rc == HYP_ASK_VEC_NOT_FOUND) {
        rv_err(v, "import refused: this store has no encoder stamp, so the shipment's space "
                  "cannot be verified — call begin with this machine's encoder first");
        return HYP_ASK_VEC_INCOMPATIBLE;
    }
    if (rc != HYP_ASK_VEC_OK) {
        return rc;
    }

    FILE *f = hyp_fopen(file_path, "rb");
    if (!f) {
        hyp_record_vec_meta_free(&m);
        rv_err(v, "import: cannot open the file");
        return HYP_ASK_VEC_ERR;
    }
    char magic[8];
    char *ship_model = NULL;
    char *ship_contract = NULL;
    uint32_t ship_format = 0;
    uint32_t ship_dim = 0;
    uint32_t ship_window = 0;
    uint64_t ship_count = 0;
    bool header_ok = rv_read_exact(f, magic, sizeof(magic)) &&
                     memcmp(magic, HYP_RECORD_VEC_SHIP_MAGIC, sizeof(magic)) == 0 &&
                     rv_read_u32(f, &ship_format);
    if (!header_ok) {
        hyp_record_vec_meta_free(&m);
        fclose(f);
        rv_err(v, "import: not a hyponoia vector shipment");
        return HYP_ASK_VEC_ERR;
    }
    if (ship_format != HYP_RECORD_VEC_SHIP_FORMAT) {
        hyp_record_vec_meta_free(&m);
        fclose(f);
        snprintf(v->errbuf, sizeof(v->errbuf),
                 "import: ship format %u against this binary's %d — refused rather than misread",
                 (unsigned)ship_format, HYP_RECORD_VEC_SHIP_FORMAT);
        return HYP_ASK_VEC_ERR;
    }
    header_ok = rv_read_u32(f, &ship_dim) && rv_read_u32(f, &ship_window) &&
                rv_read_str(f, &ship_model) && rv_read_str(f, &ship_contract) &&
                rv_read_u64(f, &ship_count) && ship_dim > 0 && ship_dim <= 65536;
    if (!header_ok) {
        free(ship_model);
        free(ship_contract);
        hyp_record_vec_meta_free(&m);
        fclose(f);
        rv_err(v, "import: truncated or malformed shipment header");
        return HYP_ASK_VEC_ERR;
    }

    /* THE GATE — the same function the code-span store refuses with. A
     * shipment from a different encoder is refused whole: cross-space cosine
     * is confidently-ranked garbage with no error attached. */
    char why[RV_ERRBUF];
    why[0] = '\0';
    int verdict =
        hyp_ask_stamp_check(m.model_id, m.prefix_contract, m.dim, m.window_tokens, ship_model,
                            ship_contract, (int)ship_dim, (int)ship_window, why, sizeof(why));
    int dim = m.dim;
    hyp_record_vec_meta_free(&m);
    if (verdict != HYP_ASK_VEC_OK) {
        free(ship_model);
        free(ship_contract);
        fclose(f);
        snprintf(v->errbuf, sizeof(v->errbuf), "import refused: %s", why);
        return HYP_ASK_VEC_INCOMPATIBLE;
    }
    free(ship_model);
    free(ship_contract);

    float *vec = malloc((size_t)dim * sizeof(float));
    if (!vec) {
        fclose(f);
        rv_err(v, "import: out of memory");
        return HYP_ASK_VEC_ERR;
    }

    /* Atomic from here: everything lands or nothing does. */
    if (rv_exec(v, "BEGIN IMMEDIATE;") != HYP_ASK_VEC_OK) {
        free(vec);
        fclose(f);
        return HYP_ASK_VEC_ERR;
    }
    bool ok = rv_exec(v, "CREATE TEMP TABLE IF NOT EXISTS rv_local (hash TEXT PRIMARY KEY);"
                         "DELETE FROM rv_local;") == HYP_ASK_VEC_OK;
    sqlite3_stmt *ins_local = NULL;
    sqlite3_stmt *sel_local = NULL;
    sqlite3_stmt *sel_main = NULL;
    sqlite3_stmt *sel_pend = NULL;
    sqlite3_stmt *ins_main = NULL;
    sqlite3_stmt *ins_pend = NULL;
    ok = ok &&
         sqlite3_prepare_v2(v->db, "INSERT OR IGNORE INTO rv_local(hash) VALUES (?1)", -1,
                            &ins_local, NULL) == SQLITE_OK &&
         sqlite3_prepare_v2(v->db, "SELECT 1 FROM rv_local WHERE hash = ?1", -1, &sel_local,
                            NULL) == SQLITE_OK &&
         sqlite3_prepare_v2(v->db, "SELECT 1 FROM record_vectors WHERE content_hash = ?1", -1,
                            &sel_main, NULL) == SQLITE_OK &&
         sqlite3_prepare_v2(v->db, "SELECT 1 FROM record_vec_pending WHERE content_hash = ?1", -1,
                            &sel_pend, NULL) == SQLITE_OK &&
         sqlite3_prepare_v2(v->db,
                            "INSERT INTO record_vectors (content_hash, vector) VALUES (?1, ?2)", -1,
                            &ins_main, NULL) == SQLITE_OK &&
         sqlite3_prepare_v2(v->db,
                            "INSERT INTO record_vec_pending (content_hash, vector, received_at)"
                            " VALUES (?1, ?2, ?3)",
                            -1, &ins_pend, NULL) == SQLITE_OK;
    if (!ok) {
        rv_err_sqlite(v, "import prepare");
    }
    for (int i = 0; ok && i < local_count; i++) {
        if (!local_hashes[i]) {
            continue;
        }
        sqlite3_reset(ins_local);
        sqlite3_clear_bindings(ins_local);
        sqlite3_bind_text(ins_local, 1, local_hashes[i], -1, RV_TRANSIENT);
        if (sqlite3_step(ins_local) != SQLITE_DONE) {
            rv_err_sqlite(v, "import local insert");
            ok = false;
        }
    }

    char now[RV_TIMEBUF];
    rv_iso_now(now, sizeof(now));
    hyp_record_vec_import_report_t rep = {0};
    for (uint64_t i = 0; ok && i < ship_count; i++) {
        char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
        if (!rv_read_exact(f, hash, HYP_ADDR_SPAN_HASH_LEN)) {
            rv_err(v, "import: shipment ends before its own row count — truncated, refused");
            ok = false;
            break;
        }
        hash[HYP_ADDR_SPAN_HASH_LEN] = '\0';
        if (!rv_hash_valid(hash)) {
            rv_err(v, "import: a row's content hash is malformed — refused");
            ok = false;
            break;
        }
        if (!rv_read_floats(f, vec, dim)) {
            rv_err(v, "import: shipment ends inside a vector — truncated, refused");
            ok = false;
            break;
        }
        int bad = -1;
        if (!hyp_ask_vectors_are_unit(vec, 1, dim, &bad)) {
            rv_err(v, "import: a shipped vector is not unit-normalised — it would score "
                      "silently wrong, so the shipment is refused");
            ok = false;
            break;
        }
        bool have = false;
        sqlite3_reset(sel_main);
        sqlite3_clear_bindings(sel_main);
        sqlite3_bind_text(sel_main, 1, hash, -1, RV_TRANSIENT);
        have = sqlite3_step(sel_main) == SQLITE_ROW;
        if (!have) {
            sqlite3_reset(sel_pend);
            sqlite3_clear_bindings(sel_pend);
            sqlite3_bind_text(sel_pend, 1, hash, -1, RV_TRANSIENT);
            have = sqlite3_step(sel_pend) == SQLITE_ROW;
        }
        if (have) {
            rep.already++; /* the union absorbing a duplicate — not an error */
            continue;
        }
        sqlite3_reset(sel_local);
        sqlite3_clear_bindings(sel_local);
        sqlite3_bind_text(sel_local, 1, hash, -1, RV_TRANSIENT);
        bool matches_local = sqlite3_step(sel_local) == SQLITE_ROW;
        sqlite3_stmt *ins = matches_local ? ins_main : ins_pend;
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_text(ins, 1, hash, -1, RV_TRANSIENT);
        sqlite3_bind_blob(ins, 2, vec, (int)((size_t)dim * sizeof(float)), RV_TRANSIENT);
        if (!matches_local) {
            sqlite3_bind_text(ins, 3, now, -1, RV_TRANSIENT);
        }
        if (sqlite3_step(ins) != SQLITE_DONE) {
            rv_err_sqlite(v, "import insert");
            ok = false;
            break;
        }
        if (matches_local) {
            rep.attached++; /* the content-hash join — attribution recomputed */
        } else {
            rep.held++; /* visible pending — kept, never attached */
        }
    }
    if (ins_local) {
        sqlite3_finalize(ins_local);
    }
    if (sel_local) {
        sqlite3_finalize(sel_local);
    }
    if (sel_main) {
        sqlite3_finalize(sel_main);
    }
    if (sel_pend) {
        sqlite3_finalize(sel_pend);
    }
    if (ins_main) {
        sqlite3_finalize(ins_main);
    }
    if (ins_pend) {
        sqlite3_finalize(ins_pend);
    }
    free(vec);
    fclose(f);
    if (!ok) {
        /* ROLLBACK also undoes the temp table; nothing may touch errbuf after
         * the refusal message that is about to be read. */
        (void)rv_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    (void)rv_exec(v, "DROP TABLE IF EXISTS rv_local;");
    if (rv_exec(v, "COMMIT;") != HYP_ASK_VEC_OK) {
        return HYP_ASK_VEC_ERR;
    }
    *out = rep;
    return HYP_ASK_VEC_OK;
}

/* ── The held state ────────────────────────────────────────────── */

int hyp_record_vectors_pending_hashes(hyp_record_vectors_t *v, char ***out, int *out_count) {
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!v || !out || !out_count) {
        rv_err(v, "pending_hashes: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    int64_t n64 = hyp_record_vectors_pending_count(v);
    if (n64 == 0) {
        return HYP_ASK_VEC_OK;
    }
    if (n64 > 1000000) {
        rv_err(v, "pending_hashes: implausibly many held vectors — refused");
        return HYP_ASK_VEC_ERR;
    }
    int cap = (int)n64;
    char **list = calloc((size_t)cap, sizeof(*list));
    if (!list) {
        rv_err(v, "pending_hashes: out of memory");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(v->db,
                           "SELECT content_hash FROM record_vec_pending ORDER BY content_hash", -1,
                           &st, NULL) != SQLITE_OK) {
        rv_err_sqlite(v, "pending prepare");
        free(list);
        return HYP_ASK_VEC_ERR;
    }
    int n = 0;
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        list[n] = rv_strdup((const char *)sqlite3_column_text(st, 0));
        if (!list[n]) {
            sqlite3_finalize(st);
            hyp_record_vectors_hashes_free(list, n);
            rv_err(v, "pending_hashes: out of memory");
            return HYP_ASK_VEC_ERR;
        }
        n++;
    }
    sqlite3_finalize(st);
    *out = list;
    *out_count = n;
    return HYP_ASK_VEC_OK;
}

void hyp_record_vectors_hashes_free(char **hashes, int count) {
    if (!hashes) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(hashes[i]);
    }
    free(hashes);
}

int hyp_record_vectors_claim(hyp_record_vectors_t *v, const hyp_record_t *const *records, int count,
                             int64_t *out_claimed) {
    if (out_claimed) {
        *out_claimed = 0;
    }
    if (!v || (count > 0 && !records)) {
        rv_err(v, "claim: bad arguments");
        return HYP_ASK_VEC_ERR;
    }
    if (count <= 0) {
        return HYP_ASK_VEC_OK;
    }
    if (rv_exec(v, "BEGIN IMMEDIATE;") != HYP_ASK_VEC_OK) {
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_stmt *mv = NULL;
    sqlite3_stmt *del = NULL;
    bool ok = sqlite3_prepare_v2(v->db,
                                 "INSERT INTO record_vectors (content_hash, vector)"
                                 " SELECT content_hash, vector FROM record_vec_pending"
                                 "  WHERE content_hash = ?1"
                                 " ON CONFLICT(content_hash) DO NOTHING",
                                 -1, &mv, NULL) == SQLITE_OK &&
              sqlite3_prepare_v2(v->db, "DELETE FROM record_vec_pending WHERE content_hash = ?1",
                                 -1, &del, NULL) == SQLITE_OK;
    if (!ok) {
        rv_err_sqlite(v, "claim prepare");
    }
    int64_t claimed = 0;
    for (int i = 0; ok && i < count; i++) {
        if (!records[i] || !records[i]->content) {
            rv_err(v, "claim: a record (or its content) is NULL");
            ok = false;
            break;
        }
        /* The join: the hash is recomputed from the record's CONTENT, right
         * here — a claim cannot be spelled against anything else. */
        char hash[HYP_ADDR_SPAN_HASH_LEN + 1];
        hyp_addr_span_hash(records[i]->content, hash);
        sqlite3_reset(mv);
        sqlite3_clear_bindings(mv);
        sqlite3_bind_text(mv, 1, hash, -1, RV_TRANSIENT);
        if (sqlite3_step(mv) != SQLITE_DONE) {
            rv_err_sqlite(v, "claim move");
            ok = false;
            break;
        }
        claimed += sqlite3_changes(v->db);
        sqlite3_reset(del);
        sqlite3_clear_bindings(del);
        sqlite3_bind_text(del, 1, hash, -1, RV_TRANSIENT);
        if (sqlite3_step(del) != SQLITE_DONE) {
            rv_err_sqlite(v, "claim delete");
            ok = false;
            break;
        }
    }
    if (mv) {
        sqlite3_finalize(mv);
    }
    if (del) {
        sqlite3_finalize(del);
    }
    if (!ok) {
        (void)rv_exec(v, "ROLLBACK;");
        return HYP_ASK_VEC_ERR;
    }
    if (rv_exec(v, "COMMIT;") != HYP_ASK_VEC_OK) {
        return HYP_ASK_VEC_ERR;
    }
    if (out_claimed) {
        *out_claimed = claimed;
    }
    return HYP_ASK_VEC_OK;
}
