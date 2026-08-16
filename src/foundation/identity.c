/*
 * identity.c — NEXT-STEPS §4 Phase 0, contract C1.
 *
 * Pure functions over strings. No store, no allocation, no I/O. The design and
 * every "why" lives in identity.h; this file only has to be right.
 */
#include "foundation/identity.h"
#include "foundation/constants.h"
#include "foundation/sha256.h"
#include "foundation/str_util.h"

#include <stdio.h>
#include <string.h>

/* The equality of this hash's width with the vector store's is asserted at
 * compile time in ask_embed.c, on the ask side — foundation does not include
 * upward. That assert is what stops the two halves from quietly hashing to
 * different widths and leaving every anchor without its vector. */

enum {
    ID_SCHEME_LEN = 4,     /* strlen(HYP_ADDR_SCHEME) */
    ID_ASCII_SPACE = 0x20, /* lowest printable byte */
    ID_ASCII_DEL = 0x7F,
};

/* ── Refusal reasons ─────────────────────────────────────────────── */

const char *hyp_addr_err_str(hyp_addr_err_t err) {
    switch (err) {
    case HYP_ADDR_OK:
        return "ok";
    case HYP_ADDR_ERR_NULL:
        return "a required argument was NULL";
    case HYP_ADDR_ERR_WORKSPACE:
        return "workspace is empty, too long, or not a valid project name";
    case HYP_ADDR_ERR_REPO:
        return "repo is empty, too long, or not a valid project name";
    case HYP_ADDR_ERR_QN_EMPTY:
        return "qualified name is empty";
    case HYP_ADDR_ERR_QN_CONTROL:
        return "qualified name contains a control byte";
    case HYP_ADDR_ERR_QN_TOO_LONG:
        return "qualified name is longer than the address can carry";
    case HYP_ADDR_ERR_SCHEME:
        return "not an address: missing the \"" HYP_ADDR_SCHEME "\" scheme";
    case HYP_ADDR_ERR_NO_NODE:
        return "not an address: no node component";
    case HYP_ADDR_ERR_SCOPE_MISMATCH:
        return "scope mismatch: a repo on a rendezvous name, or none on a repo name";
    }
    return "unknown identity error";
}

const char *hyp_addr_rel_str(hyp_addr_rel_t rel) {
    switch (rel) {
    case HYP_ADDR_REL_INVALID:
        return "invalid";
    case HYP_ADDR_REL_SAME:
        return "same";
    case HYP_ADDR_REL_EDITED:
        return "edited";
    case HYP_ADDR_REL_CONTENT_ONLY:
        return "content-only";
    case HYP_ADDR_REL_UNRELATED:
        return "unrelated";
    }
    return "unknown";
}

/* ── Field validation ────────────────────────────────────────────── */

/* A slug is a project name. The character set is NOT restated here — it is
 * hyp_validate_project_name's, and a second copy is a second thing to drift. */
static bool id_slug_ok(const char *slug) {
    if (!slug || slug[0] == '\0') {
        return false;
    }
    if (strlen(slug) > (size_t)HYP_ADDR_SLUG_MAX) {
        return false;
    }
    return hyp_validate_project_name(slug);
}

static hyp_addr_err_t id_qn_check(const char *qn) {
    if (!qn) {
        return HYP_ADDR_ERR_NULL;
    }
    if (qn[0] == '\0') {
        return HYP_ADDR_ERR_QN_EMPTY;
    }
    if (strlen(qn) > (size_t)HYP_ADDR_QN_MAX) {
        return HYP_ADDR_ERR_QN_TOO_LONG;
    }
    for (const unsigned char *p = (const unsigned char *)qn; *p; p++) {
        if (*p < ID_ASCII_SPACE || *p == ID_ASCII_DEL) {
            return HYP_ADDR_ERR_QN_CONTROL;
        }
    }
    return HYP_ADDR_OK;
}

/* ── Scope derivation ────────────────────────────────────────────── */

/* Is `qn` rooted at `repo` — i.e. exactly the slug, or the slug followed by a
 * dot? That is what hyp_pipeline_fqn_compute() produces for every node it
 * names. */
static bool id_qn_rooted_at(const char *repo, const char *qn) {
    if (!repo || !qn || repo[0] == '\0') {
        return false;
    }
    size_t n = strlen(repo);
    if (strncmp(qn, repo, n) != 0) {
        return false;
    }
    return qn[n] == '\0' || qn[n] == '.';
}

/* Does the first dot-segment carry a non-empty "__tag__" prefix? That is the
 * shape every cross-repo rendezvous QN in the tree is built with, and it is the
 * shape a project-rooted QN never has (a project slug cannot contain a closing
 * "__" pair AND stop at the first dot without being that slug itself, which the
 * rooted test above catches first). */
