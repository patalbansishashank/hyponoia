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
#include <foundation/record.h>
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

/* What a client actually ends up with for one tool result object:
 * structuredContent if it is a non-empty object, otherwise content[0].text
 * parsed as JSON if it is one. Returns a NEW doc the caller frees, or NULL
 * when the client sees no object at all. An EMPTY structuredContent is the
 * defect, not a fallback: it is indistinguishable from "the answer is
 * nothing", so it is reported as no object rather than silently falling
 * through to content. `is_error_out` reports isError. */
static yyjson_doc *surface_object_from_result(yyjson_val *result, bool *is_error_out) {
    if (is_error_out) {
        *is_error_out = false;
    }
    if (!result) {
        return NULL;
    }
    yyjson_val *is_error = yyjson_obj_get(result, "isError");
    if (is_error_out) {
        *is_error_out = is_error && yyjson_is_true(is_error);
    }

    yyjson_val *structured = yyjson_obj_get(result, "structuredContent");
    if (structured && yyjson_is_obj(structured)) {
        if (yyjson_obj_size(structured) == 0U) {
            return NULL;
        }
        char *text = yyjson_val_write(structured, 0, NULL);
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
        return NULL;
    }
    yyjson_doc *parsed = yyjson_read(text, strlen(text), 0);
    if (parsed && yyjson_is_obj(yyjson_doc_get_root(parsed))) {
        return parsed;
    }
    yyjson_doc_free(parsed);
    return NULL;
}

/* The client's object for one tools/call through a throwaway server. */
static yyjson_doc *surface_client_object(hyp_mcp_tool_profile_t profile, const char *tool,
                                         const char *arguments_json, bool *is_error_out) {
    char params[1024];
    snprintf(params, sizeof(params), "\"params\":{\"name\":\"%s\",\"arguments\":%s}", tool,
             arguments_json ? arguments_json : "{}");
    char *resp = surface_call(profile, "tools/call", params);
    if (!resp) {
        if (is_error_out) {
            *is_error_out = false;
        }
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    free(resp);
    yyjson_val *result = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "result") : NULL;
    yyjson_doc *out = surface_object_from_result(result, is_error_out);
    yyjson_doc_free(doc);
    return out;
}

/* The client's object for one call against a CONFIGURED server. The §4 F3
 * memory tests attach record sets to the server first, which surface_call's
 * throwaway instance cannot carry. */
