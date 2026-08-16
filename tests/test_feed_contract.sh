#!/usr/bin/env bash
# Contract: the properties of the feed boundary (D3/D4) that a C test cannot
# assert about itself.
#
# tests/test_feed.c covers the behaviour — ingest, atomicity, precursor
# resolution, the audit. Four of the boundary's claims are not about behaviour
# at all, and each one fails silently:
#
#   1. A SECOND ADAPTER NEEDS ONLY feed.h. This is THE test for the boundary:
#      not "does the core look clean" but "can a second adapter be written
#      without touching it". The toy adapter in tests/test_feed.c is lifted out
#      between its markers and compiled with feed.h and nothing else. Paired
#      with a negative control, because a compile check that also accepts an
#      adapter reaching into Multica proves nothing.
#   2. THE CORE SPEAKS NO FEED'S VOCABULARY. origin, thread and parent_origin
#      are opaque byte strings. The moment feed.c looked inside one it would
#      inherit that adapter's schema, and the second adapter would have to
#      translate into the first one's dialect instead of into ours. Checked with
#      a POSITIVE CONTROL — the adapter must be seen to speak that vocabulary,
#      or the grep is a guard that cannot fire and every run of it is a lie.
#   3. THE CORE PARSES NOTHING. Same argument, one level down: a separator found
#      is a format adopted.
#   4. THE CORE READS NO CLOCK. Item timestamps are the SOURCE's event time. A
#      clock read here would make ids machine-dependent, so the same feed
#      ingested on two machines would mint two ids and a sync that should be a
#      union would double the store instead. That surfaces only after two
#      machines have both ingested the same feed.
#
# The dependency direction is checked too: the core may not include an adapter's
# header. Adapters translate inward; nothing translates outward.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CORE_H="src/feed/feed.h"
CORE_C="src/feed/feed.c"
ADAPTER_H="src/feed/multica_adapter.h"
ADAPTER_C="src/feed/multica_adapter.c"
SUITE="tests/test_feed.c"

for f in "$CORE_H" "$CORE_C" "$ADAPTER_H" "$ADAPTER_C" "$SUITE"; do
    if [ ! -f "$f" ]; then
        echo "test_feed_contract: missing $f" >&2
        exit 1
    fi
done

fail() {
    echo "FAIL (feed contract): $*" >&2
    exit 1
}

# ── 1. A second adapter needs only feed.h ─────────────────────────────────────
CC_BIN="${CC:-cc}"
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    # A contract that cannot verify must not report success. "I could not check"
    # is the state this whole file exists to eliminate.
    fail "no C compiler ($CC_BIN) — cannot verify that a second adapter needs only feed.h"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Lift the toy adapter out of the suite. The markers are in tests/test_feed.c;
# if they ever go missing this fails loudly rather than checking an empty file.
python3 - "$ROOT" "$WORK" <<'PY'
import pathlib
import sys

root, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
text = (root / "tests/test_feed.c").read_text(encoding="utf-8")
begin, end = "TOY-ADAPTER-BEGIN", "TOY-ADAPTER-END"
if text.count(begin) != 1 or text.count(end) != 1:
    print("FAIL (feed contract): tests/test_feed.c must contain exactly one %s and one %s. "
          "The second-adapter proof is compiled from the region between them; without the "
          "markers there is no proof, only a claim." % (begin, end), file=sys.stderr)
    sys.exit(1)
region = text[text.index(begin) + len(begin):text.index(end)]
# Each marker sits inside a comment of its own: the region opens with the tail of
# the BEGIN comment and closes with the END comment's dangling opener. Trim both,
# or the probe is handed source that cannot compile for reasons of our making.
cut = region.find("*/")
if cut < 0:
    print("FAIL (feed contract): the %s marker is not inside a comment." % begin, file=sys.stderr)
    sys.exit(1)
region = region[cut + 2:]
cut = region.rfind("/*")
if cut < 0:
    print("FAIL (feed contract): the %s marker is not inside a comment." % end, file=sys.stderr)
    sys.exit(1)
