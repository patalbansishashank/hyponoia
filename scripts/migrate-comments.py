#!/usr/bin/env python3
"""Comment migration: lift history-shaped comment prose out of the source and
into a manifest the decision store can ingest (Track E, unit E2).

RELOCATE, DO NOT DELETE. E1's lint says a comment must not carry a date, a
commit SHA, a plan-section reference or changelog framing. Some of the best
prose in this repository wears exactly that costume, and deleting it to satisfy
a lint would trade a rule for the reasoning the rule exists to protect. So the
prose moves: this script reads every comment block E1 has a finding in, and
emits it as a manifest of records. The code keeps the invariant and its reason
in the present tense; the provenance becomes retrievable by asking.

WHAT IT READS, AND WHY THAT IS NOT THE WORKING TREE. Every byte comes from
`git show <rev>:<path>` and every attribution from `git blame` at that same
rev, HEAD by default. Two consequences, both load-bearing:

  - line numbers in the manifest mean something, because a dirty working tree
    cannot shift them out from under the blame that produced them;
  - a second machine at the same commit produces a byte-identical manifest, so
    the ingest deduplicates instead of duplicating.

RUN IT AT THE COMMIT BEFORE THE CLEANUP, NOT AFTER. This is the one ordering
that is easy to get backwards and silent when you do. Rewriting a comment in
place and then migrating captures what SURVIVED the rewrite; what the rewrite
REMOVED is then in no store and only in git. "Relocate, do not delete" is a
claim about the removed prose, so either migrate first, or pass `--rev` naming
the commit before the cleanup. Both manifests can be ingested: the origin binds
the blob, so a changed file yields a second record and an unchanged one
deduplicates, and the union is the whole history of the prose.

ATTRIBUTION COMES FROM BLAME, NEVER FROM THE CLOCK. Author and timestamp are
facts about the commit that last touched the block, not about the migration.
The record id commits to both, so a migrator that authored the corpus at
its own `now()` would mint a fresh id on every machine and every re-run: the
union would then hold N copies of one comment and have no way to tell them
apart. That is the failure this file is shaped to avoid, and it is the seam a
negative control mutates to prove the shape is doing work.

COMMENTS ONLY, STRUCTURALLY. The comment view comes from E1
(scripts/lint-comments.py) — one tokenizer, not a second one that could
disagree with the gate about what a comment is. A date inside a string literal
is code, this script cannot see it, and that is the property both prior
mechanical passes over this tree lacked.

Output is a length-prefixed manifest (see MANIFEST FORMAT below), read by
src/memory/comment_migrate.c. Nothing here knows what an address or a record
id looks like: this end supplies facts from git, the C end supplies the
contracts. A format either end can guess at is a format the two ends can
disagree about, so every field is explicit and every length is counted.

Usage:
  scripts/migrate-comments.py -o FILE          whole tracked tree at HEAD
  scripts/migrate-comments.py -o FILE PATH...  named paths only
  scripts/migrate-comments.py --rev REV -o F   the tree as it was at REV
  scripts/migrate-comments.py --list           work list, one path per finding
                                               count, no manifest
"""

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import time

MANIFEST_MAGIC = "hyp-comment-manifest-v1"

# The fields of one item, in order. The C reader expects exactly this sequence
# and refuses anything else — an ordered format has no place for a field one
# end writes and the other silently ignores.
FIELDS = ("path", "blob", "lines", "author", "timestamp_ms", "content")

# NEGATIVE-CONTROL SEAM (control 1 substitutes "now" here). "blame" attributes
# each block to the commit that last touched it, which is the same answer on
# every machine at a given commit. "now" attributes the whole corpus to the
# migrating agent at ingest time — the failure mode this unit exists to
# prevent, kept reachable only by mutating this line so the control that
# watches the duplicate appear is a real mutation of the real script.
ATTRIBUTION = "blame"

MIGRATOR_AUTHOR = "agent:comment-migrator"

# The commit the manifest describes. Set from --rev; HEAD unless asked.
REV = "HEAD"


def die(msg):
    print("migrate-comments: %s" % msg, file=sys.stderr)
    sys.exit(2)


