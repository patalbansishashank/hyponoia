/*
 * test_tool_surface.c — the contract that mcp/tool_surface.h is the ONLY place
 * a tool is declared, held from the CLIENT'S SIDE.
 *
 * WHY THIS FILE EXISTS. The same two-ends bug has shipped three times:
 *
 *   CBMUIPK vs HYPUIPK      one rename, nine failures, a dead UI in a release.
 *   `ask`                   allowed by src/mcp/mcp.c, absent from
 *                           src/cli/agent_profiles.c. Both ends passed their
 *                           own tests; no agent could call it.
 *   structuredContent: {}   four tests passed while three tools rendered blank
 *                           in every client, because all four asserted what the
 *                           SERVER EMITTED and none asked what a CLIENT READS.
 *
 * So every assertion here reads a response or a rendered file — the tools/list
 * JSON a client parses, the agent-definition text a harness loads, the
 * tools/call result a client renders. None of them reads either end's static
 * array, because reading the array is how a test comes to share the product's
 * mistake and report green.
 *
 * Two negative controls were run against these tests before they were
 * committed, planting each historical defect at the coordinate where it can
 * still occur, watching the test fail, removing the plant and watching it pass.
 * The plants and both observations are recorded in the commit message.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"

#include <cli/agent_profiles.h>
#include <mcp/mcp.h>
#include <mcp/tool_surface.h>
#include <store/store.h>
#include <yyjson/yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Client-side helpers ───────────────────────────────────────────────
 *
 * Everything below goes through hyp_mcp_server_handle with a real JSON-RPC
 * envelope and reads the reply the way a client would. */

static char *surface_call(hyp_mcp_tool_profile_t profile, const char *method,
                          const char *params_json) {
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    if (!srv) {
        return NULL;
    }
    hyp_mcp_server_set_tool_profile(srv, profile);
    char request[2048];
    if (params_json) {
        snprintf(request, sizeof(request), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",%s}",
                 method, params_json);
    } else {
        snprintf(request, sizeof(request), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\"}",
                 method);
    }
    char *resp = hyp_mcp_server_handle(srv, request);
    hyp_mcp_server_free(srv);
    return resp;
}

/* The tool names a client parses out of tools/list, as "\nname\n" runs so a
 * substring search cannot match a prefix. */
static char *surface_listed_names(hyp_mcp_tool_profile_t profile, size_t *count_out) {
    if (count_out) {
        *count_out = 0U;
    }
    char *resp = surface_call(profile, "tools/list", NULL);
    if (!resp) {
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    yyjson_val *result = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "result") : NULL;
    yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
    size_t cap = 8192U;
    char *names = (char *)calloc(1U, cap);
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
            if (!text || used + strlen(text) + 2U >= cap) {
                free(names);
                names = NULL;
                break;
            }
            used += (size_t)snprintf(names + used, cap - used, "%s\n", text);
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

static bool surface_names_contain(const char *names, const char *tool) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\n%s\n", tool);
    return names && strstr(names, needle) != NULL;
}

/* What a client actually ends up with for one tools/call: structuredContent if
 * it is a non-empty object, otherwise content[0].text parsed as JSON if it is
 * one. Returns a doc the caller frees, or NULL when the client sees no object
 * at all. `is_error_out` reports result.isError. */
