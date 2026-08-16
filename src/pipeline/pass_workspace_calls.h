#ifndef HYP_PIPELINE_PASS_WORKSPACE_CALLS_H
#define HYP_PIPELINE_PASS_WORKSPACE_CALLS_H

#include <stdbool.h>
#include <stddef.h>

#include "store/store.h"

/*
 * pass_workspace_calls.h — the PLUGIN CASE: a direct source-level call from one
 * workspace member into another. Built on the workspace registry in store.h and
 * the address contract in identity.h.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHY THIS IS NOT pass_cross_repo, AND WHY THE EDGE TYPE IS NEW
 * ═════════════════════════════════════════════════════════════════════════
 *
 * pass_cross_repo.c matches SERVICE boundaries — CROSS_HTTP_CALLS,
 * CROSS_GRPC_CALLS, CROSS_GRAPHQL_CALLS, CROSS_TRPC_CALLS, CROSS_ASYNC_CALLS,
 * CROSS_CHANNEL. All six share three properties this edge has none of:
 *
 *   1. A TRANSPORT. Every one of them carries a URL path, a topic, a channel
 *      name, an RPC service/method or a GraphQL operation, and the match IS
 *      that string. A direct call has no such string: the callee's body runs in
 *      the caller's process, on the caller's stack. There is nothing to name.
 *   2. TWO DATABASES. Those six open the OTHER project's <cache>/<slug>.db and
 *      rendezvous on a flat synthetic qualified name, because the store's
 *      authorizer denies SQLITE_ATTACH and there is therefore no join key.
 *      Each match is written as two half-edges, one per file, so the link is
 *      visible from either side.
 *   3. AN ASYNCHRONOUS OR REMOTE CALLEE. A handler on the far side of HTTP may
 *      not run at all; a channel listener may not exist yet.
 *
 * Inside one workspace all members live in ONE database (A1), so this is an
 * ORDINARY EDGE ROW: source_id and target_id are both real node ids, they
 * simply belong to different `project` values. That is why ONE row suffices —
 * the reverse half-edge pass_cross_repo writes exists only because its two ends
 * live in different files and neither could see the other. Here `edges` joins
 * `nodes` in both directions already.
 *
 * So the edge is CROSS_MEMBER_CALLS: a call, in the compiler's sense, that
 * crosses a repository boundary inside one workspace. Reading it as an
 * HTTP call would be wrong in every one of the three ways above.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE EVIDENCE HAD TO BE CREATED — IT DID NOT EXIST
 * ═════════════════════════════════════════════════════════════════════════
 *
 * A cross-member reference left NO trace in the graph. Both call-resolution
 * paths drop a call whose registry resolution comes back empty
 * (pass_calls.c resolve_single_call returns 0; pass_parallel.c
 * resolve_file_calls continues — the same condition, one site each), and both
 * import paths drop an import whose specifier resolves to no node in the
 * project (pass_definitions.c create_import_edges_for_file,
 * pass_parallel.c register_and_link_def). A host repository that includes a
 * plugin's header and calls its entry point produced zero rows mentioning
 * either. So the two dropped facts are now RECORDED, and this pass resolves
 * them afterwards.
 *
 * They are recorded as WORKSPACE-SCOPED RENDEZVOUS NODES, which is C1's I2
 * mechanism used for exactly what it was designed for: a name that belongs to
 * no single repository. Both are FLAT — never prefixed with the project slug —
 * because hyp_addr_qn_scope() applies its project-rooted test BEFORE its tag
 * test, so a rooted name is repo-scoped whatever tag it carries. Rooting these
 * by accident would make them invisible from the other member and this pass
 * would match nothing.
 *
 *   __extern__<callee>      ExternalSymbol   caller -> it, UNRESOLVED_CALLS
 *   __include__<specifier>  ExternalModule   file   -> it, UNRESOLVED_IMPORTS
 *
 * ── THE SPECIFIER KEY IS NORMALISED, AND THAT IS A DECISION ──────────────
 *
 * `#include "./plugin_api.h"` and `#include "plugin_api.h"` name one file.
 * Unresolved relative specifiers reach these passes RAW, so keying on the raw
 * string would mint two nodes for one file and rendezvous with neither. The key
 * is therefore hyp_workspace_specifier_key(): backslashes to forward slashes,
 * leading "./" and "../" segments removed, repeated slashes collapsed. The
 * removed prefix is precisely the part that cannot survive the crossing — a
 * File node's path is relative to ITS OWN member's root, so the caller-relative
 * prefix is meaningless on the far side.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * TWO INDEPENDENT SIGNALS, AND NAME EQUALITY IS NOT ONE OF THEM
 * ═════════════════════════════════════════════════════════════════════════
 *
 * An edge is written only when BOTH of these name the SAME member:
 *
 *   (a) THE CROSSING IS DECLARED. The calling FILE carries an unresolved
 *       specifier that path-suffix-matches a file inside that member, on a
 *       segment boundary — "src/plugin_api.h" matches "plugin_api.h", and
 *       "src/myplugin_api.h" does not.
 *   (b) THE MEMBER PROVIDES THE NAME, ONCE. Exactly one Function/Method in
 *       that member is named <callee>.
 *
 * Name equality alone is nowhere near enough and the counterexample is
 * ordinary: a workspace of three where repo1 calls init(), repo2 happens to
 * define init(), and the two have never heard of each other. Requiring the
 * caller's own source to name the far member is what separates a crossing from
 * a coincidence.
 *
 * (b) is deliberately NOT "the callee is declared in the matched file", which
 * was the first design and is wrong on measurement: a C prototype in a header
 * produces NO node — indexing plugin_api.h yields the include-guard macro and
 * nothing else — so that rule would resolve zero C plugin cases while claiming
 * to be stricter. The pair above is a faithful model of how the linkage
 * actually works: the caller declares a dependency on the other repository, and
 * that repository supplies the symbol by name.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * FAIL CLOSED — AMBIGUITY IS REPORTED, NEVER RESOLVED
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Two members satisfying (a) and (b) is a real configuration — two repositories
 * shipping the same shared_config.h — and it is not decidable from the graph.
 * No edge is written, the call site is counted in `ambiguous`, and each one is
 * reported with the members that answered. A false negative costs a missing
 * edge; a false positive attributes one repository's behaviour to another's
 * code, which is worse and invisible. The same rule applies WITHIN a member:
 * two callables of that name inside the one answering member and the site is
 * ambiguous too, because a repository exporting one symbol twice has not said
 * which one the caller links against either.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE CANDIDATE SET IS DERIVED FROM THE REGISTRY, NEVER WRITTEN DOWN
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Members come from hyp_store_workspace_repos() — the registry A1 binds into
 * the database, ordered by slug. There is no second member list in this file
 * and there must never be one. A store with no registry, or a workspace of one,
 * has nowhere else to look and this pass does nothing at all.
 *
 * ROLE IS NOT A FILTER. A call into a `vendored` or `generated` member is a
 * real call and is reported as one; what a role governs is whether an agent may
 * EDIT that member, which is a separate question from whether the code runs.
 */

