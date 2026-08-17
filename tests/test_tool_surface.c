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
#include <foundation/compat_fs.h>
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

/* The client's object for one call against a CONFIGURED server. The memory
 * freshness tests attach record sets to the server first, which surface_call's
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

/* ── Freshness fixtures ────────────────────────────────────────────────
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
     * vacuously. The table publishes four; if that number goes to zero the
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
 * THE PHASE 0 DEAD BRANCH, made live by the freshness work — this test held
 * the finding and has flipped to holding the fix. handle_index_status
 * documented a structured answer for "no project resolved"
 * ({"status":"no_project"}) that was UNREACHABLE from the day it shipped: the
 * branch needed store != NULL with project == NULL, and resolve_store returns
 * NULL the moment project is NULL, so REQUIRE_STORE answered isError prose
 * first, every time. The fix answers before touching the store. This asserts
 * the CLIENT's view of it — a non-error object saying status=no_project —
 * with a fresh empty cache so "nothing resolves" is a property of the fixture,
 * not of suite order. scripts/ci/mcp-client-view.py asserts the same thing
 * against a real stdio subprocess in an unindexed directory. */
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

/* ── 4c · memory absent means "no store", never "up to date" ───────────
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

/* ── 4d · unknowable is `unknown` + ABSENT, never a number ─────────────
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

/* ── 4e · checked-and-complete is `0`, a different sentence ────────────
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

/* ── 4f · N differ → the count PLUS the names ─────────────────────────
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

/* ── 4g · two freshnesses, SIBLINGS, on a real project ─────────────────
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
 * The mechanical half of reachability. `record_memory` and `search_memory` were
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
 * Transcripts enter only through a feed. The ingest audit checks completeness
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
 * last-writer-wins, which is precisely what the append-only record contract
 * forbids. It stays live and unchanged on the wire; what must not happen is a
 * new surface being built on top of it, which would make the migration a
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

/* The other half of that row, and the one C7u makes assertable. The ADR
 * document is folded into the append-only record set, so the deprecated tool
 * is a PRODUCER for the memory surface and never something the memory surface
 * consults. Two ways that could stop being true, and both are checkable from
 * the client's side of the wire:
 *
 *   - the deprecated tool grows a memory argument, which would make an ADR
 *     write a second way to author a record;
 *   - a memory tool grows ADR vocabulary, which would make the record surface
 *     need to know what an ADR is in order to read one.
 *
 * Either one is a new surface depending on the deprecated row. */
TEST(tool_surface_the_deprecated_adr_tool_shares_no_vocabulary_with_the_memory_surface) {
    const char *adr_schema = hyp_mcp_tool_input_schema("manage_adr");
    ASSERT_NOT_NULL(adr_schema); /* advertised and callable, unchanged on the wire */
    ASSERT_NOT_NULL(strstr(adr_schema, "\"mode\""));
    ASSERT_NOT_NULL(strstr(adr_schema, "\"content\""));

    /* No memory-surface argument arrived on the deprecated tool. record_memory
     * is the one writer; an ADR update reaching the record set through
     * manage_adr's own arguments would be a second one. */
    ASSERT_NULL(strstr(adr_schema, "\"kind\""));
    ASSERT_NULL(strstr(adr_schema, "\"anchor\""));
    ASSERT_NULL(strstr(adr_schema, "\"origin\""));
    ASSERT_NULL(strstr(adr_schema, "\"thread\""));
    ASSERT_NULL(strstr(adr_schema, "\"parent\""));
    /* `sections` survives as a READ mode and must never return as an argument:
     * an argument that edits part of a document is a mutation primitive, and
     * there is no such thing on this path. The mode set is exactly the three
     * it has always been, so no new mode arrived either. */
    ASSERT_NOT_NULL(strstr(adr_schema, "\"enum\":[\"get\",\"update\",\"sections\"]"));
    ASSERT_NULL(strstr(adr_schema, "\"sections\":{"));

    /* Nothing else advertised speaks of ADRs. The record set holds the folded
     * documents as ordinary decision records, so a reader needs no ADR
     * vocabulary to find one — which is exactly what "no new surface depends
     * on it" has to mean at the data layer. */
    for (int i = 0; i < hyp_mcp_tool_surface_count(); i++) {
        const char *name = hyp_mcp_tool_surface_name(i);
        if (strcmp(name, "manage_adr") == 0) {
            continue;
        }
        const char *schema = hyp_mcp_tool_input_schema(name);
        if (!schema) {
            continue;
        }
        if (strstr(schema, "adr") || strstr(schema, "ADR")) {
            FAIL("a tool other than manage_adr advertises ADR vocabulary");
        }
    }
    PASS();
}

