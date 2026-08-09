/*
 * agent_profiles.h — Canonical tiered hyponoia agent profiles.
 */
#ifndef HYP_CLI_AGENT_PROFILES_H
#define HYP_CLI_AGENT_PROFILES_H

#include <stdbool.h>

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
