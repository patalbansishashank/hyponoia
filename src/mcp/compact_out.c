/* compact_out.c — tree-format emission helpers. See compact_out.h for the contract. */
#include "mcp/compact_out.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SB_INITIAL_CAP = 1024 };

void hyp_sb_init(hyp_sb_t *sb) {
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->oom = false;
}

static bool sb_reserve(hyp_sb_t *sb, size_t extra) {
    if (sb->oom) {
        return false;
    }
    if (sb->len + extra + 1 <= sb->cap) {
        return true;
    }
    size_t ncap = sb->cap ? sb->cap : SB_INITIAL_CAP;
    while (ncap < sb->len + extra + 1) {
        ncap *= 2;
    }
    char *nbuf = (char *)realloc(sb->buf, ncap);
    if (!nbuf) {
        sb->oom = true;
        return false;
    }
    sb->buf = nbuf;
    sb->cap = ncap;
    return true;
}

void hyp_sb_append_n(hyp_sb_t *sb, const char *s, size_t n) {
    if (!s || n == 0 || !sb_reserve(sb, n)) {
        return;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void hyp_sb_append(hyp_sb_t *sb, const char *s) {
    if (s) {
        hyp_sb_append_n(sb, s, strlen(s));
    }
}

char *hyp_sb_finish(hyp_sb_t *sb) {
    if (sb->oom) {
        free(sb->buf);
        hyp_sb_init(sb);
        return NULL;
    }
    if (!sb->buf) {
        /* Empty but valid: return an owned empty string. */
        char *empty = (char *)calloc(1, 1);
        return empty;
    }
    char *out = sb->buf;
    hyp_sb_init(sb);
    return out;
}

void hyp_sb_free(hyp_sb_t *sb) {
    free(sb->buf);
    hyp_sb_init(sb);
}

/* ── Tree quoting ────────────────────────────────────────────────── */

/* True when `s` parses as an integer or real literal (sign, digits, one dot). */
static bool looks_numeric(const char *s) {
    if (!s || !*s) {
        return false;
    }
    const char *p = s;
    if (*p == '-' || *p == '+') {
        p++;
    }
    bool digit = false;
    bool dot = false;
    for (; *p; p++) {
        if (isdigit((unsigned char)*p)) {
            digit = true;
        } else if (*p == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    return digit;
}

/* UTF-8 sequence length starting at p, validating continuation bytes and the
 * lead-byte ranges (RFC 3629: no overlongs, no surrogates, max U+10FFFF);
 * 0 = not a valid sequence start. Reads stop at the first invalid byte, so a
 * terminating NUL is never overrun. */
static size_t utf8_sequence_length(const unsigned char *p) {
    unsigned char c = p[0];
    if (c < 0x80) {
        return 1;
    }
    if (c < 0xC2) {
        return 0; /* bare continuation byte or overlong lead */
    }
    if (c < 0xE0) {
        return (p[1] & 0xC0) == 0x80 ? 2 : 0;
    }
    if (c < 0xF0) {
        unsigned char lo = c == 0xE0 ? 0xA0 : 0x80;
        unsigned char hi = c == 0xED ? 0x9F : 0xBF;
        if (p[1] < lo || p[1] > hi || (p[2] & 0xC0) != 0x80) {
            return 0;
        }
        return 3;
    }
    if (c < 0xF5) {
        unsigned char lo = c == 0xF0 ? 0x90 : 0x80;
        unsigned char hi = c == 0xF4 ? 0x8F : 0xBF;
        if (p[1] < lo || p[1] > hi || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) {
            return 0;
        }
        return 4;
    }
    return 0;
}

static bool needs_quotes(const char *s) {
    if (!s || !*s) {
        return false; /* empty cells emit as the "-" placeholder, not quotes */
    }
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        /* Space-delimited rows: any internal whitespace or quote forces
         * quoting so column positions stay parseable. Control bytes and
         * invalid UTF-8 force the quoted path too, which sanitizes them —
         * one raw byte otherwise makes line-oriented consumers (BSD grep)
         * treat the ENTIRE tool output as unmatchable binary. */
        if (isspace(c) || *p == '"' || *p == '\r' || c < 0x20 || c == 0x7f) {
            return true;
        }
        if (c >= 0x80) {
            size_t len = utf8_sequence_length((const unsigned char *)p);
            if (!len) {
                return true;
            }
            p += len - 1;
        }
    }
    if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0 || strcmp(s, "null") == 0 ||
        strcmp(s, "-") == 0) {
        return true;
    }
    return looks_numeric(s);
}

static void append_quoted(hyp_sb_t *sb, const char *s) {
    hyp_sb_append_n(sb, "\"", 1);
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '"':
            hyp_sb_append_n(sb, "\\\"", 2);
            break;
        case '\\':
            hyp_sb_append_n(sb, "\\\\", 2);
            break;
        case '\n':
            hyp_sb_append_n(sb, "\\n", 2);
            break;
        case '\r':
            hyp_sb_append_n(sb, "\\r", 2);
            break;
        default: {
            unsigned char c = (unsigned char)*p;
            if (c >= 0x20 && c != 0x7f && c < 0x80) {
                hyp_sb_append_n(sb, p, 1);
            } else if (c < 0x20 || c == 0x7f) {
                /* JSON-style escape: the value stays one printable line. */
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                hyp_sb_append(sb, esc);
            } else {
                size_t len = utf8_sequence_length((const unsigned char *)p);
                if (len) {
                    hyp_sb_append_n(sb, p, len);
                    p += len - 1;
                } else {
                    /* Invalid byte → U+FFFD: output is valid UTF-8 by
                     * construction, and the corruption stays visible. */
                    hyp_sb_append_n(sb, "\xEF\xBF\xBD", 3);
                }
            }
            break;
        }
        }
    }
    hyp_sb_append_n(sb, "\"", 1);
}

