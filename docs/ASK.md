# `ask` — the semantic lane

`ask` takes **one natural-language question** and returns ranked declarations
with line ranges. It exists for the case the structural lane cannot serve: when
you can describe what code *does* but not what it is *called*.

```
ask(project="lld-elf", question="which pass folds together read-only sections
                                 that hold byte-identical contents?")

→ lld-elf.ICF.run            ICF.cpp:464-580
  lld-elf.ICF.doIcf          ICF.cpp:583-586
```

Nothing in that question appears in the answer. The code calls the concept ICF
and works in "equivalence classes"; the developer asking for it says
"deduplicate". Lexical search scores ~0 on questions like that by construction.

## How it differs from the other two search tools

| | what you give it | what it matches against |
|---|---|---|
| `search_graph(query=…)` | text | BM25 over **names**, via SQLite FTS5 |
| `search_graph(semantic_query=[…])` | an **array of keywords** | a static per-token vector table compiled into the binary |
| **`ask(question=…)`** | **one string** | **whole declarations, encoded by a transformer** |

The asymmetry is the point. Documents are encoded **bare**; the question is
encoded behind an **instruct prefix**. Those are separate entry points in the
code with no boolean flag between them, because getting it backwards still
produces a plausible ranking and there is no downstream signal that anything
went wrong.

## It is opt-in, and says so

The vectors come from a second pass that you run deliberately:

```bash
hyponoia fetch-model            # 639 MB, pinned revision, SHA-256 verified
hyponoia embed --project my-project
```

Until that index exists, `ask` answers `available: false` with the exact remedy
and **no rows** — never an empty result set. "Your codebase has no such code"
and "the index is not built" are different claims, and the tool never conflates
them.

Every answer discloses the model, the language whose prefix was rendered,
whether any declaration was truncated, and the population searched.

## What it scores

Measured on **CoIR's CosQA** — 500 public queries, 20,604 documents, scored
with CoIR's own evaluator:

| | NDCG@10 |
|---|---|
| BM25 (lexical floor) | 13.96 |
| Voyage-Code-002 (commercial) | 29.79 |
| BGE-Base (best in the CoIR paper's table) | 32.76 |
| **hyponoia `ask`** | **39.44** |
| CodeR-1.5B | 46.72 |
| Gemini-embedding (current frontier) | 50.24 |

A 0.6B open model beats every model in the paper's CosQA column, including two
commercial ones. It does not lead the field.

**Read it against the ceiling, not against 100.** CosQA's corpus is 86%
duplicate text — 20,604 documents are only 6,267 distinct strings, and 426 of
the 500 golds have a byte-identical twin the qrels mark irrelevant. A *perfect*
content-based retriever caps at **64.88**. So 39.44 is **61% of what is
achievable**; BGE-Base is 50%, BM25 is 22%.

## Where it stops working

**CodeTransOcean-DL: 23.97, against BM25's 50.13.** That task retrieves code
using code as the query, so the query and the document share vocabulary and
lexical matching nearly solves it. A semantic lane has no advantage there and
ours loses badly. That is a real boundary, not a defect — and it is the reason
`search_graph` still exists.

`ask` also refuses a question longer than 4,096 characters. It encodes a
*question*, not a document.

## The reranker that used to be here

§2.2 added a cross-encoder that re-read the top N candidates, on the strength
of **+0.183 recall@10** on a 60-question benchmark. §2.3 and §2.4 removed it.

It **regressed** on CosQA — NDCG@10 0.394 → 0.352, recall@10 0.674 → 0.602,
89 queries better against 150 worse, sign test p=9.6e-05. Two obvious confounds
were tested and cleared: not the duplicate text (the regression is *larger*,
−0.054, on the 74 queries whose gold has no twin) and not query length.

Then the original win turned out to be explicable. The benefit on lld/ELF is
significantly predicted by how much each question's vocabulary appears in the
gold declaration's **body** — Spearman **0.325, p=0.0145**; queries it helped
had mean overlap 0.234 against 0.147 for queries it hurt. Those 60 questions
were written by someone reading the code, and the benchmark's "un-greppable"
property had only ever been verified against **names**, never bodies. A
bi-encoder compresses each side to one vector and loses that token evidence; a
cross-encoder reads both together and recovers it.

So the reranker was substantially an expensive lexical matcher over declaration
bodies: 639 MB and seconds per query to do what a full-text index does in
microseconds. It was deleted — 1,784 lines, the second model, and the tool
parameter.

The lesson is recorded rather than the code: the next retrieval work worth
doing is **indexing declaration bodies for lexical search**, which §2.2's
rank-fusion lever had already named as its prerequisite.

## Cost

| | |
|---|---|
| model | Qwen3-Embedding-0.6B, Q8_0 GGUF, 639 MB, fetched once |
| embedding | ~77 declarations/s on a GPU, ~15× slower on CPU |
| query | ~0.28 s warm, dominated by encoding the question |
| index | float32, dim 1024, one row per declaration |

The GPU path is opt-in at build time (`make HYP_ASK_GPU=vulkan`) and needs a
Vulkan SDK. The default build is CPU-only and portable. Only the `embed` pass
is affected; `ask` itself is unchanged either way.

## Full records

Every number here is reproducible from the run records:
`engine/hyponoia/runs/COIR/` (the public benchmark),
`engine/hyponoia/runs/RERANK-COST/` (the reranker's removal), and
`engine/hyponoia/runs/ASK/` (how the lane was built).
