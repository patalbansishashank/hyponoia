/*
 * comment_migrate.c — the ingest half of comment relocation. Every "why" is in
 * comment_migrate.h; this file is the parser, the two derived strings, and the
 * append loop.
 *
 * No clock is read here, deliberately: the timestamp is a fact about the
 * commit that wrote the prose, and the record id commits to it (I6).
 */
#include "memory/comment_migrate.h"

#include "foundation/constants.h"
#include "foundation/identity.h"
#include "foundation/record.h"
#include "pipeline/pipeline.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

/* The manifest's first line. A file that does not open with it is not this
 * format, and reading it as though it were would invent records out of
 * whatever bytes happened to parse. */
#define CM_MAGIC "hyp-comment-manifest-v1"

/* The origin's adapter prefix. Origins are opaque to the core (I7); this
 * prefix is how THIS adapter recognises its own rows in a store that will hold
 * several feeds. */
#define CM_ORIGIN_PREFIX "comment:"

enum {
    CM_MAX_LINE_NO = 100000000, /* a source file with more lines is a mistake */
    CM_ERR_SZ = 512
};

const char *hyp_comment_migrate_status_reason(hyp_comment_migrate_status_t status) {
    switch (status) {
    case HYP_COMMENT_MIGRATE_OK:
        return "ok";
    case HYP_COMMENT_MIGRATE_ERR_NULL:
        return "a required argument was NULL";
    case HYP_COMMENT_MIGRATE_ERR_IO:
        return "the manifest could not be read";
    case HYP_COMMENT_MIGRATE_ERR_FORMAT:
        return "not a comment manifest, or a field out of order";
    case HYP_COMMENT_MIGRATE_ERR_FIELD:
        return "a field is present but unusable";
    case HYP_COMMENT_MIGRATE_ERR_ADDRESS:
        return "no address derives from the path";
    case HYP_COMMENT_MIGRATE_ERR_RECORD:
        return "the record contract refused the row";
    case HYP_COMMENT_MIGRATE_ERR_STORE:
        return "the store refused the row";
    case HYP_COMMENT_MIGRATE_ERR_ALLOC:
        return "out of memory";
    default:
        return "unknown status";
    }
}

/* ── The derived strings ────────────────────────────────────────────────── */

static bool cm_is_hex40(const char *s) {
    if (!s) {
        return false;
    }
    for (int i = 0; i < 40; i++) {
        bool hex = (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f');
        if (!hex) {
            return false;
        }
    }
    return s[40] == '\0';
}

bool hyp_comment_migrate_origin(const char *blob, const char *path, int start_line, int end_line,
                                char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return false;
    }
    out[0] = '\0';
    if (!cm_is_hex40(blob) || !path || path[0] == '\0') {
        return false;
    }
    if (start_line <= 0 || end_line < start_line || end_line > CM_MAX_LINE_NO) {
        return false;
    }
    int n = snprintf(out, out_sz, CM_ORIGIN_PREFIX "%s:%s:%d-%d", blob, path, start_line, end_line);
    if (n < 0 || (size_t)n >= out_sz || (size_t)n > HYP_RECORD_MAX_ORIGIN) {
        out[0] = '\0';
        return false;
    }
    return true;
}

hyp_comment_migrate_status_t hyp_comment_migrate_anchor(const hyp_comment_migrate_opts_t *opts,
                                                        const char *path, char *out,
                                                        size_t out_sz) {
    if (!out || out_sz == 0) {
        return HYP_COMMENT_MIGRATE_ERR_NULL;
    }
    out[0] = '\0';
    if (!opts || !opts->repo || opts->repo[0] == '\0' || !path || path[0] == '\0') {
        return HYP_COMMENT_MIGRATE_ERR_NULL;
    }
    /* The module QN comes from the one qualified-name builder, never from a
     * string assembled here: an address the rest of the system cannot look up
     * resolves to nothing while looking perfectly well-formed. */
    char *qn = hyp_pipeline_fqn_module(opts->repo, path);
    if (!qn) {
        return HYP_COMMENT_MIGRATE_ERR_ALLOC;
    }
    hyp_addr_t addr;
    hyp_addr_err_t err = hyp_addr_init(&addr, opts->workspace, opts->repo, qn);
    free(qn);
    if (err != HYP_ADDR_OK) {
        return HYP_COMMENT_MIGRATE_ERR_ADDRESS;
    }
    if (!hyp_addr_format(&addr, out, out_sz)) {
        out[0] = '\0';
        return HYP_COMMENT_MIGRATE_ERR_ADDRESS;
    }
    return HYP_COMMENT_MIGRATE_OK;
}

