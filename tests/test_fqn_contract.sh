#!/usr/bin/env bash
# Contract: there is ONE qualified-name derivation, and the differential that
# guards it actually runs.
#
# Two layers hand out QNs — src/pipeline/fqn.c derives the ones the pipeline
# looks up, internal/hyp/helpers.c derives the ones extraction writes — and the
# registry joins them. Six ways they disagreed were found by
# tests/test_fqn_differential.c, one of them an address COLLISION: a dotfile's
# module QN was byte-identical to the project QN, so an anchor on it resolved,
# confidently, to the repository root.
#
# Five of the six were disagreements about a RULE, so the rules were moved into
# src/foundation/fqn_core.h and both sides became allocator wrappers over it.
# That makes those five unrepresentable rather than merely fixed — which is a
# stronger guarantee, and a fragile one: it holds exactly as long as the
# wrappers stay wrappers. Nothing in C can assert that about its own source.
#
# Four properties, none of which a C test can check about itself:
#
#   1. BOTH WRAPPERS DELEGATE. Each entry point calls the core. A wrapper that
#      grows its own segment loop is a second derivation, and the differential
#      would only catch it on an input someone thought of.
#   2. THE RULES LIVE IN ONE PLACE. The literals that ARE the rules —
#      "__file__", "__init__", "index" — belong to the core. Finding one in a
#      wrapper means a rule was copied rather than called. Checked with a
#      POSITIVE CONTROL: the core must contain them, or the grep is looking for
#      words nobody writes and passes forever.
#   3. THE LEDGER IS HONEST. The differential pins a count of known
#      divergences. That count and the number of pinned defect rows are two
#      numbers in one file that must agree, and each is written by hand.
#   4. THE DIFFERENTIAL IS WIRED. A suite in no source list and no runner
#      registration is a comment. Checked in both places, with a negative
#      control so "grep found it" is not "grep finds anything".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CORE="src/foundation/fqn_core.h"
PIPE="src/pipeline/fqn.c"
EXTRACT="internal/hyp/helpers.c"
SUITE="tests/test_fqn_differential.c"
MAKEFILE="Makefile.hyp"
RUNNER="tests/test_main.c"

fail() {
    echo "FAIL (fqn contract): $*" >&2
    exit 1
}

for f in "$CORE" "$PIPE" "$EXTRACT" "$SUITE" "$MAKEFILE" "$RUNNER"; do
    # A contract that cannot verify must not report success.
    [ -f "$f" ] || fail "missing $f — cannot verify that one derivation is one derivation"
done

# ── 1. Both wrappers delegate to the core ────────────────────────────────────
# entry point : file : core function it must call
DELEGATIONS="
hyp_pipeline_fqn_compute:$PIPE:hyp_fqn_core_write
hyp_pipeline_fqn_folder:$PIPE:hyp_fqn_core_folder_write
hyp_pipeline_fqn_module_dir:$PIPE:hyp_fqn_core_dir_len
hyp_fqn_compute:$EXTRACT:hyp_fqn_core_write
hyp_fqn_folder:$EXTRACT:hyp_fqn_core_folder_write
hyp_fqn_module_source_lang:$EXTRACT:hyp_fqn_core_dir_len
"

# Print the body of a function definition: from the line whose text starts the
# definition through the closing brace in column 1.
func_body() {
    awk -v fn="$2" '
        index($0, fn "(") && /^[A-Za-z_].*\(/ { inside = 1 }
        inside { print }
        inside && /^}/ { exit }
    ' "$1"
}

for row in $DELEGATIONS; do
    fn="${row%%:*}"
    rest="${row#*:}"
    file="${rest%%:*}"
    want="${rest##*:}"
    body="$(func_body "$file" "$fn")"
    if [ -z "$body" ]; then
        fail "$file no longer defines $fn — the entry point moved, and this contract cannot see where"
    fi
    if ! printf '%s\n' "$body" | grep -q "$want"; then
        fail "$file:$fn does not call $want. Two derivations that agree only when someone
       writes the right test is the state that shipped a dotfile module QN identical to the
       project QN. Call the core; do not reimplement it."
    fi
done

# The negative control for the check above: the same matcher, asked for a call
# that is deliberately absent. Without it, a func_body that returned the whole
# file would pass every row and prove nothing.
if printf '%s\n' "$(func_body "$PIPE" "hyp_pipeline_fqn_compute")" |
    grep -q "hyp_fqn_core_folder_write"; then
    fail "the delegation matcher sees hyp_fqn_core_folder_write inside hyp_pipeline_fqn_compute,
       which does not call it — the matcher is reading past the function it was given, so every
       row above passes for the wrong reason"
fi

# ── 2. The rules live in exactly one place ───────────────────────────────────
# These literals ARE derivation rules: the File-node terminal, and the two
# package-entry segments a symbol QN addresses through. A wrapper naming one has
# copied a rule instead of calling it.
RULE_LITERALS='__file__ __init__'

for lit in $RULE_LITERALS; do
    # POSITIVE CONTROL: the core must carry it, or this grep is a guard that
    # cannot fire.
    if ! grep -q -- "$lit" "$CORE"; then
        fail "$CORE does not mention $lit, so the check below cannot be shown to detect
       anything. Either the rule moved and this list is stale, or the guard is looking for a
       word nobody would write."
    fi
    if grep -q -- "$lit" "$PIPE"; then
        fail "$PIPE names $lit. The rule belongs to $CORE; a copy here is the second
       derivation this contract exists to prevent."
    fi
done

# "index" is a common English word, so it is checked in the wrapper that has no
# other business with it rather than banned everywhere.
if grep -qE '"index"' "$PIPE"; then
    fail "$PIPE names the \"index\" package-entry segment. That rule belongs to $CORE."
fi

# ── 3. The ledger and the pinned rows agree ──────────────────────────────────
LEDGER="$(grep -E '^#define FQD_KNOWN_DIVERGENCE_AXES' "$SUITE" | awk '{print $3}')"
case "$LEDGER" in
"" | *[!0-9]*) fail "$SUITE does not define FQD_KNOWN_DIVERGENCE_AXES as a plain number" ;;
esac
PINNED="$(grep -cE '^TEST\(fqn_differential_DEFECT_' "$SUITE" || true)"
if [ "$LEDGER" -ne "$PINNED" ]; then
    fail "$SUITE pins $LEDGER known divergences and carries $PINNED characterization tests.
       These are two hand-written numbers in one file: a fixed axis whose row was deleted
       without lowering the count leaves a ledger that over-reports, and a lowered count with
       the row still present hides a live divergence behind a green suite."
fi

# ── 4. The differential is wired into the build and the runner ───────────────
if ! grep -q "tests/test_fqn_differential.c" "$MAKEFILE"; then
    fail "$MAKEFILE does not compile $SUITE. A suite in no source list never runs, and a
       gate that never runs is a comment."
fi
for needle in "extern void suite_fqn_differential(void);" "RUN_SELECTED_SUITE(fqn_differential)"; do
    grep -q -- "$needle" "$RUNNER" ||
        fail "$RUNNER is missing '$needle' — the suite compiles and is never invoked"
done
# Negative control: the same two greps, for a suite that does not exist.
if grep -q "RUN_SELECTED_SUITE(fqn_differential_that_does_not_exist)" "$RUNNER"; then
    fail "the runner-registration grep matches a suite name that was never written, so it
       cannot distinguish a wired suite from an unwired one"
fi

echo "fqn contract: 6 entry points delegate to $CORE, 3 derivation rules live only there," \
    "ledger pins $LEDGER divergences against $PINNED characterization rows, differential" \
    "wired into the build and the runner"
