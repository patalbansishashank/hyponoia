#!/usr/bin/env bash
# check-tracked-artifacts.sh — refuse a tree that tracks a compiled build
# artifact. NEXT-STEPS.md §3.2 step 6.
#
# It exists because of `pkg/go/hyponoia`: a 9,428,133-byte compiled Go binary
# tracked from dd41cf92, shipped inside the v0.3.0 AND v0.3.1 tags, and found
# only because a grep for an unrelated sha256 happened to match a string
# embedded in its .rodata. Nothing looked for it. `.gitignore` covers that one
# path now, which stops the same file and nothing else — a .gitignore is a
# convenience, not a gate, and it is silent about what is ALREADY tracked.
#
# The rules are about SHAPE, not about paths, because the next artifact will
# have a different name:
#
#   1. executable image  — ELF, Mach-O (incl. universal), PE-COFF (MZ with a
#                          real PE\0\0 header), ar/COFF archive, WebAssembly,
#                          Java class. A compiled image is never source.
#   2. compressed archive — zip/jar/whl, gzip, bzip2, xz, zstd, 7z, lz4, tar,
#                          GGUF. A build OUTPUT that rode in packaged.
#   3. opaque blob       — a NUL byte inside the first 8 KiB (the heuristic git
#                          itself uses for "binary") AND larger than
#                          BLOB_MAX_BYTES. Catches artifact shapes with no
#                          magic number at all.
#
# Rule 3 is deliberately size-gated and rules 1-2 deliberately are not: a
# 20 KiB ELF fixture is still a compiled image and still has to be argued for,
# while a small binary test fixture (icons, a 4 KiB manifest) is ordinary.
#
# What must NOT trip it, and does not:
#   - internal/hyp/vendored/grammars/*/parser.c — 40-104 MB of GENERATED C.
#     They are text: no magic, no NUL, so no rule reaches them at any size.
#     Size alone was never the signal; "binary" is.
#   - vendored/mimalloc/src/prim/windows/etw.man — UTF-16, so NUL-dense, but
#     3,926 bytes, so under rule 3's floor.
#
# The escape is scripts/ci/tracked-artifact-allowlist.txt: one glob per line with
# a MANDATORY justification after `:`. An entry with no justification is an
# error, and so is an entry that matches nothing — a stale exemption is how an
# allowlist quietly becomes a hole.
#
# Usage:
#   check-tracked-artifacts.sh              gate the current tree (CI + local)
#   check-tracked-artifacts.sh --list       print every tracked file the rules
#                                           classify as non-source, allowed or
#                                           not, and exit 0
#   check-tracked-artifacts.sh --selftest   plant a real ELF, a real PE, a
#                                           Mach-O and an opaque blob in a
#                                           throwaway repository and prove the
#                                           gate REFUSES them, then prove the
#                                           allowlist releases them
#
# The self-test is not optional decoration. A scanner nobody has watched fail
# is indistinguishable from `exit 0`, which is the same argument that made
# check-secret-history.sh --selftest a CI step.
set -euo pipefail

MODE=gate
case "${1:-}" in
-h | --help)
    sed -n '2,58p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
--list) MODE=list ;;
--selftest) MODE=selftest ;;
"") ;;
*)
    echo "check-tracked-artifacts: unknown argument '$1'. Please consult --help." >&2
    exit 2
    ;;
esac

# A tracked binary blob larger than this is an artifact until argued otherwise.
# 256 KiB: the largest non-allowlisted binary in this tree is etw.man at 3,926
# bytes, so the floor sits ~65x above what legitimately exists, and far below
# any compiled image worth shipping.
BLOB_MAX_BYTES="${HYP_TRACKED_BLOB_MAX_BYTES:-262144}"

