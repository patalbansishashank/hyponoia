#!/usr/bin/env python3
"""
Extract token embeddings from nomic-embed-code (7B) for static lookup table.

Loads the full model, filters the vocabulary to code-relevant tokens,
runs full inference on each token, applies simulated attention, quantizes
to int8, and outputs files compatible with vendored/unixcoder/ format.

Usage:
    pip3.9 install torch transformers sentence-transformers
    python3.9 scripts/extract_nomic_vectors.py [--output-dir vendored/nomic]

Output:
    code_vectors.bin   — [int32 count][int32 dim] + count×dim int8
    code_tokens.txt    — one token per line
    code_tokens.h      — C header: static const char *PRETRAINED_TOKENS[N]
    code_vectors.h     — C header: defines + inline accessor
    code_vectors_blob.S — assembler .incbin

One-time extraction. ~2-3h on GPU, ~6-10h on M3 Pro CPU (float16, ~14GB RAM).
"""

import argparse
import json
import os
import re
import struct
import sys
import time
from pathlib import Path

import numpy as np
import torch

# Parallelize CPU inference across all cores BEFORE any torch ops
NUM_THREADS = min(os.cpu_count() * 2, 12)
torch.set_num_threads(NUM_THREADS)
torch.set_num_interop_threads(max(NUM_THREADS // 2, 1))
os.environ.setdefault("OMP_NUM_THREADS", str(NUM_THREADS))
os.environ.setdefault("MKL_NUM_THREADS", str(NUM_THREADS))

from transformers import AutoModel, AutoTokenizer


# ── Configuration ──────────────────────────────────────────────────────

# ── Model identity — THE BLOCK TO EDIT WHEN SWAPPING MODELS ────────────
#
# Everything the generated headers say about *which* model produced the table
# is derived from these four values. Changing MODEL_NAME re-brands the comment
# banner in code_vectors.h, the vocabulary banner in code_tokens.h and the blob
# banner in code_vectors_blob.S, so no generated file can claim a provenance
# the extraction did not have. See docs/EMBEDDING-SWAP.md.
MODEL_NAME = "nomic-ai/nomic-embed-code"
MODEL_SIZE_LABEL = "7B"   # appears verbatim in the code_vectors.h banner
MODEL_LICENSE = "Apache 2.0"
MODEL_URL = f"https://huggingface.co/{MODEL_NAME}"
MODEL_DISPLAY = MODEL_NAME.split("/")[-1]

# Include-guard prefix for the generated headers. Pinned to CBM because that is
# what the committed vendored/nomic/*.h carry, and scripts/verify-vector-artifacts.py
# proves this generator reproduces them byte-for-byte. Generating into a fresh
# directory (vendored/qwen3) is the moment to pass --header-guard-prefix HYP; the
# guard's middle segment is derived from the output directory name, so
# vendored/qwen3 + HYP yields HYP_QWEN3_VECTORS_H.
HEADER_GUARD_PREFIX = "CBM"

OUTPUT_DIM = 768          # Target dimension (Matryoshka truncation if model outputs more)
SIM_ATTENTION_K = 32      # Top-K neighbors for simulated attention
SIM_ATTENTION_ITERS = 3   # Number of simulated attention iterations
SIM_ATTENTION_ALPHA = 0.3 # Blend ratio: (1-α)×original + α×neighbor_mean
BATCH_SIZE = 32           # Tokens per inference batch (sized for thread saturation)
CHECKPOINT_EVERY = 500    # Save checkpoint every N tokens


# ── Vocabulary selection ──────────────────────────────────────────────
#
# The tokenizer's vocabulary is used ONLY as a source of candidate STRINGS.
# Vectors come from running each candidate through the full model
# (extract_embeddings, below) — no row is ever read out of the model's own
# embedding matrix. Two consequences drive every rule in this section:
#
#   * a candidate does not have to be a token of the target tokenizer, so the
#     vocabulary is a starting point and not a ceiling (see CODE_SUPPLEMENT);
#   * getting a marker rule wrong LOSES candidates, it never produces a wrong
#     vector for a candidate that survives. The failure mode is a quietly
#     smaller table, which is why _assert_vocabulary_sane exists.
#
# What the runtime can actually look up (src/semantic/semantic.c):
# hyp_sem_random_index() and build_src_entry() do an EXACT string match against
# PRETRAINED_TOKENS. On the corpus path every key first goes through
# hyp_sem_tokenize() (semantic.c:166), which lowercases, splits on
# ". / _ - space ( ) , :" and on camelCase boundaries, and drops every
# non-alphanumeric character. Its output alphabet is therefore exactly
# ^[a-z0-9]+$ — an entry outside that shape can never be reached on the corpus
# path, so it must not spend a 768-byte row.

# Admissible cleaned form. Lowercase because the runtime lowercases before it
# looks anything up: MAX_SIZE, MaxSize and maxSize all arrive as ["max","size"],
# so a case-preserving table could not be queried with a capitalised key at all.
# Letter-leading because relaxing that to allow digit-leading strings was
# measured to admit 1,697 numeric BPE fragments ("0000000000", "00000080") in
# order to cover 0.031% of sub-token occurrences on a 3.86M-token C corpus.
# No underscore, because the runtime splits on it.
ADMISSIBLE_TOKEN = re.compile(r"[a-z][a-z0-9]*\Z")

# Below this a string carries no stable meaning — "t" from every *_t typedef,
# "i"/"n"/"p" from loop and pointer variables — and the deterministic sparse
# random vector it falls back to is the better representation, because it
# supplies identity without dragging unrelated declarations together.
MIN_TOKEN_LEN = 2

# Subword markers, by tokenizer family. Byte-level BPE (the GPT-2
# byte<->unicode map, used by the whole Qwen line and therefore by both
# nomic-embed-code and Qwen3-Embedding-0.6B) renders byte 0x20 as Ġ, 0x0A as
# Ċ, 0x09 as ĉ, 0x0D as č. SentencePiece uses ▁. WordPiece uses a "##"
# continuation PREFIX, which is not a character strip — leaving it unhandled
# rejects every continuation token, measured at 14.2% of the admitted set on a
# real WordPiece vocabulary.
BPE_MARKERS = "\u0120\u2581\u010a\u0109\u010d"  # Ġ ▁ Ċ ĉ č
WORDPIECE_MARKER = "##"

# These words must not appear as strings in the shipped binary. Four of them
# fail scripts/security-strings.sh outright ("wget|netcat|ncat|telnet"); the
# rest were removed for the antivirus and VirusTotal release gates, which admit
# zero malicious AND zero suspicious verdicts (SECURITY.md). They were last
# removed by hand-editing the generated artifacts (commit 8967d7cd), which left
# 11 empty strings in code_tokens.txt and 11 orphaned vectors in the blob, and
# which any regeneration silently undoes. Encoding the list here is what makes
# it survive the next extraction. Each falls back to a sparse random vector.
VOCAB_DENYLIST = frozenset({
    "wget", "curl", "netcat", "ncat", "telnet",
    "passwd", "shadow", "exploit", "hack", "inject", "malware",
})

# Identifier atoms a byte-level BPE vocabulary provably CANNOT contain. Those
# tokenizers pre-split on the GPT-2 regex, whose \p{L}+ and \p{N}{1,3} branches
# never merge a letter with a digit — measured across BPE, WordPiece and
# Unigram vocabularies, essentially no admitted entry contains one. So "uint64",
# "utf8" and "sha256" are unreachable by enumerating any vocabulary, while
# hyp_sem_tokenize emits them constantly ("uint64_t" -> ["uint64", "t"]). On
# llvm-project lld/ELF that class is 5.60% of sub-token occurrences with ZERO
# of it in the current table, and these 64 strings recover 1.94 points of it
# (78.91% -> 80.85% occurrence coverage) for 48 KB of a 30 MB blob. 56 of the
# 64 were observed in a 3.86M-token C corpus; the other 8 complete families
# whose siblings were.
CODE_SUPPLEMENT = (
    # fixed-width integer and float atoms
    "u8", "u16", "u32", "u64", "u128", "i8", "i16", "i32", "i64", "i128",
    "f32", "f64", "int8", "int16", "int32", "int64", "int128",
    "uint8", "uint16", "uint32", "uint64", "uint128", "float32", "float64",
    # architectures
    "x86", "x64", "amd64", "arm64", "aarch64", "i386", "riscv64", "ppc64",
    # text encodings
    "utf8", "utf16", "utf32", "latin1", "base32", "base64",
    # digests and checksums
    "md5", "sha1", "sha256", "sha384", "sha512", "crc32", "crc64",
    "blake2", "blake3", "xxh3", "xxh64",
    # platform, protocol and version atoms
    "win32", "win64", "ipv4", "ipv6", "v1", "v2", "v3",
    "log2", "log10", "exp2", "atan2", "cpp11", "cpp14", "cpp17", "cpp20",
)

# Strings any usable code or text vocabulary must yield. If one is missing the
# marker rules above are wrong for the tokenizer in play and the table is
# quietly short — the exact failure this script cannot otherwise see. Every
# entry was checked to be present in all three offline reference vocabularies
# (ByteLevel BPE, WordPiece, Unigram); deliberately no code-specific words —
# "buffer" and "pointer" are absent from XLM-R's 250k multilingual vocabulary,
# which is a property of that vocabulary and not a defect to alarm on.
VOCAB_PROBES = ("return", "string", "index", "error", "value",
                "function", "memory", "length", "name", "size")


def surface_form(token_str: str) -> str:
    """Strip subword markers from a raw vocabulary entry.

    Handles all three families a HuggingFace tokenizer can present, because the
    target model is being swapped and the wrong guess here is silent.
    """
    s = token_str.strip()
    if s.startswith(WORDPIECE_MARKER):
        s = s[len(WORDPIECE_MARKER):]
    return s.lstrip(BPE_MARKERS)


def clean_token(token_str: str) -> str:
    """Normalize a vocabulary entry to the form the runtime will look up."""
    # Outer underscores only: hyp_sem_tokenize treats "_" as a delimiter, so
    # "_Bool" and "__builtin_expect" reach the table as "bool" / "builtin".
    return surface_form(token_str).strip("_").lower()


def is_code_relevant(token_str: str) -> bool:
    """True if this vocabulary entry earns a row in the baked-in table.

    Decided on the CLEANED form rather than the raw entry, so the rule that
    admits a string and the string that actually ships cannot disagree. Special
    tokens ("<|endoftext|>", "[CLS]") and BPE punctuation noise are rejected by
    the shape rule itself rather than by separate early exits — two gates that
    have to agree is how the "##" gap survived.
    """
    cleaned = clean_token(token_str)
    if len(cleaned) < MIN_TOKEN_LEN:
        return False
    if not ADMISSIBLE_TOKEN.fullmatch(cleaned):
        return False
    if cleaned in VOCAB_DENYLIST:
        return False
    return True


def _assert_vocabulary_sane(selected: list, raw_vocab_size: int, from_vocab: int):
    """Fail loudly rather than ship a quietly-truncated table."""
    if not selected:
        raise SystemExit("vocabulary selection produced nothing")
    share = from_vocab / max(raw_vocab_size, 1)
    if not 0.10 <= share <= 0.90:
        raise SystemExit(
            f"vocabulary selection admitted {from_vocab}/{raw_vocab_size} "
            f"({share:.1%}) of the raw vocabulary, outside the 10-90% band "
            f"observed across BPE, WordPiece and Unigram tokenizers. The "
            f"marker rules are probably wrong for this tokenizer."
        )
    chosen = set(selected)
    missing = [p for p in VOCAB_PROBES if p not in chosen]
    if missing:
        raise SystemExit(f"vocabulary is missing basic probe tokens: {missing}")
    banned = chosen & VOCAB_DENYLIST
    if banned:
        raise SystemExit(f"denylisted tokens reached the table: {sorted(banned)}")
    bad = [t for t in selected if not ADMISSIBLE_TOKEN.fullmatch(t)]
    if bad:
        raise SystemExit(
            f"tokens the runtime can never query reached the table: {bad[:10]}")


def select_vocabulary(vocab: dict) -> list:
    """Pick the token strings that get a baked-in vector, in table order.

    `vocab` is tokenizer.get_vocab(): {token_string: token_id}. Dedup keeps the
    lowest token id, which is arbitrary but harmless: two entries that clean to
    the same string produce byte-identical inference input and therefore the
    same vector.
    """
    seen = set()
    selected = []
    for tok_str, _tok_id in sorted(vocab.items(), key=lambda kv: kv[1]):
        if not is_code_relevant(tok_str):
            continue
        cleaned = clean_token(tok_str)
        if cleaned in seen:
            continue
        seen.add(cleaned)
        selected.append(cleaned)

    from_vocab = len(selected)
    for extra in CODE_SUPPLEMENT:
        if extra in seen or extra in VOCAB_DENYLIST:
            continue
        seen.add(extra)
        selected.append(extra)

    selected.sort()
    _assert_vocabulary_sane(selected, len(vocab), from_vocab)
    print(f"  from vocabulary: {from_vocab}   "
          f"code supplement: {len(selected) - from_vocab}")
    return selected


# ── Simulated attention ──────────────────────────────────────────────

def simulated_attention(vectors: np.ndarray, k: int, iterations: int,
                        alpha: float) -> np.ndarray:
    """
    Apply simulated self-attention: for each vector, blend with mean of
    top-K nearest neighbors. This approximates contextual composition
    that real attention provides.

    vectors: (N, D) float32 unit-normalized
    Returns: (N, D) float32 unit-normalized
    """
    n, d = vectors.shape
    result = vectors.copy()

    for iteration in range(iterations):
        t0 = time.time()
        # Compute cosine similarity matrix in chunks to avoid OOM
        # For 40K vectors × 768d, full matrix = 40K² × 4 bytes = 6.4GB
        # Process in chunks of 2048
        chunk_size = 2048
        new_result = np.zeros_like(result)

        for i in range(0, n, chunk_size):
            end = min(i + chunk_size, n)
            chunk = result[i:end]  # (chunk, D)

            # Cosine similarity: chunk × all^T
            sims = chunk @ result.T  # (chunk, N)

            # For each vector in chunk, find top-K neighbors (excluding self)
            for j in range(end - i):
                global_idx = i + j
                sim_row = sims[j].copy()
                sim_row[global_idx] = -1.0  # Exclude self

                # Top-K indices
                if k < n - 1:
                    top_k_idx = np.argpartition(sim_row, -k)[-k:]
                else:
                    top_k_idx = np.arange(n)
                    top_k_idx = top_k_idx[top_k_idx != global_idx]

                neighbor_mean = result[top_k_idx].mean(axis=0)

                # Blend
                blended = (1 - alpha) * result[global_idx] + alpha * neighbor_mean
                # Re-normalize
                norm = np.linalg.norm(blended)
                if norm > 1e-8:
                    blended /= norm
                new_result[global_idx] = blended

        result = new_result
        elapsed = time.time() - t0
        print(f"  sim-attention iter {iteration + 1}/{iterations}: {elapsed:.1f}s")

    return result


# ── Extraction ───────────────────────────────────────────────────────

def extract_embeddings(model, tokenizer, tokens: list, device: str,
                       batch_size: int = 64,
                       checkpoint_path: str = None) -> np.ndarray:
    """Run full model inference on each token string. Returns (N, D) float32."""

    # Check for checkpoint
    start_idx = 0
    all_vecs = []
    if checkpoint_path and os.path.exists(checkpoint_path):
        data = np.load(checkpoint_path)
        all_vecs = list(data["vectors"])
        start_idx = len(all_vecs)
        print(f"  resuming from checkpoint: {start_idx}/{len(tokens)} tokens")

    model.eval()
    total = len(tokens)
    t0 = time.time()

    with torch.no_grad():
        for batch_start in range(start_idx, total, batch_size):
            batch_end = min(batch_start + batch_size, total)
            batch_tokens = tokens[batch_start:batch_end]

            # nomic-embed-code requires search_query or search_document prefix
            # For single tokens, we use the token as-is (query mode)
            texts = [f"search_query: {t}" for t in batch_tokens]

            encoded = tokenizer(
                texts,
                padding=True,
                truncation=True,
                max_length=64,
                return_tensors="pt"
            ).to(device)

            outputs = model(**encoded)

            # Mean pooling over non-padding tokens
            attention_mask = encoded["attention_mask"]
            token_embeddings = outputs.last_hidden_state
            input_mask_expanded = (
                attention_mask.unsqueeze(-1)
                .expand(token_embeddings.size())
                .float()
            )
            sum_embeddings = torch.sum(
                token_embeddings * input_mask_expanded, dim=1
            )
            sum_mask = torch.clamp(input_mask_expanded.sum(dim=1), min=1e-9)
            mean_pooled = sum_embeddings / sum_mask

            # Truncate to OUTPUT_DIM if model outputs more (Matryoshka)
            if mean_pooled.shape[1] > OUTPUT_DIM:
                mean_pooled = mean_pooled[:, :OUTPUT_DIM]

            # L2 normalize
            mean_pooled = torch.nn.functional.normalize(mean_pooled, p=2, dim=1)

            vecs = mean_pooled.cpu().numpy()
            all_vecs.extend(vecs)

            # Progress
            done = batch_end
            elapsed = time.time() - t0
            rate = (done - start_idx) / elapsed if elapsed > 0 else 0
            eta = (total - done) / rate if rate > 0 else 0
            print(
                f"  [{done:>6}/{total}] "
                f"{rate:.1f} tok/s  "
                f"ETA {eta / 60:.0f}m",
                flush=True
            )

            # Checkpoint
            if checkpoint_path and (done % CHECKPOINT_EVERY < batch_size):
                np.savez_compressed(
                    checkpoint_path,
                    vectors=np.array(all_vecs, dtype=np.float32)
                )

    print()
    return np.array(all_vecs, dtype=np.float32)


# ── Output generation ────────────────────────────────────────────────

def write_bin(path: str, vectors: np.ndarray, dim: int):
    """Write binary blob: [int32 count][int32 dim] + count×dim int8."""
    n = vectors.shape[0]
    # Quantize: scale to [-127, 127], round to int8
    quantized = np.clip(np.round(vectors * 127.0), -127, 127).astype(np.int8)

    with open(path, "wb") as f:
        f.write(struct.pack("<ii", n, dim))
        f.write(quantized.tobytes())

    size_mb = os.path.getsize(path) / (1024 * 1024)
    print(f"  {path}: {n} vectors × {dim}d = {size_mb:.1f} MB")


def write_tokens_txt(path: str, tokens: list):
    """Write plain text token list."""
    with open(path, "w", encoding="utf-8") as f:
        for t in tokens:
            f.write(t + "\n")
    print(f"  {path}: {len(tokens)} tokens")


def guard_stem(out_dir) -> str:
    """Derive the include-guard's middle segment from the output directory.

    vendored/nomic -> NOMIC, vendored/qwen3 -> QWEN3. Anything not [A-Z0-9] is
    folded to an underscore so the result is always a legal macro name.
    """
    base = Path(out_dir).name.upper()
    return re.sub(r"[^A-Z0-9]", "_", base) or "MODEL"


def write_tokens_h(path: str, tokens: list, guard_prefix: str = HEADER_GUARD_PREFIX,
                   stem: str = "NOMIC"):
    """Write C header with token string array."""
    guard = f"{guard_prefix}_{stem}_TOKENS_H"
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"/* {MODEL_DISPLAY} token vocabulary — {len(tokens)} tokens. */\n")
        f.write(f"#ifndef {guard}\n")
        f.write(f"#define {guard}\n\n")
        f.write(f"static const char *PRETRAINED_TOKENS[{len(tokens)}] = {{\n")
        for t in tokens:
            escaped = t.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'"{escaped}",\n')
        f.write("};\n\n")
        f.write(f"#endif /* {guard} */\n")
    print(f"  {path}: written")


