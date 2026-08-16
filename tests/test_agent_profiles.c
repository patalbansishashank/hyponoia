/* test_agent_profiles.c — Canonical Scout/Verify/Audit renderer contracts. */
#include "test_framework.h"

#include <cli/agent_profiles.h>
#include <mcp/mcp.h>
#include <yyjson/yyjson.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    hyp_graph_profile_dialect_t dialect;
    const char *syntax_fragment;
    const char *read_fragment;
    const char *grep_fragment;
} direct_dialect_expectation_t;

static const direct_dialect_expectation_t direct_dialects[] = {
    {HYP_GRAPH_DIALECT_CLAUDE, "permissionMode: plan", "  - Read\n", "  - Grep\n"},
    {HYP_GRAPH_DIALECT_CODEX, "sandbox_mode = \"read-only\"", "read", "grep"},
    {HYP_GRAPH_DIALECT_GEMINI, "kind: local", "  - read_file\n", "  - grep_search\n"},
    {HYP_GRAPH_DIALECT_QWEN, "approvalMode: plan", "  - read_file\n", "  - grep_search\n"},
    {HYP_GRAPH_DIALECT_COPILOT, "hyponoia/check_index_coverage", "  - read\n",
     "source read/grep fallback"},
    {HYP_GRAPH_DIALECT_OPENCODE, "  \"*\": deny", "  read: allow", "  grep: allow"},
    {HYP_GRAPH_DIALECT_KILO, "mode: subagent", "  read: allow", "  grep: allow"},
    {HYP_GRAPH_DIALECT_KIRO, "\"includeMcpJson\": false", "\"read\"", "\"grep\""},
    {HYP_GRAPH_DIALECT_JUNIE, "mcpServers: [\"hyponoia-", "\"Read\"", "\"Grep\""},
    {HYP_GRAPH_DIALECT_QODER, "mcp__hyponoia__check_index_coverage",
     "tools: Read,Grep,Glob,mcp__hyponoia__", "Read,Grep"},
    {HYP_GRAPH_DIALECT_CODEBUDDY, "permissionMode: plan", "tools: Read,Grep,Glob,", "Read,Grep"},
    {HYP_GRAPH_DIALECT_FACTORY, "mcp__hyponoia__check_index_coverage",
     "tools: [\"Read\", \"LS\", \"Grep\", \"Glob\"", "source read/grep fallback"},
    {HYP_GRAPH_DIALECT_VIBE, "agent_type = \"subagent\"", "\"read_file\"", "\"grep_search\""},
};

static const hyp_graph_profile_dialect_t handoff_only_dialects[] = {
    HYP_GRAPH_DIALECT_AUGMENT,
    HYP_GRAPH_DIALECT_CURSOR,
    HYP_GRAPH_DIALECT_ROVO,
    HYP_GRAPH_DIALECT_POCHI,
};

static int profile_has_mutator(const char *profile) {
    static const char *const mutators[] = {
        "index_repository",
        "delete_project",
        "manage_adr",
        "ingest_traces",
        /* The memory WRITER. Every tier says read-only in its own description,
         * so it may never be requested by one; the reader beside it may. */
        "record_memory",
    };
    for (size_t i = 0U; i < sizeof(mutators) / sizeof(mutators[0]); i++) {
        if (strstr(profile, mutators[i])) {
            return 1;
        }
    }
    return 0;
}

TEST(agent_profiles_stable_tier_identity) {
    ASSERT_STR_EQ(hyp_graph_tier_slug(HYP_GRAPH_TIER_SCOUT), "hyponoia-scout");
    ASSERT_STR_EQ(hyp_graph_tier_slug(HYP_GRAPH_TIER_VERIFY), "hyponoia");
    ASSERT_STR_EQ(hyp_graph_tier_slug(HYP_GRAPH_TIER_AUDIT), "hyponoia-auditor");
    ASSERT_STR_EQ(hyp_graph_tier_display_name(HYP_GRAPH_TIER_SCOUT), "Hyponoia Scout");
    ASSERT_STR_EQ(hyp_graph_tier_display_name(HYP_GRAPH_TIER_VERIFY), "Hyponoia Verify");
    ASSERT_STR_EQ(hyp_graph_tier_display_name(HYP_GRAPH_TIER_AUDIT), "Hyponoia Auditor");
    ASSERT_TRUE(hyp_graph_dialect_direct_capable(HYP_GRAPH_DIALECT_CLAUDE));
    ASSERT_TRUE(hyp_graph_dialect_direct_capable(HYP_GRAPH_DIALECT_KIRO));
    ASSERT_FALSE(hyp_graph_dialect_direct_capable(HYP_GRAPH_DIALECT_AUGMENT));
    ASSERT_FALSE(hyp_graph_dialect_direct_capable(HYP_GRAPH_DIALECT_CURSOR));
    ASSERT_FALSE(hyp_graph_dialect_direct_capable(HYP_GRAPH_DIALECT_ROVO));
    ASSERT_FALSE(hyp_graph_dialect_direct_capable(HYP_GRAPH_DIALECT_POCHI));
    ASSERT_FALSE(hyp_graph_dialect_direct_capable(HYP_GRAPH_DIALECT_COUNT));
    ASSERT_NULL(hyp_graph_tier_slug(HYP_GRAPH_TIER_COUNT));
    ASSERT_NULL(hyp_graph_tier_display_name(HYP_GRAPH_TIER_COUNT));
    PASS();
}