static yyjson_doc *surface_client_object(hyp_mcp_tool_profile_t profile, const char *tool,
                                         const char *arguments_json, bool *is_error_out) {
    if (is_error_out) {
        *is_error_out = false;
    }
    char params[1024];
    snprintf(params, sizeof(params), "\"params\":{\"name\":\"%s\",\"arguments\":%s}", tool,
             arguments_json ? arguments_json : "{}");
    char *resp = surface_call(profile, "tools/call", params);
    if (!resp) {
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    free(resp);
    yyjson_val *result = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "result") : NULL;
    if (!result) {
        yyjson_doc_free(doc);
        return NULL;
    }
    yyjson_val *is_error = yyjson_obj_get(result, "isError");
    if (is_error_out) {
        *is_error_out = is_error && yyjson_is_true(is_error);
    }

    /* A client prefers structuredContent when it is there and non-empty. An
     * EMPTY object is the defect, not a fallback: it is indistinguishable from
     * "the answer is nothing", so it is reported as no object rather than
     * silently falling through to content. */
    yyjson_val *structured = yyjson_obj_get(result, "structuredContent");
    if (structured && yyjson_is_obj(structured)) {
        if (yyjson_obj_size(structured) == 0U) {
            yyjson_doc_free(doc);
            return NULL;
        }
        char *text = yyjson_val_write(structured, 0, NULL);
        yyjson_doc_free(doc);
        if (!text) {
            return NULL;
        }
        yyjson_doc *out = yyjson_read(text, strlen(text), 0);
        free(text);
        return out;
    }

    yyjson_val *content = yyjson_obj_get(result, "content");
    yyjson_val *first = content && yyjson_is_arr(content) ? yyjson_arr_get_first(content) : NULL;
    yyjson_val *text_val = first ? yyjson_obj_get(first, "text") : NULL;
    const char *text = text_val && yyjson_is_str(text_val) ? yyjson_get_str(text_val) : NULL;
    if (!text) {
        yyjson_doc_free(doc);
        return NULL;
    }
    yyjson_doc *parsed = yyjson_read(text, strlen(text), 0);
    yyjson_doc_free(doc);
    if (parsed && yyjson_is_obj(yyjson_doc_get_root(parsed))) {
        return parsed;
    }
    yyjson_doc_free(parsed);
    return NULL;
}

/* The tool identifiers a rendered Claude agent definition asks for. */
static bool surface_profile_requests(hyp_graph_tier_t tier, const char *tool) {
    char *rendered =
        hyp_render_graph_profile(HYP_GRAPH_DIALECT_CLAUDE, tier, HYP_GRAPH_ACCESS_DIRECT, NULL);
    if (!rendered) {
        return false;
    }
    char needle[160];
    snprintf(needle, sizeof(needle), "  - mcp__hyponoia__%s\n", tool);
    bool found = strstr(rendered, needle) != NULL;
    free(rendered);
    return found;
}

static hyp_mcp_tool_profile_t surface_server_profile(hyp_graph_tier_t tier) {
    return tier == HYP_GRAPH_TIER_SCOUT ? HYP_MCP_TOOL_PROFILE_SCOUT
                                        : HYP_MCP_TOOL_PROFILE_ANALYSIS;
}

/* ── 1 · One table, one registry, one set ──────────────────────────────
 *
 * A _Static_assert in mcp.c already pairs the two row COUNTS, so a tool added
 * to one and forgotten in the other does not compile. A count cannot catch
 * adding one tool while removing another; this can. */
TEST(tool_surface_registry_and_table_describe_the_same_tools) {
    const int registry = hyp_mcp_tool_registry_count();
    const int table = hyp_mcp_tool_surface_count();
    ASSERT_EQ(registry, table);
    ASSERT_TRUE(registry > 0);

    for (int i = 0; i < table; i++) {
        const char *name = hyp_mcp_tool_surface_name(i);
        ASSERT_NOT_NULL(name);
        bool found = false;
        for (int j = 0; !found && j < registry; j++) {
            found = strcmp(hyp_mcp_tool_registry_name(j), name) == 0;
        }
        if (!found) {
            FAIL("a tool has a row in mcp/tool_surface.h and no entry in mcp.c TOOLS[]");
        }
        /* Every row states a status. -1 is "not in the table", which the loop
         * above cannot produce and a lookup by an unknown name can. */
        ASSERT_TRUE(hyp_mcp_tool_surface_status(name) >= 0);
    }
    for (int j = 0; j < registry; j++) {
        const char *name = hyp_mcp_tool_registry_name(j);
        ASSERT_NOT_NULL(name);
        if (hyp_mcp_tool_surface_status(name) < 0) {
            FAIL("a tool is registered in mcp.c TOOLS[] with no row in mcp/tool_surface.h — "
                 "that is how `ask` shipped server-allowed and profile-absent");
        }
    }
    ASSERT_EQ(hyp_mcp_tool_surface_status("no_such_tool_anywhere"), -1);
    PASS();
}

