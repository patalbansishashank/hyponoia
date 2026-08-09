/*
 * userconfig.h — User-defined file extension → language mappings.
 *
 * Reads extra_extensions from two optional JSON config files:
 *   Global:  $XDG_CONFIG_HOME/hyponoia/config.json
 *            (falls back to ~/.config/hyponoia/config.json)
 *   Project: {repo_root}/.hyponoia.json
 *
 * Project config wins over global. Unknown language values warn and are
 * skipped (fail-open). Missing files are silently ignored.
 *
 * Format:
 *   {"extra_extensions": {".blade.php": "php", ".mjs": "javascript"}}
 *
 * The language string matching is case-insensitive.
 */
#ifndef HYP_USERCONFIG_H
#define HYP_USERCONFIG_H

#include "hyp.h" /* HYPLanguage */
#include "foundation/sha256.h"

/* ── Types ──────────────────────────────────────────────────────── */

typedef struct {
    char *ext;        /* file extension including dot, e.g. ".blade.php" */
    HYPLanguage lang; /* resolved language enum */
} hyp_userext_t;

typedef struct {
    hyp_userext_t *entries; /* heap-allocated array */
    int count;              /* number of entries */
    /* Digests of the exact bytes/state consumed by hyp_userconfig_load(). */
    char global_source_sha256[HYP_SHA256_HEX_LEN + 1];
    char project_source_sha256[HYP_SHA256_HEX_LEN + 1];
} hyp_userconfig_t;

/* ── API ────────────────────────────────────────────────────────── */

/*
 * Load user config from global + project files, merge (project wins).
 * repo_path: absolute path to the repository root (for project config).
 * Returns a heap-allocated hyp_userconfig_t (caller must free via
 * hyp_userconfig_free). Returns NULL only on allocation failure.
 * Missing config files are silently ignored.
 */
hyp_userconfig_t *hyp_userconfig_load(const char *repo_path);

/*
 * Look up a file extension in the user config.
 * ext: extension including dot, e.g. ".blade.php"
 * Returns the mapped HYPLanguage, or HYP_LANG_COUNT if not found.
 */
HYPLanguage hyp_userconfig_lookup(const hyp_userconfig_t *cfg, const char *ext);

/* Free a hyp_userconfig_t returned by hyp_userconfig_load. NULL-safe. */
void hyp_userconfig_free(hyp_userconfig_t *cfg);

/* ── Integration hook ───────────────────────────────────────────── */

/*
 * Set the process-global user config that hyp_language_for_extension()
 * will consult before the built-in table.
 * cfg may be NULL to clear the override.
 * Not thread-safe — call before spawning worker threads.
 */
void hyp_set_user_lang_config(const hyp_userconfig_t *cfg);

/*
 * Get the currently active process-global user config.
 * Returns NULL if none has been set.
 * Called internally by hyp_language_for_extension().
 */
const hyp_userconfig_t *hyp_get_user_lang_config(void);

#endif /* HYP_USERCONFIG_H */
