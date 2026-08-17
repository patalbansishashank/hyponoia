#!/usr/bin/env bash
# Contract: the comment lint (Track E, unit E1) — comments only, forward only.
#
# scripts/lint-comments.py gates history-shaped prose (dates, commit SHAs,
# plan-section references, changelog phrasing) out of NEW comments. Two
# properties carry the whole unit, and each has already burned this plan once:
#
#   1. COMMENTS ONLY. The matcher must see comment text and nothing else.
#      Both prior burns were mechanical passes that could not tell a string
#      literal containing a date from a comment containing one. The test that
#      matters here is the one where every banned form sits inside string
#      literals and the gate stays green.
#   2. FORWARD ONLY. The gate examines lines added relative to a base ref.
#      The existing corpus is E2's migration inventory, never a gate failure —
#      pre-existing offenders must not fire from unchanged files, nor from
#      untouched lines of a file the change edits.
#
# Both properties get a NEGATIVE CONTROL: a copy of the lint is mutated at a
# marked seam (naive extractor; whole-tree gate) and the corresponding test is
# watched to FAIL, then the unmutated lint passes it. A control that cannot
# fail proves nothing, so each mutation is first asserted to have applied.
#
# Fixtures are BUILT IN A THROWAWAY REPO at run time, deliberately: committing
# files full of banned comment forms to the real tree would seed the corpus
# this lint exists to stop, and would poison its own --all inventory.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINT="$ROOT/scripts/lint-comments.py"

fail() {
    echo "FAIL (comment lint contract): $*" >&2
    exit 1
}

# A contract that cannot verify must not report success.
command -v git >/dev/null 2>&1 || fail "git not found — cannot verify"
command -v python3 >/dev/null 2>&1 || fail "python3 not found — cannot verify"
[ -f "$LINT" ] || fail "missing $LINT"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
unset HYP_COMMENT_LINT_BASE || true

GITC=(git -c user.name=contract -c user.email=contract@invalid \
          -c commit.gpgsign=false -c protocol.file.allow=always)

# run <expected-exit> <script> [args...] — captures output in $OUT.
run() {
    local expect="$1" script="$2"
    shift 2
    local rc=0
    OUT="$(python3 "$script" "$@" 2>&1)" || rc=$?
    if [ "$rc" -ne "$expect" ]; then
        printf '%s\n' "$OUT" >&2
        fail "expected exit $expect from ${script##*/} $*, got $rc"
    fi
}

# ── The throwaway repo, with a base commit full of PRE-EXISTING offenders ────
REPO="$WORK/repo"
mkdir -p "$REPO/src"
cd "$REPO"
"${GITC[@]}" init -q -b main .

cat > src/legacy.c <<'EOF'
/* Frozen 2026-08-16 by commit a582645d; see NEXT-STEPS.md §2.4.
 * This used to be a linked list and was changed to an id-keyed set. */
int legacy_answer(void) {
    return 42;
}
EOF
cat > src/other.c <<'EOF'
/* A quiet file with nothing to answer for. */
int other_answer(void) {
    return 7;
}
EOF
"${GITC[@]}" add src/legacy.c src/other.c
"${GITC[@]}" commit -q -m "base: corpus predating the rule"
"${GITC[@]}" tag base

# ── 1. Every banned form in a NEW comment fires, and is named ────────────────
cat > src/new_bad.c <<'EOF'
/* Reviewed 2026-08-16, and again on 16 Aug 2026, per §2.4 of the plan
 * (also written up in section 3.1). Commit f95d6841 renamed it; the enum
 * was changed to a table, which used to be a switch. The helper now returns
 * a pointer (renamed from old_helper). */
int new_bad(void) {
    return 1;
}
EOF
run 1 "$LINT" --base base
for pid in date-iso date-prose commit-sha section-ref section-ref-prose \
           changelog-was-changed changelog-used-to changelog-now \
           changelog-renamed-from; do
    printf '%s\n' "$OUT" | grep -q "src/new_bad.c:[0-9]*: \[$pid\]" \
        || { printf '%s\n' "$OUT" >&2; fail "gate did not flag [$pid] in src/new_bad.c"; }
done
rm src/new_bad.c