/* ═════════════════════════════════════════════════════════════════════════
 * BOTH ENDS OR NEITHER — what wiring this up requires
 * ═════════════════════════════════════════════════════════════════════════
 *
 * This unit has two ends and they are useless apart, so whoever builds the
 * multi-member index driver owns BOTH:
 *
 *   1. Before indexing each member, call
 *      hyp_pipeline_set_workspace_member_count() with the workspace's member
 *      count. Without it nothing is recorded and this pass has nothing to read.
 *   2. After every member's graph is in the one workspace store, call
 *      hyp_workspace_calls_match(). Without it the recorded crossings sit in
 *      the graph unresolved.
 *
 * Doing (1) alone adds nodes nobody reads. Doing (2) alone answers zero,
 * truthfully and uselessly. There is no venue that calls either one today:
 * a full index publishes by renaming a freshly built staging database over the
 * destination, so no code path yet puts two members in one store at all. The
 * matching half is built, tested against four real repositories, and waiting
 * for the assembly half.
 *
 * ── The recorded evidence: names shared by writer and reader ────────
 *
 * These live here, in one place, because the index-time passes write them and
 * this pass reads them. Two copies of a tag string is two things to drift, and
 * a drifted tag is a pass that silently matches nothing. */

#define HYP_WS_TAG_EXTERN "__extern__"
#define HYP_WS_TAG_INCLUDE "__include__"
#define HYP_WS_LABEL_EXTERN "ExternalSymbol"
#define HYP_WS_LABEL_INCLUDE "ExternalModule"
#define HYP_WS_EDGE_UNRESOLVED_CALL "UNRESOLVED_CALLS"
#define HYP_WS_EDGE_UNRESOLVED_IMPORT "UNRESOLVED_IMPORTS"

