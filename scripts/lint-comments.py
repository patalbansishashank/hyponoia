#!/usr/bin/env python3
"""Comment lint: reject history-shaped prose in NEW comments (Track E, unit E1).

WHY THIS EXISTS. A comment that says *when* something happened, *which commit*
did it, *which plan section* asked for it, or *what it used to do* is a claim
about history — and the comment is the one place that claim can never be
checked or updated. Git already owns when and which commit; the plan renumbers
its sections; "now returns X" stops being news the day after it lands and
becomes a lie the day the behaviour changes again. The durable home for that
prose is the decision store (unit E2 relocates it); the comment should state
what IS true, in the present tense, with the reasoning.

FORWARD-ONLY, DELIBERATELY. This gate examines only lines ADDED relative to a
base ref — the model the pre-push hook uses. The existing corpus is not
cleaned and must never fail this gate: this plan has twice been burned by
mechanical bulk edits that could not see inside string literals, and forcing
anyone who touches an old file to launder its old comments would be the same
bulk rewrite on an instalment plan. Line-granular, not file-granular: editing
one line of a file with a dated comment elsewhere does not fire. The existing
offenders are E2's migration inventory, which `--all` prints.

COMMENTS ONLY, STRUCTURALLY. The matcher runs over a view of the file in which
everything except comment text is blanked — produced by a small C tokenizer
(line/block comments, string and char literals, raw strings, escapes,
backslash-newline continuation), never by grepping raw source. Both historical
burns were tools that treated a string literal containing a date the same as a
comment containing one. A test string, a log format, a golden hash in an
assertion are all code, and this lint cannot see them by construction.

Exit codes: 0 clean (or report mode), 1 findings in gate mode, 2 cannot verify
(no repo, no resolvable base). "Could not check" refuses; it never passes.

Usage:
  scripts/lint-comments.py [--base REF]   gate: new comment lines vs REF
                                          (default: origin/dev, then dev)
  scripts/lint-comments.py --all          report: whole tracked tree, always
                                          exits 0 — E2 inventory, never a gate
"""

import argparse
import os
import re
import subprocess
import sys

# ── What counts as a lintable file ───────────────────────────────────────────
#
# C-family sources we author. Vendored trees are pinned upstream code, grammar
# shims and generated/ trees are machine-written, and tests/fixtures holds
# foreign sample code whose comments are test DATA — none of those comments are
# ours to gate.
SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".cc", ".cxx")
EXCLUDED_SEGMENTS = ("vendored", "generated", "build", "node_modules")
EXCLUDED_PREFIXES = ("tests/fixtures/", "internal/hyp/grammar_")


def is_candidate(path):
    if not path.endswith(SUFFIXES):
        return False
    parts = path.split("/")
    if any(seg in EXCLUDED_SEGMENTS for seg in parts):
        return False
    return not path.startswith(EXCLUDED_PREFIXES)


# ── The pattern table ────────────────────────────────────────────────────────
#
# Every entry says why it is banned, because a lint that cannot explain itself
# gets exported around rather than obeyed. Matching is case-sensitive unless a
# pattern says otherwise; each regex runs over comment text only.

# Capitalised month names, full and abbreviated. Capitalised on purpose: "may
# 2048 bytes" is prose about a buffer, "May 2026" is a date.
_MONTHS = (r"(?:Jan(?:uary)?|Feb(?:ruary)?|Mar(?:ch)?|Apr(?:il)?|May|"
           r"Jun(?:e)?|Jul(?:y)?|Aug(?:ust)?|Sep(?:t(?:ember)?)?|"
           r"Oct(?:ober)?|Nov(?:ember)?|Dec(?:ember)?)")

# The verbs that make "used to <verb>" a statement about repo history. The
# post-filter below removes the purpose sense ("is used to build ...").
_USED_TO = r"\bused to (?:be|have|do|return|take|live|run|mean|say|contain)\b"

# The verbs that make "now <verb>s" a change announcement rather than
# execution-time narration ("now we flush the buffer" stays legal). The
# (?!-) keeps hyphenated states out: "is now read-only" is a phase of the
# algorithm, not a release note.
_NOW = (r"\bnow (?:returns?|takes?|uses?|does|lives?|reads?|writes?|"
        r"reports?|accepts?|emits?|contains?|requires?|defaults?|ships?|"
        r"supports?|handles?)\b(?!-)")