/* ── The manifest ───────────────────────────────────────────────────────── */

typedef struct {
    char *buf;
    size_t len;
    size_t pos;
} cm_reader_t;

static void cm_say(char *err, size_t err_sz, const char *fmt, ...) {
    if (!err || err_sz == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_sz, fmt, ap);
    va_end(ap);
}

/* Read the whole manifest. Whole-file rather than streaming because the parse
 * hands out pointers INTO the buffer and NUL-terminates in place, which is
 * what keeps a value containing a newline from needing an escaping rule. */
static hyp_comment_migrate_status_t cm_slurp(const char *path, cm_reader_t *r) {
    r->buf = NULL;
    r->len = 0;
    r->pos = 0;
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return HYP_COMMENT_MIGRATE_ERR_IO;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        (void)fclose(fh);
        return HYP_COMMENT_MIGRATE_ERR_IO;
    }
    long size = ftell(fh);
    if (size < 0 || fseek(fh, 0, SEEK_SET) != 0) {
        (void)fclose(fh);
        return HYP_COMMENT_MIGRATE_ERR_IO;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        (void)fclose(fh);
        return HYP_COMMENT_MIGRATE_ERR_ALLOC;
    }
    size_t got = fread(buf, 1, (size_t)size, fh);
    (void)fclose(fh);
    buf[got] = '\0';
    r->buf = buf;
    r->len = got;
    return HYP_COMMENT_MIGRATE_OK;
}

/* One field: a header line "<name> <byte-length>", then exactly that many
 * bytes, then one newline. Returns the value NUL-terminated in place. */
static hyp_comment_migrate_status_t cm_field(cm_reader_t *r, const char *name, char **out_value,
                                             size_t *out_len) {
    size_t name_len = strlen(name);
    if (r->pos + name_len + 1 > r->len) {
        return HYP_COMMENT_MIGRATE_ERR_FORMAT;
    }
    if (memcmp(r->buf + r->pos, name, name_len) != 0 || r->buf[r->pos + name_len] != ' ') {
        return HYP_COMMENT_MIGRATE_ERR_FORMAT;
    }
    char *digits = r->buf + r->pos + name_len + 1;
    char *end = NULL;
    errno = 0;
    long declared = strtol(digits, &end, 10);
    if (errno != 0 || end == digits || !end || *end != '\n' || declared < 0) {
        return HYP_COMMENT_MIGRATE_ERR_FORMAT;
    }
    size_t start = (size_t)(end - r->buf) + 1;
    size_t value_len = (size_t)declared;
    if (start + value_len + 1 > r->len || r->buf[start + value_len] != '\n') {
        return HYP_COMMENT_MIGRATE_ERR_FORMAT;
    }
    /* An embedded NUL would make the C string shorter than the field the
     * writer counted — the two ends would then disagree about the content the
     * id commits to. Refuse rather than silently store the prefix. */
    if (memchr(r->buf + start, '\0', value_len) != NULL) {
        return HYP_COMMENT_MIGRATE_ERR_FIELD;
    }
    r->buf[start + value_len] = '\0';
    *out_value = r->buf + start;
    *out_len = value_len;
    r->pos = start + value_len + 1;
    return HYP_COMMENT_MIGRATE_OK;
}

/* "<start>-<end>", both positive, end >= start. */
static bool cm_lines(const char *s, int *out_start, int *out_end) {
    char *end = NULL;
    errno = 0;
    long a = strtol(s, &end, 10);
    if (errno != 0 || end == s || !end || *end != '-') {
        return false;
    }
    const char *second = end + 1;
    errno = 0;
    long b = strtol(second, &end, 10);
    if (errno != 0 || end == second || !end || *end != '\0') {
        return false;
    }
    if (a <= 0 || b < a || b > CM_MAX_LINE_NO) {
        return false;
    }
    *out_start = (int)a;
    *out_end = (int)b;
    return true;
}

