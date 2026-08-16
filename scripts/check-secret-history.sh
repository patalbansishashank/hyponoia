#!/usr/bin/env bash
# Refuse to push a vendor API key — searching HISTORY, not the worktree.
#
# NEXT-STEPS.md §2.13. This exists because the obvious version of it did not
# work. `tests/test_ask_provider.c` carried a LIVE Voyage key from c2c5d065
# until §2.13, on `main` and on `origin`, inside the test for the guard that
# refuses a key pasted where an environment variable name belongs. A scan for
# `key = "..."` over the checked-out tree came back clean, twice, for two
# reasons that this script is built around:
#
#   1. THE KEY WAS A BARE FUNCTION ARGUMENT, not an assignment. Anything keyed
#      on `=` misses it. So the match is on VENDOR PREFIX plus a plausible key
#      body, and it does not care what syntax surrounds it.
#   2. THE WORKTREE IS THE WRONG PLACE TO LOOK. A key deleted in the last
#      commit is still in every clone, and `git push` publishes history, not
#      the worktree. So this walks commits — `git log -G`, git's pickaxe — and
#      never greps a file on disk.
#
# THE PREFIX LIST IS NOT DUPLICATED HERE. It is parsed out of
# src/foundation/scrub.c's hyp_vendor_prefixes — the one table that
# hyp_config_validate() matches on when it refuses a key in `key_env` and that
# the transcript scrub (D2) replaces before a record id can bind the text.
# (It lived in src/cli/cli.c until D2 moved it to foundation so the scrub could
# link it.) Two copies of that list means one of them is wrong later, and it
# would be this one. Same for the body threshold: HYP_SCRUB_MIN_BODY is parsed
# out of src/foundation/scrub.h. If either parse fails this script FAILS — it
# never falls back to a value of its own.
#
#   check-secret-history.sh                 scan the commits a push would send
#   check-secret-history.sh --all           sweep every commit in the repository
#   check-secret-history.sh --selftest      prove it still catches c2c5d065
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX_SRC="$ROOT/src/foundation/scrub.c"
SCRUB_H="$ROOT/src/foundation/scrub.h"

# The commit that carried the live key, kept as the self-test fixture. If this
# ever stops being reachable the self-test says so rather than passing quietly.
KNOWN_LEAK_COMMIT="c2c5d065"

# ── What a hit is allowed to be ─────────────────────────────────────
#
# Judged BY CONTENT, not by commit id. Acknowledging commit SHAs would be
# brittle (a rebase renumbers them) and, worse, it would hide a NEW key
# introduced in an OLD commit — the reviewer sees a familiar SHA and moves on.
#
# 1. Synthetic fixtures, listed as the literal string. These are test data for
#    hyp_is_secret_value()/hyp_is_secret_binding() and were never anybody's
#    key: sequential digits and "abcdef" are the giveaway.
#    Both are the same fixture family, from the Go era and carried through the
#    C rewrite: counting digits followed by counting letters. Nobody's key
#    looks like this, which is the point of listing the LITERAL rather than the
#    commit — a real key added to one of these commits would still be caught.
KNOWN_FIXTURES=(
    "sk-1234567890abcdef12345"
    "sk-1234567890abcdefghijkl"
)
# 2. The one real key this repository ever committed, at c2c5d065, removed at
#    0e30048f. IT IS ACKNOWLEDGED BY COMMIT because the alternative is writing
#    the key into this file to allowlist it, which is the exact mistake being
#    fixed. THE KEY IS REVOKED — that, not this entry, is what makes it safe.
#    Rewriting history to excise it was considered and rejected: it was already
#    pushed, so rotation was mandatory regardless, and a rewrite would break
#    every clone and every SHA cited across the run records.
ACKNOWLEDGED_REVOKED=(
    "c2c5d065"   # introduced it
    "0e30048f"   # replaced it with filler (§2.13)
    "d9a65a54"   # the squash of that fix onto main
)

die() { printf '\n[secret-history] %s\n' "$*" >&2; exit 1; }

# ── The prefix list, read from the product ──────────────────────────
extract_prefixes() {
    [ -f "$PREFIX_SRC" ] || die "cannot read $PREFIX_SRC — the prefix list lives there, and this script refuses to invent one"
    local line
    line="$(grep -m1 'hyp_vendor_prefixes\[\] *=' "$PREFIX_SRC" || true)"
    [ -n "$line" ] || die "hyp_vendor_prefixes not found in src/foundation/scrub.c. It was renamed or moved; point this script at the new definition rather than hardcoding the list."
    # "sk-", "pa-", ... -> one per line
    printf '%s\n' "$line" | grep -oE '"[^"]+"' | tr -d '"'
}