static yyjson_doc *surface_client_object_on(hyp_mcp_server_t *srv, const char *tool,
                                            const char *arguments_json, bool *is_error_out) {
    char *resp = srv ? hyp_mcp_handle_tool(srv, tool, arguments_json ? arguments_json : "{}")
                     : NULL;
    if (!resp) {
        if (is_error_out) {
            *is_error_out = false;
        }
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    free(resp);
    yyjson_doc *out = surface_object_from_result(doc ? yyjson_doc_get_root(doc) : NULL,
                                                 is_error_out);
    yyjson_doc_free(doc);
    return out;
}

/* ── §4 F3 fixtures ────────────────────────────────────────────────────
 *
 * A fresh, empty cache pointed at by HYP_CACHE_DIR for one test: zero indexed
 * projects, deterministically, so "no project could be resolved" is a fact of
 * the fixture rather than of whatever suite ran before this one. */
typedef struct {
    char dir[256];
    char *saved;
} surface_cache_fixture_t;

static bool surface_cache_begin(surface_cache_fixture_t *fx) {
    snprintf(fx->dir, sizeof(fx->dir), "/tmp/hyp-tool-surface-XXXXXX");
    if (!hyp_mkdtemp(fx->dir)) {
        return false;
    }
    const char *saved = getenv("HYP_CACHE_DIR");
    fx->saved = saved ? strdup(saved) : NULL;
    hyp_setenv("HYP_CACHE_DIR", fx->dir, 1);
    return true;
}

static void surface_cache_end(surface_cache_fixture_t *fx) {
    if (fx->saved) {
        hyp_setenv("HYP_CACHE_DIR", fx->saved, 1);
        free(fx->saved);
        fx->saved = NULL;
    } else {
        hyp_unsetenv("HYP_CACHE_DIR");
    }
}

/* A record whose only varying field is content. C2's id commits to every
 * field, so distinct contents are distinct ids — all these tests need. The
 * timestamp is fixed and caller-supplied; nothing here reads a clock. */
static const hyp_record_t *surface_record(const char *content) {
    hyp_record_input_t in;
    memset(&in, 0, sizeof(in));
    in.kind = HYP_RECORD_DECISION;
    in.author = "agent:f3-test";
    in.timestamp_ms = INT64_C(1770985600000);
    in.content = content;
    const hyp_record_t *rec = NULL;
    return hyp_record_build(&in, &rec) == HYP_RECORD_OK ? rec : NULL;
}

/* Upstream providers for the seam: one lends the comparison set, one reports
 * "could not read" — which is NULL, never an empty set, because an empty set
 * says "upstream has nothing" and that is a different (and here false) claim. */
static const hyp_record_set_t *surface_upstream_borrow(void *ctx) {
    return (const hyp_record_set_t *)ctx;
}

static const hyp_record_set_t *surface_upstream_unreadable(void *ctx) {
    (void)ctx;
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
    surface_cache_fixture_t fx;
    if (!surface_cache_begin(&fx)) {
        FAIL("could not create a temporary cache for the client-view probe");
    }

    const char *project = "tool-surface-probe";
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", fx.dir, project);
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

    surface_cache_end(&fx);
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

/* ── 4b · index_status's no_project answer REACHES the client ──────────
 *
 * THE PHASE 0 DEAD BRANCH, made live by §4 F3 — this test held the finding
 * and has flipped to holding the fix. handle_index_status documented a
 * structured answer for "no project resolved" ({"status":"no_project"}) that
 * was UNREACHABLE from the day it shipped: the branch needed store != NULL
 * with project == NULL, and resolve_store returns NULL the moment project is
 * NULL, so REQUIRE_STORE answered isError prose first, every time. The fix
 * answers before touching the store. This asserts the CLIENT's view of it —
 * a non-error object saying status=no_project — with a fresh empty cache so
 * "nothing resolves" is a property of the fixture, not of suite order.
 * scripts/ci/mcp-client-view.py asserts the same thing against a real stdio
 * subprocess in an unindexed directory. */
TEST(tool_surface_index_status_no_project_answer_reaches_the_client) {
    surface_cache_fixture_t fx;
    if (!surface_cache_begin(&fx)) {
        FAIL("could not create a fresh cache for the no-project probe");
    }
    bool is_error = false;
    yyjson_doc *seen =
        surface_client_object(HYP_MCP_TOOL_PROFILE_ALL, "index_status", "{}", &is_error);
    surface_cache_end(&fx);
    if (is_error) {
        yyjson_doc_free(seen);
        FAIL("index_status in an unindexed directory answers isError again — the Phase 0 "
             "dead branch is back");
    }
    if (!seen) {
        FAIL("index_status succeeded without a project and a client read no object");
    }
    yyjson_val *root = yyjson_doc_get_root(seen);
    yyjson_val *status = yyjson_obj_get(root, "status");
    const char *text = status && yyjson_is_str(status) ? yyjson_get_str(status) : NULL;
    bool ok = text && strcmp(text, "no_project") == 0;
    /* Absence discipline on this path: no project key (the schema says so),
     * no code block (there is no project whose code could have a freshness),
     * and no memory block (no record store is configured here) — each absent
     * key is a real answer, not an omission. */
    bool clean = !yyjson_obj_get(root, "project") && !yyjson_obj_get(root, "code") &&
                 !yyjson_obj_get(root, "memory");
    yyjson_doc_free(seen);
    if (!ok) {
        FAIL("index_status resolved no project and did not answer status=no_project");
    }
    if (!clean) {
        FAIL("the no_project answer carries keys whose absence was the documented answer");
    }
    PASS();
}

/* ── 4c · §4 F3: memory absent means "no store", never "up to date" ────
 *
 * The `memory` sibling must be ABSENT ENTIRELY when no record store is
 * configured. Absent means "look elsewhere"; an empty object, a status, or a
 * zero here would each be a confident answer about a store that does not
 * exist — and an agent that believes it has current decisions while missing
 * 40 will confidently explain why `foo` looked the way it did last week. */
TEST(tool_surface_index_status_memory_absent_without_a_record_store) {
    surface_cache_fixture_t fx;
    if (!surface_cache_begin(&fx)) {
        FAIL("could not create a fresh cache");
    }
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    bool is_error = false;
    yyjson_doc *seen = surface_client_object_on(srv, "index_status", "{}", &is_error);
    hyp_mcp_server_free(srv);
    surface_cache_end(&fx);
    ASSERT_FALSE(is_error);
    ASSERT_NOT_NULL(seen);
    bool absent = yyjson_obj_get(yyjson_doc_get_root(seen), "memory") == NULL;
    yyjson_doc_free(seen);
    if (!absent) {
        FAIL("no record store is configured and index_status reports a memory block anyway — "
             "absent means look elsewhere, and this answer invents one");
    }
    PASS();
}

/* ── 4d · §4 F3: unknowable is `unknown` + ABSENT, never a number ──────
 *
 * Two ways the upstream set can be unknowable — no provider configured (the
 * shipped state until C6u lands sync) and a configured provider that cannot
 * read — and one honest answer for both: status "unknown" with `missing` and
 * `records_upstream` ABSENT. `missing: 0` here would be the confident wrong
 * answer F3 exists to prevent: told it is current, an agent stops checking.
 * The negative control for this test is planting exactly that (emit 0 on the
 * unreadable path) and watching it fail. */
TEST(tool_surface_index_status_memory_unknown_when_upstream_unreadable) {
    surface_cache_fixture_t fx;
    if (!surface_cache_begin(&fx)) {
        FAIL("could not create a fresh cache");
    }
    hyp_record_set_t *local = hyp_record_set_create();
    const hyp_record_t *rec = surface_record("a decision this machine holds");
    bool seeded = local && rec && hyp_record_set_add(local, rec, NULL) == HYP_RECORD_OK;
    hyp_record_free(rec);

    const char *failure = NULL;
    /* Case 1: no provider at all. Case 2: a provider that cannot read. */
    for (int unreadable = 0; seeded && !failure && unreadable < 2; unreadable++) {
        hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
        if (!srv) {
            failure = "server construction failed";
            break;
        }
        hyp_mcp_server_set_memory_store(srv, local);
        if (unreadable) {
            hyp_mcp_server_set_memory_upstream(srv, "test-feed", surface_upstream_unreadable,
                                               NULL);
        }
        bool is_error = false;
        yyjson_doc *seen = surface_client_object_on(srv, "index_status", "{}", &is_error);
        hyp_mcp_server_free(srv);
        if (is_error || !seen) {
            yyjson_doc_free(seen);
            failure = "index_status did not answer";
            break;
        }
        yyjson_val *memory = yyjson_obj_get(yyjson_doc_get_root(seen), "memory");
        yyjson_val *status = memory ? yyjson_obj_get(memory, "status") : NULL;
        const char *word = status && yyjson_is_str(status) ? yyjson_get_str(status) : NULL;
        yyjson_val *records_local = memory ? yyjson_obj_get(memory, "records_local") : NULL;
        if (!memory) {
            failure = "a record store is configured and the memory block is absent — absent "
                      "must mean no store, and here there is one";
        } else if (!word || strcmp(word, "unknown") != 0) {
            failure = "the upstream set was unreadable and memory.status is not \"unknown\"";
        } else if (yyjson_obj_get(memory, "missing")) {
            failure = "the upstream set was unreadable and `missing` carries a value — the "
                      "confident wrong answer: a count nobody computed";
        } else if (yyjson_obj_get(memory, "records_upstream") ||
                   yyjson_obj_get(memory, "checked_at") ||
                   yyjson_obj_get(memory, "missing_ids")) {
            failure = "the upstream set was unreadable and a key that means \"actually read\" "
                      "is present";
        } else if (!records_local || yyjson_get_uint(records_local) != 1U) {
            failure = "records_local must still be reported — the LOCAL set was readable";
        }
        yyjson_doc_free(seen);
    }
    hyp_record_set_free(local);
    surface_cache_end(&fx);
    if (!seeded) {
        FAIL("could not build the local record set");
    }
    if (failure) {
        FAIL(failure);
    }
    PASS();
}

/* ── 4e · §4 F3: checked-and-complete is `0`, a different sentence ─────
 *
 * Equal sets → `missing: 0`, PRESENT. Zero and absent must never be the same
 * bytes: 0 is "checked, nothing is missing", absent is "could not check". */
TEST(tool_surface_index_status_memory_missing_zero_when_sets_equal) {
    surface_cache_fixture_t fx;
    if (!surface_cache_begin(&fx)) {
        FAIL("could not create a fresh cache");
    }
    hyp_record_set_t *local = hyp_record_set_create();
    hyp_record_set_t *upstream = hyp_record_set_create();
    const hyp_record_t *first = surface_record("both sides hold this");
    const hyp_record_t *second = surface_record("and this");
    bool seeded = local && upstream && first && second &&
                  hyp_record_set_add(local, first, NULL) == HYP_RECORD_OK &&
                  hyp_record_set_add(local, second, NULL) == HYP_RECORD_OK &&
                  hyp_record_set_add(upstream, first, NULL) == HYP_RECORD_OK &&
                  hyp_record_set_add(upstream, second, NULL) == HYP_RECORD_OK;
    hyp_record_free(first);
    hyp_record_free(second);

    const char *failure = NULL;
    if (seeded) {
        hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
        hyp_mcp_server_set_memory_store(srv, local);
        hyp_mcp_server_set_memory_upstream(srv, "test-feed", surface_upstream_borrow, upstream);
        bool is_error = false;
        yyjson_doc *seen = surface_client_object_on(srv, "index_status", "{}", &is_error);
        hyp_mcp_server_free(srv);
        yyjson_val *memory =
            seen && !is_error ? yyjson_obj_get(yyjson_doc_get_root(seen), "memory") : NULL;
        yyjson_val *status = memory ? yyjson_obj_get(memory, "status") : NULL;
        const char *word = status && yyjson_is_str(status) ? yyjson_get_str(status) : NULL;
        yyjson_val *missing = memory ? yyjson_obj_get(memory, "missing") : NULL;
        yyjson_val *upstream_count = memory ? yyjson_obj_get(memory, "records_upstream") : NULL;
        yyjson_val *ids = memory ? yyjson_obj_get(memory, "missing_ids") : NULL;
        if (!memory) {
            failure = "no memory block on a configured server";
        } else if (!word || strcmp(word, "current") != 0) {
            failure = "equal sets and memory.status is not \"current\"";
        } else if (!missing || !yyjson_is_int(missing) || yyjson_get_int(missing) != 0) {
            failure = "equal sets and `missing` is not the PRESENT integer 0 — \"checked, "
                      "complete\" must be different bytes from \"could not check\"";
        } else if (!upstream_count || yyjson_get_uint(upstream_count) != 2U) {
            failure = "records_upstream must report the count that was actually read";
        } else if (!ids || !yyjson_is_arr(ids) || yyjson_arr_size(ids) != 0U) {
            failure = "equal sets and missing_ids is not the empty array — there are zero "
                      "names to name, which is an answer, not an omission";
        } else if (!yyjson_obj_get(memory, "checked_at")) {
            failure = "a compare ran and checked_at is absent";
        }
        yyjson_doc_free(seen);
    }
    hyp_record_set_free(local);
    hyp_record_set_free(upstream);
    surface_cache_end(&fx);
    if (!seeded) {
        FAIL("could not build the record sets");
    }
    if (failure) {
        FAIL(failure);
    }
    PASS();
}

/* ── 4f · §4 F3: N differ → the count PLUS the names ──────────────────
 *
 * `missing` is a set difference — upstream records this machine does not
 * hold — never a lag: the store is an id-keyed set with no order to be
 * "behind" in. Records only the local side holds are writes a push has not
 * delivered yet, and must not be counted. And the ids are named, because a
 * bare count is a number nobody can act on. */
TEST(tool_surface_index_status_memory_missing_counts_and_names_the_ids) {
    surface_cache_fixture_t fx;
    if (!surface_cache_begin(&fx)) {
        FAIL("could not create a fresh cache");
    }
    hyp_record_set_t *local = hyp_record_set_create();
    hyp_record_set_t *upstream = hyp_record_set_create();
    const hyp_record_t *shared = surface_record("both sides hold this");
    const hyp_record_t *theirs_a = surface_record("upstream-only decision A");
    const hyp_record_t *theirs_b = surface_record("upstream-only decision B");
    const hyp_record_t *ours = surface_record("local-only, not yet pushed");
    bool seeded = local && upstream && shared && theirs_a && theirs_b && ours &&
                  hyp_record_set_add(local, shared, NULL) == HYP_RECORD_OK &&
                  hyp_record_set_add(local, ours, NULL) == HYP_RECORD_OK &&
                  hyp_record_set_add(upstream, shared, NULL) == HYP_RECORD_OK &&
                  hyp_record_set_add(upstream, theirs_a, NULL) == HYP_RECORD_OK &&
                  hyp_record_set_add(upstream, theirs_b, NULL) == HYP_RECORD_OK;
    char id_a[HYP_RECORD_ID_LEN + 1] = "";
    char id_b[HYP_RECORD_ID_LEN + 1] = "";
    if (seeded) {
        snprintf(id_a, sizeof(id_a), "%s", theirs_a->id);
        snprintf(id_b, sizeof(id_b), "%s", theirs_b->id);
    }
    hyp_record_free(shared);
    hyp_record_free(theirs_a);
    hyp_record_free(theirs_b);
    hyp_record_free(ours);

    const char *failure = NULL;
    if (seeded) {
        hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
        hyp_mcp_server_set_memory_store(srv, local);
        hyp_mcp_server_set_memory_upstream(srv, "test-feed", surface_upstream_borrow, upstream);
        bool is_error = false;
        yyjson_doc *seen = surface_client_object_on(srv, "index_status", "{}", &is_error);
        hyp_mcp_server_free(srv);
        yyjson_val *memory =
            seen && !is_error ? yyjson_obj_get(yyjson_doc_get_root(seen), "memory") : NULL;
        yyjson_val *status = memory ? yyjson_obj_get(memory, "status") : NULL;
        const char *word = status && yyjson_is_str(status) ? yyjson_get_str(status) : NULL;
        yyjson_val *missing = memory ? yyjson_obj_get(memory, "missing") : NULL;
        yyjson_val *ids = memory ? yyjson_obj_get(memory, "missing_ids") : NULL;
        bool named_a = false;
        bool named_b = false;
        if (ids && yyjson_is_arr(ids)) {
            size_t index = 0U;
            size_t max = 0U;
            yyjson_val *id = NULL;
            yyjson_arr_foreach(ids, index, max, id) {
                const char *s = yyjson_is_str(id) ? yyjson_get_str(id) : NULL;
                named_a = named_a || (s && strcmp(s, id_a) == 0);
                named_b = named_b || (s && strcmp(s, id_b) == 0);
            }
        }
        if (!memory) {
            failure = "no memory block on a configured server";
        } else if (!word || strcmp(word, "incomplete") != 0) {
            failure = "records are missing and memory.status is not \"incomplete\"";
        } else if (!missing || yyjson_get_uint(missing) != 2U) {
            failure = "two upstream records are absent locally and `missing` is not 2 — either "
                      "the difference is wrong or the local-only record was counted, which "
                      "would be a lag reading of a set";
        } else if (!ids || !yyjson_is_arr(ids) || yyjson_arr_size(ids) != 2U || !named_a ||
                   !named_b) {
            failure = "`missing` counts 2 and missing_ids does not name exactly those two — "
                      "a count without the names is a number nobody can act on";
        } else if (yyjson_obj_get(memory, "missing_ids_withheld")) {
            failure = "nothing was withheld and missing_ids_withheld is present — a "
                      "truncation disclosure must say NOTHING when nothing was held back";
        }
        yyjson_doc_free(seen);
    }
    hyp_record_set_free(local);
    hyp_record_set_free(upstream);
    surface_cache_end(&fx);
    if (!seeded) {
        FAIL("could not build the record sets");
    }
    if (failure) {
        FAIL(failure);
    }
    PASS();
}

/* ── 4g · §4 F3: two freshnesses, SIBLINGS, on a real project ──────────
 *
 * With a project resolved the answer carries BOTH `code` and `memory`, never
 * merged into one verdict and neither derived from the other — and the
 * pre-F3 top-level keys stay present, because index_status is a live tool
 * and this change is additions only. */
TEST(tool_surface_index_status_reports_two_freshnesses_separately) {
    surface_cache_fixture_t fx;
    if (!surface_cache_begin(&fx)) {
        FAIL("could not create a fresh cache");
    }
    const char *project = "f3-freshness-probe";
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", fx.dir, project);
    hyp_store_t *setup = hyp_store_open_path(db_path);
    bool seeded =
        setup && hyp_store_upsert_project(setup, project, "/tmp/f3-freshness-probe") ==
                     HYP_STORE_OK;
    if (setup) {
        hyp_store_close(setup);
    }
    hyp_record_set_t *local = hyp_record_set_create();
    hyp_record_set_t *upstream = hyp_record_set_create();
    const hyp_record_t *theirs = surface_record("upstream-only decision");
    seeded = seeded && local && upstream && theirs &&
             hyp_record_set_add(upstream, theirs, NULL) == HYP_RECORD_OK;
    hyp_record_free(theirs);

    const char *failure = NULL;
    if (seeded) {
        char arguments[256];
        snprintf(arguments, sizeof(arguments), "{\"project\":\"%s\"}", project);
        hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
        hyp_mcp_server_set_memory_store(srv, local);
        hyp_mcp_server_set_memory_upstream(srv, "test-feed", surface_upstream_borrow, upstream);
        bool is_error = false;
        yyjson_doc *seen = surface_client_object_on(srv, "index_status", arguments, &is_error);
        hyp_mcp_server_free(srv);
        yyjson_val *root = seen && !is_error ? yyjson_doc_get_root(seen) : NULL;
        yyjson_val *code = root ? yyjson_obj_get(root, "code") : NULL;
        yyjson_val *memory = root ? yyjson_obj_get(root, "memory") : NULL;
        yyjson_val *top_status = root ? yyjson_obj_get(root, "status") : NULL;
        yyjson_val *code_status = code ? yyjson_obj_get(code, "status") : NULL;
        yyjson_val *mem_status = memory ? yyjson_obj_get(memory, "status") : NULL;
        const char *top = top_status && yyjson_is_str(top_status) ? yyjson_get_str(top_status)
                                                                  : NULL;
        const char *code_word =
            code_status && yyjson_is_str(code_status) ? yyjson_get_str(code_status) : NULL;
        const char *mem_word =
            mem_status && yyjson_is_str(mem_status) ? yyjson_get_str(mem_status) : NULL;
        if (!root) {
            failure = "index_status did not answer on a seeded project";
        } else if (!top || !yyjson_obj_get(root, "project") ||
                   !yyjson_obj_get(root, "nodes") || !yyjson_obj_get(root, "edges")) {
            failure = "a pre-F3 top-level key is gone — index_status is LIVE and this change "
                      "is additions only";
        } else if (!code || !memory) {
            failure = "the two freshnesses are not both present as siblings";
        } else if (!code_word || strcmp(code_word, top) != 0) {
            failure = "code.status does not restate the top-level status it exists to restate";
        } else if (!yyjson_obj_get(code, "indexed_at")) {
            failure = "the project row carries indexed_at and code does not report it";
        } else if (!mem_word || strcmp(mem_word, "incomplete") != 0 ||
                   !yyjson_obj_get(memory, "missing")) {
            failure = "code is fresh while memory is missing a record, and the answer does not "
                      "say BOTH — the case F3 exists for: an agent must not read code-current "
                      "as memory-current";
        }
        yyjson_doc_free(seen);
    }
    hyp_record_set_free(local);
    hyp_record_set_free(upstream);
    surface_cache_end(&fx);
    if (!seeded) {
        FAIL("could not seed the project and record sets");
    }
    if (failure) {
        FAIL(failure);
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
 * advertising destructiveHint and openWorldHint. The fold made a missing
 * annotation a compile error — and the test guarding it then checked only that
 * the four KEYS were present, which is not a claim about any hint. Inverting
 * readOnlyHint and idempotentHint on every row left it green.
 *
 * So the values are checked, and checked the only way that can catch an
 * emitter that disagrees with the contract: this file expands the SAME table
 * mcp.c expands, builds the expectation from it, and compares against what a
 * client reads back over tools/list. Nothing is enumerated by hand, so a row
 * added later is covered the day it is added.
 *
 * The one thing a table-derived expectation cannot catch is the table being
 * wrong, so the row that carries the meaning is ALSO pinned to literals below
 * — see tool_surface_erase_is_distinguishable_from_read. */
static const struct {
    bool read_only;
    bool destructive;
    bool idempotent;
    bool open_world;
} SURFACE_ANN_PROFILES[] = {
#define SURFACE_ANN_PROFILE_ROW(profile, ro, destr, idem, open) {ro, destr, idem, open},
    HYP_TOOL_ANNOTATION_PROFILES(SURFACE_ANN_PROFILE_ROW)
#undef SURFACE_ANN_PROFILE_ROW
};

static const struct {
    const char *name;
    hyp_tool_annotation_profile_t annotations;
} SURFACE_TOOL_ANN[] = {
#define SURFACE_TOOL_ANN_ROW(name, alias, analysis, scout, generation, status, output_schema, ann) \
    {name, ann},
    HYP_TOOL_SURFACE(SURFACE_TOOL_ANN_ROW)
#undef SURFACE_TOOL_ANN_ROW
};

/* The four hints one advertised tool carries, as a client holds them. False in
 * `found` means the tool was not advertised at all. */
typedef struct {
    bool found;
    bool read_only;
    bool destructive;
    bool idempotent;
    bool open_world;
} surface_hints_t;

static surface_hints_t surface_hints_of(yyjson_val *tools, const char *tool) {
    surface_hints_t out;
    memset(&out, 0, sizeof(out));
    size_t index = 0U;
    size_t max = 0U;
    yyjson_val *entry = NULL;
    yyjson_arr_foreach(tools, index, max, entry) {
        yyjson_val *name = yyjson_obj_get(entry, "name");
        if (!name || !yyjson_is_str(name) || strcmp(yyjson_get_str(name), tool) != 0) {
            continue;
        }
        yyjson_val *ann = yyjson_obj_get(entry, "annotations");
        if (!ann || !yyjson_is_obj(ann)) {
            return out;
        }
        yyjson_val *ro = yyjson_obj_get(ann, "readOnlyHint");
        yyjson_val *de = yyjson_obj_get(ann, "destructiveHint");
        yyjson_val *id = yyjson_obj_get(ann, "idempotentHint");
        yyjson_val *ow = yyjson_obj_get(ann, "openWorldHint");
        if (!ro || !de || !id || !ow || !yyjson_is_bool(ro) || !yyjson_is_bool(de) ||
            !yyjson_is_bool(id) || !yyjson_is_bool(ow)) {
            return out;
        }
        out.found = true;
        out.read_only = yyjson_is_true(ro);
        out.destructive = yyjson_is_true(de);
        out.idempotent = yyjson_is_true(id);
        out.open_world = yyjson_is_true(ow);
        return out;
    }
    return out;
}

TEST(tool_surface_advertised_hints_equal_the_declared_profile) {
    char *resp = surface_call(HYP_MCP_TOOL_PROFILE_ALL, "tools/list", NULL);
    ASSERT_NOT_NULL(resp);
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(doc), "result");
    yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
    ASSERT_NOT_NULL(tools);

    const char *failure = NULL;
    size_t checked = 0U;
    for (size_t i = 0U; !failure && i < sizeof(SURFACE_TOOL_ANN) / sizeof(SURFACE_TOOL_ANN[0]);
         i++) {
        const char *name = SURFACE_TOOL_ANN[i].name;
        bool advertised = hyp_mcp_tool_surface_status(name) != (int)HYP_TOOL_RESERVED;
        surface_hints_t seen = surface_hints_of(tools, name);
        if (!advertised) {
            if (seen.found) {
                failure = "a reserved row is advertised with annotations";
            }
            continue;
        }
        if (!seen.found) {
            failure = "an advertised tool carries no complete boolean annotation set";
            continue;
        }
        checked++;
        const size_t p = (size_t)SURFACE_TOOL_ANN[i].annotations;
        if (seen.read_only != SURFACE_ANN_PROFILES[p].read_only ||
            seen.destructive != SURFACE_ANN_PROFILES[p].destructive ||
            seen.idempotent != SURFACE_ANN_PROFILES[p].idempotent ||
            seen.open_world != SURFACE_ANN_PROFILES[p].open_world) {
            failure = "an advertised tool's hints differ from the annotation profile its row "
                      "declares";
        }
        /* Two derived invariants that hold whatever the table says, so a wrong
         * table is caught here too: a read-only tool cannot destroy, and every
         * shipped tool is local. */
        if (!failure && seen.read_only && seen.destructive) {
            failure = "a tool advertises readOnlyHint and destructiveHint together";
        }
        if (!failure && seen.open_world) {
            failure = "an advertised tool claims openWorldHint — every shipped tool is local";
        }
    }
    yyjson_doc_free(doc);
    free(resp);
    if (failure) {
        FAIL(failure);
    }
    ASSERT_EQ(checked, (size_t)hyp_mcp_tool_count());
    PASS();
}

/* ── 6b · A client can tell the tool that erases from the tool that reads ──
 *
 * `delete_project` and `search_graph` shipped advertising byte-identical
 * hints. Both said destructiveHint=true — one truthfully, one not — so a
 * client that confirms before a destructive call either confirmed before every
 * graph search or confirmed before none, and in neither case was it protecting
 * the database. The literals here are deliberate: this is the one row whose
 * meaning a table-derived check cannot defend, because inverting the table
 * moves the expectation with it. */
TEST(tool_surface_erase_is_distinguishable_from_read) {
    char *resp = surface_call(HYP_MCP_TOOL_PROFILE_ALL, "tools/list", NULL);
    ASSERT_NOT_NULL(resp);
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(doc), "result");
    yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
    ASSERT_NOT_NULL(tools);

    surface_hints_t erase = surface_hints_of(tools, "delete_project");
    surface_hints_t read = surface_hints_of(tools, "search_graph");
    yyjson_doc_free(doc);
    free(resp);

    if (!erase.found || !read.found) {
        FAIL("delete_project and search_graph must both be advertised with full annotations");
    }
    /* Differ at all — the property the client needs. */
    if (erase.read_only == read.read_only && erase.destructive == read.destructive &&
        erase.idempotent == read.idempotent && erase.open_world == read.open_world) {
        FAIL("delete_project and search_graph advertise identical hints: a client cannot tell "
             "the tool that erases a database from the tool that reads it");
    }
    /* And differ the RIGHT way, so the fix cannot be satisfied by moving any
     * bit at all. */
    ASSERT_TRUE(erase.destructive);
    ASSERT_FALSE(read.destructive);
    ASSERT_FALSE(erase.read_only);
    ASSERT_FALSE(read.read_only);
    PASS();
}

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

/* ── 6c · The memory surface is reachable, and fails closed ────────────
 *
 * The mechanical half of §4 C8u. `record_memory` and `search_memory` were
 * RESERVED rows — published signatures, dispatched nowhere — and this asserts
 * what a client gets now that they are not: a write that comes back with an
 * id, the same record read back by the reader, and the two refusals that must
 * stay refusals. A transcript kind accepted here would make the ingest
 * completeness audit meaningless, and an anchor accepted here would create a
 * record attached to a span nothing verified.
 *
 * The OTHER half — whether an agent actually calls it — cannot be asserted in
 * a unit test at all, and is measured against a real client instead. */
typedef struct {
    char dir[256];
    char *saved;
} surface_memory_fixture_t;

static bool surface_memory_begin(surface_memory_fixture_t *fx) {
    snprintf(fx->dir, sizeof(fx->dir), "/tmp/hyp-tool-surface-mem-XXXXXX");
    if (!hyp_mkdtemp(fx->dir)) {
        return false;
    }
    const char *saved = getenv("HYP_MEMORY_DIR");
    fx->saved = saved ? strdup(saved) : NULL;
    hyp_setenv("HYP_MEMORY_DIR", fx->dir, 1);
    return true;
}

static void surface_memory_end(surface_memory_fixture_t *fx) {
    if (fx->saved) {
        hyp_setenv("HYP_MEMORY_DIR", fx->saved, 1);
        free(fx->saved);
        fx->saved = NULL;
    } else {
        hyp_unsetenv("HYP_MEMORY_DIR");
    }
}

/* One server for the whole exchange: a write and the read that must see it are
 * the two ends this test exists to hold together. */
static const char *surface_memory_text(hyp_mcp_server_t *srv, const char *tool, const char *args,
                                       char *out, size_t cap, bool *is_error) {
    char *resp = hyp_mcp_handle_tool(srv, tool, args);
    out[0] = '\0';
    if (is_error) {
        *is_error = false;
    }
    if (!resp) {
        return out;
    }
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (is_error && root) {
        yyjson_val *err = yyjson_obj_get(root, "isError");
        *is_error = err && yyjson_is_true(err);
    }
    yyjson_val *content = root ? yyjson_obj_get(root, "content") : NULL;
    yyjson_val *first = content && yyjson_is_arr(content) ? yyjson_arr_get_first(content) : NULL;
    yyjson_val *text = first ? yyjson_obj_get(first, "text") : NULL;
    if (text && yyjson_is_str(text)) {
        snprintf(out, cap, "%s", yyjson_get_str(text));
    }
    yyjson_doc_free(doc);
    free(resp);
    return out;
}

TEST(tool_surface_memory_surface_is_live_and_fails_closed) {
    surface_memory_fixture_t fx;
    if (!surface_memory_begin(&fx)) {
        FAIL("could not create a temporary memory store directory");
    }
    ASSERT_EQ(hyp_mcp_tool_surface_status("record_memory"), (int)HYP_TOOL_LIVE);
    ASSERT_EQ(hyp_mcp_tool_surface_status("search_memory"), (int)HYP_TOOL_LIVE);

    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    if (!srv) {
        surface_memory_end(&fx);
        FAIL("no server");
    }
    char text[8192];
    bool is_error = false;

    /* A transcript kind is refused, and the refusal NAMES the accepted set —
     * an error that does not say what would work costs a turn to learn. */
    surface_memory_text(srv, "record_memory",
                        "{\"kind\":\"transcript\",\"title\":\"t\",\"body\":\"b\"}", text,
                        sizeof(text), &is_error);
    bool refused_transcript = is_error && strstr(text, "decision") && strstr(text, "verdict");
    /* A supplied anchor is refused rather than stored unverified. */
    surface_memory_text(srv, "record_memory",
                        "{\"kind\":\"decision\",\"title\":\"t\",\"body\":\"b\","
                        "\"anchor\":\"hyp1:w/r#foo\"}",
                        text, sizeof(text), &is_error);
    bool refused_anchor = is_error && strstr(text, "anchor") != NULL;

    /* The write a client actually makes. */
    surface_memory_text(srv, "record_memory",
                        "{\"kind\":\"decision\",\"title\":\"Chose the id-keyed set\","
                        "\"body\":\"A log has a machine-local order and the union has none.\","
                        "\"tags\":[\"store\"]}",
                        text, sizeof(text), &is_error);
    bool wrote = !is_error && strstr(text, "\"id\"") && strstr(text, "\"anchor_status\"") &&
                 strstr(text, "unanchored") && strstr(text, "\"written_at\"");

    /* And the reader sees it — the two ends of one store, in one exchange.
     * `tags` are searchable because they are part of what was said: the record
     * shape is frozen and has no tag field, so they join the text rather than
     * being silently dropped. */
    surface_memory_text(srv, "search_memory", "{\"format\":\"json\",\"query\":\"id-keyed\"}", text,
                        sizeof(text), &is_error);
    bool read_back = !is_error && strstr(text, "Chose the id-keyed set") &&
                     strstr(text, "\"matched\":1") && strstr(text, "store");
    /* A filter this build cannot compute is refused, never ignored: an ignored
     * filter returns a superset that reads exactly like a match. */
    surface_memory_text(srv, "search_memory", "{\"status\":\"orphaned\"}", text, sizeof(text),
                        &is_error);
    bool refused_status = is_error && strstr(text, "orphaned") != NULL;

    hyp_mcp_server_free(srv);
    surface_memory_end(&fx);

    if (!refused_transcript) {
        FAIL("record_memory accepted a transcript kind, or refused it without naming the "
             "kinds it accepts");
    }
    if (!refused_anchor) {
        FAIL("record_memory accepted an anchor nothing in this build can resolve");
    }
    if (!wrote) {
        FAIL("record_memory is live and a client gets no id back");
    }
    if (!read_back) {
        FAIL("search_memory cannot see what record_memory just wrote");
    }
    if (!refused_status) {
        FAIL("search_memory silently ignored a status filter it cannot compute");
    }
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
    RUN_TEST(tool_surface_index_status_no_project_answer_reaches_the_client);
    RUN_TEST(tool_surface_index_status_memory_absent_without_a_record_store);
    RUN_TEST(tool_surface_index_status_memory_unknown_when_upstream_unreadable);
    RUN_TEST(tool_surface_index_status_memory_missing_zero_when_sets_equal);
    RUN_TEST(tool_surface_index_status_memory_missing_counts_and_names_the_ids);
    RUN_TEST(tool_surface_index_status_reports_two_freshnesses_separately);
    RUN_TEST(tool_surface_every_advertised_tool_dispatches);
    RUN_TEST(tool_surface_declared_aliases_dispatch_and_are_never_advertised);
    RUN_TEST(tool_surface_advertised_hints_equal_the_declared_profile);
    RUN_TEST(tool_surface_erase_is_distinguishable_from_read);
    RUN_TEST(tool_surface_memory_surface_is_live_and_fails_closed);
    RUN_TEST(tool_surface_every_advertised_tool_carries_complete_annotations);
    RUN_TEST(tool_surface_transcript_kinds_are_not_authorable);
    RUN_TEST(tool_surface_no_reserved_surface_depends_on_the_deprecated_tool);
}
