#!/usr/bin/env bash
#
# test_fixture_lineage.sh — a fixture that names a commit must name one a clone
# of the remote can resolve.
#
# THE DEFECT THIS EXISTS FOR. A corpus mined on a developer's machine sees that
# machine's refs, including branches that are only ever local. Every commit
# resolves there, so the corpus looks complete and its own header can declare a
# lineage it does not keep. Somewhere else — CI, a second checkout, a reader
# following a citation — the object is simply absent, and what the consumer does
# with that absence decides whether anyone finds out. A replay harness degrades
# by one pair and reports a count. A retrieval corpus keeps working and only its
# provenance dies, silently, because nothing ever runs the cite command.
#
# So the check is on the objects, and it resolves against the refs a clone would
# have rather than against every ref this machine happens to hold. That is the
# whole point: asking local refs passes exactly on the machine where the problem
# lives.
#
# WHAT IT SWEEPS. Every file under tests/fixtures, derived by walking the tree —
# there is no list of corpora here, because a list is only as good as whoever
# maintained it and the corpus this catches would have been added after it.
# Tokens of 12 to 40 hex characters are offered to git; whatever resolves to a
# commit object is a commit the fixture names, and everything else (span hashes,
# blob ids, prose) is left alone by construction rather than by exclusion.
#
# TAGS COUNT, and the reason is not convenience. A commit can be on no branch
# at all and still be something every clone resolves, because a tag is a ref
# and the checkout action fetches refs/tags/*. This repository already uses
# that deliberately: its record/* tags are the only ref their commits have, and
# they exist so a corpus can cite a decision whose commit was never merged.
# Refusing a tag would refuse the mechanism built for exactly this case.
#
# A tag is the weaker carrier though — it is one ref with no branch behind it,
# and it can be deleted without a branch noticing — so a commit that only a tag
# holds is REPORTED rather than passed in silence. That is a disclosure, not a
# failure, and the count is printed either way.
#
# LIMIT, stated rather than discovered: refs/tags/* is a LOCAL namespace that
# happens to mirror what was fetched, so a tag created locally and never pushed
# satisfies this gate and would not satisfy a clone. Closing that needs
# `git ls-remote origin 'refs/tags/*'`, which needs the network, which a test
# gate does not get. Anyone relying on a tag-only row should run it once.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

FIXTURES="tests/fixtures"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "FAIL: $ROOT is not a git work tree, so no fixture's commits can be checked" >&2
    exit 1
fi

mapfile -t PUBLISHED < <(git for-each-ref --format='%(refname)' refs/remotes/origin refs/tags)
if [ "${#PUBLISHED[@]}" -eq 0 ]; then
    cat >&2 <<'EOF'
FAIL: this checkout tracks no remote branches and holds no tags, so "can a clone
      of the remote resolve this commit" cannot be answered here. Fetch the
      remote before running this gate; a checkout that cannot answer the
      question must not report that the answer is yes.
EOF
    exit 1
fi

checked=0
resolved=0
violations=0
tag_only=0

while IFS= read -r file; do
    checked=$((checked + 1))
    # Split on anything that is not a hex digit, then keep the plausible
    # lengths. Anything that is not a commit fails to resolve and is dropped.
    while IFS= read -r token; do
        [ -n "$token" ] || continue
        len=${#token}
        if [ "$len" -lt 12 ] || [ "$len" -gt 40 ]; then
            continue
        fi
        git rev-parse --verify --quiet "${token}^{commit}" >/dev/null 2>&1 || continue
        sha="$(git rev-parse --verify --quiet "${token}^{commit}")"
        resolved=$((resolved + 1))
        branch_ref="$(git for-each-ref --count=1 --contains "$sha" --format='%(refname)' \
            refs/remotes/origin 2>/dev/null || true)"
        if [ -n "$branch_ref" ]; then
            continue
        fi
        tag_ref="$(git for-each-ref --count=1 --contains "$sha" --format='%(refname)' \
            refs/tags 2>/dev/null || true)"
        if [ -n "$tag_ref" ]; then
            tag_only=$((tag_only + 1))
            echo "TAG-ONLY: $file names $token"
            echo "          on no remote branch; $tag_ref is the only ref carrying it"
            continue
        fi
        violations=$((violations + 1))
        echo "VIOLATION: $file names $token ($sha)"
        echo "           reachable from no ref a clone of the remote would have;" \
             "$(git for-each-ref --count=3 --contains "$sha" --format='%(refname)' \
                refs/heads 2>/dev/null | tr '\n' ' ')holds it locally and nothing else does"
    done < <(tr -c '0-9a-f' '\n' <"$file" | sort -u)
done < <(find "$FIXTURES" -type f | sort)

echo "fixture lineage: $checked files, $resolved commit references,"\
     "$tag_only carried only by a tag, $violations unreachable"

if [ "$resolved" -eq 0 ]; then
    echo "FAIL: no fixture named a single resolvable commit, so this gate checked nothing." \
         "Either the sweep is broken or the corpora moved." >&2
    exit 1
fi

if [ "$violations" -gt 0 ]; then
    cat >&2 <<EOF
FAIL: $violations commit reference(s) above live only in this repository's local
      refs. A corpus row pointing at one of them replays, or cites, on exactly
      one machine. Re-derive the row against published history, or publish the
      commit the row needs — a record/* tag is how this repository pins history
      a corpus cites.
EOF
    exit 1
fi

echo "PASS: every commit the fixtures name is reachable from a ref a clone would have"
