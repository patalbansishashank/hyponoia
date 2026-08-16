/*
 * watched_ingest.c — watched-directory ingest: the generic feed adapter (D5).
 *
 * See the header for the format and the docs page for the long form. Two
 * properties of this file are structural, not stylistic:
 *
 *   - NO CLOCK. Timestamps come from the file or the row does not pass (I6).
 *     There is no <time.h> here and nothing that could substitute for one.
 *   - NO STORE. The scanner parses and yields; building records and keeping
 *     them is the sink's business. That is what lets the feed interface (D3)
 *     mount this adapter without either side learning the other's internals.
 */
#include "ingest/watched_ingest.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson/yyjson.h>

#include "foundation/compat_fs.h"
#include "foundation/constants.h"

/* Windows CRT wants the 64-bit seek spelled differently. */
#ifdef _WIN32
#define hyp_watched_seek64(f, off) _fseeki64((f), (long long)(off), SEEK_SET)
#else
#define hyp_watched_seek64(f, off) fseeko((f), (off_t)(off), SEEK_SET)
#endif

enum { HYP_WATCHED_MAX_PATH = 4096, HYP_WATCHED_MAX_DEPTH = 64, HYP_WATCHED_DETAIL_CAP = 192 };

typedef struct {
    char *relpath;
    hyp_watched_file_state_t state;
    hyp_watched_reason_t reason;
    uint64_t consumed;  /* offset of the first byte not yet ingested */
    uint64_t failed_at; /* line-start offset of the refused line */
    uint64_t rows;      /* rows yielded, lifetime */
    bool seen;          /* found by the current pass */
    char detail[HYP_WATCHED_DETAIL_CAP];
} watched_file_t;

struct hyp_watched_scanner {
    char *root;
    hyp_watched_scrub_fn scrub;
    void *scrub_ctx;
    watched_file_t *files; /* sorted by relpath, so every pass is deterministic */
    size_t count;
    size_t cap;
};

/* What one line did to its file. */
typedef enum {
    LINE_OK = 0, /* consumed (a yielded row, or a blank line) */
    LINE_FAIL,   /* the file fails closed here; reason + detail are set */
    LINE_ABORT   /* the sink or the allocator stopped the whole scan */
} line_result_t;

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = (char *)malloc(n);
    if (copy) {
        memcpy(copy, s, n);
    }
    return copy;
}

const char *hyp_watched_reason_name(hyp_watched_reason_t reason) {
    switch (reason) {
    case HYP_WATCHED_REASON_NONE:
        return "none";
    case HYP_WATCHED_REASON_IO:
        return "io: the file cannot be read";
    case HYP_WATCHED_REASON_SHRUNK:
        return "shrunk: the file is smaller than what was already ingested";
    case HYP_WATCHED_REASON_LINE_TOO_LONG:
        return "line too long: refused, never truncated";
    case HYP_WATCHED_REASON_BAD_JSON:
        return "bad json: a line is not one JSON object";
    case HYP_WATCHED_REASON_BAD_VERSION:
        return "bad version: v is present and not the integer 1";
    case HYP_WATCHED_REASON_BAD_FIELD:
        return "bad field: wrong type, missing required, or unknown kind";
    case HYP_WATCHED_REASON_FORGED_FIELD:
        return "forged field: origin, redactions and id are derived, never supplied";
    case HYP_WATCHED_REASON_ROW_REFUSED:
        return "row refused: the record contract said no";
    case HYP_WATCHED_REASON_SCRUB_FAILED:
        return "scrub failed: an unscrubbed row must not pass";
    case HYP_WATCHED_REASON_ALLOC:
        return "alloc: out of memory";
    default:
        return "unknown reason";
    }
}

bool hyp_watched_origin(const char *relpath, uint64_t line_offset,
                        char out[HYP_RECORD_MAX_ORIGIN + 1], size_t cap) {
    if (relpath == NULL || relpath[0] == '\0' || out == NULL || cap == 0) {
        return false;
    }
    int n = snprintf(out, cap, "wd1:%s@%" PRIu64, relpath, line_offset);
    return n > 0 && (size_t)n < cap;
}

