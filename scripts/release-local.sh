#!/usr/bin/env bash
# release-local.sh — THE release. All three linux-amd64 variants, signed.
#
# ═══════════════════════════════════════════════════════════════════════════
# WHY THIS FILE EXISTS
# ═══════════════════════════════════════════════════════════════════════════
#
# CI used to cut releases with THREE build jobs — build-unix, build-linux-
# portable, build-linux-gpu — each ending in its own scripts/package-release.sh
# call. When CI was deleted, the release was reproduced by hand by running
# package-release.sh ONCE. v0.10.7 and v0.10.8 therefore shipped one archive
# where v0.10.6 had shipped three, and nothing noticed, because a release with
# assets looks exactly like a release with the RIGHT assets.
#
# That is not a hypothetical cost. pkg/homebrew, pkg/npm/install.js and
# pkg/pypi all deliberately fetch the `-portable` archive on Linux, because the
# ordinary one links glibc 2.38+ and dies with "version `GLIBC_2.38' not found"
# on anything older. Every one of those installers was pointing at an asset
# that no longer existed. This repository has the same defect on record already
# ("installers asked for an archive the build never made"), which is the
# argument for a script rather than a better memory.
#
# So the variant list lives HERE, once, and every leg is DERIVED from it. There
# is no path through this file that publishes a subset: the upload happens after
# the loop or not at all, and each variant must pass the property that defines
# it before it is allowed into the set.
#
# ═══════════════════════════════════════════════════════════════════════════
# BUILD-TIME PREREQUISITES (all four, or this refuses)
# ═══════════════════════════════════════════════════════════════════════════
#
#   libz.a                          portable: LDFLAGS carries -lz and a static
#                                   link needs the static archive. Debian gets
#                                   it from zlib1g-dev, Arch from zlib-static.
#   vulkan/vulkan.h + vulkan.hpp    gpu (Debian libvulkan-dev, Arch
#                                   vulkan-headers)
#   spirv/unified1/spirv.hpp        gpu (spirv-headers on both)
#   glslc                           gpu: compiles the shaders to SPIR-V offline.
#                                   No GPU is needed to BUILD.
#   cosign                          the .bundle signature beside every archive.
#
# ═══════════════════════════════════════════════════════════════════════════
# USAGE
# ═══════════════════════════════════════════════════════════════════════════
#
#   scripts/release-local.sh v0.10.9              build, verify, sign, stage
#   scripts/release-local.sh v0.10.9 --publish    ... and gh release create
#
# Without --publish nothing leaves the machine; the staged directory is printed
# so the archives can be inspected before anything is irreversible.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="${1:-}"
PUBLISH=0
[ "${2:-}" = "--publish" ] && PUBLISH=1

if [ -z "$VERSION" ]; then
    sed -n '2,52p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
    exit 2
fi
case "$VERSION" in
    v[0-9]*) ;;
    *)
        echo "release-local: version must look like v0.10.9 (got '$VERSION')" >&2
        exit 2
        ;;
esac

# ── THE VARIANT TABLE. Everything below is derived from it. ──────────────────
#
# suffix | extra make/build args | the property that defines this variant
#
# The third column is not decoration: a "portable" archive that is not static,
# or a "gpu" archive that does not link the Vulkan loader, is a mislabelled
# artifact, and a mislabelled artifact is worse than a missing one because it
# installs.
VARIANTS=(
    "amd64|"
    "amd64-portable|STATIC=1"
    "amd64-gpu|HYP_ASK_GPU=vulkan STATIC=0"
)

OUT="${HYP_RELEASE_OUT:-/tmp/hyp-release-$VERSION}"
rm -rf "$OUT"
mkdir -p "$OUT"

step() { printf '\n\033[1m── %s\033[0m\n' "$*" >&2; }

# ── Prerequisites, all of them, before any build burns twenty minutes ────────
step "prerequisites"
missing=0
need_file() {
    if [ -f "$1" ]; then
        echo "  OK   $1"
    else
        echo "  MISS $1  ($2)" >&2
        missing=1
    fi
}
need_cmd() {
    if command -v "$1" >/dev/null 2>&1; then
        echo "  OK   $1"
    else
        echo "  MISS $1  ($2)" >&2
        missing=1
    fi
}
need_file /usr/lib/libz.a "static zlib — Debian zlib1g-dev, Arch zlib-static"
need_file /usr/include/vulkan/vulkan.h "Debian libvulkan-dev, Arch vulkan-headers"
need_file /usr/include/vulkan/vulkan.hpp "Debian libvulkan-dev, Arch vulkan-headers"
need_file /usr/include/spirv/unified1/spirv.hpp "spirv-headers"
need_cmd glslc "shaderc"
need_cmd cosign "sigstore signing"
need_cmd gh "GitHub CLI"
if [ "$missing" -ne 0 ]; then
    echo "release-local: refusing — a missing prerequisite silently drops a variant." >&2
    exit 1
