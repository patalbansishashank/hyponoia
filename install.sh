#!/usr/bin/env bash
set -euo pipefail

# install.sh — One-line installer for hyponoia.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/patalbansishashank/hyponoia/main/install.sh | bash
#   curl -fsSL ... | bash -s -- --ui          # Install the UI variant
#   curl -fsSL ... | bash -s -- --gpu         # GPU (Vulkan) build, linux-amd64 only
#   curl -fsSL ... | bash -s -- --dir /path   # Custom install directory
#
# Environment:
#   HYP_DOWNLOAD_URL  Override base URL for downloads (for testing)

# Wrap in main() to prevent partial execution from piped downloads.
# If curl|bash is interrupted mid-transfer, bash would execute the partial
# script. With this wrapper, the function is defined but main() is never
# called because the final line hasn't arrived yet.
main() {

REPO="patalbansishashank/hyponoia"
INSTALL_DIR="$HOME/.local/bin"
# The build publishes only the UI variant, so this is the only archive that exists.
VARIANT="ui"
SKIP_CONFIG=false

# Linkage flavour, orthogonal to VARIANT. The release publishes exactly ONE GPU
# archive -- hyponoia-ui-linux-amd64-gpu.tar.gz -- built HYP_ASK_GPU=vulkan
# STATIC=0, so it is dynamically linked against libvulkan.so.1 and is the only
# build whose `embed` pass runs on the GPU.
#
# GPU_CHOICE empty means "not asked". That is not the same as "no": this script
# IS the updater (`hyponoia update` prints `bash <dir>/install.sh`, with no
# flags), so an unasked run reuses whatever flavour is already installed. A GPU
# install that quietly became a CPU install on update would be discoverable only
# by watching embed throughput.
GPU_CHOICE=""
GPU=false
GPU_INHERITED=false
HYP_DOWNLOAD_URL="${HYP_DOWNLOAD_URL:-https://github.com/${REPO}/releases/latest/download}"

# Security: every remote hop must remain HTTPS. Plain HTTP is accepted only
# for an exact loopback authority used by local smoke tests, with redirects
# disabled so a local fixture cannot bounce the installer to the network.
is_loopback_http_url() {
    [[ "$1" =~ ^http://(localhost|127\.0\.0\.1|\[::1\])(:[0-9]+)?([/?\#].*)?$ ]]
}

if [[ "$HYP_DOWNLOAD_URL" == https://* ]]; then
    HYP_DOWNLOAD_LOOPBACK=false
elif is_loopback_http_url "$HYP_DOWNLOAD_URL"; then
    HYP_DOWNLOAD_LOOPBACK=true
else
    echo "error: refusing non-HTTPS download URL: $HYP_DOWNLOAD_URL" >&2
    exit 1
fi

download_file() {
    local url="$1"
    local destination="$2"
    local progress="$3"
    if [ "$HYP_DOWNLOAD_LOOPBACK" = true ]; then
        is_loopback_http_url "$url" || {
            echo "error: loopback download escaped its authority: $url" >&2
            return 1
        }
        if command -v curl &>/dev/null; then
            local curl_args=(-fS --noproxy '*' --proto '=http')
            [ "$progress" = true ] && curl_args+=(--progress-bar) || curl_args+=(-s)
            curl "${curl_args[@]}" -o "$destination" "$url"
        elif command -v wget &>/dev/null; then
            local wget_args=(--no-proxy --max-redirect=0)
            [ "$progress" = true ] && wget_args+=(--show-progress) || wget_args+=(-q)
            wget "${wget_args[@]}" -O "$destination" "$url"
        else
            echo "error: curl or wget required" >&2
            return 1
        fi
        return
    fi

    [[ "$url" == https://* ]] || {
        echo "error: HTTPS download downgraded: $url" >&2
        return 1
    }
    if command -v curl &>/dev/null; then
        local curl_args=(-fSL --max-redirs 5 --proto '=https' --proto-redir '=https')
        [ "$progress" = true ] && curl_args+=(--progress-bar) || curl_args+=(-sS)
        curl "${curl_args[@]}" -o "$destination" "$url"
    elif command -v wget &>/dev/null; then
        local wget_args=(--https-only --max-redirect=5)
        [ "$progress" = true ] && wget_args+=(--show-progress) || wget_args+=(-q)
        wget "${wget_args[@]}" -O "$destination" "$url"
    else
        echo "error: curl or wget required" >&2
        return 1
    fi
}

# Everything --gpu can be refused for, said once. Printed to stdout for a note
# and redirected to stderr at the refusal sites, so the platform limit, the
# runtime dependency and the narrow scope of the acceleration always travel
# together with whatever verdict they explain.
gpu_scope_note() {
    echo "  The GPU build is published for linux-amd64 only. It links a Vulkan"
    echo "  loader (libvulkan.so.1) directly, so that library must be present at"
    echo "  runtime, and it accelerates the 'hyponoia embed' pass only -- 'ask'"
    echo "  encodes the question on the CPU in every build."
}

for arg in "$@"; do
    case "$arg" in
        --ui)           VARIANT="ui" ;;
        --gpu)          GPU_CHOICE="gpu" ;;
        --cpu)          GPU_CHOICE="cpu" ;;
        # The build publishes only the UI variant, so refuse rather than
        # silently hand a user who asked for standard a different archive.
        --standard)
            echo "error: no standard archives are published for this release." >&2
            echo "  The published archive includes the graph UI." >&2
            echo "  Re-run without --standard." >&2
            exit 1
            ;;
        --dir=*)        INSTALL_DIR="${arg#--dir=}" ;;
        --skip-config)  SKIP_CONFIG=true ;;
        --help|-h)
            echo "Usage: install.sh [--ui] [--gpu|--cpu] [--dir=<path>] [--skip-config]"
            echo "  --ui           Install the UI variant (with graph visualization; default)"
            echo "  --gpu          Install the GPU (Vulkan) build. linux-amd64 only:"
            echo "                 it is the only platform a GPU archive is published for."
            echo "                 The binary links libvulkan.so.1 directly, so a Vulkan"
            echo "                 loader must be installed to run it, and the GPU is used"
            echo "                 by 'hyponoia embed' only -- 'ask' encodes the question"
            echo "                 on the CPU in every build."
            echo "  --cpu          Install the CPU build even when the install directory"
            echo "                 already holds a GPU install"
            echo "  --standard     Refused: no standard archives are published"
            echo "  --dir PATH     Install directory (default: ~/.local/bin)"
            echo "  --skip-config  Skip automatic agent configuration"
            echo ""
            echo "With neither --gpu nor --cpu, the flavour already installed in the"
            echo "target directory is reused, so re-running this script as the updater"
            echo "never downgrades a GPU install to the CPU build behind your back."
            exit 0
            ;;
    esac
