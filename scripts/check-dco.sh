#!/usr/bin/env bash
set -euo pipefail

# DCO enforcement: every commit in the given range must carry a
# Signed-off-by trailer whose email matches the commit author
# (Developer Certificate of Origin 1.1 — see the DCO file).
#
# Usage: check-dco.sh <range>          e.g. origin/main..HEAD, sha1..sha2
#
# Exemptions (same as the standard DCO checks): merge commits and
# bot-authored commits (author name ending in [bot]).

RANGE="${1:?usage: check-dco.sh <commit-range>}"
ROOT_DIR="$(git rev-parse --show-toplevel)"

FAIL=0
CHECKED=0
while IFS= read -r sha; do
    # Skip merge commits
    nparents=$(git rev-list --no-walk --parents -n1 "$sha" | wc -w)
    if [ "$nparents" -gt 2 ]; then
        continue
    fi
    author_name=$(git log -1 --format='%an' "$sha")
    case "$author_name" in
        *"[bot]") continue ;;
    esac
    CHECKED=$((CHECKED + 1))
    author_email=$(git log -1 --format='%ae' "$sha")
    # One implementation, shared with the commit-msg hook. These were two
    # definitions of "signed" — a line match here, git's trailer block there —
    # and they agreed until a blank line before Co-Authored-By pushed
    # Signed-off-by out of the trailer block, passing one gate and failing the
    # other.
    msg_file=$(mktemp)
    git log -1 --format='%B' "$sha" > "$msg_file"
    signed=0
    "$ROOT_DIR/scripts/dco-signed.sh" "$author_email" "$msg_file" || signed=1
    rm -f "$msg_file"
    if [ "$signed" -ne 0 ]; then
        echo "BLOCKED: $sha lacks a Signed-off-by matching its author:"
        git log -1 --format='  author: %an <%ae>%n  subject: %s' "$sha"
        echo "  fix: git commit --amend -s   (or: git rebase --signoff <base>)"
        FAIL=1
    fi
done < <(git rev-list "$RANGE")

if [ "$FAIL" -ne 0 ]; then
    echo "=== DCO CHECK FAILED — every commit must be signed off (git commit -s) ==="
    echo "=== See the DCO file and .github/CONTRIBUTING.md ==="
    exit 1
fi
echo "OK: $CHECKED commit(s) in $RANGE carry a valid Signed-off-by"
