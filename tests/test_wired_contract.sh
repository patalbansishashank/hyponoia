#!/usr/bin/env bash
# Contract (G6): nothing ships unwired — and the count reaches zero.
#
# ─── What this gate exists to stop ─────────────────────────────────────
#
# THE REPOSITORY ASSERTS SOMETHING EXISTS THAT NO EXECUTION HAS EVER TOUCHED.
# Three surfaces, one class, all three observed in this tree:
#
#   A. A MODULE WITH NO PRODUCTION CALLER. An adversarial audit found every
#      merged Track C/D/F module had zero callers from any CLI, MCP handler,
#      daemon or watcher: correct in isolation, inert in composition. A module
#      with no callers cannot diverge from its callers, so its own suite is
#      green forever. Worse than uncalled is NEVER COMPILED: a module in no
#      source list, which the build has not once fed to a compiler, and whose
#      own suite is registered in no runner either. A caller-only check cannot
#      see that case at all — it has no symbols to call because it has no
#      object file — so this one checks COMPILED AT ALL first.
#   B. A COMMAND THE DISPATCHER SWALLOWED. `hyp onboard` was dispatched in
#      main.c, absent from the daemon's role table, fell through to MCP_CLIENT
#      and exited 0 with no output — a defect whose signature is identical to
#      success. H3 collapsed the two enumerations onto HYP_CLI_COMMAND_SURFACE.
#      The census then found a THIRD: print_help(), already stale in
#      production, with `allow-root` and `daemon` reachable and in no help
#      output. That is the half a user sees, and it is the half that failed
#      silently.
#   C. A SCHEMA ARGUMENT WITH NO HANDLER — see tests/test_tool_surface.c,
#      `tool_surface_every_advertised_property_is_one_the_handler_accepts`. It
#      lives there and not here because it must assert what a CLIENT reads back
#      from a live server, which no static scan can do.
#
# ─── Why a RATCHET, and the exit condition ─────────────────────────────
#
# Turned on absolutely today this either fails the build or ships a dozen
# exemptions, and a dozen-entry exemption list is a checklist wearing a gate's
# clothes. So the shape is A10's divergence ledger: THE CURRENT COUNT IS
# PINNED. A NEW VIOLATION FAILS THE BUILD, NAMING IT. Removing one and failing
# to strike its row ALSO fails, naming the row to delete — that is the ratchet,
# and it is the only reason the count can converge.
#
#   EXIT CONDITION: the ledger below reaches zero rows, the pinned count
#   reaches 0, and this gate becomes absolute. G6 is what says we finished;
#   this file is what stops the regression at merge time.
#
# ─── Derive, never enumerate ───────────────────────────────────────────
#
# Six enumerations were found incomplete in a single night — four source lists,
# four tool enumerations that turned out to be six, three command enumerations —
# and twice the missed entry was the most user-facing one. So:
#
#   * the module set is `find src -name '*.c'`, not a list;
#   * "compiled at all" is every file-origin variable in Makefile.hyp, expanded
#     BY MAKE ITSELF through $(.VARIABLES), not by re-parsing the file;
#   * the command set is the X() rows of HYP_CLI_COMMAND_SURFACE;
#   * the role set is the enumerators of hyp_daemon_process_role_t.
#
# The EXEMPTIONS are the one hand-written thing here, and that is deliberate:
# an exemption is a stated fact with an owner, and each row below names the
# unit that will close it. Absent means look elsewhere; empty means there is
# nothing. Only one is ever true.
#
# ─── Fail closed ───────────────────────────────────────────────────────
#
# A check that cannot run REFUSES; it never passes. security-network.sh passed
# by being absent and security-audit.sh carries a sub-check that cannot fail —
# both in this repository. Every derivation below asserts it found something
# before it asserts anything about what it found.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

fail() {
    echo "FAIL (wired contract): $*" >&2
    exit 1
}

command -v python3 >/dev/null 2>&1 || fail "python3 is required and was not found; a check that cannot run refuses"
command -v make >/dev/null 2>&1 || fail "make is required and was not found; a check that cannot run refuses"

