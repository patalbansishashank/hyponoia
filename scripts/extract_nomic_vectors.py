#!/usr/bin/env python3
"""
Extract token embeddings from an embedding model for the static lookup table.

Loads the full model, filters the vocabulary to code-relevant tokens,
runs full inference on each token, applies simulated attention, quantizes
to int8, and outputs files compatible with vendored/unixcoder/ format.

Usage:
    pip install torch transformers
    python scripts/extract_nomic_vectors.py [--output-dir vendored/nomic]

Output:
    code_vectors.bin   — [int32 count][int32 dim] + count×dim int8
    code_tokens.txt    — one token per line
    code_tokens.h      — C header: static const char *PRETRAINED_TOKENS[N]
    code_vectors.h     — C header: defines + inline accessor
    code_vectors_blob.S — assembler .incbin

One-time extraction. The model is selected by --profile; see MODEL_PROFILES.
The "~2-3h on GPU" this file used to claim was for the 7B nomic model. For
Qwen3-Embedding-0.6B on the RX 6900 XT the whole thing is under a minute,
measured: ~16s of forward passes (230,849 padded positions at 0.881
GFLOP/position, 14.4k positions/s) plus 25.1s of CPU simulated attention.
runs/EMBED-SWAP/F-extractor.json has the numbers and what is still unverified.
"""

import argparse
import hashlib
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


# ── Model profiles ─────────────────────────────────────────────────────
#
# A profile fixes model id, input prefix, pooling, padding side and dtype
# TOGETHER. Any one of them wrong yields vectors of the right SHAPE and the
# wrong CONTENT — the one failure this table cannot detect at runtime, because
# semantic.c only ever checks PRETRAINED_DIM. Hence a named profile rather than
# five independent flags.

MODEL_PROFILES = {
    # Qwen3-Embedding-0.6B. Every field below is from the published model repo,
    # not from inference:
    #   1_Pooling/config.json          -> pooling_mode_lasttoken: true
    #                                     (mean pooling is WRONG for this model)
    #   README.md transformers example -> AutoTokenizer(..., padding_side='left')
    #   config_sentence_transformers.json -> prompts.document == ""
    #                                     (documents take no instruction prefix)
    #
    # On that empty prefix, because it looks like an omission and is not: a
    # vocabulary token is a DOCUMENT, not a query. Nothing downstream plays the
    # query role — semantic.c compares these vectors against other vectors from
    # this same table — so the asymmetry the instruction encodes has no other
    # side. Prepending the query instruction would also add one shared direction
    # to all ~40.9k vectors, which is the anisotropy the mean-centering step
    # below then has to remove, and would spend ~85% of every forward pass
    # re-encoding a 15-piece constant in front of a mean-2.70-piece token.
    #   config.json                    -> hidden_size 1024, 28 layers, use_cache true
    # The tokenizer appends <|endoftext|> (151643) itself under the default
    # add_special_tokens=True — verified: tok("foo") -> ['foo', '<|endoftext|>'].
    # Last-token pooling therefore reads the EOS hidden state, as trained.
    #
    # dtype float32, not float16: this is the configuration already validated on
    # this card in runs/GPU/rocm.json, the checkpoint is published in bfloat16
    # (whose exponent range float16 does not cover), and the output is int8-
    # quantized to +-127 regardless. float16 would buy ~20s on a one-shot run
    # in exchange for a silent-NaN risk. Bad trade.
    "qwen3": {
        "model_name": "Qwen/Qwen3-Embedding-0.6B",
        "prefix": "",
        "pooling": "last",
        "padding_side": "left",
        "dtype": "float32",
        "trust_remote_code": False,
        # Provenance, stamped into the generated header banners. Qwen3-Embedding
        # ships no LICENSE file in its repo; apache-2.0 is declared in the model
        # card front matter and HF metadata. See vendored/qwen3/NOTICE.
        "size_label": "0.6B",
        "license": "Apache 2.0",
    },
    # nomic-embed-code (7B), the model the current vendored/nomic table came
    # from. Kept verbatim so that table can be REGENERATED for track E's
    # identity test. Not for the swap.
    "nomic": {
        "model_name": "nomic-ai/nomic-embed-code",
        "prefix": "search_query: ",
        "pooling": "mean",
        "padding_side": "right",
        "dtype": "float16",
        "trust_remote_code": True,
        # These two reproduce the banners in the committed vendored/nomic/*.h
        # byte-for-byte; verify-vector-artifacts.py fails if either drifts.
        "size_label": "7B",
        "license": "Apache 2.0",
    },
}
DEFAULT_PROFILE = "qwen3"