bool hyp_watched_thread(const char *relpath, char out[HYP_RECORD_MAX_THREAD + 1], size_t cap) {
    if (relpath == NULL || relpath[0] == '\0' || out == NULL || cap == 0) {
        return false;
    }
    int n = snprintf(out, cap, "wd1:%s", relpath);
    return n > 0 && (size_t)n < cap;
}

/* ── Per-file state, kept sorted by relpath ──────────────────────────────── */

static watched_file_t *file_find(hyp_watched_scanner_t *s, const char *relpath, size_t *pos_out) {
    size_t lo = 0;
    size_t hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(s->files[mid].relpath, relpath);
        if (cmp == 0) {
            if (pos_out) {
                *pos_out = mid;
            }
            return &s->files[mid];
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (pos_out) {
        *pos_out = lo;
    }
    return NULL;
}

static watched_file_t *file_intern(hyp_watched_scanner_t *s, const char *relpath) {
    size_t pos = 0;
    watched_file_t *found = file_find(s, relpath, &pos);
    if (found) {
        return found;
    }
    if (s->count == s->cap) {
        size_t next = s->cap ? s->cap * 2 : 16;
        watched_file_t *grown = (watched_file_t *)realloc(s->files, next * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        s->files = grown;
        s->cap = next;
    }
    char *owned = dup_cstr(relpath);
    if (!owned) {
        return NULL;
    }
    memmove(&s->files[pos + 1], &s->files[pos], (s->count - pos) * sizeof(*s->files));
    s->count++;
    watched_file_t *slot = &s->files[pos];
    memset(slot, 0, sizeof(*slot));
    slot->relpath = owned;
    snprintf(slot->detail, sizeof(slot->detail), "not yet scanned");
    return slot;
}

static void file_fail(watched_file_t *st, hyp_watched_reason_t reason, uint64_t at, const char *fmt,
                      ...) {
    st->state = HYP_WATCHED_FILE_FAILED;
    st->reason = reason;
    st->failed_at = at;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->detail, sizeof(st->detail), fmt, ap);
    va_end(ap);
}

/* ── One line ────────────────────────────────────────────────────────────── */

/* A JSON string with an embedded NUL would silently truncate at record
 * construction — the id would then commit to bytes the file does not hold.
 * Absent and null are DIFFERENT answers here, so the caller can distinguish
 * "field not present" (use the default) from "field: null" (there is none). */
static bool get_str_field(yyjson_val *obj, const char *key, const char **out, bool *was_null,
                          bool *type_ok) {
    *out = NULL;
    *was_null = false;
    *type_ok = true;
    yyjson_val *val = yyjson_obj_get(obj, key);
    if (val == NULL) {
        return false; /* absent */
    }
    if (yyjson_is_null(val)) {
        *was_null = true;
        return true;
    }
    if (!yyjson_is_str(val) || yyjson_get_len(val) != strlen(yyjson_get_str(val))) {
        *type_ok = false;
        return true;
    }
    *out = yyjson_get_str(val);
    return true;
}

static line_result_t handle_line(hyp_watched_scanner_t *s, watched_file_t *st, const char *line,
                                 size_t len, uint64_t line_start, hyp_watched_sink_fn sink,
                                 void *sink_ctx, uint64_t *rows_yielded) {
    /* A trailing CR is a Windows writer, not a different row. */
    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    if (len == 0) {
        return LINE_OK; /* a blank line separates nothing from nothing */
    }

    yyjson_doc *doc = yyjson_read(line, len, 0);
    if (doc == NULL) {
        file_fail(st, HYP_WATCHED_REASON_BAD_JSON, line_start,
                  "line at byte %" PRIu64 ": not valid JSON", line_start);
        return LINE_FAIL;
    }
    line_result_t result = LINE_FAIL;
    char *scrubbed = NULL;

    yyjson_val *obj = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(obj)) {
        file_fail(st, HYP_WATCHED_REASON_BAD_JSON, line_start,
                  "line at byte %" PRIu64 ": not a JSON object", line_start);
        goto done;
    }

    /* The derived fields, refused loudly rather than ignored silently: a
     * writer that supplies them believes they took effect. */
    static const char *const forged[] = {"origin", "redactions", "id"};
    for (size_t i = 0; i < sizeof(forged) / sizeof(forged[0]); i++) {
        if (yyjson_obj_get(obj, forged[i]) != NULL) {
            file_fail(st, HYP_WATCHED_REASON_FORGED_FIELD, line_start,
                      "line at byte %" PRIu64 ": '%s' is derived by the adapter, never supplied",
                      line_start, forged[i]);
            goto done;
        }
    }

    /* v: absent means 1; anything but the integer 1 is a format we do not
     * know, and guessing at it is how a v2 gets half-ingested. */
    yyjson_val *ver = yyjson_obj_get(obj, "v");
    if (ver != NULL && (!yyjson_is_int(ver) || yyjson_get_sint(ver) != 1)) {
        file_fail(st, HYP_WATCHED_REASON_BAD_VERSION, line_start,
                  "line at byte %" PRIu64 ": v must be the integer 1", line_start);
        goto done;
    }

    const char *str = NULL;
    bool was_null = false;
    bool type_ok = true;

    /* kind: absent means "message" — the dominant row of every transcript. */
    hyp_record_kind_t kind = HYP_RECORD_MESSAGE;
    if (get_str_field(obj, "kind", &str, &was_null, &type_ok)) {
        if (!type_ok || was_null || !hyp_record_kind_from_name(str, &kind)) {
            file_fail(st, HYP_WATCHED_REASON_BAD_FIELD, line_start,
                      "line at byte %" PRIu64
                      ": kind must be decision|verdict|summary|signal|message",
                      line_start);
            goto done;
        }
    }

    const char *author = NULL;
    if (!get_str_field(obj, "author", &author, &was_null, &type_ok) || !type_ok || was_null) {
        file_fail(st, HYP_WATCHED_REASON_BAD_FIELD, line_start,
                  "line at byte %" PRIu64 ": author must be a string, and is required", line_start);
        goto done;
    }

    /* timestamp_ms: the source's own time, as an integer. A string, a float or
     * a missing value is refused — a clock is not a substitute (I6). */
    yyjson_val *ts_val = yyjson_obj_get(obj, "timestamp_ms");
    if (ts_val == NULL || !yyjson_is_int(ts_val) ||
        (yyjson_is_uint(ts_val) && yyjson_get_uint(ts_val) > (uint64_t)INT64_MAX)) {
        file_fail(st, HYP_WATCHED_REASON_BAD_FIELD, line_start,
                  "line at byte %" PRIu64
                  ": timestamp_ms must be integer milliseconds since the Unix epoch",
                  line_start);
        goto done;
    }
    int64_t timestamp_ms = yyjson_get_sint(ts_val);

    yyjson_val *content_val = yyjson_obj_get(obj, "content");
    if (content_val == NULL || !yyjson_is_str(content_val) ||
        yyjson_get_len(content_val) != strlen(yyjson_get_str(content_val))) {
        file_fail(st, HYP_WATCHED_REASON_BAD_FIELD, line_start,
                  "line at byte %" PRIu64 ": content must be a string, and is required",
                  line_start);
        goto done;
    }
    const char *content = yyjson_get_str(content_val);
    size_t content_len = yyjson_get_len(content_val);

    const char *anchor = NULL;
    if (get_str_field(obj, "anchor", &anchor, &was_null, &type_ok) && !type_ok) {
        file_fail(st, HYP_WATCHED_REASON_BAD_FIELD, line_start,
                  "line at byte %" PRIu64 ": anchor must be a string or null", line_start);
        goto done;
    }

    /* thread: absent derives the file's own; null opts out of any. */
    char thread_buf[HYP_RECORD_MAX_THREAD + 1];
    const char *thread = NULL;
    if (get_str_field(obj, "thread", &thread, &was_null, &type_ok)) {
        if (!type_ok) {
            file_fail(st, HYP_WATCHED_REASON_BAD_FIELD, line_start,
                      "line at byte %" PRIu64 ": thread must be a string or null", line_start);
            goto done;
        }
    } else {
        if (!hyp_watched_thread(st->relpath, thread_buf, sizeof(thread_buf))) {
            file_fail(st, HYP_WATCHED_REASON_ROW_REFUSED, line_start,
                      "line at byte %" PRIu64 ": derived thread exceeds its cap", line_start);
            goto done;
        }
        thread = thread_buf;
    }

    const char *parent = NULL;
    if (get_str_field(obj, "parent", &parent, &was_null, &type_ok) && !type_ok) {
        file_fail(st, HYP_WATCHED_REASON_BAD_FIELD, line_start,
                  "line at byte %" PRIu64 ": parent must be a record id or null", line_start);
        goto done;
    }

    /* The scrub seam, in front of record construction by construction. */
    uint32_t redactions = 0;
    if (!s->scrub(s->scrub_ctx, content, content_len, &scrubbed, &redactions) || scrubbed == NULL) {
        scrubbed = NULL;
        file_fail(st, HYP_WATCHED_REASON_SCRUB_FAILED, line_start,
                  "line at byte %" PRIu64 ": the scrubber refused this row", line_start);
        goto done;
    }

    char origin_buf[HYP_RECORD_MAX_ORIGIN + 1];
    if (!hyp_watched_origin(st->relpath, line_start, origin_buf, sizeof(origin_buf))) {
        file_fail(st, HYP_WATCHED_REASON_ROW_REFUSED, line_start,
                  "line at byte %" PRIu64 ": derived origin exceeds its cap", line_start);
        goto done;
    }

    hyp_record_input_t row;
    memset(&row, 0, sizeof(row));
    row.kind = kind;
    row.author = author;
    row.timestamp_ms = timestamp_ms;
    row.content = scrubbed;
    row.anchor = anchor;
    row.origin = origin_buf;
    row.thread = thread;
    row.parent = parent;
    row.redactions = redactions;

    /* Pre-validate against the record contract, so the sink is never handed a
     * row that hyp_record_build() would refuse — and so the refusal is named
     * here, against the file and offset, where someone can act on it. */
    char id[HYP_RECORD_ID_LEN + 1];
    hyp_record_status_t status = hyp_record_derive_id(&row, id);
    if (status != HYP_RECORD_OK) {
        file_fail(st, HYP_WATCHED_REASON_ROW_REFUSED, line_start, "line at byte %" PRIu64 ": %s",
                  line_start, hyp_record_status_reason(status));
        goto done;
    }

    if (!sink(sink_ctx, &row)) {
        result = LINE_ABORT; /* not the file's fault: offset stays, retried */
        goto done;
    }
    st->rows++;
    (*rows_yielded)++;
    result = LINE_OK;

done:
    free(scrubbed);
    yyjson_doc_free(doc);
    return result;
}

