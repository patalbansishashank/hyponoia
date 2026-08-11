# Hyponoia

[![Release](https://img.shields.io/badge/release-v0.2.4-blue)](https://github.com/patalbansishashank/hyponoia/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![CI](https://img.shields.io/badge/CI-passing-brightgreen)](https://github.com/patalbansishashank/hyponoia/actions/workflows/ci.yml)
[![Tests](https://img.shields.io/badge/tests-6610_passing-brightgreen)](https://github.com/patalbansishashank/hyponoia)
[![Languages](https://img.shields.io/badge/languages-158-orange)](docs/ARCHITECTURE.md#language-support)
[![Hybrid LSP](https://img.shields.io/badge/Hybrid_LSP-10_languages-blue)](docs/ARCHITECTURE.md)
[![Agents](https://img.shields.io/badge/agent_surfaces-43-purple)](docs/AGENTS.md)
[![No runtime](https://img.shields.io/badge/single_binary-no_language_runtime-blue)](docs/INSTALL.md)

**A code context engine for AI coding agents.** Hyponoia indexes your
repositories into a persistent knowledge graph and answers structural questions
against it in under a millisecond — so an agent asks where the code is instead
of reading around until it finds out. It holds codebases far larger than any
context window and hands back exact spans with line numbers, not whole files.

The name is Greek — ὑπόνοια, *hyponoia*: the meaning underneath the surface
text. That is the job.

<p align="center">
  <img src="docs/graph-ui-screenshot.png" alt="Graph visualization UI showing the Hyponoia knowledge graph" width="760">
  <br>
  <em>Built-in 3D graph visualization (UI variant) at localhost:9749</em>
</p>

## Two ways to find code

Most tools give you one. The difference between them is the whole design.

**Structural — you know what it is called.** `search_graph`, `trace_path`,
`query_graph` walk a real graph: functions, classes, call chains, HTTP routes,
cross-service links. Sub-millisecond, exact, and it can answer questions about
*absence* — what has no callers, what nothing imports.

**Semantic — you know what it does.** [`ask`](docs/ASK.md) takes one
natural-language question and returns ranked declarations, encoded whole by a
transformer, so an answer can match code that shares **none** of your question's
words.

```
ask("which pass folds together read-only sections holding identical contents?")
→ lld-elf.ICF.run    ICF.cpp:464-580
```

`ask` scores **39.44 NDCG@10 on CoIR's CosQA**, ahead of every model in the
CoIR paper's column for that task including two commercial ones — and behind
the current frontier. It is opt-in, it tells you when its index isn't built
instead of returning nothing, and it has a boundary it will admit to.
[The full measurement, and the reranker we deleted because it did not survive
it →](docs/ASK.md)

## Quick start

There are no published binaries yet. Building takes two commands:

```bash
git clone https://github.com/patalbansishashank/hyponoia.git
cd hyponoia && scripts/build.sh
```

Point your agent at `build/c/hyponoia` ([MCP config](docs/INSTALL.md#manual-mcp-configuration)),
restart it, and say **"Index this project."**

For the semantic lane, one extra opt-in pass:

```bash
hyponoia fetch-model                    # 639 MB, pinned and SHA-256 verified
hyponoia embed --project my-project
```

[Full installation guide →](docs/INSTALL.md)

## Why

- **Fast** — Linux kernel (28M LOC, 75K files) in 3 minutes. RAM-first
  pipeline; memory released after indexing.
- **120× fewer tokens** — five structural queries: ~3,400 tokens against
  ~412,000 for file-by-file search.
- **158 languages** — vendored tree-sitter grammars compiled in. Nothing to
  install, nothing to break.
- **[Hybrid LSP](docs/ARCHITECTURE.md)** — real type resolution for 10
  languages, in C, with no language-server process.
- **Local** — no API key, no hosted service, no telemetry. Your code never
  leaves the machine.
- **[43 agent surfaces](docs/AGENTS.md)** — `install` configures detected
  clients and touches conditional ones only when their documented marker is
  present.

## The 16 tools

| | |
|---|---|
| **Index** | `index_repository` · `list_projects` · `index_status` · `delete_project` · `check_index_coverage` |
| **Find** | `search_graph` · [`ask`](docs/ASK.md) · `search_code` · `get_code_snippet` |
| **Understand** | `trace_path` · `query_graph` · `get_graph_schema` · `get_architecture` |
| **Change** | `detect_changes` · `manage_adr` · `ingest_traces` |

Every one of them also runs as a one-shot command:
`hyponoia cli search_graph --project my-project --label Function`.
[Tool reference and CLI mode →](docs/TOOLS.md)

## Documentation

| | |
|---|---|
| [**`ask` — the semantic lane**](docs/ASK.md) | How it works, what it scores, where it stops |
| [Installation](docs/INSTALL.md) | Build, install, MCP config, updating |
| [MCP tools & CLI](docs/TOOLS.md) | All 16 tools, one-shot invocation |
| [Graph data model](docs/GRAPH.md) | Labels, edges, qualified names, Cypher subset |
| [Architecture](docs/ARCHITECTURE.md) | Pipeline, Hybrid LSP, language tiers, performance |
| [Agent surfaces](docs/AGENTS.md) | The 43-client matrix, hooks, subagent profiles |
| [Running it](docs/OPERATIONS.md) | Daemon, UI, config, ignore rules, troubleshooting |
| [Benchmarks](docs/BENCHMARK.md) | Indexing and query measurements |
| [**All documentation →**](docs/README.md) | Everything else, indexed |

## How it works

Hyponoia is a **structural analysis backend**. It builds and queries the graph;
it does **not** contain an LLM. Your MCP client is already the intelligence
layer, so it is also the query translator — no extra key, no extra model.

```
You:      "what calls ProcessOrder?"
Agent:    trace_path(function_name="ProcessOrder", direction="inbound")
Hyponoia: executes the graph query, returns structured spans
Agent:    explains the call chain
```

## Security

Static analysis and supply-chain scanning (CodeQL, OpenSSF Scorecard) are not
wired up here: both need a public repository or a paid plan, so they were
removed rather than left failing. They come back when the repository goes
public.

This tool reads your codebase and writes to your agent's configuration files —
that is what it is for. All processing is local and nothing is sent anywhere.
Hyponoia makes **no network request of its own accord**; the only one it will
ever make is the model download you ask for by running `fetch-model`, from a
pinned revision verified against a SHA-256 compiled into the binary.

Found a security issue? See [SECURITY.md](.github/SECURITY.md).

## License

MIT — see [LICENSE](LICENSE). Includes vendored dependencies under their own
licences; `scripts/audit-license-provenance.py` checks each one byte-for-byte
against upstream.