# ── Configuration ──────────────────────────────────────────────────────

# ── Model identity — derived from the ACTIVE PROFILE, never hardcoded ──
#
# Everything the generated headers say about *which* model produced the table
# comes from these values, so no generated file can claim a provenance the
# extraction did not have. They are module globals because write_tokens_h(),
# write_vectors_h() and write_blob_s() read them at call time; main() rebinds
# them from MODEL_PROFILES[--profile] via apply_profile_identity() before any
# file is written. The defaults below are the nomic profile, which is what
# scripts/verify-vector-artifacts.py regenerates for its byte-identity check.
#
# This is the merge point of two tracks: the profile table decides how the
# model is RUN (pooling, padding, prefix, dtype); these constants decide what
# the generated headers SAY about it. Hardcoding a model name here while
# DEFAULT_PROFILE names another is exactly the mislabelling this block exists
# to prevent. See docs/EMBEDDING-SWAP.md.
MODEL_NAME = MODEL_PROFILES["nomic"]["model_name"]
MODEL_SIZE_LABEL = MODEL_PROFILES["nomic"]["size_label"]
MODEL_LICENSE = MODEL_PROFILES["nomic"]["license"]
MODEL_URL = f"https://huggingface.co/{MODEL_NAME}"
MODEL_DISPLAY = MODEL_NAME.split("/")[-1]


def apply_profile_identity(profile: dict) -> None:
    """Rebind the header-provenance globals to the profile actually being run."""
    global MODEL_NAME, MODEL_SIZE_LABEL, MODEL_LICENSE, MODEL_URL, MODEL_DISPLAY
    MODEL_NAME = profile["model_name"]
    MODEL_SIZE_LABEL = profile["size_label"]
    MODEL_LICENSE = profile["license"]
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

# BATCH_SIZE: was 32, sized for a 7B model on long inputs. This workload is
# ~40.9k identifier fragments (measured with the real Qwen3 tokenizer: mean 2.70
# BPE pieces, max 9, plus the EOS the tokenizer appends) through a 0.6B model.
#
# MEASURED on the RX 6900 XT (gfx1030, torch 2.13.0+rocm7.1) with the real
# Qwen3-Embedding-0.6B architecture — 28 layers, hidden 1024, intermediate 3072,
# 0.881 GFLOP/position — and the real token strings. 20 s of GPU:
#
#   batch    pos/s   TFLOP/s   peak VRAM   padded positions   est. full run
#      32     3,952     3.48      2.43 GB       166,868           42.2 s
#     128    10,342     9.11      2.47 GB       194,612           18.8 s
#     512    14,360    12.65      2.64 GB       230,849           16.1 s   <--
#    1024    14,038    12.37      2.95 GB       252,353           18.0 s
#    2048    13,834    12.19      3.48 GB       273,742           19.8 s
#    4096    13,089    11.53      4.86 GB       297,973           22.8 s
#
# Two effects pull against each other. Small batches are launch-bound: at 32 the
# GEMMs have ~128 rows and reach 15% of the card's 23.04 TFLOP/s fp32 peak.
# Device throughput saturates at ~14.4k positions/s by 512 (55% of peak) and
# does not improve after that — while padding waste keeps growing, because every
# batch pads to its own longest member and a bigger batch is likelier to contain
# a 9-piece token. So the curve has a real minimum and 512 is it: 2.6x faster
# than 32, and going further UP costs time rather than saving it.
#
# The spec expected "one to two orders of magnitude". The measured answer is 16x
# (32 -> 512), at the bottom of that range; 3,200 would be 40% SLOWER than 512.
# VRAM never binds: 2.64 GB of the card's 17.2 GB, nowhere near the 7.9 GB where
# runs/GPU/rocm.json recorded caching-allocator OOM warnings.
BATCH_SIZE = 512

