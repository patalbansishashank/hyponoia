/*
 * record_vectors.h — vectors for RECORD content, keyed on the content hash
 * alone.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHY THE KEY IS A HASH OF `content` AND NEVER THE RECORD ID
 * ═════════════════════════════════════════════════════════════════════════
 *
 * A record's id is a digest of EVERY field — author, timestamp, origin,
 * thread, anchor (record.h). Identical text observed in two sessions is two
 * records with two ids, and an embedding is a function of the TEXT only. Key
 * vectors on the id and every machine embeds the same bytes once per
 * observation — exactly the N× cost this store exists to delete. So the key
 * is hyp_addr_span_hash(content): one hash for the whole engine (invariant
 * I1 — hyp_ask_content_hash forwards to it, widths _Static_asserted equal),
 * and the ONLY public writer takes a record and reads its `content` field, so
 * keying on anything else is unspellable at the call site, not merely
 * discouraged.
 *
 * The same choice is what makes shipped vectors ADDRESS-FREE BY CONSTRUCTION:
 * a row is (content_hash, vector) and nothing else, the ship file carries
 * rows plus the encoder stamp, and there is no field an author, an origin, an
 * anchor or a timestamp could travel in. The receiver recomputes attribution
 * by hashing its own records' content — a join, performed locally, against
 * facts it already holds.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHY THIS IS A SIBLING STORE AND NOT A TABLE IN ask_vectors' FILE
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Same schema FAMILY, different file, and the difference is scope:
 *
 *   1. <cache>/vectors/<project>.db is PER-PROJECT and tracks one project's
 *      code corpus — graph_generation, whole_file_spans, prune-by-QN are all
 *      claims about a corpus. Records are global memory: no project, no
 *      generation, no qualified name. Housing their vectors in a per-project
 *      file either duplicates them per project (the N× again, one level
 *      down) or nominates one project's file as "the" memory store, which
 *      misstates its scope to every tool that resolves paths.
 *   2. hyp_ask_vectors_drop() deletes a project's vectors when the PROJECT
 *      is deleted. Record vectors must survive that: the records they serve
 *      outlive any project.
 *   3. The code-span table is keyed on qualified_name for documented reasons
 *      (ask_vectors.h — QN is the cross-generation-stable key for spans).
 *      Records have no QN; their stable key IS the content hash. One table
 *      with two key disciplines needs a type column and nullable keys — the
 *      shape the plan warns turns a header about one thing into a header the
 *      next fact gets added beside.
 *
 * What is SHARED is the part that must not fork: the encoder stamp
 * (model_id, prefix_contract, dim, window_tokens) and its gate. The gate is
 * literally the same function — hyp_ask_stamp_check, also called by
 * hyp_ask_vectors_check_compatible — so the two stores cannot drift in what
 * they refuse. Invariant I3 holds by construction: the stamp lives on this
 * store's singleton row, and no address and no record ever carries it.
 *
 * The default location is <cache>/memory/vectors.db — its own subdirectory,
 * for the same reason ask_vectors uses vectors/: three scanners enumerate
 * every *.db in <cache> as a project. It is NOT under vectors/ because that
 * directory's namespace is "<project>.db" and a fixed name there would
 * collide with a project of the same name.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * IMPORT: REFUSE LOUDLY, OR HOLD VISIBLY — NEVER MIX, NEVER DROP
 * ═════════════════════════════════════════════════════════════════════════
 *
 * A shipment whose stamp differs from this store's is REFUSED whole:
 * cross-space cosine returns confidently-ranked garbage with no error, so
 * the only safe mix is no mix. A store with no stamp yet also refuses —
 * "could not verify" must not act like "verified".
 *
 * A shipped vector whose hash matches no local record content is HELD in a
 * visible pending state: enumerable, counted, never silently dropped
 * (partial sync means the record may simply not have arrived yet — absent
 * means "look elsewhere", record.h) and never attached (attaching would
 * assert a join that has not happened). When the record arrives, claim()
 * performs the join and promotes the vector.
 */
