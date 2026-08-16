/*
 * agent_profiles.h — Canonical tiered hyponoia agent profiles.
 */
#ifndef HYP_CLI_AGENT_PROFILES_H
#define HYP_CLI_AGENT_PROFILES_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HYP_GRAPH_TIER_SCOUT = 0,
    HYP_GRAPH_TIER_VERIFY,
    HYP_GRAPH_TIER_AUDIT,
    HYP_GRAPH_TIER_COUNT
} hyp_graph_tier_t;

typedef enum {
    HYP_GRAPH_ACCESS_DIRECT = 0,
    HYP_GRAPH_ACCESS_HANDOFF,
    HYP_GRAPH_ACCESS_COUNT
} hyp_graph_access_t;

typedef enum {
    HYP_GRAPH_DIALECT_CLAUDE = 0,
    HYP_GRAPH_DIALECT_CODEX,
    HYP_GRAPH_DIALECT_GEMINI,
    HYP_GRAPH_DIALECT_QWEN,
    HYP_GRAPH_DIALECT_COPILOT,
    HYP_GRAPH_DIALECT_OPENCODE,
    HYP_GRAPH_DIALECT_KILO,
    HYP_GRAPH_DIALECT_KIRO,
    HYP_GRAPH_DIALECT_JUNIE,
    HYP_GRAPH_DIALECT_QODER,
    HYP_GRAPH_DIALECT_CODEBUDDY,
    HYP_GRAPH_DIALECT_FACTORY,
    HYP_GRAPH_DIALECT_VIBE,
    HYP_GRAPH_DIALECT_AUGMENT,
    HYP_GRAPH_DIALECT_CURSOR,
    HYP_GRAPH_DIALECT_ROVO,
    HYP_GRAPH_DIALECT_POCHI,
    HYP_GRAPH_DIALECT_COUNT
} hyp_graph_profile_dialect_t;

/* Stable profile identifier. VERIFY intentionally retains "hyponoia". */
const char *hyp_graph_tier_slug(hyp_graph_tier_t tier);
const char *hyp_graph_tier_display_name(hyp_graph_tier_t tier);
bool hyp_graph_dialect_direct_capable(hyp_graph_profile_dialect_t dialect);

/* Returns malloc-owned profile content, or NULL for invalid/unsafe combinations.
 * binary_path is required for direct Kiro and Codex profiles and ignored otherwise. */
char *hyp_render_graph_profile(hyp_graph_profile_dialect_t dialect, hyp_graph_tier_t tier,
                               hyp_graph_access_t access, const char *binary_path);

/* The tool set a direct tier requests comes from mcp/tool_surface.h, the table
 * the MCP server enforces. Every change to a rendered profile — that set or
 * the tier prompt text — is a new profile generation, and the installer only
 * replaces an agent file whose bytes equal the current rendering or one it
 * previously released — so each earlier generation must still render exactly.
 * hyp_render_graph_profile() / hyp_render_graph_prompt() render the current
 * generation; these render any generation up to it, NULL past it. */
unsigned hyp_graph_profile_generation(void);
char *hyp_render_graph_profile_generation(hyp_graph_profile_dialect_t dialect,
                                          hyp_graph_tier_t tier, hyp_graph_access_t access,
                                          const char *binary_path, unsigned generation);
char *hyp_render_graph_prompt_generation(hyp_graph_tier_t tier, hyp_graph_access_t access,
                                         unsigned generation);

/* The wire name (no dialect prefix) of the index-th tool a direct profile of
 * `tier` requests at the current generation, in render order; NULL past the
 * end. Lets a test hold the profile end and the server end to one table. */
const char *hyp_graph_tier_tool_name(hyp_graph_tier_t tier, size_t index);

/* v0.9.1-rc.1 direct Codex rendering (server table without a transport), kept
 * so install/uninstall can recognize and migrate those files. */
char *hyp_render_graph_profile_codex_rc1(hyp_graph_tier_t tier);

/* Vibe stores the behavioral prompt separately from its TOML agent definition.
 * Other integrations may also use this as the canonical contract text. */
char *hyp_render_graph_prompt(hyp_graph_tier_t tier, hyp_graph_access_t access);

#ifdef __cplusplus
}
#endif

#endif /* HYP_CLI_AGENT_PROFILES_H */