# CHECKPOINT_EVERY: raised from 500. It is also now a FLOOR in tokens rather
# than a modulo — the old `done % CHECKPOINT_EVERY < batch_size` test degenerates
# to "every batch" the moment batch_size >= CHECKPOINT_EVERY, which any sane
# batch for this workload makes true.
#
# 500 was chosen against a believed 2-3 h runtime. Measured, the forward stage is
# 16.1 s, and a checkpoint is a rewrite of the whole filled prefix. At batch 512
# the two settings cost:
#
#   every=500:   79 writes, 4.97 GB fsync'd  ->  ~14 s on a 16.1 s stage
#   every=4096:   9 writes, 0.57 GB fsync'd  ->   ~1.6 s, <=4096 tokens at risk
#
# (measured atomic write throughput on this volume: 275-400 MB/s.) Balancing
# write cost against expected re-work puts the optimum near 6,100 tokens; the
# curve is flat either side, so 4096 is chosen as a whole multiple of the batch.
# Use --checkpoint-every to lower it for the slow paths (the nomic 7B profile, or
# CPU), where minutes of re-work matter more than half a gigabyte of I/O.
CHECKPOINT_EVERY = 4096


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

    COST, MEASURED at the real N=40,856 x 768 with K=32 and 3 iterations
    (Ryzen 9 5950X, numpy 2.5 on OpenBLAS):

        25.12 s total   |   11.71 s matmul   |   13.20 s python loop
        1.06 GB peak RSS, zero VRAM — this stage never touches the GPU.

    So this is NOT the bottleneck and it is NOT a 6.7 GB dense-matrix problem:
    the N x N similarity matrix is never materialised. It is already chunked at
    2048 rows, so the largest live slab is 2048 x 40,856 fp32 = 334 MB.

    The obvious "fix" — hoisting the per-row loop into a whole-chunk
    argpartition plus a (2048, 32, 768) gather — was implemented and measured:
    37.43 s and 2.03 GB, i.e. 1.5x SLOWER and 2x the memory, because the 201 MB
    gather buffer leaves cache while the per-row 32 x 768 buffer stays in L2.
    Left alone deliberately; do not "optimise" this without measuring.

    Because it is cheap, K / alpha / iterations are a quality lever that costs
    almost nothing to move. Measured at N=40,909: K=32 iters=3 is 24.0 s,
    K=128 iters=3 is 28.2 s (+17% — the per-row cost is the O(N) argpartition,
    not the K-row gather), K=32 iters=6 is 48.8 s (linear). Anyone tuning these
    against a retrieval benchmark is not constrained by this stage's runtime.

    One real hazard: the matmul leg is BLAS-bound. Measured 536 GFLOP/s on a
    pip numpy (bundled OpenBLAS) and 6.1 GFLOP/s on this distro's numpy linked
    against reference cblas — a 88x difference that turns 11.7 s into ~17 min.
    Run this in the venv, not against the system numpy.
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


# ── Pooling ──────────────────────────────────────────────────────────

