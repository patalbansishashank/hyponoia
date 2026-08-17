#ifndef HYP_FOUNDATION_IDENTITY_H
#define HYP_FOUNDATION_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * identity.h — the one canonical address for a node, and the content hash of
 * its span.
 *
 * Everything that names code anchors to this: the workspace store, every role
 * derivation, decision anchoring, orphan detection and re-index survival. It
 * is a CONTRACT — pure functions over strings, no store, no indexing, no I/O,
 * no allocation.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE STRING
 * ═════════════════════════════════════════════════════════════════════════
 *
 *      hyp1:<workspace>/<repo>#<qualified_name>      repo-scoped
 *      hyp1:<workspace>#<qualified_name>             workspace-scoped
 *
 * e.g.  hyp1:hyponoia/hyponoia#hyponoia.src.pipeline.fqn.hyp_pipeline_fqn_compute
 *       hyp1:acme#__route__GET__/v2/orders/{}
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THIS FORMALISES THE EXISTING QN. IT DOES NOT SUPERSEDE IT.
 * ═════════════════════════════════════════════════════════════════════════
 *
 * A canonical name for a node already exists and is load-bearing in four
 * places: the store's uniqueness constraint UNIQUE(project, qualified_name)
 * (store.c), the graph-buffer merge key (hyp_gbuf_upsert_node), the vector
 * table's primary key (ask_vectors.c) and the MCP-facing handle. It is built by
 * hyp_pipeline_fqn_compute() as `project.dir.parts.name`.
 *
 * So the address does not invent a name. It WRAPS the qualified name in the two
 * scopes the QN does not carry — which repository, and which workspace — and
 * leaves the QN byte-for-byte untouched as the terminal field. Superseding it
 * would mean the address and the store's own uniqueness key disagreed about
 * what "the same node" means, and a divergence between two ends is the failure
 * this whole phase exists to prevent.
 *
 * NEVER build identity on nodes.id. It is AUTOINCREMENT and re-minted 1..N by
 * every full re-index (ask_vectors.h). The QN is the cross-generation-stable
 * key; that is why it is the one inside the address.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * WHAT IS DELIBERATELY NOT IN HERE
 * ═════════════════════════════════════════════════════════════════════════
 *
 *   - NO LINE NUMBERS. This is the whole of acceptance test 2. A symbol that
 *     slides 200 lines down its file keeps its address, because there was
 *     never a line number in it to go stale. C5u passes by construction.
 *
 *   - NO model_id, contract, dim OR window. Those four gate VECTOR REUSE
 *     (ask_vectors.h) and they sit right beside the span hash reused below, so
 *     the temptation to fold them in is real. They are properties of a vector,
 *     not of a node. An address carrying them would change when the encoder
 *     changed, orphaning every anchor in the workspace on a model swap — and
 *     models get swapped routinely.
 *
 *   - NO span hash. The hash is a separate field, never a component of the
 *     string. An address identifies WHERE a node is named; the hash identifies
 *     WHAT was there. Fusing them would make every edit a new address, which
 *     defeats the point of having one. See hyp_addr_relate() for how the pair
 *     is meant to be read together.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * A SINGLE REPO IS A WORKSPACE OF ONE
 * ═════════════════════════════════════════════════════════════════════════
 *
 * There is no repo mode beside a workspace mode, and there must never be one —
 * two code paths that can disagree is the defect, not the feature. Pass
 * workspace = NULL to hyp_addr_init() and the repo becomes its own workspace,
 * which is what the zero-config path (B4) gets with nothing typed. That is a
 * DEFAULT inside one code path, not a second path.
 *
 * The workspace id of a workspace of one is therefore the repo's own slug, so
 * A1's migration of an existing per-repo index is the identity function on
 * every address. The cost, stated: renaming a workspace rewrites every address
 * in it. That is the same cost as renaming a repo, which the project slug
 * already has, and it buys "no input required on first run".
 *
 * ═════════════════════════════════════════════════════════════════════════
 * BOTH SLUGS ARE PROJECT NAMES, AND THAT IS WHAT MAKES THE PARSE EXACT
 * ═════════════════════════════════════════════════════════════════════════
 *
 * `workspace` and `repo` are project slugs — what hyp_project_name_from_path()
 * produces and hyp_validate_project_name() accepts, which is [A-Za-z0-9._-]
 * with no leading dot and no "..". This module validates with that same
 * function rather than restating the character set, because a second copy of a
 * charset is a second thing to drift.
 *
 * That constraint is load-bearing. Neither '/' nor '#' can occur in a slug, so:
 *
 *   - splitting at the FIRST '#' after the scheme separates head from QN
 *     exactly, whatever the QN contains;
 *   - splitting the head at its only '/' separates workspace from repo exactly.
 *
 * The QN is then taken VERBATIM to end of string and never split again.
 *
 * ── The unescaped dot, and why C1 does not fix it ────────────────────────
 *
 * There is no escaping in the QN and there never has been:
 *
 *      hyp_pipeline_fqn_compute("proj", ".env.local", "__file__")
 *          => "proj..env.local.__file__"        (double dot, empty segment)
 *
 * pinned at tests/test_fqn.c. Dots in filenames and in symbol names are emitted
 * raw, so a QN cannot be reliably split back into its segments, and two
 * different (path, name) pairs can in principle produce one QN string.
 *
 * The ADDRESS is nonetheless unambiguous, because the address never splits the
 * QN. The QN is the last field; it is terminal by construction and needs no
 * internal escaping to round-trip. Ambiguity would only appear in a consumer
 * that tried to recover `dir.parts` and `name` from an address, and no such
 * consumer is sanctioned: parse the address, then ask the store.
 *
 * C1 therefore does NOT escape the QN, and the reason is not cost. The store's
 * uniqueness key is UNIQUE(project, qualified_name) over the RAW string. An
 * address that escaped would identify a node the store cannot look up, and two
 * QNs the store already considers one row would become two addresses. The
 * residual collision is real, bounded, pre-existing, and identical to the one
 * the store has always had — the address adds none of its own. Fixing it means
 * changing the QN builder and re-indexing, and it belongs to whoever does that,
 * not here.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE DESIGNED EXCEPTION TO "TWO REPOS, TWO ADDRESSES"
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Acceptance test 1 is that the same symbol in two repos gets two distinct
 * addresses. It does, and it has one deliberate exception.
 *
 * Some qualified names are built WITHOUT a project root on purpose:
 *
 *      __route__GET__/v2/orders/{}      __env__DATABASE_URL
 *      __grpc__Orders/Create            __channel__redis__jobs
 *      __graphql__createOrder           __trpc__orders.create
 *
 * The flatness IS the mechanism. pass_cross_repo.c matches a caller in one
 * repository to a handler in another by string-equal QN, because the store's
 * authorizer denies SQLITE_ATTACH and there is therefore no cross-database join
 * key to use instead. Two repos meeting on one name is the point.
 *
 * So those names are WORKSPACE-SCOPED: the address carries no repo, and the
 * same route in two repos is the SAME address, deliberately. Scoping them per
 * repo would silently disable every CROSS_* edge in the graph.
 *
 * The repo is ABSENT there, not empty. Absent means "this name belongs to no
 * single repository, look across the workspace". Read hyp_addr_t.scope to tell;
 * never branch on repo[0] == '\0'.
 *
 * The classification is DERIVED, not enumerated — see hyp_addr_qn_scope(). A
 * hand-written list of tags would be correct only until the next tag is added,
 * and the missed entry is reliably the one that matters.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * FAIL CLOSED
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Every entry point returns a reason and refuses rather than guessing. Nothing
 * truncates: a truncated address is a well-formed address for a DIFFERENT node,
 * which is the worst possible failure here. The "hyp1:" scheme is not
 * decoration — it is what makes passing a bare qualified name where an address
 * is expected an error instead of a silent misreading.
 */

