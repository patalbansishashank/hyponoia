/*
 * cli.h — CLI subcommand handlers for install, uninstall, update, version.
 *
 * Port of Go cmd/hyponoia/ install/update/config logic.
 *
 * Functions accept explicit paths (home_dir, binary_path) rather than
 * reading HOME internally, making them testable with temp directories.
 */
#ifndef HYP_CLI_H
#define HYP_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hyp_mcp_server hyp_mcp_server_t;

/* ── Version ──────────────────────────────────────────────────── */

/* Set the version string (called from main). */
void hyp_cli_set_version(const char *ver);

/* Get the version string. */
const char *hyp_cli_get_version(void);

/* ── CLI tool arguments (flags / --args-file / --help) ────────── */

/* Convert `--flag value` / `--flag=value` / bare-boolean `--flag` arguments for
 * a tool into a JSON arguments object string, using the tool's input_schema to
 * type values (string/integer/boolean) and to collect repeated flags into
 * array-typed properties. kebab-case flags map to snake_case keys
 * (--repo-path -> repo_path). A bare `--` ends flag parsing. On error returns
 * NULL and, if err_out is non-NULL, sets *err_out to a heap message the caller
 * must free. Caller frees the returned JSON string. */
char *hyp_cli_build_args_json(const char *tool_name, int argc, char **argv, char **err_out);

/* Print per-tool help (usage + the tool's flags with type/description/required)
 * derived from its input_schema, to stdout. Returns 0 if the tool is known,
 * non-zero (and prints nothing) if it is not. */
int hyp_cli_print_tool_help(const char *tool_name);

/* Inspect a raw MCP tool-result envelope. Returns true only when the root
 * object carries the exact boolean field `isError: true`; malformed JSON,
 * strings, and nested lookalikes are not tool errors. */
bool hyp_cli_mcp_result_is_error(const char *result);

/* Maintenance cancellation is authoritative even if a tool races to produce
 * a nominal success result. Preserve an existing failure code; otherwise turn
 * accepted cancellation into EXIT_FAILURE. */
int hyp_cli_exit_status_after_maintenance(int exit_status, bool maintenance_cancelled);

/* ── Self-update: version comparison ──────────────────────────── */

/* Compare two semver strings (e.g. "0.2.1" vs "0.2.0").
 * Returns >0 if a > b, <0 if a < b, 0 if equal.
 * Handles v-prefix and -dev suffix. */
int hyp_compare_versions(const char *a, const char *b);

/* ── Shell RC detection ───────────────────────────────────────── */

/* Detect the appropriate shell RC file path for the current user.
 * Uses SHELL env var. home_dir is the user's home directory.
 * Returns static buffer — do NOT free. Returns "" if unknown. */
const char *hyp_detect_shell_rc(const char *home_dir);

/* ── CLI binary detection ─────────────────────────────────────── */

/* Find a CLI binary by name.
 * Checks PATH first, then common install locations.
 * Returns static buffer — do NOT free. Returns "" if not found. */
const char *hyp_find_cli(const char *name, const char *home_dir);

/* ── File utilities ───────────────────────────────────────────── */

/* Copy a file from src to dst. Returns 0 on success, -1 on error. */
int hyp_copy_file(const char *src, const char *dst);

/* Copy the running binary into the canonical install target, preserving the
 * executable bit, skipping the copy when src and dst are the same file (which
 * would otherwise truncate the running binary). Returns 0 on success or skip,
 * -1 on error. Regression surface for the install --force binary-swap bug. */
int hyp_copy_binary_to_target(const char *src, const char *dst);

/* Replace a binary file: unlinks the existing file first (handles read-only),
 * then creates a new file with the given data and permissions.
 * Returns 0 on success, -1 on error. */
int hyp_replace_binary(const char *path, const unsigned char *data, int len, int mode);

/* ── Skill file management ────────────────────────────────────── */

/* Number of skill files. */
#define HYP_SKILL_COUNT 1

/* Skill name/content pair. */
typedef struct {
    const char *name;    /* e.g. "hyponoia" */
    const char *content; /* full SKILL.md content */
} hyp_skill_t;

/* Get the array of skill definitions. */
const hyp_skill_t *hyp_get_skills(void);

/* Install skills to skills_dir (e.g. ~/.claude/skills/).
 * If force is true, overwrite existing skills.
 * Returns count of skills written. */
