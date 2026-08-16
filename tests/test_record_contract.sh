#!/usr/bin/env bash
# Contract: the properties of the record shape (C2) that a C test cannot assert
# about itself.
#
# tests/test_record.c covers the behaviour — ids, refusals, the union. Four of
# the contract's claims are not about behaviour at all, and each one is the kind
# that fails silently:
#
#   1. IMMUTABILITY IS A COMPILE ERROR. Every member of hyp_record_t is const,
#      so `rec->author = x` and `*rec = other` must not compile. A runtime test
#      cannot check that, and "we all agreed not to mutate it" is not a
#      mechanism. Checked with a positive control, because a compile-rejection
#      test that also rejects valid code proves nothing.
#   2. CONSTRUCTION READS NO CLOCK. A captured timestamp makes an id depend on
#      the machine that built it, so the same event on two machines gets two
#      ids, and a sync that should be a union doubles the store instead. That
#      surfaces only after two machines have both ingested the same feed. The
#      wall clock lives in its own translation unit so the absence is a
#      structural property of record.c rather than a habit. Checked against the
#      COMPILER'S VIEW — the emitted assembly, the preprocessed translation
#      unit, and the include set — never against record.c's own text, which can
#      only show what someone chose to spell in it.
#   3. THE CORE PARSES NOTHING AND SPEAKS NO FEED'S VOCABULARY. anchor, origin
#      and thread are opaque byte strings. The moment the core looked inside one
#      it would inherit that producer's schema, and the second adapter would
#      have to translate into the first adapter's dialect instead of into ours.
#      The test for the boundary is that a second adapter can be written without
#      touching the core.
#   4. THE ID PREIMAGE IS WHAT THE HEADER SAYS IT IS. The golden vector in
#      test_record.c is recomputed here by a second implementation of the
#      encoding, written from the prose. A golden value produced only by the
#      code under test pins whatever that code does, bug included.
#
# Plus one guard against the field set drifting away from its controls: every
# member of hyp_record_input_t must have a control in test_record.c proving the
# id changes when that field changes. The field list is READ FROM THE HEADER, so
# a tenth field cannot be added without one — and the control is read from the
# ASSERTIONS, with comments stripped first, so deleting a control and leaving
# its comment behind fails here instead of reporting nine fields controlled.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

HEADER="src/foundation/record.h"
IMPL="src/foundation/record.c"
CLOCK="src/foundation/record_clock.c"
SUITE="tests/test_record.c"

for f in "$HEADER" "$IMPL" "$CLOCK" "$SUITE"; do
    if [ ! -f "$f" ]; then
        echo "test_record_contract: missing $f" >&2
        exit 1
    fi
done

fail() {
    echo "FAIL (record contract): $*" >&2
    exit 1
}

# ── 1. Immutability is a compile error ────────────────────────────────────────
CC_BIN="${CC:-cc}"
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    # A contract that cannot verify must not report success. "I could not check"
    # is the state this whole file exists to eliminate.
    fail "no C compiler ($CC_BIN) — cannot verify that records are unassignable"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

try_compile() {
    # $1 = body, $2 = "accept" | "reject"
    cat >"$WORK/probe.c" <<EOF
#include "foundation/record.h"
$1
EOF
    if "$CC_BIN" -std=c11 -Isrc -fsyntax-only "$WORK/probe.c" >"$WORK/out.txt" 2>&1; then
        result=accept
    else
        result=reject
    fi
    if [ "$result" != "$2" ]; then
        echo "--- probe ---" >&2
        cat "$WORK/probe.c" >&2
        echo "--- compiler said ---" >&2
        cat "$WORK/out.txt" >&2
        fail "expected the compiler to $2 this, it did $result"
    fi
}

# The positive control first: reading a record must compile. Without it, a
# header that failed to compile at all would satisfy every check below.
try_compile 'const char *read_author(const hyp_record_t *r) { return r->author; }
int64_t read_time(const hyp_record_t *r) { return r->timestamp_ms; }' accept