done
# Handle --dir <path> (space-separated)
prev=""
for arg in "$@"; do
    if [ "$prev" = "--dir" ]; then
        INSTALL_DIR="$arg"
    fi
    prev="$arg"
done

detect_os() {
    case "$(uname -s)" in
        # RETIRED-PLATFORM(macos): refuse here rather than compose a darwin
        # asset name that 404s. See docs/MAINTAINERS.md "Retired platforms".
        Darwin)
            echo "error: no macOS binaries are published for this release." >&2
            echo "  Building hyponoia from source on macOS still works." >&2
            echo "  See docs/INSTALL.md:" >&2
            echo "  https://github.com/${REPO}/blob/main/docs/INSTALL.md" >&2
            exit 1
            ;;
        Linux)                echo "linux" ;;
        MINGW*|MSYS*|CYGWIN*) echo "windows" ;;
        *) echo "error: unsupported OS: $(uname -s)" >&2; exit 1 ;;
    esac
}

detect_arch() {
    local arch
    arch="$(uname -m)"
    case "$arch" in
        arm64|aarch64) echo "arm64" ;;
        x86_64|amd64)
            # Rosetta detection: shell reports x86_64 but hardware is Apple Silicon
            # RETIRED-PLATFORM(macos): unreachable — detect_os exits on Darwin
            # before this runs. Kept so restoring darwin is one edit, not two.
            if [ "$(uname -s)" = "Darwin" ] && sysctl -n machdep.cpu.brand_string 2>/dev/null | grep -qi apple; then
                echo "arm64"
            else
                echo "amd64"
            fi
            ;;
        *) echo "error: unsupported architecture: $arch" >&2; exit 1 ;;
    esac
}