# A vendor prefix followed by enough body to be a key rather than a word. The
# threshold is HYP_SCRUB_MIN_BODY in src/foundation/scrub.h — the scrub's own
# rule, so "what this scanner refuses to push" and "what the scrub redacts
# from a transcript" can never disagree. scrub.h documents the number's
# reasoning (under every real key, over the filler fixtures).
extract_min_body() {
    [ -f "$SCRUB_H" ] || die "cannot read $SCRUB_H — the body threshold lives there, and this script refuses to invent one"
    local n
    n="$(grep -m1 -E '^#define HYP_SCRUB_MIN_BODY [0-9]+' "$SCRUB_H" | grep -oE '[0-9]+' || true)"
    [ -n "$n" ] || die "HYP_SCRUB_MIN_BODY not found in src/foundation/scrub.h. It was renamed or moved; point this script at the new definition rather than hardcoding the threshold."
    printf '%s' "$n"
}

build_regex() {
    local p out=""
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        # Escape regex metacharacters in the prefix ('-' and '_' are literal in
        # this position, but do not assume it).
        local esc
        esc="$(printf '%s' "$p" | sed 's/[][\.*^$(){}?+|/]/\\&/g')"
        out="${out:+$out|}${esc}"
    done
    [ -n "$out" ] || die "no prefixes parsed from hyp_vendor_prefixes"
    # Not preceded by a word character, so "compa-" and "task_" cannot match.
    printf '(^|[^A-Za-z0-9_])(%s)[A-Za-z0-9_-]{%d,}' "$out" "$MIN_BODY"
}

scan() {
    local regex="$1"; shift
    # -G is git's pickaxe over DIFF CONTENT: every commit whose diff added or
    # removed a line matching the pattern. That is what walks history.
    git -C "$ROOT" log -G"$regex" --oneline --no-color "$@" 2>/dev/null || true
}

# ── Judge a set of hits ─────────────────────────────────────────────
# Shared by --all and by the pre-push range scan, and it has to be: the first
# version judged only in --all, so the hook refused the very commit that ADDED
# this file — KNOWN_FIXTURES necessarily contains the strings it allowlists.
# Two code paths meant two verdicts on identical evidence.
#
# Sets: N_FIXTURE, N_REVOKED, UNEXPLAINED (count) and prints each unexplained.
explain_hits() {
    local regex="$1" hits="$2"
    N_FIXTURE=0; N_REVOKED=0; UNEXPLAINED=0
    local line sha subject matches leftovers f ack a
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        sha="${line%% *}"; subject="${line#* }"
        matches="$(git -C "$ROOT" show "$sha" --unified=0 --no-color 2>/dev/null \
                   | grep -aoE "$regex" | sed -E 's/^[^A-Za-z0-9_]//' | sort -u)"
        leftovers="$matches"
        for f in "${KNOWN_FIXTURES[@]}"; do
            leftovers="$(printf '%s\n' "$leftovers" | grep -Fvx "$f" || true)"
        done
        if [ -z "$(printf '%s' "$leftovers" | tr -d '[:space:]')" ]; then
            N_FIXTURE=$((N_FIXTURE + 1)); continue
        fi
        ack=0
        for a in "${ACKNOWLEDGED_REVOKED[@]}"; do
            case "$sha" in "$a"*) ack=1 ;; esac
        done
        if [ "$ack" -eq 1 ]; then
            N_REVOKED=$((N_REVOKED + 1)); continue
        fi
        printf '\n[secret-history] UNEXPLAINED vendor-key-shaped string in %s %s\n' "$sha" "$subject" >&2
        printf '  %d distinct match(es), not a known fixture. Inspect with:\n    git show %s\n' \
            "$(printf '%s\n' "$leftovers" | grep -c . || true)" "$sha" >&2
        UNEXPLAINED=$((UNEXPLAINED + 1))
    done <<< "$hits"
}