static bool cm_timestamp(const char *s, int64_t *out) {
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || !end || *end != '\0') {
        return false;
    }
    if (v < HYP_RECORD_MIN_TIMESTAMP_MS || v > HYP_RECORD_MAX_TIMESTAMP_MS) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

hyp_comment_migrate_status_t hyp_comment_migrate_run(const char *manifest_path,
                                                     const hyp_comment_migrate_opts_t *opts,
                                                     hyp_record_store_t *store,
                                                     hyp_comment_migrate_stats_t *stats, char *err,
                                                     size_t err_sz) {
    if (err && err_sz > 0) {
        err[0] = '\0';
    }
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
    if (!manifest_path || !opts || !opts->repo || !store) {
        return HYP_COMMENT_MIGRATE_ERR_NULL;
    }

    cm_reader_t r;
    hyp_comment_migrate_status_t st = cm_slurp(manifest_path, &r);
    if (st != HYP_COMMENT_MIGRATE_OK) {
        cm_say(err, err_sz, "cannot read manifest %s", manifest_path);
        return st;
    }

    size_t magic_len = strlen(CM_MAGIC);
    if (r.len < magic_len + 1 || memcmp(r.buf, CM_MAGIC, magic_len) != 0 ||
        r.buf[magic_len] != '\n') {
        free(r.buf);
        cm_say(err, err_sz, "%s does not open with " CM_MAGIC, manifest_path);
        return HYP_COMMENT_MIGRATE_ERR_FORMAT;
    }
    r.pos = magic_len + 1;

    size_t item_no = 0;
    while (r.pos < r.len) {
        char *path = NULL;
        char *blob = NULL;
        char *lines = NULL;
        char *author = NULL;
        char *ts_text = NULL;
        char *content = NULL;
        size_t ignored = 0;
        item_no++;

        /* Fixed order. A format whose fields may arrive in any order is a
         * format where a field one end forgot to write is a field the other
         * end silently defaults. */
        st = cm_field(&r, "path", &path, &ignored);
        if (st == HYP_COMMENT_MIGRATE_OK) {
            st = cm_field(&r, "blob", &blob, &ignored);
        }
        if (st == HYP_COMMENT_MIGRATE_OK) {
            st = cm_field(&r, "lines", &lines, &ignored);
        }
        if (st == HYP_COMMENT_MIGRATE_OK) {
            st = cm_field(&r, "author", &author, &ignored);
        }
        if (st == HYP_COMMENT_MIGRATE_OK) {
            st = cm_field(&r, "timestamp_ms", &ts_text, &ignored);
        }
        if (st == HYP_COMMENT_MIGRATE_OK) {
            st = cm_field(&r, "content", &content, &ignored);
        }
        if (st != HYP_COMMENT_MIGRATE_OK) {
            cm_say(err, err_sz, "item %zu: malformed", item_no);
            free(r.buf);
            return st;
        }

        int start_line = 0;
        int end_line = 0;
        int64_t timestamp_ms = 0;
        if (!cm_lines(lines, &start_line, &end_line) || !cm_timestamp(ts_text, &timestamp_ms)) {
            cm_say(err, err_sz, "item %zu (%s): unusable lines or timestamp", item_no, path);
            free(r.buf);
            return HYP_COMMENT_MIGRATE_ERR_FIELD;
        }

        char origin[HYP_RECORD_MAX_ORIGIN + 1];
        if (!hyp_comment_migrate_origin(blob, path, start_line, end_line, origin, sizeof(origin))) {
            cm_say(err, err_sz, "item %zu (%s): no origin derives from blob/path/lines", item_no,
                   path);
            free(r.buf);
            return HYP_COMMENT_MIGRATE_ERR_FIELD;
        }

        char anchor[HYP_ADDR_MAX + 1];
        st = hyp_comment_migrate_anchor(opts, path, anchor, sizeof(anchor));
        if (st != HYP_COMMENT_MIGRATE_OK) {
            cm_say(err, err_sz, "item %zu (%s): %s", item_no, path,
                   hyp_comment_migrate_status_reason(st));
            free(r.buf);
            return st;
        }

        hyp_record_input_t in;
        memset(&in, 0, sizeof(in));
        in.kind = HYP_RECORD_DECISION;
        in.author = author;
        in.timestamp_ms = timestamp_ms;
        in.content = content;
        in.anchor = anchor;
        in.origin = origin;

        const hyp_record_t *rec = NULL;
        hyp_record_status_t rs = hyp_record_build(&in, &rec);
        if (rs != HYP_RECORD_OK) {
            cm_say(err, err_sz, "item %zu (%s): %s", item_no, path,
                   hyp_record_status_reason(rs));
            free(r.buf);
            return HYP_COMMENT_MIGRATE_ERR_RECORD;
        }

        bool added = false;
        hyp_record_store_status_t ss = hyp_record_store_append(store, rec, &added);
        hyp_record_free(rec);
        if (ss != HYP_RECORD_STORE_OK) {
            cm_say(err, err_sz, "item %zu (%s): %s", item_no, path,
                   hyp_record_store_status_reason(ss));
            free(r.buf);
            return HYP_COMMENT_MIGRATE_ERR_STORE;
        }
        if (stats) {
            stats->items++;
            if (added) {
                stats->appended++;
            } else {
                stats->absorbed++;
            }
        }
    }

    free(r.buf);
    return HYP_COMMENT_MIGRATE_OK;
}

