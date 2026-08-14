/*
 * ask_vectors.h — per-declaration vector storage for the `ask` lane
 * (NEXT-STEPS §2.1, track 4).
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHERE THIS LIVES, AND WHY IT IS ITS OWN FILE
 * ═════════════════════════════════════════════════════════════════════════
 *
 * One SQLite database per project, at <cache>/vectors/<project>.db, beside but
 * NOT INSIDE the graph database at <cache>/<project>.db.
 *
 * §2.1 says "per-declaration float vectors in a SQLite BLOB" and stops there,
 * which is a storage engine, not a schema and not a location. The reference
 * implementation is no help on the question: it writes a numpy .npy plus a JSON
 * sidecar and has no SQLite in this lane at all. So this is a design decision,
 * and it is the one decision in track 4 that is hard to reverse. Four reasons,
 * in the order they should be weighed.
 *
 * 1. A FULL RE-INDEX REPLACES THE GRAPH DATABASE WHOLESALE. The pipeline builds
 *    a staging file and finishes with hyp_rename_replace(stage, final)
 *    (pipeline.c:1830); the forced-full route additionally unlinks the old file
 *    (pipeline.c:1381). Any table living in <project>.db is therefore destroyed
 *    by every full re-index — including a SUCCESSFUL one that changed three
 *    files. The vectors would not be stale: they are keyed to declaration TEXT
 *    and the content hash proves it. Throwing them away would cost 23.5 CPU-
 *    minutes on lld/ELF's 4,117 declarations, and proportionally more on
 *    anything worth indexing, to rebuild bytes that were already correct.
 *
 * 2. THIS TABLE SCALES WITH THE CORPUS WHERE NOTHING ELSE IN THE STORE DOES —
 *    §2.1's own words — and everything that touches <project>.db as a WHOLE
 *    would start paying for it. hyp_store_check_integrity is a full page scan
 *    and runs on the incremental route's health check and at publication (the
 *    code notes 35.5 s on a kernel-scale generation). Artifact export copies
 *    the database with VACUUM INTO and compresses it into .hyponoia/graph.db.zst
 *    for teammates — who would receive 17 MB of float32 that is worthless to
 *    them unless they hold the same model at the same dimension and window. The
 *    brief for this track is that the structural index keeps EXACTLY the speed
 *    it has today; putting a corpus-scaling blob table inside the file every
 *    structural operation touches is the single change most likely to break it.
 *
 * 3. THE GRAPH DATABASE IS NOT WRITTEN BY SQLITE. The production dump path is a
 *    hand-rolled B-tree page writer (internal/hyp/sqlite_writer.c) that
 *    fabricates page 1's sqlite_master from a hardcoded table list with
 *    hand-computed root pages, in an order the file format requires, and the
 *    result is integrity-checked. Adding a table there is a real change to the
 *    hot structural path, for a pass that runs later, separately, and writes
 *    through ordinary SQL anyway.
 *
 * 4. THE PRECEDENT EXISTS. <cache>/_config.db and <cache>/_cross_repo.db are
 *    already separate SQLite files in the same cache directory, and
 *    index_coverage / lsp_surface are already tables held deliberately apart
 *    from the graph tables on the grounds that they are metadata ABOUT the
 *    graph rather than part of it. A vector is metadata about a declaration.
 *
 * What the separation costs, stated plainly:
 *
 *   - NO SQL JOIN. The store's authorizer denies SQLITE_ATTACH on every
 *     connection (store.c:669-673), so a vector row cannot be joined to a node
 *     row in one statement. This costs nothing here: brute-force cosine is a
 *     full scan by construction — §2.1's own reason for having no vector index
 *     — and only the k rows that survive the scan need any node metadata at
 *     all. The scan reads (rowid, vector); the join is a k-row lookup.
 *
 *   - A SECOND FILE CAN DRIFT from the first. That is not a new risk, it is a
 *     newly VISIBLE one. The alternative drifts too, and silently: node ids are
 *     re-minted 1..N by every full re-index, so a vector table keyed on
 *     nodes.id inside the graph file would still be pointing at the wrong
 *     declarations after a rebuild that happened to preserve it. This schema
 *     keys on qualified_name — what the store itself documents as the
 *     cross-generation-stable key — carries node_id only as a per-generation
 *     hint, and records the graph generation it was built against so the lane
 *     can say "my vectors are older than your graph" instead of answering
 *     confidently from a stale matrix.
 *
 *   - DELETION HAS TO BE ARRANGED. Deleting a project must delete its vectors;
 *     RE-INDEXING one must not. Those are different call sites, which is why
 *     hyp_ask_vectors_drop exists and is not called from the pipeline.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHAT A ROW IS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * One row per DECLARATION, not per function. Every node the extractor produced
 * that has a source span is embedded — methods, fields, classes, enums, macros,
 * variables, modules. §2.1 forbids filtering by node kind, and the reason is
 * arithmetic rather than taste: skipping a kind changes the DENOMINATOR, so
 * recall either looks better because the haystack shrank or worse because the
 * gold vanished, and the number stops being comparable to the frozen benchmark.
 * Spans nest — a class span, its methods and the file-level module span are
 * separate rows covering overlapping bytes — and that is the design, not a bug:
 * a question about the ICF class and a question about ICF::run each land on
 * their own row.
 *
 * The embedded text is the VERBATIM SOURCE LINES start_line..end_line joined
 * with '\n'. No signature extraction, no qualified name, no file path, no kind
 * label, no leading doc comment (it falls outside the node's range), no
 * chunking. A different text unit invalidates every recorded number.
 *
 * The text itself is NOT stored here. Hyponoia already records the span and can
 * re-read the file; duplicating 3.4 MB of lld/ELF source into the sidecar the
 * way the reference implementation does would make §2.1's "~17 MB" dishonest.
 * The span IS stored — repo-relative path and both line numbers — so a ranked
 * answer can cite `Writer.cpp:2281-2456` without a round trip to the graph, and
 * can still cite it after a re-index has re-minted every node id.
 */