int hyp_install_skills(const char *skills_dir, bool force, bool dry_run);

/* Remove skills from skills_dir.
 * Returns count of skills removed. */
int hyp_remove_skills(const char *skills_dir, bool dry_run);

/* Remove old monolithic skill dir if it exists.
 * Returns true if it was removed. */
bool hyp_remove_old_monolithic_skill(const char *skills_dir, bool dry_run);

/* ── Editor MCP config management ─────────────────────────────── */

/* Install MCP server entry in Cursor/Windsurf/Gemini JSON config.
 * Format: { "mcpServers": { "hyponoia": { "command": binary_path } } }
 * Preserves existing entries. Returns 0 on success. */
int hyp_install_editor_mcp(const char *binary_path, const char *config_path);

/* Remove MCP server entry from Cursor/Windsurf/Gemini JSON config.
 * Returns 0 on success. */
int hyp_remove_editor_mcp(const char *config_path);
int hyp_remove_editor_mcp_owned(const char *binary_path, const char *config_path);

/* Install MCP server entry in OpenClaw JSON config.
 * Format: { "mcp": { "servers": { "hyponoia":
 * { "enabled": true, "command": binary_path, "args": [] } } } }
 * Preserves existing entries. Returns 0 on success. */
int hyp_install_openclaw_mcp(const char *binary_path, const char *config_path);

/* Remove MCP server entry from OpenClaw JSON config.
 * Returns 0 on success. */
int hyp_remove_openclaw_mcp(const char *config_path);
int hyp_remove_openclaw_mcp_owned(const char *binary_path, const char *config_path);

/* Install MCP server entry in VS Code JSON config.
 * Format: { "servers": { "hyponoia": { "type": "stdio", "command": binary_path } } }
 * Returns 0 on success. */
int hyp_install_vscode_mcp(const char *binary_path, const char *config_path);

/* Remove MCP server entry from VS Code JSON config.
 * Returns 0 on success. */
int hyp_remove_vscode_mcp(const char *config_path);
int hyp_remove_vscode_mcp_owned(const char *binary_path, const char *config_path);

/* Install MCP server entry in Zed settings.json.
 * Format: { "context_servers": { "hyponoia": { "command": path, "args": [] } } }
 * Returns 0 on success. */
int hyp_install_zed_mcp(const char *binary_path, const char *config_path);

/* Remove MCP server entry from Zed settings.json.
 * Returns 0 on success. */
int hyp_remove_zed_mcp(const char *config_path);
int hyp_remove_zed_mcp_owned(const char *binary_path, const char *config_path);

/* ── Agent detection ──────────────────────────────────────────── */

/* Detected coding agents on the system. */
typedef struct {
    bool claude_code;   /* ~/.claude/ exists */
    bool codex;         /* $CODEX_HOME or ~/.codex exists */
    bool gemini;        /* Gemini settings or executable exists */
    bool zed;           /* platform-specific Zed config dir exists */
    bool opencode;      /* opencode on PATH or config exists */
    bool antigravity;   /* Antigravity CLI config or executable exists */
    bool aider;         /* aider on PATH */
    bool kilocode;      /* KiloCode globalStorage dir exists */
    bool vscode;        /* VS Code User config dir exists */
    bool cursor;        /* ~/.cursor/ exists */
    bool windsurf;      /* ~/.codeium/windsurf/ exists */
    bool augment;       /* ~/.augment/ or Auggie CLI exists */
    bool openclaw;      /* ~/.openclaw/ exists */
    bool kiro;          /* ~/.kiro/ exists */
    bool junie;         /* ~/.junie/ exists */
    bool hermes;        /* ~/.hermes/ or hermes CLI exists */
    bool openhands;     /* ~/.openhands/ or openhands CLI exists */
    bool cline;         /* ~/.cline/ or cline CLI exists */
    bool warp;          /* Warp footprint or oz/oz-preview/warp-cli exists */
    bool qwen;          /* ~/.qwen/ or qwen CLI exists */
    bool copilot_cli;   /* $COPILOT_HOME, ~/.copilot/, or copilot CLI exists */
    bool factory_droid; /* ~/.factory/ or droid CLI exists */
    bool crush;         /* Crush config or CLI exists */
    bool goose;         /* Goose config or CLI exists */
    bool mistral_vibe;  /* $VIBE_HOME, ~/.vibe/, or vibe CLI exists */
} hyp_detected_agents_t;