TEST(agent_profiles_direct_dialects_are_coverage_aware_and_read_only) {
    for (size_t i = 0U; i < sizeof(direct_dialects) / sizeof(direct_dialects[0]); i++) {
        const direct_dialect_expectation_t *expectation = &direct_dialects[i];
        for (int tier = 0; tier < (int)HYP_GRAPH_TIER_COUNT; tier++) {
            const char *binary = expectation->dialect == HYP_GRAPH_DIALECT_KIRO ||
                                         expectation->dialect == HYP_GRAPH_DIALECT_CODEX
                                     ? "/opt/hyponoia/hyp"
                                     : NULL;
            char *profile = hyp_render_graph_profile(expectation->dialect, (hyp_graph_tier_t)tier,
                                                     HYP_GRAPH_ACCESS_DIRECT, binary);
            if (!profile) {
                FAIL("every documented direct dialect must render all three tiers");
            }
            int valid = strstr(profile, "hyponoia") != NULL &&
                        strstr(profile, "check_index_coverage") != NULL &&
                        strstr(profile, expectation->syntax_fragment) != NULL &&
                        strstr(profile, expectation->read_fragment) != NULL &&
                        strstr(profile, expectation->grep_fragment) != NULL &&
                        strstr(profile, "source read/grep fallback") != NULL &&
                        !profile_has_mutator(profile);
            free(profile);
            if (!valid) {
                FAIL("direct profiles must expose coverage plus source fallback and omit mutators");
            }
        }
    }
    PASS();
}

TEST(agent_profiles_tiers_encode_distinct_evidence_budgets) {
    char *scout = hyp_render_graph_profile(HYP_GRAPH_DIALECT_CLAUDE, HYP_GRAPH_TIER_SCOUT,
                                           HYP_GRAPH_ACCESS_DIRECT, NULL);
    char *verify = hyp_render_graph_profile(HYP_GRAPH_DIALECT_CLAUDE, HYP_GRAPH_TIER_VERIFY,
                                            HYP_GRAPH_ACCESS_DIRECT, NULL);
    char *audit = hyp_render_graph_profile(HYP_GRAPH_DIALECT_CLAUDE, HYP_GRAPH_TIER_AUDIT,
                                           HYP_GRAPH_ACCESS_DIRECT, NULL);
    int valid = scout && verify && audit && strstr(scout, "3-4 narrow graph calls") &&
                strstr(scout, "positive, provisional") && strstr(scout, "all/none claims") &&
                !strstr(scout, "mcp__hyponoia__query_graph") &&
                !strstr(scout, "mcp__hyponoia__detect_changes") &&
                !strstr(scout, "mcp__hyponoia__ask") &&
                strstr(verify, "default tier") && strstr(verify, "task-directed evidence") &&
                strstr(verify, "scope coverage before negative claims") &&
                strstr(verify, "mcp__hyponoia__query_graph") &&
                strstr(verify, "mcp__hyponoia__detect_changes") &&
                strstr(verify, "  - mcp__hyponoia__ask\n") &&
                strstr(audit, "bounded scope") && strstr(audit, "current graph generation") &&
                strstr(audit, "complete relevant pagination") && strstr(audit, "scope coverage") &&
                strstr(audit, "source fallback") &&
                strstr(audit, "mcp__hyponoia__query_graph") &&
                strstr(audit, "mcp__hyponoia__detect_changes") &&
                strstr(audit, "  - mcp__hyponoia__ask\n");
    free(scout);
    free(verify);
    free(audit);
    ASSERT_TRUE(valid);
    PASS();
}

TEST(agent_profiles_handoff_requires_parent_evidence_without_child_mcp) {
    for (int dialect = 0; dialect < (int)HYP_GRAPH_DIALECT_COUNT; dialect++) {
        for (int tier = 0; tier < (int)HYP_GRAPH_TIER_COUNT; tier++) {
            char *profile =
                hyp_render_graph_profile((hyp_graph_profile_dialect_t)dialect,
                                         (hyp_graph_tier_t)tier, HYP_GRAPH_ACCESS_HANDOFF, NULL);
            if (!profile) {
                FAIL("every dialect must be able to render a parent-handoff profile");
            }
            int valid = strstr(profile, "parent agent must supply") &&
                        strstr(profile, "coverage evidence") &&
                        strstr(profile, "must not call or claim access to MCP") &&
                        !strstr(profile, "mcpServers") &&
                        !strstr(profile, "mcp__hyponoia__") &&
                        !strstr(profile, "mcp_hyponoia_") &&
                        !strstr(profile, "@hyponoia/") &&
                        !strstr(profile, "hyponoia/");
            free(profile);
            if (!valid) {
                FAIL("handoff profiles must require parent coverage and expose no child MCP");
            }
        }
    }
    PASS();
}