fi

SHA="$(git rev-parse --short=12 HEAD)"
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "release-local: refusing — the tree is dirty, so the stamped sha would name" >&2
    echo "  a commit that does not describe what was built." >&2
    exit 1
fi
echo "  building $VERSION from $SHA"

# ── Build every variant, verify each one's defining property ─────────────────
ARCHIVES=()
for entry in "${VARIANTS[@]}"; do
    suffix="${entry%%|*}"
    args="${entry#*|}"
    step "build $suffix ${args:+($args)}"

    # shellcheck disable=SC2086 # args is a deliberate word-split of build flags
    bash scripts/build.sh --with-ui --version "$VERSION" --build-sha "$SHA" \
        CC=gcc CXX=g++ $args

    case "$suffix" in
        *-portable)
            # CI's own check: static or it is not portable.
            if ! { ldd build/c/hyponoia 2>&1 | grep -q "not a dynamic executable" ||
                ldd build/c/hyponoia 2>&1 | grep -q "statically linked"; }; then
                echo "release-local: $suffix is NOT statically linked — refusing." >&2
                exit 1
            fi
            echo "  verified: statically linked"
            ;;
        *-gpu)
            # The property this variant exists for.
            if ! ldd build/c/hyponoia 2>&1 | grep -q 'libvulkan\.so\.1'; then
                echo "release-local: $suffix does not link libvulkan.so.1 — refusing." >&2
                exit 1
            fi
            echo "  verified: links libvulkan.so.1"
            ;;
        *)
            # The ordinary build must NOT be static and must NOT pull Vulkan in;
            # otherwise it is silently one of the other two under a third name.
            if ldd build/c/hyponoia 2>&1 | grep -q 'libvulkan\.so\.1'; then
                echo "release-local: the plain variant links libvulkan — refusing." >&2
                exit 1
            fi
            echo "  verified: dynamic, no Vulkan"
            ;;
    esac

    bash scripts/package-release.sh linux "$suffix" --variant ui --out-dir "$OUT"
    ARCHIVES+=("hyponoia-ui-linux-$suffix.tar.gz")
done

# ── Every declared archive exists, and nothing else is in the directory ──────
step "archive set"
cd "$OUT"
for a in "${ARCHIVES[@]}"; do
    [ -f "$a" ] || {
        echo "release-local: $a was declared but not produced — refusing." >&2
        exit 1
    }
done
found=$(find . -maxdepth 1 -name '*.tar.gz' | wc -l)
if [ "$found" -ne "${#ARCHIVES[@]}" ]; then
    echo "release-local: ${#ARCHIVES[@]} archives declared, $found present — refusing." >&2
    exit 1
fi
sha256sum "${ARCHIVES[@]}" > checksums.txt
cat checksums.txt

# ── Sign every archive and the checksum file ────────────────────────────────
#
# Keyless: cosign opens a browser for an OIDC identity the first time. The
# bundle is what a verifier reads, and it goes beside its archive with the name
# the previous CI-built releases used, so nothing downstream has to learn a new
# one.
step "sign (cosign keyless)"
for f in "${ARCHIVES[@]}" checksums.txt; do
    COSIGN_EXPERIMENTAL=1 cosign sign-blob --yes --bundle "${f}.bundle" "$f" >/dev/null
    echo "  signed $f"
done

step "staged"
ls -la "$OUT"

if [ "$PUBLISH" -ne 1 ]; then
    echo ""
    echo "Nothing was published. Re-run with --publish to create the release:"
    echo "  scripts/release-local.sh $VERSION --publish"
    exit 0
fi

step "publish $VERSION"
gh release create "$VERSION" --repo patalbansishashank/hyponoia \
    --title "$VERSION" --generate-notes \
    "${ARCHIVES[@]}" checksums.txt ./*.bundle

echo ""
echo "published: https://github.com/patalbansishashank/hyponoia/releases/tag/$VERSION"