region = region[:cut]
if len(region.strip()) < 200:
    print("FAIL (feed contract): the toy-adapter region is %d characters. That is not an "
          "adapter, so compiling it proves nothing." % len(region.strip()), file=sys.stderr)
    sys.exit(1)
(work / "toy_region.c").write_text(region, encoding="utf-8")
PY

toy_probe() {
    # $1 = extra body appended after the toy region, $2 = "accept" | "reject"
    {
        printf '#include "feed/feed.h"\n'
        printf '#include <string.h>\n'
        printf '#include <stdbool.h>\n'
        cat "$WORK/toy_region.c"
        printf '%s\n' "$1"
    } >"$WORK/probe.c"
    if "$CC_BIN" -std=c11 -Isrc -fsyntax-only "$WORK/probe.c" >"$WORK/out.txt" 2>&1; then
        result=accept
    else
        result=reject
    fi
    if [ "$result" != "$2" ]; then
        echo "--- compiler said ---" >&2
        cat "$WORK/out.txt" >&2
        fail "expected the compiler to $2 the toy adapter probe, it did $result"
    fi
}

# The claim: the toy adapter compiles against feed.h alone. No adapter header,
# no store, no test framework.
toy_probe '' accept

# The negative control for that check. If reaching for a Multica symbol ALSO
# compiled here, the acceptance above would be measuring nothing — it would mean
# the translation unit already had the whole tree in scope.
toy_probe 'hyp_feed_status_t reach_across(hyp_multica_rows_t *r, hyp_feed_source_t *s);
hyp_feed_status_t reach_across(hyp_multica_rows_t *r, hyp_feed_source_t *s) {
    return hyp_multica_messages_open(r, s);
}' reject

# ── 2, 3, 4. What the core must not contain ───────────────────────────────────
python3 - "$ROOT" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
core_c_path = root / "src/feed/feed.c"
core_h_path = root / "src/feed/feed.h"
adapter_c = (root / "src/feed/multica_adapter.c").read_text(encoding="utf-8")
adapter_h = (root / "src/feed/multica_adapter.h").read_text(encoding="utf-8")
core_c = core_c_path.read_text(encoding="utf-8")
core_h = core_h_path.read_text(encoding="utf-8")

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


core_c_code = strip_comments(core_c)

# 4. No clock. The item timestamp is the SOURCE's event time, always.
CLOCK_NEEDLES = [
    r"<time\.h>", r"\btime\s*\(", r"\bclock_gettime\b", r"\bhyp_clock_gettime\b",
    r"\bhyp_now_ms\b", r"\bhyp_now_ns\b", r"\bGetSystemTime\w*\b", r"\b__DATE__\b",
    r"\b__TIME__\b",
]
for needle in CLOCK_NEEDLES:
    for m in re.finditer(needle, core_c_code):
        line = core_c_code[:m.start()].count("\n") + 1
        problems.append(
            "feed.c:%d reads a clock (%s). An item's timestamp is the source's event "
            "time; a clock read here makes the id depend on the machine that ingested "
            "the row, and a sync that should be a union doubles the store instead."
            % (line, m.group(0)))

# 3. It parses nothing. origin/thread/parent_origin are copied and compared,
#    never looked inside.
PARSER_NEEDLES = [
    "strchr", "strrchr", "strstr", "strtok", "strpbrk", "strspn", "strcspn",
    "sscanf", "strtol", "strtoll", "strtoul", "atoi", "atol",
]
for needle in PARSER_NEEDLES:
    for m in re.finditer(r"\b%s\b" % needle, core_c_code):
        line = core_c_code[:m.start()].count("\n") + 1
        problems.append(
            "feed.c:%d calls %s. The boundary interprets no identifier: the moment it "
            "looks inside an opaque value it inherits that adapter's format, and the "
            "next adapter has to translate into it rather than into ours." % (line, needle))