#ifndef HYP_ASK_RECORD_VECTORS_H
#define HYP_ASK_RECORD_VECTORS_H

#include "ask/ask_vectors.h"   /* HYP_ASK_VEC_* status codes, the stamp gate */
#include "foundation/record.h" /* hyp_record_t — the only shape the writer takes */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bumped when the on-disk SCHEMA changes. Read-refused on mismatch, never
 * defaulted — the same discipline as HYP_ASK_VEC_FORMAT. */
#define HYP_RECORD_VEC_FORMAT 1

/* The ship file: magic and format version. Bumping the format is a new file
 * dialect; an older binary refuses it rather than misreading it. */
#define HYP_RECORD_VEC_SHIP_MAGIC "hyprvec1"
#define HYP_RECORD_VEC_SHIP_FORMAT 1

typedef struct hyp_record_vectors hyp_record_vectors_t;

/* ── Lifecycle ─────────────────────────────────────────────────── */

/* Resolve <cache>/memory/vectors.db. False when it does not fit or no cache
 * directory can be resolved. */
bool hyp_record_vectors_default_path(char *buf, size_t bufsz);

/* Open (creating if needed) at the default path, or an explicit one. */
hyp_record_vectors_t *hyp_record_vectors_open(void);
hyp_record_vectors_t *hyp_record_vectors_open_path(const char *db_path);

void hyp_record_vectors_close(hyp_record_vectors_t *v);

/* Last error text; never NULL. */
const char *hyp_record_vectors_error(const hyp_record_vectors_t *v);

/* ── The stamp ─────────────────────────────────────────────────── */

/* Provenance read-back. Counts are computed from the rows, so the disclosure
 * and the enumeration can never disagree. */
typedef struct {
    int format;
    char *model_id;
    char *prefix_contract;
    int dim;
    int window_tokens;
    int64_t row_count;     /* attached vectors */
    int64_t pending_count; /* held vectors — visible, never attached */
} hyp_record_vec_meta_t;

/* HYP_RECORD_VEC_* status is HYP_ASK_VEC_*: OK / ERR / NOT_FOUND /
 * INCOMPATIBLE, same meanings, same vocabulary as the code-span store. */
int hyp_record_vectors_get_meta(hyp_record_vectors_t *v, hyp_record_vec_meta_t *out);
void hyp_record_vec_meta_free(hyp_record_vec_meta_t *m);

/* Stamp the store with the encoder this machine runs. Refuses
 * (HYP_ASK_VEC_INCOMPATIBLE, changing nothing) when a different stamp is
 * already recorded and `wipe_incompatible` is false; with it true, every
 * attached AND pending row is deleted first, because mixed spaces are
 * undetectable downstream. The gate is hyp_ask_stamp_check — the same
 * function the code-span store refuses with. */
int hyp_record_vectors_begin(hyp_record_vectors_t *v, const char *model_id,
                             const char *prefix_contract, int dim, int window_tokens,
                             bool wipe_incompatible);

/* ── Writing: one embed job per distinct content ───────────────── */

/* Which of these records still need an embed job. Writes into `out_indices`
 * (caller-allocated, `count` slots) the index of the FIRST record carrying
 * each distinct `content` that has no attached vector yet; `*out_job_count`
 * receives how many. N records sharing content therefore yield ONE job, and
 * content already embedded yields none. Reads `content` and nothing else —
 * ids, authors and timestamps cannot reach the key from here. */
int hyp_record_vectors_plan(hyp_record_vectors_t *v, const hyp_record_t *const *records, int count,
                            int *out_indices, int *out_job_count);

