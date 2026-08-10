/*
 * ask_doctext.c — the comment-syntax table, the leading-comment scan and the
 * header line. See ask_doctext.h for what is composed and why.
 *
 * Everything here is PURE: no I/O, no allocation. The pass owns the file
 * bytes; this owns the rules. That split is what lets the rules be tested
 * without a filesystem, which matters because "what exactly gets embedded"
 * is the one thing that invalidates every recorded number when it moves.
 */

#include "ask/ask_doctext.h"

#include <string.h>

bool hyp_ask_compose_parse(const char *name, hyp_ask_compose_t *out) {
    if (!name || !out) {
        return false;
    }
    if (strcmp(name, "source") == 0) {
        *out = HYP_ASK_COMPOSE_SOURCE;
    } else if (strcmp(name, "header") == 0) {
        *out = HYP_ASK_COMPOSE_HEADER;
    } else if (strcmp(name, "comment") == 0) {
        *out = HYP_ASK_COMPOSE_COMMENT;
    } else if (strcmp(name, "full") == 0) {
        *out = HYP_ASK_COMPOSE_FULL;
    } else {
        return false;
    }
    return true;
}

const char *hyp_ask_compose_name(hyp_ask_compose_t c) {
    switch (c) {
    case HYP_ASK_COMPOSE_SOURCE:
        return "source";
    case HYP_ASK_COMPOSE_HEADER:
        return "header";
    case HYP_ASK_COMPOSE_COMMENT:
        return "comment";
    case HYP_ASK_COMPOSE_FULL:
        return "full";
    }
    return "full";
}

/* ── The table ─────────────────────────────────────────────────── */

/* Grouped by family rather than listed per language, because that is how the
 * facts actually cluster and a per-language list of 163 entries would be 163
 * chances to mistype one.
 *
 * A LANGUAGE IS ABSENT WHEN ITS ANSWER IS NOT OBVIOUS, and absent means "no
 * leading comment", which is exactly the behaviour before §2.2. Markup and
 * template dialects (HTML, Vue, Svelte, Astro, Blade, Liquid, Jinja2,
 * Markdown, RST) are absent because their comment form depends on which of
 * two embedded languages a line is in. Vimscript is absent because its
 * comment opener is the double quote, which is also its string delimiter, so
 * a lexical scan cannot tell a comment from a line of code. COBOL is absent
 * because its comments are column-positional. Generic assembly is absent
 * because the opener depends on the assembler (`;` for NASM, `#` or `//` for
 * GAS) and the file extension does not say which. */