TEST(agent_profiles_handoff_only_dialects_fail_closed_for_direct_access) {
    for (size_t i = 0U; i < sizeof(handoff_only_dialects) / sizeof(handoff_only_dialects[0]); i++) {
        char *profile = hyp_render_graph_profile(handoff_only_dialects[i], HYP_GRAPH_TIER_VERIFY,
                                                 HYP_GRAPH_ACCESS_DIRECT, "/opt/hyp");
        ASSERT_NULL(profile);
    }
    PASS();
}

TEST(agent_profiles_server_level_dialects_hard_enforce_read_only_tools) {
    char *junie_scout = hyp_render_graph_profile(HYP_GRAPH_DIALECT_JUNIE, HYP_GRAPH_TIER_SCOUT,
                                                 HYP_GRAPH_ACCESS_DIRECT, NULL);
    char *junie = hyp_render_graph_profile(HYP_GRAPH_DIALECT_JUNIE, HYP_GRAPH_TIER_VERIFY,
                                           HYP_GRAPH_ACCESS_DIRECT, NULL);
    char *qoder = hyp_render_graph_profile(HYP_GRAPH_DIALECT_QODER, HYP_GRAPH_TIER_VERIFY,
                                           HYP_GRAPH_ACCESS_DIRECT, NULL);
    char *factory = hyp_render_graph_profile(HYP_GRAPH_DIALECT_FACTORY, HYP_GRAPH_TIER_VERIFY,
                                             HYP_GRAPH_ACCESS_DIRECT, NULL);
    ASSERT_NOT_NULL(junie_scout);
    ASSERT_NOT_NULL(junie);
    ASSERT_NOT_NULL(qoder);
    ASSERT_NOT_NULL(factory);
    ASSERT(strstr(junie_scout, "mcpServers: [\"hyponoia-scout\"]") != NULL);
    ASSERT(strstr(junie, "mcpServers: [\"hyponoia-analysis\"]") != NULL);
    ASSERT(strstr(junie, "hard-enforces the analysis tool profile") != NULL);
    ASSERT(strstr(qoder, "mcp__hyponoia__check_index_coverage") != NULL);
    ASSERT(strstr(factory, "mcp__hyponoia__check_index_coverage") != NULL);
    ASSERT(strstr(qoder, "mcpServers:") != NULL);
    ASSERT(strstr(qoder, "hyponoia") != NULL);
    ASSERT(strstr(factory, "mcpServers") == NULL);
    ASSERT(strstr(junie, "instruction-enforced") == NULL);
    ASSERT(strstr(qoder, "instruction-enforced") == NULL);
    ASSERT(strstr(factory, "instruction-enforced") == NULL);
    ASSERT(!profile_has_mutator(junie));
    ASSERT(!profile_has_mutator(qoder));
    ASSERT(!profile_has_mutator(factory));
    free(junie_scout);
    free(junie);
    free(qoder);
    free(factory);
    PASS();
}

