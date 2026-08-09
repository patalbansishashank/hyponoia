# Swapping the pretrained embedding table

How to replace the static token-vector table baked into the `hyponoia` binary.
Written while replacing nomic-embed-code with Qwen3-Embedding-0.6B, and proved
out by regenerating the *existing* nomic table and rebuilding — an identity
test, no model involved. Evidence: `engine/hyponoia/runs/EMBED-SWAP/E-wiring.json`.

Read this before you touch anything under `vendored/`.

---

## What the table is

`vendored/nomic/` holds five generated files. NEXT-STEPS calls it four; there
are five, and the one it omits — `code_vectors.h` — is the one the C code
actually includes.

| file | what it is | who can regenerate it |
|---|---|---|
| `code_vectors.bin` | `[int32 count][int32 dim]` + `count × dim` int8 | the model |
| `code_tokens.txt` | one cleaned token per line, sorted, deduplicated | the model's **tokenizer** |
| `code_tokens.h` | `PRETRAINED_TOKENS[N]` C array | anyone, from `code_tokens.txt` |
| `code_vectors.h` | `PRETRAINED_TOKEN_COUNT` / `PRETRAINED_DIM` + accessor | anyone, from the other two |
| `code_vectors_blob.S` | `.incbin` wrapper, three object formats | anyone, from nothing |

Only the first two need the model. The other three are pure functions of them,
which is why the pipeline is testable without a GPU — see
[Verify before you trust it](#verify-before-you-trust-it).

## The four wiring points

Not two. Getting three of four right still produces a binary; it is just the
wrong binary.

1. **`Makefile.hyp:348`** — `UNIXCODER_BLOB_SRC = vendored/nomic/code_vectors_blob.S`
2. **`Makefile.hyp:874`** — the object rule's second prerequisite,
   `vendored/nomic/code_vectors.bin`. **This one fails silently.** Leave it
   pointing at the old path and the build still succeeds; the blob object just
   never rebuilds when the vectors change, so you ship the old table and the
   new headers. That combination is what the runtime guard in
   `semantic.c` (`semantic.pretrained_blob_mismatch`) exists to catch.
3. **`src/semantic/semantic.c:17`** — `#include "nomic/code_vectors.h"`.
   Resolved through `-Ivendored` (`Makefile.hyp:53`), so the path is relative
   to `vendored/`.
4. **The `.incbin` path inside `code_vectors_blob.S` itself** — three
   occurrences, one per object format. The extractor writes them from
   `--output-dir`, so this one is correct automatically *if* you generate into
   the directory you intend to ship from.

## Where the artifacts live, and why

**They stay under `vendored/`, in a new sibling directory — `vendored/qwen3/` —
not by overwriting `vendored/nomic/`.**

Three reasons, in the order they matter:

- **The security gate cannot follow them out.**
  `scripts/security-vendored.sh` hardcodes its roots to `vendored/` and
  `internal/hyp/vendored/`, and `valid_vendored_path()` rejects any manifest
  entry outside them — a first-party path is not merely uncovered, it is a
  *structural failure* that also refuses `--update`. Moving the table to a
  first-party directory would therefore mean either dropping 31 MB of
  linked-verbatim binary out of integrity coverage, or editing a security gate
  to widen what its manifest accepts. The gate's own comment names this exact
  file as the reason opaque data is in scope: *"code_vectors.bin is linked
  verbatim through .incbin"*.
- **The licence decides, not the command that produced the file.** The vectors
  are the model's weights transformed and the vocabulary is the model's
  tokenizer output. Apache-2.0 governs them and attribution is required. Our
  authorship of `extract_nomic_vectors.py` does not make its output
  first-party any more than a compiler makes object files belong to the
  toolchain vendor. `vendored/` is where this repo keeps third-party material
  next to its LICENSE, its NOTICE and an integrity pin — all three of which
  these files need.
- **A new directory is what keeps upstream merges boring.** `vendored/nomic/`
  is upstream codebase-memory-mcp's directory (added in `8a06d78a`, touched by
  six upstream commits since). Overwriting it in place guarantees a conflict on
  every upstream cherry-pick that lands there. Adding a sibling never conflicts,
  and satisfies NEXT-STEPS §1's "our changes live in clearly separated
  directories" without moving anything out of the gate's reach.

The cost of a sibling directory is one line in `KNOWN_VENDORED` — which is the
gate's *own* documented mechanism for recording an explicit review decision, so
it tightens coverage rather than loosening it.

`vendored/qwen3/LICENSE` and `vendored/qwen3/NOTICE` are already in place.
`vendored/nomic/` is untouched and stays untouched until its vectors are
actually deleted.

## The procedure

### 1. Extract