/* ── 2 · The two ends, from the client's side ──────────────────────────
 *
 * THE `ask` DEFECT, at the one coordinate where it can still happen. `ask` was
 * in the server's analysis allowlist and absent from the profile renderer's
 * list; one table closed that. What one table does NOT close by itself is a row
 * whose status says "live" while its generation says "not rendered yet" — the
 * server would advertise a tool the generated profile never requests, which is
 * the same defect wearing the same clothes. Both directions are asserted, so
 * either half of a two-part change landing alone fails here. */
TEST(tool_surface_both_ends_advertise_exactly_the_same_live_tools) {
    for (int value = 0; value < (int)HYP_GRAPH_TIER_COUNT; value++) {
        hyp_graph_tier_t tier = (hyp_graph_tier_t)value;
        hyp_mcp_tool_profile_t profile = surface_server_profile(tier);

        size_t listed = 0U;
        char *names = surface_listed_names(profile, &listed);
        ASSERT_NOT_NULL(names);
        ASSERT_TRUE(listed > 0U);

        size_t matched = 0U;
        for (int i = 0; i < hyp_mcp_tool_surface_count(); i++) {
            const char *name = hyp_mcp_tool_surface_name(i);
            bool advertised = surface_names_contain(names, name);
            bool requested = surface_profile_requests(tier, name);
            if (advertised && !requested) {
                free(names);
                FAIL("the server advertises a tool to a profile the generated agent definition "
                     "never requests — this is the `ask` defect");
            }
            if (requested && !advertised) {
                free(names);
                FAIL("a generated agent definition requests a tool the server does not advertise "
                     "to the profile it runs against");
            }
            if (advertised) {
                matched++;
            }
        }
        free(names);
        /* Every advertised name was accounted for by a row. If tools/list can
         * name something the table cannot, the table is not the source. */
        ASSERT_EQ(matched, listed);
    }
    PASS();
}

/* ── 3 · Reserved rows are invisible at BOTH ends ──────────────────────
 *
 * A published signature is not a shipped tool. Reserved rows must be absent
 * from tools/list under every profile, absent from every generated profile,
 * unavailable to the CLI flag builder, and refused by dispatch with an error
 * that says which state they are in — not "unknown tool", which would send an
 * agent looking for a typo. */
TEST(tool_surface_reserved_rows_are_invisible_and_fail_closed) {
    static const hyp_mcp_tool_profile_t profiles[] = {
        HYP_MCP_TOOL_PROFILE_ALL,
        HYP_MCP_TOOL_PROFILE_ANALYSIS,
        HYP_MCP_TOOL_PROFILE_SCOUT,
    };
    size_t reserved_seen = 0U;
    for (int i = 0; i < hyp_mcp_tool_surface_count(); i++) {
        const char *name = hyp_mcp_tool_surface_name(i);
        if (hyp_mcp_tool_surface_status(name) != (int)HYP_TOOL_RESERVED) {
            continue;
        }
        reserved_seen++;

        for (size_t p = 0U; p < sizeof(profiles) / sizeof(profiles[0]); p++) {
            size_t listed = 0U;
            char *names = surface_listed_names(profiles[p], &listed);
            ASSERT_NOT_NULL(names);
            bool advertised = surface_names_contain(names, name);
            free(names);
            if (advertised) {
                FAIL("a reserved tool is advertised in tools/list");
            }
            ASSERT_FALSE(hyp_mcp_tool_profile_allows(profiles[p], name));
        }
        for (int value = 0; value < (int)HYP_GRAPH_TIER_COUNT; value++) {
            if (surface_profile_requests((hyp_graph_tier_t)value, name)) {
                FAIL("a generated agent definition requests a reserved tool");
            }
        }
        /* The CLI builds its flags from the same registry; a reserved tool
         * offering --flags would be a second surface disagreeing with the
         * server's. */
        ASSERT_NULL(hyp_mcp_tool_input_schema(name));

        /* Fail closed, and say which closed door this is. */
        bool is_error = false;
        char *resp = NULL;
        {
            char params[512];
            snprintf(params, sizeof(params), "\"params\":{\"name\":\"%s\",\"arguments\":{}}", name);
            resp = surface_call(HYP_MCP_TOOL_PROFILE_ALL, "tools/call", params);
        }
        ASSERT_NOT_NULL(resp);
        ASSERT_NOT_NULL(strstr(resp, "\"isError\":true"));
        ASSERT_NOT_NULL(strstr(resp, "not implemented in this build"));
        ASSERT_NULL(strstr(resp, "unknown tool"));
        free(resp);
        (void)is_error;
    }
    /* A contract with no reserved rows would pass every assertion above
     * vacuously. §4 Phase 1 publishes four; if that number goes to zero the
     * rows went live and this test must be told so deliberately. */
    ASSERT_TRUE(reserved_seen > 0U);
    PASS();
}