def write_vectors_h(path: str, token_count: int, dim: int,
                    guard_prefix: str = HEADER_GUARD_PREFIX, stem: str = "NOMIC"):
    """Write C header with defines and inline accessor.

    The int8-vs-float32 paragraph is a recorded measurement, not boilerplate:
    NEXT-STEPS cites this header as the place that decision is written down, so
    a regeneration must not delete it.
    """
    guard = f"{guard_prefix}_{stem}_VECTORS_H"
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"""/* {MODEL_DISPLAY} ({MODEL_NAME}) token embeddings.
 * {token_count} tokens x {dim}d int8-quantized unit vectors.
 * Distilled from {MODEL_SIZE_LABEL} model via full inference on filtered vocabulary.
 * Simulated attention: {SIM_ATTENTION_ITERS} iterations, K={SIM_ATTENTION_K}, alpha={SIM_ATTENTION_ALPHA}.
 *
 * Vector blob embedded via code_vectors_blob.S (assembler .incbin).
 * Token strings are in this header as a static array.
 *
 * Storage format: int8 × 127. We also tested float32 storage — it did NOT
 * improve performance because cooccur passes are memory-bandwidth-bound.
 * Float32 dense reads are 4x larger than int8, which cancels the CPU savings
 * from avoided int8→float conversion. int8 is a strict win on binary size
 * (30 MB vs 120 MB) and equal on runtime.
 *
 * Source: {MODEL_URL}
 * License: {MODEL_LICENSE}
 */
#ifndef {guard}
#define {guard}

#include <stdint.h>

#define PRETRAINED_TOKEN_COUNT {token_count}
#define PRETRAINED_DIM {dim}

/* Raw vector blob: first 8 bytes = [int32 count][int32 dim],
 * then count x dim int8 values (unit-normalized, x127 scaled). */
extern const unsigned char PRETRAINED_VECTOR_BLOB[];
extern const unsigned int PRETRAINED_VECTOR_BLOB_LEN;

/* Access the int8 vector for token index i. Zero-copy pointer into blob. */
static inline const int8_t *pretrained_vec_at(int i) {{
    return (const int8_t *)(PRETRAINED_VECTOR_BLOB + 8 + (size_t)i * PRETRAINED_DIM);
}}

/* Token strings (separate header to keep this file clean). */
#include "code_tokens.h"

#endif /* {guard} */
""")
    print(f"  {path}: written")