OS=$(detect_os)
ARCH=$(detect_arch)

# RETIRED-PLATFORM(windows-arm64): no native ARM64 Windows asset is published,
# so auto-detected arm64 falls back to the x86-64 build under emulation — what
# these users got before a native build existed. This is deliberately placed
# after auto-detection only. See docs/MAINTAINERS.md "Retired platforms".
if [ "$OS" = "windows" ] && [ "$ARCH" = "arm64" ]; then
    echo "note: no native ARM64 Windows build is published; using the x64 build under emulation."
    ARCH="amd64"
fi

# Build download URL
if [ "$OS" = "windows" ]; then
    EXT="zip"
else
    EXT="tar.gz"
fi

# Linux ships a fully-static "-portable" build; the standard linux binary
# dynamically links glibc 2.38+ and fails on older distros (Debian 11, RHEL 8,
# Ubuntu 20.04). Windows has no such variant.
# RETIRED-PLATFORM(macos): this used to read "macOS/Windows"; darwin no longer
# reaches here. See docs/MAINTAINERS.md "Retired platforms".
PORTABLE=""
[ "$OS" = "linux" ] && PORTABLE="-portable"

# The GPU archive is an ordinary six-member UI archive; only its linkage
# differs, so it carries "-gpu" exactly where the CPU build carries
# "-portable". package-release.sh composes the same name from the same label
# (`linux amd64-gpu --variant ui`), which is why one suffix variable is enough.
#
# Called again if the GPU selection has to be given up after the manifest
# arrives, so the archive, the label and the flavour can never disagree.
compose_archive() {
    if [ "$GPU" = true ]; then
        LINKAGE="-gpu"
        VARIANT_LABEL="$VARIANT + GPU (Vulkan)"
    else
        LINKAGE="$PORTABLE"
        VARIANT_LABEL="$VARIANT (CPU)"
    fi
    if [ "$VARIANT" = "ui" ]; then
        ARCHIVE="hyponoia-ui-${OS}-${ARCH}${LINKAGE}.${EXT}"
    else
        # Unreachable — the build publishes only the UI variant, so --standard
        # exits above. Kept so restoring a standard build is one edit, not two.
        ARCHIVE="hyponoia-${OS}-${ARCH}${LINKAGE}.${EXT}"
    fi
}

# Which flavour is installed in the target directory already? This script is
# the updater: `hyponoia update` prints `bash <dir>/install.sh` and the user
# runs it with no flags, so the choice has to survive on disk or every update
# would reinstall the CPU build over a GPU one. The marker sits beside the
# binary, among the sidecars the install directory already holds. It is written
# only for a GPU install and removed for a CPU one, so its mere presence is the
# whole answer and it cannot outlive the install it describes.
VARIANT_MARKER="$INSTALL_DIR/hyp-install-variant"
RECORDED_FLAVOUR=""
if [ -f "$VARIANT_MARKER" ] && [ ! -L "$VARIANT_MARKER" ]; then
    RECORDED_FLAVOUR=$(head -n 1 "$VARIANT_MARKER" 2>/dev/null | tr -d '[:space:]') || true
fi
case "$GPU_CHOICE" in
    gpu) GPU=true ;;
    cpu) GPU=false ;;
    *)
        if [ "$RECORDED_FLAVOUR" = "gpu" ]; then
            GPU=true
            GPU_INHERITED=true
        fi
        ;;
esac