for f in Makefile.hyp src/main.c src/cli/command_surface.h src/daemon/bootstrap.h; do
    [ -f "$f" ] || fail "missing $f — cannot derive the sets this gate is about"
done
[ -d src ] || fail "no src/ directory — cannot derive the module set"

# Uniquely-prefixed scratch, cleaned on every exit path.
HYP_G6_TMP="$(mktemp -d "${TMPDIR:-/tmp}/hyp_g6_wired.XXXXXX")"
trap 'rm -rf "$HYP_G6_TMP"' EXIT

# The one thing make can tell us that re-parsing Makefile.hyp cannot: the fully
# expanded value of every variable the makefile itself defines. $(.VARIABLES) is
# make's own list, so a source list added tomorrow is included without this file
# learning its name. `origin == file` excludes make's built-ins and the
# environment, both of which would otherwise drag in unrelated paths.
cat > "$HYP_G6_TMP/vars.mk" <<'MK'
hyp-g6-print-file-vars:
	@:$(foreach v,$(.VARIABLES),$(if $(filter file,$(origin $(v))),$(info $($(v)))))
MK

if ! make -f Makefile.hyp -f "$HYP_G6_TMP/vars.mk" hyp-g6-print-file-vars \
        >"$HYP_G6_TMP/vars.txt" 2>"$HYP_G6_TMP/vars.err"; then
    sed -n '1,20p' "$HYP_G6_TMP/vars.err" >&2 || true
    fail "make could not expand Makefile.hyp's variables; the compiled-at-all check cannot run"
fi

python3 - "$ROOT" "$HYP_G6_TMP/vars.txt" "${HYP_TEST_BINARY:-}" <<'PY'
import os
import re
import subprocess
import sys

ROOT, VARS, BINARY = sys.argv[1], sys.argv[2], sys.argv[3]

failures = []


def fail(msg):
    failures.append(msg)


# ══════════════════════════════════════════════════════════════════════
#  THE LEDGER
#
#  Every row is a STATED exemption with an owner. Two rules keep it from
#  becoming a checklist:
#
#    1. a violation NOT in this table fails the build, naming it;
#    2. a row in this table that is no longer a violation ALSO fails the
#       build, naming the row to strike.
#
#  (2) is the ratchet. Without it the count can only ever stay where it is,
#  because nobody is told when they have earned a decrement.
#
#  `unit` is the Phase-1 unit that will wire the module. DECISION means no
#  unit owns it: it is shelf-ware awaiting a decision to wire it or delete it,
#  and pretending it has an owner would be the same lie as a silent exemption.
# ══════════════════════════════════════════════════════════════════════

# module path -> (closing unit, what is missing)
NEVER_COMPILED_LEDGER = {
    'src/foundation/mem_profile.c': (
        'DECISION',
        'allocation-site profiler for the #581 harness. Its only caller is '
        'mem_override_posix.c, which is itself never compiled, so the whole leg is '
        'shelf-ware.'),
    'src/foundation/mem_override_posix.c': (
        'DECISION',
        'the POSIX half of the mimalloc --wrap interposer. MIMALLOC_WRAP_FLAGS_POSIX '
        'is computed on Linux and referenced by no LDFLAGS, so Makefile.hyp\'s '
        '"Linux wraps a smaller set for MEASUREMENT" describes something that does '
        'not happen. Its Windows counterpart IS wired, by the linker, and this gate '
        'sees that -- see the --wrap rule below.'),
}