def last_token_pool(last_hidden_states, attention_mask):
    """Last-token pooling, the mode Qwen3-Embedding was trained with.

    1_Pooling/config.json in the model repo sets pooling_mode_lasttoken: true.
    Mean-pooling this model produces vectors of the correct shape that are not
    what it emits — nothing downstream would notice.

    Handles either padding side, so a caller that forgets padding_side='left'
    still gets the right token rather than a pad embedding.
    """
    left_padded = bool(
        (attention_mask[:, -1].sum() == attention_mask.shape[0]).item()
    )
    if left_padded:
        return last_hidden_states[:, -1]
    lengths = attention_mask.sum(dim=1) - 1
    idx = torch.arange(last_hidden_states.shape[0],
                       device=last_hidden_states.device)
    return last_hidden_states[idx, lengths]


def mean_token_pool(last_hidden_states, attention_mask):
    """Mean pooling over non-padding positions (nomic-embed-code's mode)."""
    mask = (
        attention_mask.unsqueeze(-1)
        .expand(last_hidden_states.size())
        .to(last_hidden_states.dtype)
    )
    summed = torch.sum(last_hidden_states * mask, dim=1)
    counts = torch.clamp(mask.sum(dim=1), min=1e-9)
    return summed / counts


POOLERS = {"last": last_token_pool, "mean": mean_token_pool}


# ── Checkpointing ────────────────────────────────────────────────────

def checkpoint_fingerprint(profile_name: str, model_name: str,
                           tokens: list, dim: int) -> str:
    """Identify exactly which run a checkpoint belongs to.

    A checkpoint is a prefix of an output array whose row i means "the vector
    for tokens[i]". Resume it against a different token list and every row
    silently refers to the wrong token — right shape, wrong content, and no
    downstream check can catch it. Track D is actively changing the vocabulary
    filter, so a stale checkpoint in the output dir is a live hazard, not a
    hypothetical one.
    """
    h = hashlib.sha256()
    for part in (profile_name, model_name, str(dim), str(len(tokens))):
        h.update(part.encode("utf-8"))
        h.update(b"\0")
    h.update("\n".join(tokens).encode("utf-8"))
    return h.hexdigest()


def save_checkpoint(path: str, vectors: np.ndarray, fingerprint: str):
    """Write a checkpoint atomically.

    np.savez straight onto `path` is not safe here: the process being killed
    mid-write is the exact event checkpointing exists for, and it would leave a
    truncated zip that np.load refuses — losing the whole run instead of the
    last interval. Write to a sibling temp file, then rename; rename within a
    directory is atomic, so `path` is always either the previous good
    checkpoint or the new one.

    Uncompressed on purpose. Measured on the full 40,856 x 768 array:
    np.savez 0.02 s / 126 MB against np.savez_compressed 1.65 s / 116 MB.
    Unit-norm float32 barely compresses; 80x the CPU for 8% of the bytes.
    """
    tmp = path + ".tmp.npz"
    try:
        with open(tmp, "wb") as f:
            np.savez(f, vectors=vectors, fingerprint=np.array(fingerprint),
                     count=np.array(vectors.shape[0]))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise


def load_checkpoint(path: str, fingerprint: str, total: int, dim: int):
    """Return (vectors, count) from a checkpoint, or (None, 0) if unusable.

    Refuses rather than guesses. A checkpoint that does not match the current
    run is deleted, not resumed.
    """
    if not path or not os.path.exists(path):
        return None, 0
    try:
        with np.load(path, allow_pickle=False) as data:
            got = str(data["fingerprint"].item())
            vecs = np.asarray(data["vectors"], dtype=np.float32)
    except Exception as exc:                      # truncated, corrupt, old format
        print(f"  checkpoint {path} unreadable ({exc.__class__.__name__}); "
              f"starting fresh")
        return None, 0
    if got != fingerprint:
        print(f"  checkpoint {path} belongs to a DIFFERENT run "
              f"(fingerprint {got[:12]}… != {fingerprint[:12]}…); ignoring it")
        return None, 0
    if vecs.ndim != 2 or vecs.shape[1] != dim or vecs.shape[0] > total:
        print(f"  checkpoint {path} has shape {vecs.shape}, expected "
              f"(<={total}, {dim}); ignoring it")
        return None, 0
    return vecs, vecs.shape[0]