/* Detect which coding agents are installed.
 * Checks config dirs and PATH. home_dir is used for config dir checks. */
hyp_detected_agents_t hyp_detect_agents(const char *home_dir);

/* Install or refresh every detected agent integration below home. */
int hyp_install_agent_configs(const char *home, const char *binary_path, bool force, bool dry_run);

#ifdef HYP_CLI_ENABLE_TEST_API
int hyp_build_qwen_hook_command_for_testing(const char *binary_path, bool windows, char *command,
                                            size_t command_size, char *shell, size_t shell_size);
int hyp_build_qoder_hook_command_for_testing(const char *binary_path, bool windows, char *command,
                                             size_t command_size, char *shell, size_t shell_size);
int hyp_resolve_claude_hook_command_for_testing(const char *script_name, bool windows,
                                                char *command, size_t command_size);
bool hyp_optional_hook_supported_for_testing(const char *agent_name, bool windows);
void hyp_hook_sanitize_metadata_for_testing(const char *input, char *output, size_t output_size);
char *hyp_hook_augment_format_context_for_testing(const char *envelope, const char *token,
                                                  bool *is_error);
int hyp_upsert_qwen_lifecycle_hooks_for_testing(const char *settings_path, const char *binary_path,
                                                bool windows);
int hyp_upsert_qoder_context_hooks_for_testing(const char *settings_path, const char *binary_path);
int hyp_remove_qoder_context_hooks_for_testing(const char *settings_path, const char *binary_path);
/* Explicit lifecycle adapter seam for hook protocols whose output envelope is
 * not Claude/Gemini-compatible. Returns allocated JSON or NULL to fail open. */
char *hyp_hook_augment_lifecycle_json_for_dialect(const char *input, const char *forced_event,
                                                  const char *dialect);
char *hyp_hook_augment_tool_json_for_testing(const char *input, const char *dialect,
                                             const char *context, char *path, size_t path_size);
bool hyp_hook_augment_invocation_supported_for_testing(const char *dialect,
                                                       const char *forced_event);
bool hyp_hook_path_contains_for_testing(const char *root, const char *candidate,
                                        bool case_insensitive);
const char *hyp_hook_no_project_index_guidance_for_testing(const char *event);
bool hyp_mcp_command_path_probe_safe_for_testing(const char *command, bool windows);
void hyp_set_mcp_command_path_probe_counter_for_testing(int *counter);
int hyp_install_editor_mcp_with_previous_for_testing(const char *binary_path,
                                                     const char *previous_binary_path,
                                                     const char *config_path);
int hyp_upsert_junie_mcp_with_previous_for_testing(const char *binary_path,
                                                   const char *previous_binary_path,
                                                   const char *config_path);
#endif

/* ── Agent MCP config upsert (per agent) ──────────────────────── */

/* Codex CLI: upsert MCP entry in $CODEX_HOME/config.toml. Returns 0 on success. */
int hyp_upsert_codex_mcp(const char *binary_path, const char *config_path);

/* Remove CMM MCP entry from Codex config.toml. Returns 0 on success. */
int hyp_remove_codex_mcp(const char *config_path);

/* OpenCode: upsert MCP entry in opencode.json. Returns 0 on success. */
int hyp_upsert_opencode_mcp(const char *binary_path, const char *config_path);

/* Remove CMM MCP entry from opencode.json. Returns 0 on success. */
int hyp_remove_opencode_mcp(const char *config_path);
int hyp_remove_opencode_mcp_owned(const char *binary_path, const char *config_path);

/* Antigravity: upsert MCP entry in ~/.gemini/config/mcp_config.json.
 * Returns 0 on success. */
int hyp_upsert_antigravity_mcp(const char *binary_path, const char *config_path);

/* Remove CMM MCP entry from antigravity mcp_config.json. Returns 0 on success. */
int hyp_remove_antigravity_mcp(const char *config_path);
int hyp_remove_antigravity_mcp_owned(const char *binary_path, const char *config_path);

/* Junie (JetBrains): upsert MCP entry in ~/.junie/mcp/mcp.json (mcpServers format).
 * Returns 0 on success. */