# No GPU archive exists off linux-amd64, so say that by name rather than
# compose a URL that 404s. An explicitly requested --gpu is a refusal: handing
# back the CPU archive would be discovered only by watching embed throughput.
# An INHERITED selection is different — the marker may have been carried to
# another machine — so it degrades loudly instead of bricking `update`.
if [ "$GPU" = true ] && { [ "$OS" != "linux" ] || [ "$ARCH" != "amd64" ]; }; then
    if [ "$GPU_INHERITED" = true ]; then
        echo "note: $INSTALL_DIR records a GPU install, but no GPU archive is"
        echo "  published for ${OS}-${ARCH}."
        gpu_scope_note
        echo "  Installing the CPU build for this platform instead."
        echo ""
        GPU=false
    else
        echo "error: no GPU archive is published for ${OS}-${ARCH}." >&2
        gpu_scope_note >&2
        echo "  Re-run without --gpu to install the CPU build for this platform." >&2
        exit 1
    fi
fi

compose_archive

echo "hyponoia installer"
echo "  os:      $OS"
echo "  arch:    $ARCH"
echo "  variant: $VARIANT_LABEL"
echo "  archive: $ARCHIVE"
echo "  target:  $INSTALL_DIR/hyponoia"
if [ "$GPU_INHERITED" = true ] && [ "$GPU" = true ]; then
    echo "  (GPU reused from the install already in $INSTALL_DIR; --cpu overrides)"
fi
echo ""

# Download. The URL is composed after the manifest check below, because a GPU
# selection can still be given up there and a stale URL would outlive it.
DLDIR=$(mktemp -d)
trap 'rm -rf "$DLDIR"' EXIT

# Checksum verification is mandatory. Activation must never stop running HYP
# sessions for a candidate whose published digest was not positively verified.
#
# The manifest is fetched BEFORE the archive because it is also the only thing
# that can answer "does this release publish what you asked for?". --gpu has to
# refuse by name, and it cannot do that from a 404 forty megabytes into a
# transfer.
CHECKSUM_URL="${HYP_DOWNLOAD_URL}/checksums.txt"
download_file "$CHECKSUM_URL" "$DLDIR/checksums.txt" false || {
    echo "error: could not download checksums.txt" >&2
    exit 1
}
CHECKSUM_BYTES=$(wc -c < "$DLDIR/checksums.txt" | tr -d '[:space:]')
case "$CHECKSUM_BYTES" in
    ''|*[!0-9]*)
        echo "error: could not determine checksums.txt size" >&2
        exit 1
        ;;
esac
if [ "$CHECKSUM_BYTES" -gt 1048576 ]; then
    echo "error: checksums.txt exceeds the 1 MiB safety limit" >&2
    exit 1
fi

checksums_list_archive() {
    awk -v archive="$1" \
        '$2 == archive || $2 == "*" archive { found = 1 } END { exit found ? 0 : 1 }' \
        "$DLDIR/checksums.txt"
}

# A release that publishes no GPU archive is a real state -- the GPU leg is one
# optional matrix entry, and it can be dropped or fail without failing the
# release. Name that, and never resolve it by handing back the CPU archive
# under the flag that asked for the GPU one.
if [ "$GPU" = true ] && ! checksums_list_archive "$ARCHIVE"; then
    if [ "$GPU_INHERITED" = true ]; then
        echo "note: $INSTALL_DIR records a GPU install, but this release publishes"
        echo "  no $ARCHIVE."
        gpu_scope_note
        echo "  Installing the CPU build instead; re-run with --gpu once a release"
        echo "  publishes the GPU archive again."
        echo ""
        GPU=false
        compose_archive
    else
        echo "error: this release publishes no GPU archive." >&2
        echo "  checksums.txt lists no $ARCHIVE, so there is nothing to" >&2
        echo "  install, and --gpu will not fall back to the CPU archive." >&2
        gpu_scope_note >&2
        echo "  Re-run without --gpu (or with --cpu) to install the CPU build." >&2
        exit 1
    fi
