/*
 * tool_surface.h — THE table of MCP tools. Every end reads this one.
 *
 * ─── What this replaces, and why the rename ────────────────────────────
 *
 * This file was `tool_tiers.h`, and its own first line called itself "the one
 * table of which tools the restricted MCP profiles expose". It was one table
 * of TIER MEMBERSHIP, for 12 of the 16 registered tools, and three other
 * hand-written enumerations of the same tools sat beside it:
 *
 *   1. TOOLS[]            (mcp.c)  the registry: name, title, description,
 *                                  input schema. 16 rows.
 *   2. TOOL_ANNOTATIONS[] (mcp.c)  the client-visible annotation hints, keyed
 *                                  by name, WITH A SILENT FALLBACK: a tool
 *                                  absent from it still shipped, advertising
 *                                  destructiveHint=true, openWorldHint=true.
 *   3. HYP_TOOL_TIERS     (here)   tier membership. 12 rows.
 *   4. dispatch_tool()    (mcp.c)  the if-chain that routes a call.
 *
 * `index_repository`, `delete_project`, `manage_adr` and `ingest_traces` were
 * in (1) and absent from (3). Their absence was INTENDED and CHECKED BY
 * NOTHING — so a tool added to (1) and forgotten in (3) shipped silently off
 * every tier, callable by the full server and requested by no generated agent
 * profile. That is `ask` exactly, one level above where `ask` was fixed: the
 * fix closed the hole between two of the four lists and left the other two
 * open. The fourth site had not been counted.
 *
 * So the table is now the whole surface. Every row states every fact both ends
 * need, every column is mandatory, and nothing about a tool is decided
 * anywhere else:
 *
 *   - mcp.c expands it for the tier allowlist, for the annotations (the
 *     fallback is DELETED — a row cannot omit an annotation profile, it is a
 *     column), for what tools/list advertises, and for what dispatch accepts.
 *   - cli/agent_profiles.c expands it for the tool list every generated agent
 *     definition requests.
 *   - A _Static_assert pairs the row count against sizeof(TOOLS)/sizeof(*TOOLS),
 *     so adding a tool to one and not the other DOES NOT COMPILE.
 *   - tests/test_tool_surface.c holds both ends to it FROM THE CLIENT'S SIDE:
 *     the tools/list a client reads and the profile file an agent loads, never
 *     either end's own static array.
 *
 * The rename is part of the fix. A header called `tool_tiers.h` is a header
 * about tiers; a table about tiers is a table the next tool fact gets added
 * BESIDE rather than INTO. That is how there came to be four.
 *
 * ─── Columns ───────────────────────────────────────────────────────────
 *
 *   X(name, alias, analysis, scout, generation, status, output_schema, annotations)
 *
 *   name           wire name, exactly as it appears in tools/list.
 *   alias          NULL, or a second name dispatch also accepts. FOUND WHILE
 *                  WRITING THIS TABLE, and it was the fifth undeclared list:
 *                  `trace_call_path` has been accepted by dispatch_tool for its
 *                  whole life, appears in TOOLS[] nowhere, in the tier table
 *                  nowhere, in tools/list nowhere — and is the name
 *                  src/cli/cli.c:5401 tells users to prefer. A tool the server
 *                  accepts that no end declares is the `ask` defect inverted,
 *                  and it was invisible for the same reason: nothing enumerated
 *                  the names dispatch answers to. An alias is never advertised
 *                  and never on a restricted profile — it is a compatibility
 *                  shim for callers that already exist, not surface — but it is
 *                  DECLARED, and a test calls every one of them.
 *   analysis       1 if the `--tool-profile analysis` surface admits it.
 *   scout          1 if the `--tool-profile scout` surface admits it.
 *                  A tool on NO tier says so with two zeroes. There is no such
 *                  thing as a missing row.
 *   generation     the profile generation that introduced the row. See below.
 *   status         HYP_TOOL_LIVE       advertised and dispatchable.
 *                  HYP_TOOL_DEPRECATED advertised and dispatchable, unchanged
 *                                      on the wire; no NEW surface may depend
 *                                      on it (a test enforces that).
 *                  HYP_TOOL_RESERVED   the signature is published and frozen;
 *                                      NOT advertised, NOT dispatchable. A
 *                                      call fails closed with a named error.
 *   output_schema  NULL, or a JSON Schema string advertised as `outputSchema`.
 *                  NULL is not "no shape" — it is "no PROMISE of shape": the
 *                  answer is in content, which is the protocol's own default
 *                  and cannot be misread. A non-NULL schema is a promise that
 *                  every one of its `required` fields is present in what a
 *                  CLIENT reads, on every legal call. tests/test_tool_surface.c
 *                  and scripts/ci/mcp-client-view.py both check that by
 *                  reading the response, never by inspecting the emitter.
 *   annotations    a profile from HYP_TOOL_ANNOTATION_PROFILES below. Mandatory
 *                  — it is a column, so a row without one does not compile.
 *                  There is no default and no fallback. The four booleans that
 *                  reach the wire have not changed; naming the profile is what
 *                  lets the table say a thing the four booleans cannot (see
 *                  HYP_TOOL_ANN_DESTRUCTIVE).
 *
 * ─── Reserved rows, and why not "advertised but not implemented" ───────
 *
 * A reserved row publishes a signature without shipping a tool. That is the
 * point of a Phase 0 contract: the row is COMPLETE — tier, generation,
 * annotations, output promise — before it is reachable, so going live is one
 * token in one row and there is nothing left to remember.
 *
 * Advertising an unimplemented tool was rejected: a client that reads a tool
 * which errors is §3's blank-tool defect deliberately reintroduced, and §3.1
 * measured that a tool in the list is not a tool the agent uses even when it
 * WORKS (`ask`: 4/60 runs listed-only, 60/60 once the prompt taught it). A
 * tool that cannot answer would still cost every session the call.
 *
 * Two independent gates gulf a reserved row from a rendered one, and BOTH must
 * be crossed to ship it: `status` (here) and `generation` (below). Crossing one
 * without the other is loud, not silent — flipping a tiered row to LIVE without
 * bumping HYP_PROFILE_GENERATION makes the server advertise a tool the
 * generated profile does not request, which is the `ask` shape, and
 * test_tool_surface / test_agent_profiles both fail on it. That failure is the
 * negative control this file exists to pass.
 *
 * ─── Generations ──────────────────────────────────────────────────────
 *
 * Generations exist for the installer, which only replaces an agent file it
 * can prove it wrote: the bytes must equal the current rendering or one it
 * previously released. Every earlier generation therefore has to stay
 * renderable byte-for-byte, and a generation is a change to ANY rendered byte
 * of a profile — a row here or the tier prompt text in agent_profiles.c. Row
 * ORDER is part of the contract (generated profiles list tools in row order).
 * Add a tool by appending a row tagged with the new generation and bumping
 * HYP_PROFILE_GENERATION; change prompt text by gating the new words on a new
 * generation the same way; never reorder or rename an existing row, never edit
 * an older generation's words in place.
 *
 * Row order here is NOT the order of TOOLS[] in mcp.c and must not be made to
 * match it: this order is a shipped contract and that one is not. The two are
 * held to the same SET (a test) and the same COUNT (a _Static_assert), which is
 * what actually prevents a tool existing in one and not the other.
 */