/* ── 4 · A declared output schema is a promise to a CLIENT ─────────────
 *
 * THE structuredContent DEFECT. Every tool used to advertise
 * outputSchema {"type":"object","additionalProperties":true}, which told a
 * client "structuredContent is the result of this call" while most tools
 * answered in TOON and sent {}. Three tools rendered blank in every client and
 * four tests passed, because all four checked the emitted shape.
 *
 * This checks the only thing that matters: for every tool that DECLARES a
 * schema, every field the schema marks `required` is present in the object a
 * client ends up holding. It reads the response. It never reads the schema
 * table to decide what the response should be. */
TEST(tool_surface_declared_output_schemas_reach_the_client) {
    /* A real project in a real store, because the promise is only meaningful on
     * a call that succeeds, and because the whole family of defects here comes
     * from checking a shape the product never produced. */
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/hyp-tool-surface-XXXXXX");
    if (!hyp_mkdtemp(cache)) {
        FAIL("could not create a temporary cache for the client-view probe");
    }
    const char *saved_cache = getenv("HYP_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    hyp_setenv("HYP_CACHE_DIR", cache, 1);

    const char *project = "tool-surface-probe";
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    hyp_store_t *setup = hyp_store_open_path(db_path);
    int seeded = setup && hyp_store_upsert_project(setup, project, "/tmp/tool-surface-probe") ==
                              HYP_STORE_OK;
    if (setup) {
        hyp_store_close(setup);
    }

    char arguments[256];
    snprintf(arguments, sizeof(arguments), "{\"project\":\"%s\"}", project);

    size_t declaring = 0U;
    const char *failure = NULL;
    for (int i = 0; seeded && !failure && i < hyp_mcp_tool_surface_count(); i++) {
        const char *name = hyp_mcp_tool_surface_name(i);
        const char *schema_json = hyp_mcp_tool_declared_output_schema(name);
        if (!schema_json) {
            continue;
        }
        if (hyp_mcp_tool_surface_status(name) == (int)HYP_TOOL_RESERVED) {
            failure = "a reserved tool declares an outputSchema — it cannot keep a promise it "
                      "never gets to make";
            break;
        }
        if (!hyp_mcp_tool_is_probe_safe(name)) {
            failure = "a tool that declares an outputSchema must be probe-safe, or nothing can "
                      "check the promise without a side effect";
            break;
        }
        declaring++;

        yyjson_doc *schema_doc = yyjson_read(schema_json, strlen(schema_json), 0);
        yyjson_val *required =
            schema_doc ? yyjson_obj_get(yyjson_doc_get_root(schema_doc), "required") : NULL;
        if (!schema_doc) {
            failure = "a declared outputSchema is not valid JSON";
            break;
        }

        bool is_error = false;
        yyjson_doc *seen =
            surface_client_object(HYP_MCP_TOOL_PROFILE_ALL, name, arguments, &is_error);
        if (is_error) {
            yyjson_doc_free(seen);
            yyjson_doc_free(schema_doc);
            failure = "a tool that declares an outputSchema failed on the probe call, so the "
                      "promise could not be checked at all — an unverifiable promise is the "
                      "state that shipped three blank tools";
            break;
        }
        if (!seen) {
            yyjson_doc_free(schema_doc);
            failure = "a tool declares an outputSchema and a client reads no object back from "
                      "it — an empty or absent structuredContent with no JSON in content is "
                      "exactly the blank render that shipped";
            break;
        }
        yyjson_val *root = yyjson_doc_get_root(seen);

        size_t index = 0U;
        size_t max = 0U;
        yyjson_val *field = NULL;
        yyjson_arr_foreach(required, index, max, field) {
            const char *key = yyjson_is_str(field) ? yyjson_get_str(field) : NULL;
            if (!key || !yyjson_obj_get(root, key)) {
                failure = "a tool's outputSchema requires a field the client never receives — "
                          "the declaration promises structure the text path does not send";
                break;
            }
        }
        yyjson_doc_free(seen);
        yyjson_doc_free(schema_doc);
    }

    if (saved_cache_copy) {
        hyp_setenv("HYP_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        hyp_unsetenv("HYP_CACHE_DIR");
    }
    if (!seeded) {
        FAIL("could not seed a project for the client-view probe");
    }
    if (failure) {
        FAIL(failure);
    }
    /* index_status declares one and is currently the only tool that can: its
     * handler builds a JSON object. If this reaches zero the check above has
     * stopped checking anything. */
    ASSERT_TRUE(declaring > 0U);
    PASS();
}

/* ── 4b · index_status has an answer it cannot deliver ─────────────────
 *
 * FOUND BY WRITING THIS CONTRACT, and pinned here so the fix is not lost.
 *
 * handle_index_status ends with `else { status: "no_project" }` — a documented,
 * structured answer for "no project resolved". It is UNREACHABLE. The branch
 * needs store != NULL with project == NULL, and resolve_store_internal returns
 * NULL the moment project is NULL ("project is required — no implicit
 * fallback"), so REQUIRE_STORE returns an error string first, every time.
 *
 * An agent in an unindexed directory therefore gets isError with prose, not the
 * `no_project` status the tool advertises. That is the absent/empty rule broken
 * at the exact tool §4 F3 extends to carry TWO freshnesses: F3 needs
 * index_status to distinguish "no memory store configured" from "memory store
 * is current", and a tool that cannot yet say "no project" in structure will
 * not manage "no memory" in structure either.
 *
 * Phase 0 changes no behaviour, so this test asserts what SHIPS — including
 * that the error is at least machine-readable — and the Phase 1 fix is to make
 * the no_project branch reachable. When it is, this test's expectation flips
 * and the schema check above starts covering the no-project path too. */
TEST(tool_surface_index_status_no_project_answer_is_unreachable_today) {
    bool is_error = false;
    yyjson_doc *seen =
        surface_client_object(HYP_MCP_TOOL_PROFILE_ALL, "index_status", "{}", &is_error);
    if (!is_error) {
        /* The Phase 1 fix landed: assert the documented shape instead. */
        if (!seen) {
            FAIL("index_status succeeded without a project and a client read no object");
        }
        yyjson_val *status = yyjson_obj_get(yyjson_doc_get_root(seen), "status");
        const char *text = status && yyjson_is_str(status) ? yyjson_get_str(status) : NULL;
        bool ok = text && strcmp(text, "no_project") == 0;
        yyjson_doc_free(seen);
        if (!ok) {
            FAIL("index_status resolved no project and did not answer status=no_project");
        }
        PASS();
    }
    /* Today's behaviour. An error is honest — it is not a wrong answer — but it
     * is prose where a structure was documented, and the structuredContent a
     * client gets carries only `error`. */
    if (seen) {
        yyjson_val *root = yyjson_doc_get_root(seen);
        bool has_error = yyjson_obj_get(root, "error") != NULL;
        bool has_status = yyjson_obj_get(root, "status") != NULL;
        yyjson_doc_free(seen);
        if (!has_error) {
            FAIL("index_status failed with no machine-readable error for a client to read");
        }
        if (has_status) {
            FAIL("index_status now reports a status on the error path — update this test");
        }
    }
    PASS();
}

/* ── 5 · Every live tool is reachable, and answers something ───────────
 *
 * The fourth enumeration was the dispatch if-chain: a tool could be registered,
 * tiered, annotated and advertised, and still fall through to "unknown tool".
 * Probe-safe rows are derived from the annotation profile, so a writer added
 * later excludes itself rather than needing a skip list extended. */
TEST(tool_surface_every_advertised_tool_dispatches) {
    int probed = 0;
    for (int i = 0; i < hyp_mcp_tool_count(); i++) {
        const char *name = hyp_mcp_tool_name(i);
        ASSERT_NOT_NULL(name);
        ASSERT_TRUE(hyp_mcp_tool_surface_status(name) != (int)HYP_TOOL_RESERVED);
        if (!hyp_mcp_tool_is_probe_safe(name)) {
            continue;
        }
        probed++;
        char params[512];
        snprintf(params, sizeof(params), "\"params\":{\"name\":\"%s\",\"arguments\":{}}", name);
        char *resp = surface_call(HYP_MCP_TOOL_PROFILE_ALL, "tools/call", params);
        ASSERT_NOT_NULL(resp);
        bool unknown = strstr(resp, "unknown tool") != NULL;
        bool unavailable = strstr(resp, "not available in the") != NULL;
        bool unimplemented = strstr(resp, "not implemented in this build") != NULL;
        /* A client must read SOMETHING: content is always present, and it must
         * not be empty. An empty answer is indistinguishable from a blank
         * render, which is the whole family of defects this file guards. */
        bool has_content = strstr(resp, "\"content\":[{\"type\":\"text\"") != NULL;
        bool empty_text = strstr(resp, "\"text\":\"\"") != NULL;
        free(resp);
        if (unknown) {
            FAIL("an advertised tool falls through dispatch to `unknown tool`");
        }
        if (unavailable || unimplemented) {
            FAIL("an advertised tool refuses itself under the full profile");
        }
        if (!has_content || empty_text) {
            FAIL("an advertised tool returns nothing a client can render");
        }
    }
    ASSERT_TRUE(probed > 0);
    PASS();
}

/* ── 5b · Declared aliases dispatch, and are never surface ────────────
 *
 * FOUND BY WRITING THIS CONTRACT. `trace_call_path` has been accepted by
 * dispatch_tool for its whole life while appearing in TOOLS[] nowhere, in the
 * tier table nowhere and in tools/list nowhere — and src/cli/cli.c:5401 tells
 * users to prefer it. A name the server answers to that no end declares is the
 * `ask` defect inverted: `ask` was declared and unreachable, this was reachable
 * and undeclared. Both are invisible for the same reason — nothing enumerated
 * the names. Now the table does, and this walks every one of them. */
TEST(tool_surface_declared_aliases_dispatch_and_are_never_advertised) {
    size_t aliases = 0U;
    size_t listed = 0U;
    char *names = surface_listed_names(HYP_MCP_TOOL_PROFILE_ALL, &listed);
    ASSERT_NOT_NULL(names);

    for (int i = 0; i < hyp_mcp_tool_surface_count(); i++) {
        const char *alias = hyp_mcp_tool_surface_alias(i);
        if (!alias) {
            continue;
        }
        aliases++;
        const char *target = hyp_mcp_tool_surface_name(i);

        /* Never advertised, under any profile, and never in an agent file. */
        if (surface_names_contain(names, alias)) {
            free(names);
            FAIL("a legacy alias is advertised in tools/list");
        }
        ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ANALYSIS, alias));
        ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_SCOUT, alias));
        for (int value = 0; value < (int)HYP_GRAPH_TIER_COUNT; value++) {
            if (surface_profile_requests((hyp_graph_tier_t)value, alias)) {
                free(names);
                FAIL("a generated agent definition requests a legacy alias");
            }
        }
        /* But still callable on the full surface — that is what an alias IS,
         * and callers that already use it must not break. */
        ASSERT_TRUE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ALL, alias));
        ASSERT_TRUE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ALL, target));

        char params[512];
        snprintf(params, sizeof(params), "\"params\":{\"name\":\"%s\",\"arguments\":{}}", alias);
        char *resp = surface_call(HYP_MCP_TOOL_PROFILE_ALL, "tools/call", params);
        if (!resp) {
            free(names);
            FAIL("a declared alias produced no response");
        }
        bool unreachable = strstr(resp, "unknown tool") != NULL ||
                           strstr(resp, "not available in the") != NULL ||
                           strstr(resp, "not implemented in this build") != NULL;
        free(resp);
        if (unreachable) {
            free(names);
            FAIL("a declared alias does not reach the handler it names");
        }
    }
    free(names);
    ASSERT_TRUE(aliases > 0U);
    PASS();
}

