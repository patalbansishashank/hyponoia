/*
 * ask_view.c — PCA D → 3 over the vector table, persisted beside it.
 *
 * See ask_view.h for what this is for and when it runs. This file is the
 * arithmetic and the storage. The two properties it has to keep:
 *
 *   DETERMINISM. Same rows → same bits, on the same binary. Every sum runs in
 *   a fixed order over rows read ORDER BY qualified_name, the start vectors
 *   are chosen by a rule and not by a generator, and nothing here is threaded.
 *
 *   HONESTY ABOUT WHAT IS ON DISK. A row's coordinates are written by the fit
 *   and NULLed by any later rewrite of its vector (ask_vectors.c's upsert), so
 *   what a reader finds is either the fit's answer or nothing — never a stale
 *   dot for a vector that has moved. The basis carries the index's built_at at
 *   fit time so a reader can say STALE when the index has been re-sealed.
 *
 * Cost, measured (see runs/EMBED-VIEW/verdict.json): the second-moment
 * accumulation is n x d x d / 2 multiply-adds in double — the only part that
 * scales with the corpus — and the eigen-iteration is d x d x block per step,
 * independent of n.
 */

#include "ask/ask_view.h"
#include "foundation/compat.h"
#include "foundation/log.h"

#include <sqlite3.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    AV_VIEW_TIMEBUF = 32,
    /* Subspace iteration runs on a block WIDER than the three axes it keeps:
     * the extra columns absorb the eigenvalues just below the third and speed
     * the convergence of the three that matter. Deterministic either way. */
    AV_VIEW_BLOCK = 8,
    AV_VIEW_MAX_ITER = 500,
};

/* Convergence: the largest change, over the three kept axes, in the cosine
 * between successive iterates. Well below what a picture can show; the loop
 * also has a hard cap so a flat spectrum cannot spin it forever. */
static const double AV_VIEW_TOL = 1e-10;

