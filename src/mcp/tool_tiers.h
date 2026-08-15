/*
 * tool_tiers.h — The one table of which tools the restricted MCP profiles
 * expose.
 *
 * A restricted profile has two ends. The server end (mcp.c) decides what
 * `--tool-profile analysis|scout` advertises in tools/list and accepts in
 * tools/call. The client end (agent_profiles.c) renders the agent definitions
 * every install writes — the `hyponoia`, `hyponoia-auditor` and
 * `hyponoia-scout` files a coding agent actually loads — and each of those
 * requests its tools by name. Until this header existed each end kept its own
 * list, each list had its own tests, and both passed while `ask` was
 * permitted by the server and never requested by a profile: a shipped lane no
 * generated agent could call. Both ends now expand this macro, so a tool is
 * either on a tier at both ends or on it at neither.
 *
 * Columns: wire name, on the analysis tier, on the scout tier, and the
 * profile generation that introduced the row.
 *
 * `ask` joins analysis but NOT scout. Scout's promise is a small surface
 * whose every tool answers; a lane that can legitimately report unavailable
 * (the semantic index is opt-in) belongs on the surface where an agent is
 * already expected to reason about index state.
 *
 * Generations exist for the installer, which only replaces an agent file it
 * can prove it wrote: the bytes must equal the current rendering or one it
 * previously released. Every earlier generation therefore has to stay
 * renderable byte-for-byte, which is why row ORDER is part of the contract —
 * generated profiles list tools in row order. Add a tool by appending a row
 * with generation HYP_TOOL_TIER_GENERATION + 1 and bumping that constant;
 * never reorder or rename an existing row.
 */
#ifndef HYP_MCP_TOOL_TIERS_H
#define HYP_MCP_TOOL_TIERS_H

/* Generation 0 was the eleven-tool set shipped with the tiered profiles;
 * generation 1 added `ask` to analysis. */
#define HYP_TOOL_TIER_GENERATION 1U

/* X(name, analysis, scout, generation) */
#define HYP_TOOL_TIERS(X)           \
    X("search_graph", 1, 1, 0U)     \
    X("ask", 1, 0, 1U)              \
    X("trace_path", 1, 1, 0U)       \
    X("get_code_snippet", 1, 1, 0U) \
    X("query_graph", 1, 0, 0U)      \
    X("get_architecture", 1, 1, 0U) \
    X("search_code", 1, 0, 0U)      \
    X("get_graph_schema", 1, 0, 0U) \
    X("list_projects", 1, 1, 0U)    \
    X("index_status", 1, 1, 0U)     \
    X("detect_changes", 1, 0, 0U)   \
    X("check_index_coverage", 1, 1, 0U)

#endif /* HYP_MCP_TOOL_TIERS_H */