int hyp_upsert_junie_mcp(const char *binary_path, const char *config_path);

/* Remove CMM MCP entry from Junie mcp.json. Returns 0 on success. */
int hyp_remove_junie_mcp(const char *config_path);
int hyp_remove_junie_mcp_owned(const char *binary_path, const char *config_path);

/* ── Instructions file upsert ─────────────────────────────────── */

/* Upsert a hyponoia instruction section in a markdown file.
 * Uses <!-- hyponoia:start --> / <!-- hyponoia:end --> markers.
 * If markers exist, replaces content between them. Otherwise appends.
 * If file doesn't exist, creates it. Returns 0 on success. */
int hyp_upsert_instructions(const char *path, const char *content);

/* Remove the hyponoia instruction section from a markdown file.
 * Returns 0 on success, 1 if not found. */
int hyp_remove_instructions(const char *path);

/* Get the shared agent instructions content (markdown). */
const char *hyp_get_agent_instructions(void);

/* #1032: Aider variant — CLI-form discovery commands (Aider has no MCP). */
const char *hyp_get_aider_instructions(void);

/* ── Pre-tool hook management ─────────────────────────────────── */

/* Upsert a PreToolUse hook in ~/.claude/settings.json for Claude Code.
 * Adds a Grep|Glob matcher that reminds to use MCP tools.
 * Returns 0 on success. */
int hyp_upsert_claude_hooks(const char *settings_path);

/* Remove our PreToolUse hook from Claude Code settings.json.
 * Returns 0 on success. */
int hyp_remove_claude_hooks(const char *settings_path);

/* Write the PreToolUse gate shim to <home>/.claude/hooks/. The shim is a thin
 * wrapper that invokes the compiled `hook-augment` and writes to stdout only —
 * it must never create a predictable temp/state file (issue #384). Exposed for
 * testing that security property. */
bool hyp_install_hook_gate_script(const char *home, const char *binary_path);

/* Upsert a BeforeTool hook in ~/.gemini/settings.json for Gemini CLI.
 * Returns 0 on success. */
int hyp_upsert_gemini_hooks(const char *settings_path);

/* Remove our BeforeTool hook from Gemini settings.json.
 * Returns 0 on success. */
int hyp_remove_gemini_hooks(const char *settings_path);

/* Install/remove a SessionStart reminder hook in Codex config.toml (#330) and
 * Gemini settings.json — same methodology as the Claude Code
 * SessionStart hook (non-blocking; stdout injected as session context). */
int hyp_upsert_codex_hooks(const char *config_path);
int hyp_remove_codex_hooks(const char *config_path);
int hyp_upsert_gemini_session_hooks(const char *settings_path);
int hyp_remove_gemini_session_hooks(const char *settings_path);

#ifdef HYP_JSON_LIKE_ENABLE_TEST_API
typedef void (*hyp_hook_json_prewrite_test_hook_t)(const char *settings_path, void *context);
void hyp_set_hook_json_prewrite_hook_for_testing(hyp_hook_json_prewrite_test_hook_t hook,
                                                 void *context);
#endif

/* Install/remove a Claude Code SubagentStart reminder hook in settings.json.
 * Subagents spawned via the Agent tool do not fire SessionStart, so this is the
 * channel that gives them the same code-discovery guidance. Non-blocking; the
 * hook injects context via JSON additionalContext. Returns 0 on success. */
int hyp_upsert_claude_subagent_hooks(const char *settings_path);
int hyp_remove_claude_subagent_hooks(const char *settings_path);

/* ── PATH management ──────────────────────────────────────────── */

/* Append an export PATH line to the given rc file.
 * Checks if already present. Returns 0 on success, 1 if already present. */
int hyp_ensure_path(const char *bin_dir, const char *rc_file, bool dry_run);

/* ── Codex instructions (legacy, wraps hyp_get_agent_instructions) ── */

/* Get the Codex CLI instructions content. */
const char *hyp_get_codex_instructions(void);

/* ── Tar.gz / zip extraction (TEST-ONLY) ──────────────────────────
 * Reachable only from the excluded in-process updater and tests/test_cli.c.
 * Declared and defined under the test guard so the capability is absent from
 * release artifacts rather than present-but-unreachable. */
#ifdef HYP_CLI_ENABLE_TEST_API

