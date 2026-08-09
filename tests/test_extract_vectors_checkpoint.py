#!/usr/bin/env python3
"""Exercise scripts/extract_nomic_vectors.py without a GPU and without weights.

Covers the two things about that script that are otherwise unfalsifiable claims:

  * CHECKPOINT_EVERY makes a killed run resumable, and the resumed run produces
    the SAME BYTES as an uninterrupted one;
  * the Qwen3-Embedding swap pools the token the model was trained to pool.

The transformer here is a real Qwen3Model with random weights and 2 layers. The
numbers it produces are meaningless; the shapes, the padding, the KV-cache
behaviour and the pooling indices are exactly the real ones, which is what these
assertions are about.

Skips (exit 0) when torch / transformers / the Qwen3 tokenizer are unavailable —
this is not part of the C build's test suite.

Run:  python tests/test_extract_vectors_checkpoint.py
"""

import importlib.util
import os
import shutil
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(REPO, "scripts", "extract_nomic_vectors.py")
TOKENS_TXT = os.path.join(REPO, "vendored", "nomic", "code_tokens.txt")
MODEL_ID = "Qwen/Qwen3-Embedding-0.6B"


def skip(why):
    print(f"SKIP: {why}")
    sys.exit(0)


try:
    import numpy as np
    import torch
    from transformers import AutoModel, AutoTokenizer, Qwen3Config
except ImportError as exc:
    skip(f"{exc.name} not installed")

try:
    tok = AutoTokenizer.from_pretrained(MODEL_ID, padding_side="left")
except Exception as exc:                                  # offline, no cache
    skip(f"cannot load {MODEL_ID} tokenizer ({exc.__class__.__name__})")

spec = importlib.util.spec_from_file_location("extractor", SCRIPT)
X = importlib.util.module_from_spec(spec)
spec.loader.exec_module(X)

FAILED = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ""))
    if not ok:
        FAILED.append(name)


torch.manual_seed(0)
cfg = Qwen3Config(vocab_size=len(tok), hidden_size=1024, num_hidden_layers=2,
                  num_attention_heads=16, num_key_value_heads=8, head_dim=128,
                  intermediate_size=512, max_position_embeddings=4096,
                  use_cache=True)
model = AutoModel.from_config(cfg).eval().float()

with open(TOKENS_TXT) as f:
    TOKENS = sorted({t for t in f.read().split("\n") if t})[:3000]
PROFILE, PNAME = X.MODEL_PROFILES["qwen3"], "qwen3"
BS, EVERY = 128, 500


# ── the profile's factual claims about the tokenizer ──────────────────
ids = tok("foo")["input_ids"]
check("tokenizer appends EOS itself (last-token pooling reads it)",
      ids[-1] == 151643, f"tok('foo') -> {tok.convert_ids_to_tokens(ids)}")

# ── use_cache=False really does suppress the KV cache ─────────────────
enc = tok(["alpha", "beta"], padding=True, return_tensors="pt")
with torch.no_grad():
    on = model(**enc, use_cache=True)
    off = model(**enc, use_cache=False)


def cache_elems(out):
    pkv = getattr(out, "past_key_values", None)
    if pkv is None:
        return 0
    try:
        return sum(t.numel() for layer in pkv for t in layer)
    except TypeError:
        return sum(k.numel() + v.numel()
                   for k, v in zip(pkv.key_cache, pkv.value_cache))


check("use_cache=True allocates a KV cache we never read",
      cache_elems(on) > 0, f"{cache_elems(on)} elements")
check("use_cache=False allocates none", cache_elems(off) == 0)

# ── last-token pooling picks the right position ───────────────────────
texts = ["a", "getcomponentbyname", "xy"]
enc_b = tok(texts, padding=True, return_tensors="pt")
with torch.no_grad():
    hidden = model(**enc_b, use_cache=False).last_hidden_state
pooled = X.last_token_pool(hidden, enc_b["attention_mask"])
singles = []
for t in texts:
    e = tok([t], padding=True, return_tensors="pt")
    with torch.no_grad():
        singles.append(model(**e, use_cache=False).last_hidden_state[0, -1])
singles = torch.stack(singles)
cos = torch.nn.functional.cosine_similarity(pooled, singles).min().item()
check("left-padded batch pools the same vector as an unpadded single",
      cos > 0.9999, f"min cosine {cos:.6f}")

mean_cos = torch.nn.functional.cosine_similarity(
    torch.nn.functional.normalize(pooled, dim=1),
    torch.nn.functional.normalize(X.mean_token_pool(hidden, enc_b["attention_mask"]),
                                  dim=1)).max().item()
