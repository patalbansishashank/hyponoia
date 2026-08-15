#!/usr/bin/env bash
set -euo pipefail

# hyponoia setup script (Linux: binary or source; macOS: source only)
# Default: download pre-built binary from GitHub Release
# --from-source: build from source (requires Go + C compiler)
#
# RETIRED-PLATFORM(macos): no darwin binary is published, so the default
# download path refuses on macOS; --from-source is unaffected and still
# supported there. See docs/MAINTAINERS.md "Retired platforms".

REPO="patalbansishashank/hyponoia"
INSTALL_DIR="$HOME/.local/bin"
BINARY_NAME="hyponoia"
SOURCE_DIR="$HOME/.local/share/hyponoia"
CLEANUP_DIR=""  # set by download_binary for EXIT trap

# --- Colors ---

if [ -t 1 ] && command -v tput &>/dev/null; then
    GREEN=$(tput setaf 2)
    RED=$(tput setaf 1)
    YELLOW=$(tput setaf 3)
    BOLD=$(tput bold)
    RESET=$(tput sgr0)
else
    GREEN=""
    RED=""
    YELLOW=""
    BOLD=""
    RESET=""
fi

ok()   { echo "${GREEN}✓${RESET} $*"; }
fail() { echo "${RED}✗${RESET} $*"; }
warn() { echo "${YELLOW}⚠${RESET} $*"; }
info() { echo "  $*"; }

die() { fail "$@"; exit 1; }

# --- Argument parsing ---

FROM_SOURCE=false
for arg in "$@"; do
    case "$arg" in
        --from-source) FROM_SOURCE=true ;;
        --help|-h)
            echo "Usage: $0 [--from-source]"
            echo ""
            echo "  Default:        Download pre-built binary from GitHub Release"
            echo "  --from-source:  Clone and build from source (requires Go 1.23+ and a C compiler)"
            exit 0
            ;;
        *) die "Unknown argument: $arg" ;;
    esac
done

# --- Platform detection ---

detect_platform() {
    local os arch
    os=$(uname -s)
    arch=$(uname -m)

    case "$os" in
        # RETIRED-PLATFORM(macos): unreachable — the caller refuses on Darwin
        # before this runs, because die() prints on stdout and would be
        # swallowed by this function's command substitution.
        Darwin) os="darwin" ;;
        Linux)  os="linux" ;;
        *)      die "Unsupported OS: $os. Use WSL2 on Windows." ;;
    esac

    case "$arch" in
        arm64|aarch64) arch="arm64" ;;
        x86_64|amd64)
            # On macOS, uname -m returns x86_64 under Rosetta even on Apple Silicon.
            # Check the actual hardware to pick the right binary.
            # RETIRED-PLATFORM(macos): unreachable with the Darwin case above.
            if [ "$os" = "darwin" ] && sysctl -n hw.optional.arm64 2>/dev/null | grep -q '1'; then
                arch="arm64"
            else
                arch="amd64"
            fi
            ;;
        *)             die "Unsupported architecture: $arch" ;;
    esac

    echo "${os}-${arch}"
}

# --- Prerequisite checks ---

check_download_tool() {
    if command -v curl &>/dev/null; then
        echo "curl"
    elif command -v wget &>/dev/null; then
        echo "wget"
    else
        die "Neither curl nor wget found. Install one and retry."
    fi
}

check_go_version() {
    if ! command -v go &>/dev/null; then
        die "Go not found. Install Go 1.23+ from https://go.dev/dl/"
    fi

    local version
    version=$(go version | grep -oE 'go[0-9]+\.[0-9]+' | head -1)
    local major minor
    major=$(echo "$version" | grep -oE '[0-9]+' | head -1)
    minor=$(echo "$version" | grep -oE '[0-9]+' | sed -n '2p')

    if [ "$major" -lt 1 ] || { [ "$major" -eq 1 ] && [ "$minor" -lt 23 ]; }; then
        die "Go $major.$minor found, but 1.23+ is required. Update from https://go.dev/dl/"
    fi

    ok "Go $major.$minor"
}

check_c_compiler() {
    if command -v cc &>/dev/null || command -v gcc &>/dev/null || command -v clang &>/dev/null; then
        ok "C compiler found"
        return
    fi

    local os
    os=$(uname -s)
    if [ "$os" = "Darwin" ]; then
        die "No C compiler found. Run: xcode-select --install"
    else
        die "No C compiler found. Install build-essential (Debian/Ubuntu) or gcc (Fedora/RHEL)"
    fi
}

check_git() {
    if ! command -v git &>/dev/null; then
        die "Git not found. Install git and retry."
    fi
    ok "Git found"
}

# --- Download binary ---

fetch() {
    local url="$1" tool="$2"
    if [ "$tool" = "curl" ]; then
        curl -fsSL "$url"
    else
        wget -qO- "$url"
    fi
}