TEST(agent_profiles_kiro_is_valid_json_and_escapes_binary_path) {
    const char *binary = "/opt/hyp path/\"quoted\"";
    char *profile = hyp_render_graph_profile(HYP_GRAPH_DIALECT_KIRO, HYP_GRAPH_TIER_AUDIT,
                                             HYP_GRAPH_ACCESS_DIRECT, binary);
    ASSERT_NOT_NULL(profile);
    yyjson_doc *doc = yyjson_read(profile, strlen(profile), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *servers = root ? yyjson_obj_get(root, "mcpServers") : NULL;
    yyjson_val *server = servers ? yyjson_obj_get(servers, "hyponoia") : NULL;
    yyjson_val *command = server ? yyjson_obj_get(server, "command") : NULL;
    yyjson_val *args = server ? yyjson_obj_get(server, "args") : NULL;
    yyjson_val *profile_flag = args && yyjson_is_arr(args) ? yyjson_arr_get(args, 0U) : NULL;
    yyjson_val *profile_name = args && yyjson_is_arr(args) ? yyjson_arr_get(args, 1U) : NULL;
    yyjson_val *tools = root ? yyjson_obj_get(root, "tools") : NULL;
    int valid = root && yyjson_is_obj(root) && command && yyjson_is_str(command) &&
                strcmp(yyjson_get_str(command), binary) == 0 && args && yyjson_is_arr(args) &&
                yyjson_arr_size(args) == 2U && profile_flag && yyjson_is_str(profile_flag) &&
                strcmp(yyjson_get_str(profile_flag), "--tool-profile") == 0 && profile_name &&
                yyjson_is_str(profile_name) &&
                strcmp(yyjson_get_str(profile_name), "analysis") == 0 && tools &&
                yyjson_is_arr(tools) &&
                strstr(profile, "@hyponoia/check_index_coverage") != NULL;
    yyjson_doc_free(doc);
    free(profile);
    ASSERT_TRUE(valid);
    PASS();
}

TEST(agent_profiles_codex_declares_transport_and_escapes_binary_path) {
    const char *binary = "C:\\hyp bin\\hyponoia.exe";
    char *scout = hyp_render_graph_profile(HYP_GRAPH_DIALECT_CODEX, HYP_GRAPH_TIER_SCOUT,
                                           HYP_GRAPH_ACCESS_DIRECT, binary);
    char *verify = hyp_render_graph_profile(HYP_GRAPH_DIALECT_CODEX, HYP_GRAPH_TIER_VERIFY,
                                            HYP_GRAPH_ACCESS_DIRECT, binary);
    ASSERT_NOT_NULL(scout);
    ASSERT_NOT_NULL(verify);
    int valid = strstr(scout, "[mcp_servers.hyponoia]\n"
                              "command = \"C:\\\\hyp bin\\\\hyponoia.exe\"\n"
                              "args = [\"--tool-profile=scout\"]\n"
                              "enabled_tools = [") != NULL &&
                strstr(verify, "command = \"C:\\\\hyp bin\\\\hyponoia.exe\"\n"
                               "args = [\"--tool-profile=analysis\"]\n") != NULL;
    free(scout);
    free(verify);
    ASSERT_TRUE(valid);
    ASSERT_NULL(hyp_render_graph_profile(HYP_GRAPH_DIALECT_CODEX, HYP_GRAPH_TIER_VERIFY,
                                         HYP_GRAPH_ACCESS_DIRECT, NULL));
    ASSERT_NULL(hyp_render_graph_profile(HYP_GRAPH_DIALECT_CODEX, HYP_GRAPH_TIER_VERIFY,
                                         HYP_GRAPH_ACCESS_DIRECT, ""));
    char *handoff = hyp_render_graph_profile(HYP_GRAPH_DIALECT_CODEX, HYP_GRAPH_TIER_VERIFY,
                                             HYP_GRAPH_ACCESS_HANDOFF, NULL);
    ASSERT_NOT_NULL(handoff);
    ASSERT_TRUE(strstr(handoff, "[mcp_servers.") == NULL);
    free(handoff);
    char *rc1 = hyp_render_graph_profile_codex_rc1(HYP_GRAPH_TIER_VERIFY);
    ASSERT_NOT_NULL(rc1);
    ASSERT_TRUE(strstr(rc1, "[mcp_servers.hyponoia]\nenabled_tools = [") != NULL);
    ASSERT_TRUE(strstr(rc1, "command = ") == NULL);
    free(rc1);
    PASS();
}

TEST(agent_profiles_vibe_uses_matching_prompt_identifier_and_contract) {
    for (int tier = 0; tier < (int)HYP_GRAPH_TIER_COUNT; tier++) {
        const char *slug = hyp_graph_tier_slug((hyp_graph_tier_t)tier);
        char *profile = hyp_render_graph_profile(HYP_GRAPH_DIALECT_VIBE, (hyp_graph_tier_t)tier,
                                                 HYP_GRAPH_ACCESS_DIRECT, NULL);
        char *prompt = hyp_render_graph_prompt((hyp_graph_tier_t)tier, HYP_GRAPH_ACCESS_DIRECT);
        int valid = profile && prompt && strstr(profile, slug) &&
                    strstr(profile, "system_prompt_id") && strstr(prompt, "check_index_coverage") &&
                    strstr(prompt, "source read/grep fallback");
        free(profile);
        free(prompt);
        if (!valid) {
            FAIL("Vibe profile and canonical prompt must share the tier slug and contract");
        }
    }
    PASS();
}

TEST(agent_profiles_render_deterministically_and_reject_invalid_inputs) {
    char *first = hyp_render_graph_profile(HYP_GRAPH_DIALECT_QWEN, HYP_GRAPH_TIER_VERIFY,
                                           HYP_GRAPH_ACCESS_DIRECT, NULL);
    char *second = hyp_render_graph_profile(HYP_GRAPH_DIALECT_QWEN, HYP_GRAPH_TIER_VERIFY,
                                            HYP_GRAPH_ACCESS_DIRECT, NULL);
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT_STR_EQ(first, second);
    free(first);
    free(second);
    ASSERT_NULL(hyp_render_graph_profile(HYP_GRAPH_DIALECT_COUNT, HYP_GRAPH_TIER_VERIFY,
                                         HYP_GRAPH_ACCESS_DIRECT, NULL));
    ASSERT_NULL(hyp_render_graph_profile(HYP_GRAPH_DIALECT_CLAUDE, HYP_GRAPH_TIER_COUNT,
                                         HYP_GRAPH_ACCESS_DIRECT, NULL));
    ASSERT_NULL(hyp_render_graph_profile(HYP_GRAPH_DIALECT_CLAUDE, HYP_GRAPH_TIER_VERIFY,
                                         HYP_GRAPH_ACCESS_COUNT, NULL));
    ASSERT_NULL(hyp_render_graph_profile(HYP_GRAPH_DIALECT_KIRO, HYP_GRAPH_TIER_VERIFY,
                                         HYP_GRAPH_ACCESS_DIRECT, NULL));
    ASSERT_NULL(hyp_render_graph_prompt(HYP_GRAPH_TIER_COUNT, HYP_GRAPH_ACCESS_DIRECT));
    ASSERT_NULL(hyp_render_graph_prompt(HYP_GRAPH_TIER_VERIFY, HYP_GRAPH_ACCESS_COUNT));
    PASS();
}

/* The two ends of a restricted profile: what the MCP server advertises under
 * `--tool-profile`, and what the generated agent definition requests. Both
 * expand mcp/tool_surface.h; this holds them to it FROM THE CLIENT'S SIDE — the
 * server's tools/list response and the rendered profile file — rather than by
 * reading either end's static list, because `ask` was permitted by one and
 * never requested by the other while both ends' own tests passed. */
static hyp_mcp_tool_profile_t server_profile_for_tier(hyp_graph_tier_t tier) {
    return tier == HYP_GRAPH_TIER_SCOUT ? HYP_MCP_TOOL_PROFILE_SCOUT
                                        : HYP_MCP_TOOL_PROFILE_ANALYSIS;
}

/* Names in a tools/list response, joined as "\nname\n" runs so a substring
 * search cannot match a prefix. Returns malloc-owned text or NULL. */
static char *tools_list_names(hyp_mcp_tool_profile_t profile, size_t *count_out) {
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    if (!srv) {
        return NULL;
    }
    hyp_mcp_server_set_tool_profile(srv, profile);
    char *resp =
        hyp_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}");
    hyp_mcp_server_free(srv);
    if (!resp) {
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *result = root ? yyjson_obj_get(root, "result") : NULL;
    yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
    char *names = (char *)calloc(1U, 4096U);
    size_t used = 0U;
    size_t count = 0U;
    if (names && tools && yyjson_is_arr(tools)) {
        names[used++] = '\n';
        size_t index = 0U;
        size_t max = 0U;
        yyjson_val *tool = NULL;
        yyjson_arr_foreach(tools, index, max, tool) {
            yyjson_val *name = yyjson_obj_get(tool, "name");
            const char *text = name && yyjson_is_str(name) ? yyjson_get_str(name) : NULL;
            if (!text || used + strlen(text) + 2U >= 4096U) {
                free(names);
                names = NULL;
                break;
            }
            used += (size_t)snprintf(names + used, 4096U - used, "%s\n", text);
            count++;
        }
    }
    yyjson_doc_free(doc);
    free(resp);
    if (count_out) {
        *count_out = count;
    }
    return names;
}

TEST(agent_profiles_tiers_agree_with_the_server_profile_they_run_against) {
    for (int value = 0; value < (int)HYP_GRAPH_TIER_COUNT; value++) {
        hyp_graph_tier_t tier = (hyp_graph_tier_t)value;
        hyp_mcp_tool_profile_t profile = server_profile_for_tier(tier);

        /* Table end: every tool the profile requests, the server allows; every
         * registered tool the server allows, the profile requests. */
        size_t requested = 0U;
        for (const char *name; (name = hyp_graph_tier_tool_name(tier, requested)) != NULL;
             requested++) {
            if (!hyp_mcp_tool_profile_allows(profile, name)) {
                FAIL("a generated profile requests a tool its server profile refuses");
            }
        }
        size_t allowed = 0U;
        for (int i = 0; i < hyp_mcp_tool_count(); i++) {
            const char *name = hyp_mcp_tool_name(i);
            if (!hyp_mcp_tool_profile_allows(profile, name)) {
                continue;
            }
            allowed++;
            bool found = false;
            for (size_t j = 0U; !found && hyp_graph_tier_tool_name(tier, j) != NULL; j++) {
                found = strcmp(hyp_graph_tier_tool_name(tier, j), name) == 0;
            }
            if (!found) {
                FAIL("the server profile offers a tool the generated profile never requests");
            }
        }
        ASSERT_EQ(requested, allowed);

        /* Client end: the names in tools/list against the mcp__hyponoia__ lines
         * a Claude Code agent file asks for. */
        size_t listed = 0U;
        char *names = tools_list_names(profile, &listed);
        char *rendered =
            hyp_render_graph_profile(HYP_GRAPH_DIALECT_CLAUDE, tier, HYP_GRAPH_ACCESS_DIRECT, NULL);
        ASSERT_NOT_NULL(names);
        ASSERT_NOT_NULL(rendered);
        ASSERT_EQ(listed, requested);
        size_t seen = 0U;
        for (const char *cursor = rendered; (cursor = strstr(cursor, "  - mcp__hyponoia__"));) {
            cursor += strlen("  - mcp__hyponoia__");
            const char *end = strchr(cursor, '\n');
            char needle[128];
            size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
            if (len + 3U > sizeof(needle)) {
                FAIL("tool identifier longer than any registered tool");
            }
            snprintf(needle, sizeof(needle), "\n%.*s\n", (int)len, cursor);
            if (!strstr(names, needle)) {
                free(names);
                free(rendered);
                FAIL("the agent file requests a tool tools/list does not advertise");
            }
            seen++;
        }
        free(names);
        free(rendered);
        ASSERT_EQ(seen, listed);
    }

    /* And the specific tool this table exists for: analysis, never scout. */
    ASSERT_TRUE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ANALYSIS, "ask"));
    ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_SCOUT, "ask"));
    ASSERT_TRUE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ALL, "ask"));
    ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ANALYSIS, "index_repository"));
    ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_SCOUT, NULL));
    ASSERT_NULL(hyp_graph_tier_tool_name(HYP_GRAPH_TIER_COUNT, 0U));
    PASS();
}

