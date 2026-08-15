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

**What the prefix is worth**, measured on the 60-question C++ benchmark by
encoding the same questions with and without it against the same documents.
These are `Qwen3-Embedding-0.6B`'s numbers, measured before the `ask` lane
swapped encoders. The lane now runs `voyage-4-nano` — the Cost table below is
nano's — and a prefix's value does not survive a model swap: Qwen3's contract
left documents bare, where nano marks documents as well as queries. The shape
of the finding holds; the magnitudes here are Qwen3's and are not restated:

| | median rank of the answer | recall@10 | MRR@10 |
|---|---|---|---|
| question encoded bare | 24.5 | 0.433 | 0.173 |
| **question behind the prefix** | **6.0** | **0.550** | **0.379** |

The prefix wins 50 of 60 questions and loses 5. For comparison, the reranker
that used to be here bought more recall@10 (+0.183 against the prefix's +0.117)
and almost no MRR@10 (+0.003 against +0.206) — it moved answers into the top ten
without moving them to the top of it. The prefix does both, for free, which is
why its wording is pinned by a test. (That +0.183 is a fact about this encoder,
not about the reranker: re-measured against a stronger retriever it shrinks to
+0.067 and turns MRR@10 *negative* — `engine/hyponoia/runs/RERANK-NANO/`.)

**It earns that where the words don't match, and nowhere else.** Those 60
questions were built to have a vocabulary gap — the developer says "deduplicate",
the code says "ICF". Run the same test where the query is the function's *own*
docstring and the prefix has nothing left to bridge:

| public benchmark | queries | what the prefix does |
|---|---|---|
| our 60 C++ questions (vocabulary gap by construction) | 60 | **+0.117 recall@10** |
| CoIR CosQA (Python) | 500 | +0.014 NDCG@10 |
| CoIR CodeSearchNet Java (docstring → code) | 10,955 | −0.002 |
| CoIR CodeSearchNet Go (docstring → code) | 8,122 | −0.003 |

So the prefix is doing its work exactly where `ask` claims its value, and is
inert where a plain text search would already have found the answer. That is the
argument at the top of this page, measured.

It is also a **contract with these particular weights**, not a free-standing
improvement. The same prefix in front of a model trained without one *destroys*
it: `pplx-embed-v1-0.6b` loses 19% on Java and 24% on Go when it is added, and
falls from 0.567 to 0.333 recall@10 on the C++ questions. A different encoder is
not a drop-in even when its dimensions match. That has since held for four more
encoders, each wanting a different query-side contract, so treat every number in
this section as belonging to these weights and not to the idea of a prefix.

## It is opt-in, and says so

The vectors come from a second pass that you run deliberately:

```bash
hyponoia fetch-model            # 363 MB, pinned revision, SHA-256 verified
hyponoia embed --project my-project
```

Until that index exists, `ask` answers `available: false` with the exact remedy
and **no rows** — never an empty result set. "Your codebase has no such code"
and "the index is not built" are different claims, and the tool never conflates
them.

Every answer discloses the model, the language whose prefix was rendered,
whether any declaration was truncated, and the population searched.

## Escalation: a stronger model for one question

`ask(escalate=true)` brings a hosted embedding model into a single question.
It is off by default and **never chosen for you** — every score threshold this
project has set has died on a corpus change, so the engine does not decide
when you need to be right. Configure a provider once (`ask.escalation.provider`,
`.model`, `.key_env` — the *name* of the variable holding the key, never the
key) and then choose per call.

What an escalated question sends is `ask.escalation.mode`:

| mode | what leaves the machine | index it scores | gate |
|---|---|---|---|
| **`query`** (default) | the question — ~30 tokens | the **local** index you already have | both sides must share a **measured** embedding space |
| `index` | every declaration, once (`hyponoia embed --escalation`) | a second, API-built index | none needed: one model built and queries it |

**Query mode** is the cheap side to buy — a question against a corpus of
millions — and it keeps the code on the machine. It is licensed on quality:
nano documents scored by `voyage-4-large` questions beat nano on both sides by
+0.2087 MRR@10 on the 60 vocabulary-gap questions and by +0.444% reciprocal
rank (p = 5.5e-05) on 8,122 public CodeSearchNet-Go queries. Its case *over*
index mode is operational, not a quality margin: index mode replicates at that
scale too, and the two are statistically indistinguishable there (p = 0.62).
So the default is the mode with no second index, no drift between two indexes,
no corpus-sized bill, and nothing but the question sent anywhere.

**The gate is a space identity, not the prefix contract.** A cosine between two
models' vectors means something only if they live in the same embedding space,
and cross-space failure is silent — score a Qwen3 index against a voyage query
and nothing errors; the ranking comes back ordinary-looking and wrong, the worst
failure available to a tool an agent trusts. So the index stamps the space its
document encoder lives in, and query mode refuses unless that space and the
hosted model's are **both known and identical**. The allowlist is measured, not
copied from a vendor page: the voyage-4 family (`nano`, `lite`, `large`, `4`)
shares one space — self-retrieval acc@1 0.985–0.995 in both directions —
while eight negative controls (a random rotation, Qwen3, `voyage-code-3`) all
correctly said no at the 0.005 chance floor. `voyage-code-3` — same vendor,
one generation back — is *not* in the space; Jina and Gemini are unmeasured
and therefore refused. The refusal names both model ids and both spaces.

**Nothing falls back silently.** An unconfigured lane, an unset key variable,
an unbuilt index or a space that cannot be proven shared is an error that says
so — never a quiet answer from the local encoder that leaves you believing you
had the expensive one. Every answer carries `lane` (`local`,
`escalation-query` or `escalation-index`), `query_encoder` and
`index_encoder`, so the agent holding it knows what it is holding.

The record is `engine/hyponoia/runs/XMODEL/` (the frozen 60 and the space
probe) and `engine/hyponoia/runs/XMODEL-PUBLIC/` (the 8,122-query replication).

## What it scores

Measured on **CoIR's CosQA** — 500 public queries, 20,604 documents, scored
with CoIR's own evaluator:

| | NDCG@10 | measured by |
|---|---|---|
| BM25 (lexical floor) | 13.96 | the CoIR paper |
| Voyage-Code-002 (commercial) | 29.79 | the CoIR paper |
| BGE-Base (best in the CoIR paper's table) | 32.76 | the CoIR paper |
| hyponoia `ask` — `Qwen3-Embedding-0.6B`, retired 2026-08-12 | 39.44 | us, **shipped binary** |
| **hyponoia `ask` — `voyage-4-nano`, SHIPPING** | **33.69** | us, **PyTorch lane** |
| CodeR-1.5B | 46.72 | the CoIR paper |
| Gemini-embedding (current frontier) | 50.24 | the CoIR paper |

**The third column is not decoration.** Three harnesses appear in that table and
they are not interchangeable — which is exactly how the number below it got
misquoted for three days.

**Anchor each number to the encoder that produced it.** 39.44 is Qwen3's, run
through the shipped binary (`runs/COIR/`). 33.69 is nano's, run through a
PyTorch/ROCm lane (`runs/RERANK-NANO/`) — no binary-lane CosQA number exists for
nano. The two lanes are not free to compare: re-running **Qwen3** through that
same PyTorch path scores 37.24, so the harness accounts for −2.20 of the gap and
the model for the rest.

**The swap cost us this benchmark, and that is worth saying plainly.** Paired on
the same lane, nano minus Qwen3 on CosQA is **−3.55 NDCG@10 (−9.53%), CI
[−6.24, −0.87], p = 0.0154** — significant, and the first public corpus where the
shipping model loses to the retired one.

**It did not reverse the swap, and the reason is corpus size, not stubbornness.**
The evidence the swap was decided on is **CLARC**: 1,245 distinct public C/C++
queries across eight split-by-setting combinations, 2,990 query-evaluations, on
which nano beats the retired encoder on **seven of eight, all significant**
(+4.62 to +19.23 NDCG@10), the eighth a tie (+1.15, p = 0.227). CosQA is 500
queries and one loss against that. Both CodeSearchNet languages also go to nano.

**The frozen 60-question set is not the argument and should not be quoted as
one.** It is a smoke test: nano is +0.100 recall@10 on it but a *tie* on MRR@10
(+0.5%, p = 0.577), which is what a 60-question set buys you — one query moves
recall@10 by 0.0167. Lead with CLARC.

Nano's 33.69 sits above every model in the paper's CosQA column, including two
commercial ones — but by 0.93 rather than Qwen3's 6.68, and across a lane
boundary, so read that as "not obviously worse than the paper's table" rather
than as a win. It does not lead the field.

**Read it against the ceiling, not against 100.** CosQA's corpus is 86%
duplicate text — 20,604 documents are only 6,267 distinct strings, and 426 of
the 500 golds have a byte-identical twin the qrels mark irrelevant. A *perfect*
content-based retriever caps at **64.88**. So nano's 33.69 is **52% of what is
achievable** and Qwen3's 39.44 was 61%; BGE-Base is 50%, BM25 is 22%.

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

**Those numbers are Qwen3's, and the swap changed them.** Re-run with
`voyage-4-nano` retrieving, the sign flips: **+4.80%, p = 0.288** — not a
regression, and not a win either. Saying "the reranker no longer regresses on
CosQA" without that second clause would be true and misleading, because what
changed is the retriever it sits on top of: a reranker replaces the retriever's
ranking with its own, so it rescues a weak one and drags a strong one down. The
deletion stands on the grounds below, which the swap does not touch.

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
| model | `voyage-4-nano`, Q8_0 GGUF (355 MB) **plus** an 8 MB projection head, fetched once |
| embedding | 36.7 declarations/s on this GPU — measured, see below |
| query | ~0.28 s warm, dominated by encoding the question |
| index | float32, dim 1024, one row per declaration |

The model changed in §2.10 and the throughput figure is the one that was
measured rather than the one that was assumed. Both binaries were built from a
single branch and run back to back over the same 4,072 declarations with §2.2's
whole-file-span drop on: **Qwen3-Embedding-0.6B 17.0 declarations/s, nano 29.3
at the inherited batch cap, 36.7 once that cap was re-measured** — nano is
2.16× faster, not the 2.4× *slower* an earlier run reported by benchmarking one
side with an optimisation switched off. `HYP_ASK_MAX_DOCS` was Qwen3's measured
peak of 8 and had never been re-measured; nano peaks at 16.

nano ships as **two** artifacts because GGUF cannot represent its output
projection — a bias-free `nn.Linear(1024, 2048)` that the model applies after
pooling. `fetch-model` takes both, each pinned to one revision and SHA-256
verified. Applying that head after llama.cpp pools is exact rather than
approximate: it is a linear map and the pooler is a mean, so the two commute.

The GPU path is opt-in at build time (`make HYP_ASK_GPU=vulkan`) and needs a
Vulkan SDK. The default build is CPU-only and portable. Only the `embed` pass
is affected; `ask` itself is unchanged either way.

## Why this model and not another

A survey of 31 candidates against the MTEB data found exactly one
uncontaminated same-size alternative worth measuring:
`perplexity-ai/pplx-embed-v1-0.6b` — same parameters, same dim 1024, same
32,768 context, MIT, and a published CosQA well above ours.

It was measured on four benchmarks and **declined**. On CosQA it really is
better — NDCG@10 41.26 against 38.11 in the same harness, a genuine +8.3%
(p = 0.030). But CosQA is 500 queries and every one is Python, so the gain was
re-tested in two languages neither model's authors nor we chose:

| | queries | language | pplx against this model |
|---|---|---|---|
| CoIR CosQA | 500 | Python | **+8.3%** (p = 0.030) |
| CoIR CodeSearchNet Java | 10,955 | Java | −0.8% (p < 0.001) |
| CoIR CodeSearchNet Go | 8,122 | Go | −0.6% (p < 0.001) |
| our 60 questions | 60 | C++ | −12% MRR@10 (p = 0.25, underpowered) |

The gain is a property of CosQA and Python, not of the weights: the two tests
with twenty times CosQA's statistical power both go the other way. Adopting it
would also cost a converter for an architecture llama.cpp does not register, a
second pooling path, and the instruct prefix above.

Anything larger loses outright: bge-code-v1 at 37.52, Qwen3-4B at 37.98 and
Qwen3-8B at 38.04 are all below what we already score, and their dimensions
scale the index 1.5× to 4×.

**This section closed the question against the candidates it had, and the
question was reopened afterwards.** Everything above is the state as of
2026-08-11. Since then `voyage-4-large` cleared the bar by a wide margin through
an API, `voyage-4-nano` was measured as the open-weights model that keeps a
usable fraction of it, and `Alibaba-NLP/UEmbed-2B` was measured and declined.
The local encoder this page documents is the one the code on this branch
fetches; when that changes, every figure above changes with it. The records are
`engine/hyponoia/runs/VOYAGE/`, `VOYAGE-NANO/`, `CLARC/`, `ESCALATION/` and
`UEMBED/`.

## Full records

Every number here is reproducible from the run records:
`engine/hyponoia/runs/COIR/` (the public benchmark),
`engine/hyponoia/runs/RERANK-COST/` (the reranker's removal),
`engine/hyponoia/runs/PPLX/` (the prefix measurement and the declined swap),
`engine/hyponoia/runs/MODEL-SURVEY/` (the 31 candidates), and
`engine/hyponoia/runs/ASK/` (how the lane was built).