scan() {
    # $1 = repo root, $2 = allowlist path (may be absent → no exemptions)
    ROOT="$1" ALLOWLIST="$2" MODE="$MODE" BLOB_MAX_BYTES="$BLOB_MAX_BYTES" \
        python3 -c '
import os, re, subprocess, sys

root = os.environ["ROOT"]
allowlist_path = os.environ["ALLOWLIST"]
mode = os.environ["MODE"]
blob_max = int(os.environ["BLOB_MAX_BYTES"])

# ── the allowlist: "<glob> : <justification>" ────────────────────────────────
def load_allowlist(path):
    entries = []
    if not path or not os.path.isfile(path):
        return entries
    for lineno, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            sys.exit(f"allowlist {path}:{lineno}: no justification. "
                     f"Format is `<glob> : <why this file is not a build artifact>`.")
        glob, why = line.split(":", 1)
        glob, why = glob.strip(), why.strip()
        if not glob or not why:
            sys.exit(f"allowlist {path}:{lineno}: empty glob or empty justification.")
        entries.append((glob, why, lineno))
    return entries

# Path-aware glob: `*` and `?` never cross a `/`; `**/` spans directories.
# fnmatch is wrong here — its `*` matches `/`, so `vendored/*/x` would also
# exempt `vendored/a/b/c/x`, which is a wider hole than the entry claims.
def glob_to_re(glob):
    out, i = [], 0
    while i < len(glob):
        if glob.startswith("**/", i):
            out.append("(?:[^/]+/)*"); i += 3
        elif glob[i] == "*":
            out.append("[^/]*"); i += 1
        elif glob[i] == "?":
            out.append("[^/]"); i += 1
        else:
            out.append(re.escape(glob[i])); i += 1
    return re.compile("^" + "".join(out) + "$")

allow = [(g, w, ln, glob_to_re(g)) for g, w, ln in load_allowlist(allowlist_path)]

# ── the magic tables ─────────────────────────────────────────────────────────
EXEC_MAGIC = [
    (b"\x7fELF",                 "ELF executable/shared object/relocatable"),
    (b"\xfe\xed\xfa\xce",        "Mach-O 32-bit"),
    (b"\xfe\xed\xfa\xcf",        "Mach-O 64-bit"),
    (b"\xce\xfa\xed\xfe",        "Mach-O 32-bit (LE)"),
    (b"\xcf\xfa\xed\xfe",        "Mach-O 64-bit (LE)"),
    (b"\xca\xfe\xba\xbe",        "Mach-O universal binary or Java class"),
    (b"\xbe\xba\xfe\xca",        "Mach-O universal binary (LE)"),
    (b"!<arch>\n",               "ar/COFF static library"),
    (b"\x00asm",                 "WebAssembly module"),
]
ARCHIVE_MAGIC = [
    (b"PK\x03\x04",              "zip archive (zip/jar/whl/xpi)"),
    (b"PK\x05\x06",              "zip archive (empty)"),
    (b"PK\x07\x08",              "zip archive (spanned)"),
    (b"\x1f\x8b",                "gzip stream (.gz/.tgz)"),
    (b"BZh",                     "bzip2 stream"),
    (b"\xfd7zXZ\x00",            "xz stream"),
    (b"\x28\xb5\x2f\xfd",        "zstd stream"),
    (b"7z\xbc\xaf\x27\x1c",      "7-zip archive"),
    (b"\x04\x22\x4d\x18",        "lz4 frame"),
    (b"GGUF",                    "GGUF model file"),
]

def classify(path, head, size):
    for magic, what in EXEC_MAGIC:
        if head.startswith(magic):
            return "executable image", what
    # MZ alone is two printable ASCII letters and appears in ordinary text.
    # Only a DOS stub with a resolvable PE\0\0 header is a Windows image.
    if head.startswith(b"MZ") and len(head) >= 0x40:
        off = int.from_bytes(head[0x3C:0x40], "little")
        if 0 < off < size:
            with open(path, "rb") as fh:
                fh.seek(off)
                if fh.read(4) == b"PE\x00\x00":
                    return "executable image", "PE-COFF (Windows .exe/.dll)"
    for magic, what in ARCHIVE_MAGIC:
        if head.startswith(magic):
            return "compressed archive", what
    if len(head) >= 262 and head[257:262] == b"ustar":
        return "compressed archive", "tar archive"
    if size > blob_max:
        with open(path, "rb") as fh:
            window = fh.read(8192)
        if b"\x00" in window:
            return "opaque blob", f"binary (NUL in first 8 KiB) and {size:,} bytes > {blob_max:,}"
    return None, None

# ── the scan: one `git ls-files -s`, then a 264-byte read per file ───────────
out = subprocess.run(["git", "-C", root, "ls-files", "-s", "-z"],
                     capture_output=True, check=True).stdout
findings, allowed, checked = [], [], 0
for rec in out.split(b"\0"):
    if not rec:
        continue
    meta, _, name = rec.partition(b"\t")
    gitmode = meta.split(b" ", 1)[0].decode()
    path = name.decode("utf-8", "surrogateescape")
    # 120000 symlink, 160000 gitlink: neither has content in this tree.
    if gitmode not in ("100644", "100755"):
        continue
    full = os.path.join(root, path)
    if not os.path.isfile(full) or os.path.islink(full):
        continue
    checked += 1
    size = os.path.getsize(full)
    with open(full, "rb") as fh:
        head = fh.read(264)
    rule, detail = classify(full, head, size)
    if rule is None:
        continue
    hit = next(((g, w, ln) for g, w, ln, rx in allow if rx.match(path)), None)
    (allowed if hit else findings).append((path, size, rule, detail, hit))

# ── stale exemptions are holes ───────────────────────────────────────────────
tracked = [p.decode("utf-8", "surrogateescape")
           for rec in out.split(b"\0") if rec
           for p in [rec.partition(b"\t")[2]]]
stale = [(g, ln) for g, w, ln, rx in allow if not any(rx.match(p) for p in tracked)]

if mode == "list":
    print(f"[tracked-artifacts] {checked} tracked regular files scanned")
    for path, size, rule, detail, hit in sorted(allowed + findings):
        tag = "ALLOWED" if hit else "REFUSED"
        print(f"  {tag:<7} {size:>12,}  {rule:<17} {path}")
        print(f"          {detail}")
        if hit:
            print(f"          allowlist:{hit[2]} {hit[0]} — {hit[1]}")
    sys.exit(0)

rc = 0
if stale:
    rc = 1
    print("[tracked-artifacts] STALE allowlist entries — they exempt nothing that exists:",
          file=sys.stderr)
    for g, ln in stale:
        print(f"  {allowlist_path}:{ln}  {g}", file=sys.stderr)
    print("  Delete them. An exemption nobody can see the subject of is a hole.",
          file=sys.stderr)

if findings:
    rc = 1
    print(f"[tracked-artifacts] REFUSED — {len(findings)} tracked file(s) are build "
          f"artifacts, not source:", file=sys.stderr)
    for path, size, rule, detail, _ in sorted(findings):
        print(f"  {path}", file=sys.stderr)
        print(f"      {size:,} bytes · {rule} · {detail}", file=sys.stderr)
    print("", file=sys.stderr)
    print("  A compiled image in the tree ships inside every tag cut from it.", file=sys.stderr)
    print("  Remove it:      git rm --cached <path> && echo <path> >> .gitignore", file=sys.stderr)
    rel = os.path.relpath(allowlist_path, root)
    print("  Or, if it truly belongs, say why it is not an artifact in", file=sys.stderr)
    shown = allowlist_path if rel.startswith("..") else rel
    print(f"  {shown} as `<glob> : <justification>`.", file=sys.stderr)

if rc == 0:
    print(f"[tracked-artifacts] clean — {checked} tracked files, "
          f"{len(allowed)} allowlisted non-source file(s), 0 unexplained artifacts")
sys.exit(rc)
'
}