def _not_state_now(view, m):
    """Keep 'callers now use wide buffers'; drop 'token_map is now read-...'.
    A be-verb before 'now' marks execution-time state narration, tuned on the
    real tree: the changelog sense reads '<subject> now <verb>s'."""
    before = view[:m.start()].rstrip()
    last = before.split()[-1].lower() if before.split() else ""
    return last not in {"is", "are", "was", "were", "be", "been", "being",
                        "becomes", "become", "became"}


def _not_purpose_used_to(view, m):
    """Keep 'this used to return NULL'; drop 'is used to return the arena'.
    The purpose sense is always '<be/get> used to <verb>'."""
    before = view[:m.start()].rstrip()
    last = before.split()[-1].lower() if before.split() else ""
    return last not in {"is", "are", "was", "were", "be", "been", "being",
                        "am", "get", "gets", "got", "getting"}


def _looks_like_commit_ref(view, m):
    """A commit ref has digits AND letters, standing alone. Tuned on the real
    tree: a decimal ('10000000') or an all-letter hexish word ('deadbeef',
    'effaced') is a constant or English, and hex glued to '-', '@' or '#' is
    a hostname segment, a model@revision, a UUID piece or a colour — found as
    'my-svc-ab12cd34ef-uc.a.run.app' and 'voyage-4-nano-Q8_0@75e62c7dba5e',
    both format EXAMPLES a lint must not reject."""
    tok = m.group(0)
    if not (any(c.isdigit() for c in tok) and any(c in "abcdef" for c in tok)):
        return False
    before = view[m.start() - 1] if m.start() > 0 else " "
    after = view[m.end()] if m.end() < len(view) else " "
    return before not in "-@#" and after not in "-@"


# (id, regex, post-filter or None, why)
PATTERNS = [
    ("date-iso",
     re.compile(r"\b20\d{2}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12]\d|3[01])\b"),
     None,
     # A calendar date pins the comment to a moment. Git records when; a date
     # in prose goes stale silently and cannot be blamed or bisected.
     "a calendar date; git owns 'when', and dated prose belongs in the "
     "decision store"),

    ("date-prose",
     re.compile(r"\b(?:\d{1,2}(?:st|nd|rd|th)?\s+)?" + _MONTHS +
                r"\.?,?\s+(?:\d{1,2}(?:st|nd|rd|th)?,?\s+)?20\d{2}\b"),
     None,
     # Same claim as date-iso, spelled out ("frozen May 2026", "16 Aug 2025").
     "a prose date; git owns 'when', and dated prose belongs in the "
     "decision store"),

    ("commit-sha",
     # No hex neighbour on either side, so a 64-hex content hash (a golden
     # value in an assertion comment) is NOT a commit ref and never matches;
     # '0x...' constants die on the same lookbehind (the 'x').
     re.compile(r"(?<![0-9a-zA-Z_])[0-9a-f]{7,40}(?![0-9a-zA-Z_])"),
     _looks_like_commit_ref,
     # A SHA in a comment dangles after rebase or squash and nothing checks
     # it; `git log`/blame resolve history, the decision store keeps the why.
     "a commit SHA; the ref belongs to git and the reasoning to the "
     "decision store"),

    ("section-ref",
     re.compile(r"§\s*\d+(?:\.\d+)*"),
     None,
     # Plan sections renumber; the pointer rots while looking precise.
     "a plan-section reference; the plan renumbers, the comment does not — "
     "state the reasoning here or record it in the decision store"),

    ("section-ref-prose",
     re.compile(r"\b[Ss]ections?\s+\d+(?:\.\d+)+\b"),
     None,
     # "section 3.1" is the same pointer without the sign. The dotted number
     # is required so 'section 7 of POSIX' and ELF-section prose stay legal.
     "a plan-section reference; the plan renumbers, the comment does not — "
     "state the reasoning here or record it in the decision store"),

    ("changelog-was-changed",
     # Past/perfect auxiliary REQUIRED: "only calls are extracted from the
     # expanded source" is present-tense mechanism prose and legal; "was
     # extracted from cli.c" is repo history. 'moved'/'split' stay off the
     # list — "the DB was moved aside", "the input was split on commas" are
     # runtime narration, measured as such on the real tree.
     re.compile(r"\b(?:was|were|has been|have been)\s+"
                r"(?:changed|renamed|rewritten|replaced|migrated|extracted|"
                r"relocated|folded)\b"),
     None,
     # Narrates a delta from a state the reader cannot see. The diff is
     # git's; the comment should state what IS.
     "changelog phrasing; describe what the code does now — the delta is "
     "git's, the story is the decision store's"),

    ("changelog-used-to",
     re.compile(_USED_TO), _not_purpose_used_to,
     # "used to be X" documents the previous behaviour, which git blame
     # already serves with attribution.
     "changelog phrasing; describe what the code does now — the delta is "
     "git's, the story is the decision store's"),

    ("changelog-now",
     re.compile(_NOW), _not_state_now,
     # "now returns X" is a release note. It is true for one commit and
     # misleading for every commit after the next change.
     "changelog phrasing ('now ...' is a release note); describe what the "
     "code does, not that it recently started doing it"),

    ("changelog-renamed-from",
     # The bare parenthetical form ("tool_surface.h, renamed from
     # tool_tiers.h") that carries no auxiliary for the pattern above. Only
     # 'renamed': the old name is exactly what a reader can no longer look
     # up, and unlike 'extracted from' it has no innocent mechanism sense.
     re.compile(r"\brenamed\s+from\b"),
     None,
     "changelog phrasing; the former name lives in git history and the "
     "reasoning in the decision store"),
]

