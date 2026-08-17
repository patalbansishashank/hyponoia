#ifndef HYP_STORE_WORKSPACE_RESOLVE_H
#define HYP_STORE_WORKSPACE_RESOLVE_H

#include <stdbool.h>
#include <stddef.h>

#include "foundation/identity.h" /* HYP_ADDR_SLUG_MAX — a workspace id IS a slug */
#include "store/store.h"

/*
 * workspace_resolve.h — THE ONE resolver from "where am I / what was declared"
 * to "which workspace, which member repos". It feeds the store's open path and
 * is built on the identity contract (identity.h).
 *
 * One resolver, never two code paths that can disagree. Every venue that needs
 * to know what workspace a directory belongs to — the CLI, the MCP server, the
 * daemon, onboarding (B2), the zero-config path (B4) — calls hyp_wsr_resolve()
 * and nothing else. A second resolver, or a caller that peeks at the TOML
 * itself, is the defect this header exists to prevent.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * PRECEDENCE — stated, fixed, and closed
 * ═════════════════════════════════════════════════════════════════════════
 *
 *   1. EXPLICIT TOML.  `toml_path` non-NULL means the file GOVERNS. A file
 *      that cannot be read or parsed is a REFUSAL, never a fall-through —
 *      falling through would silently index a declared workspace as a
 *      workspace of one, splitting its address space.
 *   2. PROVIDER.  A non-NULL `provider` is asked next (Multica's
 *      `workspace.repos` is the first implementation; see the seam below).
 *      NONE means "no workspace known for this directory" and falls through.
 *      ERROR is a refusal — "could not tell" is never read as "no workspace".
 *   3. ZERO-CONFIG.  `start_dir` itself, a workspace of one, role "member"
 *      (B4). This is a DEFAULT inside the one code path, not a second mode:
 *      the resolved workspace is shaped identically to a TOML one.
 *
 * Discovery of a TOML file is the CALLER's decision, made explicit via
 * hyp_wsr_toml_find() — the resolver itself never goes looking, so what
 * governed a resolution is always visible in its inputs.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE TOML READ FORMAT — B3's writer builds against THIS comment
 * ═════════════════════════════════════════════════════════════════════════
 *
 *      # hyponoia workspace — read by hyp_wsr_resolve(), written by B3.
 *      name = "acme"              # optional; must satisfy
 *                                 # hyp_validate_project_name()
 *
 *      [[repos]]
 *      path = "/abs/path/backend" # required. Relative paths resolve against
 *                                 # the DIRECTORY CONTAINING THE TOML FILE.
 *      role = "member"            # optional; one of the vocabulary below
 *
 *      [[repos]]
 *      path = "../frontend"
 *
 * The accepted subset, exactly: blank lines; `#` comments (whole-line or
 * trailing); `name = "..."` before the first [[repos]], at most once;
 * `[[repos]]` headers; `path = "..."` and `role = "..."` inside a repo, at
 * most once each. Values are TOML basic strings with `\\` and `\"` escapes
 * only. ANYTHING ELSE — an unknown key, a [table], a bare value, a second
 * `path` — is a parse refusal naming the line. Fail closed: a typo'd key that
 * were skipped would silently drop a repo from the workspace.
 *
 * Canonical filename: HYP_WSR_TOML_NAME ("hyponoia.toml"). B2/B4 locate it
 * with hyp_wsr_toml_find(), which walks from start_dir toward the filesystem
 * root and returns the NEAREST one, so a workspace file beside or above the
 * member repos governs all of them.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * THE WORKSPACE ID IS DERIVED DETERMINISTICALLY — never "the first repo"
 * ═════════════════════════════════════════════════════════════════════════
 *
 * Two machines naming the same workspace differently produce DISJOINT address
 * spaces that C6u's union merges silently — and the record field set is
 * frozen, so prevention here is the only fix. Therefore, in order:
 *
 *   - TOML `name`, when present. Naming a workspace is the TOML's privilege
 *     alone; a provider supplies members, never a name.
 *   - ONE member (however sourced): the member's own slug. This is what makes
 *     A1's migration the identity function — a repo indexed yesterday as
 *     `<cache>/<slug>.db` resolves today to workspace id <slug>, the same
 *     file, and every existing address byte-for-byte unchanged.
 *   - N members: "ws-" + the first 16 hex chars of SHA-256 over the SORTED
 *     member slugs (domain-tagged "hyp-workspace-id-v1", each slug
 *     length-prefixed). A pure function of the member SET — enumeration
 *     order, declaration order and "which repo came first" cannot reach it.
 *
 * Stated limitation, deliberately not papered over: a member slug is
 * hyp_project_name_from_path() of the repo's canonical root, so machines that
 * check the same repos out at DIFFERENT paths derive different slugs and so
 * different ids. That is the pre-existing per-path identity of the project
 * slug (C1 formalised it; this module must not fork it). The remedy is the
 * TOML `name`; G4's cross-machine gate is where the residual cost is measured.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * SLUG COLLISION IS REFUSED — the gate A1's migration inherits
 * ═════════════════════════════════════════════════════════════════════════
 *
 * `/a/b` and `/a-b` both slug to "a-b". Across separate databases that was
 * harmless; inside one workspace it merges two repositories into one address
 * space. Resolution REFUSES, and the refusal names BOTH paths — never "a
 * collision occurred". The same gate sits at the store end
 * (hyp_store_workspace_bind), so neither end can be talked past alone.
 *
 * ═════════════════════════════════════════════════════════════════════════
 * ROLES — the vocabulary, and what "absent" means
 * ═════════════════════════════════════════════════════════════════════════
 *
 * The five roles are the ones A2-A5 define: "member", "vendored", "generated",
 * "upstream", "reference". A declared role outside this vocabulary is a
 * refusal (a typo'd role that defaulted would silently change writability).
 * An UNDECLARED role stays absent (member.role == "", NULL in the registry):
 * absent means "A2-A5 have not derived it yet — consumers apply A5's default,
 * reference", never "". The one exception is pinned by B4: the zero-config
 * single repo IS declared, role "member", because that repo being editable is
 * the whole point of indexing it.
 */