check("mean pooling is a different vector — the silent-swap failure mode",
      mean_cos < 0.99, f"max cosine to mean-pooled {mean_cos:.4f}")

# ── Matryoshka: the script's truncate-then-normalise is the same vector
v = torch.randn(256, 1024)
a = torch.nn.functional.normalize(v[:, :768], p=2, dim=1)
b = torch.nn.functional.normalize(
    torch.nn.functional.normalize(v, p=2, dim=1)[:, :768], p=2, dim=1)
check("truncate->normalise == normalise->truncate->renormalise",
      torch.allclose(a, b, atol=1e-6), f"max delta {(a - b).abs().max():.2e}")


# ── resumability ──────────────────────────────────────────────────────
def extract(m, bs=BS, ckpt=None):
    return X.extract_embeddings(m, tok, TOKENS, "cpu", PROFILE, PNAME,
                                batch_size=bs, checkpoint_path=ckpt,
                                checkpoint_every=EVERY)


class Killer:
    """Dies after n forwards — a SIGKILL, not a Ctrl-C: no unwinding, no
    last-gasp save. Only the periodic checkpoints survive."""

    def __init__(self, model, n):
        self.model, self.n, self.i = model, n, 0

    def __getattr__(self, k):
        return getattr(self.model, k)

    def __call__(self, *a, **kw):
        if self.i >= self.n:
            raise RuntimeError("simulated kill")
        self.i += 1
        return self.model(*a, **kw)


tmp = tempfile.mkdtemp(prefix="extract-ckpt-")
try:
    ck = os.path.join(tmp, "checkpoint.npz")
    ref = extract(model)

    try:
        extract(Killer(model, 9), ckpt=ck)
    except RuntimeError:
        pass
    check("a checkpoint survives the kill", os.path.exists(ck))
    resumed = extract(model, ckpt=ck)
    check("RESUMED RUN IS BYTE-IDENTICAL TO THE UNINTERRUPTED RUN",
          resumed.tobytes() == ref.tobytes())

    fp = X.checkpoint_fingerprint(PNAME, PROFILE["model_name"], TOKENS, 768)

    # a truncated checkpoint — what a non-atomic writer would leave behind
    X.save_checkpoint(ck, ref[:1000], fp)
    with open(ck, "r+b") as f:
        f.truncate(os.path.getsize(ck) // 3)
    check("a corrupt checkpoint is rejected and the run still completes",
          extract(model, ckpt=ck).tobytes() == ref.tobytes())

    # a checkpoint belonging to a different vocabulary (track D changes it)
    other = list(TOKENS)
    other[7] = "changed_by_the_vocabulary_filter"
    X.save_checkpoint(ck, ref[:1500],
                      X.checkpoint_fingerprint(PNAME, PROFILE["model_name"],
                                               other, 768))
    check("a checkpoint from a different token list is refused, not used",
          extract(model, ckpt=ck).tobytes() == ref.tobytes())

    # ...and one from a different model
    X.save_checkpoint(ck, ref[:1500],
                      X.checkpoint_fingerprint("nomic",
                                               "nomic-ai/nomic-embed-code",
                                               TOKENS, 768))
    check("a checkpoint from a different model profile is refused",
          extract(model, ckpt=ck).tobytes() == ref.tobytes())

    # the write is atomic: a crash mid-save keeps the previous checkpoint
    X.save_checkpoint(ck, ref[:2000], fp)
    good = open(ck, "rb").read()
    real_savez = np.savez

    def exploding_savez(*a, **kw):
        real_savez(*a, **kw)
        raise OSError("disk full, mid-write")

    np.savez = exploding_savez
    try:
        X.save_checkpoint(ck, ref[:2500], fp)
    except OSError:
        pass
    finally:
        np.savez = real_savez
    check("a crash during the write leaves the previous checkpoint intact",
          open(ck, "rb").read() == good)
    check("and leaves no temp file behind",
          os.listdir(tmp) == ["checkpoint.npz"], str(os.listdir(tmp)))

    # batch size must not change what is computed
    worst = max(float(np.abs(extract(model, bs=bs) - ref).max())
                for bs in (32, 512))
    check("BATCH_SIZE does not change what is computed",
          worst < (1.0 / 127.0) / 100,
          f"max delta {worst:.2e} vs int8 quantisation step {1/127:.5f}")
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print()
print(f"{'FAILED: ' + ', '.join(FAILED) if FAILED else 'all checks passed'}")
sys.exit(1 if FAILED else 0)