# Now the mutations that must not compile.
try_compile 'void mutate_pointer_member(hyp_record_t *r) { r->author = "someone else"; }' reject
try_compile 'void mutate_scalar_member(hyp_record_t *r) { r->timestamp_ms = 1; }' reject
try_compile 'void overwrite_whole_record(hyp_record_t *a, const hyp_record_t *b) { *a = *b; }' reject
try_compile 'void mutate_id(hyp_record_t *r) { r->id = "0"; }' reject

# ── The two artefacts the clock guard reads ───────────────────────────────────
#
# NOT record.c's own text. A guard that scans source text can only find what
# someone chose to spell there, and every route into a clock that matters is a
# route that does not appear in this file's text:
#
#   * a macro from another header — `#define NOW hyp_record_wall_clock_ms()`,
#     and record.c contains the four letters `NOW`;
#   * an include of <time.h> reached THROUGH record.h, so record.c's own claim
#     that "there is no <time.h> here" is true of its text and false of its
#     translation unit;
#   * any clock spelled some way this file's needle list did not anticipate.
#
# So the guard reads what the COMPILER read (`cc -E`) and what the compiler
# EMITTED (`cc -S`). The assembly is the part that cannot be talked around: a
# call to a clock is a symbol reference in the object regardless of which macro,
# typedef, header or spelling produced it, and record.c's whole claim is that no
# such call exists. Both are produced by the same compiler this script already
# requires, so no new tool is a new way to be unable to check.
PP="$WORK/record.i"
ASM="$WORK/record.s"
if ! "$CC_BIN" -std=c11 -Isrc -E "$IMPL" >"$PP" 2>"$WORK/cpp.txt"; then
    cat "$WORK/cpp.txt" >&2
    fail "cannot preprocess $IMPL — the clock guard reads the preprocessed translation unit, and a guard that cannot verify must refuse rather than pass"
fi
if ! "$CC_BIN" -std=c11 -Isrc -S -o "$ASM" "$IMPL" 2>"$WORK/asm.txt"; then
    cat "$WORK/asm.txt" >&2
    fail "cannot compile $IMPL to assembly — the clock guard reads the emitted symbols, and a guard that cannot verify must refuse rather than pass"
fi

# ── 2, 3. What record.c must not contain ──────────────────────────────────────
python3 - "$ROOT" "$PP" "$ASM" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
impl_path = root / "src/foundation/record.c"
header_path = root / "src/foundation/record.h"
suite_path = root / "tests/test_record.c"
impl = impl_path.read_text(encoding="utf-8")
header = header_path.read_text(encoding="utf-8")
suite = suite_path.read_text(encoding="utf-8")

problems = []


def strip_comments(text):
    """Blank out C comments, preserving line numbers. A contract a COMMENT can
    trip is a false guard, and one a comment can SATISFY is worse."""
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


impl_code = strip_comments(impl)

# ── 2. No clock ───────────────────────────────────────────────────────────────
#
# record_clock.c is the only translation unit allowed one, so the absence here
# is a property of the build rather than of anyone's discipline. Three checks,
# because each covers a route the others cannot see:
#
#   2a  the ASSEMBLY record.c compiles to — a clock CALL, however it was
#       spelled, reached or hidden behind a macro;
#   2b  the record.c regions of the preprocessed TU — the text the compiler
#       actually saw for this file, after macro expansion;
#   2c  the INCLUDE SET of that TU — a header of ours that pulls <time.h> in,
#       which is the transitive reach a text scan of record.c cannot observe.
#
# hyp_record_wall_clock_ms is on the list and is the one that matters most: it
# is the repo's OWN clock, declared in record.h, one line above the code under
# test, and the first thing anyone would reach for. A guard that catches
# time(NULL) and misses the clock it ships beside is a guard aimed at the
# wrong file.
pp_path = pathlib.Path(sys.argv[2])
asm_path = pathlib.Path(sys.argv[3])
pp_text = pp_path.read_text(encoding="utf-8", errors="replace")
asm_text = asm_path.read_text(encoding="utf-8", errors="replace")

