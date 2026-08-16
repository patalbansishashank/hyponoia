#!/usr/bin/env bash
set -euo pipefail

# Install the repo's git hooks into .git/hooks (local, per-clone).
#   commit-msg  strict DCO sign-off enforcement
#   pre-push    refuse to publish a commit carrying a vendor API key (§2.13)
#
# HOOKS DO NOT SURVIVE CLONING, so neither of these is a control — they are fast
# local feedback. The control is CI: ci.yml runs the same scanner over full
# history on every push, which is what actually covers a fresh clone.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOKS_DIR="$(git -C "$ROOT" rev-parse --git-path hooks)"

install -m 755 "$ROOT/scripts/git-hooks/commit-msg" "$HOOKS_DIR/commit-msg"
echo "installed: $HOOKS_DIR/commit-msg (DCO sign-off required on every commit)"

install -m 755 "$ROOT/scripts/git-hooks/pre-push" "$HOOKS_DIR/pre-push"
echo "installed: $HOOKS_DIR/pre-push (lints in CI's own container, then scans the commits a push would publish for vendor API keys)"