#ifndef HYP_MCP_TOOL_SURFACE_H
#define HYP_MCP_TOOL_SURFACE_H

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
 *    `project` out, and read project/project_source in the answer.
 * 4  RESERVED, not yet shipped: the §4 workspace and memory surfaces
 *    (`search_memory`, `workspace_status`). Tagging their rows 4 while
 *    HYP_PROFILE_GENERATION stays 3 is the second gate described above — the
 *    rows exist, carry their tiers, and render nowhere. Whoever flips their
 *    status to LIVE must bump HYP_PROFILE_GENERATION to 4 in the same commit
 *    AND write the prompt sentence that teaches them, because generation 2
 *    measured what a listed-but-untaught tool is worth: 4 calls in 60 runs.
 */
#define HYP_PROFILE_GENERATION 3U
#define HYP_PROFILE_GENERATION_ASK_TOOL 1U
#define HYP_PROFILE_GENERATION_ASK_GUIDANCE 2U
#define HYP_PROFILE_GENERATION_PROJECT_DEFAULT 3U
#define HYP_PROFILE_GENERATION_WORKSPACE_MEMORY 4U

/* ── Status ────────────────────────────────────────────────────────── */
typedef enum {
    HYP_TOOL_RESERVED = 0,  /* signature published; not advertised, not callable */
    HYP_TOOL_LIVE = 1,      /* advertised and callable */
    HYP_TOOL_DEPRECATED = 2 /* advertised and callable, unchanged on the wire;
                             * no new surface may depend on it */
} hyp_tool_status_t;

