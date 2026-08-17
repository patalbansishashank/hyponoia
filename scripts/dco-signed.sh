#!/usr/bin/env sh
# The single answer to "is this message signed off by this author?"
#
# Both ends of DCO enforcement call this and nothing else: the commit-msg hook
# at commit time, and check-dco.sh at merge time. They used to be two
# implementations of two definitions — the hook line-matched `^Signed-off-by:`
# while the checker read git's trailer block — and the two agreed until a blank
# line before the Co-Authored-By trailers pushed Signed-off-by out of the
# trailer block. The commit then passed the hook and failed the checker: both
# correct, about different things.
#
# Usage: dco-signed.sh <author-email> <message-file>
#   exit 0  — signed off by that author
#   exit 1  — not signed off by that author (caller prints the guidance)
#
# The definition is the DCO's own: a line certifying origin, anywhere in the
# message, whose email matches the author. Trailer-block adjacency is a git
# implementation detail and not what the certificate requires — but the
# generator is fixed too (see COMMIT-TRAILERS below), so messages satisfy the
# stricter reading as well and stay correct under ecosystem DCO tooling.
set -eu

AUTHOR_EMAIL="${1:?usage: dco-signed.sh <author-email> <message-file>}"
MSG_FILE="${2:?usage: dco-signed.sh <author-email> <message-file>}"

[ -r "$MSG_FILE" ] || exit 1

# Comment lines are stripped: git does that before committing, so a Signed-off-by
# behind a '#' is not in the commit and must not count here either.
grep -v '^#' "$MSG_FILE" \
    | grep -iE '^Signed-off-by:[[:space:]]*.+<.+@.+>' \
    | grep -qiF "<$AUTHOR_EMAIL>"
