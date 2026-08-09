#!/usr/bin/env python3
"""Identity test for the pretrained-vector artifact pipeline.

Regenerates every artifact that does NOT require the embedding model, using the
real writer stage of scripts/extract_nomic_vectors.py, and compares the result
byte-for-byte against what is committed.

    python3 scripts/verify-vector-artifacts.py [vendored/nomic]

Why this exists
---------------
Four of the five artifacts in the vector directory are pure functions of the
other two:

    code_tokens.h        = f(code_tokens.txt)
    code_vectors.h       = f(len(code_tokens.txt), dim from the blob header)
    code_vectors_blob.S  = f(output directory)   — no data input at all
    code_vectors.bin     = f(float vectors)      — but the int8 quantizer is
                           idempotent, so re-running it over the DEQUANTIZED
                           committed table must return the committed bytes

Only code_tokens.txt genuinely requires the upstream tokenizer, and the vector
VALUES genuinely require the model. Everything else is reproducible offline, so
regressions in the writer stage are catchable without a GPU — which is the
whole point: the first time this was run it found that the committed
code_vectors_blob.S and the generator had diverged, and that regenerating would
have reverted a shipped executable-stack fix.

Exit 0 = every reproducible artifact is byte-identical. Exit 1 = drift.
The model is never loaded; torch/transformers are stubbed for import only.
"""

import hashlib
import importlib.util
import struct
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("vendored/nomic")
if not TARGET.is_absolute():
    TARGET = ROOT / TARGET

# The extractor imports torch/transformers at module scope for the inference
# stage. Stub them so the writer stage can be imported on a machine with no
# GPU, no model and no 14 GB of wheels. Nothing stubbed here is ever called.
_torch = types.ModuleType("torch")
_torch.set_num_threads = lambda n: None
_torch.set_num_interop_threads = lambda n: None
_torch.get_num_threads = lambda: 1
_torch.float16 = "float16"
_cuda = types.ModuleType("torch.cuda")
_cuda.is_available = lambda: False
_torch.cuda = _cuda
_nn = types.ModuleType("torch.nn")
_nn.functional = types.ModuleType("torch.nn.functional")
_torch.nn = _nn
sys.modules.setdefault("torch", _torch)
sys.modules.setdefault("torch.cuda", _cuda)
sys.modules.setdefault("torch.nn", _nn)
_tf = types.ModuleType("transformers")
_tf.AutoModel = object
_tf.AutoTokenizer = object
sys.modules.setdefault("transformers", _tf)

import numpy as np  # noqa: E402  (after the stubs, deliberately)

_spec = importlib.util.spec_from_file_location(
    "extract_nomic_vectors", ROOT / "scripts" / "extract_nomic_vectors.py")
extractor = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(extractor)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    import tempfile

    tokens_txt = TARGET / "code_tokens.txt"
    vectors_bin = TARGET / "code_vectors.bin"
    for required in (tokens_txt, vectors_bin):
        if not required.is_file():
            print(f"FAIL: missing input artifact: {required}")
            return 1

    tokens = tokens_txt.read_text(encoding="utf-8").split("\n")
    if tokens and tokens[-1] == "":
        tokens.pop()

    with open(vectors_bin, "rb") as f:
        count, dim = struct.unpack("<ii", f.read(8))
    expected_bytes = 8 + count * dim
    actual_bytes = vectors_bin.stat().st_size
    if actual_bytes != expected_bytes:
        print(f"FAIL: {vectors_bin} is {actual_bytes} bytes, header implies "
              f"{expected_bytes} ({count} x {dim} + 8)")
        return 1
    if len(tokens) != count:
        print(f"FAIL: code_tokens.txt has {len(tokens)} tokens, blob header "
              f"says {count}")
        return 1

    print(f"target        : {TARGET.relative_to(ROOT)}")
    print(f"tokens        : {len(tokens)}")
    print(f"blob header   : count={count} dim={dim}  ({actual_bytes} bytes)")
    print(f"model loaded  : NO — writer stage only")
    print()

    # Dequantize the committed int8 table back to the float domain the writer
    # expects. round(x/127 * 127) is the identity on [-127, 127], so a faithful
    # write_bin must return the committed bytes exactly. This proves the
    # container format and the quantizer, NOT the model.
    with open(vectors_bin, "rb") as f:
        f.seek(8)
        quantized = np.frombuffer(f.read(), dtype=np.int8).reshape(count, dim)
    dequantized = quantized.astype(np.float32) / 127.0

    with tempfile.TemporaryDirectory(prefix="hyp-vector-identity-") as tmp:
        out = Path(tmp) / TARGET.name
        out.mkdir()
        extractor.write_artifacts(
            out, tokens, dequantized, dim,
            guard_prefix=extractor.HEADER_GUARD_PREFIX,
            incbin_path=f"{TARGET.relative_to(ROOT).as_posix()}/code_vectors.bin",
        )
        print()

        # code_tokens.txt is regenerated from itself, so its match is a tautology
        # and it is reported as NOT PROVEN rather than as evidence.
        reproducible = ("code_vectors.bin", "code_tokens.h", "code_vectors.h",
                        "code_vectors_blob.S")
        drift = 0
        for name in reproducible:
            want, got = sha256(TARGET / name), sha256(out / name)
            status = "IDENTICAL" if want == got else "DRIFT    "
            if want != got:
                drift += 1
            print(f"{status}  {name}")
            print(f"           committed   {want}")
            print(f"           regenerated {got}")

    print()
    print("NOT PROVEN  code_tokens.txt — the vocabulary comes from the upstream")
    print("            tokenizer; regenerating it from itself proves nothing.")
    print("NOT PROVEN  the vector VALUES in code_vectors.bin — only the container")
    print("            and the int8 quantizer round-trip are proven here.")
    print()
    if drift:
        print(f"=== ARTIFACT IDENTITY CHECK FAILED ({drift} drifted) ===")
        print("The generator and the committed artifacts disagree. Either the")
        print("artifacts were hand-edited or the generator regressed. Do NOT")
        print("regenerate until they agree — a regeneration would silently")
        print("discard whatever the hand edit was protecting.")
        return 1
    print("=== Artifact identity check passed ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
