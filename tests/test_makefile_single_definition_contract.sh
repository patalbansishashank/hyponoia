#!/usr/bin/env bash
# A make variable assigned twice UNCONDITIONALLY is not a merge conflict — it is
# a silent loss. make takes the last assignment, so the earlier one's contents
# vanish from the link with no diagnostic until an undefined symbol appears,
# possibly in a different module, possibly not at all if nothing references it
# yet.
#
# This has happened twice: a union merge of thirteen branches duplicated
# PROD_SRCS and ALL_TEST_SRCS, and the hand-written repair of those two missed
# STORE_SRCS and LINT_SRCS, which dropped src/store/record_store.c out of the
# product binary while its own suite reported green.
#
# Assignments inside ifeq/ifdef branches are legitimate and expected — that is
# how this file selects per-platform and per-compiler flags — so only
# assignments at conditional depth zero are compared.
#
# The variable set is derived from the file, never listed here: a list is only
# as good as its enumeration, and an incomplete enumeration is exactly the
# failure this gate exists to stop repeating.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAKEFILE="$ROOT/Makefile.hyp"

[ -f "$MAKEFILE" ] || { echo "FAIL: $MAKEFILE not found"; exit 1; }

dupes="$(
  awk '
    # NOTE: no \b — POSIX awk does not have it, and gawk reads it as backspace,
    # so a word-boundary anchor here silently matches nothing and every
    # conditional override reads as a duplicate.
    /^[[:space:]]*(ifeq|ifneq|ifdef|ifndef)([[:space:]]|\()/ { depth++; next }
    /^[[:space:]]*endif([[:space:]]|$)/                      { if (depth > 0) depth--; next }
    /^[[:space:]]*else([[:space:]]|$)/                       { next }
    depth > 0 { next }
    # Recursive (=) and simple (:=) only. ?= is "unless already set" and += is
    # explicitly additive; neither discards the earlier value.
    /^[A-Za-z_][A-Za-z0-9_]*[[:space:]]*:?=/ {
      name = $0
      sub(/[[:space:]]*:?=.*/, "", name)
      if (name ~ /^[A-Za-z_][A-Za-z0-9_]*$/) { count[name]++; where[name] = where[name] " " NR }
    }
    END { for (n in count) if (count[n] > 1) print count[n], n, where[n] }
  ' "$MAKEFILE" | sort -k2
)"

if [ -n "$dupes" ]; then
  echo "FAIL: Makefile.hyp assigns these variables more than once at top level."
  echo "      make takes the LAST assignment; every earlier one is silently discarded."
  echo "      Merge the contents into ONE assignment — do not delete either line's"
  echo "      contents, and never add a second definition to 'append' to a list."
  echo
  while read -r count name lines; do
    printf '  %s assignments of %-24s at lines:%s\n' "$count" "$name" "$lines"
  done <<< "$dupes"
  exit 1
fi

echo "OK: every top-level Makefile.hyp variable is assigned exactly once"