/* ── One file ────────────────────────────────────────────────────────────── */

static line_result_t process_file(hyp_watched_scanner_t *s, watched_file_t *st, const char *abs,
                                  hyp_watched_sink_fn sink, void *sink_ctx,
                                  uint64_t *rows_yielded) {
    /* A shrunk file's offsets can never be trusted again: stay refused. */
    if (st->state == HYP_WATCHED_FILE_FAILED && st->reason == HYP_WATCHED_REASON_SHRUNK) {
        return LINE_OK;
    }

    hyp_path_info_t info;
    if (hyp_path_info_utf8(abs, &info) != 0 || !info.is_regular) {
        file_fail(st, HYP_WATCHED_REASON_IO, st->consumed, "cannot stat '%s'", st->relpath);
        return LINE_OK;
    }
    if ((uint64_t)info.size < st->consumed) {
        file_fail(st, HYP_WATCHED_REASON_SHRUNK, st->consumed,
                  "file shrank below the %" PRIu64 " bytes already ingested — feed files are "
                  "append-only",
                  st->consumed);
        return LINE_OK;
    }
    if ((uint64_t)info.size == st->consumed) {
        /* Nothing new. Whatever went before, there is nothing left to refuse. */
        st->state = HYP_WATCHED_FILE_OK;
        st->reason = HYP_WATCHED_REASON_NONE;
        snprintf(st->detail, sizeof(st->detail), "up to date");
        return LINE_OK;
    }

    FILE *f = hyp_fopen(abs, "rb");
    if (f == NULL) {
        file_fail(st, HYP_WATCHED_REASON_IO, st->consumed, "cannot open '%s'", st->relpath);
        return LINE_OK;
    }
    if (hyp_watched_seek64(f, st->consumed) != 0) {
        fclose(f);
        file_fail(st, HYP_WATCHED_REASON_IO, st->consumed, "cannot seek '%s'", st->relpath);
        return LINE_OK;
    }

    size_t cap = 4096;
    char *line = (char *)malloc(cap);
    if (line == NULL) {
        fclose(f);
        file_fail(st, HYP_WATCHED_REASON_ALLOC, st->consumed, "out of memory");
        return LINE_ABORT;
    }

    /* Re-examining a previously failed file is deliberate: a fixed line heals
     * it. Until a line refuses again, the slate is clean. */
    st->state = HYP_WATCHED_FILE_OK;
    st->reason = HYP_WATCHED_REASON_NONE;
    snprintf(st->detail, sizeof(st->detail), "up to date");

    line_result_t outcome = LINE_OK;
    uint64_t pos = st->consumed; /* start offset of the line being built */
    size_t len = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) {
            if (len > 0 && st->state == HYP_WATCHED_FILE_OK) {
                /* A harness mid-append. Not an error: not yet. */
                st->state = HYP_WATCHED_FILE_PENDING;
                snprintf(st->detail, sizeof(st->detail),
                         "partial line at byte %" PRIu64 " — waiting for the newline", pos);
            }
            break;
        }
        if (c == '\n') {
            line_result_t r = handle_line(s, st, line, len, pos, sink, sink_ctx, rows_yielded);
            if (r != LINE_OK) {
                outcome = r;
                break; /* the file fails closed HERE; the prefix stays ingested */
            }
            pos += (uint64_t)len + 1;
            st->consumed = pos;
            len = 0;
            continue;
        }
        if (len == HYP_WATCHED_MAX_LINE) {
            file_fail(st, HYP_WATCHED_REASON_LINE_TOO_LONG, pos,
                      "line at byte %" PRIu64 " exceeds %u bytes — refused, never truncated", pos,
                      (unsigned)HYP_WATCHED_MAX_LINE);
            break;
        }
        if (len == cap) {
            size_t next = cap * 2;
            char *grown = (char *)realloc(line, next);
            if (grown == NULL) {
                file_fail(st, HYP_WATCHED_REASON_ALLOC, pos, "out of memory");
                outcome = LINE_ABORT;
                break;
            }
            line = grown;
            cap = next;
        }
        line[len++] = (char)c;
    }

    free(line);
    fclose(f);
    return outcome;
}