fi
URL="${HYP_DOWNLOAD_URL}/${ARCHIVE}"

# Warn, never refuse: the loader is a package away, and a user who installs it
# after this run keeps the same binary. Refusing here would also refuse on every
# machine whose loader lives outside the paths this can cheaply look in.
gpu_vulkan_loader_present() {
    if command -v ldconfig &>/dev/null &&
        ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
        return 0
    fi
    for candidate in /usr/lib/x86_64-linux-gnu/libvulkan.so.1 /usr/lib64/libvulkan.so.1 \
        /usr/lib/libvulkan.so.1 /lib/x86_64-linux-gnu/libvulkan.so.1; do
        if [ -e "$candidate" ]; then
            return 0
        fi
    done
    return 1
}
if [ "$GPU" = true ] && ! gpu_vulkan_loader_present; then
    echo "warning: no Vulkan loader (libvulkan.so.1) found on this system." >&2
    echo "  The GPU build links it directly, so it cannot start without one, and" >&2
    echo "  this installer verifies the candidate by running it. Install the" >&2
    echo "  loader (Debian/Ubuntu: libvulkan1, Fedora: vulkan-loader, Arch:" >&2
    echo "  vulkan-icd-loader) if the verification below fails." >&2
    echo "  Continuing — a loader outside the usual paths still satisfies it." >&2
    echo "" >&2
fi

echo "Downloading ${ARCHIVE}..."
download_file "$URL" "$DLDIR/$ARCHIVE" true

awk -v archive="$ARCHIVE" \
    '$2 == archive || $2 == "*" archive { print $1 }' \
    "$DLDIR/checksums.txt" > "$DLDIR/matching-checksums.txt"
EXPECTED=""
while IFS= read -r digest; do
    case "$digest" in
        ''|*[!0-9A-Fa-f]*)
            echo "error: invalid SHA-256 digest for $ARCHIVE" >&2
            exit 1
            ;;
    esac
    if [ "${#digest}" -ne 64 ]; then
        echo "error: invalid SHA-256 digest length for $ARCHIVE" >&2
        exit 1
    fi
    digest=$(printf '%s' "$digest" | tr 'A-F' 'a-f')
    if [ -n "$EXPECTED" ] && [ "$EXPECTED" != "$digest" ]; then
        echo "error: conflicting SHA-256 digests for $ARCHIVE" >&2
        exit 1
    fi
    EXPECTED="$digest"
done < "$DLDIR/matching-checksums.txt"
if [ -z "$EXPECTED" ]; then
    echo "error: no SHA-256 digest for $ARCHIVE in checksums.txt" >&2
    exit 1
fi
if command -v sha256sum &>/dev/null; then
    ACTUAL=$(sha256sum "$DLDIR/$ARCHIVE" | awk '{print $1}')
elif command -v shasum &>/dev/null; then
    ACTUAL=$(shasum -a 256 "$DLDIR/$ARCHIVE" | awk '{print $1}')
else
    echo "error: sha256sum or shasum is required to verify the download" >&2
    exit 1
fi
ACTUAL=$(printf '%s' "$ACTUAL" | tr 'A-F' 'a-f')
if [ "$EXPECTED" != "$ACTUAL" ]; then
    echo "error: CHECKSUM MISMATCH — download may be corrupted!" >&2
    echo "  expected: $EXPECTED" >&2
    echo "  actual:   $ACTUAL" >&2
    exit 1
fi
echo "Checksum verified."

# Validate the complete archive namespace before extraction. Standard releases
# are the canonical five files; UI releases add exactly one root-level,
# content-addressed pack. Anything else is a release-integrity failure, not a
# sidecar to ignore.
if [ "$OS" = "windows" ]; then
    ARCHIVE_BINARY="hyponoia.exe"
    ARCHIVE_INSTALLER="install.ps1"
else
    ARCHIVE_BINARY="hyponoia"
    ARCHIVE_INSTALLER="install.sh"
