#ifndef HYP_STORE_GENERATION_CARRY_H
#define HYP_STORE_GENERATION_CARRY_H

#include <stddef.h>

/*
 * generation_carry.h — what must survive a generation boundary, and why the
 * set is derived instead of remembered.
 *
 * Publication builds a FRESH database from ONE member's graph buffer and
 * renames it over the destination. That makes one question decide the fate of
 * every row in the store:
 *
 *     CAN THE GRAPH BUFFER REGENERATE THIS?
 *
 * Everything it can regenerate is safe by construction — it is regenerated.
 * Everything else is durable state that this module carries across the
 * boundary, or it is gone. The loss is silent when it happens: the new file is
 * internally consistent, passes integrity, and simply does not contain the
 * workspace registry, the other members' graphs, or an authored decision
 * record any more.
 *
 * Two instances of exactly that were found independently, from opposite
 * directions, which is why the general rule is written here rather than a
 * patch at each site:
 *
 *   - the workspace registry (workspace_meta / workspace_repos) is written by
 *     the binder and erased by the next full index of any member, so "all
 *     members share one database" was true of no code path;
 *   - the ADR row was carried forward as CONTENT ONLY and re-inserted, which
 *     re-stamped its timestamps with the index run's clock. Timestamps are
 *     caller-supplied precisely so ids cannot become machine-dependent; a
 *     publish path that supplies one turns a reindex into a second decision
 *     record, and an append-only union makes that permanent.
 *
 * So the rule this module enforces is: ANYTHING DURABLE THAT IS NOT DERIVED
 * FROM THE TREE MUST SURVIVE PUBLICATION, AND THE COPY MUST PRESERVE ITS
 * PROVENANCE RATHER THAN RE-CAPTURE IT. Carried rows keep their own columns —
 * their own timestamps, their own identity — because the copy is a row copy,
 * not a re-write through a capturing writer.
 *
 * ═══════════════════════════════════════════════════════════════════════
 * DERIVED, NEVER ENUMERATED
 * ═══════════════════════════════════════════════════════════════════════
 *
 * The SET of tables is read from the database itself (sqlite_master) on every
 * run. Only the JUDGEMENT — regenerable or not — is declared, and a table
 * whose judgement is missing REFUSES THE PUBLISH, naming the table. A
 * hand-written list of tables-to-preserve is what has already failed twice; a
 * derived set with a fail-closed judgement cannot silently omit its newest
 * entry, because the newest entry is exactly what it cannot classify.
 *
 * The companion assertion is hyp_generation_unclassified_tables(): point it at
 * a real published database and it names every table publication could not
 * judge. Add a durable table and forget to teach publication about it, and
 * that test fails naming your table.
 *
 * ═══════════════════════════════════════════════════════════════════════
 * NODE IDS ARE NOT DURABLE, AND THAT IS DELIBERATE
 * ═══════════════════════════════════════════════════════════════════════
 *
 * The dump writer renumbers nodes from 1 on every generation, and the store
 * mints a fresh db_uid per file so a pagination cursor cannot validate against
 * a rebuilt graph. A carried row that holds a previous generation's node id
 * therefore cannot hold it verbatim. The durable identity of a node is
 * (project, qualified_name), so such a column is translated through that pair,
 * and a row whose referent did not survive is DROPPED — a dangling reference
 * is not a preserved one.
 */

/* ── The judgement ───────────────────────────────────────────────── */

typedef enum {
    /* Publication cannot say. Refuses, naming the table. */
    HYP_TABLE_UNCLASSIFIED = 0,
    /* Rebuilt wholesale by the publish that follows. Nothing is carried, and
     * carrying it would be wrong — a stale row would outlive its input. */
    HYP_TABLE_DERIVED = 1,
    /* Regenerated for the member THIS run indexed; every other member's rows
     * are the only copy in existence. Carried where project <> the rebuilt
     * one. This class is what makes a workspace of many members possible at
     * all: without it, indexing member 2 erases member 1. */
    HYP_TABLE_PROJECT_SCOPED = 2,
    /* Derived from no tree at all — a binding, or something authored. Carried
     * verbatim: every row, every column, identity included. */
    HYP_TABLE_DURABLE = 3,
} hyp_table_class_t;

/* The one classification. UNCLASSIFIED for any table not judged. */
hyp_table_class_t hyp_generation_table_class(const char *table);

/* Stable lowercase name of a class, for messages and for tests that must not
 * key on an enum's numeric value. Never NULL. */
const char *hyp_generation_class_name(hyp_table_class_t klass);

/* Why the table was judged that way, in one sentence. NULL for an
 * unclassified table — absent means look elsewhere; there is no reason to
 * report because there is no judgement. */
const char *hyp_generation_table_reason(const char *table);

/* ── Refusals ────────────────────────────────────────────────────── */

enum {
    HYP_GEN_CARRY_OK = 0,
    /* SQLite or IO failure; err carries the engine's message. */
    HYP_GEN_CARRY_ERR = -1,
    /* A table publication cannot judge. err NAMES it. Fail closed: guessing
     * "derived" silently deletes durable state and guessing "durable" can
     * resurrect a row whose input is gone. */
    HYP_GEN_CARRY_UNCLASSIFIED = -2,
};

/* ── The carry ───────────────────────────────────────────────────── */

/*
 * Carry every non-derived row of prev_db_path into the freshly dumped
 * dest_db_path, then leave dest closed and consistent.
 *
 *   dest_db_path     the staging database the graph buffer was just dumped
 *                    into. Must not be open elsewhere.
 *   prev_db_path     the generation being replaced. ABSENT IS NOT AN ERROR —
 *                    a first index has nothing to carry. Unreadable IS an
 *                    error: "I could not tell" must never publish as "there
 *                    was nothing there."
 *   rebuilt_project  the member this generation rebuilt. Its project-scoped
 *                    rows come from the dump; everyone else's are carried.
 *   err              optional; the refusal, naming the table where one is
 *                    named.
 */
int hyp_generation_carry_forward(const char *dest_db_path, const char *prev_db_path,
                                 const char *rebuilt_project, char *err, size_t err_sz);

/*
 * Every user table in db_path that publication cannot judge, newline
 * separated in out. Returns the count, or a negative HYP_GEN_CARRY_* on
 * failure to read the database at all.
 *
 * Internal SQLite tables and the shadow tables of a virtual table are not
 * independent state and are skipped — both derived from the schema, not
 * listed: a shadow is any table whose name is its virtual parent's name plus
 * a suffix, and the virtual parents come from sqlite_master too.
 */
int hyp_generation_unclassified_tables(const char *db_path, char *out, size_t out_sz);

#endif /* HYP_STORE_GENERATION_CARRY_H */