def load_lint():
    """E1's lint, imported as a module. The filename has a hyphen, so it is not
    importable by name; loading it by path is what keeps ONE comment view and
    ONE pattern table in the tree."""
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "lint-comments.py")
    if not os.path.exists(path):
        die("missing %s — the comment view and pattern table live there" % path)
    spec = importlib.util.spec_from_file_location("lint_comments", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


LINT = load_lint()


def git(*args, binary=False):
    p = subprocess.run(["git", *args], capture_output=True)
    if p.returncode != 0:
        return None
    return p.stdout if binary else p.stdout.decode("utf-8", errors="replace")


def git_or_die(*args, binary=False):
    out = git(*args, binary=binary)
    if out is None:
        die("git %s failed" % " ".join(args))
    return out


# ── Blocks ───────────────────────────────────────────────────────────────────


def comment_blocks(view):
    """Maximal runs of consecutive lines carrying comment text, as
    (start_line, end_line, [line-text...]) with 1-based inclusive lines.

    A block is the unit of relocation because a block is the unit of prose. A
    single flagged line lifted out of the paragraph that gives it meaning is
    the deletion this unit refuses, performed one line at a time.
    """
    lines = view.split("\n")
    blocks = []
    run_start = None
    for idx, text in enumerate(lines, start=1):
        if text.strip():
            if run_start is None:
                run_start = idx
        elif run_start is not None:
            blocks.append((run_start, idx - 1, lines[run_start - 1:idx - 1]))
            run_start = None
    if run_start is not None:
        blocks.append((run_start, len(lines), lines[run_start - 1:]))
    return blocks


def findings_in(view):
    """Line numbers E1 has a finding on, using E1's own pattern table and
    post-filters. Never a second opinion about what is history-shaped."""
    hits = set()
    for _pid, rx, keep, _why in LINT.PATTERNS:
        for m in rx.finditer(view):
            if keep is not None and not keep(view, m):
                continue
            hits.add(view.count("\n", 0, m.start()) + 1)
    return hits


_LEADER = re.compile(r"^\s*\*\s?")


def block_prose(raw_lines):
    """The block's text with the block-comment leader removed and the common
    indent gone. Markers are punctuation of the source, not of the prose; what
    is stored is what a person wrote."""
    stripped = [_LEADER.sub("", ln).rstrip() for ln in raw_lines]
    body = [ln for ln in stripped]
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    if not body:
        return ""
    indents = [len(ln) - len(ln.lstrip()) for ln in body if ln.strip()]
    cut = min(indents) if indents else 0
    return "\n".join(ln[cut:] if ln.strip() else "" for ln in body)


# ── Attribution ──────────────────────────────────────────────────────────────


def blame_block(path, start, end):
    """(author, timestamp_ms) for a line range, from `git blame` at HEAD.

    A block can span commits. The answer is the LAST commit to touch any of its
    lines: that is the commit that put the prose in the form being relocated,
    and blame's answer for a single line is the same rule. Ties break on the
    commit id so two machines cannot disagree.
    """
    out = git("blame", "--porcelain", "-L", "%d,%d" % (start, end), REV, "--", path)
    if out is None:
        die("git blame failed for %s:%d-%d" % (path, start, end))
    commits = {}
    cur = None
    for line in out.split("\n"):
        m = re.match(r"^([0-9a-f]{40})\s+\d+\s+\d+", line)
        if m:
            cur = m.group(1)
            commits.setdefault(cur, {})
            continue
        if cur is None:
            continue
        if line.startswith("author "):
            commits[cur]["name"] = line[len("author "):]
        elif line.startswith("author-mail "):
            commits[cur]["mail"] = line[len("author-mail "):].strip("<>")
        elif line.startswith("author-time "):
            commits[cur]["time"] = int(line[len("author-time "):])
    usable = [(v.get("time", 0), sha, v) for sha, v in commits.items() if "time" in v]
    if not usable:
        die("no blame attribution for %s:%d-%d" % (path, start, end))
    usable.sort(key=lambda t: (t[0], t[1]))
    _t, _sha, best = usable[-1]
    name = best.get("name", "").strip()
    mail = best.get("mail", "").strip()
    if not name:
        die("blame gave no author for %s:%d-%d" % (path, start, end))
    author = "%s <%s>" % (name, mail) if mail else name
    return author, best["time"] * 1000


def attribution(path, start, end):
    if ATTRIBUTION == "blame":
        return blame_block(path, start, end)
    # Reachable only through the seam above; see the control it serves.
    return MIGRATOR_AUTHOR, int(time.time() * 1000)


# ── Manifest ─────────────────────────────────────────────────────────────────
#
# MANIFEST FORMAT (hyp-comment-manifest-v1). A magic line, then items; each
# item is FIELDS in order, each field a header line "<name> <byte-length>"
# followed by exactly that many bytes and one newline. Length-prefixed for the
# same reason the record preimage is: no value can impersonate a delimiter, so
# there is no escaping rule for either end to get wrong, and prose containing
# newlines, quotes or the word "content" is just bytes.


def emit_magic(out):
    out.write((MANIFEST_MAGIC + "\n").encode("utf-8"))


def emit_item(out, item):
    for name in FIELDS:
        payload = str(item[name]).encode("utf-8")
        out.write(("%s %d\n" % (name, len(payload))).encode("utf-8"))
        out.write(payload)
        out.write(b"\n")


def collect(paths):
    """Every comment block carrying at least one E1 finding, attributed."""
    items = []
    for path in paths:
        blob = git("rev-parse", REV + ":" + path)
        if blob is None:
            continue  # not in HEAD: an untracked or newly added file
        blob = blob.strip()
        raw = git("show", REV + ":" + path, binary=True)
        if raw is None:
            continue
        text = raw.decode("utf-8", errors="replace")
        view = LINT.VIEW(text)
        hits = findings_in(view)
        if not hits:
            continue
        for start, end, raw_lines in comment_blocks(view):
            if not any(start <= h <= end for h in hits):
                continue
            prose = block_prose(raw_lines)
            if not prose:
                continue
            author, ts = attribution(path, start, end)
            items.append({
                "path": path,
                "blob": blob,
                "lines": "%d-%d" % (start, end),
                "author": author,
                "timestamp_ms": ts,
                "content": prose,
            })
    return items


def tracked_candidates():
    """Every lintable file present AT THE REV — not in the working tree, so a
    manifest describes one commit and nothing else."""
    out = git_or_die("ls-tree", "-r", "--name-only", "-z", REV)
    return [p for p in out.split("\0") if p and LINT.is_candidate(p)]


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("-o", "--output", help="manifest path ('-' for stdout)")
    ap.add_argument("--list", action="store_true",
                    help="print the work list (path, finding count) and stop")
    ap.add_argument("--rev", default="HEAD",
                    help="the commit to read (default HEAD); name the commit "
                         "BEFORE a cleanup to capture what it removed")
    ap.add_argument("paths", nargs="*", help="paths to migrate (default: all)")
    args = ap.parse_args()

    global REV
    REV = args.rev

    root = git("rev-parse", "--show-toplevel")
    if root is None:
        die("not inside a git repository")
    os.chdir(root.strip())

    paths = args.paths or tracked_candidates()
    paths = [p for p in paths if LINT.is_candidate(p)]

    if args.list:
        rows = []
        for path in paths:
            raw = git("show", REV + ":" + path, binary=True)
            if raw is None:
                continue
            n = len(findings_in(LINT.VIEW(raw.decode("utf-8", errors="replace"))))
            if n:
                rows.append((n, path))
        for n, path in sorted(rows, key=lambda r: (-r[0], r[1])):
            print("%4d  %s" % (n, path))
        print("\nmigrate-comments: %d file%s carry findings"
              % (len(rows), "" if len(rows) == 1 else "s"))
        return 0

    if not args.output:
        die("-o/--output is required (use '-' for stdout)")

    items = collect(paths)
    fh = sys.stdout.buffer if args.output == "-" else open(args.output, "wb")
    try:
        emit_magic(fh)
        for item in items:
            emit_item(fh, item)
    finally:
        if fh is not sys.stdout.buffer:
            fh.close()
    print("migrate-comments: %d block%s from %d file%s"
          % (len(items), "" if len(items) == 1 else "s",
             len({i["path"] for i in items}),
             "" if len({i["path"] for i in items}) == 1 else "s"),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