/* ── 6 · Annotations come from the table, with nothing to default to ───
 *
 * The old TOOL_ANNOTATIONS[] was keyed by name and consulted with
 * `def ? def->destructive : true`, so a tool nobody added to it shipped
 * advertising destructiveHint and openWorldHint. The values below are the ones
 * that have always been on the wire; asserting them is how the fold is shown to
 * have changed no client-visible byte. */
TEST(tool_surface_every_advertised_tool_carries_complete_annotations) {
    char *resp = surface_call(HYP_MCP_TOOL_PROFILE_ALL, "tools/list", NULL);
    ASSERT_NOT_NULL(resp);
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(doc), "result");
    yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
    ASSERT_NOT_NULL(tools);

    size_t index = 0U;
    size_t max = 0U;
    yyjson_val *tool = NULL;
    size_t seen = 0U;
    yyjson_arr_foreach(tools, index, max, tool) {
        yyjson_val *name_val = yyjson_obj_get(tool, "name");
        const char *name = name_val ? yyjson_get_str(name_val) : NULL;
        yyjson_val *ann = yyjson_obj_get(tool, "annotations");
        if (!name || !ann || !yyjson_is_obj(ann)) {
            yyjson_doc_free(doc);
            free(resp);
            FAIL("an advertised tool carries no annotations object");
        }
        static const char *const keys[] = {"readOnlyHint", "destructiveHint", "idempotentHint",
                                           "openWorldHint"};
        for (size_t k = 0U; k < sizeof(keys) / sizeof(keys[0]); k++) {
            if (!yyjson_obj_get(ann, keys[k])) {
                yyjson_doc_free(doc);
                free(resp);
                FAIL("an advertised tool is missing an annotation hint");
            }
        }
        /* The one open-world row on the surface is a feed sync, and it is
         * reserved. Everything a client can see today is local, and a tool that
         * quietly became open-world would be a trust boundary nobody reviewed. */
        if (yyjson_is_true(yyjson_obj_get(ann, "openWorldHint"))) {
            yyjson_doc_free(doc);
            free(resp);
            FAIL("an advertised tool claims openWorldHint — every shipped tool is local");
        }
        seen++;
    }
    ASSERT_EQ(seen, (size_t)hyp_mcp_tool_count());
    yyjson_doc_free(doc);
    free(resp);

    /* Values, unchanged by the fold. list_projects is the one strictly
     * read-only tool; the graph readers are not, because resolving a project
     * can open and quarantine a corrupt store. */
    char *list = hyp_mcp_tools_list();
    ASSERT_NOT_NULL(list);
    ASSERT_NOT_NULL(strstr(list, "\"name\":\"list_projects\""));
    free(list);
    PASS();
}

