/*
 * ask_view.h — the 3-D view of the `ask` lane's vector space
 * (NEXT-STEPS §3.1 step 5: "alongside, and gating nothing: see the embeddings").
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHAT THIS IS, AND WHAT IT IS NOT
 * ═════════════════════════════════════════════════════════════════════════
 *
 * A projection D → 3 of every stored declaration vector, fitted once over the
 * whole table and persisted BESIDE the vectors — three floats per row plus
 * the basis that produced them — so that a QUERY vector can later be dropped
 * into the same three-space and drawn next to the declarations that ranked.
 *
 * The picture is a DIAGNOSTIC, not a measurement. D → 3 discards almost all
 * of the space (three of 1,024 axes on the current model); two dots that sit
 * together here may be far apart in the space the ranking used, and the
 * ranking is by cosine in D dimensions, never by anything in this view. Every
 * consumer of these coordinates has to say so on the surface it draws them on
 * — the UI does — and nothing in this module may be used to score, rank, or
 * filter. It exists to let a person LOOK at where a question landed relative
 * to its neighbours, which until now was only ever a statistic.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * METHOD: PCA, BECAUSE IT IS DETERMINISTIC
 * ═════════════════════════════════════════════════════════════════════════
 *
 * The plan allowed UMAP or PCA and required the seed to be pinned, because a
 * re-render that moves everything looks exactly like an index that changed.
 * PCA has no seed: mean-centre, take the top-3 eigenvectors of the covariance.
 * The implementation here is one streaming pass over the table to accumulate
 * the second-moment matrix in double, then subspace iteration with a FIXED
 * deterministic start (the three coordinate axes of largest variance) and a
 * fixed operation order, single-threaded. Same rows in, same order (ORDER BY
 * qualified_name — never physical row order, which an upsert can permute)
 * → the same bits out, on the same binary. Across compilers or FMA settings
 * the coordinates agree to ~1e-6 relative, which is far below anything the
 * picture can show. There is no vendored UMAP and writing a stochastic
 * embedder to then pin its seed buys separation the plan itself calls a
 * liability; the plan's own caveat — PCA gives WEAKER separation than UMAP —
 * is accepted and recorded.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHEN IT IS (RE)FITTED — AND WHY THE WHOLE LAYOUT MOVES WHEN IT IS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * The fit is NOT incremental. Adding rows re-fits the basis and every
 * coordinate moves. It runs
 *
 *   1. at the end of EVERY `hyponoia embed` pass that wrote or pruned rows —
 *      after finish_build sealed the index — over ALL rows in the table, so the
 *      view and the index are sealed together; and
 *   2. on demand, `hyponoia embed --project <p> --fit-view`, which fits the
 *      view over an existing index without encoding anything (the path for an
 *      index built by a binary that predates this file).
 *
 * The record beside the basis carries `fitted_against` = the index's built_at
 * at fit time. A reader compares it to the current built_at and reports the
 * view STALE when they differ, rather than drawing coordinates that belong to
 * an older matrix as if they were current. A row whose vector was rewritten
 * by hyp_ask_vectors_put has its coordinates NULLed in the same statement,
 * so a torn pass leaves gaps that a reader can count, not wrong dots.
 *
 * On-disk shape (in the SAME per-project vector file, ask_vectors.c):
 *
 *   ask_vectors.view_x/view_y/view_z  REAL, NULL until fitted — the row's
 *                                     projection in the vector's own units,
 *                                     unscaled. Consumers scale for display.
 *   ask_view                          one row: method, dim, rows, mean BLOB
 *                                     (dim float32), basis BLOB (3 x dim
 *                                     float32, orthonormal), the three
 *                                     eigenvalues and the trace, fitted_at,
 *                                     fitted_against, fit_ms, iterations.
 */
#ifndef HYP_ASK_VIEW_H
#define HYP_ASK_VIEW_H

#include "ask/ask_vectors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The name a reader checks before trusting the blobs. Bumped if the meaning
 * of `mean`/`basis` or their layout changes; a mismatch is refused, not read. */
