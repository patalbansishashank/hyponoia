/*
 * log.h — Structured key-value logging to stderr.
 *
 * Design:
 *   - All output goes to stderr (stdout is reserved for MCP JSON-RPC)
 *   - Structured text format: "level=info msg=pass.timing pass=defs elapsed_ms=42"
 *   - Optional JSON format for local structured parsing
 *   - Levels: DEBUG, INFO, WARN, ERROR
 *   - Level filtering at runtime via hyp_log_set_level() or the
 *     HYP_LOG_LEVEL env var (see hyp_log_init_from_env)
 *   - Thread-safe (each fprintf is atomic on POSIX for lines < PIPE_BUF)
 */
#ifndef HYP_LOG_H
#define HYP_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    HYP_LOG_DEBUG = 0,
    HYP_LOG_INFO = 1,
    HYP_LOG_WARN = 2,
    HYP_LOG_ERROR = 3,
    HYP_LOG_NONE = 4 /* disable all logging */
} HYPLogLevel;

typedef enum {
    HYP_LOG_FORMAT_TEXT = 0,
    HYP_LOG_FORMAT_JSON = 1,
} HYPLogFormat;

typedef enum {
    HYP_LOG_SINK_REPLACE = 0,
    HYP_LOG_SINK_TEE = 1,
} HYPLogSinkMode;

/* Apply the HYP_LOG_LEVEL environment variable to the runtime log level.
 * Accepts (case-insensitive) "debug", "info", "warn", "error", "none", or
 * the numeric equivalents 0..4 matching HYPLogLevel. Unknown, empty, or
 * unset values leave the level unchanged (fail-open).
 *
 * Also applies HYP_LOG_FORMAT=text|json. If unset, the current format is left
 * unchanged. Call once at startup before any threads or log lines. */
void hyp_log_init_from_env(void);

/* Set minimum log level (default: INFO). */
void hyp_log_set_level(HYPLogLevel level);

/* Get current log level. */
HYPLogLevel hyp_log_get_level(void);

/* Set/get output format. Default is text. */
void hyp_log_set_format(HYPLogFormat format);
HYPLogFormat hyp_log_get_format(void);

/* Core logging function. msg is a short semantic tag.
 * Variadic args are key-value pairs: (const char *key, const char *value)...
 * Terminated by NULL key.
 *
 * Example:
 *   hyp_log(HYP_LOG_INFO, "pass.timing",
 *           "pass", "defs", "elapsed_ms", "42", NULL);
 *
 * Output:
 *   level=info msg=pass.timing pass=defs elapsed_ms=42
 */
void hyp_log(HYPLogLevel level, const char *msg, ...);

/* Convenience macros. */
#define hyp_log_debug(msg, ...) hyp_log(HYP_LOG_DEBUG, msg, ##__VA_ARGS__, NULL)
#define hyp_log_info(msg, ...) hyp_log(HYP_LOG_INFO, msg, ##__VA_ARGS__, NULL)

/* Always-delivered internal control/discovery record. It bypasses the level
 * threshold and always uses the JSON encoding, so exact values (paths with
 * spaces or control bytes) survive unambiguously; it flows through the
 * configured sink like every other record. Reserve it for the rare
 * discovery/control events that ordinary log filtering must never suppress
 * (e.g. diagnostics.start path announcement). */
void hyp_log_control_record(const char *msg, ...);
#define hyp_log_control(msg, ...) hyp_log_control_record(msg, ##__VA_ARGS__, NULL)
#define hyp_log_warn(msg, ...) hyp_log(HYP_LOG_WARN, msg, ##__VA_ARGS__, NULL)
#define hyp_log_error(msg, ...) hyp_log(HYP_LOG_ERROR, msg, ##__VA_ARGS__, NULL)

/* Log with integer value (avoids sprintf for common case). */
void hyp_log_int(HYPLogLevel level, const char *msg, const char *key, int64_t value);

/* Operational event helpers. They deliberately avoid request bodies, headers,
 * arguments, and query strings. */
void hyp_log_mcp_request(const char *method, const char *tool_name, bool is_error,
                         int64_t duration_us);
void hyp_log_http_request(const char *component, const char *method, const char *path, int status,
                          int64_t duration_ms, size_t request_bytes, size_t response_bytes);

/* Optional log sink callback — called with the formatted log line. */
typedef void (*hyp_log_sink_fn)(const char *line);
void hyp_log_set_sink(hyp_log_sink_fn fn);
void hyp_log_set_sink_ex(hyp_log_sink_fn fn, HYPLogSinkMode mode);

#endif /* HYP_LOG_H */
