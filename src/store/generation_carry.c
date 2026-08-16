/*
 * generation_carry.c — the judgement, and the copy that preserves provenance.
 *
 * See generation_carry.h for why this exists. Three structural properties are
 * worth stating where the code is:
 *
 *   1. NO CLOCK. There is no <time.h> in this file and nothing here mints a
 *      value of any kind — no timestamp, no machine id, no sequence number.
 *      A carried row's columns are the previous generation's columns. The one
 *      exception is a node id, which is not durable and cannot be (see the
 *      header), and it is translated rather than invented.
 *   2. THE TABLE SET IS READ FROM THE DATABASE. Nothing here assumes which
 *      tables exist. A table the judgement does not cover stops the publish
 *      and is named.
 *   3. ONE STATEMENT PER TABLE. The copy is a single INSERT ... SELECT inside
 *      one transaction, so a partial carry cannot be published.
 */
#include "store/generation_carry.h"

#include "foundation/constants.h"
#include "store/store.h" /* hyp_store_coverage_shadow_project */

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    GC_MAX_NODE_ID_COLS = 2,
    GC_MAX_COLS = 64,
    GC_NAME_MAX = 128,
    GC_SQL_MAX = HYP_SZ_8K,
    GC_DDL_MAX = HYP_SZ_4K,
};

/* ── The judgement ───────────────────────────────────────────────── */

typedef struct {
    const char *table;
    hyp_table_class_t klass;
    /* Which column names the member that owns the row. Declared rather than
     * assumed: the projects table calls it `name`, everything else calls it
     * `project`, and a project-scoped table whose owning column is absent
     * refuses instead of carrying every row of every member. */
    const char *project_col;
    /* Columns holding a nodes.id of the PREVIOUS generation. */
    const char *node_id_cols[GC_MAX_NODE_ID_COLS];
    const char *why;
} gc_policy_t;

/* Order is the carry order: a table must appear after anything its rows are
 * translated through. nodes therefore precedes edges and node_vectors, and
 * projects precedes nodes. */
static const gc_policy_t GC_POLICY[] = {
    {"projects",
     HYP_TABLE_PROJECT_SCOPED,
     "name",
     {NULL, NULL},
     "one row per indexed member: its name, its root, and the instant THAT "
     "member was indexed. This run writes the rebuilt member's row; another "
     "member's row is the only record it was ever indexed at all, and its "
     "indexed_at must keep the clock of its own run"},
    {"nodes",
     HYP_TABLE_PROJECT_SCOPED,
     "project",
     {NULL, NULL},
     "the graph buffer holds exactly one member's nodes, so every other "
     "member's node disappears unless it is carried. Row ids are renumbered "
     "from 1 by the dump writer, so carried rows take fresh ones"},
    {"edges",
     HYP_TABLE_PROJECT_SCOPED,
     "project",
     {"source_id", "target_id"},
     "as nodes; both endpoints are translated through (project, "
     "qualified_name) because the ids they hold belong to the generation "
     "being replaced"},
    {"node_vectors",
     HYP_TABLE_PROJECT_SCOPED,
     "project",
     {"node_id", NULL},
     "static semantic vectors joined to the graph by node id: regenerated for "
     "the rebuilt member, translated for every other one"},
    {"token_vectors",
     HYP_TABLE_PROJECT_SCOPED,
     "project",
     {NULL, NULL},
     "per-project token vectors whose row id is local to the file"},
    {"file_hashes",
     HYP_TABLE_PROJECT_SCOPED,
     "project",
     {NULL, NULL},
     "the manifest a member's next incremental run diffs against; dropping "
     "another member's manifest silently forces it into a full rebuild"},
    {"lsp_surface",
     HYP_TABLE_PROJECT_SCOPED,
     "project",
     {NULL, NULL},
     "per-file cross-file resolution input, recomputed only for the member "
     "being indexed"},
    {"index_coverage",
     HYP_TABLE_PROJECT_SCOPED,
     "project",
     {NULL, NULL},
     "what a member's own index run could not cover"},
    {"index_coverage_meta",
     HYP_TABLE_PROJECT_SCOPED,
     "project",
     {NULL, NULL},
     "one row per member's coverage persistence, carrying that run's "
     "recorded_at — re-stamping it would claim this run measured a tree it "
     "never read"},
    {"project_summaries",
     HYP_TABLE_DURABLE,
     NULL,
     {NULL, NULL},
     "the ADR: authored, not derived. No tree can regenerate it, and "
     "created_at/updated_at ARE its provenance — the row moves whole so the "
     "publishing clock never reaches them"},
    {"workspace_meta",
     HYP_TABLE_DURABLE,
     NULL,
     {NULL, NULL},
     "which workspace this file is. A binding, derived from no tree, and the "
     "row every member's address space hangs from"},
    {"workspace_repos",
     HYP_TABLE_DURABLE,
     NULL,
     {NULL, NULL},
     "the member registry: slug, canonical root, role. Derived from what was "
     "declared and from the repositories themselves, never from the member "
     "being indexed"},
    {"store_meta",
     HYP_TABLE_DERIVED,
     NULL,
     {NULL, NULL},
     "db_uid is minted per FILE deliberately, so a pagination cursor issued "
     "against the previous generation cannot validate against a graph whose "
     "node ids all moved. Carrying it forward would make stale cursors look "
     "fresh"},
    {"nodes_fts",
     HYP_TABLE_DERIVED,
     NULL,
     {NULL, NULL},
     "an inverted index over nodes, deleted and rebuilt wholesale by the "
     "publish that follows"},
};