/* ── Shape ───────────────────────────────────────────────────────── */

#define HYP_ADDR_SCHEME "hyp1"
#define HYP_ADDR_SEP_SCHEME ':'
#define HYP_ADDR_SEP_REPO '/'
#define HYP_ADDR_SEP_NODE '#'

/* A slug is a project name. 200 bytes is the cap hyp_project_name_from_path()
 * already enforces (FQN_MAX_NAME_LEN in fqn.c), because the slug is also the
 * database filename component "<cache>/<slug>.db". Kept in agreement by a test
 * that feeds the producer's output to this consumer, not by comment. */
#define HYP_ADDR_SLUG_MAX 200

/* Qualified names are unbounded in the store's schema and short in practice.
 * The cap exists so an address fits a fixed struct; exceeding it is an ERROR,
 * never a truncation. */
#define HYP_ADDR_QN_MAX 2048

/* Longest canonical string, excluding the NUL:
 * "hyp1" ':' slug '/' slug '#' qn */
#define HYP_ADDR_MAX (4 + 1 + HYP_ADDR_SLUG_MAX + 1 + HYP_ADDR_SLUG_MAX + 1 + HYP_ADDR_QN_MAX)

/* ── Scope ───────────────────────────────────────────────────────── */

typedef enum {
    /* The QN names a node inside one repository. The address carries a repo. */
    HYP_ADDR_SCOPE_REPO = 0,
    /* The QN is a cross-repo rendezvous name — a route, a topic, an env key, an
     * RPC method. It belongs to no single repository, so the address carries no
     * repo and two repositories share it on purpose. */
    HYP_ADDR_SCOPE_WORKSPACE = 1,
} hyp_addr_scope_t;

