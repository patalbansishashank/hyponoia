#ifndef HYP_DISCOVER_ROLE_H
#define HYP_DISCOVER_ROLE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * role.h — what a repository in a workspace IS, derived, never enumerated.
 *
 * A workspace holds many trees and only some of them are yours. An agent that
 * cannot tell which is which will rewrite a dependency with the same
 * confidence it edits your code. The role is the one bit that prevents that,
 * so it is derived from properties of the tree — registries that already
 * exist, gitignore semantics, generated-code markers, the git remote — and
 * NEVER from a hand-written list of directories. A list is only as good as
 * its enumeration, and twice in this project the missed entry was the most
 * user-facing one.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE ROLES
 * ═════════════════════════════════════════════════════════════════════════
 *
 *   member     yours; editable. NEVER derived here — it is declared by the
 *              resolver (TOML / the zero-config "this repo" default). This
 *              module can only ever take a repository AWAY from editability,
 *              not hand editability out.
 *   vendored   someone else's code carried in-tree, pinned by a checksum
 *              registry. Read-only.
 *   generated  emitted by a build or a generator; editing it is editing the
 *              output instead of the input. Read-only.
 *   upstream   a checkout of a project this workspace vendors — the clone of
 *              ggml-org/llama.cpp you keep beside the tree that embeds it.
 *              Read-only.
 *   reference  "I added this repo for inspiration." The ONLY role that cannot
 *              be derived, because that sentence is intent, not a property of
 *              a tree. Read-only, and the fail-closed default (A5).
 *
 * ═════════════════════════════════════════════════════════════════════════
 * EVERY ANSWER SAYS WHICH RULE FIRED
 * ═════════════════════════════════════════════════════════════════════════
 *
 * A role without its source is a claim without a witness — the discipline the
 * MCP `project_source` field set. hyp_role_result_t always carries the source
 * token, one of exactly:
 *
 *   declared-toml              the resolver declared it (A6 owns declaration;
 *                              this module only validates and echoes it)
 *   derived-vendored-registry  the tree is covered by an enclosing repo's
 *                              scripts/vendored-checksums.txt (A2)
 *   derived-gitignore          an enclosing repo gitignores the tree (A3)
 *   derived-do-not-edit        a bounded sample of the tree's files carry a
 *                              DO-NOT-EDIT / @generated header (A3)
 *   derived-remote             the origin remote is a recorded vendoring
 *                              upstream of this workspace (A4)
 *   default-reference          nothing fired; fail closed to read-only (A5)
 *
 * ═════════════════════════════════════════════════════════════════════════
 * PRECEDENCE, STATED ONCE AND TESTED
 * ═════════════════════════════════════════════════════════════════════════
 *
 *   declared-toml
 *     > derived-vendored-registry
 *     > derived-gitignore
 *     > derived-do-not-edit
 *     > derived-remote
 *     > default-reference
 *
 * Declaration wins over every derivation because a human's stated intent
 * outranks a heuristic — and when the two CONTRADICT, that is a finding to
 * REPORT, never to absorb: hyp_role_audit() (A7) exists so the declaration
 * survives and the contradiction is visible. Among derivations, the order is
 * by strength of evidence: a checksum registry entry is a signed statement,
 * gitignore is an explicit authorial act, a DO-NOT-EDIT header is a sampled
 * heuristic, and a remote match says where a tree came from but not what it
 * is here for. All five non-member outcomes are read-only, so a wrong pick
 * among them is recoverable by construction.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * A2 — VENDORED, FROM THE REGISTRY THAT ALREADY EXISTS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * This repository already declares its vendored code three times over, all
 * fed from one registry: scripts/vendored-checksums.txt (the sha256 manifest
 * scripts/security-vendored.sh verifies), the licence gate that walks the
 * same trees (scripts/license-gate.sh), and the provenance audit that reports
 * `IDENTICAL against ggml-org/llama.cpp` from its LIBS table
 * (scripts/audit-license-provenance.py). This module is a fourth READER of
 * that registry, not a second place to declare it: a tree is vendored iff an
 * enclosing repository's scripts/vendored-checksums.txt covers it. The
 * manifest format is reused verbatim — `<sha256><spaces><relative path>` —
 * so newly vendored code becomes role `vendored` the moment the existing
 * gates learn about it, with no edit here.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * A3 — GENERATED, FROM GITIGNORE SEMANTICS AND DO-NOT-EDIT HEADERS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Two independent signals, checked in this order:
 *
 *   1. The tree is ignored by the nearest enclosing git repository —
 *      matched with the same hyp_gitignore engine discovery uses, over every
 *      .gitignore on the path from that repo's root down to the tree.
 *      A build directory is the canonical case.
 *   2. A bounded scan: up to HYP_ROLE_SCAN_MAX_FILES text files, first
 *      HYP_ROLE_SCAN_HEAD_BYTES of each, looking for "do not edit" (any
 *      case) or "@generated". A strict MAJORITY of the sample marks the tree
 *      generated. The scan is a sample, deliberately — reading every byte of
 *      every file to answer a metadata question is the wrong trade — and the
 *      derived-do-not-edit source token is what keeps the heuristic honest.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * A4 — UPSTREAM, FROM THE GIT REMOTE. THE RULE:
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Parse the tree's `origin` remote url (read from the git config file —
 * worktree indirection followed, no subprocess) into a host-independent
 * lowercase coordinate, "owner/name". Then:
 *
 *   - coordinate == the workspace's own origin coordinate → NOT upstream:
 *     that is another checkout of this project, and its role is the
 *     resolver's to declare.
 *   - coordinate ∈ the known-upstream set → upstream. The set is READ from
 *     the registries that already exist (the provenance audit's LIBS/FORKS
 *     tables and the grammar manifest's verified-upstream table) via
 *     hyp_role_upstreams_load() — never hand-written.
 *   - anything else → no signal. A random clone is not "upstream of nothing";
 *     it falls through to reference.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * A5 — UNKNOWN DEFAULTS TO reference, ALWAYS
 * ═════════════════════════════════════════════════════════════════════════
 *
 * No declaration and no rule fired → reference. A declared role this module
 * does not recognise → reference, with the evidence saying so. An unreadable
 * tree → reference. Wrong-but-read-only is recoverable; wrong-but-editable
 * means an agent rewrites a dependency. There is no code path from "no
 * signal" to `member`.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * A7 — THE AUDIT REPORTS. IT NEVER REASSIGNS.
 * ═════════════════════════════════════════════════════════════════════════
 *
 * A repo tagged `member` whose files are overwhelmingly DO-NOT-EDIT is a
 * contradiction between a person's claim and the tree's evidence. Resolving
 * it silently — either way — destroys information: absorbing the derived
 * role hides that the declaration is stale; keeping quiet hides that the
 * evidence disagrees. So hyp_role_audit() returns the contradictions and
 * touches nothing; the resolved role after an audit is byte-identical to the
 * role before it, and the test that holds this invariant is the point, not a
 * formality. The audit is asymmetric by design: it only examines trees
 * resolved `member`, because member is the only editable role — a read-only
 * tree wrongly tagged read-only is a recoverable mistake, not a hazard.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * PURE DERIVATION — THE RESOLVER CALLS THIS, NEVER THE REVERSE
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Path in, role + source out. No TOML parsing, no workspace store, no Multica,
 * no clock, no subprocess. Everything the derivation cannot see from the tree
 * itself (a declaration, the workspace's own origin, the known-upstream set)
 * arrives through hyp_role_ctx_t, supplied by the caller. Evidence strings
 * are human-readable disclosure and MAY truncate to fit; they are never
 * parsed, so truncation is safe here (unlike an address, which must refuse).
 */

/* ── Shape ───────────────────────────────────────────────────────── */

enum {
    /* Longest path this module will reason about; matches the 4096-byte
     * contract of hyp_canonical_path(). */
    HYP_ROLE_PATH_MAX = 4096,
    /* Human-readable evidence; truncation permitted (see header). */
    HYP_ROLE_EVIDENCE_MAX = 512,
    /* Normalized "owner/name" remote coordinate. */
    HYP_ROLE_COORD_MAX = 256,
    /* Bounded DO-NOT-EDIT scan: sample size, head window, walk depth. */
    HYP_ROLE_SCAN_MAX_FILES = 64,
    HYP_ROLE_SCAN_HEAD_BYTES = 4096,
    HYP_ROLE_SCAN_MAX_DEPTH = 16,
};

typedef enum {
    HYP_ROLE_MEMBER = 0,
    HYP_ROLE_VENDORED,
    HYP_ROLE_GENERATED,
    HYP_ROLE_UPSTREAM,
    HYP_ROLE_REFERENCE,
} hyp_role_t;

typedef enum {
    HYP_ROLE_SRC_DECLARED_TOML = 0,
    HYP_ROLE_SRC_DERIVED_VENDORED_REGISTRY,
    HYP_ROLE_SRC_DERIVED_GITIGNORE,
    HYP_ROLE_SRC_DERIVED_DO_NOT_EDIT,
    HYP_ROLE_SRC_DERIVED_REMOTE,
    HYP_ROLE_SRC_DEFAULT_REFERENCE,
} hyp_role_source_t;

typedef struct {
    hyp_role_t role;
    hyp_role_source_t source;
    /* Which registry line / ignore file / remote fired, for a human. */
    char evidence[HYP_ROLE_EVIDENCE_MAX + 1];
} hyp_role_result_t;

/* Stable wire names: "member", "vendored", "generated", "upstream",
 * "reference". */
const char *hyp_role_name(hyp_role_t role);

/* Stable wire names, exactly the six tokens listed in the header. */
const char *hyp_role_source_name(hyp_role_source_t source);

/* Parse a declared role name (exact, lowercase). Returns false — caller
 * falls back to reference — on anything else. Deliberately strict: two
 * spellings of one role is two things that can drift. */
bool hyp_role_parse(const char *name, hyp_role_t *out);

/* ── The known-upstream set (read from existing registries) ──────── */

typedef struct {
    char coord[HYP_ROLE_COORD_MAX + 1];          /* lowercase "owner/name" */
    char origin_file[HYP_ROLE_EVIDENCE_MAX + 1]; /* registry it came from */
} hyp_role_upstream_entry_t;

typedef struct {
    hyp_role_upstream_entry_t *entries;
    size_t count;
    size_t capacity;
} hyp_role_upstreams_t;

void hyp_role_upstreams_init(hyp_role_upstreams_t *set);

/* Append every upstream coordinate the repository at repo_root already
 * declares: scripts/audit-license-provenance.py (LIBS / FORKS /
 * DISAGREEMENT entries, their formats reused verbatim) and
 * internal/hyp/vendored/grammars/MANIFEST.md (the verified-upstream table).
 * Absent registry files contribute nothing and are not an error — a
 * workspace member without registries simply vendors nothing. Duplicates are
 * collapsed. Returns false only on allocation failure (set unchanged from
 * the last successful append). */
bool hyp_role_upstreams_load(hyp_role_upstreams_t *set, const char *repo_root);

void hyp_role_upstreams_free(hyp_role_upstreams_t *set);

/* ── Caller-supplied context ─────────────────────────────────────── */

typedef struct {
    /* Declared role string from the resolver (TOML / zero-config default),
     * NULL when undeclared. This module validates it; it never reads TOML. */
    const char *declared;
    /* Lowercase "owner/name" coordinate of the workspace's own origin, NULL
     * when unknown. A candidate whose remote equals this is another checkout
     * of the workspace's own project, never `upstream`. */
    const char *workspace_origin;
    /* Known vendoring upstreams, NULL when none were loaded. */
    const hyp_role_upstreams_t *known_upstreams;
} hyp_role_ctx_t;

/* ── The derivation ──────────────────────────────────────────────── */

/* Derive the role of the repository rooted at repo_root, in the precedence
 * order stated in the header. ctx may be NULL (no declaration, no remote
 * context). Always fills *out — the fail-closed default guarantees an
 * answer — and returns false only when repo_root or out is NULL. */
bool hyp_role_derive(const char *repo_root, const hyp_role_ctx_t *ctx, hyp_role_result_t *out);

/* ── The individual rules, exposed for tests and for the audit ───── */

/* A2. True iff an ancestor's scripts/vendored-checksums.txt covers
 * repo_root. evidence (optional, evidence_cap bytes) names the manifest and
 * the first covering entry. */
bool hyp_role_is_vendored(const char *repo_root, char *evidence, size_t evidence_cap);

/* A3, signal 1. True iff the nearest enclosing git repository STRICTLY above
 * repo_root ignores it (every .gitignore from that root down to repo_root's
 * parent is consulted, on the directory itself and each ancestor component). */
bool hyp_role_is_gitignored(const char *repo_root, char *evidence, size_t evidence_cap);

/* A3, signal 2 — the bounded scan. Counts, not a verdict: sampled text
 * files, and how many of them carry a DO-NOT-EDIT / @generated marker in
 * their head bytes. Returns false when repo_root cannot be walked at all. */
typedef struct {
    unsigned sampled;
    unsigned marked;
} hyp_role_scan_t;

bool hyp_role_scan_do_not_edit(const char *repo_root, hyp_role_scan_t *out);

/* Derivation threshold: a strict majority of the sample is marked. */
bool hyp_role_scan_majority(const hyp_role_scan_t *scan);

/* Audit threshold (A7's "overwhelmingly"): at least 9 in 10 marked. */
bool hyp_role_scan_overwhelming(const hyp_role_scan_t *scan);

/* A4, mechanics. Resolve repo_root's `origin` remote url to a lowercase
 * "owner/name" coordinate. Follows a worktree's `.git` file and `commondir`
 * indirection; reads files only, never runs git. Returns false when there is
 * no origin or the url has no host/path shape (a filesystem remote is not
 * classifiable — fail closed). */
bool hyp_role_remote_coordinate(const char *repo_root, char *out, size_t out_cap);

/* A4, the rule (see header). */
bool hyp_role_is_upstream(const char *repo_root, const hyp_role_ctx_t *ctx, char *evidence,
                          size_t evidence_cap);

/* ── A7 — the role audit ─────────────────────────────────────────── */

typedef enum {
    /* member, but the vendored registry covers the tree. */
    HYP_ROLE_CONTRA_MEMBER_VENDORED = 0,
    /* member, but the sampled files are overwhelmingly DO-NOT-EDIT. */
    HYP_ROLE_CONTRA_MEMBER_DO_NOT_EDIT,
    /* member, but the origin remote is a recorded vendoring upstream. */
    HYP_ROLE_CONTRA_MEMBER_UPSTREAM,
} hyp_role_contra_kind_t;

typedef struct {
    hyp_role_contra_kind_t kind;
    char evidence[HYP_ROLE_EVIDENCE_MAX + 1];
} hyp_role_contradiction_t;

/* Compare a RESOLVED role against the tree's derived evidence and report
 * contradictions. Writes at most cap entries into out but returns the TRUE
 * count — a count is a floor, and capping the report must not cap the
 * finding. `resolved` is read, never written: the audit reports, the role
 * stays whatever it was, and tests hold that invariant. Returns 0 for a
 * consistent pair, for any non-member role (asymmetric by design — see
 * header), and for NULL arguments. */
size_t hyp_role_audit(const char *repo_root, const hyp_role_result_t *resolved,
                      const hyp_role_ctx_t *ctx, hyp_role_contradiction_t *out, size_t cap);

#endif /* HYP_DISCOVER_ROLE_H */