/* The edge this unit exists to produce. */
#define HYP_WS_EDGE_CROSS_MEMBER "CROSS_MEMBER_CALLS"

/* Normalise an import/include specifier into the rendezvous key described
 * above. Returns false (and writes "") when spec is NULL, empty, normalises to
 * nothing, or does not fit — refused, never truncated, because a truncated
 * specifier is a well-formed specifier for a different file. */
bool hyp_workspace_specifier_key(const char *spec, char *out, size_t out_sz);

/* ── Recording, at index time ────────────────────────────────────────
 *
 * ONE writer each, called from BOTH resolution paths — the sequential
 * (pass_calls.c / pass_definitions.c) and the parallel (pass_parallel.c). The
 * two paths have historically drifted; a shared writer is the only form in
 * which they cannot. Both return the rendezvous node's id, or 0 when nothing
 * was recorded.
 *
 * NEITHER IS CALLED UNLESS THE RUN IS PART OF A MULTI-MEMBER WORKSPACE. A
 * workspace of one has nowhere else to look, so it records nothing and its
 * graph is byte-identical to what it was before this unit existed — which
 * matters, because otherwise every printf() and console.log() in every
 * repository would mint a node. */

typedef struct hyp_gbuf hyp_gbuf_t;

int64_t hyp_pipeline_record_unresolved_call(hyp_gbuf_t *gbuf, int64_t caller_id,
                                            const char *callee);
int64_t hyp_pipeline_record_unresolved_import(hyp_gbuf_t *gbuf, int64_t file_id,
                                              const char *specifier);

/* ── Ambiguity reporting ─────────────────────────────────────────── */

enum {
    HYP_WS_MAX_AMBIGUOUS = 64, /* reported; the count is not capped */
    HYP_WS_QN_BUF = 512,
    HYP_WS_NAME_BUF = 256,
    HYP_WS_MEMBERS_BUF = 512,
};

typedef struct {
    char caller_qn[HYP_WS_QN_BUF];    /* the call site that could not be resolved */
    char callee[HYP_WS_NAME_BUF];     /* the name it called */
    char members[HYP_WS_MEMBERS_BUF]; /* the slugs that answered, comma-separated */
} hyp_workspace_ambiguity_t;

typedef struct {
    int members;    /* workspace members enumerated from the registry */
    int candidates; /* recorded call sites this pass considered */
    int edges;      /* CROSS_MEMBER_CALLS rows written */
    int ambiguous;  /* call sites refused because more than one answer existed */
    bool failed;    /* a query or write failed; counts above are a floor */
    /* First HYP_WS_MAX_AMBIGUOUS refusals, in scan order. `ambiguous` is the
     * true total, so a full array means "look at the count, not the array" —
     * every count in this plan is a floor and this one says so. */
    int reported;
    hyp_workspace_ambiguity_t report[HYP_WS_MAX_AMBIGUOUS];
} hyp_workspace_calls_result_t;

/* Resolve every recorded cross-member call site in an assembled workspace
 * store and write CROSS_MEMBER_CALLS for the unambiguous ones.
 *
 * Idempotent: existing CROSS_MEMBER_CALLS rows for each member are removed
 * first, so a re-run over an unchanged store produces an identical graph.
 *
 * A NULL store, an absent registry, or a workspace of one is not a failure —
 * it is "nothing to do", and `failed` stays false with every count at zero. */
hyp_workspace_calls_result_t hyp_workspace_calls_match(hyp_store_t *store);

#endif /* HYP_PIPELINE_PASS_WORKSPACE_CALLS_H */
