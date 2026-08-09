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


# ── Token filtering ───────────────────────────────────────────────────

def is_code_relevant(token_str: str) -> bool:
    """Filter vocabulary to code-relevant tokens.

    Goal: keep tokens that our runtime camelCase/snake_case splitter would
    produce from identifiers. Reject BPE noise, punctuation combos, and
    non-Latin scripts.
    """
    s = token_str.strip()
    if not s:
        return False

    # Remove BPE markers (Ġ = space prefix, ▁ = sentencepiece, Ċ/ċ = newline in Qwen)
    clean = s.lstrip("\u0120\u2581")  # Ġ, ▁
    if not clean:
        return False

    # Skip special tokens
    if clean.startswith("<") and clean.endswith(">"):
        return False
    if clean.startswith("[") and clean.endswith("]"):
        return False

    # Strip leading/trailing underscores (common in BPE) but keep content
    inner = clean.strip("_")
    if not inner:
        return False

    # STRICT: must be purely alphanumeric + underscores (identifier-shaped)
    # This rejects BPE noise like "!");ċ", "!!!!ċċ", etc.
    if not re.match(r'^[a-zA-Z][a-zA-Z0-9_]*$', inner):
        return False

    # Must be at least 2 chars of actual content
    if len(inner) < 2:
        return False

    return True


def clean_token(token_str: str) -> str:
    """Normalize a BPE token to the form our runtime tokenizer produces."""
    s = token_str.strip()
    # Strip BPE space markers
    s = s.lstrip("\u0120\u2581")
    # Strip leading/trailing underscores
    s = s.strip("_")
    # Lowercase (our runtime tokenizer lowercases)
    s = s.lower()
    return s


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

    # Filter and deduplicate
    seen = set()
    filtered_tokens = []
    for tok_str, tok_id in sorted(vocab.items(), key=lambda x: x[1]):
        if not is_code_relevant(tok_str):
            continue
        clean = clean_token(tok_str)
        if not clean or clean in seen:
            continue
        if len(clean) < 2:
            continue
        seen.add(clean)
        filtered_tokens.append(clean)

    filtered_tokens.sort()
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
