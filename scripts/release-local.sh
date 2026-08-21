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
UNSIGNED=0
for a in "$@"; do
    [ "$a" = "--publish" ] && PUBLISH=1
    [ "$a" = "--unsigned" ] && UNSIGNED=1
done

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

    # ldd EXITS 1 on a static binary, and this script runs under `set -o
    # pipefail`, so `ldd ... | grep -q ...` returns 1 even when grep matched.
    # That reported a correctly-static portable build as not static and refused
    # a good release. CI never hit it because GitHub's plain `run:` steps are
    # `bash -e` without pipefail. Capture once, match against the variable.
    linkage="$(ldd build/c/hyponoia 2>&1 || true)"
    kind="$(file -b build/c/hyponoia 2>/dev/null || true)"

    case "$suffix" in
        *-portable)
            # Static or it is not portable. Two independent witnesses, because
            # this is the property the whole variant exists for: ldd's own
            # words and file(1)'s classification.
            case "$linkage$kind" in
                *"not a dynamic executable"* | *"statically linked"*)
                    echo "  verified: statically linked" ;;
                *)
                    echo "release-local: $suffix is NOT statically linked — refusing." >&2
                    exit 1 ;;
            esac
            ;;
        *-gpu)
            # The property this variant exists for.
            case "$linkage" in
                *libvulkan.so.1*) echo "  verified: links libvulkan.so.1" ;;
                *)
                    echo "release-local: $suffix does not link libvulkan.so.1 — refusing." >&2
                    exit 1 ;;
            esac
            ;;
        *)
            # The ordinary build must NOT pull Vulkan in; otherwise it is
            # silently the gpu variant under a different name.
            case "$linkage" in
                *libvulkan.so.1*)
                    echo "release-local: the plain variant links libvulkan — refusing." >&2
                    exit 1 ;;
                *) echo "  verified: dynamic, no Vulkan" ;;
            esac
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
# Keyless cosign needs an OIDC identity. CI had one ambiently (GitHub's
# id-token); on a workstation cosign falls back to an INTERACTIVE browser
# flow, which cannot run unattended and fails at the token POST.
#
# --unsigned exists so that shipping unsigned is a decision someone typed,
# never something that quietly happened because auth was unavailable. A
# script that degraded to unsigned on error would produce unsigned releases
# forever and never say so -- the same shape as the bug this file exists to
# prevent.
if [ "$UNSIGNED" -eq 1 ]; then
    step "sign — SKIPPED (--unsigned)"
    echo "  No .bundle files. The release will say so." >&2
else
    step "sign (cosign keyless)"
    for f in "${ARCHIVES[@]}" checksums.txt; do
        if ! COSIGN_EXPERIMENTAL=1 cosign sign-blob --yes --bundle "${f}.bundle" "$f" \
            >/dev/null 2>&1; then
            echo "release-local: cosign could not sign $f." >&2
            echo "  Keyless signing needs an OIDC identity and opens a browser; there is" >&2
            echo "  none here. Either run it interactively, or re-run with --unsigned to" >&2
            echo "  publish without signatures deliberately." >&2
            exit 1
        fi
        echo "  signed $f"
    done
fi

step "staged"
ls -la "$OUT"

if [ "$PUBLISH" -ne 1 ]; then
    echo ""
    echo "Nothing was published. Re-run with --publish to create the release:"
    echo "  scripts/release-local.sh $VERSION --publish"
    exit 0
fi

step "publish $VERSION"
UPLOAD=("${ARCHIVES[@]}" checksums.txt)
if [ "$UNSIGNED" -eq 0 ]; then
    UPLOAD+=(./*.bundle)
fi
gh release create "$VERSION" --repo patalbansishashank/hyponoia \
    --title "$VERSION" --generate-notes "${UPLOAD[@]}"

echo ""
echo "published: https://github.com/patalbansishashank/hyponoia/releases/tag/$VERSION"
