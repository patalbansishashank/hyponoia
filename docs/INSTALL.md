# Installing Hyponoia

Getting the binary onto your machine and your agent talking to it.

[← README](../README.md)

> **Where the project actually is.** The repository is public. There are no
> published binaries yet — so the `curl … | bash` one-liners, the package
> managers and the release archives described below **do not work today**. They
> are the shape of the distribution once releases are cut. Until then,
> **[build from source](#build-from-source)** is the path that works, and it is
> two commands.

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

<!-- RETIRED-PLATFORM(macos): the darwin-arm64 and darwin-amd64 rows were removed
     here. See docs/MAINTAINERS.md "Retired platforms". -->

One archive per platform, and it includes the graph UI:

| Platform | Archive |
|----------|---------|
| Linux (x86_64) | `hyponoia-ui-linux-amd64.tar.gz` |
| Linux (x86_64, static) | `hyponoia-ui-linux-amd64-portable.tar.gz` |
| Linux (ARM64) | `hyponoia-ui-linux-arm64.tar.gz` |
| Linux (ARM64, static) | `hyponoia-ui-linux-arm64-portable.tar.gz` |
| Windows (x86_64) | `hyponoia-ui-windows-amd64.zip` |

There is no separate "standard" archive. The graph UI's asset pack is about
1.3 MB of a ~41 MB download, so a second variant without it would save 3% and
double the build; the `--standard` flag is kept only to say that, rather than
silently handing you the UI archive under another name.

There is no macOS download and no ARM64 Windows download. On macOS,
[build from source](#build-from-source) — that path is fully supported. On
ARM64 Windows, use the amd64 archive under emulation.

Every release includes `checksums.txt` with SHA-256 hashes. Keep the native executable together with the authenticated `hyp-integrations.json` asset and its single content-addressed `hyp-ui-<sha256>.pack`. Linux `-portable` archives contain the fully static builds; ordinary platform archives use their native system ABI.

> **Windows note**: SmartScreen may show a warning for unsigned software. Click **"More info"** → **"Run anyway"**. Verify integrity with `checksums.txt`.

## Setup Scripts

<details>
<summary>Automated download + install</summary>

<!-- RETIRED-PLATFORM(macos): this heading used to read "macOS / Linux". No macOS
     binary is published, so there is nothing for the script to download.
     See docs/MAINTAINERS.md "Retired platforms". -->

**Linux:**

```bash
curl -fsSL https://raw.githubusercontent.com/patalbansishashank/hyponoia/main/scripts/setup.sh | bash
```

**Windows (PowerShell):**

```powershell
irm https://raw.githubusercontent.com/patalbansishashank/hyponoia/main/scripts/setup-windows.ps1 | iex
```

On macOS the download path refuses — there is no binary to fetch. Building from
source still works, and `setup.sh` will do it:

```bash
curl -fsSL https://raw.githubusercontent.com/patalbansishashank/hyponoia/main/scripts/setup.sh | bash -s -- --from-source
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

Works on Linux, macOS and Windows. This is the only path on macOS, where no
binary is published.

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
scripts/package-release.sh <linux|windows> <amd64|arm64>   # windows: amd64 only
```

<!-- RETIRED-PLATFORM(macos): the script still accepts `darwin`, but no darwin
     archive is published, so it is no longer documented here.
     See docs/MAINTAINERS.md "Retired platforms". -->

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
