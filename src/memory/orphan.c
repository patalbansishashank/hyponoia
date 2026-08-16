/*
 * orphan.c — the read path over anchored memory.
 *
 * Every "why" lives in orphan.h. The one structural note worth repeating at
 * the read site: there is no function in this file that writes to a record
 * store. Detection cannot mutate the thing it is detecting, and the way to
 * guarantee that is not to be careful, it is to have nowhere to put the call.
 */
#include "memory/orphan.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct hyp_orphan_view {
    hyp_record_set_t *set; /* owns the records the entries borrow */
    hyp_orphan_entry_t *entries;
    int count;
};

const char *hyp_orphan_status_reason(hyp_orphan_status_t status) {
    switch (status) {
    case HYP_ORPHAN_OK:
        return "ok";
    case HYP_ORPHAN_ERR_NULL:
        return "a required argument was NULL";
    case HYP_ORPHAN_ERR_QUERY:
        return "a view of unanchored records is not a view: an unanchored record has no anchor "
               "status";
    case HYP_ORPHAN_ERR_STORE:
        return "the record store refused the query";
    case HYP_ORPHAN_ERR_NOT_ORPHANED:
        return "the anchor attaches, so there is no orphaning to observe";
    case HYP_ORPHAN_ERR_RECORD:
        return "the record contract refused the signal";
    case HYP_ORPHAN_ERR_ALLOC:
        return "out of memory";
    }
    return "unknown";
}

/* ── Building the view ───────────────────────────────────────────── */

hyp_orphan_status_t hyp_orphan_view_build(hyp_record_store_t *records, hyp_store_t *index,
                                          const char *workspace,
                                          const hyp_record_store_query_t *query,
                                          hyp_orphan_view_t **out) {
    if (!out) {
        return HYP_ORPHAN_ERR_NULL;
    }
    *out = NULL;
    if (!records || !index) {
        return HYP_ORPHAN_ERR_NULL;
    }

    hyp_record_store_query_t q;
    if (query) {
        q = *query;
    } else {
        memset(&q, 0, sizeof(q));
    }
    if (q.unanchored_only) {
        return HYP_ORPHAN_ERR_QUERY;
    }
    q.anchored_only = true;

    hyp_record_set_t *set = NULL;
    if (hyp_record_store_query(records, &q, &set) != HYP_RECORD_STORE_OK) {
        return HYP_ORPHAN_ERR_STORE;
    }

    hyp_orphan_view_t *view = calloc(1, sizeof(*view));
    if (!view) {
        hyp_record_set_free(set);
        return HYP_ORPHAN_ERR_ALLOC;
    }
    view->set = set;
    size_t n = hyp_record_set_count(set);
    if (n > 0) {
        view->entries = calloc(n, sizeof(*view->entries));
        if (!view->entries) {
            hyp_orphan_view_free(view);
            return HYP_ORPHAN_ERR_ALLOC;
        }
    }

    /* Every matched record gets an entry, whatever it classifies as. A sweep
     * that only kept the interesting ones would be deciding what the reader
     * gets to see, and dropping an orphan is the one thing this module exists
     * to make impossible. */
    for (size_t i = 0; i < n; i++) {
        const hyp_record_t *rec = hyp_record_set_at(set, i);
        hyp_orphan_entry_t *e = &view->entries[view->count];
        e->record = rec;
        hyp_anchor_resolve(index, workspace, rec ? rec->anchor : NULL, &e->res);
        view->count++;
    }

    *out = view;
    return HYP_ORPHAN_OK;
}

void hyp_orphan_view_free(hyp_orphan_view_t *view) {
    if (!view) {
        return;
    }
    for (int i = 0; i < view->count; i++) {
        hyp_anchor_res_free(&view->entries[i].res);
    }
    free(view->entries);
    hyp_record_set_free(view->set);
    free(view);
}

int hyp_orphan_view_count(const hyp_orphan_view_t *view) {
    return view ? view->count : 0;
}

const hyp_orphan_entry_t *hyp_orphan_view_at(const hyp_orphan_view_t *view, int index) {
    if (!view || index < 0 || index >= view->count) {
        return NULL;
    }
    return &view->entries[index];
}

int hyp_orphan_view_count_status(const hyp_orphan_view_t *view, hyp_anchor_status_t status) {
    int n = 0;
    for (int i = 0; view && i < view->count; i++) {
        if (view->entries[i].res.status == status) {
            n++;
        }
    }
    return n;
}

/* ── Presentation ────────────────────────────────────────────────── */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool failed;
} orph_buf_t;

static void orph_addf(orph_buf_t *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void orph_addf(orph_buf_t *b, const char *fmt, ...) {
    if (b->failed) {
        return;
    }
    for (;;) {
        size_t room = b->cap - b->len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->buf ? b->buf + b->len : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->failed = true;
            return;
        }
        if ((size_t)n < room) {
            b->len += (size_t)n;
            return;
        }
        size_t want = b->len + (size_t)n + 1;
        size_t cap = b->cap ? b->cap : 256;
        while (cap < want) {
            cap *= 2;
        }
        char *grown = realloc(b->buf, cap);
        if (!grown) {
            b->failed = true;
            return;
        }
        b->buf = grown;
        b->cap = cap;
    }
}