/* An installer only replaces a profile whose bytes it can prove it wrote, so
 * every generation it ever shipped must still render exactly. Generation 0 is
 * the eleven-tool set; 1 added `ask` to analysis (tools list only); 2 makes
 * the analysis prompt teach ask and the project-name rule; 3 DELETES the
 * project-name rule, because the server derives the project itself and the
 * prose now contradicts it, and stops sending the agent to get_code_snippet
 * for code that `ask` now returns with the answer — a tool that can answer in
 * one call is still worth nothing while the prompt orders a second. 4 makes
 * the memory surface reachable: `search_memory` in the requested list AND the
 * sentence saying when to reach for it, because either alone is a shape this
 * repository has already shipped and measured.
 * Scout is byte-identical across all of them. */
/* The half of the ask guidance that is stable across 2 and 3 — the marks below
 * are what each generation ADDS, so a rewording of the shared sentence cannot
 * silently satisfy a generation's own assertion. */
static const char ASK_GUIDANCE_MARK[] = "call ask first with the question as one string";
/* Generation 2 sent the agent to get_code_snippet for the code; generation 3
 * says the code arrived with the answer, which is what makes one call enough. */
static const char SNIPPET_FOLLOWUP_MARK[] = "verify with get_code_snippet";
static const char SOURCE_INLINE_MARK[] = "verbatim source of the top 2";
static const char PROJECT_RULE_MARK[] = "/home/u/repo \xe2\x86\x92 home-u-repo";
static const char PROJECT_DEFAULT_MARK[] = "Omit the project argument";
/* Generation 4 adds the memory surface, and adds it at BOTH ends: the sentence
 * that says when to reach for it, and the tool in the requested list. Either
 * one alone is the shape this table exists to prevent — a tool merely listed
 * was called in 4 runs of 60, and a sentence naming a tool the profile never
 * requests is the `ask` defect. So both marks are asserted, separately. */