#ifndef HYP_ASK_VECTORS_H
#define HYP_ASK_VECTORS_H

#include "semantic/ask_embed.h" /* hyp_ask_trunc_state_t, HYP_ASK_DIM */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HYP_ASK_VEC_OK 0
#define HYP_ASK_VEC_ERR (-1)
#define HYP_ASK_VEC_NOT_FOUND (-2)
/* The stored index was built by a different model/dim/window, or in a format
 * this binary does not read. Refused, never coerced. */
#define HYP_ASK_VEC_INCOMPATIBLE (-3)

/* Bumped when the on-disk shape changes. A version that never changes is a
 * version nobody checks, and reading a newer file with older code is how a
 * matrix gets MISINTERPRETED rather than rejected. Refuse on mismatch; do not
 * default on read. */
#define HYP_ASK_VEC_FORMAT 1

/* Content hashes are sha256 of the span text, kept to 32 hex chars — the
 * reference implementation's width. 128 bits of collision resistance against a
 * corpus of millions of declarations, at a quarter of the storage. */
#define HYP_ASK_VEC_HASH_LEN 32

typedef struct hyp_ask_vectors hyp_ask_vectors_t;

/* Which index. The two are SEPARATE FILES with separate provenance and separate
 * staleness, because they are built by different models at different times and
 * are expected to drift apart — indexing daily and re-embedding the escalation
 * lane weekly is the normal case, not an error (NEXT-STEPS §2.10 step 4).
 *
 * The local lane keeps the historical path so an index built before lanes
 * existed is still found; only the escalation lane gets a suffix. */
typedef enum {
    HYP_ASK_LANE_LOCAL = 0,
    HYP_ASK_LANE_ESCALATION = 1,
} hyp_ask_lane_t;

const char *hyp_ask_lane_name(hyp_ask_lane_t lane);

/* Provenance recorded beside the matrix. A vector whose provenance is unknown
 * may not be reused, and two models' vectors are not comparable while nothing
 * downstream can detect the mix — so these are gates, not decoration. */