/* Does this status hand the reader a location it may treat as attached? Only
 * the two that resolved. Everything else prints `reattached: no`. */
static bool orph_attaches(hyp_anchor_status_t status) {
    return status == HYP_ANCHOR_RESOLVED || status == HYP_ANCHOR_RESOLVED_EDITED;
}

static void orph_render_into(orph_buf_t *b, const hyp_orphan_entry_t *e) {
    const hyp_anchor_res_t *r = &e->res;
    orph_addf(b, "record: %s\n", e->record && e->record->id ? e->record->id : "");
    orph_addf(b, "anchor: %s\n",
              e->record && e->record->anchor ? e->record->anchor : "");
    orph_addf(b, "anchor_status: %s\n", hyp_anchor_status_str(r->status));

    if (orph_attaches(r->status)) {
        orph_addf(b, "attached: %s/%s:%d-%d\n", r->project, r->file_path, r->start_line,
                  r->end_line);
        orph_addf(b, "content: %s\n", hyp_addr_rel_str(r->relation));
    } else {
        /* The one line that says a decision was NOT made. It prints for every
         * status that does not attach, including the ones carrying candidates
         * that look exactly like an answer. */
        orph_addf(b, "reattached: no\n");
    }

    if (r->status == HYP_ANCHOR_ORPHANED || r->status == HYP_ANCHOR_AMBIGUOUS) {
        if (r->status == HYP_ANCHOR_ORPHANED && !r->anchor.has_hash) {
            /* Absent, not empty: no content was ever recorded for this anchor,
             * so no scan could run. "none" here would claim the workspace was
             * searched and holds nothing, which is a different and false
             * statement. */
            orph_addf(b, "candidates: not-scanned\n");
        } else if (r->candidate_count == 0) {
            orph_addf(b, "candidates: none\n");
        } else {
            orph_addf(b, "candidates: %d\n", r->candidate_count);
        }
        for (int i = 0; i < r->candidate_count; i++) {
            const hyp_anchor_candidate_t *c = &r->candidates[i];
            orph_addf(b, "candidate: %s %s/%s:%d-%d evidence=%s\n", c->address, c->project,
                      c->file_path, c->start_line, c->end_line, hyp_addr_rel_str(c->relation));
        }
    }
    if (r->scan_skipped > 0) {
        /* Disclosed, never absorbed: with a span unread the list is at most a
         * floor, and a reader who thinks it is a list will conclude "no
         * candidate" from an incomplete search. */
        orph_addf(b, "candidates_are_a_floor: %d span(s) unread\n", r->scan_skipped);
    }
    if (r->reason[0] != '\0') {
        orph_addf(b, "reason: %s\n", r->reason);
    }
}

char *hyp_orphan_render_entry(const hyp_orphan_entry_t *entry) {
    if (!entry) {
        return NULL;
    }
    orph_buf_t b = {0};
    orph_render_into(&b, entry);
    if (b.failed) {
        free(b.buf);
        return NULL;
    }
    if (!b.buf) {
        b.buf = calloc(1, 1);
    }
    return b.buf;
}

char *hyp_orphan_render(const hyp_orphan_view_t *view) {
    if (!view) {
        return NULL;
    }
    orph_buf_t b = {0};
    for (int i = 0; i < view->count; i++) {
        if (i > 0) {
            orph_addf(&b, "\n");
        }
        orph_render_into(&b, &view->entries[i]);
    }
    if (b.failed) {
        free(b.buf);
        return NULL;
    }
    if (!b.buf) {
        b.buf = calloc(1, 1);
    }
    return b.buf;
}

/* ── The signal ──────────────────────────────────────────────────── */

hyp_orphan_status_t hyp_orphan_signal_build(const hyp_orphan_entry_t *entry, const char *author,
                                            int64_t timestamp_ms, const hyp_record_t **out) {
    if (!out) {
        return HYP_ORPHAN_ERR_NULL;
    }
    *out = NULL;
    if (!entry || !entry->record || !author) {
        return HYP_ORPHAN_ERR_NULL;
    }
    if (orph_attaches(entry->res.status)) {
        return HYP_ORPHAN_ERR_NOT_ORPHANED;
    }
    char *rendered = hyp_orphan_render_entry(entry);
    if (!rendered) {
        return HYP_ORPHAN_ERR_ALLOC;
    }
    hyp_record_input_t in = {
        .kind = HYP_RECORD_SIGNAL,
        .author = author,
        .timestamp_ms = timestamp_ms,
        .content = rendered,
        /* No anchor. The observation is about a record, and the address it
         * names is the one that does not resolve. */
        .anchor = NULL,
        .origin = NULL,
        .thread = NULL,
        .parent = entry->record->id,
        .redactions = 0,
    };
    hyp_record_status_t rc = hyp_record_build(&in, out);
    free(rendered);
    if (rc != HYP_RECORD_OK) {
        *out = NULL;
        return HYP_ORPHAN_ERR_RECORD;
    }
    return HYP_ORPHAN_OK;
}