fi
ARCHIVE_MEMBERS_FILE="$DLDIR/archive-members.txt"
if [ "$EXT" = "zip" ]; then
    if ! unzip -Z1 "$DLDIR/$ARCHIVE" > "$ARCHIVE_MEMBERS_FILE"; then
        echo "error: could not enumerate release archive" >&2
        exit 1
    fi
else
    if ! tar -tzf "$DLDIR/$ARCHIVE" > "$ARCHIVE_MEMBERS_FILE"; then
        echo "error: could not enumerate release archive" >&2
        exit 1
    fi
fi

BINARY_MEMBERS=0
INTEGRATION_MEMBERS=0
LICENSE_MEMBERS=0
INSTALLER_MEMBERS=0
NOTICE_MEMBERS=0
UI_PACK_MEMBERS=0
ARCHIVE_MEMBER_COUNT=0
UI_PACK_NAME=""
while IFS= read -r member || [ -n "$member" ]; do
    ARCHIVE_MEMBER_COUNT=$((ARCHIVE_MEMBER_COUNT + 1))
    case "$member" in
        "$ARCHIVE_BINARY") BINARY_MEMBERS=$((BINARY_MEMBERS + 1)) ;;
        hyp-integrations.json) INTEGRATION_MEMBERS=$((INTEGRATION_MEMBERS + 1)) ;;
        LICENSE) LICENSE_MEMBERS=$((LICENSE_MEMBERS + 1)) ;;
        "$ARCHIVE_INSTALLER") INSTALLER_MEMBERS=$((INSTALLER_MEMBERS + 1)) ;;
        THIRD_PARTY_NOTICES.md) NOTICE_MEMBERS=$((NOTICE_MEMBERS + 1)) ;;
        *)
            if [ "$VARIANT" = "ui" ] &&
                [[ "$member" =~ ^hyp-ui-[0-9a-f]{64}\.pack$ ]]; then
                UI_PACK_MEMBERS=$((UI_PACK_MEMBERS + 1))
                UI_PACK_NAME="$member"
            else
                echo "error: release archive contains unexpected member: $member" >&2
                exit 1
            fi
            ;;
    esac
done < "$ARCHIVE_MEMBERS_FILE"

EXPECTED_MEMBER_COUNT=5
if [ "$VARIANT" = "ui" ]; then
    EXPECTED_MEMBER_COUNT=6
fi
if [ "$BINARY_MEMBERS" -ne 1 ] || [ "$INTEGRATION_MEMBERS" -ne 1 ] ||
    [ "$LICENSE_MEMBERS" -ne 1 ] || [ "$INSTALLER_MEMBERS" -ne 1 ] ||
    [ "$NOTICE_MEMBERS" -ne 1 ] || [ "$UI_PACK_MEMBERS" -ne $((EXPECTED_MEMBER_COUNT - 5)) ] ||
    [ "$ARCHIVE_MEMBER_COUNT" -ne "$EXPECTED_MEMBER_COUNT" ]; then
    echo "error: release archive does not match the exact $VARIANT member set" >&2
    exit 1
fi

# Extract
echo "Extracting..."
if [ "$EXT" = "zip" ]; then
    unzip -q "$DLDIR/$ARCHIVE" -d "$DLDIR"
else
    tar -xzf "$DLDIR/$ARCHIVE" -C "$DLDIR"
fi

for extracted_member in "$ARCHIVE_BINARY" hyp-integrations.json LICENSE \
    "$ARCHIVE_INSTALLER" THIRD_PARTY_NOTICES.md; do
    if [ ! -f "$DLDIR/$extracted_member" ] || [ -L "$DLDIR/$extracted_member" ]; then
        echo "error: release member is not a regular file: $extracted_member" >&2
        exit 1
    fi
done

DLBIN="$DLDIR/$ARCHIVE_BINARY"
if [ ! -f "$DLBIN" ] || [ -L "$DLBIN" ]; then
    echo "error: binary not found after extraction" >&2
    exit 1
fi

