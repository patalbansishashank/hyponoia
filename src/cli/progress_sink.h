/*
 * progress_sink.h — Human-readable progress for one-shot CLI commands.
 *
 * Installs a log sink that maps structured pipeline events to phase labels.
 * Interactive terminals enable it automatically; --progress forces it when
 * stderr is redirected.
 * Usage:
 *   hyp_progress_sink_init(stderr);
 *   // ... run pipeline ...
 *   hyp_progress_sink_fini();
 */
#ifndef HYP_PROGRESS_SINK_H
#define HYP_PROGRESS_SINK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Interactive terminals get lifecycle feedback automatically. --progress
 * forces the same behavior for redirected stderr without touching stdout. */
bool hyp_cli_progress_enabled(bool explicitly_requested, bool stderr_is_tty);
void hyp_cli_progress_start(FILE *out, const char *tool_name);
void hyp_cli_progress_finish(FILE *out, const char *tool_name, bool success, uint64_t elapsed_ms);

void hyp_progress_sink_init(FILE *out);
void hyp_progress_sink_fini(void);
void hyp_progress_sink_fn(const char *line);

#endif
