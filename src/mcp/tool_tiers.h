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
 * renderable byte-for-byte, and a generation is a change to ANY rendered
 * byte of a profile — a row here or the tier prompt text in
 * agent_profiles.c. Row ORDER is part of the contract (generated profiles
 * list tools in row order). Add a tool by appending a row tagged with the
 * new generation and bumping HYP_PROFILE_GENERATION; change prompt text by
 * gating the new words on a new generation the same way; never reorder or
 * rename an existing row, never edit an older generation's words in place.
 */
#ifndef HYP_MCP_TOOL_TIERS_H
#define HYP_MCP_TOOL_TIERS_H

/* 0  the eleven-tool set shipped with the tiered profiles
 * 1  `ask` on analysis (tools list only)
 * 2  the analysis prompt teaches ask and the project-name rule. Measured
 *    (runs/ASK-REACHABLE, sonnet-5, frozen 60 on lld/ELF, 60 runs a
 *    column): with ask merely listed the agent called it in 4/60 runs and
 *    tokens per correct answer moved -2.9% (a null); with one sentence in
 *    the body it called ask 60/60 and tokens per correct answer fell 25.5%
 *    (CI [-1,547, -657]), best accuracy of the columns, 0 capped runs. A
 *    tool in the list is not reachable until the body says when to use it.
 * 3  the project-name rule is deleted from the prompt because the SERVER now
 *    derives it (mcp.c, resolve_project_arg). Generation 2 taught the agent
 *    to COMPUTE the name and pass it, and it did not work: 120 of 120 runs
 *    still opened with list_projects. Prose describing a derivation the
 *    server can perform is dead weight paid on every run, and after the
 *    server performs it the prose is also wrong — it tells the agent to send
 *    an argument it should omit. The replacement says the opposite: leave
 *    `project` out, and read project/project_source in the answer. */
#define HYP_PROFILE_GENERATION 3U
#define HYP_PROFILE_GENERATION_ASK_TOOL 1U
#define HYP_PROFILE_GENERATION_ASK_GUIDANCE 2U
#define HYP_PROFILE_GENERATION_PROJECT_DEFAULT 3U

/* X(name, analysis, scout, generation) */
#define HYP_TOOL_TIERS(X)                           \
    X("search_graph", 1, 1, 0U)                     \
    X("ask", 1, 0, HYP_PROFILE_GENERATION_ASK_TOOL) \
    X("trace_path", 1, 1, 0U)                       \
    X("get_code_snippet", 1, 1, 0U)                 \
    X("query_graph", 1, 0, 0U)                      \
    X("get_architecture", 1, 1, 0U)                 \
    X("search_code", 1, 0, 0U)                      \
    X("get_graph_schema", 1, 0, 0U)                 \
    X("list_projects", 1, 1, 0U)                    \
    X("index_status", 1, 1, 0U)                     \
    X("detect_changes", 1, 0, 0U)                   \
    X("check_index_coverage", 1, 1, 0U)

#endif /* HYP_MCP_TOOL_TIERS_H */
