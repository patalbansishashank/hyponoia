# Packaging and registry submission

Everything needed to publish Hyponoia to a package registry, and the reasons
not to publish it to some of them.

[← docs](README.md) · [Installing](INSTALL.md) · [Maintainers](MAINTAINERS.md)

> **Nothing has been submitted anywhere.** As of `v0.3.1` (2026-08-16) all five
> manifests carry that release's real SHA-256 checksums and have been validated
> offline, and not one has been sent to a registry. No registry account exists
> for this project. Submission is the step this document makes mechanical; it
> is not a step that has been taken.

## What each manifest installs

| Registry | Manifest in this repo | Archive it downloads | Verified against the live release |
|---|---|---|---|
| Homebrew | [`pkg/homebrew/Formula/hyponoia.rb`](../pkg/homebrew/Formula/hyponoia.rb) | `hyponoia-ui-linux-{amd64,arm64}-portable.tar.gz` | `045e2e08…` / `43f4be9c…` |
| Scoop | [`pkg/scoop/hyponoia.json`](../pkg/scoop/hyponoia.json) | `hyponoia-ui-windows-amd64.zip` | `fee50ab1…` |
| Chocolatey | [`pkg/chocolatey/`](../pkg/chocolatey/) | `hyponoia-ui-windows-amd64.zip` | `fee50ab1…` |
| AUR | [`pkg/aur/`](../pkg/aur/) | `hyponoia-ui-linux-{amd64,arm64}.tar.gz` | `c4fa4ef5…` / `946d6036…` |
| winget | [`pkg/winget/manifests/…`](../pkg/winget/manifests/p/patalbansishashank/Hyponoia/0.3.1/) | `hyponoia-ui-windows-amd64.zip` | `fee50ab1…` |
| npm | [`pkg/npm/`](../pkg/npm/) | `hyponoia-ui-linux-*-portable.tar.gz`, `hyponoia-ui-windows-amd64.zip` | composed at install time |
| PyPI | [`pkg/pypi/`](../pkg/pypi/) | same as npm | composed at install time |

**Homebrew takes the static archive and AUR takes the dynamic one, deliberately.**
The ordinary Linux archives link glibc 2.38+ and `GLIBCXX_3.4.32`. Homebrew on
Linux exists largely to serve distributions older than that — installed into the
`homebrew/brew` image (Ubuntu 22.04, glibc 2.35) the ordinary archive resolves,
checksums, links and then dies on the first run with ``version `GLIBC_2.38' not
found``. Arch is never older than the build host, so the AUR package links the
system runtime like any other distribution package. npm and PyPI already chose
`-portable` for the same reason Homebrew now does.

## The rule that keeps a checksum honest

A real checksum for the wrong tag fails at install time with a mismatch nobody
can attribute to a cause. So checksums move **only together with the version**:

1. Bump the version in every manifest and **zero the checksums**.
2. Cut the release.
3. Copy the checksums from the release's own `checksums.txt`.
4. Re-verify by downloading each archive and hashing it yourself — the
   `checksums.txt` and the manifest can agree with each other and both be wrong
   about what is actually stored under that URL.
5. Run the validators below.

Step 4 is not ceremony. `v0.3.0` shipped six installers that asked for an
archive name the build never produced, and nothing caught it because no manifest
had ever been resolved against a real release.

## Validating without submitting

Every command below runs locally, downloads nothing that is not public, and
sends nothing anywhere. Each was run against `v0.3.1`.

```bash
# Homebrew — needs no local brew: the official image has one.
docker run --rm -v "$PWD:/repo:ro" homebrew/brew bash -lc '
  mkdir /tmp/r && tar -C /repo -cf - --exclude=.git . | tar -C /tmp/r -xf -
  cd /tmp/r && git init -q && git add -A && git -c user.email=a@b.c -c user.name=t commit -qm i
  brew tap patalbansishashank/hyponoia /tmp/r
  brew style patalbansishashank/hyponoia/hyponoia
  brew audit --formula --strict --online patalbansishashank/hyponoia/hyponoia
  brew install hyponoia && hyponoia --version && brew test hyponoia'