/* ── The address ─────────────────────────────────────────────────── */

typedef struct {
    char workspace[HYP_ADDR_SLUG_MAX + 1];
    /* Empty IF AND ONLY IF scope == HYP_ADDR_SCOPE_WORKSPACE. Branch on scope,
     * never on this — absent and empty are different claims and only one of
     * them is ever true here. */
    char repo[HYP_ADDR_SLUG_MAX + 1];
    char qn[HYP_ADDR_QN_MAX + 1];
    hyp_addr_scope_t scope;
} hyp_addr_t;

/* ── Refusals ────────────────────────────────────────────────────── */

typedef enum {
    HYP_ADDR_OK = 0,
    /* A required argument was NULL. */
    HYP_ADDR_ERR_NULL,
    /* Workspace slug is empty, over HYP_ADDR_SLUG_MAX bytes, or rejected by
     * hyp_validate_project_name. */
    HYP_ADDR_ERR_WORKSPACE,
    /* Same, for the repo slug. */
    HYP_ADDR_ERR_REPO,
    /* Qualified name is empty. */
    HYP_ADDR_ERR_QN_EMPTY,
    /* Qualified name holds a control byte (< 0x20 or 0x7F). Addresses travel
     * through JSON, logs and line-oriented files; a newline inside one would
     * split a record. */
    HYP_ADDR_ERR_QN_CONTROL,
    /* Qualified name is over HYP_ADDR_QN_MAX bytes. Refused, not truncated. */
    HYP_ADDR_ERR_QN_TOO_LONG,
    /* String does not begin with "hyp1:" — most likely a bare qualified name
     * handed to a function that wanted an address. */
    HYP_ADDR_ERR_SCHEME,
    /* No '#': the string names no node. */
    HYP_ADDR_ERR_NO_NODE,
    /* The string is syntactically fine and semantically impossible: a repo
     * pinned to a rendezvous QN, or a repo-scoped QN with no repo. Refusing
     * these is what keeps hyp_addr_parse the exact inverse of
     * hyp_addr_format(hyp_addr_init(...)) — the two ends cannot drift, because
     * neither accepts anything the other cannot produce. */
    HYP_ADDR_ERR_SCOPE_MISMATCH,
} hyp_addr_err_t;

