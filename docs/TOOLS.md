# MCP tools and CLI mode

The 16 tools, and how to call any of them as a one-shot command.

[← README](../README.md)

## CLI Mode

Every MCP tool can be invoked as a local, one-shot command. CLI tools neither start nor connect to the coordination daemon and leave no standing process behind. They hold a crash-safe exact-build admission lease only for the command lifetime. `index_repository` is the only exception internally: it starts a temporary, exact-build supervised worker for the index, then stops that worker before the CLI command exits; the worker holds its own lease until exit.

Commands that mutate graph data use shared OS-backed, per-project locks. This serializes conflicting work from CLI and MCP sessions on the same project while allowing unrelated projects to proceed independently.

When stderr is an interactive terminal, the CLI automatically shows lifecycle and indexing progress. Pass `--progress` to force the same feedback when stderr is redirected or the command is run non-interactively. Progress is written only to stderr; stdout remains reserved for the command result, so pipes and scripts stay machine-safe. Pass `--json` when the full MCP result envelope is needed.

Use `cli <tool> --help` to see the flags generated from that tool's input schema:

```bash
hyponoia cli index_repository --repo-path /path/to/repo
hyponoia cli list_projects

## `--project` is optional: omitted, the tool uses the project for the
## working directory it runs in. Pass it to override, or when you are not
## standing in the repository you mean.
hyponoia cli search_graph --name-pattern '.*Handler.*' --label Function
hyponoia cli search_graph --project my-project --name-pattern '.*Handler.*' --label Function
hyponoia cli trace_path --project my-project --function-name Search --direction both
hyponoia cli query_graph --project my-project --query 'MATCH (f:Function) RETURN f.name LIMIT 5'

## Force human-readable progress without contaminating stdout.
hyponoia cli --progress index_repository --repo-path /path/to/repo
hyponoia cli search_graph --project my-project --label Function | jq '.results[].name'
```

JSON arguments can also be piped on stdin. Inline JSON remains accepted for backward compatibility but is deprecated in favor of flags, `--args-file`, or stdin.

## MCP Tools

## Indexing

| Tool | Description |
|------|-------------|
| `index_repository` | Index a repository into the graph. Auto-sync keeps it fresh after that. |
| `list_projects` | List all indexed projects with node/edge counts. |
| `delete_project` | Remove a project and all its graph data. |
| `index_status` | Check indexing status of a project. |

## Querying

