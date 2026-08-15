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

## Use the "name" returned by list_projects as the project value.
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
| `ask` | Ask ONE natural-language question; ranked declarations with line ranges. The semantic lane — documents encoded whole and bare, the question behind an instruct prefix, so an answer can match code sharing none of the question's words. Reports `available: false` with a remedy (never zero results) until the opt-in semantic index is built. `escalate=true` (off by default, per question) brings a configured hosted model in: in the default `ask.escalation.mode=query` only the question is sent and scored against the local index (refused unless both are in a measured shared space); `index` mode queries a second API-built index. Every answer names `lane`, `query_encoder` and `index_encoder`; nothing falls back silently. |
| `trace_path` | BFS traversal — who calls a function and what it calls (alias: `trace_call_path`). Depth 1-5. |
| `detect_changes` | Map git diff to affected symbols + blast radius with risk classification. |
| `query_graph` | Execute Cypher-like graph queries (read-only). |
| `get_graph_schema` | Node/edge counts, relationship patterns, property definitions per label. Run this first. |
| `get_code_snippet` | Read source code for a function by qualified name. |
| `get_architecture` | Codebase overview: languages, packages, routes, hotspots, clusters, ADR. |
| `search_code` | Grep-like text search within indexed project files. |
| `manage_adr` | CRUD for Architecture Decision Records. Query modes do not wait behind a same-project reindex; writes remain serialized. |
| `ingest_traces` | Ingest runtime traces to validate HTTP_CALLS edges. |

`manage_adr` query modes (`get` and `sections`) use the server's cached query store so they can proceed while a same-project reindex is running. If another process publishes a replacement store during reindexing, they can return the pre-publication ADR until idle eviction refreshes that cache. Updates remain serialized through the project mutation guard.