hyp_ask_comment_syntax_t hyp_ask_comment_syntax(HYPLanguage lang) {
    hyp_ask_comment_syntax_t none = {NULL, NULL, NULL};
    hyp_ask_comment_syntax_t c_family = {"//", "/*", "*/"};
    hyp_ask_comment_syntax_t slash_only = {"//", NULL, NULL};
    hyp_ask_comment_syntax_t hash = {"#", NULL, NULL};
    hyp_ask_comment_syntax_t dash = {"--", NULL, NULL};
    hyp_ask_comment_syntax_t semi = {";", NULL, NULL};
    hyp_ask_comment_syntax_t percent = {"%", NULL, NULL};
    hyp_ask_comment_syntax_t bang = {"!", NULL, NULL};
    hyp_ask_comment_syntax_t block_c = {NULL, "/*", "*/"};
    hyp_ask_comment_syntax_t ml_paren = {NULL, "(*", "*)"};
    hyp_ask_comment_syntax_t slash_paren = {"//", "(*", "*)"};

    switch (lang) {
    /* Line comments with `//`, block comments with slash-star … star-slash. */
    case HYP_LANG_C:
    case HYP_LANG_CPP:
    case HYP_LANG_CUDA:
    case HYP_LANG_OBJC:
    case HYP_LANG_CSHARP:
    case HYP_LANG_JAVA:
    case HYP_LANG_JAVASCRIPT:
    case HYP_LANG_TYPESCRIPT:
    case HYP_LANG_TSX:
    case HYP_LANG_RUST:
    case HYP_LANG_GO:
    case HYP_LANG_SWIFT:
    case HYP_LANG_KOTLIN:
    case HYP_LANG_SCALA:
    case HYP_LANG_DART:
    case HYP_LANG_PHP:
    case HYP_LANG_GROOVY:
    case HYP_LANG_SOLIDITY:
    case HYP_LANG_GLSL:
    case HYP_LANG_HLSL:
    case HYP_LANG_WGSL:
    case HYP_LANG_VERILOG:
    case HYP_LANG_SYSTEMVERILOG:
    case HYP_LANG_DLANG:
    case HYP_LANG_JSON5:
    case HYP_LANG_JSONNET:
    case HYP_LANG_PROTOBUF:
    case HYP_LANG_THRIFT:
    case HYP_LANG_APEX:
    case HYP_LANG_QML:
    case HYP_LANG_ISPC:
    case HYP_LANG_SLANG:
    case HYP_LANG_MOVE:
    case HYP_LANG_SQUIRREL:
    case HYP_LANG_SCSS:
    case HYP_LANG_TYPST:
    case HYP_LANG_RESCRIPT:
    case HYP_LANG_ODIN:
    case HYP_LANG_PONY:
    case HYP_LANG_SWAY:
    case HYP_LANG_TABLEGEN:
    case HYP_LANG_DEVICETREE:
    case HYP_LANG_LINKERSCRIPT:
    case HYP_LANG_MAGMA:
    case HYP_LANG_KDL:
    case HYP_LANG_RON:
    case HYP_LANG_BICEP:
    case HYP_LANG_PKL:
    case HYP_LANG_WIT:
    case HYP_LANG_FUNC:
        return c_family;

    /* `//` and nothing else. */
    case HYP_LANG_ZIG:
    case HYP_LANG_GLEAM:
    case HYP_LANG_CAIRO:
    case HYP_LANG_PRISMA:
    case HYP_LANG_SMITHY:
    case HYP_LANG_GOMOD:
    case HYP_LANG_PINE:
    case HYP_LANG_SOQL:
    case HYP_LANG_SOSL:
        return slash_only;

    /* `#`. */
    case HYP_LANG_PYTHON:
    case HYP_LANG_RUBY:
    case HYP_LANG_BASH:
    case HYP_LANG_ZSH:
    case HYP_LANG_FISH:
    case HYP_LANG_PERL:
    case HYP_LANG_R:
    case HYP_LANG_YAML:
    case HYP_LANG_TOML:
    case HYP_LANG_MAKEFILE:
    case HYP_LANG_CMAKE:
    case HYP_LANG_DOCKERFILE:
    case HYP_LANG_NIX:
    case HYP_LANG_ELIXIR:
    case HYP_LANG_JULIA:
    case HYP_LANG_POWERSHELL:
    case HYP_LANG_AWK:
    case HYP_LANG_TCL:
    case HYP_LANG_GDSCRIPT:
    case HYP_LANG_CRYSTAL:
    case HYP_LANG_NIM:
    case HYP_LANG_STARLARK:
    case HYP_LANG_MESON:
    case HYP_LANG_HCL:
    case HYP_LANG_PROPERTIES:
    case HYP_LANG_REQUIREMENTS:
    case HYP_LANG_GITIGNORE:
    case HYP_LANG_GITATTRIBUTES:
    case HYP_LANG_DOTENV:
    case HYP_LANG_SSHCONFIG:
    case HYP_LANG_KCONFIG:
    case HYP_LANG_BITBAKE:
    case HYP_LANG_JUST:
    case HYP_LANG_PUPPET:
    case HYP_LANG_GN:
    case HYP_LANG_CAPNP:
    case HYP_LANG_GRAPHQL:
    case HYP_LANG_KUSTOMIZE:
    case HYP_LANG_K8S:
    case HYP_LANG_MOJO:
    case HYP_LANG_NICKEL:
    case HYP_LANG_SMALI:
    case HYP_LANG_PO:
        return hash;

    /* `--`. */
    case HYP_LANG_LUA:
    case HYP_LANG_LUAU:
    case HYP_LANG_TEAL:
    case HYP_LANG_SQL:
    case HYP_LANG_HASKELL:
    case HYP_LANG_PURESCRIPT:
    case HYP_LANG_ELM:
    case HYP_LANG_ADA:
    case HYP_LANG_VHDL:
    case HYP_LANG_AGDA:
    case HYP_LANG_LEAN:
        return dash;

    /* `;`. */
    case HYP_LANG_CLOJURE:
    case HYP_LANG_COMMONLISP:
    case HYP_LANG_SCHEME:
    case HYP_LANG_RACKET:
    case HYP_LANG_EMACSLISP:
    case HYP_LANG_FENNEL:
    case HYP_LANG_JANET:
    case HYP_LANG_NASM:
    case HYP_LANG_LLVM_IR:
    case HYP_LANG_BEANCOUNT:
        return semi;

    /* `%`. */
    case HYP_LANG_ERLANG:
    case HYP_LANG_MATLAB:
    case HYP_LANG_BIBTEX:
        return percent;

    /* `!`. */
    case HYP_LANG_FORTRAN:
        return bang;

    /* Block comments only. */
    case HYP_LANG_CSS:
        return block_c;

    /* `(* … *)` only. */
    case HYP_LANG_OCAML:
    case HYP_LANG_WOLFRAM:
        return ml_paren;

    /* `//` plus `(* … *)`. */
    case HYP_LANG_FSHARP:
    case HYP_LANG_PASCAL:
        return slash_paren;

    default:
        return none;
    }
}