# AUR — --printsrcinfo and --verifysource only. Neither builds nor installs.
cd pkg/aur && makepkg --printsrcinfo | diff -u .SRCINFO -   # must be identical
cp PKGBUILD /tmp/aur/ && cd /tmp/aur && makepkg --verifysource

# Scoop — the bucket CI's own schema.
curl -sSLO https://raw.githubusercontent.com/ScoopInstaller/Scoop/master/schema.json
check-jsonschema --schemafile schema.json pkg/scoop/hyponoia.json

# winget — the schemas the winget-pkgs pipeline validates against.
B=https://raw.githubusercontent.com/microsoft/winget-cli/master/schemas/JSON/manifests/v1.6.0
for s in version defaultLocale installer; do curl -sSLO "$B/manifest.$s.1.6.0.json"; done
check-jsonschema --schemafile manifest.installer.1.6.0.json  pkg/winget/manifests/p/*/*/*/*.installer.yaml

# Chocolatey — no choco on Linux. NuGet's nuspec.xsd is a template with a {0}
# namespace placeholder; substitute the 2015/06 namespace and add Chocolatey's
# eight extension elements (packageSourceUrl, projectSourceUrl, docsUrl,
# mailingListUrl, bugTrackerUrl, replaces, provides, conflicts).
xmllint --noout --schema nuspec-choco.xsd pkg/chocolatey/hyponoia.nuspec
docker run --rm -v "$PWD/pkg/chocolatey:/c:ro" mcr.microsoft.com/powershell \
  pwsh -NoProfile -Command '[System.Management.Automation.Language.Parser]::ParseFile(
    "/c/tools/chocolateyInstall.ps1", [ref]$null, [ref]$errors)'

# npm / PyPI — pack and check, never publish.
cd pkg/npm  && npm pack --dry-run
cd pkg/pypi && python -m build && twine check dist/*
```

**What cannot be checked here.** `winget validate --manifest` and `choco pack`
are Windows-only and have no Linux equivalent; the schema and XSD checks above
are the closest available substitutes and are not the same thing. Chocolatey's
own package validator and verifier run only on submission.

---

## Homebrew

**Where it goes: nowhere new.** This repository *is* the tap. The root
`Formula` symlink pointing at `pkg/homebrew/Formula` is what makes that true —
Homebrew scans `Formula/`, `HomebrewFormula/` and the tap root, and nothing
deeper, so without the symlink `brew tap` reports "Tapped 0 formulae" and
`brew install hyponoia` fails with "No available formula". Do not delete it.

```bash
brew tap patalbansishashank/hyponoia https://github.com/patalbansishashank/hyponoia
brew install hyponoia
```

Verified end-to-end against the published repository: `Tapped 1 formula`,
`brew info hyponoia` resolves, `brew install` verifies the SHA-256 and runs the
install block, `brew test` passes, `hyponoia --version` prints `hyponoia 0.3.1`.

- **Credentials:** none. **Review latency:** none. There is no submission.
- **Per release:** `version`, and the two `sha256` lines.
- **Known `--strict` audit finding, deliberate:** *"`version 0.3.1` is redundant
  with version scanned from URL"*. Removing the `version` stanza would satisfy
  the auditor and break `_smoke.yml`, which reads that exact line to decide
  whether the pinned release exists — and would degrade its failure into a
  misleading skip. The formula is tap-only, so a homebrew-core style rule does
  not bind it. `brew style` is clean.

**Do not submit to homebrew-core.** Three independent blockers, any one of which
is fatal: homebrew-core formulae are built from source and this one installs a
prebuilt binary; homebrew-core has notability requirements (stars, forks,
watchers) this project does not meet; and the formula is `depends_on :linux`,
because macOS is a [retired platform](MAINTAINERS.md#retired-platforms).
A personal tap is the correct and permanent home for it — and it already exists.

## Scoop

**Where it goes:** a bucket repository, not this one. Scoop reads manifests from
a bucket's `bucket/` directory, falling back to the repository root — the same
"the tool only looks in certain places" trap as Homebrew, and
`pkg/scoop/hyponoia.json` is in neither.

- **Personal bucket (recommended first step).** Create
  `patalbansishashank/scoop-hyponoia` with the manifest at
  `bucket/hyponoia.json`, then users run:
  ```
  scoop bucket add hyponoia https://github.com/patalbansishashank/scoop-hyponoia
  scoop install hyponoia
  ```
  Credentials: a GitHub account (the owner's). Review latency: none.
- **`ScoopInstaller/Extras`.** Fork, add `bucket/hyponoia.json`, open a PR.
  Extras is where third-party CLI and GUI apps live. Review is community
  volunteers: typically days.
- **`ScoopInstaller/Main` — not yet.** Main is reserved for well-known,
  widely-used CLI tools. A project with no installs has no case for it, and
  being turned down there is a worse first contact than not asking.

`checkver` and `autoupdate` were exercised rather than assumed: the GitHub
releases atom feed yields `v0.3.1` under Scoop's `/releases/tag/(?:v|V)?([\d.]+)`
extraction, and the `autoupdate` hash regex
`([a-f0-9]{64})\s+hyponoia-ui-windows-amd64\.zip` run over the release's real
`checksums.txt` returns exactly the hash pinned in the manifest. A bucket's
Excavator bot can therefore bump this manifest unattended.

- **Per release:** nothing by hand if Excavator runs; otherwise `version`, the
  `url` and the `hash`.

## Chocolatey

**Where it goes:** `https://push.chocolatey.org/` (the community repository),
by API key.

```powershell
choco pack pkg\chocolatey\hyponoia.nuspec
choco apikey --source https://push.chocolatey.org/ --key <API KEY>
choco push hyponoia.0.3.1.nupkg --source https://push.chocolatey.org/
```

- **Credentials:** an account on community.chocolatey.org and its API key.
- **Where it can be done:** Windows only. `choco` does not exist on Linux, so
  neither `choco pack` nor `choco push` can be run or rehearsed from this
  machine. The nuspec was validated against the NuGet 2015.06 schema instead
  (see above), and both PowerShell scripts were parsed by pwsh in a container.
- **Review latency:** moderation, not merge. An automated validator and verifier
  run first, then a human moderator. Days to weeks is normal, and a rejection
  usually asks for packaging changes rather than product changes.
- **Per release:** `<version>` and `<releaseNotes>` in the nuspec, `$version`
  and `$checksum64` in `tools/chocolateyInstall.ps1`, and the version, URL and
  hash quoted in `tools/VERIFICATION.txt`.
- `tools/VERIFICATION.txt` and `tools/LICENSE.txt` are present because
  moderation asks for them on any package that fetches a binary.

## AUR

**Where it goes:** `ssh://aur@aur.archlinux.org/hyponoia-bin.git`. The AUR
repository *is* the package: it contains `PKGBUILD` and `.SRCINFO` and nothing
else.

```bash
git clone ssh://aur@aur.archlinux.org/hyponoia-bin.git
cp pkg/aur/PKGBUILD pkg/aur/.SRCINFO hyponoia-bin/
cd hyponoia-bin && git add PKGBUILD .SRCINFO && git commit && git push
```

- **Credentials:** an AUR account with an SSH public key registered on it.
- **Review latency: none, and that is the risk.** A push is live immediately.
  There is no moderator to catch a bad checksum; `makepkg --verifysource` before
  pushing is the only gate that exists.
- **The package is named `hyponoia-bin`,** not `hyponoia`. AUR naming rules
  reserve the plain name for a from-source package and require the `-bin` suffix
  on one that repackages an upstream binary. `docs/INSTALL.md` has always told
  users to install `hyponoia-bin`; the PKGBUILD now agrees with it.
- **Per release:** `pkgver`, `pkgrel=1`, both `sha256sums_*`, then regenerate:
  ```bash
  cd pkg/aur && makepkg --printsrcinfo > .SRCINFO
  ```
  `.SRCINFO` is not documentation — it is what the AUR web interface and every
  helper read. A `.SRCINFO` that disagrees with its `PKGBUILD` is the most
  common AUR defect there is, and the diff above is a one-line check.

## winget

**Where it goes:** a pull request to
[`microsoft/winget-pkgs`](https://github.com/microsoft/winget-pkgs), adding
`manifests/p/patalbansishashank/Hyponoia/0.3.1/` — three files, unchanged from
this repository.

```bash
gh repo fork microsoft/winget-pkgs --clone
cp -r pkg/winget/manifests/p/patalbansishashank winget-pkgs/manifests/p/
# then a branch, a commit and a PR from the fork
```

- **Credentials:** a GitHub account and a fork. On Windows,
  `wingetcreate submit` does the same thing with a token.
- **Review latency:** an automated validation pipeline runs within minutes and a
  moderator follows; hours to a few days is typical. Most rejections are schema
  or installer-behaviour issues, both of which are checkable in advance.
- **`winget validate --manifest` could not be run here** — it is Windows-only.
  All three manifests validate against the published 1.6.0 JSON schemas, which
  is what the pipeline checks, but it is not the same tool.
- **`ManifestVersion` is 1.6.0; the current schema is 1.28.0.** Older manifest
  versions are still accepted. A reviewer may ask for a bump; that is a
  three-line change plus re-validation against the newer schemas.
- **Per release:** a new version directory, `PackageVersion` in all three files,
  `InstallerUrl`, `InstallerSha256`, `ReleaseDate`, `ReleaseNotesUrl`.

## npm and PyPI — automated, and currently switched off

Neither is a manual submission. [`release.yml`](../.github/workflows/release.yml)
publishes both from the `publish-registries` job, gated on secrets that its
`preflight` job probes for presence:

| Channel | Secret | Effect when absent |
|---|---|---|
| npm | `NPM_TOKEN` | `npm publish` step skipped; release continues |
| PyPI | `PYPI_TOKEN` | `python -m build` + `twine upload` skipped; release continues |
| VirusTotal scan | `VIRUS_TOTAL_SCANNER_API_KEY` | scan skipped; release continues |
| MCP registry | *(none — gated on `NPM_TOKEN`)* | skipped, because the registry proves ownership through the published npm package |

**All three secrets are absent today.**
`gh api repos/patalbansishashank/hyponoia/actions/secrets --jq .total_count`
returns `0`. So every release so far has published the GitHub release and
nothing else, silently and by design — the preflight writes the table above into
the job summary so the skip is visible rather than inferred.

Both names are unclaimed: `registry.npmjs.org/hyponoia` and
`pypi.org/pypi/hyponoia/json` both return 404, and the MCP registry returns zero
servers for the query. Nobody else is going to take them by accident, but the
first release with a token set is also the moment the name is claimed.

To enable, from a machine with the tokens:

```bash
gh secret set NPM_TOKEN  -R patalbansishashank/hyponoia
gh secret set PYPI_TOKEN -R patalbansishashank/hyponoia
```

npm publishes with `--provenance` (the job already has `id-token: write`), and a
prerelease version goes to the `next` dist-tag rather than `latest`.

## What a new release must touch

| File | Fields |
|---|---|
| `pkg/homebrew/Formula/hyponoia.rb` | `version`, 2 × `sha256` |
| `pkg/scoop/hyponoia.json` | `version`, `url`, `hash` |
| `pkg/chocolatey/hyponoia.nuspec` | `<version>`, `<releaseNotes>` |
| `pkg/chocolatey/tools/chocolateyInstall.ps1` | `$version`, `$checksum64` |
| `pkg/chocolatey/tools/VERIFICATION.txt` | version, URL, hash |
| `pkg/aur/PKGBUILD` | `pkgver`, `pkgrel`, 2 × `sha256sums_*` |
| `pkg/aur/.SRCINFO` | regenerated, never edited |
| `pkg/winget/manifests/…/<version>/` | new directory, 3 files |
| `pkg/npm/package.json`, `pkg/pypi/pyproject.toml` | rewritten by `release.yml` itself |

The npm and PyPI versions are the only two the pipeline sets for you — it
rewrites them from the release tag before publishing, because `0.8.0` once
failed there on a hand-pinned version that no longer matched.
