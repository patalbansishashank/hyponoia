/* compact_out.h — TOON (Token-Oriented Object Notation) emission helpers.
 *
 * Tool responses are consumed by LLM agents, where every byte is context
 * tokens. TOON encodes the same data as JSON but declares tabular fields
 * once in a header and streams rows line by line, cutting 40-60% of tokens
 * on homogeneous result sets at equal-or-better retrieval accuracy
 * (toonformat.dev/guide/benchmarks). Emitters here cover the subset we
 * emit: scalar key-value lines and flat tables with explicit [N] lengths.
 *
 * Quoting: a cell/scalar is double-quoted iff it is empty, has leading or
 * trailing whitespace, contains a comma, quote, newline, or CR, or would
 * read as a non-string literal (true/false/null/number). Quotes and
 * backslashes are escaped JSON-style; newlines become \n.
 */
#ifndef HYP_MCP_COMPACT_OUT_H
#define HYP_MCP_COMPACT_OUT_H

#include <stdbool.h>
#include <stddef.h>

/* Minimal growing string buffer (OOM-safe: sticky flag, finish returns NULL). */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} hyp_sb_t;

void hyp_sb_init(hyp_sb_t *sb);
void hyp_sb_append_n(hyp_sb_t *sb, const char *s, size_t n);
void hyp_sb_append(hyp_sb_t *sb, const char *s);
/* Returns the heap buffer (caller frees) and resets sb. NULL on OOM. */
char *hyp_sb_finish(hyp_sb_t *sb);
void hyp_sb_free(hyp_sb_t *sb);

/* `key: value` scalar lines (top-level, no indent). */
void hyp_tree_scalar_str(hyp_sb_t *sb, const char *key, const char *val);
void hyp_tree_scalar_int(hyp_sb_t *sb, const char *key, long long v);
void hyp_tree_scalar_bool(hyp_sb_t *sb, const char *key, bool v);

/* `key: n  (cols: col1 col2 ...)` table header; rows follow at 2-space
 * indent. NOTE: this comment used to describe a `key[n]{col1,col2}:` form the
 * emitter has never produced — a test written from the header failed against
 * working code. Kept in sync with hyp_tree_table_header() below. */
void hyp_tree_table_header(hyp_sb_t *sb, const char *key, int n, const char *const *cols,
                           int ncols);

/* Row cells: call row_begin, then cell_* per column (first=true for the
 * first cell), then row_end. Empty/NULL strings emit as empty cells. */
void hyp_tree_row_begin(hyp_sb_t *sb);
void hyp_tree_cell_str(hyp_sb_t *sb, const char *val, bool first);
void hyp_tree_cell_int(hyp_sb_t *sb, long long v, bool first);
void hyp_tree_cell_real(hyp_sb_t *sb, double v, bool first);
void hyp_tree_cell_bool(hyp_sb_t *sb, bool v, bool first);
void hyp_tree_row_end(hyp_sb_t *sb);

#endif /* HYP_MCP_COMPACT_OUT_H */