static char *avw_strdup(const char *s) {
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

static void avw_iso_now(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tm;
    hyp_gmtime_r(&t, &tm);
    (void)strftime(buf, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static double avw_elapsed_ms(struct timespec start) {
    struct timespec now;
    hyp_clock_gettime(CLOCK_MONOTONIC, &now);
    return ((double)(now.tv_sec - start.tv_sec) * 1000.0) +
           ((double)(now.tv_nsec - start.tv_nsec) / 1000000.0);
}

/* ── The eigen step ─────────────────────────────────────────────── */

/* Modified Gram-Schmidt on the columns of Q (d x m, column-major as m arrays
 * of d). A column that collapses to zero — a rank-deficient covariance, e.g.
 * two rows — is replaced by a deterministic unit axis so the basis stays
 * orthonormal and the coordinates along it read 0. */
static void avw_orthonormalise(double **q, int d, int m) {
    for (int k = 0; k < m; k++) {
        for (int j = 0; j < k; j++) {
            double dot = 0.0;
            for (int i = 0; i < d; i++) {
                dot += q[j][i] * q[k][i];
            }
            for (int i = 0; i < d; i++) {
                q[k][i] -= dot * q[j][i];
            }
        }
        double norm = 0.0;
        for (int i = 0; i < d; i++) {
            norm += q[k][i] * q[k][i];
        }
        norm = sqrt(norm);
        if (norm < 1e-300) {
            /* Rank-deficient: pick the first coordinate axis not already
             * (numerically) inside the span, and orthogonalise it too. */
            for (int axis = 0; axis < d; axis++) {
                for (int i = 0; i < d; i++) {
                    q[k][i] = i == axis ? 1.0 : 0.0;
                }
                for (int j = 0; j < k; j++) {
                    double dot = q[j][axis];
                    for (int i = 0; i < d; i++) {
                        q[k][i] -= dot * q[j][i];
                    }
                }
                norm = 0.0;
                for (int i = 0; i < d; i++) {
                    norm += q[k][i] * q[k][i];
                }
                norm = sqrt(norm);
                if (norm > 1e-6) {
                    break;
                }
            }
            if (norm < 1e-300) {
                norm = 1.0; /* d < m; leave the zero column, nothing to project onto */
            }
        }
        for (int i = 0; i < d; i++) {
            q[k][i] /= norm;
        }
    }
}

/* Top-k eigenvectors of the symmetric d x d matrix `cov` (row-major, full)
 * by subspace iteration on a block of `m >= k` columns. Writes the k
 * eigenvectors into `out_basis` (k x d, row-major, orthonormal) and their
 * eigenvalues into `out_eigen`, sorted descending. Returns the iteration
 * count, or -1 on allocation failure. */
static int avw_top_eigen(const double *cov, int d, int k, int m, float *out_basis,
                         double *out_eigen) {
    if (m < k) {
        m = k;
    }
    if (m > d) {
        m = d;
    }
    if (k > d) {
        k = d;
    }
    double **q = calloc((size_t)m, sizeof(*q));
    double **z = calloc((size_t)m, sizeof(*z));
    double *lambda = calloc((size_t)m, sizeof(*lambda));
    int *order = calloc((size_t)m, sizeof(*order));
    if (!q || !z || !lambda || !order) {
        free(q);
        free(z);
        free(lambda);
        free(order);
        return -1;
    }
    bool ok = true;
    for (int c = 0; c < m; c++) {
        q[c] = calloc((size_t)d, sizeof(double));
        z[c] = calloc((size_t)d, sizeof(double));
        if (!q[c] || !z[c]) {
            ok = false;
        }
    }
    int iterations = -1;
    if (ok) {
        /* DETERMINISTIC START: the m coordinate axes of largest variance
         * (largest diagonal entries), ties broken by index. No generator, no
         * seed — the whole reason PCA was chosen over UMAP. */
        for (int c = 0; c < m; c++) {
            int best = -1;
            for (int i = 0; i < d; i++) {
                bool taken = false;
                for (int p = 0; p < c; p++) {
                    if (order[p] == i) {
                        taken = true;
                        break;
                    }
                }
                if (taken) {
                    continue;
                }
                if (best < 0 || cov[(size_t)i * d + i] > cov[(size_t)best * d + best]) {
                    best = i;
                }
            }
            order[c] = best;
            q[c][best] = 1.0;
        }

        iterations = 0;
        for (int it = 0; it < AV_VIEW_MAX_ITER; it++) {
            iterations = it + 1;
            /* Z = C Q, column by column. */
            for (int c = 0; c < m; c++) {
                for (int i = 0; i < d; i++) {
                    const double *row = cov + (size_t)i * d;
                    double acc = 0.0;
                    for (int j = 0; j < d; j++) {
                        acc += row[j] * q[c][j];
                    }
                    z[c][i] = acc;
                }
            }
            /* Rayleigh quotients BEFORE orthonormalising: q is orthonormal, so
             * lambda_c = q_c . (C q_c). */
            for (int c = 0; c < m; c++) {
                double acc = 0.0;
                for (int i = 0; i < d; i++) {
                    acc += q[c][i] * z[c][i];
                }
                lambda[c] = acc;
            }
            avw_orthonormalise(z, d, m);
            /* Convergence over the k kept columns: cosine between successive
             * iterates. Sign flips are allowed (they are the same axis). */
            double worst = 0.0;
            for (int c = 0; c < k; c++) {
                double dot = 0.0;
                for (int i = 0; i < d; i++) {
                    dot += q[c][i] * z[c][i];
                }
                double delta = 1.0 - fabs(dot);
                if (delta > worst) {
                    worst = delta;
                }
            }
            for (int c = 0; c < m; c++) {
                double *t = q[c];
                q[c] = z[c];
                z[c] = t;
            }
            if (worst < AV_VIEW_TOL) {
                break;
            }
        }
        /* Final Rayleigh quotients on the converged basis. */
        for (int c = 0; c < m; c++) {
            for (int i = 0; i < d; i++) {
                const double *row = cov + (size_t)i * d;
                double acc = 0.0;
                for (int j = 0; j < d; j++) {
                    acc += row[j] * q[c][j];
                }
                z[c][i] = acc;
            }
            double acc = 0.0;
            for (int i = 0; i < d; i++) {
                acc += q[c][i] * z[c][i];
            }
            lambda[c] = acc;
        }
        /* Sort the block by eigenvalue, descending, stable on ties. */
        for (int c = 0; c < m; c++) {
            order[c] = c;
        }
        for (int a = 1; a < m; a++) {
            int cur = order[a];
            int b = a - 1;
            while (b >= 0 && lambda[order[b]] < lambda[cur]) {
                order[b + 1] = order[b];
                b--;
            }
            order[b + 1] = cur;
        }
        for (int c = 0; c < k; c++) {
            const double *vec = q[order[c]];
            /* SIGN CONVENTION: the component of largest magnitude is positive.
             * An eigenvector is defined up to sign; fixing it here means two
             * fits over the same rows agree even if the iteration path
             * differed, and a query lands on the same side of the origin. */
            int big = 0;
            for (int i = 1; i < d; i++) {
                if (fabs(vec[i]) > fabs(vec[big])) {
                    big = i;
                }
            }
            double sign = vec[big] < 0.0 ? -1.0 : 1.0;
            for (int i = 0; i < d; i++) {
                out_basis[(size_t)c * d + i] = (float)(sign * vec[i]);
            }
            out_eigen[c] = lambda[order[c]];
        }
    }
    for (int c = 0; c < m; c++) {
        free(q[c]);
        free(z[c]);
    }
    free(q);
    free(z);
    free(lambda);
    free(order);
    return iterations;
}

/* ── Fit ────────────────────────────────────────────────────────── */

typedef struct {
    char *qualified_name;
    float xyz[HYP_ASK_VIEW_K];
} avw_coord_t;

static void avw_set_err(hyp_ask_vectors_t *v, const char *msg) {
    hyp_ask_vectors_set_error(v, msg);
}

int hyp_ask_view_fit(hyp_ask_vectors_t *v, hyp_ask_view_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!v) {
        return HYP_ASK_VEC_ERR;
    }
    sqlite3 *db = hyp_ask_vectors_db(v);
    hyp_ask_vec_meta_t meta;
    int mrc = hyp_ask_vectors_get_meta(v, &meta);
    if (mrc != HYP_ASK_VEC_OK) {
        return mrc; /* NOT_FOUND: no index at all */
    }
    int d = meta.dim;
    char fitted_against[AV_VIEW_TIMEBUF];
    (void)snprintf(fitted_against, sizeof(fitted_against), "%s",
                   meta.built_at ? meta.built_at : "");
    hyp_ask_vec_meta_free(&meta);
    if (d <= 0) {
        avw_set_err(v, "view: the index records no dimension");
        return HYP_ASK_VEC_ERR;
    }

    struct timespec t0;
    hyp_clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Pass 1: second moments and the sum, in double, upper triangle only.
     * Rows in a FIXED order so the floating-point sums are reproducible. */
    double *sm = calloc((size_t)d * (size_t)d, sizeof(double));
    double *sum = calloc((size_t)d, sizeof(double));
    if (!sm || !sum) {
        free(sm);
        free(sum);
        avw_set_err(v, "view: out of memory for the covariance");
        return HYP_ASK_VEC_ERR;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT vector FROM ask_vectors ORDER BY qualified_name", -1, &st,
                           NULL) != SQLITE_OK) {
        free(sm);
        free(sum);
        avw_set_err(v, "view: prepare pass 1");
        return HYP_ASK_VEC_ERR;
    }
    int64_t n = 0;
    size_t want = (size_t)d * sizeof(float);
    int step = 0;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int len = sqlite3_column_bytes(st, 0);
        if (!blob || (size_t)len != want) {
            continue; /* a mis-sized row is skipped here exactly as search skips it */
        }
        const float *x = (const float *)blob;
        for (int i = 0; i < d; i++) {
            double xi = (double)x[i];
            sum[i] += xi;
            double *row = sm + (size_t)i * d;
            for (int j = i; j < d; j++) {
                row[j] += xi * (double)x[j];
            }
        }
        n++;
    }
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) {
        free(sm);
        free(sum);
        avw_set_err(v, "view: scan pass 1");
        return HYP_ASK_VEC_ERR;
    }
    if (n == 0) {
        free(sm);
        free(sum);
        return HYP_ASK_VEC_NOT_FOUND;
    }

    /* Covariance C = S/n - mu mu^T, mirrored to a full matrix. */
    double inv_n = 1.0 / (double)n;
    double *mean_d = sum; /* reuse: sum becomes the mean */
    for (int i = 0; i < d; i++) {
        mean_d[i] *= inv_n;
    }
    double total_var = 0.0;
    for (int i = 0; i < d; i++) {
        for (int j = i; j < d; j++) {
            double c = sm[(size_t)i * d + j] * inv_n - mean_d[i] * mean_d[j];
            sm[(size_t)i * d + j] = c;
            sm[(size_t)j * d + i] = c;
        }
        total_var += sm[(size_t)i * d + i];
    }

    float *basis = calloc((size_t)HYP_ASK_VIEW_K * (size_t)d, sizeof(float));
    float *mean = calloc((size_t)d, sizeof(float));
    double eigen[HYP_ASK_VIEW_K] = {0.0, 0.0, 0.0};
    if (!basis || !mean) {
        free(sm);
        free(sum);
        free(basis);
        free(mean);
        avw_set_err(v, "view: out of memory for the basis");
        return HYP_ASK_VEC_ERR;
    }
    for (int i = 0; i < d; i++) {
        mean[i] = (float)mean_d[i];
    }
    int iterations = avw_top_eigen(sm, d, HYP_ASK_VIEW_K, AV_VIEW_BLOCK, basis, eigen);
    free(sm);
    free(sum);
    if (iterations < 0) {
        free(basis);
        free(mean);
        avw_set_err(v, "view: out of memory in the eigen iteration");
        return HYP_ASK_VEC_ERR;
    }

    hyp_ask_view_t view;
    memset(&view, 0, sizeof(view));
    view.method = avw_strdup(HYP_ASK_VIEW_METHOD);
    view.dim = d;
    view.rows = n;
    view.mean = mean;
    view.basis = basis;
    memcpy(view.eigen, eigen, sizeof(eigen));
    view.total_var = total_var;
    view.iterations = iterations;
    view.fitted_against = avw_strdup(fitted_against);

    /* Pass 2: project every row. Collected first, written after — updating a
     * table while a SELECT cursor is open on it is not something SQLite
     * promises anything about. */
    avw_coord_t *coords = calloc((size_t)n, sizeof(*coords));
    if (!coords || !view.method || !view.fitted_against) {
        free(coords);
        hyp_ask_view_free(&view);
        avw_set_err(v, "view: out of memory for the coordinates");
        return HYP_ASK_VEC_ERR;
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT qualified_name, vector FROM ask_vectors ORDER BY qualified_name",
                           -1, &st, NULL) != SQLITE_OK) {
        free(coords);
        hyp_ask_view_free(&view);
        avw_set_err(v, "view: prepare pass 2");
        return HYP_ASK_VEC_ERR;
    }
    int64_t filled = 0;
    while ((step = sqlite3_step(st)) == SQLITE_ROW && filled < n) {
        const void *blob = sqlite3_column_blob(st, 1);
        int len = sqlite3_column_bytes(st, 1);
        if (!blob || (size_t)len != want) {
            continue;
        }
        coords[filled].qualified_name = avw_strdup((const char *)sqlite3_column_text(st, 0));
        hyp_ask_view_project(&view, (const float *)blob, coords[filled].xyz);
        filled++;
    }
    sqlite3_finalize(st);

    int rc = HYP_ASK_VEC_OK;
    char now[AV_VIEW_TIMEBUF];
    avw_iso_now(now, sizeof(now));
    view.fitted_at = avw_strdup(now);
    view.fit_ms = avw_elapsed_ms(t0);

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        rc = HYP_ASK_VEC_ERR;
        avw_set_err(v, "view: begin");
    }
    sqlite3_stmt *up = NULL;
    if (rc == HYP_ASK_VEC_OK &&
        sqlite3_prepare_v2(db,
                           "UPDATE ask_vectors SET view_x = ?1, view_y = ?2, view_z = ?3"
                           " WHERE qualified_name = ?4",
                           -1, &up, NULL) != SQLITE_OK) {
        rc = HYP_ASK_VEC_ERR;
        avw_set_err(v, "view: prepare update");
    }
    for (int64_t i = 0; rc == HYP_ASK_VEC_OK && i < filled; i++) {
        sqlite3_reset(up);
        sqlite3_clear_bindings(up);
        sqlite3_bind_double(up, 1, (double)coords[i].xyz[0]);
        sqlite3_bind_double(up, 2, (double)coords[i].xyz[1]);
        sqlite3_bind_double(up, 3, (double)coords[i].xyz[2]);
        sqlite3_bind_text(up, 4, coords[i].qualified_name, -1, SQLITE_STATIC);
        if (sqlite3_step(up) != SQLITE_DONE) {
            rc = HYP_ASK_VEC_ERR;
            avw_set_err(v, "view: update step");
        }
    }
    if (up) {
        sqlite3_finalize(up);
    }
    sqlite3_stmt *ins = NULL;
    if (rc == HYP_ASK_VEC_OK &&
        sqlite3_prepare_v2(db,
                           "INSERT INTO ask_view (singleton, method, dim, rows, mean, basis,"
                           "  eigen0, eigen1, eigen2, total_var, fitted_at, fitted_against,"
                           "  fit_ms, iterations)"
                           " VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)"
                           " ON CONFLICT(singleton) DO UPDATE SET"
                           "  method = excluded.method, dim = excluded.dim, rows = excluded.rows,"
                           "  mean = excluded.mean, basis = excluded.basis,"
                           "  eigen0 = excluded.eigen0, eigen1 = excluded.eigen1,"
                           "  eigen2 = excluded.eigen2, total_var = excluded.total_var,"
                           "  fitted_at = excluded.fitted_at,"
                           "  fitted_against = excluded.fitted_against,"
                           "  fit_ms = excluded.fit_ms, iterations = excluded.iterations",
                           -1, &ins, NULL) != SQLITE_OK) {
        rc = HYP_ASK_VEC_ERR;
        avw_set_err(v, "view: prepare basis insert");
    }
    if (rc == HYP_ASK_VEC_OK) {
        sqlite3_bind_text(ins, 1, view.method, -1, SQLITE_STATIC);
        sqlite3_bind_int(ins, 2, d);
        sqlite3_bind_int64(ins, 3, n);
        sqlite3_bind_blob(ins, 4, view.mean, (int)((size_t)d * sizeof(float)), SQLITE_STATIC);
        sqlite3_bind_blob(ins, 5, view.basis,
                          (int)((size_t)HYP_ASK_VIEW_K * (size_t)d * sizeof(float)), SQLITE_STATIC);
        sqlite3_bind_double(ins, 6, view.eigen[0]);
        sqlite3_bind_double(ins, 7, view.eigen[1]);
        sqlite3_bind_double(ins, 8, view.eigen[2]);
        sqlite3_bind_double(ins, 9, view.total_var);
        sqlite3_bind_text(ins, 10, view.fitted_at, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 11, view.fitted_against, -1, SQLITE_STATIC);
        sqlite3_bind_double(ins, 12, view.fit_ms);
        sqlite3_bind_int(ins, 13, view.iterations);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            rc = HYP_ASK_VEC_ERR;
            avw_set_err(v, "view: basis insert step");
        }
    }
    if (ins) {
        sqlite3_finalize(ins);
    }
    if (rc == HYP_ASK_VEC_OK) {
        if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &err) != SQLITE_OK) {
            sqlite3_free(err);
            rc = HYP_ASK_VEC_ERR;
            avw_set_err(v, "view: commit");
        }
    } else {
        (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    for (int64_t i = 0; i < filled; i++) {
        free(coords[i].qualified_name);
    }
    free(coords);

    if (rc == HYP_ASK_VEC_OK) {
        char nb[32];
        char db_[32];
        char ms[32];
        char it[32];
        (void)snprintf(nb, sizeof(nb), "%lld", (long long)n);
        (void)snprintf(db_, sizeof(db_), "%d", d);
        (void)snprintf(ms, sizeof(ms), "%.1f", view.fit_ms);
        (void)snprintf(it, sizeof(it), "%d", view.iterations);
        hyp_log_info("ask.view.fit", "method", HYP_ASK_VIEW_METHOD, "rows", nb, "dim", db_,
                     "fit_ms", ms, "iterations", it);
    }
    if (rc == HYP_ASK_VEC_OK && out) {
        *out = view;
    } else {
        hyp_ask_view_free(&view);
    }
    return rc;
}

/* ── Load / project / stale ─────────────────────────────────────── */

int hyp_ask_view_load(hyp_ask_vectors_t *v, hyp_ask_view_t *out) {
    if (!v || !out) {
        return HYP_ASK_VEC_ERR;
    }
    memset(out, 0, sizeof(*out));
    sqlite3 *db = hyp_ask_vectors_db(v);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT method, dim, rows, mean, basis, eigen0, eigen1, eigen2,"
                           "       total_var, fitted_at, fitted_against, fit_ms, iterations"
                           " FROM ask_view WHERE singleton = 1",
                           -1, &st, NULL) != SQLITE_OK) {
        /* No table: an index a previous binary wrote and this one has not
         * opened for writing. Not fitted, by definition. */
        return HYP_ASK_VEC_NOT_FOUND;
    }
    int rc = HYP_ASK_VEC_NOT_FOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *method = (const char *)sqlite3_column_text(st, 0);
        int d = sqlite3_column_int(st, 1);
        const void *mean_blob = sqlite3_column_blob(st, 3);
        int mean_len = sqlite3_column_bytes(st, 3);
        const void *basis_blob = sqlite3_column_blob(st, 4);
        int basis_len = sqlite3_column_bytes(st, 4);
        if (!method || strcmp(method, HYP_ASK_VIEW_METHOD) != 0 || d <= 0 || !mean_blob ||
            !basis_blob || (size_t)mean_len != (size_t)d * sizeof(float) ||
            (size_t)basis_len != (size_t)HYP_ASK_VIEW_K * (size_t)d * sizeof(float)) {
            hyp_ask_vectors_set_error(v, "view: stored under a method or shape this binary does "
                                         "not read — refused rather than misinterpreted");
            rc = HYP_ASK_VEC_INCOMPATIBLE;
        } else {
            out->method = avw_strdup(method);
            out->dim = d;
            out->rows = sqlite3_column_int64(st, 2);
            out->mean = malloc((size_t)mean_len);
            out->basis = malloc((size_t)basis_len);
            if (out->mean && out->basis && out->method) {
                memcpy(out->mean, mean_blob, (size_t)mean_len);
                memcpy(out->basis, basis_blob, (size_t)basis_len);
                out->eigen[0] = sqlite3_column_double(st, 5);
                out->eigen[1] = sqlite3_column_double(st, 6);
                out->eigen[2] = sqlite3_column_double(st, 7);
                out->total_var = sqlite3_column_double(st, 8);
                out->fitted_at = avw_strdup((const char *)sqlite3_column_text(st, 9));
                out->fitted_against = avw_strdup((const char *)sqlite3_column_text(st, 10));
                out->fit_ms = sqlite3_column_double(st, 11);
                out->iterations = sqlite3_column_int(st, 12);
                rc = HYP_ASK_VEC_OK;
            } else {
                hyp_ask_view_free(out);
                hyp_ask_vectors_set_error(v, "view: out of memory loading the basis");
                rc = HYP_ASK_VEC_ERR;
            }
        }
    }
    sqlite3_finalize(st);
    if (rc == HYP_ASK_VEC_OK) {
        /* The basis must be the index's width, or a query projected with it
         * lands somewhere meaningless. */
        hyp_ask_vec_meta_t meta;
        if (hyp_ask_vectors_get_meta(v, &meta) == HYP_ASK_VEC_OK) {
            if (meta.dim != out->dim) {
                hyp_ask_vectors_set_error(v, "view: basis width does not match the index");
                hyp_ask_view_free(out);
                rc = HYP_ASK_VEC_INCOMPATIBLE;
            }
            hyp_ask_vec_meta_free(&meta);
        }
    }
    return rc;
}