static const char MEMORY_GUIDANCE_MARK[] = "call search_memory";
static const char MEMORY_TOOL_MARK[] = "search_memory";

TEST(agent_profiles_earlier_generations_still_render_for_migration) {
    ASSERT_EQ(hyp_graph_profile_generation(), 4U);
    for (size_t d = 0U; d < sizeof(direct_dialects) / sizeof(direct_dialects[0]); d++) {
        hyp_graph_profile_dialect_t dialect = direct_dialects[d].dialect;
        const char *binary = dialect == HYP_GRAPH_DIALECT_KIRO || dialect == HYP_GRAPH_DIALECT_CODEX
                                 ? "/opt/hyponoia/hyp"
                                 : NULL;
        for (int value = 0; value < (int)HYP_GRAPH_TIER_COUNT; value++) {
            hyp_graph_tier_t tier = (hyp_graph_tier_t)value;
            bool scout = tier == HYP_GRAPH_TIER_SCOUT;
            char *current =
                hyp_render_graph_profile(dialect, tier, HYP_GRAPH_ACCESS_DIRECT, binary);
            char *gen4 = hyp_render_graph_profile_generation(dialect, tier, HYP_GRAPH_ACCESS_DIRECT,
                                                             binary, 4U);
            char *gen3 = hyp_render_graph_profile_generation(dialect, tier, HYP_GRAPH_ACCESS_DIRECT,
                                                             binary, 3U);
            char *gen2 = hyp_render_graph_profile_generation(dialect, tier, HYP_GRAPH_ACCESS_DIRECT,
                                                             binary, 2U);
            char *gen1 = hyp_render_graph_profile_generation(dialect, tier, HYP_GRAPH_ACCESS_DIRECT,
                                                             binary, 1U);
            char *gen0 = hyp_render_graph_profile_generation(dialect, tier, HYP_GRAPH_ACCESS_DIRECT,
                                                             binary, 0U);
            char *future = hyp_render_graph_profile_generation(dialect, tier,
                                                               HYP_GRAPH_ACCESS_DIRECT, binary, 5U);
            bool ok = current && gen4 && gen3 && gen2 && gen1 && gen0 && !future &&
                      strcmp(current, gen4) == 0;
            if (ok) {
                /* 0 -> 1: the tools list, so Junie (names a server) never
                 * moved and scout never moves. 1 -> 2: the prompt, so every
                 * analysis profile that carries the prompt moved — Junie
                 * included, Vibe excluded (its prompt is a separate file,
                 * covered below) — and scout still did not. The guidance
                 * words appear exactly at 2. 2 -> 3: the same prompt again,
                 * this time swapping the project-name rule for "omit it" —
                 * the rule is present at exactly 2 and gone at 3. */
                bool tools_moved = dialect != HYP_GRAPH_DIALECT_JUNIE && !scout;
                bool prompt_moved = dialect != HYP_GRAPH_DIALECT_VIBE && !scout;
                ok = (strcmp(gen1, gen0) != 0) == tools_moved &&
                     (strcmp(gen2, gen1) != 0) == prompt_moved &&
                     (strcmp(gen3, gen2) != 0) == prompt_moved &&
                     (strstr(gen2, ASK_GUIDANCE_MARK) != NULL) == prompt_moved &&
                     (strstr(gen3, ASK_GUIDANCE_MARK) != NULL) == prompt_moved &&
                     (strstr(gen2, PROJECT_RULE_MARK) != NULL) == prompt_moved &&
                     (strstr(gen3, PROJECT_DEFAULT_MARK) != NULL) == prompt_moved &&
                     (strstr(gen2, SNIPPET_FOLLOWUP_MARK) != NULL) == prompt_moved &&
                     (strstr(gen3, SOURCE_INLINE_MARK) != NULL) == prompt_moved &&
                     !strstr(gen3, PROJECT_RULE_MARK) && !strstr(gen2, PROJECT_DEFAULT_MARK) &&
                     !strstr(gen3, SNIPPET_FOLLOWUP_MARK) && !strstr(gen2, SOURCE_INLINE_MARK) &&
                     !strstr(gen1, ASK_GUIDANCE_MARK) && !strstr(gen0, ASK_GUIDANCE_MARK);
                /* 3 -> 4: the memory surface, at BOTH ends. Every non-scout
                 * direct profile moves — Junie's prompt carries the sentence
                 * and Vibe's tool list carries the tool, so unlike 1->2 there
                 * is no dialect that stands still. The tool never reaches
                 * scout, and the WRITER never reaches any of them: a tier that
                 * states read-only in its own description does not request a
                 * tool that appends. */
                ok = ok && (strcmp(gen4, gen3) != 0) == !scout &&
                     (strstr(gen4, MEMORY_GUIDANCE_MARK) != NULL) == prompt_moved &&
                     (strstr(gen4, MEMORY_TOOL_MARK) != NULL) == !scout &&
                     !strstr(gen3, MEMORY_TOOL_MARK) && !strstr(gen3, MEMORY_GUIDANCE_MARK) &&
                     !strstr(gen4, "record_memory");
            }
            free(current);
            free(gen4);
            free(gen3);
            free(gen2);
            free(gen1);
            free(gen0);
            free(future);
            if (!ok) {
                FAIL("every shipped generation must render; 0->1 is ask in the tools list, "
                     "1->2 is the analysis prompt, 2->3 drops the project-name rule, 3->4 adds "
                     "search_memory to both the list and the prompt and record_memory to "
                     "neither, scout never moves");
            }
        }
    }
    /* The prompt on its own, which Vibe stores as a separate file. */
    for (int value = 0; value < (int)HYP_GRAPH_TIER_COUNT; value++) {
        hyp_graph_tier_t tier = (hyp_graph_tier_t)value;
        bool scout = tier == HYP_GRAPH_TIER_SCOUT;
        char *current = hyp_render_graph_prompt(tier, HYP_GRAPH_ACCESS_DIRECT);
        char *gen4 = hyp_render_graph_prompt_generation(tier, HYP_GRAPH_ACCESS_DIRECT, 4U);
        char *gen3 = hyp_render_graph_prompt_generation(tier, HYP_GRAPH_ACCESS_DIRECT, 3U);
        char *gen2 = hyp_render_graph_prompt_generation(tier, HYP_GRAPH_ACCESS_DIRECT, 2U);
        char *gen1 = hyp_render_graph_prompt_generation(tier, HYP_GRAPH_ACCESS_DIRECT, 1U);
        char *gen0 = hyp_render_graph_prompt_generation(tier, HYP_GRAPH_ACCESS_DIRECT, 0U);
        char *handoff = hyp_render_graph_prompt(tier, HYP_GRAPH_ACCESS_HANDOFF);
        char *handoff0 = hyp_render_graph_prompt_generation(tier, HYP_GRAPH_ACCESS_HANDOFF, 0U);
        bool ok = current && gen4 && gen3 && gen2 && gen1 && gen0 && handoff && handoff0 &&
                  strcmp(current, gen4) == 0 && strcmp(gen1, gen0) == 0 &&
                  (strcmp(gen2, gen1) != 0) == !scout && (strcmp(gen3, gen2) != 0) == !scout &&
                  (strcmp(gen4, gen3) != 0) == !scout &&
                  (strstr(gen2, ASK_GUIDANCE_MARK) != NULL) == !scout &&
                  (strstr(gen3, ASK_GUIDANCE_MARK) != NULL) == !scout &&
                  (strstr(gen2, PROJECT_RULE_MARK) != NULL) == !scout &&
                  (strstr(gen3, PROJECT_DEFAULT_MARK) != NULL) == !scout &&
                  (strstr(gen4, MEMORY_GUIDANCE_MARK) != NULL) == !scout &&
                  !strstr(gen3, MEMORY_GUIDANCE_MARK) && !strstr(gen4, "record_memory") &&
                  !strstr(gen3, PROJECT_RULE_MARK) && !strstr(gen2, PROJECT_DEFAULT_MARK) &&
                  strcmp(handoff, handoff0) == 0 && !strstr(handoff, ASK_GUIDANCE_MARK) &&
                  !strstr(handoff, MEMORY_GUIDANCE_MARK) &&
                  !hyp_render_graph_prompt_generation(tier, HYP_GRAPH_ACCESS_DIRECT, 5U);
        free(current);
        free(gen4);
        free(gen3);
        free(gen2);
        free(gen1);
        free(gen0);
        free(handoff);
        free(handoff0);
        if (!ok) {
            FAIL("prompt generations: 0 == 1, 2 adds ask guidance to analysis only, 3 swaps the "
                 "project-name rule for the omit-it rule, 4 adds the memory sentence to analysis "
                 "only and never names the writer, handoff never");
        }
    }
    char *rc1 = hyp_render_graph_profile_codex_rc1(HYP_GRAPH_TIER_VERIFY);
    ASSERT_NOT_NULL(rc1);
    ASSERT_NULL(strstr(rc1, "\"ask\""));
    ASSERT_NULL(strstr(rc1, ASK_GUIDANCE_MARK));
    ASSERT_NULL(strstr(rc1, MEMORY_TOOL_MARK));
    free(rc1);
    PASS();
}