/* Extract a binary named "hyponoia*" from a tar.gz buffer.
 * Returns malloc'd binary content and sets *out_len.
 * Returns NULL on error. Caller must free. */
unsigned char *hyp_extract_binary_from_targz(const unsigned char *data, int data_len, int *out_len);

/* Extract the hyponoia binary from a zip archive in memory.
 * Returns malloc'd binary content and sets *out_len.
 * Returns NULL on error. Caller must free. */
unsigned char *hyp_extract_binary_from_zip(const unsigned char *data, int data_len, int *out_len);

#endif /* HYP_CLI_ENABLE_TEST_API */

/* ── Index management ─────────────────────────────────────────── */

/* List .db files in the cache directory (~/.cache/hyponoia/).
 * Prints each file path to stdout. Returns count of .db files found. */
int hyp_list_indexes(const char *home_dir);

/* Remove all .db files in the cache directory. Returns count removed. */
int hyp_remove_indexes(const char *home_dir);

/* ── Config store (persistent key-value, backed by _config.db) ── */

typedef struct hyp_config hyp_config_t;

/* Open the config store in the given cache directory.
 * Creates _config.db if it doesn't exist. Returns NULL on error. */
hyp_config_t *hyp_config_open(const char *cache_dir);

/* Close the config store. */
void hyp_config_close(hyp_config_t *cfg);

/* Get a config value. Returns default_val if key not found. Database-backed
 * results remain valid until the next call on the same thread. */
const char *hyp_config_get(hyp_config_t *cfg, const char *key, const char *default_val);

/* Get a config value as bool. "true"/"1"/"on" → true. */
bool hyp_config_get_bool(hyp_config_t *cfg, const char *key, bool default_val);

/* Get a config value as int. Returns default_val if not found or invalid. */
int hyp_config_get_int(hyp_config_t *cfg, const char *key, int default_val);

/* Set a config value. Returns 0 on success. */
int hyp_config_set(hyp_config_t *cfg, const char *key, const char *value);

/* Delete a config key. Returns 0 on success. */
int hyp_config_delete(hyp_config_t *cfg, const char *key);

/* Well-known config keys */
#define HYP_CONFIG_AUTO_INDEX "auto_index"
#define HYP_CONFIG_AUTO_INDEX_LIMIT "auto_index_limit"
#define HYP_CONFIG_AUTO_WATCH "auto_watch"
#define HYP_CONFIG_UI_LANG "ui-lang"

/* The `ask` model and its optional escalation lane (NEXT-STEPS.md §2.10). */
#define HYP_CONFIG_ASK_MODEL "ask.model"
#define HYP_CONFIG_ASK_ESC_PROVIDER "ask.escalation.provider"
#define HYP_CONFIG_ASK_ESC_MODEL "ask.escalation.model"
/* THE NAME OF AN ENVIRONMENT VARIABLE, NEVER A KEY. This config DB is backed
 * up and sits beside a graph.db.zst that gets shared with teammates, so a key
 * written here is a key handed out. hyp_config_validate refuses values that
 * look like secrets rather than trusting the documentation to be read. */
#define HYP_CONFIG_ASK_ESC_KEY_ENV "ask.escalation.key_env"
/* WHAT `ask(escalate=true)` SENDS TO THE PROVIDER (NEXT-STEPS.md §3.1 step 3):
 *
 *   query  — the QUESTION only. It is encoded by the hosted model and scored
 *            against the local index that already exists. No second index, no
 *            drift between two indexes, no corpus-sized bill, and the code
 *            never leaves the machine — only the ~30-token question does.
 *            Gated on a shared embedding SPACE (hyp_ask_space_id_for_model):
 *            voyage-4-only until another family is measured. THE DEFAULT.
 *   index  — the whole corpus, once, into a second API-built index that the
 *            escalated question is then encoded by the same model against
 *            (`hyponoia embed --escalation`). Provider-agnostic because both
 *            sides are one model; kept for anyone who wants a fully-API index.
 *
 * Query mode is licensed on QUALITY — nano documents + voyage-4-large queries
 * beat nano/nano by +0.2087 MRR@10 on the vocabulary-gap frozen 60 and by
 * +0.444% RR (p 5.5e-05) on 8,122 public CodeSearchNet-Go queries — and its
 * case OVER index mode is OPERATIONAL, not a quality margin: index mode
 * replicates at scale too and the two are indistinguishable there (p 0.62).
 * Anything but these two words is refused by hyp_config_validate. */