static const gc_policy_t *gc_policy_for(const char *table) {
    if (!table) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(GC_POLICY) / sizeof(GC_POLICY[0]); i++) {
        if (strcmp(GC_POLICY[i].table, table) == 0) {
            return &GC_POLICY[i];
        }
    }
    return NULL;
}

hyp_table_class_t hyp_generation_table_class(const char *table) {
    const gc_policy_t *p = gc_policy_for(table);
    return p ? p->klass : HYP_TABLE_UNCLASSIFIED;
}

const char *hyp_generation_table_reason(const char *table) {
    const gc_policy_t *p = gc_policy_for(table);
    return p ? p->why : NULL;
}

const char *hyp_generation_class_name(hyp_table_class_t klass) {
    switch (klass) {
    case HYP_TABLE_DERIVED:
        return "derived";
    case HYP_TABLE_PROJECT_SCOPED:
        return "project_scoped";
    case HYP_TABLE_DURABLE:
        return "durable";
    case HYP_TABLE_UNCLASSIFIED:
    default:
        return "unclassified";
    }
}

/* ── Reading the schema ──────────────────────────────────────────── */

static void gc_fail(char *err, size_t err_sz, const char *fmt, const char *a, const char *b) {
    if (!err || err_sz == 0) {
        return;
    }
    snprintf(err, err_sz, fmt, a ? a : "", b ? b : "");
}

/* Every table in `schema` that is independent state: SQLite's own tables and
 * the shadow tables of a virtual table are excluded, both derived from
 * sqlite_master rather than listed — a shadow is any table whose name is its
 * virtual parent's name plus a suffix, and the parents are exactly the rows
 * whose DDL opens with CREATE VIRTUAL TABLE. */
static const char *gc_table_scan_sql(bool main_schema) {
    return main_schema
               ? "SELECT m.name FROM main.sqlite_master m WHERE m.type = 'table' "
                 "AND m.name NOT GLOB 'sqlite_*' AND NOT EXISTS (SELECT 1 FROM "
                 "main.sqlite_master v WHERE v.type = 'table' AND v.name <> m.name AND "
                 "upper(ltrim(v.sql)) GLOB 'CREATE VIRTUAL TABLE*' AND m.name GLOB (v.name "
                 "|| '_*')) ORDER BY m.name;"
               : "SELECT m.name FROM prev.sqlite_master m WHERE m.type = 'table' "
                 "AND m.name NOT GLOB 'sqlite_*' AND NOT EXISTS (SELECT 1 FROM "
                 "prev.sqlite_master v WHERE v.type = 'table' AND v.name <> m.name AND "
                 "upper(ltrim(v.sql)) GLOB 'CREATE VIRTUAL TABLE*' AND m.name GLOB (v.name "
                 "|| '_*')) ORDER BY m.name;";
}

