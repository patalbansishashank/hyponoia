# Installing Hyponoia

Getting the binary onto your machine and your agent talking to it.

[← README](../README.md)

> **Where the project actually is.** There are no published binaries yet, and
> the repository is private — so the `curl … | bash` one-liners, the package
> managers and the release archives described below **do not work today**. They
> are the shape of the distribution once releases are cut and the repository is
> public. Until then, **[build from source](#build-from-source)** is the path
> that works, and it is two commands.

## Build from source

```bash
git clone https://github.com/patalbansishashank/hyponoia.git
cd hyponoia
scripts/build.sh                    # binary at build/c/hyponoia
```

Needs a C compiler, a C++ compiler and zlib. On Debian/Ubuntu:
`apt install build-essential zlib1g-dev`. macOS: `xcode-select --install`.

The `ask` lane's encoder is compiled in by default on the portable CPU path.
For the GPU path, `make -f Makefile.hyp hyp HYP_ASK_GPU=vulkan`, which
additionally needs Vulkan-Headers, SPIRV-Headers and `glslc`; point
`VULKAN_INCLUDE` and `SPIRV_INCLUDE` at the first two. It only speeds up
`hyponoia embed`.

Point your agent at the binary — see [Manual MCP Configuration](#manual-mcp-configuration).

Run the tests:

```bash
scripts/test.sh                     # full: sanitizer build + all suites + guards
scripts/test.sh --suites ask        # one suite, seconds
build/c/test-runner --list-suites   # what is available
```

---

*Everything below describes the distribution as designed. It becomes live when
releases are published.*

## Pre-built Binaries

| Platform | Standard | With Graph UI |
|----------|----------|---------------|
| macOS (Apple Silicon) | `hyponoia-darwin-arm64.tar.gz` | `hyponoia-ui-darwin-arm64.tar.gz` |
| macOS (Intel) | `hyponoia-darwin-amd64.tar.gz` | `hyponoia-ui-darwin-amd64.tar.gz` |
| Linux (x86_64) | `hyponoia-linux-amd64.tar.gz` | `hyponoia-ui-linux-amd64.tar.gz` |
| Linux (ARM64) | `hyponoia-linux-arm64.tar.gz` | `hyponoia-ui-linux-arm64.tar.gz` |
| Windows (x86_64) | `hyponoia-windows-amd64.zip` | `hyponoia-ui-windows-amd64.zip` |

Every release includes `checksums.txt` with SHA-256 hashes. Keep the native executable together with the authenticated `hyp-integrations.json` asset and, for the UI variant, its single content-addressed `hyp-ui-<sha256>.pack`. Linux `-portable` archives contain the fully static builds; ordinary platform archives use their native system ABI.

> **Windows note**: SmartScreen may show a warning for unsigned software. Click **"More info"** → **"Run anyway"**. Verify integrity with `checksums.txt`.

## Setup Scripts

<details>
<summary>Automated download + install</summary>

**macOS / Linux:**

```bash
curl -fsSL https://raw.githubusercontent.com/patalbansishashank/hyponoia/main/scripts/setup.sh | bash
```

**Windows (PowerShell):**

```powershell
irm https://raw.githubusercontent.com/patalbansishashank/hyponoia/main/scripts/setup-windows.ps1 | iex
```

</details>

## AUR (Arch Linux)

```bash
yay -S hyponoia-bin
```

```bash
paru -S hyponoia-bin
```

The `hyponoia-bin` package is available at: https://aur.archlinux.org/packages/hyponoia-bin

## Install via Claude Code

```
You: "Install this MCP server: https://github.com/patalbansishashank/hyponoia"
```

## Build from Source

<details>
<summary>Prerequisites: C compiler + zlib</summary>

| Requirement | Check | Install |
|-------------|-------|---------|
| **C compiler** (gcc or clang) | `gcc --version` or `clang --version` | macOS: `xcode-select --install`, Linux: `apt install build-essential` |
| **C++ compiler** | `g++ --version` or `clang++ --version` | Same as above |
| **zlib** | — | macOS: included, Linux: `apt install zlib1g-dev` |
| **Git** | `git --version` | Pre-installed on most systems |

</details>

```bash
git clone https://github.com/patalbansishashank/hyponoia.git
cd hyponoia
scripts/build.sh                    # standard binary
scripts/build.sh --with-ui          # with graph visualization
## Binary at: build/c/hyponoia   (hyponoia.exe on Windows)
```

Every platform builds a **verified runtime set**: the native executable, `hyp-integrations.json`, and—when `--with-ui` is selected—exactly one content-addressed `hyp-ui-<sha256>.pack`. These files are authenticated and published together; do not separate the executable from its sidecars.

Run the test suite (6,768 tests across 120 suites):

```bash
scripts/test.sh                     # full: clean sanitizer build + all suites + guards
scripts/test.sh --suites <name>     # one suite, incremental, seconds
build/c/test-runner --list-suites   # what is available
```

`scripts/test.sh` is the same entry the CI gates run, so a local pass means the same thing a CI pass does. Packaging a release archive locally uses the same canonical script the release pipeline calls:

```bash
scripts/package-release.sh <linux|darwin|windows> <amd64|arm64>
```

## Manual MCP Configuration

<details>
<summary>If you prefer not to use the install command</summary>

Add to `~/.claude.json` (user scope) or project `.mcp.json`:

```json
{
  "mcpServers": {
    "hyponoia": {
      "command": "/path/to/hyponoia",
      "args": []
    }
  }
}
```

Restart your agent. Verify with `/mcp` — you should see `hyponoia` with 16 tools.

</details>
