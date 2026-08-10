/*
 * discover.h — File discovery, language detection, and gitignore matching.
 *
 * Provides:
 *   - Language detection from filename/extension (HYPLanguage registry)
 *   - .m file disambiguation (Objective-C vs Magma vs MATLAB)
 *   - Gitignore-style pattern parsing and matching
 *   - Recursive directory walk with hardcoded + gitignore filtering
 *
 * Depends on: foundation (platform.h for file ops), hyp.h (HYPLanguage enum)
 */
#ifndef HYP_DISCOVER_H
#define HYP_DISCOVER_H

#include <stdbool.h>
#include <stdint.h>

/* Use the existing HYPLanguage enum from extraction layer */
#include "hyp.h"

/* ── Language detection ──────────────────────────────────────────── */

/* Detect language from a filename (basename only, not full path).
 * Checks special filenames first (Makefile, CMakeLists.txt, etc.),
 * then falls back to extension-based lookup.
 * Returns HYP_LANG_COUNT if unknown. */
HYPLanguage hyp_language_for_filename(const char *filename);

/* Detect language from a file extension (including the dot, e.g. ".go").
 * Returns HYP_LANG_COUNT if unknown. */
HYPLanguage hyp_language_for_extension(const char *ext);

/* Get the human-readable name for a language enum value.
 * Returns "Unknown" for HYP_LANG_COUNT or out-of-range values. */
const char *hyp_language_name(HYPLanguage lang);

/* Get the name to interpolate into the `ask` instruct prefix (§2.1) — the
 * spelling a developer writes in prose, which is what the embedding model
 * saw in training: "C++", not "cpp"; "TypeScript", not "tsx".
 *
 * Same table as hyp_language_name() for 155 of 163 languages, with a small
 * documented override set (see LANG_PROMPT_NAMES in language.c) where the
 * file label and the prose name differ.
 *
 * Returns NULL — never "Unknown", never the grammar id — for out-of-range
 * values and for any language with no name. Callers MUST NOT substitute a
 * fallback: a prefix built around the wrong token still returns ranked
 * results, so the only way this failure is visible is if it refuses. */
const char *hyp_language_prompt_name(HYPLanguage lang);

/* Disambiguate .m files by reading first 4KB of content.
 * Returns HYP_LANG_OBJC, HYP_LANG_MAGMA, or HYP_LANG_MATLAB.
 * On read failure, defaults to HYP_LANG_MATLAB. */
HYPLanguage hyp_disambiguate_m(const char *path);

/* Disambiguate .cls files by reading first 4KB of content.
 * Returns HYP_LANG_OBJECTSCRIPT_UDL if a line starts with "Class <Uppercase>",
 * otherwise HYP_LANG_APEX. On read failure, defaults to HYP_LANG_APEX. */
HYPLanguage hyp_disambiguate_cls(const char *path);

/* Disambiguate .inc files by reading first 4KB of content.
 * Returns HYP_LANG_OBJECTSCRIPT_ROUTINE if it looks like an ObjectScript
 * include (a "ROUTINE <Uppercase>" header), otherwise HYP_LANG_BITBAKE.
 * On read failure, defaults to HYP_LANG_BITBAKE. */
HYPLanguage hyp_disambiguate_inc(const char *path);

/* ── Gitignore pattern matching ──────────────────────────────────── */

typedef struct hyp_gitignore hyp_gitignore_t;

/* Parse gitignore patterns from a file. Returns NULL on error (file not found, etc.).
 * Caller must call hyp_gitignore_free(). */
hyp_gitignore_t *hyp_gitignore_load(const char *path);

/* Parse gitignore patterns from a string (for testing).
 * Caller must call hyp_gitignore_free(). */
hyp_gitignore_t *hyp_gitignore_parse(const char *content);

/* Check if a relative path matches any gitignore pattern.
 * rel_path should use '/' separators. is_dir indicates if path is a directory. */
bool hyp_gitignore_matches(const hyp_gitignore_t *gi, const char *rel_path, bool is_dir);

/* Free a gitignore matcher. NULL-safe. */
void hyp_gitignore_free(hyp_gitignore_t *gi);

/* Append all patterns from src into dst. dst takes ownership of deep copies
 * of each src pattern; src is unchanged and must still be freed by the caller.
 * NULL-safe on either argument.
 * Returns true on success (or when there is nothing to merge). Returns false on
 * allocation failure, in which case dst is left exactly as it was (atomic) — no
 * partial merge — so a failed merge degrades to "as if src was absent". */
bool hyp_gitignore_merge(hyp_gitignore_t *dst, const hyp_gitignore_t *src);

/* ── Directory skip / suffix filters ─────────────────────────────── */

/* Index mode controls filtering aggressiveness.
 * IMPORTANT: these values MUST match pipeline.h exactly.  A previous
 * mismatch (this header had FAST=1, pipeline.h has FAST=2) caused
 * fast-mode filtering to silently no-op depending on include order —
 * the pipeline passed value 2, discover.c compared against 1, and no
 * files got filtered. */
#ifndef HYP_INDEX_MODE_T_DEFINED
#define HYP_INDEX_MODE_T_DEFINED
typedef enum {
    HYP_MODE_FULL = 0,     /* parse everything supported */
    HYP_MODE_MODERATE = 1, /* aggressive filtering + similarity/semantic edges */
    HYP_MODE_FAST = 2,     /* aggressive filtering + no similarity/semantic edges */
} hyp_index_mode_t;
#endif