# ── self-test ────────────────────────────────────────────────────────────────
selftest() {
    local tmp
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/hyp-artifact-selftest.XXXXXX")"
    trap 'rm -rf "$tmp"' RETURN

    git -C "$tmp" init -q
    git -C "$tmp" config user.email selftest@example.invalid
    git -C "$tmp" config user.name selftest

    python3 - "$tmp" <<'PY'
import os, sys, struct
d = sys.argv[1]
# A real ELF64 header, not a lookalike: e_ident + type/machine/version.
elf = bytes([0x7f]) + b"ELF" + bytes([2, 1, 1, 0]) + bytes(8)
elf += struct.pack("<HHI", 2, 0x3e, 1) + bytes(0x100)
open(os.path.join(d, "planted-elf"), "wb").write(elf)
# A real PE: DOS stub, e_lfanew at 0x3C pointing at a PE\0\0 signature.
pe = bytearray(b"MZ" + bytes(0x3a))
pe += struct.pack("<I", 0x80)
pe += bytes(0x80 - len(pe))
pe += b"PE\x00\x00" + struct.pack("<HH", 0x8664, 0) + bytes(0x100)
open(os.path.join(d, "planted.exe"), "wb").write(bytes(pe))
# Mach-O 64-bit little-endian.
open(os.path.join(d, "planted-macho"), "wb").write(b"\xcf\xfa\xed\xfe" + bytes(0x100))
# An opaque blob with no magic at all — only rule 3 can see this one.
open(os.path.join(d, "planted-blob.dat"), "wb").write(b"\x91\x00\x17" + os.urandom(400 * 1024))
# The control: large, NUL-free text, exactly the shape of a generated parser.c.
open(os.path.join(d, "generated-parser.c"), "w").write("/* generated */\n" + "int x;\n" * 200000)
PY

    git -C "$tmp" add -A
    git -C "$tmp" -c commit.gpgsign=false commit -q -m "planted artifacts"

    local empty="$tmp/empty-allowlist.txt"
    printf '# no exemptions\n' >"$empty"

    echo "--- 1. an empty allowlist must REFUSE all four planted artifacts ---"
    local out rc
    set +e
    out="$(MODE=gate scan "$tmp" "$empty" 2>&1)"
    rc=$?
    set -e
    printf '%s\n' "$out"
    [ "$rc" -ne 0 ] || {
        echo "SELFTEST FAILED: the gate passed a tree with four planted artifacts" >&2
        exit 1
    }
    local f
    for f in planted-elf planted.exe planted-macho planted-blob.dat; do
        printf '%s' "$out" | grep -q "$f" || {
            echo "SELFTEST FAILED: $f was not named in the refusal" >&2
            exit 1
        }
    done
    printf '%s' "$out" | grep -q "generated-parser.c" && {
        echo "SELFTEST FAILED: large NUL-free generated C was flagged — rule 3 is size-only" >&2
        exit 1
    }
    echo "    all four refused; the 1.4 MB generated .c control was not"

    echo "--- 2. a justified allowlist must RELEASE them (the escape works) ---"
    local full="$tmp/full-allowlist.txt"
    cat >"$full" <<'EOF'
# selftest fixtures
planted-elf : selftest fixture
planted.exe : selftest fixture
planted-macho : selftest fixture
planted-blob.dat : selftest fixture
EOF
    MODE=gate scan "$tmp" "$full"
    echo "    allowlisted tree passes"

    echo "--- 3. a PARTIAL allowlist must still refuse the one it omits ---"
    local partial="$tmp/partial-allowlist.txt"
    cat >"$partial" <<'EOF'
# selftest fixtures, minus the ELF
planted.exe : selftest fixture
planted-macho : selftest fixture
planted-blob.dat : selftest fixture
EOF
    set +e
    out="$(MODE=gate scan "$tmp" "$partial" 2>&1)"
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || {
        echo "SELFTEST FAILED: the omitted ELF was not refused" >&2
        exit 1
    }
    printf '%s' "$out" | grep -q "planted-elf" || {
        echo "SELFTEST FAILED: the refusal did not name planted-elf" >&2
        exit 1
    }
    echo "    the omitted artifact is still refused"

    echo "--- 4. a stale allowlist entry must be an error, not a shrug ---"
    local stale="$tmp/stale-allowlist.txt"
    cat >"$full.stale" <<'EOF'
# selftest fixtures
planted-elf : selftest fixture
planted.exe : selftest fixture
planted-macho : selftest fixture
planted-blob.dat : selftest fixture
build/some-binary-that-was-deleted : left behind by a cleanup
EOF
    stale="$full.stale"
    set +e
    out="$(MODE=gate scan "$tmp" "$stale" 2>&1)"
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || {
        echo "SELFTEST FAILED: a stale allowlist entry was tolerated" >&2
        exit 1
    }
    printf '%s' "$out" | grep -q "STALE" || {
        echo "SELFTEST FAILED: the stale entry was not reported as stale" >&2
        exit 1
    }
    echo "    stale exemptions fail the gate"

    echo "[tracked-artifacts] self-test PASSED (detection + escape + partial + stale)"
}

if [ "$MODE" = selftest ]; then
    selftest
    exit 0
fi

ROOT="$(git rev-parse --show-toplevel)"
scan "$ROOT" "${HYP_TRACKED_ARTIFACT_ALLOWLIST:-$ROOT/scripts/ci/tracked-artifact-allowlist.txt}"