main() {
    local mode="${1:-range}"
    local prefixes regex
    prefixes="$(extract_prefixes)"
    MIN_BODY="$(extract_min_body)"
    regex="$(printf '%s\n' "$prefixes" | build_regex)"
    printf '[secret-history] %d prefixes from src/foundation/scrub.c hyp_vendor_prefixes (body >= %s): %s\n' \
        "$(printf '%s\n' "$prefixes" | grep -c .)" "$MIN_BODY" "$(printf '%s' "$prefixes" | tr '\n' ' ')"

    case "$mode" in
    --selftest)
        # A scanner nobody has seen fail is not a scanner. Prove BOTH halves:
        # it finds the known leak, and it is quiet on the current tree.
        if ! git -C "$ROOT" rev-parse --verify --quiet "${KNOWN_LEAK_COMMIT}^{commit}" >/dev/null; then
            die "self-test fixture $KNOWN_LEAK_COMMIT is unreachable; the proof this works is gone"
        fi
        local hits
        hits="$(scan "$regex" --all)"
        if ! printf '%s\n' "$hits" | grep -q "^${KNOWN_LEAK_COMMIT}"; then
            die "SELF-TEST FAILED: the scan did not find the known key in $KNOWN_LEAK_COMMIT. It is no longer proven to detect anything."
        fi
        printf '[secret-history] self-test: FOUND the known key in %s — detection proven\n' "$KNOWN_LEAK_COMMIT"

        # DETECTION IS NOT REFUSAL. The scan finding a commit proves nothing if
        # the judgement then waves it through — and c2c5d065 is on the
        # acknowledged list precisely so it does. So run the judgement again
        # with both allowances emptied and require it to REFUSE. Otherwise the
        # only path ever exercised is the one that says yes.
        local saved_fix=("${KNOWN_FIXTURES[@]}") saved_ack=("${ACKNOWLEDGED_REVOKED[@]}")
        KNOWN_FIXTURES=(); ACKNOWLEDGED_REVOKED=()
        explain_hits "$regex" \
            "$(git -C "$ROOT" log -1 --oneline --no-color "$KNOWN_LEAK_COMMIT")" 2>/dev/null
        local refused="$UNEXPLAINED"
        KNOWN_FIXTURES=("${saved_fix[@]}"); ACKNOWLEDGED_REVOKED=("${saved_ack[@]}")
        if [ "$refused" -ne 1 ]; then
            die "SELF-TEST FAILED: with every allowance removed, $KNOWN_LEAK_COMMIT was still not flagged. The refusal path does not work, so nothing would ever be blocked."
        fi
        printf '[secret-history] self-test: REFUSED %s with allowances emptied — the blocking path works\n' "$KNOWN_LEAK_COMMIT"
        printf '[secret-history] self-test: PASSED (detection + refusal)\n'
        return 0
        ;;
    --all)
        local hits
        hits="$(scan "$regex" --all)"
        explain_hits "$regex" "$hits"
        if [ "$UNEXPLAINED" -gt 0 ]; then
            die "REFUSING: $UNEXPLAINED commit(s) unaccounted for. If any is a real key, ROTATE IT FIRST — it is in history, and history is what a push publishes. Deleting it from the worktree fixes nothing."
        fi
        printf '[secret-history] full history clean — %d commit(s) matched, all explained (%d synthetic fixture, %d revoked-and-acknowledged)\n' \
            "$((N_FIXTURE + N_REVOKED))" "$N_FIXTURE" "$N_REVOKED"
        return 0
        ;;
    range)
        # pre-push feeds "<local ref> <local sha> <remote ref> <remote sha>" on
        # stdin. Scan only what this push would ADD — full history on every push
        # costs ~90 s and the old commits are already published.
        local total=0 unex=0 fix=0 rev=0 lref lsha rref rsha range hits
        while read -r lref lsha rref rsha; do
            [ -n "${lsha:-}" ] || continue
            case "$lsha" in *[!0]*) ;; *) continue ;; esac   # branch deletion
            if [ -z "${rsha:-}" ] || [ "$rsha" = "0000000000000000000000000000000000000000" ]; then
                range="$lsha --not --remotes"
            else
                range="$rsha..$lsha"
            fi
            # shellcheck disable=SC2086
            hits="$(scan "$regex" $range)"
            [ -n "$hits" ] || continue
            explain_hits "$regex" "$hits"
            total=$((total + N_FIXTURE + N_REVOKED + UNEXPLAINED))
            unex=$((unex + UNEXPLAINED)); fix=$((fix + N_FIXTURE)); rev=$((rev + N_REVOKED))
        done
        if [ "$unex" -gt 0 ]; then
            die "REFUSING TO PUSH. If it is a real key: ROTATE IT FIRST, then rewrite. If it is a fixture, shorten the body — the prefix is all a test needs."
        fi
        if [ "$total" -gt 0 ]; then
            printf '[secret-history] push range clean — %d commit(s) matched, all explained (%d synthetic fixture, %d revoked-and-acknowledged)\n' "$total" "$fix" "$rev"
        else
            printf '[secret-history] push range clean\n'
        fi
        return 0
        ;;
    *)
        die "unknown mode: $mode (expected --all, --selftest, or nothing)"
        ;;
    esac
}

main "$@"