/* ── Annotation profiles ───────────────────────────────────────────────
 *
 * X(profile, read_only, destructive, idempotent, open_world)
 *
 * These four booleans are exactly what tools/list has always emitted; not one
 * of them changes here. What changes is that they now come from a NAMED
 * profile in a mandatory column instead of a name-keyed lookup with a silent
 * default, so "tool present in the registry, absent from the annotation list"
 * is no longer a state the program can be in.
 *
 * Naming them also lets the table record something the wire cannot.
 * HYP_TOOL_ANN_DESTRUCTIVE and HYP_TOOL_ANN_STORE_READ carry IDENTICAL
 * booleans, which means `delete_project` and `search_graph` are
 * indistinguishable to an MCP client today: the one tool that erases a
 * database advertises the same four hints as the tool that reads it. That is a
 * real defect and fixing it changes bytes a client reads, so it is not fixed
 * here; the table names it so the fix is one row when someone takes it.
 */
#define HYP_TOOL_ANNOTATION_PROFILES(X)                       \
    /* reads nothing but its own listing */                   \
    X(HYP_TOOL_ANN_PURE_READ, true, false, true, false)       \
    /* reads the graph; opening a project can quarantine a    \
     * corrupt store, so not strictly read-only */            \
    X(HYP_TOOL_ANN_STORE_READ, false, true, true, false)      \
    /* erases a project database. See the note above: the     \
     * booleans are the same as STORE_READ and should not be. \
     */                                                       \
    X(HYP_TOOL_ANN_DESTRUCTIVE, false, true, true, false)     \
    /* builds or refreshes an index */                        \
    X(HYP_TOOL_ANN_INDEX_WRITE, false, false, true, false)    \
    /* replaces a whole document — see manage_adr's row */    \
    X(HYP_TOOL_ANN_DOC_REPLACE, false, true, false, false)    \
    /* appends a record; never mutates one */                 \
    X(HYP_TOOL_ANN_APPEND, false, false, false, false)        \
    /* talks to a feed outside this machine. The only         \
     * open-world row on the surface, and the only one that   \
     * should ever be. */                                     \
    X(HYP_TOOL_ANN_FEED_SYNC, false, false, false, true)

typedef enum {
#define HYP_TOOL_ANN_ENUM_ROW(profile, ro, destr, idem, open) profile,
    HYP_TOOL_ANNOTATION_PROFILES(HYP_TOOL_ANN_ENUM_ROW)
#undef HYP_TOOL_ANN_ENUM_ROW
        HYP_TOOL_ANN_COUNT
} hyp_tool_annotation_profile_t;

/* A tool safe to CALL with no arguments in a test — it neither writes nor
 * leaves the machine. Derived from the annotation profile, so a new writer is
 * excluded by its own row rather than by a hand-kept skip list. */
#define HYP_TOOL_ANN_IS_PROBE_SAFE(ann) \
    ((ann) == HYP_TOOL_ANN_PURE_READ || (ann) == HYP_TOOL_ANN_STORE_READ)