#define HYP_ASK_VIEW_METHOD "pca3-v1"
#define HYP_ASK_VIEW_K 3

/* The persisted basis. Owned strings and arrays; free with hyp_ask_view_free. */
typedef struct {
    char *method;
    int dim;
    int64_t rows;                 /* rows the fit saw */
    float *mean;                  /* dim floats */
    float *basis;                 /* HYP_ASK_VIEW_K x dim floats, row-major, orthonormal rows */
    double eigen[HYP_ASK_VIEW_K]; /* variance captured by each axis, descending */
    double total_var;             /* trace of the covariance: what the 3 were taken from */
    char *fitted_at;
    char *fitted_against; /* ask_index.built_at at fit time; "" when the index was unsealed */
    double fit_ms;
    int iterations; /* subspace iterations the fit took to converge */
} hyp_ask_view_t;

/* Fit the view over every row in the store and persist it: writes view_x/y/z
 * on every row and the basis into ask_view, in one transaction. `out` may be
 * NULL; when given it receives the fitted view. Returns HYP_ASK_VEC_OK,
 * HYP_ASK_VEC_NOT_FOUND when the store has no rows (nothing to fit — and
 * nothing is written), or HYP_ASK_VEC_ERR with the store's error text set. */
int hyp_ask_view_fit(hyp_ask_vectors_t *v, hyp_ask_view_t *out);

/* Read the persisted basis. HYP_ASK_VEC_NOT_FOUND when no view has been
 * fitted, HYP_ASK_VEC_INCOMPATIBLE when one exists under a method this binary
 * does not read or with a dim that does not match the index. */
int hyp_ask_view_load(hyp_ask_vectors_t *v, hyp_ask_view_t *out);

void hyp_ask_view_free(hyp_ask_view_t *view);

/* Drop `vec` (view->dim floats) into the view's three-space. Pure. */
void hyp_ask_view_project(const hyp_ask_view_t *view, const float *vec, float out[HYP_ASK_VIEW_K]);

/* True when the index has been re-sealed since the view was fitted — the
 * coordinates on disk belong to an older matrix. Also true when the store's
 * meta cannot be read. */
bool hyp_ask_view_is_stale(hyp_ask_vectors_t *v, const hyp_ask_view_t *view);

/* One projected declaration, for drawing the cloud. */
typedef struct {
    int64_t node_id;
    char *qualified_name;
    char *label;
    char *file_path;
    int start_line;
    int end_line;
    float x, y, z;
} hyp_ask_view_point_t;

/* Every row that HAS coordinates, ORDER BY qualified_name. Rows whose
 * coordinates are NULL (rewritten since the last fit, or never fitted) are
 * counted into *out_unprojected and not returned — a gap the caller can
 * report, not a dot at the origin. */
int hyp_ask_view_points(hyp_ask_vectors_t *v, hyp_ask_view_point_t **out, int *out_count,
                        int64_t *out_unprojected);
void hyp_ask_view_points_free(hyp_ask_view_point_t *points, int count);

/* Coordinates of one row by its key. HYP_ASK_VEC_NOT_FOUND when the row does
 * not exist OR has no coordinates yet; the two are told apart by
 * *out_present when it is non-NULL. */
int hyp_ask_view_lookup(hyp_ask_vectors_t *v, const char *qualified_name, float out[HYP_ASK_VIEW_K],
                        int64_t *out_node_id, bool *out_present);

/* The sentence every surface that draws these coordinates must carry. One
 * definition so the UI, the JSON and the tests cannot drift on the wording. */
#define HYP_ASK_VIEW_CAVEAT                                                                 \
    "Distances in this view are not real: the projection keeps three of the vector's "      \
    "dimensions' worth of variance and discards the rest, and the ranking was computed by " \
    "cosine in the full space, never here. Nearness in the picture is a hint about the "    \
    "neighbourhood, not a measurement of it."

#endif /* HYP_ASK_VIEW_H */