/* ── The walk ────────────────────────────────────────────────────────────── */

static bool has_jsonl_suffix(const char *name) {
    size_t n = strlen(name);
    return n > 6 && strcmp(name + n - 6, ".jsonl") == 0;
}

/* Collect candidate files under root/rel into the state table. Returns false
 * only on allocation failure; an unreadable subdirectory is skipped — it will
 * be somebody's permissions experiment, and one bad directory must not block
 * the rest any more than one bad file does. */
static bool walk(hyp_watched_scanner_t *s, const char *rel, int depth) {
    if (depth > HYP_WATCHED_MAX_DEPTH) {
        return true;
    }
    char abs[HYP_WATCHED_MAX_PATH];
    int n = rel[0] ? snprintf(abs, sizeof(abs), "%s/%s", s->root, rel)
                   : snprintf(abs, sizeof(abs), "%s", s->root);
    if (n <= 0 || (size_t)n >= sizeof(abs)) {
        return true;
    }
    hyp_dir_t *dir = hyp_opendir(abs);
    if (dir == NULL) {
        return true;
    }
    hyp_dirent_t *ent = NULL;
    bool ok = true;
    while (ok && (ent = hyp_readdir(dir)) != NULL) {
        if (ent->name[0] == '.') {
            continue; /* dot entries, hidden files, editor droppings */
        }
        char child_rel[HYP_WATCHED_MAX_PATH];
        int m = rel[0] ? snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->name)
                       : snprintf(child_rel, sizeof(child_rel), "%s", ent->name);
        if (m <= 0 || (size_t)m >= sizeof(child_rel)) {
            continue;
        }
        char child_abs[HYP_WATCHED_MAX_PATH];
        int a = snprintf(child_abs, sizeof(child_abs), "%s/%s", s->root, child_rel);
        if (a <= 0 || (size_t)a >= sizeof(child_abs)) {
            continue;
        }
        hyp_path_info_t info;
        if (hyp_path_info_utf8(child_abs, &info) != 0 || info.is_symlink) {
            continue; /* a symlink is a door out of the root; do not open it */
        }
        if (info.is_directory) {
            ok = walk(s, child_rel, depth + 1);
        } else if (info.is_regular && has_jsonl_suffix(ent->name)) {
            watched_file_t *st = file_intern(s, child_rel);
            if (st == NULL) {
                ok = false;
            } else {
                st->seen = true;
            }
        }
    }
    hyp_closedir(dir);
    return ok;
}