static void append_value(hyp_sb_t *sb, const char *s) {
    if (!s || !*s) {
        hyp_sb_append_n(sb, "-", 1); /* stable column positions for empties */
        return;
    }
    if (needs_quotes(s)) {
        append_quoted(sb, s);
    } else {
        hyp_sb_append(sb, s);
    }
}

/* ── Scalars ────────────────────────────────────────────────────── */

void hyp_tree_scalar_str(hyp_sb_t *sb, const char *key, const char *val) {
    hyp_sb_append(sb, key);
    hyp_sb_append_n(sb, ": ", 2);
    append_value(sb, val);
    hyp_sb_append_n(sb, "\n", 1);
}

void hyp_tree_scalar_int(hyp_sb_t *sb, const char *key, long long v) {
    char num[32];
    snprintf(num, sizeof(num), "%lld", v);
    hyp_sb_append(sb, key);
    hyp_sb_append_n(sb, ": ", 2);
    hyp_sb_append(sb, num);
    hyp_sb_append_n(sb, "\n", 1);
}

void hyp_tree_scalar_bool(hyp_sb_t *sb, const char *key, bool v) {
    hyp_sb_append(sb, key);
    hyp_sb_append_n(sb, ": ", 2);
    hyp_sb_append(sb, v ? "true" : "false");
    hyp_sb_append_n(sb, "\n", 1);
}

/* ── Tables ─────────────────────────────────────────────────────── */

/* Tree-syntax table header: `key: N  (cols: a b c)` — count first (agents
 * read scale before rows), column names once, rows indented beneath. */
void hyp_tree_table_header(hyp_sb_t *sb, const char *key, int n, const char *const *cols,
                           int ncols) {
    char num[32];
    snprintf(num, sizeof(num), ": %d  (cols:", n);
    hyp_sb_append(sb, key);
    hyp_sb_append(sb, num);
    for (int i = 0; i < ncols; i++) {
        hyp_sb_append_n(sb, " ", 1);
        hyp_sb_append(sb, cols[i]);
    }
    hyp_sb_append_n(sb, ")\n", 2);
}

void hyp_tree_row_begin(hyp_sb_t *sb) {
    hyp_sb_append_n(sb, "  ", 2);
}

void hyp_tree_cell_str(hyp_sb_t *sb, const char *val, bool first) {
    if (!first) {
        hyp_sb_append_n(sb, " ", 1);
    }
    append_value(sb, val ? val : "");
}

void hyp_tree_cell_int(hyp_sb_t *sb, long long v, bool first) {
    char num[32];
    snprintf(num, sizeof(num), "%lld", v);
    if (!first) {
        hyp_sb_append_n(sb, " ", 1);
    }
    hyp_sb_append(sb, num);
}

void hyp_tree_cell_real(hyp_sb_t *sb, double v, bool first) {
    char num[48];
    snprintf(num, sizeof(num), "%.4g", v);
    if (!first) {
        hyp_sb_append_n(sb, " ", 1);
    }
    hyp_sb_append(sb, num);
}

void hyp_tree_cell_bool(hyp_sb_t *sb, bool v, bool first) {
    if (!first) {
        hyp_sb_append_n(sb, " ", 1);
    }
    hyp_sb_append(sb, v ? "true" : "false");
}

void hyp_tree_row_end(hyp_sb_t *sb) {
    hyp_sb_append_n(sb, "\n", 1);
}