void hyp_ask_view_free(hyp_ask_view_t *view) {
    if (!view) {
        return;
    }
    free(view->method);
    free(view->mean);
    free(view->basis);
    free(view->fitted_at);
    free(view->fitted_against);
    memset(view, 0, sizeof(*view));
}

void hyp_ask_view_project(const hyp_ask_view_t *view, const float *vec, float out[HYP_ASK_VIEW_K]) {
    for (int k = 0; k < HYP_ASK_VIEW_K; k++) {
        out[k] = 0.0F;
    }
    if (!view || !vec || !view->mean || !view->basis || view->dim <= 0) {
        return;
    }
    int d = view->dim;
    for (int k = 0; k < HYP_ASK_VIEW_K; k++) {
        const float *b = view->basis + (size_t)k * d;
        double acc = 0.0;
        for (int i = 0; i < d; i++) {
            acc += ((double)vec[i] - (double)view->mean[i]) * (double)b[i];
        }
        out[k] = (float)acc;
    }
}

bool hyp_ask_view_is_stale(hyp_ask_vectors_t *v, const hyp_ask_view_t *view) {
    if (!v || !view) {
        return true;
    }
    hyp_ask_vec_meta_t meta;
    if (hyp_ask_vectors_get_meta(v, &meta) != HYP_ASK_VEC_OK) {
        return true;
    }
    const char *now = meta.built_at ? meta.built_at : "";
    const char *then = view->fitted_against ? view->fitted_against : "";
    bool stale = strcmp(now, then) != 0;
    hyp_ask_vec_meta_free(&meta);
    return stale;
}