/* ── The scan ──────────────────────────────────────────────────── */

static bool dt_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f';
}

/* Trim leading and trailing whitespace, and the '\r' of a CRLF line ending. */
static void dt_trim(const char *s, size_t len, const char **out, size_t *out_len) {
    size_t a = 0;
    while (a < len && dt_is_space(s[a])) {
        a++;
    }
    size_t b = len;
    while (b > a && dt_is_space(s[b - 1])) {
        b--;
    }
    *out = s + a;
    *out_len = b - a;
}

static bool dt_starts(const char *s, size_t len, const char *pat) {
    size_t n = strlen(pat);
    return len >= n && memcmp(s, pat, n) == 0;
}

static bool dt_ends(const char *s, size_t len, const char *pat) {
    size_t n = strlen(pat);
    return len >= n && memcmp(s + len - n, pat, n) == 0;
}

static bool dt_contains(const char *s, size_t len, const char *pat) {
    size_t n = strlen(pat);
    if (n == 0 || len < n) {
        return false;
    }
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(s + i, pat, n) == 0) {
            return true;
        }
    }
    return false;
}

/* Line-start byte offsets for lines `lo`..`start`, inclusive. Only the window
 * the scan is allowed to reach is materialised — 66 offsets, never the file. */
typedef struct {
    size_t off[HYP_ASK_DOC_MAX_LINES + 2];
    size_t count;
    int lo;
} dt_lines_t;

static void dt_line_at(const char *text, const dt_lines_t *L, int line, const char **out,
                       size_t *out_len) {
    size_t i = (size_t)(line - L->lo);
    size_t from = L->off[i];
    size_t to = L->off[i + 1];
    /* off[i+1] is the first byte of the next line, so step back over its '\n'
     * (and over a '\r' that precedes it — dt_trim would strip it anyway, but
     * not counting it keeps the byte budget honest). */
    if (to > from && text[to - 1] == '\n') {
        to--;
    }
    dt_trim(text + from, to - from, out, out_len);
}

int hyp_ask_leading_comment_start(const char *file_text, HYPLanguage lang, int start) {
    if (!file_text || start <= 1) {
        return start > 1 ? start : 1;
    }
    hyp_ask_comment_syntax_t syn = hyp_ask_comment_syntax(lang);
    if (!syn.line && !syn.open) {
        return start;
    }

    int lo = start - HYP_ASK_DOC_MAX_LINES;
    if (lo < 1) {
        lo = 1;
    }
    dt_lines_t L;
    L.lo = lo;
    L.count = 0;
    int line = 1;
    size_t i = 0;
    if (lo == 1) {
        L.off[L.count++] = 0;
    }
    while (line < start && file_text[i] != '\0') {
        if (file_text[i] == '\n') {
            line++;
            if (line >= lo && line <= start) {
                L.off[L.count++] = i + 1;
            }
        }
        i++;
    }
    if (line < start || L.count < 2) {
        /* The file has fewer lines than the span claims, or the span starts on
         * line 1 of a window with nothing above it. Either way there is no
         * comment to take, and inventing one is not on the menu. */
        return start;
    }

    size_t budget = HYP_ASK_DOC_MAX_BYTES;
    int block_start = start;
    int k = start - 1;
    while (k >= lo) {
        const char *p = NULL;
        size_t plen = 0;
        dt_line_at(file_text, &L, k, &p, &plen);
        if (plen == 0 || plen > budget) {
            break; /* a blank line ends the block; so does the byte cap */
        }
        if (syn.line && dt_starts(p, plen, syn.line)) {
            budget -= plen;
            block_start = k;
            k--;
            continue;
        }
        if (syn.close && syn.open && dt_ends(p, plen, syn.close)) {
            /* A block comment closes on this line. Walk up to the line that
             * OPENS it. A line that merely contains the opener without
             * starting with it is code with a comment on the end, which is not
             * this declaration's doc comment. */
            int j = k;
            size_t used = 0;
            bool found = false;
            while (j >= lo) {
                const char *q = NULL;
                size_t qlen = 0;
                dt_line_at(file_text, &L, j, &q, &qlen);
                if (qlen == 0 || used + qlen > budget) {
                    break;
                }
                used += qlen;
                if (dt_starts(q, qlen, syn.open)) {
                    found = true;
                    break;
                }
                if (dt_contains(q, qlen, syn.open)) {
                    break;
                }
                j--;
            }
            if (!found) {
                break;
            }
            budget -= used;
            block_start = j;
            k = j - 1;
            continue;
        }
        break;
    }
    return block_start;
}