/* Append every unclassified table name in `schema` to out. Returns the number
 * appended, or HYP_GEN_CARRY_ERR. */
static int gc_collect_unclassified(sqlite3 *db, bool main_schema, char *out, size_t out_sz,
                                   size_t *used) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, gc_table_scan_sql(main_schema), -1, &st, NULL) != SQLITE_OK) {
        return HYP_GEN_CARRY_ERR;
    }
    int found = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        if (!name || hyp_generation_table_class(name) != HYP_TABLE_UNCLASSIFIED) {
            continue;
        }
        found++;
        if (out && used && *used + strlen(name) + 2 < out_sz) {
            int n = snprintf(out + *used, out_sz - *used, "%s\n", name);
            if (n > 0) {
                *used += (size_t)n;
            }
        }
    }
    sqlite3_finalize(st);
    return found;
}

typedef struct {
    char name[GC_NAME_MAX];
    char type[GC_NAME_MAX];
    int pk;
} gc_col_t;

static int gc_columns(sqlite3 *db, const char *schema, const char *table, gc_col_t *out, int max,
                      int *count) {
    char sql[GC_SQL_MAX];
    /* schema and table are compile-time literals from GC_POLICY; nothing
     * user-supplied reaches here, and PRAGMA takes no bound parameter. */
    snprintf(sql, sizeof(sql), "PRAGMA %s.table_info(%s);", schema, table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        return HYP_GEN_CARRY_ERR;
    }
    *count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && *count < max) {
        const char *name = (const char *)sqlite3_column_text(st, 1);
        const char *type = (const char *)sqlite3_column_text(st, 2);
        if (!name) {
            continue;
        }
        snprintf(out[*count].name, sizeof(out[*count].name), "%s", name);
        snprintf(out[*count].type, sizeof(out[*count].type), "%s", type ? type : "");
        out[*count].pk = sqlite3_column_int(st, 5);
        (*count)++;
    }
    sqlite3_finalize(st);
    return HYP_GEN_CARRY_OK;
}

static bool gc_has_column(const gc_col_t *cols, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(cols[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool gc_case_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - ('a' - 'A')) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - ('a' - 'A')) : *b;
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

/* Read a table's own CREATE statement out of the schema it lives in. */
static bool gc_table_ddl(sqlite3 *db, bool main_schema, const char *table, char *out,
                         size_t out_sz) {
    const char *sql = main_schema
                          ? "SELECT sql FROM main.sqlite_master WHERE type='table' AND name=?1;"
                          : "SELECT sql FROM prev.sqlite_master WHERE type='table' AND name=?1;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
    bool got = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *ddl = (const char *)sqlite3_column_text(st, 0);
        if (ddl && strlen(ddl) < out_sz) {
            snprintf(out, out_sz, "%s", ddl);
            got = true;
        }
    }
    sqlite3_finalize(st);
    return got;
}

/* The column SQLite assigns for you when it is omitted: the single INTEGER
 * PRIMARY KEY of a rowid table. Only project-scoped rows give theirs up — a
 * durable row keeps its identity along with everything else. */
static bool gc_is_rowid_alias(const gc_col_t *cols, int count, int idx, const char *ddl) {
    if (cols[idx].pk != 1 || !gc_case_eq(cols[idx].type, "INTEGER")) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (i != idx && cols[i].pk != 0) {
            return false; /* composite key: no alias */
        }
    }
    return strstr(ddl, "WITHOUT ROWID") == NULL;
}

/* ── The copy ────────────────────────────────────────────────────── */

typedef struct {
    char cols[GC_SQL_MAX];
    char exprs[GC_SQL_MAX];
    char joins[GC_SQL_MAX];
    int col_count;
} gc_plan_t;