typedef struct {
    int format;
    char *project;
    char *model_id;
    /* WHICH PREFIX CONTRACT PRODUCED THESE DOCUMENT VECTORS, and therefore what
     * a query must be encoded with to be in the same space (NEXT-STEPS §2.10
     * step 3). A gate, not a disclosure: the contract is applied to the text
     * BEFORE encoding, so changing it changes every vector in the table.
     *
     * Five encoders have now been measured on this corpus and all five wanted a
     * different one — Qwen3 an instruct prefix worth +0.206 MRR@10, pplx
     * destroyed by that same prefix, Jina its own LoRA adapter, Voyage an
     * `input_type`, nano two literal strings from its model card. §2.9 then
     * showed the same model wanting OPPOSITE contracts on two corpora. So the
     * model id alone does not identify the space, and that is why this field
     * exists rather than being inferred from model_id.
     *
     * An index written before this column existed reads "" — meaning "not
     * recorded", which is NOT the same claim as "no contract" and so is never
     * compared. See hyp_ask_vectors_check_compatible. */
    char *prefix_contract;
    int dim;
    int window_tokens;
    /* The graph generation this pass read. Versioned SEPARATELY from the
     * structural index on purpose: it is what lets the vector lane go stale
     * while the structural lane is fresh, AND SAY SO, instead of answering
     * confidently from an older corpus. */
    char *graph_generation;
    /* Which device built this index, as the encoder reported it. Recorded
     * because a 45-minute CPU build and a 3-minute GPU build produce
     * indistinguishable files, and the only way to find out afterwards which
     * one you paid for is to have written it down. Not a reuse gate: track 2
     * measured GPU and CPU vectors agreeing at min cosine 0.9996, so a
     * GPU-built index is searchable from a CPU query encoder and mixing the two
     * devices is safe. Mixing MODELS is not, and that is a separate field. */
    char *device_note;
    /* "drop" or "keep" — what the build did with spans covering a whole file
     * (NEXT-STEPS §2.2 lever 3). Recorded because the two policies index
     * DIFFERENT POPULATIONS, and a recall number measured over one of them is
     * not comparable to one measured over the other. An index written before
     * the policy existed reads "keep", which is what it did.
     *
     * Not a reuse gate and deliberately not one: the excluded rows are keyed by
     * qualified_name like every other, so flipping the policy and re-running
     * converges by pruning and encoding, not by discarding vectors that are
     * still correct. */
    char *whole_file_spans;
    char *built_at;
    int64_t row_count;
    /* THE THIRD STATE. false means the encoder could not say whether anything
     * was truncated — which is NOT the same claim as "nothing was". See
     * hyp_ask_trunc_t. */
    bool truncation_known;
    int64_t truncated_count;
} hyp_ask_vec_meta_t;

/* One declaration's worth of everything the pass writes. */
typedef struct {
    const char *qualified_name;
    int64_t node_id;
    const char *label;
    const char *file_path; /* repo-relative, as the graph stores it */
    int start_line;
    int end_line;
    const char *lang; /* grammar id; the display map is track 5's */
    const char *content_hash;
    bool truncated;
    const float *vector; /* dim floats, unit-normalised */
} hyp_ask_vec_row_t;

/* A search hit. Everything needed to cite the span without touching the graph. */
typedef struct {
    char *qualified_name;
    char *label;
    char *file_path;
    int start_line;
    int end_line;
    char *lang;
    int64_t node_id;
    /* Meaningful ONLY when the index's truncation state is not UNKNOWN. When it
     * is unknown every row reads false, because false is what an unattested
     * flag has to be — and the index says UNKNOWN so nobody may read that false
     * as "this row fitted". Check hyp_ask_vectors_truncation first. */
    bool truncated;
    float score; /* cosine == dot product; both sides are unit vectors */
} hyp_ask_vec_hit_t;

/* ── Lifecycle ─────────────────────────────────────────────────── */

/* Resolve <cache>/vectors/<project>.db. Writes into `buf`; returns false if it
 * would not fit or the cache directory cannot be resolved.
 *
 * The `vectors/` SUBDIRECTORY is not cosmetic. Three separate scanners
 * enumerate every `.db` in <cache> and treat each as a project — hyp_list_indexes
 * and hyp_remove_indexes in cli.c, and the cross-repo cache walk. A sibling
 * file named <project>.vectors.db would be listed as an index and deleted as
 * one. A subdirectory is invisible to all three. */
bool hyp_ask_vectors_path(const char *project, char *buf, size_t bufsz);

/* The same, for a named lane. HYP_ASK_LANE_LOCAL is byte-identical to the call
 * above; the escalation lane lands beside it under a distinct name so the two
 * can be built, disclosed and invalidated independently. */
bool hyp_ask_vectors_path_lane(const char *project, hyp_ask_lane_t lane, char *buf, size_t bufsz);