/* ── Shape ───────────────────────────────────────────────────────── */

#define HYP_WSR_TOML_NAME "hyponoia.toml"

enum {
    HYP_WSR_MAX_REPOS = 32,
    HYP_WSR_PATH_MAX = 1024, /* refused when exceeded, never truncated */
    HYP_WSR_ROLE_MAX = 16,
};

/* The role vocabulary (A2-A5). hyp_wsr_role_known is the ONE membership test. */
#define HYP_WSR_ROLE_MEMBER "member"
#define HYP_WSR_ROLE_VENDORED "vendored"
#define HYP_WSR_ROLE_GENERATED "generated"
#define HYP_WSR_ROLE_UPSTREAM "upstream"
#define HYP_WSR_ROLE_REFERENCE "reference"
bool hyp_wsr_role_known(const char *role);

/* A resolved member repository. */
typedef struct {
    /* hyp_project_name_from_path(root) — THE project slug, the same one the
     * indexer keys the projects table and every QN with. Never derived any
     * other way, or the registry and the graph would disagree about names. */
    char slug[HYP_ADDR_SLUG_MAX + 1];
    /* Canonical absolute path of the member's root. */
    char root[HYP_WSR_PATH_MAX];
    /* Declared role, or "" when undeclared (see the roles section). */
    char role[HYP_WSR_ROLE_MAX];
} hyp_wsr_member_t;

typedef enum {
    HYP_WSR_SOURCE_TOML = 0,
    HYP_WSR_SOURCE_PROVIDER = 1,
    HYP_WSR_SOURCE_ZERO_CONFIG = 2,
} hyp_wsr_source_t;

/* A resolved workspace. members[] is SORTED BY SLUG — deterministic
 * enumeration, so two machines resolving the same set hold identical structs. */
typedef struct {
    char id[HYP_ADDR_SLUG_MAX + 1];
    hyp_wsr_source_t source;
    int member_count;
    hyp_wsr_member_t members[HYP_WSR_MAX_REPOS];
} hyp_wsr_resolved_t;

/* ── The provider seam (Multica is AN implementation, not a dependency) ──
 *
 * Nothing in core depends on Multica existing. A provider is handed in by the
 * caller; core code links against this struct only. Multica's adapter reads
 * its `workspace.repos`; D5-style feeds can implement the same three-valued
 * answer. A provider supplies member PATHS (and optionally roles) — slugs are
 * always derived here, by the one slug function, so a provider cannot
 * introduce a second naming scheme. */

typedef struct {
    char path[HYP_WSR_PATH_MAX]; /* member repo root; absolute, or relative
                                  * to start_dir */
    char role[HYP_WSR_ROLE_MAX]; /* "" = undeclared */
} hyp_wsr_member_spec_t;