/* ── G6 check C · every advertised property is one a handler takes ─────
 *
 * THE THIRD SURFACE OF "the repository asserts something no execution has
 * touched". A module with no caller is inert; a command absent from help is
 * unfindable; and an ARGUMENT IN A SCHEMA THAT NO CALL CAN USE is worse than
 * either, because a client generated from the schema sends it and is refused
 * for obeying the contract it was handed.
 *
 * WHAT IS ASSERTED, and it is two things:
 *
 *   (1) EVERY advertised property has at least one value the handler takes.
 *       An advertised argument no call can ever use is a published promise
 *       with nothing behind it.
 *   (2) A property that DECLARES A DEFAULT is held to that exact value. A
 *       generated client sends the default without being asked, so a refused
 *       default fails a caller who did nothing but obey the schema.
 *
 * (1) is the larger property and (2) is not inside it: a handler can accept
 * one value of an argument and refuse the value its own schema declares, and
 * that passes (1) while failing every generated client. Both ship, because a
 * check narrower than its name is the failure this walk exists to close.
 *
 * DERIVED, NOT LISTED, at three levels. The tools are the ones tools/list
 * advertises; the properties are the ones each advertised inputSchema
 * declares; and the tool set is narrowed by the ANNOTATIONS the same response
 * carries — a row that says openWorldHint leaves this machine, and everything
 * else is contained by the cache directory and the record store this fixture
 * owns. Nothing here names a tool or a property, so a tool added tomorrow is
 * in the set without this file learning its name.
 *
 * A SEEDED PROJECT, because the alternative measures nothing. With no project
 * to resolve, most of this surface refuses every call for a reason that has
 * nothing to do with any argument, and a probe whose baseline is already an
 * error can conclude nothing about the argument it added. The fixture seeds
 * exactly ONE project into a fresh cache, so the server's own resolution rule
 * — the single indexed project, when it is single — supplies it to every call
 * without the walk naming `project` anywhere.
 *
 * THE VALUES A WALK CAN TRY come from three places and no fourth: what the
 * schema declares (its default, its enum, its type), what this fixture makes
 * true (the seeded project's name and its root path), and WHAT THE TOOL
 * ITSELF SAID — the answer it gave its own base call, the refusal the property
 * under test just drew, and every answer it has given this walk so far. That
 * last source is the one that does the work, and it is not a trick: this
 * surface's refusals name the accepted set and its answers carry the values
 * its own arguments take, so a walk that reads them is using the contract
 * rather than guessing at it. A record id out of one answer is the value
 * `supersedes` wants; a refusal listing the kinds it writes hands the walk the
 * kind it needs; and a resume token exists at all only because the walk asked
 * for a page small enough to truncate.
 *
 * ATTRIBUTED BY DIFFERENCE, so a refusal for an unrelated reason cannot be
 * blamed on an argument. The base call for a tool and the call with one
 * property added differ in exactly that property, and the fixture is restored
 * between them, so nothing else moved. A FINDING THEREFORE REQUIRES A BASE
 * CALL THAT ANSWERED: where the tool refuses its own base call, a refusal of
 * the property is a refusal of something else just as easily, and every
 * property of that tool is counted and named rather than blamed.
 *
 * AND SEPARATED FROM A REFUSAL OF THE VALUE, which is the distinction this
 * check would be worthless without. `since` demanding a timestamp and `kind`
 * demanding a known kind are properties a caller CAN use; they refuse a value
 * this walk failed to guess, not the argument. The tell is whether the
 * refusal MOVES WITH THE VALUE: two different values drawing two different
 * refusals is a handler reading the value, and the property is reported as
 * unattributable rather than flagged. A value-independent refusal that no
 * value escapes is the finding — the argument itself is what is unsupported.
 * The price of that separation, stated because a stated hole is not a silent
 * one: an unconditional refusal that ECHOES THE VALUE it refused is invisible
 * here. It moves with the value, so it reads exactly like a handler validating
 * input, and nothing outside the handler tells the two apart. A check that
 * guessed between them would report findings it cannot support.
 *
 * READ THE REACH BEFORE READING THE COUNT. The summary prints OBSERVABLE
 * beside the total and NAMES every property, judged or not, because 0
 * findings is 0 among the properties this walk can attribute, never 0 among
 * every property in the tree. A gate that lets its clean result be read as
 * full coverage is the next silent failure.
 *
 * WHAT IT STILL CANNOT SEE, stated because a stated hole is not a silent one:
 * a property whose only legal values are ones neither the schema, this
 * fixture nor the tool's own words contain; a property on a tool whose base
 * call cannot be made to answer at all; and any tool that leaves this
 * machine, which no fixture can contain. Each of those is counted, named and
 * printed on every run, pass or fail.
 */

enum {
    PROBE_TEXT_CAP = 24576, /* one refusal or answer, for comparison */
    PROBE_VALUE_CAP = 256,  /* one JSON-encoded argument value */
    PROBE_KEY_CAP = 48,     /* one property name */
    PROBE_PAIR_CAP = 16,    /* arguments in one base call */
    PROBE_CAND_CAP = 80,    /* values tried for one property */
    PROBE_TOKEN_CAP = 96,   /* one borrowed word */
    PROBE_WORDS_CAP = 512,  /* words kept from what one tool has said */
    PROBE_TOOL_CAP = 32,    /* advertised tools */
    PROBE_REPAIR_CAP = 40,  /* calls spent making one base call answer */
    PROBE_BORROW_CAP = 20,  /* words borrowed from one thing it said */
    PROBE_ARGS_CAP = 6144   /* one rendered arguments object */
};

/* Where a probed call's writes land, so "contained" is also restorable. */
static char probe_cache_dir[256];
static char probe_root_dir[256];
static char probe_db_path[512];
static char probe_project_json[80];
static char probe_root_json[320];
static int probe_calls;
static int probe_unanswered;

#define PROBE_PROJECT "hyp-g6-probe"

/* One project, one symbol, and one CALL each way, seeded so the surface has
 * something to answer ABOUT.
 *
 * The symbol is called `probe` because that word is in the walk's own value
 * battery: a fixture whose one symbol no derived value can name leaves every
 * symbol-taking tool unreachable, which is a hole in the reach report rather
 * than a fact about the surface. It calls MORE THINGS THAN ONE PAGE HOLDS on
 * purpose, and that is the load-bearing part: a tool mints a resume token only
 * when it truncates, so an argument whose only legal values the tool itself
 * issues is unexercisable until the fixture makes it issue one. The walk
 * varies ONE property per call, which is what makes attribution possible and
 * also what makes a value needing two properties at once out of reach — so the
 * fixture, not the walk, is where that is fixed.
 *
 * Idempotent by inspection, not by hope: edges INSERT rather than upsert, so
 * re-seeding blindly before every call would grow the graph under the walk and
 * make one probe's answer depend on how many probes came before it. */