/* ── Surface ─────────────────────────────────────────────────────────────── */

hyp_watched_scanner_t *hyp_watched_scanner_create(const char *root, hyp_watched_scrub_fn scrub,
                                                  void *scrub_ctx) {
    /* No scrubber, no scanner. Unscrubbed ingest must be impossible to
     * express, because the id commits to content and a record is permanent. */
    if (root == NULL || root[0] == '\0' || scrub == NULL) {
        return NULL;
    }
    hyp_watched_scanner_t *s =
        (hyp_watched_scanner_t *)calloc(HYP_ALLOC_ONE, sizeof(hyp_watched_scanner_t));
    if (s == NULL) {
        return NULL;
    }
    s->root = dup_cstr(root);
    if (s->root == NULL) {
        free(s);
        return NULL;
    }
    /* A trailing separator would double up in every join. */
    size_t rn = strlen(s->root);
    while (rn > 1 && (s->root[rn - 1] == '/' || s->root[rn - 1] == '\\')) {
        s->root[--rn] = '\0';
    }
    s->scrub = scrub;
    s->scrub_ctx = scrub_ctx;
    return s;
}

void hyp_watched_scanner_free(hyp_watched_scanner_t *scanner) {
    if (scanner == NULL) {
        return;
    }
    for (size_t i = 0; i < scanner->count; i++) {
        free(scanner->files[i].relpath);
    }
    free(scanner->files);
    free(scanner->root);
    free(scanner);
}