install_integration_asset() {
    local source_dir="$1"
    local source_path="${source_dir}/hyp-integrations.json"

    if [ ! -f "$source_path" ]; then
        die "Runtime asset missing from build or release: hyp-integrations.json"
    fi

    cp "$source_path" "${INSTALL_DIR}/hyp-integrations.json"
    chmod 644 "${INSTALL_DIR}/hyp-integrations.json"
}

# Graph UI asset pack. Without it the UI has no assets to serve, so the
# archive's sixth member cannot be dropped on the floor. The name is the sha256
# of the pack's own bytes, so it can only ever be matched by glob.
#
# Beside the binary, not ../share/hyponoia: this installer already places
# hyp-integrations.json beside the binary, that is the first location
# src/ui/asset_pack.c looks in and the only one src/cli/cli.c accepts, and
# ${INSTALL_DIR}/../share/hyponoia is $HOME/.local/share/hyponoia — which this
# script uses as the source checkout for --from-source.
#
# Guarded like THIRD_PARTY_NOTICES.md elsewhere in packaging: a release archive
# that predates the pack must still install, with the UI degraded rather than
# the install failed.
install_ui_pack_asset() {
    local source_dir="$1"
    local pack name found=false

    for pack in "${source_dir}"/hyp-ui-*.pack; do
        [ -f "$pack" ] || continue
        name=$(basename "$pack")
        cp "$pack" "${INSTALL_DIR}/${name}"
        chmod 644 "${INSTALL_DIR}/${name}"
        found=true
    done

    if [ "$found" = false ]; then
        warn "No UI asset pack found — the graph UI will be unavailable."
    fi
}

