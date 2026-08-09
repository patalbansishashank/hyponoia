/*
 * yaml.h — Minimal YAML parser for config files.
 *
 * Handles the subset needed by .cgrconfig:
 *   - key: value pairs (string, float, bool)
 *   - Nested maps (indentation-based)
 *   - String lists (- item)
 *   - Comment lines (#)
 *
 * NOT a general YAML parser — no multiline strings, anchors, flow style, etc.
 */
#ifndef HYP_YAML_H
#define HYP_YAML_H

#include <stdbool.h>

typedef struct hyp_yaml_node hyp_yaml_node_t;

/* Parse a YAML string into a tree. Returns NULL on error.
 * Caller must free with hyp_yaml_free(). */
hyp_yaml_node_t *hyp_yaml_parse(const char *text, int len);

/* Free a parsed YAML tree. */
void hyp_yaml_free(hyp_yaml_node_t *root);

/* Get a scalar value by dot-separated path (e.g. "http_linker.min_confidence").
 * Returns NULL if not found or not a scalar. */
const char *hyp_yaml_get_str(const hyp_yaml_node_t *root, const char *path);

/* Get a float value by path, returning default_val if not found. */
double hyp_yaml_get_float(const hyp_yaml_node_t *root, const char *path, double default_val);

/* Get a bool value by path, returning default_val if not found.
 * Recognizes: true/false, yes/no, on/off (case-insensitive). */
bool hyp_yaml_get_bool(const hyp_yaml_node_t *root, const char *path, bool default_val);

/* Get a list of string values at a path (e.g. "http_linker.exclude_paths").
 * Writes up to max_out pointers into out[]. Pointers are owned by the YAML tree.
 * Returns count of items written. */
int hyp_yaml_get_str_list(const hyp_yaml_node_t *root, const char *path, const char **out,
                          int max_out);

/* Check if a node at the given path exists. */
bool hyp_yaml_has(const hyp_yaml_node_t *root, const char *path);

#endif /* HYP_YAML_H */