bool hyp_watched_scan_once(hyp_watched_scanner_t *scanner, hyp_watched_sink_fn sink, void *sink_ctx,
                           hyp_watched_report_t *out_report) {
    if (out_report != NULL) {
        memset(out_report, 0, sizeof(*out_report));
    }
    if (scanner == NULL || sink == NULL) {
        return false;
    }
    hyp_path_info_t info;
    if (hyp_path_info_utf8(scanner->root, &info) != 0 || !info.is_directory) {
        return false; /* no root, no pass — a confident error, not a quiet zero */
    }
    for (size_t i = 0; i < scanner->count; i++) {
        scanner->files[i].seen = false;
    }
    if (!walk(scanner, "", 0)) {
        return false;
    }

    hyp_watched_report_t report;
    memset(&report, 0, sizeof(report));
    bool completed = true;
    for (size_t i = 0; i < scanner->count && completed; i++) {
        watched_file_t *st = &scanner->files[i];
        if (!st->seen) {
            continue; /* gone this pass; already-ingested rows are kept */
        }
        report.files_seen++;
        char abs[HYP_WATCHED_MAX_PATH];
        int n = snprintf(abs, sizeof(abs), "%s/%s", scanner->root, st->relpath);
        if (n <= 0 || (size_t)n >= sizeof(abs)) {
            file_fail(st, HYP_WATCHED_REASON_IO, st->consumed, "path too long");
        } else if (process_file(scanner, st, abs, sink, sink_ctx, &report.rows_yielded) ==
                   LINE_ABORT) {
            completed = false; /* the sink said stop; nothing is lost, offsets held */
        }
        if (st->state == HYP_WATCHED_FILE_FAILED) {
            report.files_failed++;
        } else if (st->state == HYP_WATCHED_FILE_PENDING) {
            report.files_pending++;
        }
    }
    if (out_report != NULL) {
        *out_report = report;
    }
    return completed;
}

size_t hyp_watched_file_count(const hyp_watched_scanner_t *scanner) {
    return scanner == NULL ? 0 : scanner->count;
}

bool hyp_watched_file_at(const hyp_watched_scanner_t *scanner, size_t index,
                         hyp_watched_file_t *out) {
    if (scanner == NULL || out == NULL || index >= scanner->count) {
        return false;
    }
    const watched_file_t *st = &scanner->files[index];
    out->relpath = st->relpath;
    out->state = st->state;
    out->reason = st->reason;
    out->detail = st->detail;
    out->consumed = st->consumed;
    out->failed_at = st->failed_at;
    out->rows = st->rows;
    return true;
}