DL_UI_PACK=""
if [ "$VARIANT" = "ui" ]; then
    DL_UI_PACK="$DLDIR/$UI_PACK_NAME"
    if [ ! -f "$DL_UI_PACK" ] || [ -L "$DL_UI_PACK" ]; then
        echo "error: UI asset pack not found after extraction" >&2
        exit 1
    fi
    PACK_EXPECTED="${UI_PACK_NAME#hyp-ui-}"
    PACK_EXPECTED="${PACK_EXPECTED%.pack}"
    if command -v sha256sum &>/dev/null; then
        PACK_ACTUAL=$(sha256sum "$DL_UI_PACK" | awk '{print $1}')
    else
        PACK_ACTUAL=$(shasum -a 256 "$DL_UI_PACK" | awk '{print $1}')
    fi
    PACK_ACTUAL=$(printf '%s' "$PACK_ACTUAL" | tr 'A-F' 'a-f')
    if [ "$PACK_EXPECTED" != "$PACK_ACTUAL" ]; then
        echo "error: UI asset pack digest does not match its filename" >&2
        exit 1
    fi
fi

# macOS: fix signing
# RETIRED-PLATFORM(macos): unreachable — detect_os exits on Darwin. Kept, not
# deleted, so restoring darwin is one edit. docs/MAINTAINERS.md "Retired platforms".
if [ "$OS" = "darwin" ]; then
    echo "Fixing macOS code signing..."
    xattr -d com.apple.quarantine "$DLBIN" 2>/dev/null || true
    codesign --sign - --force "$DLBIN" 2>/dev/null || true
fi

# Verify the candidate before it requests account-wide maintenance. The
# candidate itself owns process draining and the transactional target swap.
chmod 755 "$DLBIN"
if ! CANDIDATE_VERSION=$("$DLBIN" --version 2>&1); then
    echo "error: downloaded binary failed to run" >&2
    if [ "$GPU" = true ]; then
        echo "  This is the GPU build. It links libvulkan.so.1 directly and cannot" >&2
        echo "  start without a Vulkan loader; install one (Debian/Ubuntu:" >&2
        echo "  libvulkan1, Fedora: vulkan-loader, Arch: vulkan-icd-loader) and" >&2
        echo "  re-run with --gpu, or re-run with --cpu for the portable CPU build." >&2
    fi
    exit 1
fi
echo "Verified candidate: $CANDIDATE_VERSION"

# The candidate publishes the runtime set under one native activation guard,
# with sidecars before the executable and retained per-file backups for
# cooperative rollback. The set is intentionally recoverable/fail-closed, not
# claimed to be a crash-atomic multi-file filesystem transaction.
new_exclusive_sibling_temp() {
    local destination="$1"
    local directory basename
    directory="$(dirname "$destination")"
    basename="$(basename "$destination")"
    mktemp "$directory/.${basename}.tmp.XXXXXX"
}

DEST="$INSTALL_DIR/hyponoia"
[ "$OS" = "windows" ] && DEST="$INSTALL_DIR/hyponoia.exe"
INSTALL_ARGS=(-y --force "--dir=$INSTALL_DIR")
if [ "$SKIP_CONFIG" = true ]; then
    INSTALL_ARGS+=(--skip-config)
fi
"$DLBIN" install "${INSTALL_ARGS[@]}"