/* ── The header line ───────────────────────────────────────────── */

size_t hyp_ask_header_line(char *buf, size_t cap, HYPLanguage lang, const char *label,
                           const char *qualified_name, const char *project,
                           const char *rel_path) {
    if (!buf || cap == 0) {
        return 0;
    }
    buf[0] = '\0';

    const char *qn = qualified_name ? qualified_name : "";
    if (project && project[0]) {
        size_t pl = strlen(project);
        if (strncmp(qn, project, pl) == 0 && qn[pl] == '.') {
            qn += pl + 1;
        }
    }
    const char *lb = (label && label[0]) ? label : NULL;
    const char *fp = (rel_path && rel_path[0]) ? rel_path : NULL;
    if (!qn[0] && !lb && !fp) {
        return 0;
    }

    /* "<Label> <name> in <file>", with each piece dropped when it is absent
     * rather than left as an empty slot — a header reading "Method  in " is
     * worse than a shorter one. */
    char body[HYP_ASK_DOC_HEADER_MAX];
    size_t w = 0;
    const char *pieces[3];
    int np = 0;
    if (lb) {
        pieces[np++] = lb;
    }
    if (qn[0]) {
        pieces[np++] = qn;
    }
    for (int p = 0; p < np; p++) {
        size_t n = strlen(pieces[p]);
        if (w && w + 1 < sizeof(body)) {
            body[w++] = ' ';
        }
        if (w + n >= sizeof(body)) {
            n = sizeof(body) - 1 - w;
        }
        memcpy(body + w, pieces[p], n);
        w += n;
    }
    if (fp) {
        const char *sep = w ? " in " : "";
        size_t sl = strlen(sep);
        size_t n = strlen(fp);
        if (w + sl + n >= sizeof(body)) {
            n = (w + sl < sizeof(body) - 1) ? sizeof(body) - 1 - w - sl : 0;
        }
        if (n > 0) {
            memcpy(body + w, sep, sl);
            w += sl;
            memcpy(body + w, fp, n);
            w += n;
        }
    }
    body[w] = '\0';
    if (w == 0) {
        return 0;
    }

    hyp_ask_comment_syntax_t syn = hyp_ask_comment_syntax(lang);
    size_t out = 0;
    if (syn.line) {
        size_t n = strlen(syn.line);
        if (n + 1 + w >= cap) {
            return 0;
        }
        memcpy(buf, syn.line, n);
        out = n;
        buf[out++] = ' ';
        memcpy(buf + out, body, w);
        out += w;
    } else if (syn.open && syn.close) {
        size_t no = strlen(syn.open);
        size_t nc = strlen(syn.close);
        if (no + 1 + w + 1 + nc >= cap) {
            return 0;
        }
        memcpy(buf, syn.open, no);
        out = no;
        buf[out++] = ' ';
        memcpy(buf + out, body, w);
        out += w;
        buf[out++] = ' ';
        memcpy(buf + out, syn.close, nc);
        out += nc;
    } else {
        /* No comment syntax we can vouch for. The header's CONTENT is what
         * carries the signal; the wrapping only keeps the document looking
         * like source. Emit it bare rather than lose it. */
        if (w >= cap) {
            return 0;
        }
        memcpy(buf, body, w);
        out = w;
    }
    buf[out] = '\0';
    return out;
}
