# Running Hyponoia

Ignore rules, configuration, environment, persistence, and what to do when something is wrong.

[← README](../README.md)

## Ignoring Files

Layered: hardcoded patterns (`.git`, `node_modules`, etc.) → `.gitignore` hierarchy → `.hypignore` (project-specific, gitignore syntax). Symlinks are always skipped.

See [docs/hypignore.md](docs/hypignore.md) for the full `.hypignore` how-to: syntax, precedence across the ignore layers, and negation semantics.

## Configuration

```bash
hyponoia config list                          # show all settings
hyponoia config set auto_index true           # auto-index on session start
hyponoia config set auto_index_limit 50000    # max files for auto-index
hyponoia config set auto_watch false          # don't register background git watcher (default: true)
hyponoia config reset auto_index              # reset to default
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `HYP_ALLOWED_ROOT` | *(unset)* | Confine `index_repository` to paths within this directory. When set, a `repo_path` that resolves (after symlink / `..` resolution) outside this root is refused, and the same check now applies to the graph UI's `POST /api/index` route rather than only to the MCP tool. Unset imposes no *containment* restriction — but see the always-on limits below, which apply whether or not this is set. Useful when the server may be driven by an untrusted caller, e.g. agentic or multi-tenant deployments. |
| `HYP_CACHE_DIR` | `~/.cache/hyponoia` | Override the database storage directory. All project indexes and config are stored here. One account can use only one canonical cache root at a time; close active HYP sessions/commands before switching it. |
| `HYP_DAEMON_RUNTIME_PARENT` | *(unset)* | Run this build's daemon under a private rendezvous instead of the account-wide one, so it can coexist with an already-running build. Requires a non-default `HYP_CACHE_DIR`. See [Running a second build beside the active one](#running-a-second-build-beside-the-active-one). |
| `HYP_DIAGNOSTICS` | `false` | Set to `1` or `true` to enable the shared daemon's periodic `snapshot.json` and retained `trajectory.ndjson` below a fresh owner-private directory in the system temp directory. Exact paths are logged by `diagnostics.start`. |
| `HYP_DOWNLOAD_URL` | *(GitHub releases)* | Override the download URL for updates. Used for testing or self-hosted deployments. |
| `HYP_LOG_LEVEL` | `info` | Set the minimum log level. Accepted values (case-insensitive): `debug`, `info`, `warn`, `error`, `none` — or their numeric equivalents `0`–`4` matching the internal enum. Thin-frontend messages go to that session's stderr; detached daemon events go to `${HYP_CACHE_DIR}/logs/hyp-daemon.log`. Stdout is reserved for MCP JSON-RPC. |
| `HYP_WORKERS` | *(detected)* | Override the parallel-indexing worker count returned by `hyp_default_worker_count`. Useful inside containers where `sysconf(_SC_NPROCESSORS_ONLN)` reports host CPUs rather than the cgroup's effective quota. Range 1–256; invalid values are ignored with a warning. |
| `HYP_MEM_BUDGET_MB` | *(detected)* | Override the in-memory graph budget with an explicit cap in MiB, taking precedence over the `ram_fraction × total_RAM` default. Useful on bare-metal hosts without a cgroup limit, or to pin a budget *below* the cgroup limit so headroom is left for sibling processes. Must be a positive integer; it is clamped to detected total RAM (logged as `mem.budget.clamped`), and non-numeric or non-positive values are ignored with a warning (`mem.budget.env.invalid`). |
| `HYP_DUMP_VERIFY_MIN_RATIO` | `0.5` | After indexing, compare persisted SQLite node count to the in-memory dump count. When persisted nodes fall below this fraction of committed nodes (and committed > 50), `index_repository` returns `status:"degraded"` instead of silent `indexed`. Range 0–1; set `0` to disable. Invalid values are ignored with a warning. |

Environment used by daemon-owned components—such as diagnostics, daemon logging, and process-wide indexing resource limits—is captured from the first daemon-backed session that starts the daemon. Later sessions join that process and cannot replace those values. To change them, close all daemon-backed sessions, update the relevant agent configurations consistently, and restart a session. `HYP_ALLOWED_ROOT` remains session-specific, a conflicting `HYP_CACHE_DIR` is rejected, and one-shot CLI commands read their own environment without starting the daemon.

```bash
## Store indexes in a custom directory
export HYP_CACHE_DIR=~/my-projects/hyp-data
```

## Running a second build beside the active one

HYP coordinates through **one rendezvous directory per account** —
`/tmp/hyp-daemon-<uid>/` on Linux and macOS — holding the daemon socket, the
version-cohort record, and the project mutation leases. Inside it, exactly one
build is admitted. A second build is refused, by design:

```
hyponoia: HYP could not start because a conflicting HYP process is active
(build; active version 0.3.1, build 5949f2d7…; requested version dev, build 98d04a55…).
Close all HYP sessions and commands, then retry.
```

The refusal is protecting a **cache**, not a socket. Two builds admitted at
once would interleave writes into one set of SQLite databases, and the project
leases that serialise those writes live in the rendezvous directory, so two
rendezvous directories over one cache serialise nothing at all.

That is also the whole rule for escaping it: **give the second build its own
cache, and it may have its own rendezvous.**

```bash
export HYP_DAEMON_RUNTIME_PARENT=/tmp/hyp-isolated
export HYP_CACHE_DIR=/tmp/hyp-isolated/cache
./build/c/hyponoia daemon start        # coexists with the installed daemon
./build/c/hyponoia daemon status
./build/c/hyponoia cli search_graph --name-pattern '.*'
./build/c/hyponoia daemon stop         # retires only this build's daemon
```

Every command that reaches the daemon reads both variables, including the
detached daemon child, so setting them once in a shell (or in an MCP server's
`env` block) is enough. `daemon stop` under them retires only the isolated
daemon; the account-wide one is untouched.

What the two variables mean:

- **`HYP_DAEMON_RUNTIME_PARENT`** is where the private rendezvous tree is
  created. It is created mode `0700` and owned by you, and every existing
  ancestor must already be owned by you (or by root) and must not be group- or
  world-writable — the same rule the account-wide `/tmp` directory satisfies.
  The daemon socket is a unix-domain path with a hard 108-byte limit
  (104 on the BSDs), so the parent may be at most ~48 bytes; a longer one is
  refused by name rather than failing later as "secure daemon endpoint could
  not be created". `/tmp/hyp-isolated` and `$XDG_RUNTIME_DIR/hyp` both fit.
- **`HYP_CACHE_DIR`** must not resolve to the account default
  (`~/.cache/hyponoia`). If it does, the isolation is refused outright:

  ```
  hyponoia: HYP_DAEMON_RUNTIME_PARENT is set, but HYP_CACHE_DIR still resolves
  to the account default (/home/you/.cache/hyponoia). An isolated rendezvous
  over the shared cache would run two daemons against one store, which is the
  corruption the cohort guard exists to prevent. Point HYP_CACHE_DIR at a
  directory this build owns, then retry.
  ```

The private directory is named from a digest of the canonical cache root —
`<parent>/hyp-scope-<12 hex>/hyp-daemon-<uid>/`. That is what makes the escape
safe rather than merely convenient: the rendezvous is a **function of the
cache**, so any two processes pointed at one cache still land in one rendezvous
and still meet the guard, no matter which parent each of them named. Several
isolated runs can therefore share one `HYP_DAEMON_RUNTIME_PARENT` and stay
separated by their caches alone.

The build fingerprint is deliberately **not** part of that name. Keying the
meeting place by build is precisely how two builds would stop meeting while
still writing to one store; it stays what the cohort *compares* once two
processes have met.

## Custom File Extensions

The JSON config files support a single key, `extra_extensions`, which maps additional file extensions to supported languages. Useful for framework-specific extensions like `.blade.php` (Laravel) or `.mjs` (ES modules). (For other tunables, see [Environment Variables](#environment-variables) and the `config` subcommand above.)

Need the full config-file reference? See [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

**Per-project** (in your repo root):
```json
// .hyponoia.json
{"extra_extensions": {".blade.php": "php", ".mjs": "javascript"}}
```

**Global** (applies to all projects):
```json
// ~/.config/hyponoia/config.json  (or $XDG_CONFIG_HOME/...)
{"extra_extensions": {".twig": "html", ".phtml": "php"}}
```

Each entry maps an extension (which **must** start with `.`) to a language name. Language names are matched **case-insensitively**. Accepted values (aliases in parentheses) are:

`bash` (`sh`), `c`, `c++` (`cpp`), `c#` (`csharp`), `clojure`, `cmake`, `cobol`, `common lisp` (`commonlisp`, `lisp`), `css`, `cuda`, `dart`, `dockerfile`, `elixir`, `elm`, `emacs lisp` (`emacslisp`), `erlang`, `f#` (`fsharp`), `form`, `fortran`, `glsl`, `go`, `graphql`, `groovy`, `haskell`, `hcl` (`terraform`), `html`, `ini`, `java`, `javascript`, `json`, `julia`, `kotlin`, `lean`, `lua`, `magma`, `makefile`, `markdown`, `matlab`, `meson`, `nix`, `objective-c` (`objc`), `ocaml`, `perl`, `php`, `protobuf`, `python`, `r`, `ruby`, `rust`, `scala`, `scss`, `sql`, `svelte`, `swift`, `toml`, `tsx`, `typescript`, `verilog`, `vimscript`, `vue`, `wolfram`, `xml`, `yaml`, `zig`.