def write_blob_s(path: str, incbin_path: str):
    """Write the assembler .incbin wrapper.

    Three object formats, and the ELF branch carries a .note.GNU-stack section
    that is load-bearing: this is the only assembly source in the build, so an
    unannotated object makes GNU ld assume an executable stack for the WHOLE
    link. An earlier version of this function emitted the Mach-O branch alone;
    regenerating with it would have silently reverted that fix and broken the
    ELF and COFF links outright. scripts/verify-vector-artifacts.py is what
    catches that, and scripts/ci/check-binary-composition.sh is what catches it
    on the shipped artifact.
    """
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"""/* {MODEL_DISPLAY} vector blob embedded via assembler.
 * Cross-platform: macOS (Mach-O) vs Linux (ELF) vs Windows (COFF). */

#if defined(__APPLE__)
    .section __DATA,__const
    .globl _PRETRAINED_VECTOR_BLOB
    .globl _PRETRAINED_VECTOR_BLOB_LEN
    .p2align 4
_PRETRAINED_VECTOR_BLOB:
    .incbin "{incbin_path}"
_PRETRAINED_VECTOR_BLOB_END:

    .section __DATA,__const
    .p2align 2
_PRETRAINED_VECTOR_BLOB_LEN:
    .long _PRETRAINED_VECTOR_BLOB_END - _PRETRAINED_VECTOR_BLOB

#elif defined(_WIN32) || defined(__CYGWIN__) || defined(__MINGW32__)
    .section .rdata,"dr"
    .globl PRETRAINED_VECTOR_BLOB
    .globl PRETRAINED_VECTOR_BLOB_LEN
    .p2align 4
PRETRAINED_VECTOR_BLOB:
    .incbin "{incbin_path}"
PRETRAINED_VECTOR_BLOB_END:

    .section .rdata,"dr"
    .p2align 2
PRETRAINED_VECTOR_BLOB_LEN:
    .long PRETRAINED_VECTOR_BLOB_END - PRETRAINED_VECTOR_BLOB

#else
    /* WHY: an ELF object that carries no .note.GNU-stack tells the linker
     * nothing about its stack requirement, and GNU ld then assumes the WORST
     * for the whole link — every Linux release binary shipped GNU_STACK RWE
     * because of this one omission. This is the only assembly source in the
     * build, so it alone decided that property. The note must stay even though
     * a blob of constant data obviously never executes: absence is the signal,
     * not the contents. -Wl,-z,noexecstack in the link flags enforces the
     * outcome, and scripts/ci/check-binary-composition.sh fails the release if
     * an executable stack ever comes back. */
    .section .note.GNU-stack,"",@progbits

    .section .rodata,"a",@progbits
    .globl PRETRAINED_VECTOR_BLOB
    .globl PRETRAINED_VECTOR_BLOB_LEN
    .p2align 4
PRETRAINED_VECTOR_BLOB:
    .incbin "{incbin_path}"
PRETRAINED_VECTOR_BLOB_END:

    .section .rodata,"a",@progbits
    .p2align 2
PRETRAINED_VECTOR_BLOB_LEN:
    .long PRETRAINED_VECTOR_BLOB_END - PRETRAINED_VECTOR_BLOB
#endif
""")
    print(f"  {path}: written")


