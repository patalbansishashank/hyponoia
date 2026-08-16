/*
 * anchor.c — NEXT-STEPS §4 Phase 1, unit C3u.
 *
 * Symbol identity + content hash, never line numbers. The design and every
 * "why" lives in anchor.h; the comparison semantics live in identity.h
 * (hyp_addr_relate) and are consumed here, not restated.
 */
#include "memory/anchor.h"

#include "ask/ask_embed.h"
#include "foundation/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    AN_HASH_LEN = HYP_ADDR_SPAN_HASH_LEN,
};

/* ── The anchor string ───────────────────────────────────────────── */

static bool an_hash_ok(const char *h) {
    if (!h) {
        return false;
    }
    for (int i = 0; i < AN_HASH_LEN; i++) {
        bool hex = (h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f');
        if (!hex) {
            return false;
        }
    }
    return h[AN_HASH_LEN] == '\0';
}

bool hyp_anchor_format(const hyp_addr_t *addr, const char *span_hash, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return false;
    }
    out[0] = '\0';
    if (!addr) {
        return false;
    }
    char addr_str[HYP_ADDR_MAX + 1];
    if (!hyp_addr_format(addr, addr_str, sizeof(addr_str))) {
        return false;
    }
    int n;
    if (span_hash) {
        /* A malformed hash is refused, not passed through: an anchor is a
         * claim, and a claim whose evidence cannot be re-checked is noise. */
        if (!an_hash_ok(span_hash)) {
            return false;
        }
        n = snprintf(out, out_sz, "%s@%s", span_hash, addr_str);
    } else {
        n = snprintf(out, out_sz, "%s", addr_str);
    }
    if (n < 0 || (size_t)n >= out_sz) {
        out[0] = '\0';
        return false;
    }
    return true;
}

hyp_addr_err_t hyp_anchor_parse(const char *s, hyp_anchor_t *out) {
    if (!out) {
        return HYP_ADDR_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));
    if (!s) {
        return HYP_ADDR_ERR_NULL;
    }
    /* The two grammars are disjoint at byte 1 ('y' is not hex — anchor.h), so
     * this is a decision, not a heuristic: a leading scheme is a bare
     * address, a leading hash must be followed by '@' and an address, and
     * anything else is neither. */
    const char *addr_part = s;
    if (strncmp(s, HYP_ADDR_SCHEME, strlen(HYP_ADDR_SCHEME)) != 0) {
        size_t len = strlen(s);
        if (len < (size_t)AN_HASH_LEN + 1 || s[AN_HASH_LEN] != '@') {
            return HYP_ADDR_ERR_SCHEME;
        }
        char hash[AN_HASH_LEN + 1];
        memcpy(hash, s, (size_t)AN_HASH_LEN);
        hash[AN_HASH_LEN] = '\0';
        if (!an_hash_ok(hash)) {
            return HYP_ADDR_ERR_SCHEME;
        }
        memcpy(out->span_hash, hash, sizeof(hash));
        out->has_hash = true;
        addr_part = s + AN_HASH_LEN + 1;
    }
    hyp_addr_err_t err = hyp_addr_parse(addr_part, &out->addr);
    if (err != HYP_ADDR_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }
    return HYP_ADDR_OK;
}

/* ── Classification ──────────────────────────────────────────────── */

const char *hyp_anchor_status_str(hyp_anchor_status_t status) {
    switch (status) {
    case HYP_ANCHOR_ERROR:
        return "error";
    case HYP_ANCHOR_RESOLVED:
        return "resolved";
    case HYP_ANCHOR_RESOLVED_EDITED:
        return "resolved-edited";
    case HYP_ANCHOR_ORPHANED:
        return "orphaned";
    case HYP_ANCHOR_UNKNOWN_WORKSPACE:
        return "unknown-workspace";
    }
    return "unknown";
}

/* Set ERROR with a reason. Returns HYP_ANCHOR_ERROR for tail-calling. */
static hyp_anchor_status_t an_error(hyp_anchor_res_t *out, const char *fmt, const char *a,
                                    const char *b) {
    out->status = HYP_ANCHOR_ERROR;
    snprintf(out->reason, sizeof(out->reason), fmt, a ? a : "", b ? b : "");
    return HYP_ANCHOR_ERROR;
}

/* The current content hash of a node's span, via the project's root. Returns
 * false when the span cannot be read — which the caller must treat as "could
 * not tell", never as any relation. */