# ── Comment extraction ───────────────────────────────────────────────────────


def comment_view(text):
    """Return TEXT with everything except comment interiors blanked.

    Same length, newlines preserved, so match offsets convert straight to
    line numbers. A tokenizer-lite pass over C/C++: '//' (with backslash-
    newline continuation), '/* */', string and char literals with escapes,
    and raw strings R"delim(...)delim". Inside a literal, comment markers are
    just bytes; inside a comment, quotes are just bytes. That single property
    is the reason this file exists as code instead of as a grep.
    """
    out = [c if c == "\n" else " " for c in text]
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            i += 2
            while i < n:
                if text[i] == "\\" and text[i + 1:i + 2] == "\n":
                    i += 2  # spliced line: the comment continues
                    continue
                if text[i] == "\\" and text[i + 1:i + 3] == "\r\n":
                    i += 3
                    continue
                if text[i] == "\n":
                    break
                out[i] = text[i]
                i += 1
        elif two == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end
            for j in range(i + 2, end):
                out[j] = text[j]
            i = min(end + 2, n)
        elif c == '"':
            # Raw string? The '"' is glued to R (optionally u8R/uR/UR/LR).
            m = re.search(r'(?:u8|[uUL])?R$', text[max(0, i - 3):i])
            if m and not re.match(r"[A-Za-z0-9_]",
                                  text[max(0, i - len(m.group(0)) - 1):
                                       max(0, i - len(m.group(0)))] or " "):
                d_end = text.find("(", i + 1)
                if d_end < 0:
                    i = n
                    continue
                closer = ")" + text[i + 1:d_end] + '"'
                stop = text.find(closer, d_end + 1)
                i = n if stop < 0 else stop + len(closer)
                continue
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
        else:
            i += 1
    return "".join(out)


def naive_view(text):
    """The whole file, literals included — what a grep would see. NEVER used
    by the lint. It exists so the contract test can substitute it (via the
    seam below) and PROVE the string-literal test detects the difference; a
    negative control that cannot fail proves nothing."""
    return text


# NEGATIVE-CONTROL SEAM (tests substitute naive_view here)
VIEW = comment_view

# NEGATIVE-CONTROL SEAM (tests substitute "tree" here): in gate mode, "diff"
# restricts findings to lines this change adds. "tree" would gate the whole
# corpus — the forward-only fixture test fails if anyone flips it.
GATE_SCOPE = "diff"

# ── Git plumbing ─────────────────────────────────────────────────────────────


def die(msg):
    print("lint-comments: %s" % msg, file=sys.stderr)
    sys.exit(2)


def git(*args):
    p = subprocess.run(["git", *args], capture_output=True, text=True)
    if p.returncode != 0:
        return None
    return p.stdout


def resolve_base(explicit):
    candidates = [explicit] if explicit else []
    env = os.environ.get("HYP_COMMENT_LINT_BASE")
    if env:
        candidates.append(env)
    candidates += ["origin/dev", "dev"]
    for ref in candidates:
        if git("rev-parse", "--verify", "--quiet", ref + "^{commit}") is not None:
            return ref
    # Refusing here is the point: a gate that silently picked an arbitrary
    # base would either miss new comments or fail on someone else's.
    die("no base ref resolves (tried: %s). Pass --base REF or set "
        "HYP_COMMENT_LINT_BASE." % ", ".join(candidates))