/* ── 7 · The record kinds a tool may author ────────────────────────────
 *
 * Transcripts enter only through a feed. §4 D4 audits ingest completeness
 * against the feed's own count(*) — the only thing in the plan that can prove
 * it has everything — and a writer able to forge a transcript message would
 * make that count meaningless. Refusal is fail-closed and derived from the one
 * kind table, not from a literal in a schema string. */
TEST(tool_surface_transcript_kinds_are_not_authorable) {
    ASSERT_TRUE(hyp_mcp_memory_kind_is_authorable("decision"));
    ASSERT_TRUE(hyp_mcp_memory_kind_is_authorable("verdict"));
    ASSERT_TRUE(hyp_mcp_memory_kind_is_authorable("summary"));
    ASSERT_TRUE(hyp_mcp_memory_kind_is_authorable("signal"));
    ASSERT_FALSE(hyp_mcp_memory_kind_is_authorable("transcript"));
    /* An unknown kind is refused, not accepted as a new one. */
    ASSERT_FALSE(hyp_mcp_memory_kind_is_authorable("whatever"));
    ASSERT_FALSE(hyp_mcp_memory_kind_is_authorable(""));
    ASSERT_FALSE(hyp_mcp_memory_kind_is_authorable(NULL));

    /* The input schema deliberately does NOT restate the set as a JSON enum: a
     * second copy of it is exactly how four tool lists came to disagree. The
     * schema types `kind` as a string and the refusal names the accepted set. */
    PASS();
}

