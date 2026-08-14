# Hyponoia documentation

[← README](../README.md)

## Start here

| | |
|---|---|
| [**`ask` — the semantic lane**](ASK.md) | One question, ranked declarations. How it works, what it scores on a public benchmark, and where it stops working. |
| [Installation](INSTALL.md) | Build from source, MCP configuration, updating, uninstall. |
| [MCP tools & CLI mode](TOOLS.md) | All 16 tools, and running any of them as a one-shot command. |

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
| [`.hypignore`](hypignore.md) | Syntax, precedence across ignore layers, negation semantics. |

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
