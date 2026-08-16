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
#      structural property of record.c rather than a habit.
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
# a tenth field cannot be added without one.
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

# ── 2, 3. What record.c must not contain ──────────────────────────────────────
python3 - "$ROOT" <<'PY'
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

# 2. No clock. record_clock.c is the only translation unit allowed one, so the
#    absence here is a property of the build rather than of anyone's discipline.
CLOCK_NEEDLES = [
    r"<time\.h>",
    r"\btime\s*\(",
    r"\bclock_gettime\b",
    r"\bhyp_clock_gettime\b",
    r"\bhyp_now_ms\b",
    r"\bhyp_now_ns\b",
    r"\bGetSystemTime\w*\b",
    r"\b__DATE__\b",
    r"\b__TIME__\b",
]
for needle in CLOCK_NEEDLES:
    for m in re.finditer(needle, impl_code):
        line = impl_code[:m.start()].count("\n") + 1
        problems.append(
            "record.c:%d reads a clock (%s). Construction must never capture a "
            "timestamp: it would make an id depend on the machine that built it, "
            "and sync would duplicate rather than unify. The wall clock belongs "
            "in record_clock.c." % (line, m.group(0)))

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

for field in fields:
    marker = "id-sensitivity: %s" % field
    if marker not in suite:
        problems.append(
            "tests/test_record.c has no control marked '%s'. Every field is in the "
            "id preimage; a field with no control is a field nothing proves the id "
            "commits to, and a field the id misses is a collision the union has to "
            "resolve by picking a payload." % marker)

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

if expected not in suite:
    problems.append(
        "tests/test_record.c does not pin the golden id %s. Either the canonical "
        "encoding changed — which gives every record ever written a new id — or "
        "the two descriptions of it have drifted apart." % expected)

if problems:
    for p in problems:
        print("FAIL (record contract): %s" % p, file=sys.stderr)
    sys.exit(1)

print("record contract: immutability, no clock, no parsing, no feed vocabulary, "
      "%d fields controlled, golden id %s" % (len(fields), expected[:12]))
PY