UNREACHABLE_LEDGER = {
    # ── Phase 1 arrivals: an unfinished half of a merged unit ──────────
    'src/ask/record_vectors.c': (
        'F1', 'the content-hash-keyed vector store has no ship/receive caller.'),
    'src/discover/role.c': (
        'A6', 'the one role resolver; hyp_cmd_onboard reimplemented it instead of '
              'calling it, which is the F-3 finding.'),
    'src/feed/feed.c': (
        'D4', 'the pull-source boundary and the set-of-origins completeness audit; '
              'nothing production-side pulls yet.'),
    'src/feed/multica_adapter.c': (
        'D3', 'the first adapter behind the feed boundary; the live libpq wire is '
              'D3\'s stated follow-up.'),
    'src/ingest/transcript_ingest.c': (
        'D1', 'pull/scrub/build/merge exists; no production trigger drives it.'),
    'src/ingest/watched_ingest.c': (
        'D5', 'the on-disk feed format parses; the inotify/kqueue trigger is the '
              'watcher\'s, and does not call it.'),
    # src/memory/anchor.c and src/memory/orphan.c were BOTH here, tagged C8u,
    # as a dead cluster: anchor.c reached only from orphan.c, orphan.c reached
    # by nobody. They are gone from this ledger because they are wired --
    # src/mcp/mcp.c calls hyp_anchor_resolve from record_memory's handler and
    # hyp_orphan_view_build from search_memory's, in the same commit that put
    # `anchor` and `status` back into both schemas. Two of the three was the
    # divergence; this is the third.
    # ── Pre-existing: predates Phase 1, no unit owns it ────────────────
    'src/foundation/str_intern.c': (
        'DECISION', 'added 2026-03-15, in FOUNDATION_SRCS, called by tests only.'),
    'src/pipeline/pass_compile_commands.c': (
        'DECISION', 'compile_commands.json parsing, added 2026-03-15, called by '
                    'tests only.'),
    'src/pipeline/pass_envscan.c': (
        'DECISION', 'environment-URL scanner, added 2026-03-15, called by tests only.'),
    'src/semantic/ask_prefix.c': (
        'DECISION', 'a SECOND implementation of the `ask` instruct prefix. The '
                    'encoder renders its own (src/ask/ask_llama.c, ask_encoder.h), so '
                    'the frozen wording here is never what ships. Two ends, one dead.'),
    'src/traces/traces.c': (
        'DECISION', 'OTLP trace processing. The `ingest_traces` MCP tool is LIVE and '
                    'advertised, and its handler neither includes traces.h nor calls '
                    'one symbol from it -- it counts the array and answers '
                    '"not yet implemented".'),
}

# ── Check A · Modules ─────────────────────────────────────────────────
#
# SET: every .c under src/. Derived by walking the tree.
#
# TEST 1 — COMPILED AT ALL. The file appears in the expansion of some
#   Makefile.hyp variable. It runs FIRST because a never-compiled module has no
#   callers for a trivial reason — no object file, so no symbol to call — and a
#   caller-only check would file it as one ordinary unwired module among many
#   instead of as a file the build has never seen. The distinction is not
#   cosmetic: nothing has type-checked such a file, so it can be broken outright
#   and stay green, and a repo-wide sweep can edit it into not compiling at all
#   without a single gate noticing.
#
# TEST 2 — REACHABLE FROM THE PRODUCTION ENTRY POINT. NEXT-STEPS.md §4 words
#   this as "at least one non-test caller". That is too weak, and this tree
#   contains two live counter-examples: src/memory/anchor.c is called by
#   src/memory/orphan.c, and src/feed/feed.c by src/ingest/transcript_ingest.c —
#   and neither caller has a caller. Both dead clusters pass "has a non-test
#   caller" and neither is wired to anything a user can run. So the property is
#   REACHABILITY from src/main.c, which is what "wired" means.
#
# The graph: a file-level edge f -> g when f mentions a token that g exports.
# Exports are non-static top-level definitions; comments and string literals
# are blanked first, so a name in an error message is not a call. internal/hyp
# is a RELAY, not a subject — its 470-odd grammar_*.c are three-line vendored
# tree-sitter shims picked up by $(wildcard), where "in a source list" is
# vacuous and "has a caller" is answered by a generated registry rather than by
# a call. It participates in the graph so that a src/ module wired only from
# the extraction layer is correctly seen as wired, and it is not judged.

KEYWORDS = {
    "static", "typedef", "return", "if", "while", "for", "switch", "sizeof",
    "else", "do", "case", "goto", "extern", "inline", "break", "continue",
}