/* Stable, human-facing reason. Never NULL, including for an unknown value. */
const char *hyp_addr_err_str(hyp_addr_err_t err);

/* ── Compose, render, parse ──────────────────────────────────────── */

/* Fill `out` from its parts.
 *
 * `workspace` may be NULL, which means "workspace of one": `repo` becomes its
 * own workspace. This is the zero-config default and it is the SAME code path,
 * not a second mode.
 *
 * `repo` is always required, even for a rendezvous QN — it is how the scope is
 * derived (hyp_addr_qn_scope) and it is what a caller always has. When the QN
 * turns out to be workspace-scoped the repo is dropped from the address and
 * out->scope says so; the caller has not lost information, it has been told
 * that this name is shared.
 *
 * On refusal `out` is zeroed, so a caller that ignores the return value gets an
 * empty address rather than a half-built one. */
hyp_addr_err_t hyp_addr_init(hyp_addr_t *out, const char *workspace, const char *repo,
                             const char *qn);

/* Render the canonical string into `out`.
 *
 * Returns false and writes "" when `out_sz` cannot hold the whole address —
 * NEVER a truncation, because a truncated address is a valid address for a
 * different node. HYP_ADDR_MAX + 1 always suffices. */
bool hyp_addr_format(const hyp_addr_t *addr, char *out, size_t out_sz);

/* Parse a canonical string. The exact inverse of hyp_addr_format: it accepts
 * every string that function emits and no other. On refusal `out` is zeroed. */
hyp_addr_err_t hyp_addr_parse(const char *s, hyp_addr_t *out);

/* ── Scope derivation ────────────────────────────────────────────── */

/* Is `qn` a node inside `repo`, or a cross-repo rendezvous name?
 *
 * DERIVED FROM THE SHAPE THE BUILDERS EMIT, not from a list of tags. Every
 * rendezvous name in the tree is written with a leading double-underscore tag
 * and no project root — "__route__…", "__grpc__…", "__graphql__…", "__trpc__…",
 * "__channel__…", "__env__…" (pass_calls.c, pass_route_nodes.c,
 * pass_definitions.c, pass_parallel.c, pipeline.c) — while every repo-scoped QN
 * is rooted at the project slug because hyp_pipeline_fqn_compute() prepends it.
 * So the test is: a closing "__" in the first dot-segment, AND not rooted at
 * this repo. A tag added tomorrow is covered the day it is added.
 *
 * The second condition is not redundant. "<project>.__branch__.<slug>" is
 * project-rooted and so stays repo-scoped, correctly; and a repository whose
 * own slug happens to look like a tag ("__foo__bar") keeps its own nodes.
 *
 * Ambiguity resolves toward HYP_ADDR_SCOPE_REPO, deliberately. Misreading a
 * rendezvous name as repo-scoped costs a cross-repo edge that fails to meet — a
 * false negative, recoverable. Misreading a real node as workspace-scoped
 * collides two repositories' distinct symbols into one address — a false
 * positive, and it attaches one repo's reasoning to another's code.
 *
 * NULL or empty `qn` is HYP_ADDR_SCOPE_REPO; it is not a rendezvous name and
 * hyp_addr_init refuses it separately. */
hyp_addr_scope_t hyp_addr_qn_scope(const char *repo, const char *qn);

/* ── Comparison ──────────────────────────────────────────────────── */

/* Every canonical field equal, scope included. */
bool hyp_addr_equal(const hyp_addr_t *a, const hyp_addr_t *b);

/* Same workspace AND same repository. False when either address is
 * workspace-scoped: a rendezvous name is in no repository, so it is in no
 * SAME repository either. Absent is not a match. */
