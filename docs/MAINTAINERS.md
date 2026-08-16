# Maintainers

Hyponoia has one maintainer.

| Handle | Contact | Scope |
| --- | --- | --- |
| [`@patalbansishashank`](https://github.com/patalbansishashank) | mail@creative.desi | Everything: code, review, security, releases, and repository settings. |

Every pull request is reviewed and merged by the maintainer. There is no review
quorum, no delegation, and no escalation path, because with one person those
would be fiction. `.github/CODEOWNERS` is the binding version of this file;
this document explains it.

Security reports go through [SECURITY.md](../.github/SECURITY.md), not the issue tracker.
Conduct reports go to the address above — see
[CODE_OF_CONDUCT.md](../.github/CODE_OF_CONDUCT.md).

## Release discipline

Release process is a checklist rather than an approval chain:

- `dry-run.yml` completes successfully on the release-candidate commit. Let it
  finish: a cancelled job is not a passing job, and a run stopped early reports
  the first failure rather than the count.
- `full-suite.yml` runs the same full matrix every Monday without being asked.
  It is the standing check between releases — `ci.yml` covers only 301 of the
  ~6,610 tests, and that gap is how five months of drift once accumulated
  unseen. A red weekly run is a release blocker discovered early, not noise.
- Indexing benchmarks are run with the candidate binary on real repositories,
  not test-only shortcuts, and compared against the previous release on the
  same machine class and same repository revisions.
- An unexplained indexing slowdown over 15%, or an unexplained shift in node
  and edge counts, blocks the release until it is understood.
- Benchmark logs, repository revisions, binary version, and machine details are
  kept with the release notes.
- Package-manager manifests are bumped with the version and their checksums
  **zeroed**, then filled from the release's own `checksums.txt` and re-verified
  by re-hashing the downloaded archives. [Packaging](PACKAGING.md) lists every
  file and field a release touches, the offline validator for each registry, and
  what submitting to each one would actually require.

## Retired platforms

macOS (`darwin-arm64`, `darwin-amd64`) and ARM64 Windows (`windows-arm64`) are
retired as of 2026-08-15. They are not tested, not built, and not published.
Published assets are exactly five: `linux-amd64`, `linux-arm64`, the two Linux
`-portable` static builds, and `windows-amd64`.

This was not a preference. Both platforms were already failing before the
compiler reached a test:

- **macOS** — vendored llama.cpp does not compile against `MacOSX26.sdk` under
  `-std=c11`, which hides `u_int` (it wants `_DARWIN_C_SOURCE`). No test result
  was available on macOS, and no artifact could have been produced to ship.
- **ARM64 Windows** — the product does not link: ggml's aarch64 GEMM kernels are
  never compiled into the CLANGARM64 target
  (`undefined symbol: ggml_gemm_q4_0_4x4_q8_0`).

Neither is a test to repair, and neither could be repaired on the hardware this
project is developed on. Retiring them is what makes the remaining matrix mean
something: a red board that nobody can turn green teaches you to ignore red.

**Every site is marked.** `grep -rn RETIRED-PLATFORM` finds all of them, tagged
`RETIRED-PLATFORM(macos)` or `RETIRED-PLATFORM(windows-arm64)`. Where a job held
a recipe worth keeping — `test-lsan-macos` is the only written description of how
to get LeakSanitizer on darwin, and `build-windows-arm64` is a working CLANGARM64
build — the job is disabled with `if: false` rather than deleted, so restoring it
is one line.

What users see, deliberately rather than by accident:

- **macOS** — the installers refuse early and say that no macOS binaries are
  published for this release. They do not proceed to a download that 404s.
  Building from source on macOS is not removed; only the published binary is.
- **Windows on ARM** — the installers fall back to the `amd64` binary under
  emulation, which is what these users got before a native ARM64 build existed.
  Explicit `HYP_ARCH`-style overrides still win; the fallback applies only to
  auto-detection.
- **Already-installed macOS users cannot be rescued.** `hyponoia update` builds
  its download URL in the shipped binary, so it will 404 against every future
  release. Say so in the release notes; there is no code change that reaches
  binaries already on disk.

**To bring one back:** flip the `if: false`, restore the matrix rows, and fix the
count in `scripts/ci/extract-release-archives.sh` and the two contract tests that
keep their own copies of the target list — the exact-set gate in the release's
`verify` job will otherwise fail *after* the draft is created and the tag pushed.
The one-line test for whether it is time: does the platform build and link from a
clean checkout on a current runner image?

## Adding maintainers

When someone else starts maintaining part of this project, add a row to the
table above and give them the paths they own in `.github/CODEOWNERS`:

```
/src/mcp/  @their-handle
```

That is the whole process. Roles and review policy can be written down when
there are enough people for them to mean something.
