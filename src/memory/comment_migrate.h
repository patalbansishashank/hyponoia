#ifndef HYP_MEMORY_COMMENT_MIGRATE_H
#define HYP_MEMORY_COMMENT_MIGRATE_H

#include "store/record_store.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * comment_migrate.h — comment prose into the decision store.
 *
 * The comment lint refuses history-shaped prose: a date, a commit SHA, a plan
 * section, a note about what the code returned before. The best writing in this
 * tree wears exactly that costume, so the rule cannot be enforced by deletion:
 * the reasoning would be the thing lost, and it is the thing the rule exists to
 * protect. RELOCATE, DO NOT DELETE: the invariant and its reason are rewritten
 * in the present tense where the code is read, and the provenance — which
 * measurement, which incident, which plan section — becomes a record.
 *
 * ── The two ends, and the seam between them ──────────────────────────────────
 *
 * scripts/migrate-comments.py finds the comment blocks and attributes them;
 * this file turns them into records. The split is not arbitrary. Finding a
 * comment must use the comment view the LINT uses, or the migrator and the
 * gate could disagree about what a comment is — and both previous mechanical
 * passes over this tree failed precisely by treating a date inside a string
 * literal as a comment. Building a record must use the record contract, or
 * there would be a second implementation of an id. So each end owns what it
 * alone can be right about, and they meet at a length-prefixed manifest where
 * no value can impersonate a delimiter.
 *
 * ── Attribution comes from git, and that is the whole design ─────────────────
 *
 * Author is the blame author; the timestamp is that commit's time. Neither is
 * ever read from a clock here (I6). The record id commits to both, so a
 * migrator that authored the corpus as itself at its own `now()` would mint
 * fresh ids on every machine and every re-run: the union would hold N copies
 * of one comment, indistinguishable from N different comments, and there is no
 * mutation available to repair it afterwards. Attribution from git is what
 * makes a second machine's run DEDUPLICATE.
 *
 * The same argument settles the anchor, and it is the reason this file takes
 * no code index. An anchor derived from the index would be derived from a
 * per-machine artifact that tracks a working tree — so a machine with an index
 * and a machine without one would produce different anchors for one comment,
 * hence different ids, hence the duplicate storm again, from the other
 * direction. Everything in a migrated record therefore comes from git alone:
 * blob, path, line range, blame. Git is the only thing two machines are
 * guaranteed to hold identically.
 *
 *   anchor   hyp1:<workspace>/<repo>#<module-qn> — the file's module address,
 *            derived from the path by the identity contract, with no span
 *            hash. Absent means no content observation was recorded, which is
 *            exactly true: the migrator observes a comment, not a span.
 *   origin   "comment:" blob ":" path ":" start "-" end. Content-derived and
 *            position-bearing, so re-running at the same commit yields the
 *            same origin, the same id, and a union of one — idempotent by
 *            construction rather than by an "already seen" table. Editing the
 *            file changes the blob, so a rewritten comment becomes a SECOND
 *            record and the first is still there.
 *
 *            WHICH MAKES THE ORDER LOAD-BEARING, and getting it backwards is
 *            silent: a manifest taken AFTER a rewrite holds what survived, and
 *            what the rewrite removed is then in no store at all. Migrate
 *            before rewriting, or take a second manifest at the commit before
 *            the rewrite (`migrate-comments.py --rev`) and ingest both — the
 *            unchanged blocks deduplicate and only the changed ones are added,
 *            so the union is the whole history of the prose either way.
 *   kind     decision. A comment explaining why the code is shaped as it is,
 *            written to be found later, is what the kind means.
 *
 * Line numbers live in `origin` and never in the anchor. Anchor resolution
 * consults symbol identity and content, never lines (anchor.h), so an anchor
 * built out of a line number would smuggle in the one input that contract
 * refuses.
 */

/* ── Statuses ───────────────────────────────────────────────────────────── */

typedef enum {
    HYP_COMMENT_MIGRATE_OK = 0,
    HYP_COMMENT_MIGRATE_ERR_NULL,    /* a required argument was NULL */
    HYP_COMMENT_MIGRATE_ERR_IO,      /* the manifest could not be read */
    HYP_COMMENT_MIGRATE_ERR_FORMAT,  /* not this manifest, or a field out of order */
    HYP_COMMENT_MIGRATE_ERR_FIELD,   /* a field present but unusable */
    HYP_COMMENT_MIGRATE_ERR_ADDRESS, /* no address derives from the path */
    HYP_COMMENT_MIGRATE_ERR_RECORD,  /* the record contract refused the row */
    HYP_COMMENT_MIGRATE_ERR_STORE,   /* the store refused the row */
    HYP_COMMENT_MIGRATE_ERR_ALLOC    /* out of memory */
} hyp_comment_migrate_status_t;

/* Stable, human-facing reason for a status. Never NULL. */
const char *hyp_comment_migrate_status_reason(hyp_comment_migrate_status_t status);

/* ── Running one ────────────────────────────────────────────────────────── */

typedef struct {
    /* The workspace these files belong to, or NULL for a workspace of one —
     * the same parameter, and the same meaning, as anchor resolution takes. */
    const char *workspace;
    /* The project slug: what hyp_project_name_from_path() returns for the
     * repository root. Required, because an address without a repo is not an
     * address. */
    const char *repo;
} hyp_comment_migrate_opts_t;

typedef struct {
    size_t items;    /* blocks read from the manifest */
    size_t appended; /* records the store did not already hold */
    size_t absorbed; /* records already present — the union absorbing a
                      * duplicate, which is what a re-run should be (I8) */
} hyp_comment_migrate_stats_t;

/* Ingest a manifest. ATOMIC ONLY PER RECORD: a refusal stops the run and says
 * which item refused, leaving everything appended so far — appending is a
 * union, so a resumed run re-appends them for free. `err` (optional) receives
 * a message naming the item. */
hyp_comment_migrate_status_t hyp_comment_migrate_run(const char *manifest_path,
                                                     const hyp_comment_migrate_opts_t *opts,
                                                     hyp_record_store_t *store,
                                                     hyp_comment_migrate_stats_t *stats, char *err,
                                                     size_t err_sz);

/* ── The derived forms, exposed so tests assert them ────────────────────── */

/* "comment:" blob ":" path ":" start "-" end. False when the parts are
 * malformed or the result would exceed HYP_RECORD_MAX_ORIGIN — a truncated
 * origin is a well-formed origin naming a different range, which is the class
 * of silent corruption every cap in the record contract refuses. */
bool hyp_comment_migrate_origin(const char *blob, const char *path, int start_line, int end_line,
                                char *out, size_t out_sz);

/* The module address for `path` under `opts`. Out is "" on refusal. */
hyp_comment_migrate_status_t hyp_comment_migrate_anchor(const hyp_comment_migrate_opts_t *opts,
                                                        const char *path, char *out, size_t out_sz);

/* ── The command ────────────────────────────────────────────────────────── */

/* `hyp migrate-comments --manifest FILE --store DIR [--repo SLUG]
 *  [--workspace NAME]`. The repo slug defaults to the current directory's
 * project name. Prints the three counts; a re-run prints them as absorbed,
 * which is the only externally visible proof that the run was idempotent. */
int hyp_cmd_migrate_comments(int argc, char **argv);

#endif /* HYP_MEMORY_COMMENT_MIGRATE_H */