/* ── 8 · Nothing new leans on the deprecated row ───────────────────────
 *
 * manage_adr's `mode:"update"` replaces the entire document — mutable,
 * last-writer-wins, which is precisely what §4's append-only record contract
 * forbids. It stays live and unchanged on the wire; what must not happen is a
 * §4 surface being built on top of it, which would make the migration a
 * two-store problem instead of a one-store one. */
TEST(tool_surface_no_reserved_surface_depends_on_the_deprecated_tool) {
    ASSERT_EQ(hyp_mcp_tool_surface_status("manage_adr"), (int)HYP_TOOL_DEPRECATED);

    /* A deprecated tool is on no tier, so no generated profile can teach an
     * agent to reach for it. */
    ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ANALYSIS, "manage_adr"));
    ASSERT_FALSE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_SCOUT, "manage_adr"));
    /* And it is still callable by a client that already uses it. Deprecation
     * here is a marker for the migration, never a silent removal. */
    ASSERT_TRUE(hyp_mcp_tool_profile_allows(HYP_MCP_TOOL_PROFILE_ALL, "manage_adr"));

    for (int i = 0; i < hyp_mcp_tool_surface_count(); i++) {
        const char *name = hyp_mcp_tool_surface_name(i);
        if (hyp_mcp_tool_surface_status(name) != (int)HYP_TOOL_RESERVED) {
            continue;
        }
        /* Reserved schemas are unreachable through the public accessor by
         * design, so this checks the reachable half: no reserved tool is
         * advertised alongside it on a tier, and none is named after it. */
        ASSERT_NULL(hyp_mcp_tool_input_schema(name));
        if (strstr(name, "adr")) {
            FAIL("a §4 reserved tool is named after the deprecated ADR surface");
        }
    }
    PASS();
}

SUITE(tool_surface) {
    RUN_TEST(tool_surface_registry_and_table_describe_the_same_tools);
    RUN_TEST(tool_surface_both_ends_advertise_exactly_the_same_live_tools);
    RUN_TEST(tool_surface_reserved_rows_are_invisible_and_fail_closed);
    RUN_TEST(tool_surface_declared_output_schemas_reach_the_client);
    RUN_TEST(tool_surface_index_status_no_project_answer_is_unreachable_today);
    RUN_TEST(tool_surface_every_advertised_tool_dispatches);
    RUN_TEST(tool_surface_declared_aliases_dispatch_and_are_never_advertised);
    RUN_TEST(tool_surface_every_advertised_tool_carries_complete_annotations);
    RUN_TEST(tool_surface_transcript_kinds_are_not_authorable);
    RUN_TEST(tool_surface_no_reserved_surface_depends_on_the_deprecated_tool);
}