def added_lines(merge_base, path):
    """Line numbers added in PATH, worktree vs MERGE_BASE (hunks of -U0)."""
    diff = git("diff", "-U0", "--no-color", merge_base, "--", path)
    if diff is None:
        return set()
    lines = set()
    for m in re.finditer(r"^@@ [^+]*\+(\d+)(?:,(\d+))? @@", diff, re.M):
        start = int(m.group(1))
        count = int(m.group(2)) if m.group(2) is not None else 1
        lines.update(range(start, start + count))
    return lines


def read_text(path):
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", errors="replace")


def findings_for(path, eligible):
    """(line, pattern-id, snippet, why) for PATH; ELIGIBLE = line set or None
    for 'every line'."""
    view = VIEW(read_text(path))
    out = []
    for pid, rx, keep, why in PATTERNS:
        for m in rx.finditer(view):
            if keep is not None and not keep(view, m):
                continue
            line = view.count("\n", 0, m.start()) + 1
            if eligible is not None and line not in eligible:
                continue
            out.append((line, pid, m.group(0).strip(), why))
    return sorted(out)


# ── Modes ────────────────────────────────────────────────────────────────────


def run_gate(base_arg):
    base = resolve_base(base_arg)
    head = git("rev-parse", "--verify", "--quiet", "HEAD^{commit}")
    if head is None:
        die("no HEAD commit — cannot diff against a base")
    mb = git("merge-base", base, "HEAD")
    if mb is None:
        die("no merge-base between %s and HEAD" % base)
    mb = mb.strip()

    tracked_changed = (git("diff", "--name-only", "--diff-filter=ACMR", "-z",
                           mb, "--") or "").split("\0")
    untracked = (git("ls-files", "--others", "--exclude-standard", "-z")
                 or "").split("\0")
    tracked_changed = [p for p in tracked_changed if p and is_candidate(p)]
    untracked = [p for p in untracked if p and is_candidate(p)]

    total = 0
    if GATE_SCOPE == "tree":
        # Reachable only through the negative-control seam above.
        allfiles = (git("ls-files", "-z") or "").split("\0")
        scan = [(p, None) for p in allfiles if p and is_candidate(p)]
    else:
        scan = [(p, added_lines(mb, p)) for p in tracked_changed]
        scan += [(p, None) for p in untracked]  # new file: every line is new

    for path, eligible in scan:
        if not os.path.exists(path):
            continue
        for line, pid, snippet, why in findings_for(path, eligible):
            print("%s:%d: [%s] %r — %s" % (path, line, pid, snippet, why))
            total += 1

    if total:
        print("\nlint-comments: FAILED — %d history-shaped comment%s added "
              "(vs %s)." % (total, "" if total == 1 else "s", base),
              file=sys.stderr)
        print("Say what IS true, in the present tense; git owns when and "
              "which commit, the decision store owns the story. Existing "
              "comments are exempt — only lines this change adds are gated.",
              file=sys.stderr)
        return 1
    print("lint-comments: OK — no history-shaped prose in comments added "
          "vs %s" % base)
    return 0


def run_report():
    """Whole-tree inventory for E2's relocation. ALWAYS exits 0: the corpus
    predates the rule, and a report that can fail a gate is a gate."""
    files = (git("ls-files", "-z") or "").split("\0")
    per_file = {}
    total = 0
    for path in files:
        if not path or not is_candidate(path) or not os.path.exists(path):
            continue
        rows = findings_for(path, None)
        if rows:
            per_file[path] = len(rows)
            total += len(rows)
        for line, pid, snippet, why in rows:
            print("%s:%d: [%s] %r" % (path, line, pid, snippet))
    print("\nlint-comments: report — %d finding%s in %d file%s (informational"
          "; this mode never gates)"
          % (total, "" if total == 1 else "s",
             len(per_file), "" if len(per_file) == 1 else "s"))
    for path, count in sorted(per_file.items(), key=lambda kv: -kv[1])[:15]:
        print("  %4d  %s" % (count, path))
    return 0


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--base", help="base ref for the diff (gate mode)")
    ap.add_argument("--all", action="store_true",
                    help="report on the whole tracked tree; never fails")
    args = ap.parse_args()

    root = git("rev-parse", "--show-toplevel")
    if root is None:
        die("not inside a git repository")
    os.chdir(root.strip())

    if args.all:
        sys.exit(run_report())
    sys.exit(run_gate(args.base))


if __name__ == "__main__":
    main()