Project config overrides global for conflicting extensions. An entry whose language name is unknown, or whose extension does not start with `.`, is skipped and a warning is logged to stderr (shown at the default `info` log level). Missing config files are ignored.

## Persistence

SQLite databases stored at `~/.cache/hyponoia/`. Persists across restarts (WAL mode, ACID-safe). To reset: `rm -rf ~/.cache/hyponoia/`.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `/mcp` doesn't show the server | Check `.mcp.json` path is absolute. Restart agent. Test: `echo '{}' \| /path/to/binary` should output JSON. **An agent whose HYP server fails to start keeps running with no tools and does not always say so** — check the agent's MCP status before trusting an answer that never cites the graph. |
| `conflicting HYP process is active` / `uses a different cache directory` | Another build's daemon holds the account. Either close it (`hyponoia daemon stop`), or run this build isolated — see [Running a second build beside the active one](#running-a-second-build-beside-the-active-one). |
| `install refused before any file was changed` | The directory chain above the binary you are installing is group- or world-writable, which install refuses so no other account can substitute the file mid-copy. The message names the exact directory and mode. Copy the binary into a directory you own privately and install from there. |
| `index_repository` fails | Pass absolute path: `index_repository(repo_path="/absolute/path")` |
| `trace_path` returns 0 results | Use `search_graph(name_pattern=".*PartialName.*")` first to find the exact name. |
| Queries return wrong project results | Add `project="name"` parameter. Use `list_projects` to see names. |
| Binary not found after install | Add to PATH: `export PATH="$HOME/.local/bin:$PATH"` |
| UI not loading | Ensure you downloaded the `ui` variant and ran `--ui=true`. Check `http://localhost:9749`. |