/* Check if a directory name should always be skipped (e.g. .git, node_modules).
 * Only checks the basename, not the full path. */
bool hyp_should_skip_dir(const char *dirname, hyp_index_mode_t mode);

/* Check if a file has a suffix that should be skipped (e.g. .pyc, .png). */
bool hyp_has_ignored_suffix(const char *filename, hyp_index_mode_t mode);

/* Check if a specific filename should be skipped in fast mode (e.g. LICENSE, go.sum). */
bool hyp_should_skip_filename(const char *filename, hyp_index_mode_t mode);

/* Check if a path matches fast-mode substring patterns (e.g. .d.ts, .pb.go). */
bool hyp_matches_fast_pattern(const char *filename, hyp_index_mode_t mode);

/* ── File discovery ──────────────────────────────────────────────── */

typedef struct {
    char *path;           /* absolute path (heap-allocated) */
    char *rel_path;       /* relative to repo root (heap-allocated) */
    HYPLanguage language; /* detected language */
    int64_t size;         /* file size in bytes */
} hyp_file_info_t;

typedef struct {
    hyp_index_mode_t mode;   /* HYP_MODE_FULL or HYP_MODE_FAST */
    const char *ignore_file; /* path to .hypignore file, or NULL */
    int64_t max_file_size;   /* 0 = no limit */
} hyp_discover_opts_t;

typedef enum {
    HYP_DISCOVER_ERROR = -1,
    HYP_DISCOVER_OK = 0,
    HYP_DISCOVER_LIMIT_EXCEEDED = 1,
} hyp_discover_status_t;

/* Walk a repository directory tree and discover all source files.
 * Applies hardcoded filters, gitignore patterns, and language detection.
 * Returns 0 on success, -1 on error.
 * Caller must call hyp_discover_free() on the results. */
int hyp_discover(const char *repo_path, const hyp_discover_opts_t *opts, hyp_file_info_t **out,
                 int *count);

/* Apply the exact same full discovery/filter policy without retaining a file
 * array. Stops before counting more than max_files and performs no per-file
 * allocation. deadline_ms is an absolute hyp_now_ms() deadline; zero disables
 * it. Returns LIMIT_EXCEEDED when at least max_files + 1 indexable files exist,
 * ERROR on traversal/deadline/allocation failure, or OK with the exact count. */
hyp_discover_status_t hyp_discover_count_bounded(const char *repo_path,
                                                 const hyp_discover_opts_t *opts, int max_files,
                                                 uint64_t deadline_ms, int *count_out);

/* Like hyp_discover(), but also reports the directory subtrees that were
 * skipped during the walk (hardcoded ALWAYS_SKIP/FAST_SKIP dirs + gitignore
 * matches), so callers can surface which subtrees were dropped (#411).
 * On success, *excluded_out receives a heap-allocated array of strdup'd
 * relative directory paths and *excluded_count_out its length; the caller
 * owns it and must free via hyp_discover_free_excluded(). Pass NULL for
 * excluded_out (and/or excluded_count_out) to discard the list — the internal
 * accumulator is freed in that case (no leak).
 * Returns 0 on success, -1 on error. */
int hyp_discover_ex(const char *repo_path, const hyp_discover_opts_t *opts, hyp_file_info_t **out,
                    int *count, char ***excluded_out, int *excluded_count_out);

/* One deliberately-not-indexed file (#963): an individual file dropped by an
 * ignore mechanism during the walk (its parent directory was NOT excluded —
 * whole subtrees are reported separately as excluded dirs). BY DESIGN, not a
 * failure. */
typedef struct {
    char *rel_path; /* heap-allocated, relative to repo root */
    char *reason;   /* heap-allocated: "gitignore" | "hypignore" |
                     * "skip-list" | "ignored-suffix" | "fast-pattern" |
                     * "size-cap" */
} hyp_ignored_file_t;

/* Stored per-file ignore entries are capped (the walk still counts ALL of
 * them in *ignored_total_out, so truncation is always explicit, never
 * silent). Whole excluded subtrees stay exhaustive via excluded_out. */
enum { HYP_DISCOVER_IGNORED_CAP = 2000 };

/* Like hyp_discover_ex(), but additionally reports the individual files that
 * ignore rules dropped (#963 "purposely not indexed"). *ignored_out receives
 * a heap array (caller frees via hyp_discover_free_ignored),
 * *ignored_count_out its stored length (<= HYP_DISCOVER_IGNORED_CAP), and
 * *ignored_total_out the TOTAL number of ignored files seen. Pass NULL to
 * skip the collection entirely. */
int hyp_discover_ex2(const char *repo_path, const hyp_discover_opts_t *opts, hyp_file_info_t **out,
                     int *count, char ***excluded_out, int *excluded_count_out,
                     hyp_ignored_file_t **ignored_out, int *ignored_count_out,
                     int *ignored_total_out);

/* Free an array of file info results. NULL-safe. */
void hyp_discover_free(hyp_file_info_t *files, int count);

/* Free the excluded-directory list returned by hyp_discover_ex(). NULL-safe. */
void hyp_discover_free_excluded(char **excluded, int count);

/* Free the ignored-file list returned by hyp_discover_ex2(). NULL-safe. */
void hyp_discover_free_ignored(hyp_ignored_file_t *ignored, int count);

#endif /* HYP_DISCOVER_H */