typedef enum {
    /* No workspace known for this directory. Resolution FALLS THROUGH to
     * zero-config. Absent, not error. */
    HYP_WSR_PROVIDER_NONE = 0,
    HYP_WSR_PROVIDER_FOUND = 1,
    /* The provider could not answer (its store unreadable, its schema
     * unexpected). Resolution REFUSES — "could not tell" must never be read
     * as "no workspace", which would quietly split an address space. */
    HYP_WSR_PROVIDER_ERROR = -1,
} hyp_wsr_provider_rc_t;

typedef struct hyp_wsr_provider {
    const char *name; /* for refusal messages, e.g. "multica" */
    hyp_wsr_provider_rc_t (*resolve)(void *ctx, const char *start_dir, hyp_wsr_member_spec_t *out,
                                     int max, int *count);
    void *ctx;
} hyp_wsr_provider_t;

/* ── Refusals ────────────────────────────────────────────────────── */

typedef enum {
    HYP_WSR_OK = 0,
    HYP_WSR_ERR_NULL,           /* required argument NULL */
    HYP_WSR_ERR_TOML_READ,      /* explicit toml_path unreadable / oversized */
    HYP_WSR_ERR_TOML_PARSE,     /* outside the published subset; err names the line */
    HYP_WSR_ERR_NAME,           /* TOML name fails hyp_validate_project_name */
    HYP_WSR_ERR_NO_REPOS,       /* TOML/provider yielded zero members */
    HYP_WSR_ERR_TOO_MANY_REPOS, /* over HYP_WSR_MAX_REPOS */
    HYP_WSR_ERR_REPO_PATH,      /* member root missing, not a directory,
                                 * uncanonicalizable, or over PATH_MAX */
    HYP_WSR_ERR_ROLE,           /* declared role outside the vocabulary */
    HYP_WSR_ERR_SLUG,           /* slug derivation failed or invalid */
    HYP_WSR_ERR_SLUG_COLLISION, /* err names BOTH colliding paths */
    HYP_WSR_ERR_PROVIDER,       /* provider returned ERROR — refused, never
                                 * fallen through */
} hyp_wsr_err_t;

/* Stable, human-facing reason. Never NULL, including for an unknown value. */
const char *hyp_wsr_err_str(hyp_wsr_err_t err);

/* ── Resolve ─────────────────────────────────────────────────────── */

/*
 * Resolve the workspace governing start_dir.
 *
 *   start_dir  required; an existing directory (the repo root, or the
 *              directory the tool was invoked in).
 *   toml_path  an EXPLICIT workspace file, or NULL. Non-NULL means the file
 *              governs and its failures refuse (precedence 1).
 *   provider   the Multica-or-other seam, or NULL when absent (precedence 2).
 *   out        filled on HYP_WSR_OK; zeroed on refusal, so a caller that
 *              ignores the return value holds an empty workspace, not a
 *              half-built one.
 *   err        optional (may be NULL/0): human-facing refusal detail — the
 *              offending line, both colliding paths, the unknown role.
 */
hyp_wsr_err_t hyp_wsr_resolve(const char *start_dir, const char *toml_path,
                              const hyp_wsr_provider_t *provider, hyp_wsr_resolved_t *out,
                              char *err, size_t err_sz);

/* Find the nearest HYP_WSR_TOML_NAME at or above start_dir. True when found
 * (absolute path in out); false when none exists — which is not an error, it
 * is precedence 2 and 3's turn. */
bool hyp_wsr_toml_find(const char *start_dir, char *out, size_t out_sz);

/* ── Open (A1's one open path) ───────────────────────────────────── */

/*
 * Open the workspace's store: <cache>/<id>.db via hyp_store_open() — the SAME
 * file naming, the SAME layout, the SAME open path a per-repo project has
 * always had. There is no "workspace mode": a workspace of one opens the
 * byte-identical file the repo's per-project index already lives in, and
 * binding the registry there IS A1's migration — it writes the workspace_meta
 * and workspace_repos rows and touches nothing else, so it is the identity
 * function on every existing address.
 *
 * Refuses (NULL + err) when the store cannot open or the registry bind
 * refuses — a different id already bound, or a slug collision (the store-end
 * gate). The caller owns the returned store.
 */
hyp_store_t *hyp_wsr_store_open(const hyp_wsr_resolved_t *ws, char *err, size_t err_sz);

#endif /* HYP_STORE_WORKSPACE_RESOLVE_H */