/* ── Output promises ───────────────────────────────────────────────────
 *
 * `index_status` is the only tool that declares an outputSchema, and it can:
 * its handler builds a yyjson object, so hyp_mcp_text_result parses it into
 * structuredContent on every call. Every other tool answers in TOON or in
 * prose, or switches shape on `format`, so none of them may promise one.
 *
 * `required` is deliberately ["status"] and nothing else. `status` is the one
 * key present on every path INCLUDING the no-project path, which answers
 * {"status":"no_project"} and nothing more. Everything else is optional
 * because its ABSENCE CARRIES MEANING, which is the whole reason this tool
 * gets a schema at all:
 *
 *     absent  = "look elsewhere"      empty = "there is nothing"
 *     Only one is ever true, and a schema that requires a key forces the
 *     server to invent a value for the case where the honest answer is that
 *     it does not know.
 *
 * ─── F3, the extension this schema is shaped for ──────────────────────
 *
 * §4 F3 asks index_status to say "your code is current, your decisions are 40
 * records behind." An agent that believes it has current decisions while 40 are
 * missing will confidently explain why `foo` looked the way it did last week.
 * The two freshnesses are genuinely independent axes and not two views of one
 * store — Multica ships pgvector in its image with the extension never enabled
 * and zero vector columns, so it holds sessions it cannot search, and its record
 * count moves for reasons the code index never sees.
 *
 * SET DIFFERENCE, NOT LAG — and this corrects §4's own wording. "40 behind"
 * presumes a sequence, and the record contract has none: the store is an
 * id-keyed SET whose order is deliberately non-semantic, because any
 * machine-local ordinal breaks the union that makes sync free. There is no
 * ordering against which to be "behind". What IS computable is what you do not
 * have: compare set digests for a free yes/no, then exchange ids for the exact
 * count. So the field is `missing`, and it means "records I do not have" — never
 * `behind`, which would be a number nothing can produce.
 *
 * F3 SHIPPED the two SIBLING objects. They are never merged into one status,
 * and neither is derived from the other:
 *
 *   "code":   { "status": "ready" | "empty" | "stale" | "unknown",
 *               "indexed_at": <iso8601>,       // absent if the store has none
 *               "nodes": n, "edges": n,
 *               "files_behind": n }            // absent if not computed
 *
 *   "memory": { "feed": "<adapter name>",      // absent if none configured
 *               "records_local": n,
 *               "status": "current" | "incomplete" | "unknown",
 *               "records_upstream": n,         // ABSENT unless actually read
 *               "missing": n,                  // ABSENT unless actually read
 *               "missing_ids": [ids],          // with `missing`; the NAMES —
 *                                              // a count alone cannot be
 *                                              // acted on. Capped, and the
 *                                              // cap is disclosed:
 *               "missing_ids_withheld": n,     // only when the list was capped
 *               "checked_at": <iso8601> }      // only when a compare RAN
 *
 * `code` restates today's top-level answer under its own key; the top-level
 * keys are the shipped surface and stay byte-identical, so existing consumers
 * read what they always read. `code.files_behind` and the "stale"/"unknown"
 * statuses are declared, not yet computed — a key must not appear until
 * something computes it.
 *
 * THE RULE THAT MATTERS, and the one a Phase 1 author will get wrong if it is
 * not written down:
 *
 *   "memory" absent entirely   -> this build has no memory store configured.
 *                                 Look elsewhere. NOT "you are up to date".
 *   status "unknown"           -> no upstream provider is configured (the
 *                                 shipped state until C6u lands sync), or the
 *                                 configured one could not read the set.
 *                                 `missing` and `records_upstream` are ABSENT.
 *   "missing": 0               -> checked, and nothing is missing.
 *
 * Absent `missing` and `missing: 0` must never be the same bytes. "0 missing"
 * emitted when nobody could reach the feed is the exact confident wrong answer
 * F3 exists to prevent, and it is more dangerous than the staleness it hides,
 * because an agent that is told it is current stops checking.
 *
 * And it is `missing`, never `behind` — a union has no total order, so "N
 * behind" is a number nothing can produce. The set-digest compare is the free
 * yes/no; the id walk is the count plus the names. mcp.h documents the seam
 * (hyp_mcp_server_set_memory_store / _set_memory_upstream).
 *
 * These keys are in `properties` below and NOT in `required`, because their
 * ABSENCE CARRIES MEANING. Declaring `required: ["memory"]` is the planted
 * negative control in tests/test_tool_surface.c and scripts/ci/
 * mcp-client-view.py: it makes the tool promise a field the no-store path
 * never sends, which is `structuredContent: {}` in its general form, and both
 * client-view checks fail on it. Watch it fail before trusting it.
 */