static void gc_append(char *buf, size_t buf_sz, const char *sep, const char *text) {
    size_t len = strlen(buf);
    if (len > 0 && sep[0]) {
        snprintf(buf + len, buf_sz - len, "%s", sep);
        len = strlen(buf);
    }
    snprintf(buf + len, buf_sz - len, "%s", text);
}

static int gc_build_plan(const gc_policy_t *pol, const gc_col_t *dest, int dest_n,
                         const gc_col_t *prev, int prev_n, const char *dest_ddl, gc_plan_t *plan,
                         char *err, size_t err_sz) {
    memset(plan, 0, sizeof(*plan));
    for (int i = 0; i < dest_n; i++) {
        const char *name = dest[i].name;
        if (!gc_has_column(prev, prev_n, name)) {
            continue; /* a column the previous generation never had */
        }
        int node_ref = HYP_NOT_FOUND;
        for (int k = 0; k < GC_MAX_NODE_ID_COLS; k++) {
            if (pol->node_id_cols[k] && strcmp(pol->node_id_cols[k], name) == 0) {
                node_ref = k;
                break;
            }
        }
        char frag[GC_NAME_MAX * 4];
        if (node_ref >= 0) {
            snprintf(frag, sizeof(frag), "m%d.id", node_ref);
            gc_append(plan->exprs, sizeof(plan->exprs), ", ", frag);
            snprintf(frag, sizeof(frag),
                     " JOIN prev.nodes p%d ON p%d.id = s.%s"
                     " JOIN main.nodes m%d ON m%d.project = p%d.project"
                     " AND m%d.qualified_name = p%d.qualified_name",
                     node_ref, node_ref, name, node_ref, node_ref, node_ref, node_ref, node_ref);
            gc_append(plan->joins, sizeof(plan->joins), "", frag);
        } else if (pol->klass == HYP_TABLE_PROJECT_SCOPED &&
                   gc_is_rowid_alias(dest, dest_n, i, dest_ddl)) {
            continue;
        } else {
            snprintf(frag, sizeof(frag), "s.%s", name);
            gc_append(plan->exprs, sizeof(plan->exprs), ", ", frag);
        }
        gc_append(plan->cols, sizeof(plan->cols), ", ", name);
        plan->col_count++;
    }
    /* A declared node-id column that no longer exists means the judgement has
     * drifted from the schema; guessing past that is how a dangling id gets
     * published. */
    for (int k = 0; k < GC_MAX_NODE_ID_COLS; k++) {
        if (pol->node_id_cols[k] && !gc_has_column(dest, dest_n, pol->node_id_cols[k])) {
            gc_fail(err, err_sz, "table '%s' no longer has node-id column '%s'", pol->table,
                    pol->node_id_cols[k]);
            return HYP_GEN_CARRY_UNCLASSIFIED;
        }
    }
    return HYP_GEN_CARRY_OK;
}

