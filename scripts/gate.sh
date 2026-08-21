#!/usr/bin/env bash
# gate.sh — everything CI used to run, on this machine.
#
# There is no CI. Not "CI is disabled", not "CI runs on demand": the workflows
# are deleted and GitHub Actions is turned off for this repository, by the
# owner's instruction. This script is what replaced them, and it is the only
# thing that replaced them.
#
# ── Why one script and not a menu ────────────────────────────────────────────
#
# scripts/lint.sh and scripts/test.sh were already the single source of truth --
# every venue called them rather than reimplementing a leg, and the venue-parity
# contract existed to keep it that way. Deleting the venues did not change which
# script is canonical; it removed every OTHER caller. So this is a caller, not a
# new gate: it must never grow a check of its own. A check that lives here and
# nowhere else is exactly the hand-copied leg the deleted contract forbade.
#
# ── The three tiers, and why the split is by COST ────────────────────────────
#
#   --fast    seconds.  Format + the Step-0 contract gates + secret scan.
#             This is what the pre-push hook runs. It is not a token gesture:
#             the Step-0 contracts are the checks that have actually caught
#             things in this repository, and they are nearly free.
#   --full    ~10 min.  Adds cppcheck and the rest of scripts/lint.sh. cppcheck
#             is essentially the entire cost. It lived in CI precisely because
#             it is too slow for a hook, and with CI gone it has nowhere else to
#             be -- so it is here, in the tier you run before merging.
#   --all     hours.    Adds the full scripts/test.sh suite. Before a release,
#             or after anything that touches the product rather than the build.
#
# Default is --fast, because a default that takes ten minutes is a default that
# gets skipped, and the honest failure mode of this whole change is a gate
# nobody runs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINT_IMAGE="hyp-lint"
LINT_DOCKERFILE="$ROOT/test-infrastructure/Dockerfile.lint"

TIER="fast"
case "${1:-}" in
    --fast | "") TIER="fast" ;;
    --full) TIER="full" ;;
    --all) TIER="all" ;;
    -h | --help)
        sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
        exit 0
        ;;
    *)
        echo "gate.sh: unknown argument '$1' (expected --fast, --full, --all)" >&2
        exit 2
        ;;
esac

step() { printf '\n\033[1m── %s\033[0m\n' "$*" >&2; }

# ── Formatting, in the pinned environment ────────────────────────────────────
#
# The container is not optional and not belt-and-braces. This machine's
# clang-format is a different major version from the pinned one and reformats
# code the pinned one accepts, so checking with the local binary reports green
# on code the pinned binary rewrites. That mismatch cost a full cycle back when
# there was a CI to be red; it now costs a commit that has to be amended.
step "format (clang-format-20, containerised)"
if ! command -v docker >/dev/null 2>&1; then
    echo "gate: docker not found; the pinned clang-format cannot be reproduced." >&2
    echo "  Install docker, or accept that formatting is unchecked on this machine." >&2
    exit 1
fi
if ! docker image inspect "$LINT_IMAGE" >/dev/null 2>&1; then
    echo "gate: building $LINT_IMAGE (first run only; cppcheck compiles from source)..." >&2
    docker build -t "$LINT_IMAGE" -f "$LINT_DOCKERFILE" "$ROOT/test-infrastructure/" >&2
fi
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT":/src --entrypoint make "$LINT_IMAGE" \
    -f Makefile.hyp lint-format CLANG_FORMAT=clang-format-20 >&2

# ── Step-0 contracts ─────────────────────────────────────────────────────────
#
# Driven through scripts/test.sh so there is one definition of the Step-0 set.
# --contracts-only stops before the compiler-heavy legs.
step "Step-0 contract gates"
bash "$ROOT/scripts/test.sh" --contracts-only

# ── Secrets ──────────────────────────────────────────────────────────────────
#
# --all, not a worktree scan. That script's entire design point is that the
# worktree is the wrong place to look: a key deleted in the last commit is still
# in every clone, and push publishes history. Run by hand there is no push range
# on stdin, so the honest substitute is the full history sweep, not a weaker
# check wearing the same name. The pre-push hook keeps using the default range
# scan, which is the precise one.
step "secret scan (full history)"
bash "$ROOT/scripts/check-secret-history.sh" --all

if [ "$TIER" = "fast" ]; then
    printf '\n\033[1;32mgate: fast tier passed.\033[0m Before merging: gate.sh --full (~10 min).\n' >&2
    exit 0
fi

# ── The rest of lint, cppcheck included ──────────────────────────────────────
step "full lint (cppcheck — this is the ten minutes)"
docker run --rm -v "$ROOT":/src "$LINT_IMAGE" >&2

if [ "$TIER" = "full" ]; then
    printf '\n\033[1;32mgate: full tier passed.\033[0m Before a release: gate.sh --all (hours).\n' >&2
    exit 0
fi

# ── The suite ────────────────────────────────────────────────────────────────
step "full suite (scripts/test.sh)"
bash "$ROOT/scripts/test.sh"

printf '\n\033[1;32mgate: all tiers passed.\033[0m\n' >&2