# ── 2. THE test: the same forms inside string literals must NOT fire ─────────
cat > src/new_strings.c <<'EOF'
/* Golden inputs for the parser. The comments in this file are clean. */
static const char *golden[] = {
    "frozen 2026-08-16 by commit a582645d, see §2.4 and section 3.1",
    "this used to be a list and was changed to a set; now returns NULL",
    "16 Aug 2026 (renamed from old_helper)",
    "// a line comment inside a string, dated 2026-01-02",
    "/* a block comment inside a string, per §9.9 */",
};
static const char *tricky = "escaped quote \" then 2026-03-04 still a string";
int new_strings_count(void) {
    return (int)(sizeof(golden) / sizeof(golden[0]));
}
EOF
run 0 "$LINT" --base base

# ── 3. Clean comments pass, including the tuned near-misses ──────────────────
cat > src/new_clean.c <<'EOF'
/* The arena is used to return per-call scratch without a heap round trip.
 * token_map is now read-only in this phase; calls are extracted from the
 * expanded source. Hostnames look like "svc-ab12cd34ef-uc.a.run.app"; a
 * content hash is 64 hex and stays legal on its own line:
 * 9f2c1a7d09b3e4f5a6c8d0e1f2a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5
 * Buffers may 2048 bytes... never mind; see the .text section 7 notes. */
int new_clean(void) {
    return 3;
}
EOF
run 0 "$LINT" --base base

# ── 4. FORWARD-ONLY: the pre-existing corpus never gates ─────────────────────
# 4a. Untouched offender file, unrelated edit elsewhere (worktree change).
printf '\nint other_answer_two(void) {\n    return 8;\n}\n' >> src/other.c
run 0 "$LINT" --base base

# 4b. Even EDITING the offender file only gates the added lines: append a
#     clean function to legacy.c; its dated header comment stays exempt.
printf '\nint legacy_answer_two(void) {\n    return 43;\n}\n' >> src/legacy.c
run 0 "$LINT" --base base

# 4c. ...and an added line in a tracked file IS gated, at the right line.
badline_at=$(( $(wc -l < src/other.c) + 1 ))
printf '/* audited 2026-08-16 */\n' >> src/other.c
run 1 "$LINT" --base base
printf '%s\n' "$OUT" | grep -q "src/other.c:$badline_at: \[date-iso\]" \
    || { printf '%s\n' "$OUT" >&2; fail "tracked-file addition not flagged at src/other.c:$badline_at"; }
"${GITC[@]}" checkout -q -- src/other.c src/legacy.c

# ── 5. --all is an inventory, never a gate: sees the corpus, exits 0 ─────────
run 0 "$LINT" --all
printf '%s\n' "$OUT" | grep -q 'src/legacy.c:1: \[date-iso\]' \
    || { printf '%s\n' "$OUT" >&2; fail "--all report does not list the legacy offender"; }

# ── 6. Fail closed: no resolvable base, and no repo, both refuse (exit 2) ────
run 2 "$LINT" --base no-such-ref
mkdir -p "$WORK/notrepo"
cd "$WORK/notrepo"
GIT_CEILING_DIRECTORIES="$WORK" run 2 "$LINT"
cd "$REPO"

# ── 7. NEGATIVE CONTROL: naive extractor must fail the string-literal test ───
# Mutate the marked seam so the lint greps the whole file the way both
# historical burns did, assert the mutation applied, and watch test 2 fail.
MUT1="$WORK/mutant_naive.py"
sed 's/^VIEW = comment_view$/VIEW = naive_view/' "$LINT" > "$MUT1"
grep -q '^VIEW = naive_view$' "$MUT1" || fail "control 1: mutation did not apply (seam moved?)"
run 1 "$MUT1" --base base       # string literals now (wrongly) flagged
run 0 "$LINT" --base base       # restored: the real lint stays green

# ── 8. NEGATIVE CONTROL: a whole-tree gate must fail the forward-only test ───
MUT2="$WORK/mutant_tree.py"
sed 's/^GATE_SCOPE = "diff"$/GATE_SCOPE = "tree"/' "$LINT" > "$MUT2"
grep -q '^GATE_SCOPE = "tree"$' "$MUT2" || fail "control 2: mutation did not apply (seam moved?)"
run 1 "$MUT2" --base base       # legacy corpus now (wrongly) gates
printf '%s\n' "$OUT" | grep -q 'src/legacy.c' \
    || { printf '%s\n' "$OUT" >&2; fail "control 2 fired, but not on the legacy corpus"; }
run 0 "$LINT" --base base       # restored: forward-only holds

echo "comment lint contract: 9 banned forms flag, string literals do not, the"
echo "corpus is exempt at line granularity, --all only reports, missing base"
echo "refuses, and both negative controls failed on cue."