static bool an_node_current_hash(hyp_store_t *store, const char *project, const char *rel_path,
                                 int start_line, int end_line, char *out_hash) {
    out_hash[0] = '\0';
    if (!project || !rel_path) {
        return false;
    }
    hyp_project_t proj = {0};
    if (hyp_store_get_project(store, project, &proj) != HYP_STORE_OK) {
        return false;
    }
    char abs_path[HYP_PATH_MAX];
    int n = snprintf(abs_path, sizeof(abs_path), "%s/%s", proj.root_path ? proj.root_path : "",
                     rel_path);
    hyp_project_free_fields(&proj);
    if (n < 0 || (size_t)n >= sizeof(abs_path)) {
        return false;
    }
    char *text = hyp_ask_read_span_lines(abs_path, start_line, end_line, NULL);
    if (!text) {
        return false;
    }
    hyp_addr_span_hash(text, out_hash);
    free(text);
    return true;
}

/* Fill the "where it is NOW" half of the result from a found node. */
static void an_fill_location(hyp_anchor_res_t *out, const hyp_node_t *node) {
    snprintf(out->project, sizeof(out->project), "%s", node->project ? node->project : "");
    snprintf(out->file_path, sizeof(out->file_path), "%s", node->file_path ? node->file_path : "");
    out->start_line = node->start_line;
    out->end_line = node->end_line;
}

/* Classify a found node against the anchor. `node_project` is the project the
 * QN was found in — for a rendezvous name that need not be a repo the address
 * mentions, and the address it re-derives collapses to the anchor's own. */
static hyp_anchor_status_t an_classify_found(hyp_store_t *store, hyp_anchor_res_t *out,
                                             const hyp_node_t *node) {
    an_fill_location(out, node);
    bool readable = an_node_current_hash(store, node->project, node->file_path, node->start_line,
                                         node->end_line, out->current_hash);
    if (!out->anchor.has_hash) {
        /* No content observation was recorded, so the QN alone decides — and
         * it was found. The current hash is reported when readable so a
         * caller can record a fresher anchor, but nothing hinges on it. */
        out->status = HYP_ANCHOR_RESOLVED;
        return out->status;
    }
    if (!readable) {
        return an_error(out,
                        "cannot compare recorded span hash: span of \"%s\" is unreadable "
                        "in the working tree%s",
                        node->qualified_name, NULL);
    }
    /* The comparison is the identity contract's, not a local re-statement.
     * The node's own address re-derived here equals the anchor's (same
     * workspace, same scope derivation), so only SAME or EDITED can come
     * back; anything else means the two ends disagree and is refused. */
    hyp_addr_t node_addr;
    if (hyp_addr_init(&node_addr, out->anchor.addr.workspace, node->project,
                      node->qualified_name) != HYP_ADDR_OK) {
        return an_error(out, "node \"%s\" cannot form an address%s", node->qualified_name, NULL);
    }
    hyp_addr_rel_t rel =
        hyp_addr_relate(&out->anchor.addr, out->anchor.span_hash, &node_addr, out->current_hash);
    out->relation = rel;
    if (rel == HYP_ADDR_REL_SAME) {
        out->status = HYP_ANCHOR_RESOLVED;
    } else if (rel == HYP_ADDR_REL_EDITED) {
        out->status = HYP_ANCHOR_RESOLVED_EDITED;
    } else {
        return an_error(out, "identity contract returned \"%s\" for a found node%s",
                        hyp_addr_rel_str(rel), NULL);
    }
    return out->status;
}

/* ── The candidate scan ──────────────────────────────────────────── */

static int an_candidate_cmp(const void *a, const void *b) {
    return strcmp(((const hyp_anchor_candidate_t *)a)->address,
                  ((const hyp_anchor_candidate_t *)b)->address);
}

/* Gather every node in the workspace whose CURRENT span content hashes to the
 * anchor's recorded hash. Evidence, never a decision: the caller has already
 * classified the anchor ORPHANED and nothing here changes that. */