#define HYP_TOOL_SCHEMA_INDEX_STATUS                                                            \
    "{\"type\":\"object\","                                                                     \
    "\"properties\":{"                                                                          \
    "\"status\":{\"type\":\"string\",\"description\":\"ready | empty | no_project. no_project " \
    "means no project could be resolved — it is not an error and not an empty project.\"},"     \
    "\"project\":{\"type\":\"string\",\"description\":\"Absent when status is no_project.\"},"  \
    "\"project_source\":{\"type\":\"string\"},"                                                 \
    "\"nodes\":{\"type\":\"integer\"},"                                                         \
    "\"edges\":{\"type\":\"integer\"},"                                                         \
    "\"root_path\":{\"type\":\"string\"},"                                                      \
    "\"code\":{\"type\":\"object\",\"description\":\"Freshness of this machine's code index, "  \
    "as a sibling of memory — the two are independent axes and are never merged. Absent when "  \
    "status is no_project.\"},"                                                                 \
    "\"memory\":{\"type\":\"object\",\"description\":\"Freshness of the record (memory) "       \
    "store. ABSENT entirely when no record store is configured — look elsewhere, it does NOT "  \
    "mean current. memory.missing counts upstream records this machine does not hold (a set "   \
    "difference — a union has no order to be behind in) and is absent, with "                   \
    "status=unknown, whenever the upstream set could not be read; missing=0 means checked "     \
    "and complete. missing_ids names them.\"}"                                                  \
    "},"                                                                                        \
    "\"required\":[\"status\"]}"

/* ── The record kinds a tool may write ─────────────────────────────────
 *
 * X(kind, authorable)
 *
 * C2 owns the append-only record shape; this is the tool surface's view of it,
 * and when C2's header lands this macro MOVES THERE VERBATIM and this file
 * includes it. It lives here now so that `record_memory`'s accepted set and
 * its refusal message come from one place rather than from a literal in a JSON
 * schema string and a second literal in a handler.
 *
 * `transcript` is deliberately NOT authorable. Transcripts enter only through
 * a feed (§4 D1/D3/D5) and D4 audits ingest completeness against the feed's
 * own count(*) — that audit is the ONLY thing in the plan that can prove it
 * has everything, and an MCP writer able to forge a transcript message would
 * make its count meaningless. Refusing it is fail-closed: the tool names the
 * kinds it accepts rather than quietly writing the record under another label.
 *
 * The input schema deliberately does NOT restate this list as a JSON enum. A
 * schema enum would be a second copy of the set, and the two would drift
 * exactly the way the four tool lists did. The schema types `kind` as a string
 * and says the rule; the refusal error names the accepted kinds, generated
 * from this macro.
 */
#define HYP_MEMORY_KINDS(X) \
    X("decision", true)     \
    X("verdict", true)      \
    X("summary", true)      \
    X("signal", true)       \
    X("transcript", false)

/* ── THE TABLE ─────────────────────────────────────────────────────────
 *
 * X(name, alias, analysis, scout, generation, status, output_schema, annotations)
 */