```
python3 scripts/extract_nomic_vectors.py \
    --output-dir vendored/qwen3 \
    --header-guard-prefix HYP
```

Before running it, edit the **model identity block** at the top of the script:
`MODEL_NAME`, `MODEL_SIZE_LABEL`, `MODEL_LICENSE`. Every banner in every
generated file is derived from those, so no artifact can claim a provenance the
run did not have.

Do **not** raise `OUTPUT_DIM`. Qwen3-Embedding-0.6B emits 1024; the script
Matryoshka-truncates to `OUTPUT_DIM = 768` in two places — inside
`extract_embeddings` (per batch, before L2 normalization) and again in `main`
after extraction, where it re-normalizes. That truncation is *why* no C change
is needed. `HYP_SEM_DIM` stays 768.

### 2. Wire

Change the four points above from `nomic` to `qwen3`. Point 4 is already
correct if `--output-dir` was right.

### 3. Licence and attribution

- Fill in the token count and confirm the dimension in `vendored/qwen3/NOTICE`,
  and delete its `STATUS` section.
- Leave `vendored/nomic/LICENSE` and `NOTICE` alone until step 6.

### 4. Manifest

```
scripts/security-vendored.sh          # read what it says changed
scripts/security-vendored.sh --update # only after you have read it
scripts/security-vendored.sh          # must pass
```

Never run `--update` before reading the plain run. `--update` rehashes whatever
is on disk; the review is the whole control.

### 5. Rebuild and verify

```
make -f Makefile.hyp clean-c
make -f Makefile.hyp hyp                # must be clean under -Werror
make -f Makefile.hyp test-foundation    # 301 passed, 1 skipped
scripts/ci/check-binary-composition.sh  # GNU_STACK must not be RWE
```

### 6. Only then, remove nomic

In one commit: delete `vendored/nomic/`, drop `nomic` from `KNOWN_VENDORED` in
`scripts/security-vendored.sh`, drop it from the model loop in
`scripts/audit-license-provenance.py`, delete the nomic block in
`THIRD_PARTY.md`, re-run `--update`. Not before — a licence must not be deleted
while the code it covers is still linked into the binary.

## Verify before you trust it

```
python3 scripts/verify-vector-artifacts.py vendored/qwen3
```

This regenerates every artifact that does not need the model, through the same
`write_artifacts()` entry point a real extraction uses, and compares sha256
against what is committed. It loads no model and needs no GPU.

It exists because the first time it was run against `vendored/nomic/` it
failed, and what it found was not cosmetic: the committed
`code_vectors_blob.S` had been hand-fixed to emit ELF and COFF branches plus a
`.note.GNU-stack` section, while the generator still emitted the Mach-O branch
alone. Re-running the extractor would have silently reverted that fix — an
executable-stack regression on every Linux build, and a link failure on ELF and
Windows. The generator has since been repaired to emit what the tree has.

Two things this check cannot prove, and does not claim to:

- `code_tokens.txt` — regenerating it from itself proves nothing. The
  vocabulary comes from the upstream tokenizer.
- the vector **values** — only the container format and the int8 quantizer
  round-trip are proven.

## What fails loudly now, and what does not

Three guards were added because a dimension or vocabulary change used to
corrupt results silently:

- `semantic.c` — `_Static_assert(PRETRAINED_DIM == HYP_SEM_DIM)`. Without it, a
  table narrower than 768 makes every `pretrained_vec_at(i)` stride into the
  next token's row and the last token read past the end of `.rodata`, because
  `sem_vec_add_int8_scaled` reads `HYP_SEM_DIM` int8 from that pointer with no
  bound.
- `semantic.h` — `_Static_assert(HYP_RSQ_IN_DIM == HYP_SEM_DIM)`. `rotsq.h`
  declares its own `768` and only a comment tied the two together.
- `semantic.c` — a one-time runtime check that the blob's own
  `[count][dim]` prefix and `PRETRAINED_VECTOR_BLOB_LEN` agree with the
  generated header. This is the only guard that can catch wiring point 2,
  because the `.bin` is invisible to the compiler. On mismatch it logs
  `semantic.pretrained_blob_mismatch` at error level and leaves the lookup map
  empty, so every token falls back to the hashed random-index vector rather
  than reading out of bounds.

**The token count is not guarded**, and does not need to be: every loop bounds
itself with `PRETRAINED_TOKEN_COUNT` from the same generated header, and the
runtime check compares that count against the blob's own prefix.

**Not a build concern, but do not skip it:** the blend weights
(`HYP_SEM_W_RI` among eleven signals) and `HYP_SEM_EDGE_THRESHOLD` were tuned
against nomic's score distribution. A different model has a different
distribution. See NEXT-STEPS §2 "Merge and measure".
