#!/usr/bin/env bash
# Documentation claims must match the binary.
#
# WHY THIS EXISTS. The MCP tool count was hand-copied into seven files and had
# drifted to FOUR different values at once — 16 in the running server, 15 in
# README/llms.txt, 14 in the npm and winget package descriptions. Nothing
# regenerates them and nothing checked them, so every one of those numbers was
# somebody's honest transcription of a number that had since changed.
#
# This asks the built binary what is true and fails when a document disagrees.
# It is deliberately narrow: it checks the claims that are cheap to verify and
# expensive to get wrong, not prose.
#
# Usage: scripts/check-docs-claims.sh [path-to-binary]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-$ROOT/build/c/hyponoia}"

if [ ! -x "$BIN" ]; then
  echo "check-docs-claims: no binary at $BIN — build first (scripts/build.sh)" >&2
  exit 2
fi

fail=0
note() { printf '  %s\n' "$1"; }

# ── The tool count, from the server itself ────────────────────────
TOOLS=$(printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"docs-check","version":"1"}}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | HYP_LOG_LEVEL=error "$BIN" 2>/dev/null \
  | python3 -c '
import sys, json
for line in sys.stdin:
    try: d = json.loads(line)
    except ValueError: continue
    if d.get("id") == 2:
        print(len(d["result"]["tools"])); break
')

if [ -z "$TOOLS" ]; then
  echo "check-docs-claims: could not read tools/list from the binary" >&2
  exit 2
fi
echo "binary reports $TOOLS MCP tools"

# Every file that states a tool count, checked per file so the failure names it.
for f in $(grep -rlE '[0-9]+ MCP tools' --include='*.md' --include='*.txt' --include='*.html' \
             --include='*.yaml' --include='*.nuspec' \
             "$ROOT/README.md" "$ROOT/docs" "$ROOT/pkg" "$ROOT/.github" 2>/dev/null); do
  for n in $(grep -oE '[0-9]+ MCP tools' "$f" | grep -oE '^[0-9]+' | sort -u); do
    if [ "$n" != "$TOOLS" ]; then
      note "MISMATCH ${f#$ROOT/} says '$n MCP tools' — binary has $TOOLS"
      fail=1
    fi
  done
done

# ── The language count ────────────────────────────────────────────
GRAMMARS=$(ls -1 "$ROOT/internal/hyp/vendored/grammars" 2>/dev/null | wc -l | tr -d ' ')
for f in $(grep -rlE '[0-9]{3} languages' --include='*.md' --include='*.txt' --include='*.html' \
             --include='*.yaml' --include='*.nuspec' \
             "$ROOT/README.md" "$ROOT/docs" "$ROOT/pkg" 2>/dev/null); do
  for n in $(grep -oE '[0-9]{3} languages' "$f" | grep -oE '^[0-9]+' | sort -u); do
    # Grammar directories are not exactly the supported-language count (some
    # grammars back several names, a few are first-party). One number, stated
    # consistently, is the property worth enforcing — so compare files to the
    # README rather than to the directory listing.
    README_N=$(grep -oE '[0-9]{3} languages' "$ROOT/README.md" | grep -oE '^[0-9]+' | head -1)
    if [ -n "$README_N" ] && [ "$n" != "$README_N" ]; then
      note "MISMATCH ${f#$ROOT/} says '$n languages' — README says $README_N"
      fail=1
    fi
  done
done

if [ "$fail" -ne 0 ]; then
  echo
  echo "check-docs-claims: FAILED — a document states a number the binary contradicts." >&2
  echo "Fix the document, not this check." >&2
  exit 1
fi
echo "check-docs-claims: OK — documented counts match the binary ($TOOLS tools, $GRAMMARS grammar dirs)"