#define HYP_CONFIG_ASK_ESC_MODE "ask.escalation.mode"
#define HYP_CONFIG_ASK_ESC_MODE_QUERY "query"
#define HYP_CONFIG_ASK_ESC_MODE_INDEX "index"
#define HYP_CONFIG_ASK_ESC_MODE_DEFAULT HYP_CONFIG_ASK_ESC_MODE_QUERY

/* Check a key/value pair before it is stored. Returns 0 when the value is
 * acceptable, non-zero otherwise with a caller-facing sentence in `err`.
 * Unknown keys are allowed through unchanged — this validates the keys it
 * knows, it is not an allow-list of every key the product may ever have. */
int hyp_config_validate(const char *key, const char *value, char *err, size_t errlen);

/* ── Binary activation safety ─────────────────────────────────── */

typedef void *hyp_cli_activation_lock_t;
typedef int (*hyp_cli_activation_mutation_fn)(void *context);

/* High-level mutation reservation seam. Production maps this to the
 * crash-safe version-cohort maintenance barrier, which requests bounded
 * quiescence of active modern HYP processes before returning an exclusive
 * lease. Tests use it to prove that mutation never runs before the cohort has
 * drained. Return 1 with a non-NULL lease on success, 0 when participants did
 * not drain before the deadline, or -1 on an unsafe/IO failure. */
typedef int (*hyp_cli_activation_reserve_fn)(void *context, hyp_cli_activation_lock_t *lease_out);
typedef void (*hyp_cli_activation_release_fn)(void *context, hyp_cli_activation_lock_t lease);

/* Injectable high-level boundary for deterministic drain/mutation ordering
 * tests. Production never acquires startup while asking participants to
 * quiesce: a bootstrap participant may already retain it. Only after lifetime
 * EX proves the cohort drained does production acquire startup, re-probe the
 * daemon generation, and retain startup through mutation. Startup is released
 * before the maintenance lease. */
typedef struct {
    void *context;
    hyp_cli_activation_reserve_fn reserve_for_mutation;
    hyp_cli_activation_release_fn mutation_lease_release;
    void (*visible_diagnostic)(void *context, const char *message);
} hyp_cli_activation_ops_t;

/* Reserve and drain the coordinated cohort, run the mutation only while its
 * lease is retained, then release it on every callback result. A NULL mutation
 * is a guarded no-op. Returns 0 on success, 1 when activation is refused, or
 * the mutation callback's non-zero error. */
int hyp_cli_activation_guard_with_ops(const hyp_cli_activation_ops_t *ops,
                                      hyp_cli_activation_mutation_fn mutation,
                                      void *mutation_context);

/* Internal test seam used only by the C suite to route install/update/uninstall
 * through deterministic callbacks. NULL restores the production daemon IPC path. */
void hyp_cli_set_activation_ops_for_test(const hyp_cli_activation_ops_t *ops);

/* Internal integration-test seam: isolate the stable endpoint beneath a
 * private runtime parent. NULL restores the platform default. This is not a
 * command-line or environment override. */
void hyp_cli_set_activation_runtime_parent_for_test(const char *runtime_parent);

/* Internal test seam: stage from THIS file instead of the running executable.
 *
 * `install` copies /proc/self/exe, so the C suite was implicitly installing
 * the 600 MB test-runner out of the checkout — and the transaction refuses any
 * source whose directory chain is group- or world-writable, which is a normal
 * state for a workstation checkout (/media/DEV here is 0777). Seven install
 * tests failed for that reason alone and were written off as "environmental".
 * With this set, the test owns the source: a few hundred bytes in its own
 * 0700 mkdtemp directory, so the tests assert activation behaviour instead of
 * asserting where the repository happens to live. NULL restores the running
 * executable. It overrides only the staging source; the running binary's own
 * path still drives runtime-asset resolution. */
void hyp_cli_set_install_candidate_for_test(const char *candidate_path);

/* ── Subcommands (wired from main.c) ─────────────────────────── */

/* install: copy binary, install skills, install editor MCP configs, ensure PATH.
 * Prompts to delete old indexes if any exist — rejects on "no". */
int hyp_cmd_install(int argc, char **argv);