static int gc_carry_table(sqlite3 *db, const gc_policy_t *pol, const char *rebuilt,
                          const char *shadow, char *err, size_t err_sz) {
    char prev_ddl[GC_DDL_MAX];
    if (!gc_table_ddl(db, false, pol->table, prev_ddl, sizeof(prev_ddl))) {
        return HYP_GEN_CARRY_OK; /* absent in the previous generation */
    }
    char dest_ddl[GC_DDL_MAX];
    if (!gc_table_ddl(db, true, pol->table, dest_ddl, sizeof(dest_ddl))) {
        /* The staging file does not have this table yet. Create it from the
         * previous generation's own DDL rather than a copy of it kept here:
         * one definition, and a durable table added elsewhere lands correctly
         * without this file being edited. */
        if (sqlite3_exec(db, prev_ddl, NULL, NULL, NULL) != SQLITE_OK) {
            gc_fail(err, err_sz, "cannot create carried table '%s': %s", pol->table,
                    sqlite3_errmsg(db));
            return HYP_GEN_CARRY_ERR;
        }
        snprintf(dest_ddl, sizeof(dest_ddl), "%s", prev_ddl);
    }

    gc_col_t dest_cols[GC_MAX_COLS];
    gc_col_t prev_cols[GC_MAX_COLS];
    int dest_n = 0;
    int prev_n = 0;
    if (gc_columns(db, "main", pol->table, dest_cols, GC_MAX_COLS, &dest_n) != HYP_GEN_CARRY_OK ||
        gc_columns(db, "prev", pol->table, prev_cols, GC_MAX_COLS, &prev_n) != HYP_GEN_CARRY_OK) {
        gc_fail(err, err_sz, "cannot read columns of '%s': %s", pol->table, sqlite3_errmsg(db));
        return HYP_GEN_CARRY_ERR;
    }

    bool scoped = pol->klass == HYP_TABLE_PROJECT_SCOPED;
    if (scoped && (!pol->project_col || !gc_has_column(prev_cols, prev_n, pol->project_col))) {
        gc_fail(err, err_sz, "table '%s' is judged project-scoped but has no owning column '%s'",
                pol->table, pol->project_col ? pol->project_col : "(none declared)");
        return HYP_GEN_CARRY_UNCLASSIFIED;
    }

    gc_plan_t plan;
    int plan_rc = gc_build_plan(pol, dest_cols, dest_n, prev_cols, prev_n, dest_ddl, &plan, err,
                                err_sz);
    if (plan_rc != HYP_GEN_CARRY_OK) {
        return plan_rc;
    }
    if (plan.col_count == 0) {
        return HYP_GEN_CARRY_OK;
    }

    char sql[GC_SQL_MAX];
    /* OR IGNORE on the project-scoped graph tables and nowhere else: their
     * uniqueness keys ARE the graph's own dedup, so a collision means the two
     * rows are the same edge. A durable row that will not go in is a failure
     * to carry, and must stop the publish rather than vanish quietly. */
    char filter[GC_NAME_MAX * 4] = "";
    if (scoped) {
        /* The rebuilt member owns its own rows AND its derived miss-graph
         * shadow, which the coverage writer rebuilds a few steps later. */
        snprintf(filter, sizeof(filter), " WHERE s.%s <> ?1 AND s.%s <> ?2", pol->project_col,
                 pol->project_col);
    }
    snprintf(sql, sizeof(sql), "INSERT %sINTO main.%s (%s) SELECT %s FROM prev.%s s%s%s;",
             scoped ? "OR IGNORE " : "", pol->table, plan.cols, plan.exprs, pol->table, plan.joins,
             filter);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        gc_fail(err, err_sz, "cannot prepare carry of '%s': %s", pol->table, sqlite3_errmsg(db));
        return HYP_GEN_CARRY_ERR;
    }
    if (scoped) {
        sqlite3_bind_text(st, 1, rebuilt, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, shadow, -1, SQLITE_STATIC);
    }
    int step = sqlite3_step(st);
    sqlite3_finalize(st);
    if (step != SQLITE_DONE) {
        gc_fail(err, err_sz, "cannot carry '%s': %s", pol->table, sqlite3_errmsg(db));
        return HYP_GEN_CARRY_ERR;
    }
    return HYP_GEN_CARRY_OK;
}

/* ── Entry points ────────────────────────────────────────────────── */

static bool gc_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    (void)fclose(f);
    return true;
}

int hyp_generation_unclassified_tables(const char *db_path, char *out, size_t out_sz) {
    if (out && out_sz > 0) {
        out[0] = '\0';
    }
    if (!db_path) {
        return HYP_GEN_CARRY_ERR;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HYP_GEN_CARRY_ERR;
    }
    size_t used = 0;
    int found = gc_collect_unclassified(db, true, out, out_sz, &used);
    sqlite3_close(db);
    return found;
}