/* ── The writer sits on no tier, and that is the point ─────────────────
 *
 * §4 C8u's constraint, asserted rather than commented: every generated tier
 * states read-only in its own description, so a tool that APPENDS may not be
 * on one. Reachability for the writer therefore comes from the full server,
 * not from a profile — which is why this test checks the negative on the
 * profile end and the positive on the server end, and would fail the moment
 * someone "fixed" the missing row by adding it to analysis. */
TEST(agent_profiles_the_memory_writer_is_on_no_tier) {
    ASSERT_TRUE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ALL, "record_memory"));
    ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ANALYSIS, "record_memory"));
    ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_SCOUT, "record_memory"));

    /* The reader is a different question and gets a different answer: it is
     * read-only, so analysis admits it and scout — the small surface where
     * every tool answers fast — does not. */
    ASSERT_TRUE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ALL, "search_memory"));
    ASSERT_TRUE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ANALYSIS, "search_memory"));
    ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_SCOUT, "search_memory"));

    for (int value = 0; value < (int)HYP_GRAPH_TIER_COUNT; value++) {
        hyp_graph_tier_t tier = (hyp_graph_tier_t)value;
        bool scout = tier == HYP_GRAPH_TIER_SCOUT;
        bool requests_writer = false;
        bool requests_reader = false;
        for (size_t i = 0U; hyp_graph_tier_tool_name(tier, i) != NULL; i++) {
            const char *name = hyp_graph_tier_tool_name(tier, i);
            requests_writer = requests_writer || strcmp(name, "record_memory") == 0;
            requests_reader = requests_reader || strcmp(name, "search_memory") == 0;
        }
        if (requests_writer) {
            FAIL("a read-only tier requests the memory WRITER");
        }
        if (requests_reader == scout) {
            FAIL("search_memory belongs to analysis and audit, never to scout");
        }
        /* And at the client end: the rendered Claude agent file. */
        char *rendered =
            hyp_render_graph_profile(HYP_GRAPH_DIALECT_CLAUDE, tier, HYP_GRAPH_ACCESS_DIRECT, NULL);
        ASSERT_NOT_NULL(rendered);
        bool file_has_writer = strstr(rendered, "mcp__hyponoia__record_memory") != NULL;
        bool file_has_reader = strstr(rendered, "mcp__hyponoia__search_memory") != NULL;
        free(rendered);
        if (file_has_writer) {
            FAIL("a generated agent file requests the memory writer");
        }
        if (file_has_reader == scout) {
            FAIL("the generated agent file disagrees with the tier about search_memory");
        }
    }
    PASS();
}

SUITE(agent_profiles) {
    RUN_TEST(agent_profiles_stable_tier_identity);
    RUN_TEST(agent_profiles_tiers_agree_with_the_server_profile_they_run_against);
    RUN_TEST(agent_profiles_earlier_generations_still_render_for_migration);
    RUN_TEST(agent_profiles_the_memory_writer_is_on_no_tier);
    RUN_TEST(agent_profiles_direct_dialects_are_coverage_aware_and_read_only);
    RUN_TEST(agent_profiles_tiers_encode_distinct_evidence_budgets);
    RUN_TEST(agent_profiles_handoff_requires_parent_evidence_without_child_mcp);
    RUN_TEST(agent_profiles_handoff_only_dialects_fail_closed_for_direct_access);
    RUN_TEST(agent_profiles_server_level_dialects_hard_enforce_read_only_tools);
    RUN_TEST(agent_profiles_kiro_is_valid_json_and_escapes_binary_path);
    RUN_TEST(agent_profiles_codex_declares_transport_and_escapes_binary_path);
    RUN_TEST(agent_profiles_vibe_uses_matching_prompt_identifier_and_contract);
    RUN_TEST(agent_profiles_render_deterministically_and_reject_invalid_inputs);
}
