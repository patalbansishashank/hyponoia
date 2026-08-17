#!/usr/bin/env bash
# Contract: the properties of the transcript ingest path (D1) that a C test
# cannot assert about itself.
#
# tests/test_transcript.c covers the behaviour — the refusal, the redaction
# count, idempotence, atomicity. Four of this unit's claims are not about
# behaviour at all, and each is the kind that fails silently:
#
#   1. NOTHING ON THIS PATH CALLS hyp_record_build(). The scrub seam
#      (hyp_record_ingest_scrubbed) cannot run without a scrubber; the builder
#      can. A direct call to the builder anywhere on the transcript path is a
#      second door through which unscrubbed text becomes a PERMANENT record —
#      the id binds content, there is no update, and it syncs by union. That is
#      exactly what this path did before this gate existed. Checked over the
#      feed boundary AND the transcript module, because together they are the
#      path; with a positive control, since a grep for a symbol nobody writes
#      passes forever.
#   2. THE REDACTION COUNT CANNOT BE SUPPLIED. Neither hyp_feed_item_t (what an
#      adapter fills) nor hyp_record_ingest_input_t (what reaches construction)
#      has a member for one. A declared count is a claim the core cannot check,
#      and this is the one field where an unchecked claim is permanent — so it
#      is not validated, it is unrepresentable. The control is hyp_record_input_t,
#      which DOES carry the field: the detector must be seen to find it there.
#   3. THERE IS NO SECOND DOOR, and the check is DERIVED rather than listed:
#      any function declaration taking a feed source AND a writable record sink
#      must also take an hyp_scrub_fn. An audit that only reads takes a const
#      sink and is correctly exempt by the same rule, so there is no exemption
#      list to keep in step. A convenience entry point added later without a
#      scrubber fails here rather than being discovered later.
#   4. NO CLOCK, NO PARSING, NO FEED VOCABULARY in the transcript module — the
#      same three the record and feed contracts already hold, for the same
#      reasons, on the file that joins them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

for f in src/feed/feed.h src/feed/feed.c src/ingest/transcript_ingest.h \
         src/ingest/transcript_ingest.c src/foundation/scrub.h src/foundation/scrub.c \
         src/foundation/record.h src/feed/multica_adapter.c tests/test_transcript.c; do
    if [ ! -f "$f" ]; then
        echo "test_transcript_contract: missing $f" >&2
        exit 1
    fi
done

command -v python3 >/dev/null 2>&1 || {
    # A contract that cannot verify must not report success.
    echo "FAIL (transcript contract): python3 not found — cannot verify" >&2
    exit 1
}

python3 - "$ROOT" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
problems = []


def read(rel):
    return (root / rel).read_text(encoding="utf-8")


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


# ── 1. Nothing on the path constructs a record itself ────────────────────────

BUILDERS = ["hyp_record_build", "hyp_record_derive_id"]
PATH_FILES = ["src/feed/feed.c", "src/ingest/transcript_ingest.c"]

for rel in PATH_FILES:
    code = strip_comments(read(rel))
    for needle in BUILDERS:
        for m in re.finditer(r"\b%s\b" % needle, code):
            line = code[:m.start()].count("\n") + 1
            problems.append(
                "%s:%d calls %s. The scrub seam cannot run without a scrubber; the "
                "builder can, so a call to it here is a path on which unscrubbed feed "
                "text becomes a record whose id binds the unscrubbed bytes — permanent, "
                "because records are never rewritten, and already syncing by the time "
                "anyone notices. Construct through hyp_record_ingest_scrubbed()."
                % (rel, line, needle))

# THE POSITIVE CONTROL. The seam itself calls the builder — that is where the
# one legitimate call lives. If the grep cannot find it there, it is looking for
# a symbol nobody writes and its silence over the path above means nothing.
seam = strip_comments(read("src/foundation/scrub.c"))
if not re.search(r"\bhyp_record_build\b", seam):
    problems.append(
        "src/foundation/scrub.c does not call hyp_record_build, so the check above "
        "cannot be shown to detect anything. Either the seam changed or the guard is "
        "looking for the wrong name. A guard that cannot fire is not a guard.")