hyp_ask_vectors_t *hyp_ask_vectors_open_lane(const char *project, hyp_ask_lane_t lane);

/* Open (creating if needed) the vector database for `project`. */
hyp_ask_vectors_t *hyp_ask_vectors_open(const char *project);

/* Open an explicit path — for tests, and for anyone who needs the store
 * somewhere other than the cache. */
hyp_ask_vectors_t *hyp_ask_vectors_open_path(const char *project, const char *db_path);

void hyp_ask_vectors_close(hyp_ask_vectors_t *v);

/* Last error text; never NULL. */
const char *hyp_ask_vectors_error(const hyp_ask_vectors_t *v);

/* Delete the vector database for a project, files and all. Call this when a
 * PROJECT is deleted. Do NOT call it when a project is re-indexed — surviving a
 * re-index is the entire point of the separate file. */
int hyp_ask_vectors_drop(const char *project);

/* ── Provenance ────────────────────────────────────────────────── */

/* Read the recorded provenance. HYP_ASK_VEC_NOT_FOUND when no index has been
 * built yet — which is the ordinary state, not an error, and is what makes the
 * `ask` lane legitimately `unavailable`. */
int hyp_ask_vectors_get_meta(hyp_ask_vectors_t *v, hyp_ask_vec_meta_t *out);
void hyp_ask_vec_meta_free(hyp_ask_vec_meta_t *m);

/* Refuse to serve or extend an index built by a different model, under a
 * different prefix contract, at a different dimension or window, or in an
 * unknown format. Returns HYP_ASK_VEC_OK when the stored index is compatible,
 * HYP_ASK_VEC_NOT_FOUND when there is none yet, HYP_ASK_VEC_INCOMPATIBLE
 * otherwise, and writes an error naming BOTH sides.
 *
 * `prefix_contract` may be NULL or "" to mean "this caller cannot say", in
 * which case it is not compared — an unknown contract must not be allowed to
 * masquerade as a matching one, but neither may it refuse every index written
 * before the field existed. */
int hyp_ask_vectors_check_compatible(hyp_ask_vectors_t *v, const char *model_id,
                                     const char *prefix_contract, int dim, int window_tokens);

/* ── Writing ───────────────────────────────────────────────────── */

/* Begin a build. `wipe_incompatible` true drops every existing row when the
 * stored provenance does not match — which is what a model change requires,
 * because mixed vectors are undetectable downstream. Returns
 * HYP_ASK_VEC_INCOMPATIBLE (and changes nothing) when it does not match and
 * `wipe_incompatible` is false. */
int hyp_ask_vectors_begin_build(hyp_ask_vectors_t *v, const char *model_id,
                                const char *prefix_contract, int dim, int window_tokens,
                                const char *graph_generation, const char *device_note,
                                bool wipe_incompatible);

/* Record which whole-file-span policy this build ran under: "drop" or "keep".
 * Separate from begin_build rather than an argument to it because it is a
 * DISCLOSURE, not a compatibility gate — mixing the two populations in one
 * index is merely stale, where mixing two models is undetectably wrong, and
 * only the second may refuse. */
int hyp_ask_vectors_set_whole_file_spans(hyp_ask_vectors_t *v, const char *policy);

/* Upsert `count` rows in one transaction. Rows are keyed on qualified_name. */
int hyp_ask_vectors_put(hyp_ask_vectors_t *v, const hyp_ask_vec_row_t *rows, int count);

/* Drop rows whose qualified_name is not in `keep` — the declarations that no
 * longer exist. Writes the number removed to `*out_removed`. */
int hyp_ask_vectors_prune(hyp_ask_vectors_t *v, const char *const *keep, int keep_count,
                          int64_t *out_removed);

/* Seal the build: record row_count, the truncation state and built_at.
 * `truncation_known` false records the UNKNOWN state — it is a claim the lane
 * discloses, not an absence to be read as zero. */
int hyp_ask_vectors_finish_build(hyp_ask_vectors_t *v, bool truncation_known);

/* ── Reuse ─────────────────────────────────────────────────────── */