static bool probe_seed(void) {
    hyp_store_t *store = hyp_store_open_path(probe_db_path);
    if (!store) {
        return false;
    }
    hyp_node_t existing;
    memset(&existing, 0, sizeof(existing));
    if (hyp_store_find_node_by_qn(store, PROBE_PROJECT, "probe", &existing) == HYP_STORE_OK) {
        hyp_node_free_fields(&existing);
        hyp_store_close(store);
        return true;
    }
    bool ok = hyp_store_upsert_project(store, PROBE_PROJECT, probe_root_dir) == HYP_STORE_OK;
    if (ok) {
        enum { PROBE_CALLEES = 140 };
        int64_t root = 0;
        for (int i = 0; i <= PROBE_CALLEES; i++) {
            char named[64];
            if (i == 0) {
                snprintf(named, sizeof(named), "probe");
            } else {
                snprintf(named, sizeof(named), "probe_callee_%03d", i);
            }
            hyp_node_t node;
            memset(&node, 0, sizeof(node));
            node.project = PROBE_PROJECT;
            node.label = "Function";
            node.name = named;
            node.qualified_name = named;
            node.file_path = "probe.c";
            node.start_line = i + 1;
            node.end_line = i + 1;
            int64_t id = hyp_store_upsert_node(store, &node);
            if (i == 0) {
                root = id;
                continue;
            }
            hyp_edge_t edge;
            memset(&edge, 0, sizeof(edge));
            edge.project = PROBE_PROJECT;
            edge.source_id = root;
            edge.target_id = id;
            edge.type = "CALLS";
            (void)hyp_store_insert_edge(store, &edge);
        }
    }
    hyp_store_close(store);
    return ok;
}

/* Put the fixture back to one project before every call. Two calls that differ
 * only in the property under test must differ in nothing else, and a tool that
 * deletes or creates a project would otherwise change what the NEXT call
 * resolves — which would attribute one tool's writes to another tool's
 * argument. */
static void probe_reset(void) {
    char doomed[8][256];
    int doomed_count = 0;
    hyp_dir_t *dir = hyp_opendir(probe_cache_dir);
    if (dir) {
        hyp_dirent_t *entry = NULL;
        while ((entry = hyp_readdir(dir)) != NULL && doomed_count < 8) {
            size_t len = strlen(entry->name);
            if (entry->is_dir || len < 4U || strcmp(entry->name + len - 3, ".db") != 0) {
                continue;
            }
            if (strcmp(entry->name, PROBE_PROJECT ".db") == 0) {
                continue;
            }
            snprintf(doomed[doomed_count], sizeof(doomed[0]), "%s/%s", probe_cache_dir,
                     entry->name);
            doomed_count++;
        }
        hyp_closedir(dir);
    }
    for (int i = 0; i < doomed_count; i++) {
        hyp_remove_db_sidecars(doomed[i]);
        hyp_unlink(doomed[i]);
    }
    (void)probe_seed();
}

/* One call, read the way a CLIENT reads it: the `isError` it branches on and
 * the text it renders. Never the emitted shape — four tests once asserted a
 * broken MCP output because they checked what the server sent. */
static bool probe_call(const char *tool, const char *args, char *text, size_t cap) {
    probe_reset();
    probe_calls++;
    if (text && cap) {
        text[0] = '\0';
    }
    hyp_mcp_server_t *srv = hyp_mcp_server_new(NULL);
    if (!srv) {
        probe_unanswered++;
        return true;
    }
    /* No update checks and no session auto-index: a probe must not index the
     * directory the test runner happens to be standing in. */
    hyp_mcp_server_set_background_tasks(srv, false);
    char *resp = hyp_mcp_handle_tool(srv, tool, args);
    hyp_mcp_server_free(srv);
    if (!resp) {
        probe_unanswered++;
        return true;
    }
    yyjson_doc *doc = yyjson_read(resp, strlen(resp), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root) {
        probe_unanswered++;
    }
    yyjson_val *err = root ? yyjson_obj_get(root, "isError") : NULL;
    bool is_error = err != NULL && yyjson_is_true(err);
    if (text && cap) {
        yyjson_val *content = root ? yyjson_obj_get(root, "content") : NULL;
        yyjson_val *first = content && yyjson_is_arr(content) ? yyjson_arr_get_first(content) : NULL;
        yyjson_val *body = first ? yyjson_obj_get(first, "text") : NULL;
        if (body && yyjson_is_str(body)) {
            snprintf(text, cap, "%s", yyjson_get_str(body));
        }
    }
    yyjson_doc_free(doc);
    free(resp);
    return is_error;
}

/* ── The words a tool says, kept as candidate values ───────────────────
 *
 * Restricted to characters that need no JSON escaping, so a borrowed word is
 * a legal argument by construction rather than by a quoting routine nobody
 * would test. */
typedef struct {
    char item[PROBE_WORDS_CAP][PROBE_TOKEN_CAP];
    int count;
} probe_words_t;

static bool probe_token_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-' || c == ':' || c == '/' || c == '+' || c == '@';
}

static void probe_words_add(probe_words_t *set, const char *token) {
    size_t len = token ? strlen(token) : 0U;
    if (len < 2U || len >= PROBE_TOKEN_CAP || set->count >= PROBE_WORDS_CAP) {
        return;
    }
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->item[i], token) == 0) {
            return;
        }
    }
    snprintf(set->item[set->count], PROBE_TOKEN_CAP, "%s", token);
    set->count++;
}

