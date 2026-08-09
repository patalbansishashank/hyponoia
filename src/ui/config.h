/*
 * config.h — Persistent UI configuration.
 *
 * Stores ui_enabled and ui_port in ~/.cache/hyponoia/config.json.
 * Thread-safe: load/save are independent operations on the filesystem.
 */
#ifndef HYP_UI_CONFIG_H
#define HYP_UI_CONFIG_H

#include <stdbool.h>

/* Default values */
#define HYP_UI_DEFAULT_PORT 9749
#define HYP_UI_DEFAULT_ENABLED false

typedef struct {
    bool ui_enabled;
    int ui_port;
} hyp_ui_config_t;

/* Load config from disk. Missing/corrupt file → defaults. */
void hyp_ui_config_load(hyp_ui_config_t *cfg);

/* Atomically save one complete config generation. Creates the directory if
 * needed and reports write/sync/replace failures. */
bool hyp_ui_config_save(const hyp_ui_config_t *cfg);

/* Get the config file path. Writes to buf (up to bufsz bytes).
 * Exposed for testing. */
void hyp_ui_config_path(char *buf, int bufsz);

#endif /* HYP_UI_CONFIG_H */