def blank_comments_and_literals(text):
    """Replace comment and string/char literal bodies with spaces.

    Byte offsets and line numbers are preserved so the result can still be
    scanned line-wise. A symbol named inside a diagnostic string is not a
    reference to it, and a function named in a comment is not a definition of
    it -- both would be edges this gate must not invent.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c in '"\'':
            quote = c
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == quote or text[j] == '\n':
                    break
                j += 1
            j = min(j + 1, n)
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def blank_comments_keeping_strings(text):
    """Blank comment bodies only. The command table's tokens ARE string
    literals, and a row commented out is not a row -- so comments have to go
    and strings have to stay."""
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    return re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), text)


DEF_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_ \t*]*?)([A-Za-z_][A-Za-z0-9_]*)[ \t]*([(=\[])')
TOKEN_RE = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')


def exported_symbols(blanked):
    """Non-static top-level definitions: functions and file-scope objects.

    Column zero plus at least two tokens before the '(' is what separates a
    definition from a macro invocation such as _Static_assert(...) or a table
    expansion at file scope, either of which would otherwise be published as an
    export and could invent an edge.
    """
    found = set()
    for line in blanked.split('\n'):
        if not line[:1].isalpha() and not line[:1] == '_':
            continue
        m = DEF_RE.match(line)
        if not m:
            continue
        prefix, name, kind = m.group(1), m.group(2), m.group(3)
        words = prefix.replace('*', ' ').split()
        if not words or words[0] in KEYWORDS:
            continue
        if any(w in ('static', 'typedef', 'extern') for w in words):
            continue
        if kind == '(' and line.rstrip().endswith(';'):
            continue  # a forward declaration is not a definition
        found.add(name)
    return found


modules = []
for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, 'src')):
    for name in filenames:
        if name.endswith('.c'):
            rel = os.path.relpath(os.path.join(dirpath, name), ROOT)
            modules.append(rel.replace(os.sep, '/'))
modules.sort()
if not modules:
    fail("check A found no modules under src/ — the derivation is broken, not the tree")

relay_dir = os.path.join(ROOT, 'internal', 'hyp')
relays = sorted('internal/hyp/' + f for f in os.listdir(relay_dir)
                if f.endswith('.c')) if os.path.isdir(relay_dir) else []
translation_units = modules + relays

with open(VARS, encoding='utf-8', errors='replace') as fh:
    expanded = fh.read()
compiled = {t for t in expanded.split() if t.startswith('src/') and t.endswith('.c')}
if not compiled:
    fail("check A read no src/*.c out of Makefile.hyp's expanded variables — "
         "the compiled-at-all derivation is broken, not the tree")
compiled |= set(relays)

exports, references = {}, {}
for unit in translation_units:
    with open(os.path.join(ROOT, unit), encoding='utf-8', errors='replace') as fh:
        blanked = blank_comments_and_literals(fh.read())
    exports[unit] = exported_symbols(blanked)
    references[unit] = set(TOKEN_RE.findall(blanked))

owner = {}
for unit in translation_units:
    for symbol in exports[unit]:
        owner.setdefault(symbol, set()).add(unit)

edges = {unit: set() for unit in translation_units}
for unit in translation_units:
    for symbol in references[unit]:
        for defining in owner.get(symbol, ()):
            if defining != unit:
                edges[unit].add(defining)

# THE LINKER IS A CALLER TOO. src/foundation/mem_override_win.c defines
# __wrap_malloc and friends; no .c will ever name them, because the symbol is
# bound by -Wl,--wrap= at link time. Reading that as "no caller" would put a
# correctly wired module in the ledger, and an exemption for a thing that is
# not a defect is how a ledger stops meaning anything.
#
# The wrap set is DERIVED, and derived platform-independently: the flag itself
# is built inside an ifeq, so its expansion differs per machine and a gate that
# read the expansion would give two answers. The SYMBOL LIST is unconditional,
# so the foreach that builds the flag is matched in Makefile.hyp's text and its
# list variable is taken from make's own expansion.
with open(os.path.join(ROOT, 'Makefile.hyp'), encoding='utf-8', errors='replace') as fh:
    makefile_src = fh.read()


wrap_lists = re.findall(
    r'\$\(foreach\s+([A-Za-z_][A-Za-z0-9_]*)\s*,\s*\$\(([A-Za-z_][A-Za-z0-9_]*)\)\s*,'
    r'[^\n]*?--wrap=\$\(\1\)', makefile_src)
wrapped = set(re.findall(r'--wrap=([A-Za-z_][A-Za-z0-9_]*)', makefile_src))
for _loop_var, list_var in wrap_lists:
    m = re.search(r'^' + list_var + r'\s*:?=(.*(?:\\\n.*)*)$', makefile_src, re.M)
    if m:
        wrapped |= set(re.findall(r'[A-Za-z_][A-Za-z0-9_]*',
                                  m.group(1).replace('\\\n', ' ')))
if '--wrap' in makefile_src and not wrapped:
    fail("Makefile.hyp asks the linker to --wrap symbols and check A derived none "
         "of them; every link-wired module would read as uncalled and this check "
         "refuses")
linker_bound = {'__wrap_' + sym for sym in wrapped}

ENTRY = 'src/main.c'
if ENTRY not in modules:
    fail("check A cannot find %s — the production entry point moved and this "
         "gate must be re-pointed before it means anything" % ENTRY)
    reachable = set()
else:
    roots = [ENTRY] + [m for m in modules
                       if m in compiled and (exports[m] & linker_bound)]
    reachable, stack = set(roots), list(roots)
    while stack:
        for nxt in edges.get(stack.pop(), ()):
            if nxt not in reachable and nxt in compiled:
                reachable.add(nxt)
                stack.append(nxt)

# The detector's own positive control. exported_symbols() and the reachability
# walk are otherwise only ever exercised by assertions expecting agreement,
# so an extractor that returned nothing would green every module in the tree.
# Plant a module that exports a symbol nobody mentions and require the walk to
# say so; plant one the entry point does mention and require it not to.
_probe_wired, _probe_orphan = 'src/hyp_g6_probe_wired.c', 'src/hyp_g6_probe_orphan.c'
_probe_exports = {
    _probe_wired: exported_symbols(blank_comments_and_literals(
        'int hyp_g6_probe_called_from_main(void) { return 0; }\n')),
    _probe_orphan: exported_symbols(blank_comments_and_literals(
        'int hyp_g6_probe_called_by_nobody(void) { return 0; }\n')),
}
if _probe_exports[_probe_wired] != {'hyp_g6_probe_called_from_main'}:
    fail("check A's export extractor did not find a plain non-static definition; "
         "every module in this tree would read as having nothing to call")
if _probe_exports[_probe_orphan] != {'hyp_g6_probe_called_by_nobody'}:
    fail("check A's export extractor did not find a plain non-static definition; "
         "every module in this tree would read as having nothing to call")
_probe_edges = dict(edges)
_probe_edges[ENTRY] = set(edges.get(ENTRY, ())) | {_probe_wired}
_probe_edges[_probe_wired] = set()
_probe_edges[_probe_orphan] = set()
_probe_compiled = compiled | {_probe_wired, _probe_orphan}
_probe_seen, _stack = {ENTRY}, [ENTRY]
while _stack:
    for nxt in _probe_edges.get(_stack.pop(), ()):
        if nxt not in _probe_seen and nxt in _probe_compiled:
            _probe_seen.add(nxt)
            _stack.append(nxt)
if _probe_wired not in _probe_seen:
    fail("check A's reachability walk lost a module the entry point calls — "
         "it would report wired modules as unwired")
if _probe_orphan in _probe_seen:
    fail("check A's reachability walk reached a module nothing calls — "
         "it cannot report an unwired module and every count below is vacuous")

never_compiled = [m for m in modules if m not in compiled]
unreachable = [m for m in modules if m in compiled and m not in reachable]


def judge(observed, ledger, headline, remedy):
    new = [m for m in observed if m not in ledger]
    stale = [m for m in ledger if m not in observed]
    for m in new:
        fail("%s: %s\n      %s" % (headline, m, remedy))
    for m in stale:
        fail("%s is no longer unwired — STRIKE its row from the ledger in "
             "tests/test_wired_contract.sh.\n      Leaving it there pins the count at a "
             "number the tree has already beaten, and the count is the only thing that "
             "can converge." % m)
    return len(observed)


count_never = judge(
    never_compiled, NEVER_COMPILED_LEDGER,
    "a module under src/ is in NO source list and has never been compiled",
    "Add it to a source list, or delete it. A file the build has never seen "
    "cannot be tested by its own suite passing.")
count_unreachable = judge(
    unreachable, UNREACHABLE_LEDGER,
    "a module under src/ is compiled and UNREACHABLE from src/main.c",
    "Wire it to something a user can run, or state it in the ledger with the "
    "unit that will. Being called only by another unreachable module is not "
    "being wired.")

print("check A · modules: %d under src/, %d compiled, %d reachable from %s"
      % (len(modules), len(compiled & set(modules)), len(reachable & set(modules)), ENTRY))
print("           pinned: %d never compiled, %d unreachable (exit condition: 0 and 0)"
      % (count_never, count_unreachable))

# ── Check B · Commands ────────────────────────────────────────────────
#
# SET: the X() rows of HYP_CLI_COMMAND_SURFACE, plus the enumerators of
# hyp_daemon_process_role_t. Both read out of the headers that define them.
#
# H3 already holds the two ends of the table together from C — every row
# classifies as its stated role, the dispatcher is expanded rather than
# written, tokens are pairwise distinct. What follows is what a C test in that
# binary cannot see, and it is the direction a user sees:
#
#   B1  print_help() is EXPANDED FROM THE TABLE and lists no command of its
#       own. print_help was the third enumeration and it was already stale in
#       production: `allow-root` and `daemon` were reachable and in no help
#       output. A generated help block cannot go stale; a hand-written line
#       beside the expansion can, and nothing else looks for one.
#   B2  every role the classifier can return is BRANCHED ON in main(). The
#       observed defect was a role table missing a command; the next one is an
#       enumerator main() never tests, which lands in whatever branch is last.
#   B3  the binary's OWN --help output names every command whose row carries a
#       usage block. Asserted against stdout, never against the table's usage
#       column — asserting the column is asserting what the checker controls.

surface_h = os.path.join(ROOT, 'src/cli/command_surface.h')
with open(surface_h, encoding='utf-8', errors='replace') as fh:
    surface_src = fh.read()

table = re.search(r'#define\s+HYP_CLI_COMMAND_SURFACE\(X\)(.*?)\n/\* clang-format on \*/',
                  surface_src, re.S)
if not table:
    fail("check B could not find HYP_CLI_COMMAND_SURFACE in %s; the command set "
         "cannot be derived and this check refuses" % surface_h)
    rows = []
else:
    # The rows carry backslash-newline continuations; join them so one row is
    # one string, then read the six columns positionally.
    flat = re.sub(r'\\\s*\n\s*', ' ', table.group(1))
    flat = blank_comments_keeping_strings(flat)
    rows = re.findall(
        r'X\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([^"]*)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,'
        r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(NULL|")',
        flat)
if len(rows) < 2:
    fail("check B derived %d command rows from HYP_CLI_COMMAND_SURFACE; a table "
         "with fewer than two rows means the parse failed, not that the CLI has "
         "one command" % len(rows))

documented = [r[1] for r in rows if r[5] != 'NULL']
internal = [r[1] for r in rows if r[5] == 'NULL']
if not documented:
    fail("check B found no command row carrying a usage block — either every "
         "command is undocumented or the usage column moved")

with open(os.path.join(ROOT, 'src/main.c'), encoding='utf-8', errors='replace') as fh:
    main_src = fh.read()
main_blanked = blank_comments_and_literals(main_src)

# B1 · print_help is generated, and lists nothing of its own.
help_fn = re.search(r'\nstatic void print_help\(void\)\s*\{(.*?)\n\}\n', main_src, re.S)
if not help_fn:
    fail("check B could not find print_help() in src/main.c under that name; the "
         "help surface moved and this check refuses rather than passing")
else:
    body = help_fn.group(1)
    if 'HYP_CLI_COMMAND_SURFACE(' not in body:
        fail("print_help() no longer expands HYP_CLI_COMMAND_SURFACE. It is then a "
             "second list of commands, which is exactly the state in which "
             "`allow-root` and `daemon` were reachable and undocumented.")
    # Every usage block lives in command_surface.h, so a `hyponoia <command>`
    # line spelled HERE is hand-written by construction. The bare no-argument
    # line is not a command and is excluded by requiring a command-shaped word:
    # a lower-case token or a flag, never prose.
    handwritten = re.findall(r'hyponoia\s+(-{0,2}[a-z][a-z0-9-]*)', body)
    handwritten = [w for w in handwritten if not w.startswith('--ui')
                   and not w.startswith('--port') and not w.startswith('--tool-profile')]
    if handwritten:
        fail("print_help() spells a command listing of its own beside the "
             "generated one — a fourth enumeration of the command surface: "
             + ", ".join(sorted(set(handwritten))[:8]) +
             "\n      Every usage block belongs in the `usage` column of "
             "HYP_CLI_COMMAND_SURFACE, where both ends read it.")

# B2 · every role the classifier can return is branched on in main().
with open(os.path.join(ROOT, 'src/daemon/bootstrap.h'), encoding='utf-8', errors='replace') as fh:
    bootstrap_h = fh.read()
roles = re.findall(r'\b(HYP_DAEMON_PROCESS_[A-Z_]+)\b',
                   blank_comments_and_literals(bootstrap_h))
roles = sorted(set(roles))
if len(roles) < 3:
    fail("check B derived %d process roles from bootstrap.h; the enum moved and "
         "this check refuses" % len(roles))
unhandled = [r for r in roles if not re.search(r'\brole\s*==\s*' + r + r'\b', main_blanked)]
if unhandled:
    fail("a process role the classifier can return is never branched on in "
         "src/main.c, so an argv that selects it lands in whatever branch is "
         "last: " + ", ".join(unhandled))

# B3 · the binary's own --help names every documented command.
if BINARY:
    if not (os.path.isfile(BINARY) and os.access(BINARY, os.X_OK)):
        fail("HYP_TEST_BINARY=%s is not an executable file; the help half cannot be "
             "checked against what a user sees and this check refuses" % BINARY)
    else:
        proc = subprocess.run([BINARY, '--help'], capture_output=True, text=True,
                              stdin=subprocess.DEVNULL, timeout=120)
        printed = (proc.stdout or '') + (proc.stderr or '')
        if not printed.strip():
            fail("`%s --help` printed nothing. Exit 0 with no output is the exact "
                 "signature `hyp onboard` shipped with." % BINARY)
        missing = [t for t in documented
                   if not re.search(r'(?<![A-Za-z0-9_-])' + re.escape(t) + r'(?![A-Za-z0-9_-])',
                                    printed)]
        if missing:
            fail("a command carries a usage block in HYP_CLI_COMMAND_SURFACE and "
                 "does NOT appear in what `--help` prints: " + ", ".join(missing) +
                 "\n      This is the half a user sees, and the half that failed "
                 "silently before.")
    print("check B · commands: %d rows, %d documented, %d internal by declaration, "
          "%d roles branched; --help checked against the built binary"
          % (len(rows), len(documented), len(internal), len(roles)))
else:
    print("check B · commands: %d rows, %d documented, %d internal by declaration, "
          "%d roles branched; --help leg deferred to the step that has a binary"
          % (len(rows), len(documented), len(internal), len(roles)))

if failures:
    print()
    print("=" * 72)
    for f in failures:
        print("FAIL (wired contract): " + f)
    print("=" * 72)
    print("Every line above is a FINDING. Do not widen the ledger to green the "
          "build without naming the unit that will close the row.")
    sys.exit(1)
PY

echo "OK: nothing ships unwired (checks A and B)"
