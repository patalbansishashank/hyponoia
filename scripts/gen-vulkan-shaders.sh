#!/usr/bin/env bash
set -euo pipefail

# gen-vulkan-shaders.sh — compile vendored/llama.cpp's GLSL into the C++ arrays
# ggml-vulkan.cpp links against.
#
#   scripts/gen-vulkan-shaders.sh --features <shader-src-dir> <glslc> <cache>
#   scripts/gen-vulkan-shaders.sh --generate <shader-src-dir> <glslc> <cache> \
#                                            <out-dir> <cxx>
#
# ggml's CMake does this with an ExternalProject and 136 add_custom_command
# rules. The work itself is small: build one host tool from one .cpp, ask it
# for a header, then ask it once per .comp for a .cpp holding that shader's
# SPIR-V as a byte array. This script is that, so Makefile.hyp does not have to
# grow a second build system to get a GPU lane.
#
# The ~200 MB it emits is a BUILD ARTIFACT and is never committed — see
# vendored/llama.cpp/NOTICE. What supply-chain integrity needs is the GLSL, and
# that IS vendored and checksummed.
#
# --features exists because the SAME set of extension defines has to reach both
# the generator and ggml-vulkan.cpp. Give the backend a define the generator
# did not get and it declares pipelines nobody emitted; the link fails on
# missing symbols, which is the good outcome — the bad one is the reverse.
# Which extensions glslc supports is PROBED, exactly as ggml's CMakeLists does:
# this machine's glslc supports coopmat, coopmat2, coopmat2 decode_vector,
# integer_dot, bfloat16 and E4M3 but NOT E2M1. Result is cached, because make
# expands this on every invocation of every target.

MODE="${1:-}"
SRC_DIR="${2:-}"
GLSLC="${3:-}"
CACHE="${4:-}"

if [[ -z "$MODE" || -z "$SRC_DIR" || -z "$GLSLC" || -z "$CACHE" ]]; then
    echo "usage: $0 --features|--generate <src> <glslc> <cache> [<out> <cxx>]" >&2
    exit 2
fi

probe_features() {
    local defs=() ft="$SRC_DIR/feature-tests"
    local ext test_file def
    while read -r ext test_file def; do
        [[ -n "$ext" ]] || continue
        [[ -f "$ft/$test_file" ]] || continue
        local err
        err="$("$GLSLC" -o /dev/null -fshader-stage=compute --target-env=vulkan1.3 \
               "$ft/$test_file" 2>&1 || true)"
        if [[ "$err" != *"extension not supported: $ext"* ]]; then
            defs+=("-D$def")
        fi
    done <<'EOF'
GL_KHR_cooperative_matrix coopmat.comp GGML_VULKAN_COOPMAT_GLSLC_SUPPORT
GL_NV_cooperative_matrix2 coopmat2.comp GGML_VULKAN_COOPMAT2_GLSLC_SUPPORT
GL_NV_cooperative_matrix_decode_vector coopmat2_decode_vector.comp GGML_VULKAN_COOPMAT2_DECODE_VECTOR_GLSLC_SUPPORT
GL_EXT_integer_dot_product integer_dot.comp GGML_VULKAN_INTEGER_DOT_GLSLC_SUPPORT
GL_EXT_bfloat16 bfloat16.comp GGML_VULKAN_BFLOAT16_GLSLC_SUPPORT
GL_EXT_float_e2m1 float_e2m1.comp GGML_VULKAN_FLOAT_E2M1_GLSLC_SUPPORT
GL_EXT_float_e4m3 float_e4m3.comp GGML_VULKAN_FLOAT_E4M3_GLSLC_SUPPORT
EOF
    printf '%s' "${defs[*]-}"
}

features() {
    if [[ -s "$CACHE" ]]; then cat "$CACHE"; return; fi
    command -v "$GLSLC" >/dev/null 2>&1 || return 0
    mkdir -p "$(dirname "$CACHE")"
    probe_features > "$CACHE"
    cat "$CACHE"
}

case "$MODE" in
--features)
    features
    ;;
--generate)
    OUT_DIR="${5:?out-dir required}"
    CXX_BIN="${6:?cxx required}"
    command -v "$GLSLC" >/dev/null 2>&1 || {
        echo "BLOCKED: glslc not found ($GLSLC)." >&2
        echo "  The Vulkan lane needs it to compile ggml's GLSL. Install shaderc," >&2
        echo "  or build with HYP_ASK_GPU=none for a CPU-only \`ask\` lane." >&2
        exit 1
    }
    read -r -a DEFS <<< "$(features)"
    mkdir -p "$OUT_DIR/spv"
    GEN="$OUT_DIR/vulkan-shaders-gen"
    HPP="$OUT_DIR/ggml-vulkan-shaders.hpp"

    "$CXX_BIN" -std=c++17 -O2 -w ${DEFS[@]+"${DEFS[@]}"} \
        -o "$GEN" "$SRC_DIR/vulkan-shaders-gen.cpp" -lpthread

    "$GEN" --output-dir "$OUT_DIR/spv" --target-hpp "$HPP"

    n=0
    for comp in "$SRC_DIR"/*.comp; do
        out="$OUT_DIR/$(basename "$comp").cpp"
        "$GEN" --glslc "$GLSLC" --source "$comp" \
               --output-dir "$OUT_DIR/spv" --target-hpp "$HPP" --target-cpp "$out" &
        while [[ "$(jobs -rp | wc -l)" -ge "$(nproc)" ]]; do wait -n; done
        n=$((n + 1))
    done
    wait
    echo "generated $n vulkan shader translation units in $OUT_DIR"
    ;;
*)
    echo "unknown mode: $MODE" >&2
    exit 2
    ;;
esac