CLOCK_NEEDLES = [
    r"\btime\b",
    r"\bclock\b",
    r"\bclock_gettime\b",
    r"\bgettimeofday\b",
    r"\bftime\b",
    r"\btimespec_get\b",
    r"\btimespec_getres\b",
    r"\bmktime\b",
    r"\blocaltime\w*\b",
    r"\bgmtime\w*\b",
    r"\bhyp_clock_gettime\b",
    r"\bhyp_now_ms\b",
    r"\bhyp_now_ns\b",
    r"\bhyp_record_wall_clock_ms\b",
    r"\bGetSystemTime\w*\b",
    r"\bGetLocalTime\b",
    r"\bGetTickCount\w*\b",
    r"\bQueryPerformanceCounter\b",
    r"\bQueryPerformanceFrequency\b",
    r"\b__DATE__\b",
    r"\b__TIME__\b",
    r"\b__TIMESTAMP__\b",
]
CLOCK_WHY = ("Construction must never capture a timestamp: it would make an id "
             "depend on the machine that built it, and sync would duplicate "
             "rather than unify. The wall clock belongs in record_clock.c.")

# 2a. The emitted symbols. This is the check that cannot be talked around.
for needle in CLOCK_NEEDLES:
    for m in re.finditer(needle, asm_text):
        problems.append(
            "record.c compiles to a reference to %s. %s Found in the ASSEMBLY, so "
            "no spelling, macro or transitive include hides it." % (m.group(0), CLOCK_WHY))
        break  # one report per needle: the second occurrence is the same fact

# 2b. The preprocessed TU, restricted to the regions that came from record.c
#     itself. Restricted on purpose, in both directions: libc's headers declare
#     time() and record.h DECLARES hyp_record_wall_clock_ms — neither is a
#     finding, because the contract is that record.c never CALLS one. What this
#     adds over a text scan is macro expansion: whatever record.c wrote, this is
#     what the compiler read.
LINEMARKER = re.compile(r'^#\s+\d+\s+"([^"]*)"', re.M)
own_regions = []
includes_seen = set()
marks = [(m.start(), m.group(1).replace("\\\\", "/").replace("\\", "/"))
         for m in LINEMARKER.finditer(pp_text)]
for idx, (pos, name) in enumerate(marks):
    end = marks[idx + 1][0] if idx + 1 < len(marks) else len(pp_text)
    includes_seen.add(name)
    if pathlib.PurePosixPath(name).name == impl_path.name:
        own_regions.append(pp_text[pos:end])
own = "\n".join(own_regions)
if "hyp_record_build" not in own:
    problems.append(
        "the preprocessed translation unit does not attribute hyp_record_build to "
        "record.c — the line-marker scan is not reading the file it claims to, so "
        "this guard would pass vacuously")
for needle in CLOCK_NEEDLES:
    for m in re.finditer(needle, own):
        problems.append(
            "record.c names %s. %s Found AFTER preprocessing, so a macro defined "
            "in another header does not hide it." % (m.group(0), CLOCK_WHY))
        break

# 2c. No header of ours reaches a clock header. record.c's comment claims
#     "there is no <time.h>"; without this, that claim is only true of its text.
CLOCK_HEADERS = re.compile(r'#\s*include\s*[<"](?:sys/)?(?:time\.h|timeb\.h|times\.h)[>"]')
root_abs = root.resolve()


def ours(name):
    """The repo's own files among the TU's includes, whatever spelling cpp used
    for them. Returns the resolved path or None; libc's headers are its own
    business, and 2a already holds record.c to never CALL what they declare."""
    if not name or name.startswith("<"):
        return None
    for cand in ((root / name), pathlib.Path(name)):
        try:
            resolved = cand.resolve()
        except OSError:
            continue
        if not resolved.is_file():
            continue
        try:
            resolved.relative_to(root_abs)
        except ValueError:
            continue
        return resolved
    return None


# record.h and record.c are checked by name as well as through the include set:
# the set is spelled by the compiler and the two files that matter must be
# checked on every platform, not only where the spelling resolved.
checked = set()
for name in sorted(includes_seen) + [str(impl_path), str(header_path)]:
    resolved = ours(name)
    if resolved is None or resolved in checked:
        continue
    checked.add(resolved)
    try:
        src = resolved.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        problems.append(
            "cannot read %s, which record.c's translation unit includes — a guard "
            "that cannot verify must refuse, not pass (%s)" % (name, exc))
        continue
    if resolved.name == "record_clock.c":
        continue  # the one translation unit allowed a clock
    hit = CLOCK_HEADERS.search(strip_comments(src))
    if hit:
        problems.append(
            "%s includes a clock header (%s) and record.c reaches it. %s The "
            "absence has to hold for the translation unit, not for one file's text."
            % (resolved.relative_to(root_abs), hit.group(0).strip(), CLOCK_WHY))