/* ── The command ────────────────────────────────────────────────────────── */

static void cm_usage(void) {
    (void)fprintf(stderr, "usage: hyponoia migrate-comments --manifest FILE --store DIR\n"
                          "                                [--repo SLUG] [--workspace NAME]\n");
}

int hyp_cmd_migrate_comments(int argc, char **argv) {
    const char *manifest = NULL;
    const char *store_dir = NULL;
    const char *repo = NULL;
    const char *workspace = NULL;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(arg, "--manifest") == 0 && value) {
            manifest = value;
            i++;
        } else if (strcmp(arg, "--store") == 0 && value) {
            store_dir = value;
            i++;
        } else if (strcmp(arg, "--repo") == 0 && value) {
            repo = value;
            i++;
        } else if (strcmp(arg, "--workspace") == 0 && value) {
            workspace = value;
            i++;
        } else {
            cm_usage();
            return 2;
        }
    }
    if (!manifest || !store_dir) {
        cm_usage();
        return 2;
    }

    char *derived = NULL;
    if (!repo) {
        char cwd[HYP_PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            (void)fprintf(stderr, "migrate-comments: cannot read the working directory\n");
            return 1;
        }
        derived = hyp_project_name_from_path(cwd);
        if (!derived) {
            (void)fprintf(stderr, "migrate-comments: no project slug derives from %s; pass "
                                  "--repo\n",
                          cwd);
            return 1;
        }
        repo = derived;
    }

    hyp_record_store_t *store = NULL;
    hyp_record_store_status_t os = hyp_record_store_open(store_dir, &store);
    if (os != HYP_RECORD_STORE_OK) {
        (void)fprintf(stderr, "migrate-comments: %s: %s\n", store_dir,
                      hyp_record_store_status_reason(os));
        free(derived);
        return 1;
    }

    hyp_comment_migrate_opts_t opts = {.workspace = workspace, .repo = repo};
    hyp_comment_migrate_stats_t stats;
    char err[CM_ERR_SZ];
    hyp_comment_migrate_status_t st =
        hyp_comment_migrate_run(manifest, &opts, store, &stats, err, sizeof(err));
    hyp_record_store_close(store);
    free(derived);

    if (st != HYP_COMMENT_MIGRATE_OK) {
        (void)fprintf(stderr, "migrate-comments: %s%s%s\n",
                      hyp_comment_migrate_status_reason(st), err[0] ? " — " : "", err);
        return 1;
    }
    (void)printf("migrate-comments: %zu block%s — %zu appended, %zu already present\n",
                 stats.items, stats.items == 1 ? "" : "s", stats.appended, stats.absorbed);
    return 0;
}
