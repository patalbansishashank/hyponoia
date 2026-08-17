# Hyponoia documentation

[← README](../README.md)

> **A note on `#NNN` in code comments.** Hyponoia is a fork of
> [DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp),
> and roughly 458 comments across 77 files cite issue numbers from **that**
> project's tracker — `#513`, `#581`, `#636` and so on. They are not issues in
> this repository, which does not use an issue tracker at all. They are left
> exactly as written because each one records *why* the code around it exists,
> and renumbering or deleting them would throw that away to fix a cosmetic
> problem. Read them as upstream history.

## Start here

| | |
|---|---|
| [**`ask` — the semantic lane**](ASK.md) | One question, ranked declarations. How it works, what it scores on a public benchmark, and where it stops working. |
| [Installation](INSTALL.md) | Build from source, MCP configuration, updating, uninstall. |
| [MCP tools & CLI mode](TOOLS.md) | All 18 tools, and running any of them as a one-shot command. |

## Reference

| | |
|---|---|
| [Graph data model](GRAPH.md) | Node labels, edge types, qualified names, the supported Cypher subset. |
| [Architecture, Hybrid LSP, languages](ARCHITECTURE.md) | The two-pass pipeline, per-language type resolution, the 158-language tier table. |
| [Agent surfaces](AGENTS.md) | The 43-client matrix, what `install` writes, and where it deliberately does nothing. |
| [Running Hyponoia](OPERATIONS.md) | Ignore rules, configuration, environment variables, persistence, troubleshooting. |
| [Configuration files](CONFIGURATION.md) | The full config-file reference. |
| [Third-party licences](THIRD_PARTY.md) | Every vendored dependency and its licence. |
| [Maintainers](MAINTAINERS.md) | Who owns what, and how decisions are made. |
| [Packaging and registry submission](PACKAGING.md) | Where each package-manager manifest would be submitted, how to validate it first, and which registries this project should not submit to. |
| [`.hypignore`](hypignore.md) | Syntax, precedence across ignore layers, negation semantics. |
| [Watched-directory ingest](WATCHED-INGEST.md) | The generic feed format: append-only JSONL any harness can write, and how origins make re-ingest idempotent. |

## Measurements and plans

| | |
|---|---|
| [Benchmarks](BENCHMARK.md) | Indexing and query timings. |
| [Evaluation plan](EVALUATION_PLAN.md) | How a full 159-language evaluation would be run. A **plan, not results** — not yet executed. |
| [Swapping the embedding table](EMBEDDING-SWAP.md) | How to replace the static token-vector table compiled into the binary. Note this is the **static table** used by `search_graph(semantic_query=…)`, not the `ask` lane's model. |

## Security

| | |
|---|---|
| [Security policy](../.github/SECURITY.md) | How to report an issue. |
| [Disclosure process](SECURITY-DISCLOSURE.md) | What happens after you report one. |

## Where the numbers come from

Retrieval claims in [ASK.md](ASK.md) are reproducible from the run records kept
outside this repository, under `engine/hyponoia/runs/`:

- `COIR/` — the public CoIR benchmark: CosQA and CodeTransOcean-DL, scored with
  CoIR's own evaluator, plus the duplicate-text ceiling that reframes them.
- `RERANK-COST/` — why the cross-encoder reranker was measured and then deleted.
- `PPLX/` — what the instruct prefix is worth, and the same-size model that was
  measured and declined.
- `MODEL-SURVEY/` — 31 candidate encoders against the MTEB data, and what each
  would cost.
- `VOYAGE/`, `VOYAGE-NANO/`, `CLARC/`, `UEMBED/` — the encoders measured after
  that survey: one accepted through an API, one taken as the open-weights
  model, one declined, and a public C/C++ coordinate for all of them.
- `RERANK-NANO/` — the cross-encoder re-measured against a stronger retriever,
  and why its gain was never a property of the reranker.
- `ASK/` — how the semantic lane was built, and the four levers tried on it.
- `EMBED-SWAP/` — the Qwen3 static-table swap that was measured and rejected.

Each directory leads with a `verdict.json` that states the question, the answer
and the date before any supporting detail.