# And the seam must actually be the constructor the path uses, or the absence
# above would be satisfied by a path that builds no records at all.
if not re.search(r"\bhyp_record_ingest_scrubbed\b", strip_comments(read("src/feed/feed.c"))):
    problems.append(
        "src/feed/feed.c never calls hyp_record_ingest_scrubbed. The absence of the "
        "builder is only meaningful if the seam is what replaced it.")

# ── 2. The redaction count is unrepresentable on the way in ──────────────────


def struct_members(text, name):
    m = re.search(r"typedef struct \{(.*?)\} %s;" % re.escape(name), text, re.S)
    if not m:
        return None
    body = strip_comments(m.group(1))
    return re.findall(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;", body)


CARRIERS = [("hyp_feed_item_t", "src/feed/feed.h", "what a feed adapter fills"),
            ("hyp_record_ingest_input_t", "src/foundation/scrub.h",
             "what reaches record construction")]

for name, rel, what in CARRIERS:
    members = struct_members(read(rel), name)
    if members is None:
        problems.append(
            "%s: cannot find %s, so its field set cannot be checked and this guard "
            "would pass vacuously." % (rel, name))
        continue
    if len(members) < 4:
        problems.append(
            "%s: parsed only %d members from %s — the parse is wrong, so this guard "
            "would pass vacuously." % (rel, len(members), name))
    if "redactions" in members:
        problems.append(
            "%s carries a `redactions` member (%s). A count an adapter can DECLARE is "
            "a claim nothing verifies, and unscrubbed text labelled as cleaned is "
            "permanent. The count must be producible only by the scrubber that ran."
            % (name, what))

# THE POSITIVE CONTROL for the detector above: hyp_record_input_t is the frozen
# record contract and DOES carry the field. Finding it there is what shows the
# struct parse and the membership test both work.
control = struct_members(read("src/foundation/record.h"), "hyp_record_input_t")
if not control or "redactions" not in control:
    problems.append(
        "hyp_record_input_t (src/foundation/record.h) does not parse as carrying "
        "`redactions`, so the absence checks above are not evidence of anything — the "
        "struct parser or the membership test is broken.")

# ── 3. No second door, derived from the headers ──────────────────────────────

DECL = re.compile(r"\b(hyp_[A-Za-z0-9_]+)\s*\(([^;{)]*(?:\([^)]*\)[^;{)]*)*)\)\s*;", re.S)


def scrubless_doors(text):
    """Declarations taking a feed source AND a writable record sink but no
    scrubber. The rule is derived from the signature: a surface that can WRITE
    records from a source must gate on a scrubber; one that only reads a const
    sink (the completeness audit) is exempt by the same rule rather than by an
    exemption list."""
    found = []
    for m in DECL.finditer(strip_comments(text)):
        name, params = m.group(1), m.group(2)
        if "hyp_feed_source_t" not in params:
            continue
        sinks = [p for p in re.findall(r"[^,]+", params)
                 if ("hyp_record_set_t" in p or "hyp_record_store_t" in p)]
        writable = [p for p in sinks if "const" not in p]
        if not writable:
            continue
        if "hyp_scrub_fn" not in params:
            found.append(name)
    return found

for rel in ["src/feed/feed.h", "src/ingest/transcript_ingest.h"]:
    for name in scrubless_doors(read(rel)):
        problems.append(
            "%s declares %s(), which takes a feed source and a writable record sink "
            "but no hyp_scrub_fn. That is a second door: an ingest surface reachable "
            "without a scrubber. Every writing entry point takes the scrubber, so "
            "there is nothing to remember and nothing to enforce by review."
            % (rel, name))

# THE POSITIVE CONTROL for that derivation. A synthetic scrubberless door must
# be detected, or the clean result above only means the matcher never matched.
PROBE = ("hyp_feed_status_t hyp_feed_ingest_unchecked(hyp_feed_source_t *src, "
         "hyp_record_set_t *store);\n")
if scrubless_doors(read("src/feed/feed.h") + PROBE) != ["hyp_feed_ingest_unchecked"]:
    problems.append(
        "the scrubberless-door detector does not flag a synthetic scrubberless door, "
        "so its silence over the real headers is not evidence. A guard that cannot "
        "fire is not a guard.")

# And its negative control: the audit reads a const sink and must stay exempt,
# or the rule would be "no surface may take a source", which is not the rule.
AUDIT_PROBE = ("hyp_feed_status_t hyp_feed_audit_probe(hyp_feed_source_t *src, "
               "const hyp_record_set_t *store);\n")
if scrubless_doors(AUDIT_PROBE):
    problems.append(
        "the scrubberless-door detector flags a read-only surface over a const sink. "
        "The rule is about surfaces that WRITE records; widening it to every surface "
        "taking a source would make the guard wrong rather than strict.")

# ── 4. What the transcript module must not contain ───────────────────────────

impl = strip_comments(read("src/ingest/transcript_ingest.c"))
header = read("src/ingest/transcript_ingest.h")

CLOCK_NEEDLES = [r"<time\.h>", r"\btime\s*\(", r"\bclock_gettime\b", r"\bhyp_clock_gettime\b",
                 r"\bhyp_now_ms\b", r"\bhyp_now_ns\b", r"\bGetSystemTime\w*\b",
                 r"\bhyp_record_wall_clock_ms\b", r"\b__DATE__\b", r"\b__TIME__\b"]
for needle in CLOCK_NEEDLES:
    for m in re.finditer(needle, impl):
        line = impl[:m.start()].count("\n") + 1
        problems.append(
            "transcript_ingest.c:%d reads a clock (%s). An item's timestamp is the "
            "source's event time; a clock read here makes the id depend on the machine "
            "that ingested the row, so re-ingest stops being idempotent and a sync that "
            "should be a union doubles the store." % (line, m.group(0)))

PARSER_NEEDLES = ["strchr", "strrchr", "strstr", "strtok", "strpbrk", "strspn", "strcspn",
                  "sscanf", "strtol", "strtoll", "strtoul", "atoi", "atol"]
for needle in PARSER_NEEDLES:
    for m in re.finditer(r"\b%s\b" % needle, impl):
        line = impl[:m.start()].count("\n") + 1
        problems.append(
            "transcript_ingest.c:%d calls %s. origin and thread are opaque: a separator "
            "found is a format adopted, and the next adapter would have to translate "
            "into the first one's dialect instead of into ours." % (line, needle))

DISTINCTIVE_VOCABULARY = ["multica", "task_id", "task_message", "agent_task_queue",
                          "created_at", "parent_task_id", "issue_id", "workspace_id",
                          "attribution_fail_closed", "pgvector"]
GENERIC_VOCABULARY = ["squad", "seq", "issue", "github", "postgres", "jsonb"]

for rel, text in (("transcript_ingest.h", header),
                  ("transcript_ingest.c", read("src/ingest/transcript_ingest.c"))):
    for haystack, needles in ((text, DISTINCTIVE_VOCABULARY),
                              (strip_comments(text), GENERIC_VOCABULARY)):
        for needle in needles:
            for m in re.finditer(r"\b%s\b" % re.escape(needle), haystack, re.IGNORECASE):
                line = haystack[:m.start()].count("\n") + 1
                problems.append(
                    "%s:%d names a specific feed's schema (%s). Adapters translate "
                    "inward; if this vocabulary reaches the ingest core, the second "
                    "adapter has to speak the first one's dialect."
                    % (rel, line, m.group(0)))

# The positive control for the vocabulary grep, same as the feed contract's:
# the adapter certainly speaks this schema and must trip the same list.
adapter = read("src/feed/multica_adapter.c") + read("src/feed/multica_adapter.h")
unseen = [w for w in DISTINCTIVE_VOCABULARY[:-1]
          if not re.search(r"\b%s\b" % re.escape(w), adapter, re.IGNORECASE)]
if unseen:
    problems.append(
        "the adapter does not contain %s, so the vocabulary check cannot be shown to "
        "detect anything." % ", ".join(sorted(unseen)))

if problems:
    for p in problems:
        print("FAIL (transcript contract): %s" % p, file=sys.stderr)
    sys.exit(1)

print("transcript contract: no record builder on the path (seam confirmed reachable), "
      "no redaction count on hyp_feed_item_t or hyp_record_ingest_input_t (detector "
      "confirmed on hyp_record_input_t), no scrubberless ingest door in 2 headers "
      "(detector confirmed both directions), no clock, no parsing, no feed vocabulary")
PY