/* Store `vector` (stamp-dim floats, unit-normalised — checked, refused
 * otherwise) for `rec`'s content. The key is hyp_addr_span_hash(rec->content),
 * computed HERE: the caller cannot supply a key, so the commonest way to
 * burn N× VRAM — keying on the id — is unspellable. Re-putting the same
 * content upserts the one row; a pending vector for the same content is
 * superseded (the content is now locally embedded and attached). Requires a
 * stamp; refuses before it. */
int hyp_record_vectors_put(hyp_record_vectors_t *v, const hyp_record_t *rec, const float *vector);

/* ── Reading: attribution is the content-hash join ─────────────── */

int64_t hyp_record_vectors_count(hyp_record_vectors_t *v);

/* The attached vector for `rec`'s content, or HYP_ASK_VEC_NOT_FOUND. `out`
 * must hold `dim` floats and `dim` must equal the stamp's — refused
 * otherwise. A vector held in pending is NOT returned: held is not
 * attached. */
int hyp_record_vectors_get(hyp_record_vectors_t *v, const hyp_record_t *rec, float *out, int dim);

/* ── Shipping ──────────────────────────────────────────────────── */

/* Write every ATTACHED row to `file_path`: a header carrying the magic, the
 * ship format and the stamp, then (content_hash, vector) rows in canonical
 * order (ascending hash — two exports of equal stores are byte-identical).
 * No ids, no authors, no origins, no threads, no anchors, no timestamps:
 * there is no field in the format that could carry one. Integers and floats
 * are little-endian, written explicitly. Requires a stamp. */
int hyp_record_vectors_export(hyp_record_vectors_t *v, const char *file_path,
                              int64_t *out_exported);

typedef struct {
    int64_t attached; /* hash matched local record content — joined */
    int64_t held;     /* matched nothing local — visible pending, kept */
    int64_t already;  /* already present (attached or pending) — the union
                       * absorbing a duplicate, not an error */
} hyp_record_vec_import_report_t;

/* Import a shipment. ATOMIC: everything is validated before anything lands,
 * so a refusal leaves the store exactly as it was.
 *
 * REFUSED (HYP_ASK_VEC_INCOMPATIBLE, nothing imported) when the file's stamp
 * differs from this store's on model, contract, dim or window — and when
 * this store has NO stamp yet, because "could not verify the space" must
 * refuse rather than adopt the shipper's word for it. HYP_ASK_VEC_ERR for a
 * file that is not a shipment, a newer ship format, or a row that is not
 * unit-normalised.
 *
 * `local_hashes` is the receiver's view of its own records — content hashes
 * (hyp_addr_span_hash over each record's content), `local_count` of them. A
 * shipped vector whose hash appears there is attached; one whose hash does
 * not is HELD: inserted into the pending state, counted in `held`,
 * enumerable via hyp_record_vectors_pending_hashes — never dropped, never
 * attached. */
int hyp_record_vectors_import(hyp_record_vectors_t *v, const char *file_path,
                              const char *const *local_hashes, int local_count,
                              hyp_record_vec_import_report_t *out);

/* ── The held state — visible, queryable, claimable ────────────── */

int64_t hyp_record_vectors_pending_count(hyp_record_vectors_t *v);

/* Every held hash, canonical order, owned by the caller. `*out_count` 0 and
 * `*out` NULL when nothing is held. */
int hyp_record_vectors_pending_hashes(hyp_record_vectors_t *v, char ***out, int *out_count);
void hyp_record_vectors_hashes_free(char **hashes, int count);

/* The join, run later: for each record whose content hash matches a held
 * vector, promote it to attached. This is the ONLY path from held to
 * attached, and it takes records — the hash is recomputed from `content`
 * here, so a claim cannot be spelled against anything but real local
 * content. `*out_claimed` (optional) receives how many moved. */
int hyp_record_vectors_claim(hyp_record_vectors_t *v, const hyp_record_t *const *records, int count,
                             int64_t *out_claimed);

#endif /* HYP_ASK_RECORD_VECTORS_H */