download_binary() {
    local platform="$1" tool="$2"

    echo ""
    echo "${BOLD}Fetching latest release...${RESET}"
    local tag
    tag=$(fetch "https://api.github.com/repos/${REPO}/releases/latest" "$tool" | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"//;s/".*//')

    if [ -z "$tag" ]; then
        die "Could not determine latest release. Check https://github.com/${REPO}/releases"
    fi
    ok "Latest release: $tag"

    # The build publishes only the UI variant, so this is the only archive that exists.
    local asset="hyponoia-ui-${platform}.tar.gz"
    local url="https://github.com/${REPO}/releases/download/${tag}/${asset}"

    echo "${BOLD}Downloading ${asset}...${RESET}"
    CLEANUP_DIR=$(mktemp -d)
    trap 'rm -rf "$CLEANUP_DIR"' EXIT
    local tmpdir="$CLEANUP_DIR"

    fetch "$url" "$tool" > "${tmpdir}/${asset}"
    tar -xzf "${tmpdir}/${asset}" -C "$tmpdir"

    mkdir -p "$INSTALL_DIR"
    mv "${tmpdir}/${BINARY_NAME}" "${INSTALL_DIR}/${BINARY_NAME}"
    chmod +x "${INSTALL_DIR}/${BINARY_NAME}"
    install_integration_asset "$tmpdir"
    install_ui_pack_asset "$tmpdir"

    ok "Installed to ${INSTALL_DIR}/${BINARY_NAME}"
}

# --- Build from source ---

build_from_source() {
    echo ""
    echo "${BOLD}Checking prerequisites...${RESET}"
    check_go_version
    check_c_compiler
    check_git

    echo ""
    if [ -d "$SOURCE_DIR/.git" ]; then
        echo "${BOLD}Updating source...${RESET}"
        git -C "$SOURCE_DIR" pull --ff-only
    else
        echo "${BOLD}Cloning repository...${RESET}"
        mkdir -p "$(dirname "$SOURCE_DIR")"
        git clone "https://github.com/${REPO}.git" "$SOURCE_DIR"
    fi
    ok "Source at ${SOURCE_DIR}"

    echo ""
    echo "${BOLD}Building binary (this may take a minute)...${RESET}"
    mkdir -p "$INSTALL_DIR"

    (cd "$SOURCE_DIR" && scripts/build.sh)
    cp "${SOURCE_DIR}/build/c/hyponoia" "${INSTALL_DIR}/${BINARY_NAME}"
    chmod +x "${INSTALL_DIR}/${BINARY_NAME}"
    install_integration_asset "${SOURCE_DIR}/build/c"
    install_ui_pack_asset "${SOURCE_DIR}/build/c"

    ok "Built and installed to ${INSTALL_DIR}/${BINARY_NAME}"
}

# --- MCP auto-configuration ---

configure_claude() {
    echo ""
    local binary_path="${INSTALL_DIR}/${BINARY_NAME}"
    local claude_config_dir="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"
    local settings_file="${claude_config_dir}/settings.json"

    printf "%s" "${BOLD}Configure Claude Code to use hyponoia? [y/N] ${RESET}"
    read -r answer
    if [[ ! "$answer" =~ ^[Yy]$ ]]; then
        echo ""
        info "Add this to your .mcp.json or ${claude_config_dir}/settings.json:"
        echo ""
        echo '  {'
        echo '    "mcpServers": {'
        echo '      "hyponoia": {'
        echo '        "type": "stdio",'
        echo "        \"command\": \"${binary_path}\""
        echo '      }'
        echo '    }'
        echo '  }'
        return
    fi

    local mcp_entry
    mcp_entry=$(cat <<JSONEOF
{"type":"stdio","command":"${binary_path}"}
JSONEOF
)

    mkdir -p "$(dirname "$settings_file")"

    if command -v jq &>/dev/null; then
        # Use jq to merge
        if [ -f "$settings_file" ]; then
            local tmp
            tmp=$(mktemp)
            jq --argjson entry "$mcp_entry" '.mcpServers["hyponoia"] = $entry' "$settings_file" > "$tmp"
            mv "$tmp" "$settings_file"
        else
            echo "{}" | jq --argjson entry "$mcp_entry" '.mcpServers["hyponoia"] = $entry' > "$settings_file"
        fi
        ok "Updated ${settings_file}"
    elif command -v python3 &>/dev/null; then
        python3 -c "
import json, os
path = os.path.expanduser('$settings_file')
data = {}
if os.path.exists(path):
    with open(path) as f:
        data = json.load(f)
data.setdefault('mcpServers', {})['hyponoia'] = json.loads('$mcp_entry')
with open(path, 'w') as f:
    json.dump(data, f, indent=2)
print()
"
        ok "Updated ${settings_file}"
    else
        warn "Neither jq nor python3 found — cannot auto-configure."
        echo ""
        info "Add this to ${settings_file} manually:"
        echo ""
        echo '  "mcpServers": {'
        echo '    "hyponoia": {'
        echo '      "type": "stdio",'
        echo "      \"command\": \"${binary_path}\""
        echo '    }'
        echo '  }'
    fi
}

# --- PATH check ---

check_path() {
    if [[ ":$PATH:" != *":${INSTALL_DIR}:"* ]]; then
        echo ""
        warn "${INSTALL_DIR} is not on your PATH."
        info "Add this to your shell profile (~/.bashrc, ~/.zshrc, etc.):"
        echo ""
        echo "  export PATH=\"${INSTALL_DIR}:\$PATH\""
    fi
}

# --- Main ---

echo ""
echo "${BOLD}hyponoia installer${RESET}"
echo ""

if [ "$FROM_SOURCE" = true ]; then
    build_from_source
else
    # RETIRED-PLATFORM(macos): refuse before composing a darwin asset name that
    # 404s. Deliberately here rather than in detect_platform(), whose output is
    # captured by command substitution — a message printed there is invisible.
    # See docs/MAINTAINERS.md "Retired platforms".
    if [ "$(uname -s)" = "Darwin" ]; then
        fail "No macOS binaries are published for this release."
        info "Building from source on macOS still works — re-run with --from-source:"
        info "  curl -fsSL https://raw.githubusercontent.com/${REPO}/main/scripts/setup.sh | bash -s -- --from-source"
        info "See docs/INSTALL.md: https://github.com/${REPO}/blob/main/docs/INSTALL.md"
        exit 1
    fi

    platform=$(detect_platform)
    ok "Platform: ${platform}"
    tool=$(check_download_tool)
    ok "Download tool: ${tool}"
    download_binary "$platform" "$tool"
fi

# Verify binary
if [ ! -x "${INSTALL_DIR}/${BINARY_NAME}" ]; then
    die "Binary at ${INSTALL_DIR}/${BINARY_NAME} is not executable"
fi

ver_output=$("${INSTALL_DIR}/${BINARY_NAME}" --version 2>&1) || true
if [ -n "$ver_output" ]; then
    ok "$ver_output"
else
    ok "Binary is executable"
fi

configure_claude
check_path

# --- Git hooks ---
# If run from inside the repo, activate tracked hooks
if [ -d "scripts/hooks" ] && git rev-parse --git-dir &>/dev/null; then
    git config core.hooksPath scripts/hooks
    ok "Git hooks activated (scripts/hooks/)"
fi

echo ""
ok "Done! Restart Claude Code and verify with /mcp"
echo ""
info "To uninstall:"
info "  rm ${INSTALL_DIR}/${BINARY_NAME}"
info "  rm ${INSTALL_DIR}/hyp-integrations.json"
info "  rm ${INSTALL_DIR}/hyp-ui-*.pack  # graph UI assets"
info "  rm -rf ${SOURCE_DIR}  # if built from source"
info "  rm -rf ~/.cache/hyponoia/  # graph database"