# ── Extraction ───────────────────────────────────────────────────────

def extract_embeddings(model, tokenizer, tokens: list, device: str,
                       profile: dict, profile_name: str,
                       batch_size: int = BATCH_SIZE,
                       checkpoint_path: str = None,
                       checkpoint_every: int = CHECKPOINT_EVERY) -> np.ndarray:
    """Run full model inference on each token string. Returns (N, D) float32."""

    total = len(tokens)
    hidden = int(model.config.hidden_size)
    dim = min(hidden, OUTPUT_DIM)
    pool = POOLERS[profile["pooling"]]
    prefix = profile["prefix"]

    fingerprint = checkpoint_fingerprint(
        profile_name, profile["model_name"], tokens, dim
    )

    # One preallocated (total, dim) buffer instead of a growing python list:
    # 40,856 x 768 float32 is 125 MB, so it costs nothing, and it makes a
    # checkpoint a slice rather than a rebuild of the whole array.
    out = np.zeros((total, dim), dtype=np.float32)
    resumed, start_idx = load_checkpoint(checkpoint_path, fingerprint, total, dim)
    if start_idx:
        out[:start_idx] = resumed
        print(f"  resuming from checkpoint: {start_idx}/{total} tokens")

    model.eval()
    t0 = time.time()
    last_ckpt = start_idx
    batch_start = start_idx

    def _checkpoint(done):
        if checkpoint_path:
            save_checkpoint(checkpoint_path, out[:done], fingerprint)

    try:
        with torch.no_grad():
            for batch_start in range(start_idx, total, batch_size):
                batch_end = min(batch_start + batch_size, total)
                batch_tokens = tokens[batch_start:batch_end]

                texts = [prefix + t for t in batch_tokens] if prefix \
                    else list(batch_tokens)

                encoded = tokenizer(
                    texts,
                    padding=True,
                    truncation=True,
                    max_length=64,
                    return_tensors="pt"
                ).to(device)

                # use_cache=False: config.json ships use_cache true, so the
                # forward would otherwise build a KV cache we never read —
                # 28 layers x 2 x (B x S) x 1024 x 4 B, which at batch 1024 is
                # ~2.4 GB of VRAM allocated and immediately discarded, and at
                # batch 4096 is ~9.4 GB, i.e. an OOM on this 16 GB card.
                outputs = model(**encoded, use_cache=False)

                pooled = pool(outputs.last_hidden_state,
                              encoded["attention_mask"])

                # Matryoshka: truncate to OUTPUT_DIM, THEN L2-normalise.
                # This is equivalent to the canonical normalise -> truncate ->
                # renormalise, because normalisation is multiplication by a
                # positive scalar and truncation is a coordinate projection P:
                #   normalize(P(v/||v||)) == normalize(P(v)/||v||) == normalize(P(v))
                # so the order here is not a shortcut, it is the same vector.
                if pooled.shape[1] > dim:
                    pooled = pooled[:, :dim]
                pooled = torch.nn.functional.normalize(pooled.float(), p=2, dim=1)

                out[batch_start:batch_end] = pooled.cpu().numpy()

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

                # checkpoint_every as a floor in tokens. The old
                # `done % CHECKPOINT_EVERY < batch_size` silently becomes
                # "every batch" once batch_size >= CHECKPOINT_EVERY.
                if done - last_ckpt >= checkpoint_every and done < total:
                    _checkpoint(done)
                    last_ckpt = done
    except KeyboardInterrupt:
        _checkpoint(batch_start)
        print(f"\n  interrupted; checkpointed {batch_start}/{total} tokens")
        raise

    print()
    return out


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
    parser = argparse.ArgumentParser(description="Extract token embeddings for the static table")
    parser.add_argument("--profile", default=DEFAULT_PROFILE,
                        choices=sorted(MODEL_PROFILES),
                        help=f"Model profile (default: {DEFAULT_PROFILE}). "
                             "Fixes model, prefix, pooling, padding side and "
                             "dtype together — see MODEL_PROFILES.")
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
    parser.add_argument("--checkpoint-every", type=int, default=CHECKPOINT_EVERY,
                        help=f"Checkpoint at least every N tokens "
                             f"(default: {CHECKPOINT_EVERY}); lower it for slow "
                             f"runs where re-work costs more than the I/O")
    args = parser.parse_args()

    batch_size = args.batch_size
    profile_name = args.profile
    profile = MODEL_PROFILES[profile_name]
    model_name = profile["model_name"]
    # Stamp the generated headers with the model actually being run, not a
    # hardcoded default — otherwise a Qwen3 table ships claiming nomic origin.
    apply_profile_identity(profile)

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
    print(f"profile={profile_name}")
    print(f"model={model_name}")
    print(f"pooling={profile['pooling']}  padding_side={profile['padding_side']}  "
          f"dtype={profile['dtype']}  prefix={profile['prefix']!r}")
    print(f"output_dim={OUTPUT_DIM}")
    print(f"batch_size={batch_size}  checkpoint_every={args.checkpoint_every}")
    print()

    # Create output dir
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    checkpoint_path = args.checkpoint or str(out_dir / "checkpoint.npz")

    # ── Step 1: Load model + tokenizer ──
    print("step 1: loading model + tokenizer...")
    t0 = time.time()
    # padding_side comes from the profile: last-token pooling wants left
    # padding, mean pooling does not care. Qwen3 is a native transformers
    # architecture (model_type "qwen3", requires transformers>=4.51), so it
    # needs no remote code — only the nomic profile opts into that.
    tokenizer = AutoTokenizer.from_pretrained(
        model_name,
        trust_remote_code=profile["trust_remote_code"],
        padding_side=profile["padding_side"],
    )
    model = AutoModel.from_pretrained(
        model_name,
        trust_remote_code=profile["trust_remote_code"],
        dtype=getattr(torch, profile["dtype"]),
        low_cpu_mem_usage=True,          # Stream weights, no 2x peak during load
    )
    model = model.to(device)
    print(f"  loaded in {time.time() - t0:.1f}s")
    print(f"  hidden_size={model.config.hidden_size}")
    print(f"  vocab_size={tokenizer.vocab_size}")
    print(f"  padding_side={tokenizer.padding_side}")
    if model.config.hidden_size < OUTPUT_DIM:
        sys.exit(f"FATAL: hidden_size {model.config.hidden_size} < OUTPUT_DIM "
                 f"{OUTPUT_DIM}; the C side hardcodes HYP_SEM_DIM=768 and this "
                 f"would silently emit short vectors.")
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
        profile, profile_name,
        batch_size=batch_size, checkpoint_path=checkpoint_path,
        checkpoint_every=args.checkpoint_every
    )
    elapsed = time.time() - t0
    print(f"  extracted {vectors.shape[0]} vectors × {vectors.shape[1]}d in {elapsed:.0f}s")

    # Matryoshka truncation already happened per batch, before the L2
    # normalisation, which is the same vector as truncate-then-renormalise
    # (see extract_embeddings). Qwen3-Embedding is 1024d native and MRL-trained
    # down to 32d, so 768 is inside its supported range and this is exactly why
    # HYP_SEM_DIM / PRETRAINED_DIM stay at 768 and no C changes are needed.
    assert vectors.shape[1] == min(int(model.config.hidden_size), OUTPUT_DIM)
    norms = np.linalg.norm(vectors, axis=1)
    print(f"  final shape: {vectors.shape} "
          f"(native {model.config.hidden_size}d -> {vectors.shape[1]}d)")
    print(f"  unit-norm check: min {norms.min():.6f} max {norms.max():.6f}")

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
    print(f"  model:      {model_name}  (profile {profile_name}, "
          f"{profile['pooling']}-pooling)")
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