if impl_path.resolve() not in checked or header_path.resolve() not in checked:
    problems.append(
        "the clock-header check did not reach record.c and record.h — a guard that "
        "cannot verify must refuse, not pass")

# 3a. It parses nothing. anchor/origin/thread are opaque; the encoding is
#     length-prefixed precisely so no separator ever has to be found.
PARSER_NEEDLES = [
    "strchr", "strrchr", "strstr", "strtok", "strpbrk", "strspn", "strcspn",
    "sscanf", "strtol", "strtoll", "strtoul", "atoi", "atol",
]
for needle in PARSER_NEEDLES:
    for m in re.finditer(r"\b%s\b" % needle, impl_code):
        line = impl_code[:m.start()].count("\n") + 1
        problems.append(
            "record.c:%d calls %s. The record shape interprets no field: the "
            "moment it looks inside an opaque value it inherits that producer's "
            "format, and the next adapter has to translate into it." % (line, needle))

# 3b. No feed's vocabulary reaches the core. These are the column and table
#     names of the first feed this system will adapt — the point is that a
#     SECOND adapter must be writable without editing these files, which cannot
#     be true if the first one's schema is spelled out in them. Comments are
#     included on purpose: naming the schema in prose is how it starts.
#     Distinctive names are banned everywhere, prose included. Names that are
#     also ordinary English (issue, seq, squad) are banned in CODE only: a guard
#     that fires on a sentence gets disabled rather than obeyed, and this one has
#     to survive years of editing to be worth anything.
DISTINCTIVE_VOCABULARY = ["multica", "task_id", "task_message", "created_at", "pgvector"]
GENERIC_VOCABULARY = ["squad", "seq", "issue", "github", "workspace_id"]

clock_src = (root / "src/foundation/record_clock.c").read_text(encoding="utf-8")
for path, text in (("record.h", header), ("record.c", impl), ("record_clock.c", clock_src)):
    scanned = ((text, DISTINCTIVE_VOCABULARY), (strip_comments(text), GENERIC_VOCABULARY))
    for haystack, needles in scanned:
        for needle in needles:
            for m in re.finditer(r"\b%s\b" % re.escape(needle), haystack, re.IGNORECASE):
                line = haystack[:m.start()].count("\n") + 1
                problems.append(
                    "%s:%d names a specific feed's schema (%s). The record shape is "
                    "ours; adapters translate inward. If this vocabulary is in the "
                    "core, the second adapter has to speak the first one's dialect."
                    % (path, line, m.group(0)))

# 4. Every field of the record has a control proving the id notices it. The list
#    is derived from the header, never typed here — an enumeration is only as
#    good as its enumeration, and the field that gets forgotten is never a
#    harmless one.
struct_src = re.search(r"typedef struct \{(.*?)\} hyp_record_input_t;", header, re.S)
if not struct_src:
    problems.append("record.h: cannot find hyp_record_input_t — the field list is "
                    "derived from it, so the controls below cannot be checked")
    fields = []
else:
    body = strip_comments(struct_src.group(1))
    fields = re.findall(r"([A-Za-z_]\w*)\s*;", body)

if len(fields) < 5:
    problems.append("record.h: parsed only %d fields from hyp_record_input_t — the "
                    "parse is wrong, so this guard would pass vacuously" % len(fields))

#    And the control is the ASSERTIONS, never a comment naming the field. The
#    first version of this check looked for a marker comment, which meant the
#    four assertion lines under `/* id-sensitivity: anchor */` could be deleted,
#    the comment left behind, and this script would still print "9 fields
#    controlled" and exit 0. A contract a comment can trip is a false guard; one
#    a comment can SATISFY is worse than none, and that is what it was. So the
#    control test's body is parsed WITH COMMENTS STRIPPED, and a field counts as
#    controlled only when some chunk of it perturbs exactly that field, derives
#    an id from the perturbed input, and asserts the id moved.
control = re.search(r"TEST\(record_id_commits_to_every_field\)\s*\{(.*?)\n\}", suite, re.S)
controlled = {}
if not control:
    problems.append(
        "tests/test_record.c: cannot find TEST(record_id_commits_to_every_field) — "
        "the per-field controls are read out of its body, so without it this guard "
        "would pass vacuously")