static void probe_words_absorb(probe_words_t *set, const char *text) {
    if (!text) {
        return;
    }
    size_t i = 0U;
    while (text[i] != '\0') {
        if (!probe_token_char(text[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (text[j] != '\0' && probe_token_char(text[j])) {
            j++;
        }
        size_t len = j - i;
        if (len >= 2U && len < PROBE_TOKEN_CAP) {
            char token[PROBE_TOKEN_CAP];
            memcpy(token, text + i, len);
            token[len] = '\0';
            probe_words_add(set, token);
        }
        i = j;
    }
}

static void probe_words_scan(probe_words_t *set, const char *text) {
    set->count = 0;
    probe_words_absorb(set, text);
}

/* ── One JSON value of a declared shape ────────────────────────────────
 *
 * `variant` walks the values this walk knows how to build; false ends the
 * walk. An enum is walked member by member and nothing else is offered: a
 * handler that refuses every member of its own enum is stating the argument
 * is unusable, which is exactly the finding. */
static bool probe_scalar(const char *type, int variant, char *out, size_t cap) {
    if (type && (strcmp(type, "integer") == 0 || strcmp(type, "number") == 0)) {
        if (variant == 0) {
            snprintf(out, cap, "1");
            return true;
        }
        if (variant == 1) {
            snprintf(out, cap, "2");
            return true;
        }
        return false;
    }
    if (type && strcmp(type, "boolean") == 0) {
        if (variant == 0) {
            snprintf(out, cap, "false");
            return true;
        }
        if (variant == 1) {
            snprintf(out, cap, "true");
            return true;
        }
        return false;
    }
    if (variant == 0) {
        /* The name the fixture's own symbol carries, FIRST. A base call is
         * built from each property's first value, so the first string decides
         * whether a tool that looks a symbol up finds one — and a tool whose
         * base answers about nothing answers every later probe about nothing
         * too. */
        snprintf(out, cap, "\"probe\"");
        return true;
    }
    if (variant == 1) {
        snprintf(out, cap, "%s", probe_project_json);
        return true;
    }
    if (variant == 2) {
        snprintf(out, cap, "%s", probe_root_json);
        return true;
    }
    if (variant == 3) {
        snprintf(out, cap, "\"hyponoia probe\"");
        return true;
    }
    if (variant == 4) {
        /* A shape, not a word. A handler can require uppercase-and-underscores
         * of an argument whose schema declares only `string`, and a walk that
         * only ever sends lowercase would call that argument unusable. */
        snprintf(out, cap, "\"PROBE\"");
        return true;
    }
    return false;
}

static bool probe_flat_value(yyjson_val *spec, int variant, char *out, size_t cap) {
    yyjson_val *type_val = spec ? yyjson_obj_get(spec, "type") : NULL;
    const char *type = type_val && yyjson_is_str(type_val) ? yyjson_get_str(type_val) : NULL;
    yyjson_val *choices = spec ? yyjson_obj_get(spec, "enum") : NULL;
    if (choices && yyjson_is_arr(choices)) {
        yyjson_val *member = yyjson_arr_get(choices, (size_t)variant);
        if (!member) {
            return false;
        }
        char *rendered = yyjson_val_write(member, 0, NULL);
        if (!rendered) {
            return false;
        }
        snprintf(out, cap, "%s", rendered);
        free(rendered);
        return true;
    }
    yyjson_val *props = spec ? yyjson_obj_get(spec, "properties") : NULL;
    if (type && strcmp(type, "object") == 0 && props && yyjson_is_obj(props)) {
        if (variant > 1) {
            return false;
        }
        char body[PROBE_VALUE_CAP];
        size_t used = 0U;
        body[0] = '\0';
        yyjson_obj_iter it;
        yyjson_obj_iter_init(props, &it);
        yyjson_val *key = NULL;
        int written = 0;
        while ((key = yyjson_obj_iter_next(&it)) != NULL && written < 6) {
            if (used + 8U >= sizeof(body)) {
                break;
            }
            yyjson_val *sub = yyjson_obj_iter_get_val(key);
            yyjson_val *sub_type = yyjson_is_obj(sub) ? yyjson_obj_get(sub, "type") : NULL;
            const char *sub_name =
                sub_type && yyjson_is_str(sub_type) ? yyjson_get_str(sub_type) : NULL;
            char value[PROBE_VALUE_CAP];
            if (!probe_scalar(sub_name, variant, value, sizeof(value))) {
                continue;
            }
            int wrote = snprintf(body + used, sizeof(body) - used, "%s\"%s\":%s",
                                 written ? "," : "", yyjson_get_str(key), value);
            if (wrote < 0 || (size_t)wrote >= sizeof(body) - used) {
                break;
            }
            used += (size_t)wrote;
            written++;
        }
        snprintf(out, cap, "{%s}", body);
        return true;
    }
    if (type && strcmp(type, "object") == 0) {
        if (variant > 0) {
            return false;
        }
        snprintf(out, cap, "{\"probe\":\"probe\"}");
        return true;
    }
    return probe_scalar(type, variant, out, cap);
}

static bool probe_value_for(yyjson_val *spec, int variant, char *out, size_t cap) {
    yyjson_val *type_val = spec ? yyjson_obj_get(spec, "type") : NULL;
    const char *type = type_val && yyjson_is_str(type_val) ? yyjson_get_str(type_val) : NULL;
    if (type && strcmp(type, "array") == 0) {
        char inner[PROBE_VALUE_CAP];
        if (!probe_flat_value(spec ? yyjson_obj_get(spec, "items") : NULL, variant, inner,
                              sizeof(inner))) {
            return false;
        }
        snprintf(out, cap, "[%s]", inner);
        return true;
    }
    return probe_flat_value(spec, variant, out, cap);
}

typedef struct {
    char text[PROBE_VALUE_CAP];
} probe_value_t;

static void probe_cand_push(probe_value_t *out, int cap, int *count, const char *json) {
    if (!json || json[0] == '\0' || *count >= cap) {
        return;
    }
    for (int i = 0; i < *count; i++) {
        if (strcmp(out[i].text, json) == 0) {
            return;
        }
    }
    snprintf(out[*count].text, PROBE_VALUE_CAP, "%s", json);
    (*count)++;
}

/* The declared default FIRST, always: it is the value a generated client sends
 * without being asked, so it is the one whose refusal is a finding on its own.
 * Then the shapes the declared type admits. */
static int probe_candidates(yyjson_val *spec, probe_value_t *out, int cap) {
    int count = 0;
    yyjson_val *fallback = spec ? yyjson_obj_get(spec, "default") : NULL;
    if (fallback) {
        char *rendered = yyjson_val_write(fallback, 0, NULL);
        if (rendered) {
            probe_cand_push(out, cap, &count, rendered);
            free(rendered);
        }
    }
    for (int variant = 0; variant < 8; variant++) {
        char buf[PROBE_VALUE_CAP];
        if (!probe_value_for(spec, variant, buf, sizeof(buf))) {
            break;
        }
        probe_cand_push(out, cap, &count, buf);
    }
    return count;
}

/* Borrow candidate values from something the tool SAID — its own answer, the
 * refusal it just wrote, or the pile of answers it has given this walk so far.
 * An enumerated property borrows nothing: its legal set is published, and
 * widening it past the enum would let a value the schema never offered stand
 * in for one it did.
 *
 * LONGEST FIRST when borrowing from the pile, and that ordering is the whole
 * reason the pile is usable. A value this surface MINTS — a resume token, a
 * record id, a timestamp — is long and unlike a word; the prose it is embedded
 * in is short English, and a tool that hands back a token immediately explains
 * in a sentence what to do with it. Taking the pile in text order spends the
 * whole borrowing budget on that sentence. Taking it longest-first puts the
 * minted value at the front, where a bounded budget can still reach it. */
static int probe_borrow_words(yyjson_val *spec, const probe_words_t *words, bool longest_first,
                              probe_value_t *out, int cap, int count) {
    yyjson_val *type_val = spec ? yyjson_obj_get(spec, "type") : NULL;
    const char *type = type_val && yyjson_is_str(type_val) ? yyjson_get_str(type_val) : NULL;
    yyjson_val *items = spec ? yyjson_obj_get(spec, "items") : NULL;
    yyjson_val *item_type_val = items ? yyjson_obj_get(items, "type") : NULL;
    const char *item_type =
        item_type_val && yyjson_is_str(item_type_val) ? yyjson_get_str(item_type_val) : NULL;
    bool enumerated = spec != NULL && yyjson_obj_get(spec, "enum") != NULL;
    bool item_enumerated = items != NULL && yyjson_obj_get(items, "enum") != NULL;
    bool string_like = type == NULL || strcmp(type, "string") == 0;
    bool number_like =
        type != NULL && (strcmp(type, "integer") == 0 || strcmp(type, "number") == 0);
    bool string_array = type != NULL && strcmp(type, "array") == 0 && item_type != NULL &&
                        strcmp(item_type, "string") == 0 && !item_enumerated;
    if (!words || enumerated || (!string_like && !number_like && !string_array)) {
        return count;
    }
    bool spent[PROBE_WORDS_CAP];
    memset(spent, 0, sizeof(spent));
    int borrowed = 0;
    for (int step = 0; step < words->count && borrowed < PROBE_BORROW_CAP && count < cap; step++) {
        int i = step;
        if (longest_first) {
            i = -1;
            size_t best = 0U;
            for (int j = 0; j < words->count; j++) {
                size_t len = strlen(words->item[j]);
                if (!spent[j] && (i < 0 || len > best)) {
                    i = j;
                    best = len;
                }
            }
            if (i < 0) {
                break;
            }
            spent[i] = true;
        }
        const char *token = words->item[i];
        char buf[PROBE_VALUE_CAP];
        if (number_like) {
            bool digits = true;
            for (const char *p = token; *p != '\0'; p++) {
                if (*p < '0' || *p > '9') {
                    digits = false;
                    break;
                }
            }
            if (!digits) {
                continue;
            }
            snprintf(buf, sizeof(buf), "%s", token);
        } else if (string_array) {
            snprintf(buf, sizeof(buf), "[\"%s\"]", token);
        } else {
            snprintf(buf, sizeof(buf), "\"%s\"", token);
        }
        int before = count;
        probe_cand_push(out, cap, &count, buf);
        if (count != before) {
            borrowed++;
        }
    }
    return count;
}

static int probe_borrow(yyjson_val *spec, const char *said, bool longest_first, probe_value_t *out,
                        int cap, int count) {
    if (!said) {
        return count;
    }
    probe_words_t words;
    probe_words_scan(&words, said);
    return probe_borrow_words(spec, &words, longest_first, out, cap, count);
}

/* ── The arguments object one call carries ─────────────────────────────── */
typedef struct {
    char key[PROBE_PAIR_CAP][PROBE_KEY_CAP];
    char val[PROBE_PAIR_CAP][PROBE_VALUE_CAP];
    int count;
} probe_args_t;

static void probe_args_set(probe_args_t *args, const char *key, const char *value) {
    for (int i = 0; i < args->count; i++) {
        if (strcmp(args->key[i], key) == 0) {
            snprintf(args->val[i], PROBE_VALUE_CAP, "%s", value);
            return;
        }
    }
    if (args->count >= PROBE_PAIR_CAP) {
        return;
    }
    snprintf(args->key[args->count], PROBE_KEY_CAP, "%s", key);
    snprintf(args->val[args->count], PROBE_VALUE_CAP, "%s", value);
    args->count++;
}

/* The base call, with one property overridden or added. That single difference
 * is the whole instrument. */
static void probe_args_render(const probe_args_t *args, const char *key, const char *value,
                              char *out, size_t cap) {
    size_t used = 1U;
    bool present = false;
    snprintf(out, cap, "{");
    for (int i = 0; i < args->count; i++) {
        if (used + 8U >= cap) {
            break;
        }
        const char *use = args->val[i];
        if (key && strcmp(args->key[i], key) == 0) {
            use = value;
            present = true;
        }
        int wrote = snprintf(out + used, cap - used, "%s\"%s\":%s", used > 1U ? "," : "",
                             args->key[i], use);
        if (wrote < 0 || (size_t)wrote >= cap - used - 2U) {
            break;
        }
        used += (size_t)wrote;
    }
    if (key && !present && used + 8U < cap) {
        int wrote =
            snprintf(out + used, cap - used, "%s\"%s\":%s", used > 1U ? "," : "", key, value);
        if (wrote > 0 && (size_t)wrote < cap - used - 2U) {
            used += (size_t)wrote;
        }
    }
    snprintf(out + used, cap - used, "}");
}

static bool probe_is_required(yyjson_val *required, const char *name) {
    if (!required || !yyjson_is_arr(required) || !name) {
        return false;
    }
    size_t index = 0U;
    size_t max = 0U;
    yyjson_val *entry = NULL;
    yyjson_arr_foreach(required, index, max, entry) {
        if (yyjson_is_str(entry) && strcmp(yyjson_get_str(entry), name) == 0) {
            return true;
        }
    }
    return false;
}

static void probe_append(char *buf, size_t cap, const char *line) {
    size_t used = strlen(buf);
    if (used + strlen(line) + 1U < cap) {
        strncat(buf, line, cap - used - 1U);
    }
}

/* ── The base call, made as answerable as this surface allows ───────────
 *
 * Required properties are filled from the schema first. If the call still
 * refuses, the refusal is read for words that would work and they are tried,
 * property by property — a refusal on this surface names the accepted set, so
 * this uses the contract rather than guessing at it. A base that cannot be
 * made to answer is reported by name and every property of that tool is
 * counted unattributable; it is never blamed. */
static bool probe_build_base(const char *tool, yyjson_val *props, yyjson_val *required,
                             probe_args_t *base, char *text, size_t cap) {
    base->count = 0;
    yyjson_obj_iter first;
    yyjson_obj_iter_init(props, &first);
    yyjson_val *key = NULL;
    while ((key = yyjson_obj_iter_next(&first)) != NULL) {
        const char *name = yyjson_get_str(key);
        if (!name || !probe_is_required(required, name)) {
            continue;
        }
        probe_value_t seeds[PROBE_CAND_CAP];
        int seed_count = probe_candidates(yyjson_obj_iter_get_val(key), seeds, PROBE_CAND_CAP);
        if (seed_count > 0) {
            probe_args_set(base, name, seeds[0].text);
        }
    }

    char args[PROBE_ARGS_CAP];
    probe_args_render(base, NULL, NULL, args, sizeof(args));
    bool is_error = probe_call(tool, args, text, cap);
    int spent = 1;

    for (int round = 0; round < 4 && is_error && spent < PROBE_REPAIR_CAP; round++) {
        bool moved = false;
        for (int pass = 0; pass < 2 && !moved && spent < PROBE_REPAIR_CAP; pass++) {
            yyjson_obj_iter it;
            yyjson_obj_iter_init(props, &it);
            yyjson_val *pkey = NULL;
            while ((pkey = yyjson_obj_iter_next(&it)) != NULL && spent < PROBE_REPAIR_CAP) {
                const char *name = yyjson_get_str(pkey);
                if (!name) {
                    continue;
                }
                bool wanted = probe_is_required(required, name);
                if ((pass == 0) != wanted) {
                    continue;
                }
                yyjson_val *spec = yyjson_obj_iter_get_val(pkey);
                probe_value_t cands[PROBE_CAND_CAP];
                int count = probe_candidates(spec, cands, PROBE_CAND_CAP);
                /* Whatever the tool last said, longest first when it ANSWERED (a
                 * minted value is long) and in text order when it REFUSED (a
                 * refusal names what would work in its first sentence). */
                count = probe_borrow(spec, text, !is_error, cands, PROBE_CAND_CAP, count);
                int nudge = -1;
                char nudge_text[PROBE_TEXT_CAP];
                nudge_text[0] = '\0';
                for (int c = 0; c < count && spent < PROBE_REPAIR_CAP; c++) {
                    char trial[PROBE_ARGS_CAP];
                    char answer[PROBE_TEXT_CAP];
                    probe_args_render(base, name, cands[c].text, trial, sizeof(trial));
                    bool failed = probe_call(tool, trial, answer, sizeof(answer));
                    spent++;
                    if (!failed) {
                        probe_args_set(base, name, cands[c].text);
                        snprintf(text, cap, "%s", answer);
                        return false;
                    }
                    if (nudge < 0 && wanted && strcmp(answer, text) != 0) {
                        nudge = c;
                        snprintf(nudge_text, sizeof(nudge_text), "%s", answer);
                    }
                }
                if (nudge >= 0) {
                    probe_args_set(base, name, cands[nudge].text);
                    snprintf(text, cap, "%s", nudge_text);
                    moved = true;
                    break;
                }
            }
        }
        if (!moved) {
            break;
        }
    }
    return is_error;
}

static probe_args_t probe_base[PROBE_TOOL_CAP];
static char probe_base_text[PROBE_TOOL_CAP][PROBE_TEXT_CAP];
static bool probe_base_error[PROBE_TOOL_CAP];
/* Every word every ANSWER this tool has given the walk. A value only this
 * surface can mint is in here or nowhere. */
static probe_words_t probe_said[PROBE_TOOL_CAP];
/* What the base call ASKED, kept so the report can show it. A reach number
 * nobody can check is a number; the call that produced it is evidence. */
static char probe_base_args[PROBE_TOOL_CAP][192];

TEST(tool_surface_every_advertised_property_is_one_the_handler_accepts) {
    surface_memory_fixture_t memory;
    if (!surface_memory_begin(&memory)) {
        FAIL("could not create a temporary memory store directory");
    }
    surface_cache_fixture_t cache;
    if (!surface_cache_begin(&cache)) {
        surface_memory_end(&memory);
        FAIL("could not create a fresh cache directory");
    }
    char root[256];
    snprintf(root, sizeof(root), "/tmp/hyp-tool-surface-root-XXXXXX");
    if (!hyp_mkdtemp(root)) {
        surface_cache_end(&cache);
        surface_memory_end(&memory);
        FAIL("could not create a project root for the probe");
    }
    snprintf(probe_cache_dir, sizeof(probe_cache_dir), "%s", cache.dir);
    snprintf(probe_root_dir, sizeof(probe_root_dir), "%s", root);
    snprintf(probe_db_path, sizeof(probe_db_path), "%s/%s.db", cache.dir, PROBE_PROJECT);
    snprintf(probe_project_json, sizeof(probe_project_json), "\"%s\"", PROBE_PROJECT);
    snprintf(probe_root_json, sizeof(probe_root_json), "\"%s\"", root);
    char source[320];
    snprintf(source, sizeof(source), "%s/probe.c", root);
    FILE *seedfile = fopen(source, "w");
    if (seedfile) {
        fputs("int probe(void) { return 0; }\n", seedfile);
        fclose(seedfile);
    }
    probe_calls = 0;
    probe_unanswered = 0;
    for (int i = 0; i < PROBE_TOOL_CAP; i++) {
        probe_said[i].count = 0;
    }
    if (!probe_seed()) {
        surface_cache_end(&cache);
        surface_memory_end(&memory);
        FAIL("could not seed the one project the probe resolves against");
    }

    /* The client's view of the surface: the names, the annotations and the
     * schemas all come out of the one tools/list a client parses. */
    char *listing = surface_call(HYP_MCP_TOOL_PROFILE_ALL, "tools/list", NULL);
    yyjson_doc *ldoc = listing ? yyjson_read(listing, strlen(listing), 0) : NULL;
    free(listing);
    yyjson_val *result = ldoc ? yyjson_obj_get(yyjson_doc_get_root(ldoc), "result") : NULL;
    yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
    if (!tools || !yyjson_is_arr(tools)) {
        yyjson_doc_free(ldoc);
        surface_cache_end(&cache);
        surface_memory_end(&memory);
        FAIL("a client cannot read the advertised tool list at all");
    }

    int walked_tools = 0;
    int walked_props = 0;
    int accepted = 0;
    int findings = 0;
    int unattr_tool = 0;
    int unattr_value = 0;
    int open_world = 0;
    int base_unanswered = 0;
    char report[6000];
    char bases[9000];
    char taken[4000];
    char blind_tool[2400];
    char blind_value[2400];
    char blind_base[600];
    report[0] = '\0';
    bases[0] = '\0';
    taken[0] = '\0';
    blind_tool[0] = '\0';
    blind_value[0] = '\0';
    blind_base[0] = '\0';

    for (int phase = 0; phase < 2; phase++) {
        size_t index = 0U;
        size_t max = 0U;
        yyjson_val *tool = NULL;
        yyjson_arr_foreach(tools, index, max, tool) {
            if (index >= (size_t)PROBE_TOOL_CAP) {
                break;
            }
            yyjson_val *name_val = yyjson_obj_get(tool, "name");
            const char *name =
                name_val && yyjson_is_str(name_val) ? yyjson_get_str(name_val) : NULL;
            yyjson_val *hints = yyjson_obj_get(tool, "annotations");
            yyjson_val *open = hints ? yyjson_obj_get(hints, "openWorldHint") : NULL;
            if (!name || !open) {
                continue;
            }
            if (yyjson_is_true(open)) {
                /* It leaves this machine; no fixture contains it. */
                if (phase == 0) {
                    open_world++;
                }
                continue;
            }
            yyjson_val *schema = yyjson_obj_get(tool, "inputSchema");
            yyjson_val *props = schema ? yyjson_obj_get(schema, "properties") : NULL;
            yyjson_val *required = schema ? yyjson_obj_get(schema, "required") : NULL;
            if (!props || !yyjson_is_obj(props)) {
                continue;
            }
            int slot = (int)index;

            if (phase == 0) {
                walked_tools++;
                probe_base_error[slot] = probe_build_base(name, props, required, &probe_base[slot],
                                                          probe_base_text[slot], PROBE_TEXT_CAP);
                if (probe_base_error[slot]) {
                    base_unanswered++;
                    char line[128];
                    snprintf(line, sizeof(line), "%s%s", blind_base[0] ? ", " : "", name);
                    probe_append(blind_base, sizeof(blind_base), line);
                } else {
                    probe_words_absorb(&probe_said[slot], probe_base_text[slot]);
                }
                {
                    char rendered[PROBE_ARGS_CAP];
                    probe_args_render(&probe_base[slot], NULL, NULL, rendered, sizeof(rendered));
                    snprintf(probe_base_args[slot], sizeof(probe_base_args[0]), "%s", rendered);
                    char line[640];
                    snprintf(line, sizeof(line), "\n        %-22s %-8s %.60s -> %.300s", name,
                             probe_base_error[slot] ? "REFUSED" : "answered",
                             probe_base_args[slot], probe_base_text[slot]);
                    probe_append(bases, sizeof(bases), line);
                }
                continue;
            }

            yyjson_obj_iter it;
            yyjson_obj_iter_init(props, &it);
            yyjson_val *key = NULL;
            while ((key = yyjson_obj_iter_next(&it)) != NULL) {
                const char *prop = yyjson_get_str(key);
                if (!prop) {
                    continue;
                }
                yyjson_val *spec = yyjson_obj_iter_get_val(key);
                bool declares_default = spec != NULL && yyjson_obj_get(spec, "default") != NULL;
                probe_value_t cands[PROBE_CAND_CAP];
                int schema_count = probe_candidates(spec, cands, PROBE_CAND_CAP);
                int count = probe_borrow(spec, probe_base_text[slot], !probe_base_error[slot],
                                         cands, PROBE_CAND_CAP, schema_count);
                walked_props++;

                bool took_it = false;
                bool default_refused = false;
                int distinct = 0;
                int tried = 0;
                char first_refusal[PROBE_TEXT_CAP];
                char default_refusal[PROBE_TEXT_CAP];
                char longest_tried[PROBE_VALUE_CAP];
                first_refusal[0] = '\0';
                default_refusal[0] = '\0';
                longest_tried[0] = '\0';

                /* Round 0 tries what the schema declares plus what the tool
                 * said to its own base call; round 1 what the refusal itself
                 * named — the handler saying what would have worked; round 2
                 * everything this tool has answered so far, newest first,
                 * which is where a token the tool mints at run time lives.
                 *
                 * THE SCHEMA-DECLARED VALUES ARE ALL TRIED EVEN AFTER ONE IS
                 * ACCEPTED, and that is not waste: an argument accepted at its
                 * default may only MINT the value another argument needs at a
                 * different one, and a walk that stops at the first success
                 * never causes the answer it will later have to borrow from. */
                int done = 0;
                for (int round = 0; round < 3 && count > 0; round++) {
                    for (int c = done; c < count; c++) {
                        if (took_it && c >= schema_count) {
                            break;
                        }
                        char args[PROBE_ARGS_CAP];
                        char answer[PROBE_TEXT_CAP];
                        probe_args_render(&probe_base[slot], prop, cands[c].text, args,
                                          sizeof(args));
                        bool failed = probe_call(name, args, answer, sizeof(answer));
                        tried++;
                        if (strlen(cands[c].text) > strlen(longest_tried)) {
                            snprintf(longest_tried, sizeof(longest_tried), "%s", cands[c].text);
                        }
                        if (!failed) {
                            took_it = true;
                            probe_words_absorb(&probe_said[slot], answer);
                            continue;
                        }
                        if (c == 0 && declares_default) {
                            default_refused = true;
                            snprintf(default_refusal, sizeof(default_refusal), "%s", answer);
                        }
                        if (distinct == 0) {
                            distinct = 1;
                            snprintf(first_refusal, sizeof(first_refusal), "%s", answer);
                        } else if (distinct == 1 && strcmp(first_refusal, answer) != 0) {
                            distinct = 2;
                        }
                    }
                    done = count;
                    if (took_it) {
                        break;
                    }
                    if (round == 0) {
                        count = probe_borrow(spec, first_refusal, false, cands,
                                             PROBE_CAND_CAP, count);
                    } else {
                        count = probe_borrow_words(spec, &probe_said[slot], true, cands,
                                                   PROBE_CAND_CAP, count);
                    }
                    if (count == done) {
                        break;
                    }
                }

                if (took_it) {
                    accepted++;
                    char line[160];
                    snprintf(line, sizeof(line), "%s%s.%s", taken[0] ? ", " : "", name, prop);
                    probe_append(taken, sizeof(taken), line);
                } else if (probe_base_error[slot] || count == 0) {
                    /* The tool refuses its own base call, so a refusal of this
                     * property is a refusal of something else as easily as of
                     * this argument. Counted, named, never blamed. */
                    unattr_tool++;
                    char line[160];
                    snprintf(line, sizeof(line), "%s%s.%s", blind_tool[0] ? ", " : "", name, prop);
                    probe_append(blind_tool, sizeof(blind_tool), line);
                } else if (distinct >= 2) {
                    /* The refusal moved with the value, so the handler READ the
                     * value: the argument is reachable and this walk simply
                     * could not derive a legal one. */
                    unattr_value++;
                    char line[160];
                    snprintf(line, sizeof(line), "%s%s.%s", blind_value[0] ? ", " : "", name, prop);
                    probe_append(blind_value, sizeof(blind_value), line);
                } else {
                    findings++;
                    char line[600];
                    char vocab[1600];
                    vocab[0] = '\0';
                    for (int w = 0; w < probe_said[slot].count; w++) {
                        char one[128];
                        snprintf(one, sizeof(one), "%s%s", w ? " " : "", probe_said[slot].item[w]);
                        probe_append(vocab, sizeof(vocab), one);
                    }
                    snprintf(line, sizeof(line),
                             "\n      %s advertises \"%s\", and NO value this walk can derive is "
                             "one the handler takes (%d tried, longest %.50s): %.130s"
                             "\n        every word this tool said: %.1400s",
                             name, prop, tried, longest_tried, first_refusal, vocab);
                    probe_append(report, sizeof(report), line);
                }
                /* A refused DEFAULT is its own finding even when some other
                 * value works: a generated client sends the default. */
                if (default_refused && !probe_base_error[slot]) {
                    findings++;
                    char line[600];
                    snprintf(line, sizeof(line),
                             "\n      %s advertises \"%s\" with a DEFAULT its handler refuses: "
                             "%.200s",
                             name, prop, default_refusal);
                    probe_append(report, sizeof(report), line);
                }
            }
        }
    }

    yyjson_doc_free(ldoc);
    surface_cache_end(&cache);
    surface_memory_end(&memory);

    /* The instrument, before anything that trusts it. A walk that visited no
     * schema, or that could not attribute a single property, would pass on
     * every tree forever. */
    if (walked_tools == 0 || walked_props == 0) {
        FAIL("the walk visited no advertised property; the set derivation is "
             "broken, not the surface");
    }
    if (accepted == 0) {
        FAIL("the walk found no property any handler accepts, which is the "
             "fixture failing rather than the surface");
    }
    if (probe_unanswered > 0) {
        FAIL("an advertised tool returned nothing a client can parse");
    }

    int observable = accepted + findings;
    printf("\n    check C: %d tools walked (%d skipped: they leave this machine), %d advertised "
           "properties, %d OBSERVABLE (%d accepted, %d refused), %d unattributable, in %d calls.\n"
           "      accepted — %s\n"
           "      unreachable THROUGH THE TOOL (%d): it refuses the base call the same way — %s\n"
           "      unreachable THROUGH THE VALUE (%d): the refusal moves with the value and no "
           "legal one was derivable — %s\n"
           "      base call never answered (%d tools): %s\n"
           "      the base call every probe of a tool differs from by exactly one property:%s\n"
           "      Read OBSERVABLE, never the total: the rest were counted and never judged.\n",
           walked_tools, open_world, walked_props, observable, accepted, findings,
           unattr_tool + unattr_value, probe_calls, taken[0] ? taken : "none", unattr_tool,
           blind_tool[0] ? blind_tool : "none", unattr_value,
           blind_value[0] ? blind_value : "none", base_unanswered,
           blind_base[0] ? blind_base : "none", bases);

    if (findings > 0) {
        printf("%s\n"
               "      A client generated from the schema sends the argument and is refused for "
               "obeying the contract. There is no exemption table to add a row to: withdraw the "
               "advertisement, or make the handler take it.\n",
               report);
        FAIL("a tool advertises a property no call of its own can use");
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
    RUN_TEST(tool_surface_the_deprecated_adr_tool_shares_no_vocabulary_with_the_memory_surface);
    RUN_TEST(tool_surface_every_advertised_property_is_one_the_handler_accepts);
}
