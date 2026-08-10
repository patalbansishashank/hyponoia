#!/usr/bin/env bash
set -euo pipefail

# vendor-llama-cpp.sh — reproduce vendored/llama.cpp/ from an upstream checkout.
#
# The `ask` lane runs Qwen3-Embedding-0.6B in-process (NEXT-STEPS.md §2.1,
# runs/ASK/T2-inference-runtime.json). llama.cpp is the vehicle. This script is
# the record of WHICH files were taken and WHAT was changed, so the vendored
# tree can be re-derived and diffed rather than trusted.
#
#   scripts/vendor-llama-cpp.sh <path-to-llama.cpp-checkout>
#
# Upstream: https://github.com/ggml-org/llama.cpp
# Pinned:   74ce15741b420b8d6f12e720398458b576c51c2c  (2026-08-09)
#
# Sources are read with `git show <pin>:<path>` rather than from the working
# tree, so a dirty checkout cannot smuggle an unrecorded edit into the vendored
# copy — the two patches below are the ONLY difference from upstream.
#
# WHAT IS TAKEN
#   ggml/include              ggml's public headers
#   ggml/src (top level)      ggml-base + the backend registry
#   ggml/src/ggml-cpu         the CPU backend, minus spacemit/ — a RISC-V TCM
#                             helper this build never compiles and the only
#                             file in the set that calls dlopen()
#   ggml/src/ggml-vulkan      the Vulkan backend: ggml-vulkan.cpp and the GLSL
#                             shader sources. The ~200 MB of C++ that
#                             vulkan-shaders-gen emits from them is a BUILD
#                             artifact and is never committed.
#   src, include              the llama library
#
# WHAT IS NOT TAKEN
#   tools/ examples/ tests/ common/ vendor/ (minja, cpp-httplib), every other
#   backend (CUDA, HIP, SYCL, Metal, OpenCL, RPC, WebGPU, ...), the Python
#   conversion scripts, and the CMake build. None of it is reachable from the
#   two entry points src/ask/ask_llama.cpp calls.
#
# THE TWO PATCHES, applied at the end and described in HYPONOIA-PATCHES.md
#   1. ggml-backend-dl.cpp   dlopen/dlsym -> nullptr. Runtime .so backend
#      loading is dead code in a static build (every backend is registered
#      statically) and hyponoia's binary has ZERO undefined dlopen today.
#   2. ggml.c                ggml_print_backtrace() forks and execlp()s gdb on
#      abort. A shipped binary that spawns a debugger against its own pid reads
#      like malware and trips scripts/security-vendored.sh:303. The dead branch
#      is DELETED rather than `#if 0`-ed: the gate greps text, not preprocessor
#      state, so a commented-out fork() still blocks.

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <path-to-llama.cpp-checkout>" >&2
    exit 2
fi

SRC="$(cd "$1" && pwd -P)"
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
DST="$ROOT/vendored/llama.cpp"
PIN="74ce15741b420b8d6f12e720398458b576c51c2c"

have="$(git -C "$SRC" rev-parse HEAD)"
if [[ "$have" != "$PIN" ]]; then
    echo "REFUSING: checkout is at $have, not the pinned $PIN" >&2
    exit 1
fi

rm -rf "$DST"
mkdir -p "$DST"

# Paths to take, resolved against the pinned tree.
git -C "$SRC" ls-tree -r --name-only "$PIN" \
  | grep -E '^(ggml/include/.*\.(h|hpp)|ggml/src/[^/]+\.(c|cpp|h|hpp)|ggml/src/ggml-cpu/.*\.(c|cpp|h|hpp)|src/.*\.(c|cpp|h|hpp)|include/llama(-cpp)?\.h|ggml/src/ggml-vulkan/ggml-vulkan\.cpp|ggml/src/ggml-vulkan/vulkan-shaders/.*\.(comp|glsl|cpp|h)|LICENSE)$' \
  | grep -v '^ggml/src/ggml-cpu/spacemit/' \
  | while IFS= read -r f; do
        mkdir -p "$DST/$(dirname "$f")"
        git -C "$SRC" show "$PIN:$f" > "$DST/$f"
    done

# ── Patch 1: no dlopen/dlsym ────────────────────────────────────────
python3 - "$DST/ggml/src/ggml-backend-dl.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()

old = """dl_handle * dl_load_library(const fs::path & path) {
    dl_handle * handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    return handle;
}

void * dl_get_sym(dl_handle * handle, const char * name) {
    return dlsym(handle, name);
}

const char * dl_error() {
    const char *rslt = dlerror();
    return rslt != nullptr ? rslt : "";
}"""
new = """/* HYPONOIA PATCH 1 of 2 — see vendored/llama.cpp/HYPONOIA-PATCHES.md.
   Hyponoia registers every ggml backend statically, so runtime .so loading is
   dead code by construction. Left in, it puts dlopen/dlsym in the binary's
   undefined dynamic symbols — a property the shipped binary does not have
   today (`nm -D --undefined-only build/c/hyponoia | grep -c dlopen` = 0) —
   breaks `-static`, and trips scripts/security-vendored.sh. */
dl_handle * dl_load_library(const fs::path & path) {
    (void) path;
    return nullptr;
}

void * dl_get_sym(dl_handle * handle, const char * name) {
    (void) handle; (void) name;
    return nullptr;
}

const char * dl_error() {
    return "dynamic backend loading is compiled out";
}"""
if old not in s:
    sys.stderr.write("PATCH 1 FAILED: POSIX dl_* bodies not found as expected\n")
    sys.exit(1)
open(p, "w").write(s.replace(old, new))
PY

# ── Patch 2: no fork + execlp("gdb") on abort ───────────────────────
python3 - "$DST/ggml/src/ggml.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()

anchor = "void ggml_print_backtrace(void) {\n"
i = s.find(anchor)
if i < 0:
    sys.stderr.write("PATCH 2 FAILED: ggml_print_backtrace not found\n")
    sys.exit(1)
end = s.find("\n}\n", i)
if end < 0:
    sys.stderr.write("PATCH 2 FAILED: function end not found\n")
    sys.exit(1)
body = s[i + len(anchor):end]
if "fork()" not in body or "execlp" not in body:
    sys.stderr.write("PATCH 2 FAILED: expected fork/execlp in the replaced body\n")
    sys.exit(1)

new = anchor + """    /* HYPONOIA PATCH 2 of 2 — see vendored/llama.cpp/HYPONOIA-PATCHES.md.
       Upstream forks and execlp()s gdb (then lldb) against its own pid to dump
       a backtrace on abort. A shipped binary that spawns a debugger on itself
       reads exactly like malware to a generic classifier, and fork/execl in
       vendored code is a hard block in scripts/security-vendored.sh:303 — one
       that greps TEXT, so the dead branch is deleted rather than #if 0-ed.
       backtrace()/backtrace_symbols_fd() spawn nothing. */
    ggml_print_backtrace_symbols();"""
s = s[:i] + new + s[end:]
open(p, "w").write(s)
PY

echo "vendored $(find "$DST" -type f | wc -l) files into vendored/llama.cpp/"