else:
    cbody = strip_comments(control.group(1))
    # Each control resets the variant to the baseline, changes ONE field, and
    # asserts the derived id differs. Splitting on the reset gives one chunk per
    # control; a chunk touching two fields proves neither, so it is not counted.
    for chunk in re.split(r"\bv\s*=\s*base\s*;", cbody)[1:]:
        touched = set(re.findall(r"\bv\.([A-Za-z_]\w*)\s*=[^=]", chunk))
        if len(touched) != 1:
            continue
        derived = re.search(r"hyp_record_derive_id\s*\(\s*&v\s*,\s*id\s*\)", chunk)
        asserted = re.search(r"ASSERT_STR_NEQ\s*\(\s*id\s*,\s*base_id\s*\)", chunk)
        if derived and asserted:
            controlled[touched.pop()] = True
    if not controlled:
        problems.append(
            "tests/test_record.c: parsed 0 per-field controls out of "
            "record_id_commits_to_every_field — the parse is wrong, so this guard "
            "would pass vacuously")

for field in fields:
    if field not in controlled:
        problems.append(
            "tests/test_record.c has no ASSERTED control for '%s': "
            "record_id_commits_to_every_field must perturb v.%s from the baseline, "
            "derive an id, and ASSERT_STR_NEQ it against base_id. Every field is in "
            "the id preimage; a field with no control is a field nothing proves the "
            "id commits to, and a field the id misses is a collision the union has "
            "to resolve by picking a payload." % (field, field))

# 5. The golden id, recomputed from the spec by a second implementation.
import hashlib
import struct


def enc_str(value):
    if value is None:
        return b"N"
    raw = value.encode("utf-8")
    return b"S" + struct.pack(">Q", len(raw)) + raw


def enc_i64(value):
    return b"I" + struct.pack(">Q", value & 0xFFFFFFFFFFFFFFFF)


def enc_u32(value):
    return b"U" + struct.pack(">Q", value)


# The record pinned by test_record.c's golden vector.
preimage = enc_str("hyp-record-v1")
preimage += enc_str("kind") + enc_str("message")
preimage += enc_str("author") + enc_str("agent:golden")
preimage += enc_str("timestamp_ms") + enc_i64(1700000000000)
preimage += enc_str("content") + enc_str("hello")
preimage += enc_str("anchor") + enc_str(None)
preimage += enc_str("origin") + enc_str(None)
preimage += enc_str("thread") + enc_str(None)
preimage += enc_str("parent") + enc_str(None)
preimage += enc_str("redactions") + enc_u32(0)
expected = hashlib.sha256(preimage).hexdigest()

# PINNED means asserted, not mentioned. Searched with comments stripped, and
# required to sit inside an ASSERT_STR_EQ against a derived id: a bare substring
# search over the file is satisfied by the hash appearing in a comment, which is
# the same defect the field controls above had — the golden vector's ASSERT could
# be deleted, the hash left behind in the prose explaining it, and this guard
# would still report the id pinned.
suite_code = strip_comments(suite)
golden_assert = re.search(
    r"ASSERT_STR_EQ\s*\(\s*id\s*,\s*\"%s\"\s*\)" % re.escape(expected), suite_code)
if not golden_assert:
    problems.append(
        "tests/test_record.c does not ASSERT the golden id %s (searched with "
        "comments stripped, for an ASSERT_STR_EQ against a derived id). Either the "
        "canonical encoding changed — which gives every record ever written a new "
        "id — or the two descriptions of it have drifted apart, or the assertion "
        "was removed and only the prose about it remains." % expected)

if problems:
    for p in problems:
        print("FAIL (record contract): %s" % p, file=sys.stderr)
    sys.exit(1)

print("record contract: immutability, no clock (assembly + preprocessed TU + "
      "include set), no parsing, no feed vocabulary, %d/%d fields controlled by "
      "assertion, golden id %s" % (len(controlled), len(fields), expected[:12]))
PY
