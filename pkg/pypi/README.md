# Hyponoia

mcp-name: io.github.patalbansishashank/hyponoia

**Fast code intelligence engine for AI coding agents.** Indexes an average repository in milliseconds, the Linux kernel (28M LOC) in 3 minutes. Answers structural queries in under 1ms.

This Python wrapper downloads the selected `hyponoia` runtime set from [GitHub Releases](https://github.com/patalbansishashank/hyponoia/releases) on first run and verifies it before publishing it in your OS cache directory. The standard set contains the native executable and authenticated integration asset; `HYP_VARIANT=ui` additionally selects the content-addressed UI pack.

## Installation

```bash
pip install hyponoia
# or
pipx install hyponoia
```

To use the UI variant, set `HYP_VARIANT=ui` when invoking the wrapper (and consistently for any package-managed update or reinstall).

## Usage

```bash
hyponoia install   # configure your coding agents
hyponoia --help
```

## Supported platforms

| OS      | Architecture |
|---------|-------------|
| Linux   | arm64, amd64 |
| Windows | amd64 |

No binaries are published for macOS or for ARM64 Windows. macOS users can build
from source; ARM64 Windows runs the amd64 build under emulation.

<!-- RETIRED-PLATFORM(macos), RETIRED-PLATFORM(windows-arm64): the macOS row and
     the Windows arm64 entry were removed here. See docs/MAINTAINERS.md
     "Retired platforms". -->

## Full documentation

See [github.com/patalbansishashank/hyponoia](https://github.com/patalbansishashank/hyponoia)