# Place the installer beside the binary so `update` can point at a local file
# instead of a URL, and so the next update uses THIS release's installer.
#
# Two things make this safe. The source is the copy from the archive we just
# checksum-verified -- never "$0", which does not exist under `curl | bash` and
# would pin us to the OLD installer forever. And it is published by atomic
# rename, never by writing over the live path: bash reads a script incrementally
# by byte offset, so overwriting the file it is executing continues reading the
# NEW bytes at the OLD offset. That fails silently and bizarrely, which is worse
# than failing loudly.
#
# Best effort by design: a user who cannot write to INSTALL_DIR still gets a
# working install, and `update` falls back to explaining where to find it.
DL_INSTALLER="$DLDIR/install.sh"
if [ -f "$DL_INSTALLER" ]; then
    INSTALLER_TMP=""
    if INSTALLER_TMP="$(new_exclusive_sibling_temp "$INSTALL_DIR/install.sh" 2>/dev/null)" &&
        cp "$DL_INSTALLER" "$INSTALLER_TMP" 2>/dev/null &&
        chmod 755 "$INSTALLER_TMP" 2>/dev/null &&
        mv -f "$INSTALLER_TMP" "$INSTALL_DIR/install.sh" 2>/dev/null; then
        echo "Installed updater -> $INSTALL_DIR/install.sh"
    else
        # A non-empty path was exclusively created by mktemp above. If
        # reservation failed, leave every pre-existing sibling untouched.
        [ -z "$INSTALLER_TMP" ] || rm -f "$INSTALLER_TMP" 2>/dev/null || true
        echo "note: could not place install.sh in $INSTALL_DIR (update will explain where to find it)"
    fi
fi

# Record the flavour beside the binary, by the same atomic-rename rule as the
# updater above, so the next flagless run of this script reinstalls what is
# actually here. GPU writes the marker and CPU removes it: presence is the whole
# encoding, so there is no state to parse and no way for a stale marker to
# survive a deliberate --cpu install. Best effort, like the updater: a user who
# cannot write to INSTALL_DIR still gets a working install and is told that the
# next update needs --gpu again.
if [ "$GPU" = true ]; then
    MARKER_TMP=""
    if MARKER_TMP="$(new_exclusive_sibling_temp "$VARIANT_MARKER" 2>/dev/null)" &&
        printf 'gpu\n' > "$MARKER_TMP" 2>/dev/null &&
        chmod 644 "$MARKER_TMP" 2>/dev/null &&
        mv -f "$MARKER_TMP" "$VARIANT_MARKER" 2>/dev/null; then
        echo "Recorded GPU variant -> $VARIANT_MARKER"
    else
        [ -z "$MARKER_TMP" ] || rm -f "$MARKER_TMP" 2>/dev/null || true
        echo "note: could not record the GPU variant in $INSTALL_DIR;" \
            "pass --gpu again on the next update."
    fi
elif [ -f "$VARIANT_MARKER" ] && [ ! -L "$VARIANT_MARKER" ]; then
    rm -f "$VARIANT_MARKER" 2>/dev/null ||
        echo "note: could not remove the stale GPU marker $VARIANT_MARKER"
fi

# Verify
VERSION=$("$DEST" --version 2>&1) || {
    echo "error: installed binary failed to run" >&2
    # RETIRED-PLATFORM(macos): unreachable — detect_os exits on Darwin.
    if [ "$OS" = "darwin" ]; then
        echo "  try: xattr -cr $DEST && codesign --force --sign - $DEST" >&2
    fi
    exit 1
}
echo "Installed: $VERSION"
# Which variant landed, on every path — not only the GPU one. The banner above
# states an intention; this states an outcome, and the two differ whenever a
# GPU selection had to be given up after the manifest arrived.
echo "  variant: $VARIANT_LABEL (from $ARCHIVE)"
if [ "$GPU" = true ]; then
    echo "  The GPU runs the 'hyponoia embed' pass only; 'ask' encodes the"
    echo "  question on the CPU. Needs libvulkan.so.1 at runtime."
fi

# Agent configuration is part of the candidate-owned activation window.
if [ "$SKIP_CONFIG" = true ]; then
    echo ""
    echo "Skipping agent configuration (--skip-config)"
fi

# PATH check
if ! echo "$PATH" | tr ':' '\n' | grep -qx "$INSTALL_DIR"; then
    echo ""
    echo "NOTE: $INSTALL_DIR is not in your PATH."
    echo "Add it to your shell config:"
    echo ""
    echo "  echo 'export PATH=\"$INSTALL_DIR:\$PATH\"' >> ~/.zshrc"
fi

echo ""
echo "Done! Restart your coding agent to start using hyponoia."

} # end main()

main "$@"