/* ── Points ─────────────────────────────────────────────────────── */

int hyp_ask_view_points(hyp_ask_vectors_t *v, hyp_ask_view_point_t **out, int *out_count,
                        int64_t *out_unprojected) {
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (out_unprojected) {
        *out_unprojected = 0;
    }
    if (!v || !out || !out_count) {
        return HYP_ASK_VEC_ERR;
    }
    sqlite3 *db = hyp_ask_vectors_db(v);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT node_id, qualified_name, label, file_path, start_line,"
                           "       end_line, view_x, view_y, view_z"
                           " FROM ask_vectors ORDER BY qualified_name",
                           -1, &st, NULL) != SQLITE_OK) {
        /* No view columns: this index has never been opened by a binary that
         * knows the view. Nothing projected — not an error. */
        return HYP_ASK_VEC_NOT_FOUND;
    }
    int cap = 1024;
    int n = 0;
    int64_t gaps = 0;
    hyp_ask_view_point_t *pts = calloc((size_t)cap, sizeof(*pts));
    if (!pts) {
        sqlite3_finalize(st);
        return HYP_ASK_VEC_ERR;
    }
    int rc = HYP_ASK_VEC_OK;
    int step = 0;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        if (sqlite3_column_type(st, 6) == SQLITE_NULL ||
            sqlite3_column_type(st, 7) == SQLITE_NULL ||
            sqlite3_column_type(st, 8) == SQLITE_NULL) {
            gaps++;
            continue;
        }
        if (n == cap) {
            int ncap = cap * 2;
            hyp_ask_view_point_t *grown = realloc(pts, (size_t)ncap * sizeof(*pts));
            if (!grown) {
                rc = HYP_ASK_VEC_ERR;
                break;
            }
            pts = grown;
            cap = ncap;
        }
        hyp_ask_view_point_t *p = &pts[n];
        memset(p, 0, sizeof(*p));
        p->node_id = sqlite3_column_int64(st, 0);
        p->qualified_name = avw_strdup((const char *)sqlite3_column_text(st, 1));
        p->label = avw_strdup((const char *)sqlite3_column_text(st, 2));
        p->file_path = avw_strdup((const char *)sqlite3_column_text(st, 3));
        p->start_line = sqlite3_column_int(st, 4);
        p->end_line = sqlite3_column_int(st, 5);
        p->x = (float)sqlite3_column_double(st, 6);
        p->y = (float)sqlite3_column_double(st, 7);
        p->z = (float)sqlite3_column_double(st, 8);
        n++;
    }
    sqlite3_finalize(st);
    if (rc == HYP_ASK_VEC_OK && step != SQLITE_DONE) {
        rc = HYP_ASK_VEC_ERR;
    }
    if (rc != HYP_ASK_VEC_OK) {
        hyp_ask_view_points_free(pts, n);
        return rc;
    }
    *out = pts;
    *out_count = n;
    if (out_unprojected) {
        *out_unprojected = gaps;
    }
    return HYP_ASK_VEC_OK;
}