static void an_scan_candidates(hyp_store_t *store, hyp_anchor_res_t *out) {
    hyp_project_t *projects = NULL;
    int project_count = 0;
    if (hyp_store_list_projects(store, &projects, &project_count) != HYP_STORE_OK) {
        out->scan_skipped++; /* the whole scan; candidates are a floor */
        return;
    }
    int cap = 0;
    for (int pi = 0; pi < project_count; pi++) {
        const char *project = projects[pi].name;
        const char *root = projects[pi].root_path ? projects[pi].root_path : "";
        char **files = NULL;
        int file_count = 0;
        if (hyp_store_list_files(store, project, &files, &file_count) != HYP_STORE_OK) {
            out->scan_skipped++;
            continue;
        }
        for (int fi = 0; fi < file_count; fi++) {
            hyp_node_t *nodes = NULL;
            int node_count = 0;
            if (hyp_store_find_nodes_by_file(store, project, files[fi], &nodes, &node_count) !=
                HYP_STORE_OK) {
                out->scan_skipped++;
                continue;
            }
            char abs_path[HYP_PATH_MAX];
            int pn = snprintf(abs_path, sizeof(abs_path), "%s/%s", root, files[fi]);
            for (int ni = 0; ni < node_count; ni++) {
                const hyp_node_t *node = &nodes[ni];
                if (pn < 0 || (size_t)pn >= sizeof(abs_path) || !node->qualified_name) {
                    out->scan_skipped++;
                    continue;
                }
                char *text =
                    hyp_ask_read_span_lines(abs_path, node->start_line, node->end_line, NULL);
                if (!text) {
                    out->scan_skipped++;
                    continue;
                }
                char cur[HYP_ADDR_SPAN_HASH_LEN + 1];
                hyp_addr_span_hash(text, cur);
                free(text);
                if (strcmp(cur, out->anchor.span_hash) != 0) {
                    continue;
                }
                /* A content match. Its address is re-derived, not assumed —
                 * a node that cannot form one cannot be presented. */
                hyp_addr_t cand_addr;
                char cand_str[HYP_ADDR_MAX + 1];
                if (hyp_addr_init(&cand_addr, out->anchor.addr.workspace, project,
                                  node->qualified_name) != HYP_ADDR_OK ||
                    !hyp_addr_format(&cand_addr, cand_str, sizeof(cand_str))) {
                    out->scan_skipped++;
                    continue;
                }
                if (out->candidate_count == cap) {
                    int new_cap = cap == 0 ? 4 : cap * 2;
                    hyp_anchor_candidate_t *grown =
                        realloc(out->candidates, (size_t)new_cap * sizeof(*grown));
                    if (!grown) {
                        out->scan_skipped++;
                        continue;
                    }
                    out->candidates = grown;
                    cap = new_cap;
                }
                hyp_anchor_candidate_t *c = &out->candidates[out->candidate_count];
                memset(c, 0, sizeof(*c));
                snprintf(c->address, sizeof(c->address), "%s", cand_str);
                snprintf(c->qn, sizeof(c->qn), "%s", node->qualified_name);
                snprintf(c->project, sizeof(c->project), "%s", project ? project : "");
                snprintf(c->file_path, sizeof(c->file_path), "%s",
                         node->file_path ? node->file_path : "");
                c->start_line = node->start_line;
                c->end_line = node->end_line;
                snprintf(c->label, sizeof(c->label), "%s", node->label ? node->label : "");
                c->relation = HYP_ADDR_REL_CONTENT_ONLY;
                out->candidate_count++;
            }
            hyp_store_free_nodes(nodes, node_count);
        }
        for (int fi = 0; fi < file_count; fi++) {
            free(files[fi]);
        }
        free(files);
    }
    hyp_store_free_projects(projects, project_count);
    if (out->candidate_count > 1) {
        qsort(out->candidates, (size_t)out->candidate_count, sizeof(*out->candidates),
              an_candidate_cmp);
    }
}

/* ── Resolution ──────────────────────────────────────────────────── */