int hyp_generation_carry_forward(const char *dest_db_path, const char *prev_db_path,
                                 const char *rebuilt_project, char *err, size_t err_sz) {
    if (err && err_sz > 0) {
        err[0] = '\0';
    }
    if (!dest_db_path || !rebuilt_project) {
        gc_fail(err, err_sz, "carry forward called without a destination or a project", NULL, NULL);
        return HYP_GEN_CARRY_ERR;
    }
    /* No previous generation is not a failure — a first index has nothing to
     * carry. An UNREADABLE one is: "I could not tell" must never publish as
     * "there was nothing there." */
    if (!prev_db_path || !gc_file_exists(prev_db_path)) {
        return HYP_GEN_CARRY_OK;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(dest_db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        gc_fail(err, err_sz, "cannot open the staging generation: %s", sqlite3_errmsg(db), NULL);
        sqlite3_close(db);
        return HYP_GEN_CARRY_ERR;
    }

    int rc = HYP_GEN_CARRY_OK;
    sqlite3_stmt *att = NULL;
    if (sqlite3_prepare_v2(db, "ATTACH DATABASE ?1 AS prev;", -1, &att, NULL) != SQLITE_OK) {
        gc_fail(err, err_sz, "cannot attach the previous generation: %s", sqlite3_errmsg(db), NULL);
        sqlite3_close(db);
        return HYP_GEN_CARRY_ERR;
    }
    sqlite3_bind_text(att, 1, prev_db_path, -1, SQLITE_STATIC);
    bool attached = sqlite3_step(att) == SQLITE_DONE;
    sqlite3_finalize(att);
    if (!attached) {
        gc_fail(err, err_sz, "cannot attach the previous generation: %s", sqlite3_errmsg(db), NULL);
        sqlite3_close(db);
        return HYP_GEN_CARRY_ERR;
    }

    /* The judgement is checked against BOTH files before anything is copied:
     * the previous generation may hold a table this build has never seen, and
     * the staging file may hold one the dump writer added. Either way the
     * publish stops and says which. */
    char unknown[HYP_SZ_1K];
    unknown[0] = '\0';
    size_t used = 0;
    int unknown_prev = gc_collect_unclassified(db, false, unknown, sizeof(unknown), &used);
    int unknown_dest = gc_collect_unclassified(db, true, unknown, sizeof(unknown), &used);
    if (unknown_prev < 0 || unknown_dest < 0) {
        gc_fail(err, err_sz, "cannot read the schema: %s", sqlite3_errmsg(db), NULL);
        rc = HYP_GEN_CARRY_ERR;
    } else if (unknown_prev + unknown_dest > 0) {
        gc_fail(err, err_sz,
                "publication cannot judge whether these tables are derived, so it refuses "
                "rather than guess: %s",
                unknown, NULL);
        rc = HYP_GEN_CARRY_UNCLASSIFIED;
    }

    char shadow[HYP_SZ_512];
    hyp_store_coverage_shadow_project(shadow, sizeof(shadow), rebuilt_project);

    if (rc == HYP_GEN_CARRY_OK &&
        sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        gc_fail(err, err_sz, "cannot begin the carry: %s", sqlite3_errmsg(db), NULL);
        rc = HYP_GEN_CARRY_ERR;
    } else if (rc == HYP_GEN_CARRY_OK) {
        for (size_t i = 0; i < sizeof(GC_POLICY) / sizeof(GC_POLICY[0]); i++) {
            if (GC_POLICY[i].klass == HYP_TABLE_DERIVED) {
                continue;
            }
            rc = gc_carry_table(db, &GC_POLICY[i], rebuilt_project, shadow, err, err_sz);
            if (rc != HYP_GEN_CARRY_OK) {
                break;
            }
        }
        if (rc == HYP_GEN_CARRY_OK) {
            if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
                gc_fail(err, err_sz, "cannot commit the carry: %s", sqlite3_errmsg(db), NULL);
                rc = HYP_GEN_CARRY_ERR;
            }
        } else {
            (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        }
    }

    (void)sqlite3_exec(db, "DETACH DATABASE prev;", NULL, NULL, NULL);
    sqlite3_close(db);
    return rc;
}