static bool id_qn_has_tag(const char *qn) {
    if (!qn || qn[0] != '_' || qn[1] != '_') {
        return false;
    }
    for (const char *p = qn + PAIR_LEN; *p != '\0' && *p != '.'; p++) {
        if (p[0] == '_' && p[1] == '_') {
            return p > qn + PAIR_LEN; /* "____" is an empty tag, not a namespace */
        }
    }
    return false;
}

hyp_addr_scope_t hyp_addr_qn_scope(const char *repo, const char *qn) {
    if (!qn || qn[0] == '\0') {
        return HYP_ADDR_SCOPE_REPO;
    }
    if (id_qn_rooted_at(repo, qn)) {
        return HYP_ADDR_SCOPE_REPO;
    }
    return id_qn_has_tag(qn) ? HYP_ADDR_SCOPE_WORKSPACE : HYP_ADDR_SCOPE_REPO;
}

/* ── Compose ─────────────────────────────────────────────────────── */

hyp_addr_err_t hyp_addr_init(hyp_addr_t *out, const char *workspace, const char *repo,
                             const char *qn) {
    if (!out) {
        return HYP_ADDR_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));
    if (!repo) {
        return HYP_ADDR_ERR_NULL;
    }
    /* A single repo is a workspace of one. One code path, one default — never a
     * second mode that can disagree with the first. */
    const char *ws = workspace ? workspace : repo;
    if (!id_slug_ok(ws)) {
        return HYP_ADDR_ERR_WORKSPACE;
    }
    if (!id_slug_ok(repo)) {
        return HYP_ADDR_ERR_REPO;
    }
    hyp_addr_err_t qerr = id_qn_check(qn);
    if (qerr != HYP_ADDR_OK) {
        return qerr;
    }

    out->scope = hyp_addr_qn_scope(repo, qn);
    snprintf(out->workspace, sizeof(out->workspace), "%s", ws);
    if (out->scope == HYP_ADDR_SCOPE_REPO) {
        snprintf(out->repo, sizeof(out->repo), "%s", repo);
    }
    snprintf(out->qn, sizeof(out->qn), "%s", qn);
    return HYP_ADDR_OK;
}

/* ── Render ──────────────────────────────────────────────────────── */

bool hyp_addr_format(const hyp_addr_t *addr, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return false;
    }
    out[0] = '\0';
    if (!addr) {
        return false;
    }
    /* A struct that could not have come out of hyp_addr_init renders nothing. */
    if (!id_slug_ok(addr->workspace) || id_qn_check(addr->qn) != HYP_ADDR_OK) {
        return false;
    }
    if (addr->scope == HYP_ADDR_SCOPE_REPO) {
        if (!id_slug_ok(addr->repo)) {
            return false;
        }
    } else if (addr->repo[0] != '\0') {
        return false;
    }

    int n;
    if (addr->scope == HYP_ADDR_SCOPE_REPO) {
        n = snprintf(out, out_sz, "%s%c%s%c%s%c%s", HYP_ADDR_SCHEME, HYP_ADDR_SEP_SCHEME,
                     addr->workspace, HYP_ADDR_SEP_REPO, addr->repo, HYP_ADDR_SEP_NODE, addr->qn);
    } else {
        n = snprintf(out, out_sz, "%s%c%s%c%s", HYP_ADDR_SCHEME, HYP_ADDR_SEP_SCHEME,
                     addr->workspace, HYP_ADDR_SEP_NODE, addr->qn);
    }
    /* Refuse rather than truncate: a truncated address is a well-formed address
     * for a DIFFERENT node. */
    if (n < 0 || (size_t)n >= out_sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* ── Parse ───────────────────────────────────────────────────────── */

hyp_addr_err_t hyp_addr_parse(const char *s, hyp_addr_t *out) {
    if (!out) {
        return HYP_ADDR_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));
    if (!s) {
        return HYP_ADDR_ERR_NULL;
    }
    if (strncmp(s, HYP_ADDR_SCHEME, ID_SCHEME_LEN) != 0 ||
        s[ID_SCHEME_LEN] != HYP_ADDR_SEP_SCHEME) {
        return HYP_ADDR_ERR_SCHEME;
    }
    const char *head = s + ID_SCHEME_LEN + SKIP_ONE;

    /* Split at the FIRST '#'. Neither slug can contain one, so this separates
     * head from qualified name exactly, whatever the qualified name holds. */
    const char *hash = strchr(head, HYP_ADDR_SEP_NODE);
    if (!hash) {
        return HYP_ADDR_ERR_NO_NODE;
    }
    const char *qn = hash + SKIP_ONE;
    hyp_addr_err_t qerr = id_qn_check(qn);
    if (qerr != HYP_ADDR_OK) {
        return qerr;
    }

    /* The head is workspace, optionally '/' repo. A slug cannot contain '/'. */
    char ws[HYP_ADDR_SLUG_MAX + 1 + SKIP_ONE];
    char repo[HYP_ADDR_SLUG_MAX + 1 + SKIP_ONE];
    ws[0] = '\0';
    repo[0] = '\0';
    const char *slash = memchr(head, HYP_ADDR_SEP_REPO, (size_t)(hash - head));
    size_t ws_len = (size_t)((slash ? slash : hash) - head);
    if (ws_len >= sizeof(ws)) {
        return HYP_ADDR_ERR_WORKSPACE;
    }
    memcpy(ws, head, ws_len);
    ws[ws_len] = '\0';
    if (!id_slug_ok(ws)) {
        return HYP_ADDR_ERR_WORKSPACE;
    }
    if (slash) {
        size_t repo_len = (size_t)(hash - (slash + SKIP_ONE));
        if (repo_len >= sizeof(repo)) {
            return HYP_ADDR_ERR_REPO;
        }
        memcpy(repo, slash + SKIP_ONE, repo_len);
        repo[repo_len] = '\0';
        if (!id_slug_ok(repo)) {
            return HYP_ADDR_ERR_REPO;
        }
    }

    /* Accept exactly what hyp_addr_init would have produced, and nothing else.
     * Without this the parser would admit strings the composer cannot emit —
     * a rendezvous name pinned to one repo, or a repo-scoped name floating free
     * — and the two ends of the contract would have drifted on day one. */
    hyp_addr_scope_t want = hyp_addr_qn_scope(repo, qn);
    if (slash ? (want != HYP_ADDR_SCOPE_REPO) : (want != HYP_ADDR_SCOPE_WORKSPACE)) {
        return HYP_ADDR_ERR_SCOPE_MISMATCH;
    }

    out->scope = want;
    snprintf(out->workspace, sizeof(out->workspace), "%s", ws);
    snprintf(out->repo, sizeof(out->repo), "%s", repo);
    snprintf(out->qn, sizeof(out->qn), "%s", qn);
    return HYP_ADDR_OK;
}