bool hyp_addr_same_repo(const hyp_addr_t *a, const hyp_addr_t *b);

/* ── The content hash of a span ──────────────────────────────────── */

/* SHA-256 of the span text, lowercase hex, kept to 32 chars. 128 bits against a
 * corpus of millions of declarations, at a quarter of the storage.
 *
 * This is THE SAME HASH the vector store has always used, not a second one.
 * hyp_ask_content_hash() (ask_embed.c) forwards here, and HYP_ASK_VEC_HASH_LEN
 * is asserted equal to this at compile time. There is exactly one
 * implementation, on purpose: this file criticises a duplicated FQN builder for
 * being two implementations of one idea, and would have no standing to if it
 * shipped a second hash beside the first. */
#define HYP_ADDR_SPAN_HASH_LEN 32

/* Hash `text`. `out` must hold HYP_ADDR_SPAN_HASH_LEN + 1 bytes. A NULL `text`
 * hashes the empty string, matching the existing behaviour exactly. */
void hyp_addr_span_hash(const char *text, char *out);

/* ── Reading an address and a hash together ──────────────────────── */

/*
 * The span hash is taken over the span's VERBATIM LINES, declaration line
 * INCLUDED — spans are (file_path, start_line, end_line), 1-based, and
 * start_line is the declaration, not the body. That single fact is what makes a
 * symbol rename separable from a move: renaming `foo` changes the QN (so the
 * address) AND the declaration text (so the hash), while relocating it changes
 * only the address. It is pinned by a test rather than left to hold by accident
 * of span extraction, because C4u is built on it.
 */
typedef enum {
    /* Could not compare — a NULL argument or a malformed hash. NOT "unrelated";
     * a caller must never read "I could not tell" as "no relationship". */
    HYP_ADDR_REL_INVALID = 0,
    /* Same address, same content. Intact. */
    HYP_ADDR_REL_SAME,
    /* Same address, content changed. Still attached: the symbol is where it was
     * and was edited in place. Reasoning attached to it may be stale, which is
     * a different problem from being lost. */
    HYP_ADDR_REL_EDITED,
    /*
     * DIFFERENT ADDRESS, SAME CONTENT. A CANDIDATE. NEVER AN AUTOMATIC
     * RE-ATTACHMENT.
     *
     * Read this before using it. Same-hash-different-address is strong evidence
     * and it is NOT proof of a move:
     *
     *   - a cross-directory relocation (src/a/foo.c -> src/b/foo.c) produces it;
     *   - a file rename produces it, identically — this contract cannot tell
     *     those two apart, and does not pretend to;
     *   - and a COPY-PASTE of `foo` into a second file produces it too, with
     *     the original still sitting untouched at the old address.
     *
     * That last one is why this value exists instead of being folded into
     * HYP_ADDR_REL_SAME. Re-attaching last month's reasoning to a copy is the
     * false positive the whole asymmetry guards against: a false negative makes
     * a decision unfindable, which is recoverable; a false positive explains
     * code that never had that reasoning, which is worse and is invisible.
     *
     * C4u presents this with its evidence. C4u does not decide it.
     */
    HYP_ADDR_REL_CONTENT_ONLY,
    /* Neither address nor content matches. Nothing may be inferred. */
    HYP_ADDR_REL_UNRELATED,
} hyp_addr_rel_t;

/* Stable, human-facing name. Never NULL. */
const char *hyp_addr_rel_str(hyp_addr_rel_t rel);

/* Relate two observations of (address, span hash). Pure; no store, no lookup.
 * Hashes are HYP_ADDR_SPAN_HASH_LEN hex chars as produced by
 * hyp_addr_span_hash; anything else is HYP_ADDR_REL_INVALID. */
hyp_addr_rel_t hyp_addr_relate(const hyp_addr_t *before, const char *before_hash,
                               const hyp_addr_t *after, const char *after_hash);

#endif /* HYP_FOUNDATION_IDENTITY_H */