hyp_anchor_status_t hyp_anchor_resolve(hyp_store_t *store, const char *workspace,
                                       const char *anchor, hyp_anchor_res_t *out) {
    if (!out) {
        return HYP_ANCHOR_ERROR;
    }
    memset(out, 0, sizeof(*out));
    out->relation = HYP_ADDR_REL_INVALID;
    if (!store || !anchor) {
        return an_error(out, "a required argument was NULL%s%s", NULL, NULL);
    }

    hyp_addr_err_t perr = hyp_anchor_parse(anchor, &out->anchor);
    if (perr != HYP_ADDR_OK) {
        return an_error(out, "not an anchor: %s%s", hyp_addr_err_str(perr), NULL);
    }
    const hyp_addr_t *addr = &out->anchor.addr;

    /* The workspace gate, before any lookup. An anchor from a workspace this
     * index does not serve must fail VISIBLY, naming the workspace — an
     * empty result would read as "nothing attached here", which is a claim
     * about the wrong universe (§4 assertion table, C3u row). */
    if (workspace) {
        if (strcmp(workspace, addr->workspace) != 0) {
            out->status = HYP_ANCHOR_UNKNOWN_WORKSPACE;
            snprintf(out->reason, sizeof(out->reason),
                     "anchor names unknown workspace \"%s\"; this index serves workspace \"%s\"",
                     addr->workspace, workspace);
            return out->status;
        }
    } else {
        /* Workspace of one: the workspace id IS a repo's slug (identity.h),
         * so "known" means "an indexed project by that name". */
        hyp_project_t proj = {0};
        int rc = hyp_store_get_project(store, addr->workspace, &proj);
        if (rc == HYP_STORE_OK) {
            hyp_project_free_fields(&proj);
        } else if (rc == HYP_STORE_NOT_FOUND) {
            out->status = HYP_ANCHOR_UNKNOWN_WORKSPACE;
            snprintf(out->reason, sizeof(out->reason),
                     "anchor names unknown workspace \"%s\"; no indexed project matches it",
                     addr->workspace);
            return out->status;
        } else {
            return an_error(out, "store failed looking up workspace \"%s\"%s", addr->workspace,
                            NULL);
        }
    }

    /* Symbol identity. ONE code path for both scopes; the scope (derived from
     * shape, I2) only chooses the project set the QN is looked up in. */
    if (addr->scope == HYP_ADDR_SCOPE_REPO) {
        hyp_node_t node = {0};
        int rc = hyp_store_find_node_by_qn(store, addr->repo, addr->qn, &node);
        if (rc == HYP_STORE_OK) {
            hyp_anchor_status_t st = an_classify_found(store, out, &node);
            hyp_node_free_fields(&node);
            return st;
        }
        if (rc != HYP_STORE_NOT_FOUND) {
            return an_error(out, "store failed looking up \"%s\"%s", addr->qn, NULL);
        }
    } else {
        /* Rendezvous: the QN found in ANY project resolves — two repos
         * sharing the name is the point. With a recorded hash, prefer the
         * copy whose content still matches; several copies existing is the
         * namespace working, not an ambiguity. */
        hyp_project_t *projects = NULL;
        int project_count = 0;
        if (hyp_store_list_projects(store, &projects, &project_count) != HYP_STORE_OK) {
            return an_error(out, "store failed listing projects%s%s", NULL, NULL);
        }
        hyp_node_t first = {0};
        bool have_first = false;
        bool first_readable = false;
        hyp_anchor_status_t st = HYP_ANCHOR_ERROR;
        bool decided = false;
        for (int pi = 0; pi < project_count && !decided; pi++) {
            hyp_node_t node = {0};
            int rc = hyp_store_find_node_by_qn(store, projects[pi].name, addr->qn, &node);
            if (rc == HYP_STORE_ERR) {
                hyp_store_free_projects(projects, project_count);
                if (have_first) {
                    hyp_node_free_fields(&first);
                }
                return an_error(out, "store failed looking up \"%s\"%s", addr->qn, NULL);
            }
            if (rc != HYP_STORE_OK) {
                continue;
            }
            if (!out->anchor.has_hash) {
                st = an_classify_found(store, out, &node);
                decided = true;
            } else {
                char cur[HYP_ADDR_SPAN_HASH_LEN + 1];
                bool readable = an_node_current_hash(store, node.project, node.file_path,
                                                     node.start_line, node.end_line, cur);
                if (readable && strcmp(cur, out->anchor.span_hash) == 0) {
                    st = an_classify_found(store, out, &node);
                    decided = true;
                } else if (!have_first || (readable && !first_readable)) {
                    /* Keep the best fallback: a readable copy beats one whose
                     * span cannot be re-read, because the latter can only be
                     * classified ERROR and a real EDITED answer exists. */
                    if (have_first) {
                        hyp_node_free_fields(&first);
                    }
                    first = node;
                    have_first = true;
                    first_readable = readable;
                    continue; /* ownership moved into `first` */
                }
            }
            hyp_node_free_fields(&node);
        }
        hyp_store_free_projects(projects, project_count);
        if (decided) {
            if (have_first) {
                hyp_node_free_fields(&first);
            }
            return st;
        }
        if (have_first) {
            /* Found somewhere, matched nowhere: attached and edited. */
            st = an_classify_found(store, out, &first);
            hyp_node_free_fields(&first);
            return st;
        }
        /* fall through to orphan */
    }

    /* The QN is gone from its universe: ORPHANED. With a recorded hash the
     * workspace is scanned for content matches — candidates with evidence,
     * presented by C4u, re-attached by nobody (I5). */
    out->status = HYP_ANCHOR_ORPHANED;
    if (out->anchor.has_hash) {
        an_scan_candidates(store, out);
    }
    return out->status;
}

void hyp_anchor_res_free(hyp_anchor_res_t *res) {
    if (!res) {
        return;
    }
    free(res->candidates);
    res->candidates = NULL;
    res->candidate_count = 0;
}