#define HYP_TOOL_SURFACE(X)                                                                        \
    /* ── Rows 0-11: the tiered surface. ORDER IS A SHIPPED CONTRACT —                       \
     * generated agent profiles list tools in this order. Never reorder. */                        \
    X("search_graph", NULL, 1, 1, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)                \
    /* `ask` joins analysis but NOT scout. Scout's promise is a small surface                      \
     * whose every tool answers; a lane that can legitimately report                               \
     * unavailable (the semantic index is opt-in) belongs on the surface where                     \
     * an agent is already expected to reason about index state. */                                \
    X("ask", NULL, 1, 0, HYP_PROFILE_GENERATION_ASK_TOOL, HYP_TOOL_LIVE, NULL,                     \
      HYP_TOOL_ANN_STORE_READ)                                                                     \
    X("trace_path", "trace_call_path", 1, 1, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)     \
    X("get_code_snippet", NULL, 1, 1, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)            \
    X("query_graph", NULL, 1, 0, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)                 \
    X("get_architecture", NULL, 1, 1, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)            \
    X("search_code", NULL, 1, 0, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)                 \
    X("get_graph_schema", NULL, 1, 0, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)            \
    X("list_projects", NULL, 1, 1, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_PURE_READ)                \
    X("index_status", NULL, 1, 1, 0U, HYP_TOOL_LIVE, HYP_TOOL_SCHEMA_INDEX_STATUS,                 \
      HYP_TOOL_ANN_STORE_READ)                                                                     \
    X("detect_changes", NULL, 1, 0, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)              \
    X("check_index_coverage", NULL, 1, 1, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_STORE_READ)        \
                                                                                                   \
    /* ── Rows 12-15: live, on NO tier. Stated with two zeroes, not by being                   \
     * absent. Every one of these four used to have no row at all, and their                       \
     * absence was intended by someone and checked by nobody. */                                   \
    X("index_repository", NULL, 0, 0, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_INDEX_WRITE)           \
    /* keeps `project` REQUIRED, alone among the 14 tools that take it: a                          \
     * destructive tool does not infer its target. */                                              \
    X("delete_project", NULL, 0, 0, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_DESTRUCTIVE)             \
    /* DEPRECATED by the append-only decision store, and live and unchanged on                     \
     * the wire. `mode:"update"` writes THROUGH that store: the document becomes                   \
     * a kind=decision record whose `origin` is the ADR's project id and whose                     \
     * anchor is NULL, and the project_summaries row the UI and `mode:"get"`                       \
     * read is refreshed from that record as a PROJECTION of it. What the row                      \
     * no longer does is DESTROY the text it supersedes, which is what made it                     \
     * a second, mutable, last-writer-wins store of the one thing the record                       \
     * contract exists to keep — the same shape as two UI keys for one concept,                  \
     * one level down at the data layer. Deprecation stays: no new memory                          \
     * surface may depend on this row, and test_tool_surface asserts it from                       \
     * both directions — no memory argument arrives here, no ADR vocabulary                      \
     * leaves. src/memory/adr_records.h holds the fold and the attribution                         \
     * argument. NOT ONE BYTE a client reads moves — annotations, input schema                   \
     * and the `semantics` field in the answer are all what they were. The                         \
     * annotation profile reads pessimistically for a writer that destroys                         \
     * nothing, and stays anyway: those four booleans are bytes a client                           \
     * branches on, and the unit that re-baselines them is the one flipping a                      \
     * reserved row live, not this one. */                                                         \
    X("manage_adr", NULL, 0, 0, 0U, HYP_TOOL_DEPRECATED, NULL, HYP_TOOL_ANN_DOC_REPLACE)           \
    X("ingest_traces", NULL, 0, 0, 0U, HYP_TOOL_LIVE, NULL, HYP_TOOL_ANN_APPEND)                   \
                                                                                                   \
    /* ── Rows 16-19: RESERVED. §4 Phase 1 signatures, published and frozen,                  \
     * advertised nowhere and callable nowhere until their status flips. Each                      \
     * row is already complete, so the flip is one token. See the prose block                      \
     * below each name in this header for the argument and output contract. */                     \
    X("record_memory", NULL, 0, 0, HYP_PROFILE_GENERATION_WORKSPACE_MEMORY, HYP_TOOL_RESERVED,     \
      NULL, HYP_TOOL_ANN_APPEND)                                                                   \
    X("search_memory", NULL, 1, 0, HYP_PROFILE_GENERATION_WORKSPACE_MEMORY, HYP_TOOL_RESERVED,     \
      NULL, HYP_TOOL_ANN_STORE_READ)                                                               \
    X("workspace_status", NULL, 1, 1, HYP_PROFILE_GENERATION_WORKSPACE_MEMORY, HYP_TOOL_RESERVED,  \
      NULL, HYP_TOOL_ANN_STORE_READ)                                                               \
    X("sync_memory", NULL, 0, 0, HYP_PROFILE_GENERATION_WORKSPACE_MEMORY, HYP_TOOL_RESERVED, NULL, \
      HYP_TOOL_ANN_FEED_SYNC)

/* The number of rows above, for the _Static_assert in mcp.c that pairs this
 * table against TOOLS[]. Counted by the preprocessor, never by hand. */
#define HYP_TOOL_SURFACE_ROW_COUNT_ONE(name, al, a, s, g, st, os, ann) +1
#define HYP_TOOL_SURFACE_ROW_COUNT (0 HYP_TOOL_SURFACE(HYP_TOOL_SURFACE_ROW_COUNT_ONE))