# 2. No feed's vocabulary reaches the core. Same two tiers as the record
#    contract: distinctive names are banned everywhere, prose included, because
#    naming the schema in prose is how it starts; names that are also ordinary
#    English are banned in CODE only, because a guard that fires on a sentence
#    gets disabled rather than obeyed.
#    The ban list may run ahead of today's adapter — "pgvector" is here because
#    a store column is exactly the kind of thing that arrives later and reads as
#    harmless. But a term nobody has ever written cannot demonstrate that the
#    grep works, so the positive control below runs over a SUBSET: the terms
#    this adapter certainly does use. Ban broadly; prove the mechanism on what
#    is actually there.
DISTINCTIVE_VOCABULARY = [
    "multica", "task_id", "task_message", "agent_task_queue", "created_at", "pgvector",
    "parent_task_id", "issue_id", "workspace_id", "attribution_fail_closed",
]
CONTROL_VOCABULARY = [
    "multica", "task_id", "task_message", "agent_task_queue", "created_at",
    "parent_task_id", "issue_id", "workspace_id", "attribution_fail_closed",
]
GENERIC_VOCABULARY = ["squad", "seq", "issue", "github", "postgres", "jsonb"]

stray = [w for w in CONTROL_VOCABULARY if w not in DISTINCTIVE_VOCABULARY]
if stray:
    problems.append(
        "the control set names %s, which the core is not actually banned from using. A "
        "control for a rule that does not exist proves nothing." % ", ".join(sorted(stray)))

for path, text in (("feed.h", core_h), ("feed.c", core_c)):
    scanned = ((text, DISTINCTIVE_VOCABULARY), (strip_comments(text), GENERIC_VOCABULARY))
    for haystack, needles in scanned:
        for needle in needles:
            for m in re.finditer(r"\b%s\b" % re.escape(needle), haystack, re.IGNORECASE):
                line = haystack[:m.start()].count("\n") + 1
                problems.append(
                    "%s:%d names a specific feed's schema (%s). The boundary is ours; "
                    "adapters translate inward. If this vocabulary is in the core, the "
                    "second adapter has to speak the first one's dialect."
                    % (path, line, m.group(0)))

# THE POSITIVE CONTROL for the check above. A vocabulary grep that finds nothing
# because it is looking for the wrong words passes forever and means nothing —
# so the adapter, which certainly does speak this schema, must trip it.
adapter_text = adapter_c + adapter_h
unseen = [w for w in CONTROL_VOCABULARY
          if not re.search(r"\b%s\b" % re.escape(w), adapter_text, re.IGNORECASE)]
if unseen:
    problems.append(
        "the adapter does not contain %s, so the vocabulary check above cannot be shown "
        "to detect anything. Either the schema moved and this list is stale, or the "
        "guard is looking for words nobody would write. A guard that cannot fire is not "
        "a guard." % ", ".join(sorted(unseen)))

# The dependency direction. Adapters include the boundary; the boundary includes
# no adapter, ever.
for path, text in (("feed.h", core_h), ("feed.c", core_c)):
    for m in re.finditer(r'#\s*include\s+"([^"]+)"', strip_comments(text)):
        included = m.group(1)
        if "adapter" in included or "multica" in included.lower():
            line = text[:m.start()].count("\n") + 1
            problems.append(
                "%s:%d includes %s. The boundary must not know its implementations: an "
                "edge pointing that way makes the second adapter a change to the core."
                % (path, line, included))

if problems:
    for p in problems:
        print("FAIL (feed contract): %s" % p, file=sys.stderr)
    sys.exit(1)

print("feed contract: a second adapter compiles against feed.h alone (and cannot reach "
      "past it), no clock, no parsing, no feed vocabulary in the core, %d of %d banned "
      "terms confirmed detectable in the adapter"
      % (len(CONTROL_VOCABULARY), len(DISTINCTIVE_VOCABULARY)))
PY