void hyp_ask_view_points_free(hyp_ask_view_point_t *points, int count) {
    if (!points) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(points[i].qualified_name);
        free(points[i].label);
        free(points[i].file_path);
    }
    free(points);
}

int hyp_ask_view_lookup(hyp_ask_vectors_t *v, const char *qualified_name, float out[HYP_ASK_VIEW_K],
                        int64_t *out_node_id, bool *out_present) {
    if (out_present) {
        *out_present = false;
    }
    if (out_node_id) {
        *out_node_id = 0;
    }
    if (out) {
        out[0] = out[1] = out[2] = 0.0F;
    }
    if (!v || !qualified_name) {
        return HYP_ASK_VEC_ERR;
    }
    sqlite3 *db = hyp_ask_vectors_db(v);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT node_id, view_x, view_y, view_z FROM ask_vectors"
                           " WHERE qualified_name = ?1",
                           -1, &st, NULL) != SQLITE_OK) {
        return HYP_ASK_VEC_NOT_FOUND;
    }
    sqlite3_bind_text(st, 1, qualified_name, -1, SQLITE_STATIC);
    int rc = HYP_ASK_VEC_NOT_FOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (out_present) {
            *out_present = true;
        }
        if (out_node_id) {
            *out_node_id = sqlite3_column_int64(st, 0);
        }
        if (sqlite3_column_type(st, 1) != SQLITE_NULL &&
            sqlite3_column_type(st, 2) != SQLITE_NULL &&
            sqlite3_column_type(st, 3) != SQLITE_NULL) {
            if (out) {
                out[0] = (float)sqlite3_column_double(st, 1);
                out[1] = (float)sqlite3_column_double(st, 2);
                out[2] = (float)sqlite3_column_double(st, 3);
            }
            rc = HYP_ASK_VEC_OK;
        }
    }
    sqlite3_finalize(st);
    return rc;
}