/* ── Comparison ──────────────────────────────────────────────────── */

bool hyp_addr_equal(const hyp_addr_t *a, const hyp_addr_t *b) {
    if (!a || !b) {
        return false;
    }
    return a->scope == b->scope && strcmp(a->workspace, b->workspace) == 0 &&
           strcmp(a->repo, b->repo) == 0 && strcmp(a->qn, b->qn) == 0;
}

bool hyp_addr_same_repo(const hyp_addr_t *a, const hyp_addr_t *b) {
    if (!a || !b) {
        return false;
    }
    if (a->scope != HYP_ADDR_SCOPE_REPO || b->scope != HYP_ADDR_SCOPE_REPO) {
        return false;
    }
    return strcmp(a->workspace, b->workspace) == 0 && strcmp(a->repo, b->repo) == 0;
}

/* ── Span hash ───────────────────────────────────────────────────── */

void hyp_addr_span_hash(const char *text, char *out) {
    if (!out) {
        return;
    }
    char hex[HYP_SHA256_HEX_LEN + SKIP_ONE];
    hyp_sha256_hex(text ? text : "", text ? strlen(text) : 0, hex);
    memcpy(out, hex, HYP_ADDR_SPAN_HASH_LEN);
    out[HYP_ADDR_SPAN_HASH_LEN] = '\0';
}

/* ── Relation ────────────────────────────────────────────────────── */

static bool id_hash_ok(const char *h) {
    if (!h) {
        return false;
    }
    size_t i = 0;
    for (; h[i] != '\0'; i++) {
        bool hex = (h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f');
        if (!hex || i >= (size_t)HYP_ADDR_SPAN_HASH_LEN) {
            return false;
        }
    }
    return i == (size_t)HYP_ADDR_SPAN_HASH_LEN;
}

hyp_addr_rel_t hyp_addr_relate(const hyp_addr_t *before, const char *before_hash,
                               const hyp_addr_t *after, const char *after_hash) {
    if (!before || !after || !id_hash_ok(before_hash) || !id_hash_ok(after_hash)) {
        return HYP_ADDR_REL_INVALID;
    }
    bool same_addr = hyp_addr_equal(before, after);
    bool same_hash = strcmp(before_hash, after_hash) == 0;
    if (same_addr) {
        return same_hash ? HYP_ADDR_REL_SAME : HYP_ADDR_REL_EDITED;
    }
    /* Deliberately its OWN value and not a match. See the header: a copy-paste
     * produces this too, and re-attaching on it is the false positive that is
     * worse than losing the anchor. */
    return same_hash ? HYP_ADDR_REL_CONTENT_ONLY : HYP_ADDR_REL_UNRELATED;
}