/* uninstall: remove skills, remove editor MCP configs, remove binary. */
int hyp_cmd_uninstall(int argc, char **argv);

/* Hidden package-wrapper health probe. Verifies only immutable sidecars
 * adjacent to the running executable; never consults fallbacks or mutates. */
int hyp_cmd_verify_runtime_assets(void);
bool hyp_cli_verify_runtime_assets_at(const char *binary_path, char *err, size_t err_sz);

/* update: check latest release, prompt for index deletion, prompt for ui/standard,
 * download and replace binary. */
int hyp_cmd_update(int argc, char **argv);

/* config: get/set/list/reset runtime config values. */
int hyp_cmd_config(int argc, char **argv);

/* hook-augment: stdin-driven Claude Code PreToolUse augmenter.
 * Reads the hook JSON from stdin and emits hookSpecificOutput.additionalContext
 * with search_graph hits for Grep/Glob calls. NEVER blocks: every failure
 * path returns 0 with no stdout output. */
int hyp_cmd_hook_augment(int argc, char **argv);

/* Build the documented hookSpecificOutput payload for a SessionStart or
 * SubagentStart input. Returns allocated JSON, or NULL for another/invalid
 * event. Exposed so lifecycle adapters have contract-level regression tests. */
char *hyp_hook_augment_lifecycle_json(const char *input);

/* Variant used by lifecycle adapters whose stdin omits the event. When
 * copilot_dialect is true, emits Copilot CLI's top-level additionalContext. */
char *hyp_hook_augment_lifecycle_json_for(const char *input, const char *forced_event,
                                          bool copilot_dialect);

/* Thin daemon frontend support: preserve the hook's bounded stdin read and
 * hard fail-open deadline without constructing a local MCP/store instance. */
void hyp_hook_augment_arm_deadline(void);
char *hyp_hook_augment_read_stdin(void);

/* Why a hook client is not augmenting. The hook caller only ever sees stdout,
 * so each reason that is actionable by the user must have a stdout notice
 * (#1388: a build-conflicted daemon used to report on stderr alone, which is
 * invisible in-session and reads as silent skips). */
typedef enum {
    HYP_HOOK_ADMISSION_DAEMON_ABSENT = 0, /* no daemon running: `daemon start` heals it */
    HYP_HOOK_ADMISSION_BUILD_CONFLICT     /* daemon runs another build: needs `daemon stop` */
} hyp_hook_admission_t;

/* The JSON systemMessage a hook client must print on stdout for `reason`, or
 * NULL when nothing should be printed. hook_dialect NULL = Claude Code, the
 * only dialect that surfaces a stdout systemMessage to the user. */
const char *hyp_hook_admission_notice(hyp_hook_admission_t reason, const char *hook_dialect);

/* Process one already-read hook payload using a caller-owned MCP session.
 * Returns a malloc-owned hook output JSON string, or NULL for fail-open/no
 * augmentation. This is the daemon entry; it never arms a process-global
 * timer and never constructs a second store/session. */
char *hyp_hook_augment_process(hyp_mcp_server_t *srv, const char *input_json);

/* Dialect-aware daemon entry. forced_event and dialect_name are borrowed and
 * may be NULL for the ordinary event dialect. Unsupported combinations fail
 * open with NULL, matching the direct hook command. */
bool hyp_hook_augment_invocation_supported(const char *forced_event, const char *dialect_name);
char *hyp_hook_augment_process_for(hyp_mcp_server_t *srv, const char *input_json,
                                   const char *forced_event, const char *dialect_name);

/* True for an absolute path the augmenter can walk up: POSIX "/..." or a
 * Windows drive root — "X:/..." or a bare "X:" (callers normalize '\\' to '/'
 * first). Exposed for tests — regression coverage for the Windows drive-letter
 * no-op (#618). */
bool hyp_hook_path_is_abs(const char *path);

/* Build the agent.install.plan.v1 install receipt for <home> (issue #388):
 * a machine-readable JSON list of config/instruction/skill/agent/hook files `install`
 * would write, produced WITHOUT mutating anything. Returns a heap JSON string
 * (caller frees) or NULL on error. Exposed for `install --plan` and testing. */
char *hyp_build_install_plan_json(const char *home, const char *binary_path);

#endif /* HYP_CLI_H */