| Tool | Description |
|------|-------------|
| `search_graph` | Structured search by label, name pattern, file pattern, degree filters. Pagination via limit/offset. |
| `ask` | Ask ONE natural-language question; ranked declarations with line ranges **and the source of the top 2**, so a common-case question is one call rather than `ask` + `get_code_snippet`. 3 candidates by default (`limit` still honoured to 500); spans capped at 40 lines / 1600 bytes each and 3200 bytes total, and a span over the cap is marked `CUT` with its full range named — never silently shortened. `include_source=false` for coordinates only. An `exact` column appears only when the question spells a candidate's name — a fact about two strings, not a confidence. The semantic lane — documents encoded whole and bare, the question behind an instruct prefix, so an answer can match code sharing none of the question's words. Reports `available: false` with a remedy (never zero results) until the opt-in semantic index is built. `escalate=true` (off by default, per question) brings a configured hosted model in: in the default `ask.escalation.mode=query` only the question is sent and scored against the local index (refused unless both are in a measured shared space); `index` mode queries a second API-built index. Every answer names `lane`, `query_encoder`, `index_encoder` and `key_custody`; nothing falls back silently. A warm daemon will not read your API key on a client’s behalf unless `ask.escalation.daemon_key=allow` — see [ASK.md § Whose key pays](ASK.md#whose-key-pays). |
| `trace_path` | BFS traversal — who calls a function and what it calls (alias: `trace_call_path`). Depth 1-5. |
| `detect_changes` | Map git diff to affected symbols + blast radius with risk classification. |
| `query_graph` | Execute Cypher-like graph queries (read-only). |
| `get_graph_schema` | Node/edge counts, relationship patterns, property definitions per label. Run this first. |
| `get_code_snippet` | Read source code for a function by qualified name. |
| `get_architecture` | Codebase overview: languages, packages, routes, hotspots, clusters, ADR. |
| `search_code` | Grep-like text search within indexed project files. |
| `manage_adr` | CRUD for Architecture Decision Records. Query modes do not wait behind a same-project reindex; writes remain serialized. |
| `ingest_traces` | Ingest runtime traces to validate HTTP_CALLS edges. |

## Memory

The graph answers what the code does; these two answer why it is the way it is.
Records are **append-only** — there is no mode that edits or deletes one, because
merging two machines' stores is a union with no conflict resolution — and they
are **global**, not scoped to a project: a decision outlives the repository
layout it was taken in.

| Tool | Description |
|------|-------------|
| `record_memory` | Append one record: a `decision`, `verdict`, `summary` or `signal`, with a `title` and a `body`. Transcript kinds are refused — transcripts enter only through a feed, and a writer that could forge one would make the ingest completeness audit meaningless. To replace an earlier record, write a new one naming it in `supersedes`; the earlier one is never modified. There is **no author argument**: an author a caller can state is an author a caller can forge. Returns the record id. |
| `search_memory` | Read the store: by `kind`, by free text, by time range. Omitting `kind` searches every kind — it does not mean none. When no store exists on the machine yet the answer says so and sends **no** records list, because an empty list would claim nothing was ever recorded. |

`record_memory` is on **no tool profile**, deliberately: Scout and Analysis both
promise read-only surfaces, and a writer admitted to one would break that promise
rather than widen it. It is available on the full server. `search_memory` reads,
so Analysis admits it and Scout — the small surface where every tool answers
fast — does not.

**The memory store is per WORKSPACE**, at
`<cache>/memory/<workspace-id>/records.db`. That is the design sentence taken
literally — "the database is workspace-level; a single repo is a workspace of
one" — and it is what keeps one project's reasoning out of another's answers.

Which workspace a call belongs to is decided by `hyp_wsr_resolve`, the same and
only resolver the indexer uses, starting from the project's own indexed root
(so `--project beta` reads beta's workspace, not whichever one the server is
sitting in) and falling back to the session's working directory. Its precedence
is fixed: an explicit `hyponoia.toml` governs; then a provider; then
zero-config, which makes the directory a workspace of one.

The workspace id is deterministic and is a pure function of the member SET:
the TOML `name` when given, else the single member's slug, else
`ws-<sha256 over the sorted member slugs>`. Neither declaration order nor "the
first repo" can reach it, so two machines resolving the same set agree — which
is what lets two stores merge as a union.

What that means in practice:

| Shape | Memory |
|---|---|
| One repo, no config | Its own workspace; nothing else can see it. |
| Several repos in one `hyponoia.toml` | **One shared memory.** A decision recorded in `backend` is returned when working in `frontend` — that is the point of declaring them together. |
| A member with role `reference` / `upstream` / `vendored` (read-only) | Still a member: it reads and is read by the workspace. Role governs whether an agent may EDIT the tree, never who may see a decision. |
| Members coupled by a C-ABI/DLL call, or over HTTP | Identical. Coupling is a property of the code graph (`CROSS_*` edges), not of memory scope. Neither needs a special case. |

If the workspace cannot be determined the tools **refuse** rather than falling
back to a shared store — a silent fallback is exactly the bleed this removes,
and its answer would look identical to a correct one.

`HYP_MEMORY_DIR` still overrides the whole path outright, for tests and
isolated runs. **Note it does not isolate a store while a daemon is running**:
the daemon does the work with its own environment, so an isolated run needs
`HYP_DAEMON_RUNTIME_PARENT` and `HYP_CACHE_DIR` too.

A store written before this change sits at `<cache>/memory/records.db`. It is
not read: its records carry no workspace, so splitting it is impossible and
adopting it wholesale would restore the bleed. The tools say it is there rather
than reporting "nothing was ever recorded".

Anchoring a record to one symbol IS available. Both tools take an `anchor` — a
qualified name, the same one `search_graph` reports, or a canonical address —
and the resolver behind them is `src/memory/anchor.c`.

The two tools treat it asymmetrically, on purpose. `record_memory` resolves the
name against the live index **before** the record is built (it must: the anchor
is in the id preimage, so it cannot be attached afterwards) and REFUSES a name
that matches nothing or matches several, naming the candidates in the second
case. A record is never created already-orphaned and never attached to a
plausible neighbour. `search_memory` does **not** require the name to resolve,
because "what was decided about a function that has since been deleted" is
exactly the question the orphaned half of this surface exists to answer.

Every anchored answer carries `anchor_status`: `attached`, `attached_edited`
(still there, span changed since), `orphaned`, `ambiguous`, or `unknown` (no
index for that project — not checked, which is neither attached nor orphaned).
Unanchored records carry none, because a record about the repository has no
anchor to have lost. `status` filters on it, defaulting to `attached` only when
an `anchor` was supplied — with no anchor there is nothing to be attached to,
and defaulting otherwise would hide every repo-scoped record.

## The `project` argument

`project` is **optional on every tool that takes it except `delete_project`**. Omitted, the server resolves it in this order and says in the answer which rule applied:

1. **`supplied`** — an explicit `project` argument. It always wins and is never second-guessed.
2. **`derived from working directory <path>`** — the directory the MCP server was started in, which is the client's own working directory (the client spawns the server). The name is derived with the same rule the indexer used: the absolute path with its leading separator dropped and every other separator mapped to `-`, so `/home/u/repo` is `home-u-repo`. If that directory is not indexed, the nearest indexed ancestor of it is used and the disclosure names both paths.
3. **`the only indexed project`** — and only when exactly one project is indexed on the machine.

If none of these resolve — the working directory is not an indexed project and there is more than one candidate — the call returns a `which project?` error that **lists the candidates**, rather than picking one. A confidently wrong project is worse than a question.

Every successful answer carries two fields, `project` and `project_source`, so a caller can always tell which project answered and why. `delete_project` is the single holdout and still requires an explicit `project`: a destructive tool does not infer its target.

## Node properties worth knowing

`get_graph_schema` lists the property NAMES carried by each label and, under `property_notes`, the semantics of the three that are read wrong most often.

| Property | Type | Meaning |
|----------|------|---------|
| `is_test` | JSON boolean | True for **every** declaration — Function, Method, Class, Variable, Module — in a file the extractor classified as a test file. Classification is a per-language basename rule (`*_test.go`, `test_*.py`, `*.test.ts`, `*Test.java`, `*_test.c` / `test_*.c`, …) **or** a path with a `tests/`, `test/`, `spec/` or `__tests__/` directory segment. Additionally true for individual Rust `#[test]`/`#[tokio::test]` items and C++ GoogleTest macros inside a file that is not otherwise a test file. It is never guessed from a function's own name. In `query_graph` compare it with `= true`; `= 1` matches nothing. |
| `alloc_in_loop` | integer **count** | Allocation/append calls inside loops. NOT a boolean — `WHERE alloc_in_loop = true` matches nothing; use `> 0`. |
| `linear_scan_in_loop` | integer **count** | Linear-scan calls (find/contains/indexOf) inside loops. Same trap: use `> 0`. |

`search_graph`'s BM25 mode deprioritises `is_test` rows by 5.0 — half the Function/Method boost. They are ranked down, never filtered out, so a test still surfaces when it is clearly the best match for the query. The `name_pattern`/`qn_pattern` mode is a filter, not a ranker, and is unaffected; pass `fields: ["is_test"]` to see the flag on any row.

`manage_adr` query modes (`get` and `sections`) use the server's cached query store so they can proceed while a same-project reindex is running. If another process publishes a replacement store during reindexing, they can return the pre-publication ADR until idle eviction refreshes that cache. Updates remain serialized through the project mutation guard.