def write_artifacts(out_dir, tokens: list, vectors, dim: int,
                    guard_prefix: str = HEADER_GUARD_PREFIX,
                    incbin_path: str = None):
    """Write all five artifacts. THE ONLY writer entry point.

    main() and scripts/verify-vector-artifacts.py both go through here, so the
    identity test exercises the exact code path a real extraction uses — the
    only difference is where `vectors` came from.
    """
    out_dir = Path(out_dir)
    stem = guard_stem(out_dir)
    if incbin_path is None:
        incbin_path = out_dir.as_posix()
        incbin_path = f"{incbin_path}/code_vectors.bin"

    write_bin(str(out_dir / "code_vectors.bin"), vectors, dim)
    write_tokens_txt(str(out_dir / "code_tokens.txt"), tokens)
    write_tokens_h(str(out_dir / "code_tokens.h"), tokens, guard_prefix, stem)
    write_vectors_h(str(out_dir / "code_vectors.h"), len(tokens), dim,
                    guard_prefix, stem)
    write_blob_s(str(out_dir / "code_vectors_blob.S"), incbin_path)


# ── Main ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Extract nomic-embed-code token embeddings")
    parser.add_argument("--output-dir", default="vendored/nomic",
                        help="Output directory (default: vendored/nomic)")
    parser.add_argument("--device", default=None,
                        help="Device: cuda, mps, cpu (auto-detected)")
    parser.add_argument("--skip-attention", action="store_true",
                        help="Skip simulated attention (faster, lower quality)")
    parser.add_argument("--batch-size", type=int, default=BATCH_SIZE,
                        help=f"Batch size (default: {BATCH_SIZE})")
    parser.add_argument("--checkpoint", default=None,
                        help="Checkpoint file path (auto: <output-dir>/checkpoint.npz)")
    parser.add_argument("--header-guard-prefix", default=HEADER_GUARD_PREFIX,
                        help=f"Include-guard prefix (default: {HEADER_GUARD_PREFIX}; "
                             "use HYP for a new output directory)")
    args = parser.parse_args()

    batch_size = args.batch_size

    # Auto-detect device
    # Prefer CPU for 7B models on Apple Silicon — MPS shares unified memory
    # with the system and can cause OOM/crashes. CPU keeps allocation predictable.
    # Use --device mps to override if you have enough headroom (32GB+).
    if args.device:
        device = args.device
    elif torch.cuda.is_available():
        device = "cuda"
    else:
        device = "cpu"

    # Force line-buffered stdout so tee/log sees output immediately
    sys.stdout.reconfigure(line_buffering=True)

    print(f"device={device}")
    print(f"threads={torch.get_num_threads()}")
    print(f"model={MODEL_NAME}")
    print(f"output_dim={OUTPUT_DIM}")
    print()

    # Create output dir
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    checkpoint_path = args.checkpoint or str(out_dir / "checkpoint.npz")

    # ── Step 1: Load model + tokenizer ──
    print("step 1: loading model + tokenizer...")
    t0 = time.time()
    tokenizer = AutoTokenizer.from_pretrained(MODEL_NAME, trust_remote_code=True)
    model = AutoModel.from_pretrained(
        MODEL_NAME,
        trust_remote_code=True,
        dtype=torch.float16,             # 7B×2B = ~14GB (vs 28GB float32)
        low_cpu_mem_usage=True,          # Stream weights, no 2x peak during load
    )
    model = model.to(device)
    print(f"  loaded in {time.time() - t0:.1f}s")
    print(f"  hidden_size={model.config.hidden_size}")
    print(f"  vocab_size={tokenizer.vocab_size}")
    print()

    # ── Step 2: Filter vocabulary ──
    print("step 2: filtering vocabulary to code-relevant tokens...")
    vocab = tokenizer.get_vocab()
    print(f"  raw vocabulary: {len(vocab)} tokens")

    # Filter, deduplicate, supplement and sanity-check — see the vocabulary
    # selection section at the top of this file.
    filtered_tokens = select_vocabulary(vocab)
    print(f"  code-relevant (deduplicated): {len(filtered_tokens)} tokens")

    # Show sample
    sample = filtered_tokens[:20]
    print(f"  sample: {sample}")
    print()

    # ── Step 3: Extract embeddings (full inference) ──
    print(f"step 3: extracting embeddings ({len(filtered_tokens)} tokens, batch_size={batch_size})...")
    t0 = time.time()
    vectors = extract_embeddings(
        model, tokenizer, filtered_tokens, device,
        batch_size=batch_size, checkpoint_path=checkpoint_path
    )
    elapsed = time.time() - t0
    print(f"  extracted {vectors.shape[0]} vectors × {vectors.shape[1]}d in {elapsed:.0f}s")

    # Truncate to OUTPUT_DIM if needed
    if vectors.shape[1] > OUTPUT_DIM:
        print(f"  truncating {vectors.shape[1]}d -> {OUTPUT_DIM}d (Matryoshka)")
        vectors = vectors[:, :OUTPUT_DIM]
        # Re-normalize after truncation
        norms = np.linalg.norm(vectors, axis=1, keepdims=True)
        norms = np.maximum(norms, 1e-8)
        vectors = vectors / norms

    print(f"  final shape: {vectors.shape}")

    # Mean-center to fix anisotropy (transformer embeddings cluster tightly,
    # making all cosine similarities ~0.95+). Subtracting the corpus mean
    # spreads vectors apart, making cosine discriminative.
    mean_vec = vectors.mean(axis=0)
    mean_norm = np.linalg.norm(mean_vec)
    print(f"  mean vector norm before centering: {mean_norm:.4f} (>0.5 = anisotropic)")
    vectors = vectors - mean_vec
    # Re-normalize after centering
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    norms = np.maximum(norms, 1e-8)
    vectors = vectors / norms
    mean_after = np.linalg.norm(vectors.mean(axis=0))
    print(f"  mean vector norm after centering: {mean_after:.6f}")
    print()

    # ── Step 4: Simulated attention ──
    if not args.skip_attention:
        print(f"step 4: simulated attention (K={SIM_ATTENTION_K}, "
              f"iters={SIM_ATTENTION_ITERS}, alpha={SIM_ATTENTION_ALPHA})...")
        t0 = time.time()
        vectors = simulated_attention(
            vectors, SIM_ATTENTION_K, SIM_ATTENTION_ITERS, SIM_ATTENTION_ALPHA
        )
        print(f"  completed in {time.time() - t0:.1f}s")
        print()
    else:
        print("step 4: simulated attention SKIPPED")
        print()

    # ── Step 5: Write output files ──
    print("step 5: writing output files...")
    dim = vectors.shape[1]

    write_artifacts(out_dir, filtered_tokens, vectors, dim,
                    guard_prefix=args.header_guard_prefix)
    print()

    # Cleanup checkpoint
    if os.path.exists(checkpoint_path):
        os.remove(checkpoint_path)
        print(f"  removed checkpoint: {checkpoint_path}")

    # ── Summary ──
    bin_size = os.path.getsize(str(out_dir / "code_vectors.bin"))
    print()
    print("=" * 60)
    print(f"  model:      {MODEL_NAME}")
    print(f"  tokens:     {len(filtered_tokens)}")
    print(f"  dimensions: {dim}")
    print(f"  blob size:  {bin_size / (1024*1024):.1f} MB")
    print(f"  sim-attn:   {'yes' if not args.skip_attention else 'no'}")
    print(f"  output:     {out_dir}/")
    print("=" * 60)
    print()
    print("next steps — FOUR wiring points, not two (docs/EMBEDDING-SWAP.md):")
    print(f"  1. Makefile.hyp:348  UNIXCODER_BLOB_SRC = {out_dir}/code_vectors_blob.S")
    print(f"  2. Makefile.hyp:874  prerequisite            {out_dir}/code_vectors.bin")
    print(f"     (a stale path here does not fail — the object just never rebuilds)")
    print(f"  3. src/semantic/semantic.c:17  #include \"{out_dir.name}/code_vectors.h\"")
    print(f"  4. the .incbin path inside {out_dir}/code_vectors_blob.S, which this")
    print(f"     script already wrote as \"{out_dir.as_posix()}/code_vectors.bin\"")
    print(f"  5. scripts/security-vendored.sh --update   (review the diff FIRST)")
    print(f"  6. make -f Makefile.hyp clean-c && make -f Makefile.hyp hyp")


if __name__ == "__main__":
    main()