/* ══════════════════════════════════════════════════════════════════════
 *  THE RESERVED SIGNATURES — §4 Phase 1
 *
 *  Argument shapes live in TOOLS[] beside every other input schema, because a
 *  reserved tool is a tool whose signature is published, not a tool kept in a
 *  different place. What follows is the contract those schemas implement: the
 *  reasoning, the failure modes, and the output shapes, which have nowhere
 *  else to live because a tool that does not run cannot have its output
 *  checked yet.
 *
 *  Three rules govern every one of them, and they are not style:
 *
 *  (1) FAIL CLOSED. A confident error beats a plausible wrong answer. Where a
 *      tool cannot know, it says so; it does not pick.
 *  (2) ABSENT means "look elsewhere". EMPTY means "there is nothing". Only one
 *      is ever true, and no output below is permitted to blur them.
 *  (3) NEVER A PLAUSIBLE NEIGHBOUR. §4 C4u: a false negative makes a decision
 *      unfindable; a false positive attaches last month's reasoning to code
 *      that never had it, which is worse.
 *
 * ── record_memory ─────────────────────────────────── §4 C1u [C2] ────
 *
 *  The ONE writer. Kind-parameterised rather than one tool per record kind,
 *  because C2 makes decisions, verdicts, summaries, signals and transcript
 *  messages one record shape, and five writers would be five places to forget
 *  the sixth. Named `record_*` and not `manage_*` so that mutation is not
 *  representable in the signature: there is no mode that edits, because
 *  append-only is what makes sync a union.
 *
 *    project     OPTIONAL. Resolved exactly as every other tool resolves it,
 *                and the answer reports project_source. It is optional even
 *                though the write is durable, and that differs deliberately
 *                from delete_project: an append is recoverable and a delete is
 *                not, and §3.2 measured `required` costing a list_projects
 *                round trip in 120 of 120 runs.
 *    kind        REQUIRED. One of the authorable kinds in HYP_MEMORY_KINDS.
 *                `transcript` is refused, fail-closed, naming the accepted
 *                set — see that macro for why a forgeable transcript writer
 *                would make D4's completeness audit meaningless.
 *    title       REQUIRED.
 *    body        REQUIRED.
 *    anchor      OPTIONAL object, C1's address of a span.
 *                  ABSENT      -> a repo-scoped record. Legitimate: a decision
 *                                 about code not yet written has nothing to
 *                                 attach to.
 *                  SUPPLIED and unresolvable -> ERROR. A record is never
 *                                 created already-orphaned, and never attached
 *                                 to the nearest plausible symbol. Rule (3).
 *                  SUPPLIED and ambiguous    -> ERROR listing the candidates.
 *                                 The tool does not choose among several, the
 *                                 same way project resolution does not.
 *    supersedes  OPTIONAL record id. Supersession is a NEW RECORD naming the
 *                old one. The superseded record is never edited and never
 *                deleted — that is what keeps merge a union with no conflict
 *                resolution and no merge UI.
 *    tags        OPTIONAL array of strings.
 *
 *  There is NO `author` argument and there must never be one. C2 derives a
 *  record id from content + author + timestamp; an author a caller can state
 *  is an author a caller can forge, and provenance that can be forged is not
 *  provenance.
 *
 *  Returns a JSON object: { id, kind, project, project_source, anchor_status,
 *  written_at }. `anchor_status` is "attached" or "unanchored" — never
 *  "orphaned", because rule (3) forbids creating one.
 *
 * ── search_memory ─────────────────────────────────── §4 C2u [C2] ────
 *
 *  The ONE reader, over every kind. This is the surface §4 G2 tests: ask "why
 *  does foo do X?" and check that what actually happened comes back.
 *
 *    project     OPTIONAL, resolved as everywhere else.
 *    kind        OPTIONAL. ABSENT means every kind — it does not mean none,
 *                and it is not an empty filter.
 *    query       OPTIONAL free text.
 *    anchor      OPTIONAL, C1's address. Valid only for kinds that carry an
 *                anchor; supplying it with kind="transcript" is an ERROR and
 *                not a silently ignored argument, because an ignored filter
 *                returns a superset that reads exactly like a match.
 *    status      OPTIONAL: "attached" (default) | "orphaned" | "any".
 *                "orphaned" is how §4 C4u's orphans become VISIBLE. An orphan
 *                that no query can list is a silent drop by another name.
 *    since/until OPTIONAL timestamps.
 *    limit/offset, format  as elsewhere.
 *
 *  Output, and this is the part that carries the unit:
 *
 *    anchor_status "resolved"  + records: []   -> THE ANCHOR RESOLVED AND
 *                                 NOTHING IS ATTACHED. An empty list here is a
 *                                 real answer and the agent should believe it.
 *    anchor_status "orphaned"  + records ABSENT -> the anchor did not resolve.
 *                                 Look elsewhere. An empty list here would
 *                                 read as "nothing was ever recorded", which
 *                                 is a different and false claim.
 *    anchor_status "ambiguous" + candidates[] + records ABSENT -> fail closed.
 *                                 No nearest neighbour is ever returned.
 *
 *  Truncation follows §3.3's disclosure convention: a truncated answer states
 *  what was withheld and how to get it, and says NOTHING when nothing was held
 *  back — `truncated`/`withheld`/`how` are absent, not false and zero.
 *
 * ── workspace_status ──────────────────── §4 A1/A6/A7 [C1] ───────────
 *
 *  The workspace analogue of index_status, and NOT a second addressing scheme:
 *  §4 forbids "a workspace mode beside a repo mode", so this is a status view
 *  over the one scheme, and there is deliberately no `list_workspaces`. The
 *  cheap listing stays in list_projects (which gains `workspace`, and
 *  `role`/`role_source` per project); A6's resolver precedence and A7's audit
 *  belong in a status surface, not stuffed into a listing.
 *
 *    workspace   OPTIONAL, resolved by the same ladder as project: supplied ->
 *                the workspace of the working directory or nearest indexed
 *                ancestor -> the only workspace. Never by picking among
 *                several. The answer reports workspace_source.
 *    verbose     OPTIONAL.
 *
 *  Returns: { workspace, workspace_source, repos: [ { project, root_path,
 *  role, role_source, indexed } ], precedence: [...], contradictions: [...] }
 *
 *    role_source names WHICH derivation produced the role — the vendored
 *    checksum registry (A2), a gitignored path or DO NOT EDIT header (A3), the
 *    git remote (A4), the TOML, or the default. A role with no stated source
 *    is a guess wearing a fact's clothes. Unknown role defaults to
 *    `reference`: wrong-but-read-only is recoverable, wrong-but-editable means
 *    an agent rewrites a dependency.
 *
 *    `precedence` is emitted BY THE RESOLVER, not written as prose in a doc,
 *    because §4 A6 asks for one resolver with a stated precedence and a stated
 *    precedence nothing computes is a comment.
 *
 *    contradictions ABSENT -> the A7 audit did not run.
 *    contradictions []     -> it ran and found none.
 *    Blurring those two turns "we did not look" into "there is nothing", which
 *    is the single most expensive substitution in this plan.
 *
 * ── sync_memory ──────────────────────── §4 C6u / D3 / D4 ────────────
 *
 *  PROPOSED, and the one row here I would cut first: §4 asks for sync as a
 *  union of append-only records and for a feed interface, and neither says the
 *  trigger must be an MCP tool rather than `hyp sync` on the CLI. It is in the
 *  table because F3 reports a memory lag and reporting a lag with no reachable
 *  way to act on it is a dead end; delete the row if the CLI is the answer.
 *
 *    direction   REQUIRED: "pull" | "push" | "both". Required for the same
 *                reason delete_project keeps `project` required: an operation
 *                that moves records between machines does not infer which way.
 *    feed        OPTIONAL adapter name. ABSENT means every configured feed.
 *    dry_run     OPTIONAL, default false.
 *
 *  Returns: { feed, direction, pulled, pushed, local_total, upstream_total,
 *  complete } — D4's completeness audit, indexed count against the feed's own
 *  count(*), which is why §4 chose a pull: it is the only feed that can prove
 *  it has everything. `upstream_total` and `complete` are ABSENT when the
 *  feed could not be counted, by the same rule that governs memory.missing.
 *  A push that silently drops a session is invisible, and every count in this
 *  plan is a floor.
 *
 *  This is the only open-world row on the surface (HYP_TOOL_ANN_FEED_SYNC).
 *  Everything else Hyponoia advertises is local, and it should stay that way:
 *  Multica is A feed, never THE feed, and nothing in the core may depend on it
 *  existing.
 * ══════════════════════════════════════════════════════════════════════ */

#endif /* HYP_MCP_TOOL_SURFACE_H */