/* The content hash of a row already stored under `qualified_name`, or
 * HYP_ASK_VEC_NOT_FOUND. `out_hash` must hold HYP_ASK_VEC_HASH_LEN + 1 bytes.
 * `out_truncated` receives the stored flag.
 *
 * A row may be reused when its text hash matches AND the index's provenance
 * matches — same model, same dim, same window — which begin_build has already
 * established. A reused row keeps its truncation flag only on the licence that
 * let its vector be reused; if the previous index's truncation state was
 * UNKNOWN it attests nothing and the row is re-probed. */
int hyp_ask_vectors_stored_hash(hyp_ask_vectors_t *v, const char *qualified_name, char *out_hash,
                                bool *out_truncated);

/* Correct the truncation flag on rows whose VECTOR was reused.
 *
 * This exists because of one specific way the three states can be corrupted. A
 * row copied from a previous index carries its previous truncation flag on the
 * licence that let its vector be reused — but that licence covers the VECTOR
 * (same text, same model), not the DISCLOSURE. If the previous index's
 * truncation state was UNKNOWN, its per-row flags attest nothing, and carrying
 * them into a run that CAN report would let an unattested zero be published as
 * an attested one. The reference implementation's rule is to re-probe those
 * rows; this is where the re-probed answer is written back. */
int hyp_ask_vectors_set_truncated_batch(hyp_ask_vectors_t *v, const char *const *qualified_names,
                                        const bool *flags, int count);

/* ── Reading ───────────────────────────────────────────────────── */

int64_t hyp_ask_vectors_count(hyp_ask_vectors_t *v);

/* Exact top-k by cosine. One pass over the matrix and a bounded insertion into
 * a k-sized heap; no approximation anywhere.
 *
 * There is no vector index and there should not be one below ~300k rows.
 * Brute force was measured at 0.094 ms over 3,000 vectors, 12.1 ms over
 * 100,000 and 40.4 ms over 300,000, against a ~50 ms budget derived from a
 * 300 ms end-to-end target of which the query encode alone takes ~248 ms. It
 * crosses the budget at 1,000,000 (125.6 ms). And the exactness is the real
 * argument: brute force returns the TRUE top-k by construction, every ANN index
 * is approximate, and this lane's criteria are recall criteria.
 *
 * `query` must be `dim` floats and unit-normalised; it is checked, because a
 * query normalised with a different convention produces scores that are
 * silently wrong rather than loudly absent. */
int hyp_ask_vectors_search(hyp_ask_vectors_t *v, const float *query, int dim, int k,
                           hyp_ask_vec_hit_t **out, int *out_count);

void hyp_ask_vec_hits_free(hyp_ask_vec_hit_t *hits, int count);

/* ── The truncation counter ────────────────────────────────────── */

/* THREE STATES, and they must not collapse into two.
 *
 * UNKNOWN is not "none of them" — it is *the encoder could not say*. A store
 * that reported an unattested truncation set as empty would mint exactly the
 * kind of confident silence this engine exists to remove: a recall ceiling
 * nobody was told about, indistinguishable from not having one.
 *
 * The distinction is what makes deferring chunking SAFE rather than a gamble.
 * Chunking fixes truncation; truncation currently never happens on the
 * reference corpus; so chunking is not built. The counter is the tripwire that
 * says when that stops being true — and a counter that cannot tell "zero" from
 * "unmeasured" is not a tripwire. */
/* The three states live in semantic/ask_embed.h — one definition, both
 * halves of the lane. */

typedef struct {
    hyp_ask_trunc_state_t state;
    int64_t count;
    int window_tokens;
    /* The largest offender by span line count, when state == SOME. Owned. */
    char *worst_qualified_name;
    char *worst_file_path;
    int worst_start_line;
    int worst_end_line;
} hyp_ask_trunc_t;

/* Read the truncation state. Cheap enough to call on EVERY answer, which is the
 * point: a caveat that appeared only when a truncated row ranked would be
 * silent in exactly the case that hurts — the one where truncation is why it
 * did not rank. */
int hyp_ask_vectors_truncation(hyp_ask_vectors_t *v, hyp_ask_trunc_t *out);
void hyp_ask_trunc_free(hyp_ask_trunc_t *t);

/* Render the state as one line of prose, the way it should reach a user. Always
 * writes something; never writes nothing for the UNKNOWN state. */
void hyp_ask_trunc_describe(const hyp_ask_trunc_t *t, char *buf, size_t bufsz);

#endif /* HYP_ASK_VECTORS_H */
