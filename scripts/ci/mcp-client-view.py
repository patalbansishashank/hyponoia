#!/usr/bin/env python3
"""mcp-client-view.py [binary] — what an MCP CLIENT actually reads back.

WHY THIS EXISTS, because the unit tests did not catch it.

Every advertised tool used to declare
`outputSchema: {"type":"object","additionalProperties":true}`. That schema
constrains nothing, so its only effect was to tell a client "structuredContent
is the result of this call". But most tools answer in TOON, which is not a JSON
object, so `hyp_mcp_text_result` emitted `structuredContent: {}` and put the
answer in `content[0].text`. A client that honours the declaration — which is
what the declaration is FOR — rendered an empty result.

Measured on the shipped v0.3.0 binary, over the real stdio protocol:
`query_graph`, `search_graph` and `ask` all came back as `{}` while their
payloads (83, 211 and 605 bytes) sat unread in `content`. Those are the three
tools whose entire job is answering questions about code.

Three unit tests and one smoke check asserted the broken shape — one of them
literally required `"structuredContent":{}` and another said outputSchema
"stays mandatory". They all verified what the server EMITS. None asked what a
client READS. That is the same failure as the v0.2.4 UI-pack magic: a test that
shares the product's assumption reports green.

So this script is deliberately a CLIENT, not a unit test: it performs the real
initialize handshake over stdio, calls tools, and applies one rule —

    a result must be reachable. Either structuredContent carries a NON-EMPTY
    object, or it is ABSENT and the answer is in content. A present-but-empty
    structuredContent is a failure, because it is indistinguishable from
    "the answer is nothing".

A SECOND client-visible thing was being checked the same way — by shape rather
than by value. Every tool advertises four annotation hints, and the test
guarding them asserted only that the four KEYS existed: inverting readOnlyHint
and idempotentHint on every row left it green. Those hints decide client
behaviour, and `delete_project` and `search_graph` once carried byte-identical
ones, so nothing told a client which of the two erases a database. So the
values are now compared against the table that declares them, derived from
tool_surface.h rather than listed here.

Exit 0 if every probed tool is reachable and every hint matches, 1 otherwise.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

BINARY = sys.argv[1] if len(sys.argv) > 1 else "build/c/hyponoia"
SURFACE_HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "..", "..", "src", "mcp", "tool_surface.h")

FIXTURE = """\
#include <stdio.h>
static int helper(int a) { return a * 2; }
int compute(int a) { return helper(a) + 1; }
int main(void) { printf("%d\\n", compute(3)); return 0; }
"""


class Client:
    def __init__(self, binary, env, args=(), cwd=None):
        self.p = subprocess.Popen([binary, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env,
                                  cwd=cwd)

    def send(self, obj):
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()

    def read(self):
        while True:
            line = self.p.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                continue
            try:
                return json.loads(line)
            except Exception:
                continue

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        self.p.terminate()


def profile_view(binary, env, project):
    """The restricted profiles as a client sees them.

    `--tool-profile analysis` is what the generated `hyponoia` and
    `hyponoia-auditor` agents run against, `scout` what `hyponoia-scout` runs
    against. `ask` must be advertised by analysis, never by scout, and an
    analysis client that calls it must get a reachable answer — the server
    permitted `ask` on analysis for a whole release while no generated profile
    requested it, and every unit test on both ends passed. Returns a list of
    failures.
    """
    bad = []
    seen = {}
    for profile in ("analysis", "scout"):
        c = Client(binary, env, ("--tool-profile=%s" % profile,))
        c.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                           "clientInfo": {"name": "mcp-client-view", "version": "0"}}})
        if c.read() is None:
            print("FAIL: --tool-profile=%s sent no initialize response" % profile)
            bad.append("profile:%s" % profile)
            c.close()
            continue
        c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        c.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        tools = (((c.read() or {}).get("result")) or {}).get("tools") or []
        names = [t["name"] for t in tools]
        seen[profile] = names
        print("--tool-profile=%-8s advertises %2d tools: %s" % (profile, len(names), ", ".join(names)))
        if profile == "analysis":
            if "ask" not in names:
                print("FAIL: analysis profile does not advertise ask")
                bad.append("analysis:ask-missing")
            c.send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                    "params": {"name": "ask",
                               "arguments": {"project": project, "question": "how is a value doubled"}}})
            resp = c.read() or {}
            res = resp.get("result") or {}
            content = res.get("content") or []
            text = content[0].get("text", "") if content else ""
            sc = res.get("structuredContent")
            if "structuredContent" in res and isinstance(sc, dict) and not sc:
                print("FAIL: analysis ask -> structuredContent {} (renders nothing)")
                bad.append("analysis:ask-empty")
            elif not text and not sc:
                print("FAIL: analysis ask -> no content, no structure")
                bad.append("analysis:ask-unreachable")
            else:
                print("analysis ask -> %d bytes of content, reachable" % len(text))
        else:
            if "ask" in names:
                print("FAIL: scout profile advertises ask; scout is the surface where every tool answers")
                bad.append("scout:ask-present")
            c.send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                    "params": {"name": "ask", "arguments": {"project": project, "question": "x"}}})
            resp = c.read() or {}
            res = resp.get("result") or {}
            if not res.get("isError"):
                print("FAIL: scout profile executed ask instead of refusing it")
                bad.append("scout:ask-callable")
        c.close()
    if "analysis" in seen and "scout" in seen:
        if not set(seen["scout"]) < set(seen["analysis"]):
            print("FAIL: scout is not a strict subset of analysis")
            bad.append("profiles:not-nested")
    return bad


def _strip_c_comments(text):
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def _slice(text, start_marker, end_marker):
    a = text.index(start_marker)
    b = text.index(end_marker, a)
    return text[a:b]


def declared_annotations(header=SURFACE_HEADER):
    """{tool: (readOnly, destructive, idempotent, openWorld)} from the ONE table.

    Derived, never enumerated: both maps come out of the same X-macro rows the
    server expands, so a tool added tomorrow is covered tomorrow. Returns
    (per_tool, error) — error is a string when the header could not be read,
    because a check that silently degrades to checking nothing is the shape
    this whole script exists to catch.
    """
    try:
        with open(header, encoding="utf-8") as f:
            text = _strip_c_comments(f.read())
    except Exception as exc:                                    # noqa: BLE001
        return {}, "could not read %s: %s" % (header, exc)
    try:
        profile_block = _slice(text, "#define HYP_TOOL_ANNOTATION_PROFILES(X)", "typedef enum")
        table_block = _slice(text, "#define HYP_TOOL_SURFACE(X)",
                             "#define HYP_TOOL_SURFACE_ROW_COUNT_ONE")
    except ValueError as exc:                                   # noqa: BLE001
        return {}, "tool_surface.h no longer has the blocks this parses: %s" % exc

    profiles = {}
    for name, ro, de, idem, ow in re.findall(
            r"X\(\s*(HYP_TOOL_ANN_\w+)\s*,\s*(true|false)\s*,\s*(true|false)\s*,"
            r"\s*(true|false)\s*,\s*(true|false)\s*\)", profile_block):
        profiles[name] = tuple(v == "true" for v in (ro, de, idem, ow))
    if not profiles:
        return {}, "no annotation profiles parsed out of tool_surface.h"

    per_tool = {}
    for tool, profile in re.findall(r'X\(\s*"(\w+)"\s*,.*?(HYP_TOOL_ANN_\w+)\s*\)',
                                    table_block, flags=re.S):
        if profile not in profiles:
            return {}, "tool %s names annotation profile %s, which is not declared" % (tool, profile)
        per_tool[tool] = profiles[profile]
    if not per_tool:
        return {}, "no tool rows parsed out of tool_surface.h"
    return per_tool, None


def annotation_view(tools):
    """The four hints, as VALUES, in what a client holds.

    The hints used to be checked only for KEY PRESENCE, which is not a claim
    about any hint: inverting readOnlyHint and idempotentHint on every row left
    the suite green. They are client-visible bytes and they decide client
    BEHAVIOUR — a client that confirms before a destructive call reads exactly
    these — so they are compared against the table that declares them, plus two
    invariants that hold whatever the table says.

    The specific defect this closes: `delete_project` and `search_graph` once
    advertised byte-identical hints, so nothing told a client which of the two
    erases a database.
    """
    bad = []
    declared, err = declared_annotations()
    if err:
        print("FAIL: annotations unverifiable — %s" % err)
        return ["annotations:unreadable"]

    seen = {}
    for t in tools:
        ann = t.get("annotations")
        name = t.get("name")
        if not isinstance(ann, dict):
            print("FAIL: %s is advertised with no annotations object" % name)
            bad.append("annotations:%s-missing" % name)
            continue
        keys = ("readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint")
        if any(not isinstance(ann.get(k), bool) for k in keys):
            print("FAIL: %s advertises a non-boolean or absent hint: %r" % (name, ann))
            bad.append("annotations:%s-shape" % name)
            continue
        got = tuple(ann[k] for k in keys)
        seen[name] = got
        want = declared.get(name)
        if want is None:
            print("FAIL: %s is advertised and has no row in tool_surface.h" % name)
            bad.append("annotations:%s-unrowed" % name)
        elif got != want:
            print("FAIL: %s advertises %r, its row declares %r" % (name, got, want))
            bad.append("annotations:%s-value" % name)
        if got[0] and got[1]:
            print("FAIL: %s advertises readOnlyHint and destructiveHint together" % name)
            bad.append("annotations:%s-readonly-destructive" % name)
        if got[3]:
            print("FAIL: %s advertises openWorldHint; every shipped tool is local" % name)
            bad.append("annotations:%s-openworld" % name)

    erase = seen.get("delete_project")
    read = seen.get("search_graph")
    if erase is None or read is None:
        print("FAIL: delete_project and search_graph must both be advertised with hints")
        bad.append("annotations:erase-read-missing")
    elif erase == read:
        print("FAIL: delete_project and search_graph advertise identical hints %r — a client "
              "cannot tell the tool that erases a database from the tool that reads it" % (erase,))
        bad.append("annotations:erase-read-identical")
    elif not (erase[1] and not read[1]):
        print("FAIL: destructiveHint is %r on delete_project and %r on search_graph"
              % (erase[1], read[1]))
        bad.append("annotations:erase-read-wrong-bit")
    else:
        print("annotations: %d tools match their declared profile; delete_project "
              "destructiveHint=True, search_graph destructiveHint=False" % len(seen))
    return bad


def no_project_view(binary):
    """§4 F3, first half: index_status in an UNINDEXED directory, at the client.

    handle_index_status documents a structured answer for "no project could be
    resolved" — {"status":"no_project"}, not an error — and that branch was
    dead from the day it shipped: it needed store != NULL with project == NULL,
    and resolve_store returns NULL the moment project is NULL, so
    REQUIRE_STORE answered with isError prose first, every time. This spawns a
    real stdio server in a directory that is not a project, with a cache
    holding no project at all, and asserts what the CLIENT holds: a non-error
    result whose object says status == "no_project". Returns a list of
    failures.
    """
    bad = []
    tmp = tempfile.mkdtemp(prefix="hyp-mcp-noproj-")
    try:
        cwd = os.path.join(tmp, "not-a-project")
        os.makedirs(cwd)
        env = dict(os.environ)
        env["HYP_CACHE_DIR"] = os.path.join(tmp, "cache")
        runtime_parent = os.path.join(tmp, "run")
        os.makedirs(runtime_parent, mode=0o700, exist_ok=True)
        env["HYP_TEST_DAEMON_RUNTIME_PARENT"] = runtime_parent
        # The normal-build cohort escape: the test seam above exists only under
        # HYP_ENABLE_TEST_SEAMS, and without either a dev binary refuses to
        # start whenever an installed build's daemon is already serving this
        # account. Legal here because HYP_CACHE_DIR is non-default (above).
        env["HYP_DAEMON_RUNTIME_PARENT"] = runtime_parent

        c = Client(binary, env, cwd=cwd)
        c.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                           "clientInfo": {"name": "mcp-client-view", "version": "0"}}})
        if c.read() is None:
            print("FAIL: unindexed-dir server sent no initialize response")
            c.close()
            return ["no_project:no-init"]
        c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        c.send({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                "params": {"name": "index_status", "arguments": {}}})
        resp = c.read() or {}
        c.close()
        res = resp.get("result") or {}
        content = res.get("content") or []
        text = content[0].get("text", "") if content else ""
        sc = res.get("structuredContent")
        seen = sc if isinstance(sc, dict) and sc else None
        if seen is None and text:
            try:
                parsed = json.loads(text)
                seen = parsed if isinstance(parsed, dict) else None
            except Exception:
                seen = None
        if res.get("isError"):
            print("FAIL: index_status in an unindexed directory answers isError, "
                  "not the structured no_project status it documents")
            bad.append("no_project:is-error")
        elif seen is None:
            print("FAIL: index_status in an unindexed directory gives a client no object")
            bad.append("no_project:no-object")
        elif seen.get("status") != "no_project":
            print("FAIL: index_status in an unindexed directory answers %r, not status=no_project"
                  % (seen.get("status"),))
            bad.append("no_project:wrong-status")
        else:
            print("index_status (unindexed dir) -> structured status=no_project, reachable")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return bad


def main():
    binary = os.path.abspath(BINARY)
    if not os.path.exists(binary):
        print("mcp-client-view: no binary at %s" % binary, file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="hyp-mcp-view-")
    src = os.path.join(tmp, "src")
    os.makedirs(src)
    with open(os.path.join(src, "sample.c"), "w") as f:
        f.write(FIXTURE)

    env = dict(os.environ)
    env["HYP_CACHE_DIR"] = os.path.join(tmp, "cache")
    # The version-cohort guard lives in <runtime-parent>/hyp-daemon-<uid>, a
    # fixed per-uid path that HYP_CACHE_DIR does NOT move. Without this seam a
    # dev build refuses to start whenever an installed build is already serving
    # an editor — i.e. exactly when you are trying to test a fix.
    runtime_parent = os.path.join(tmp, "run")
    os.makedirs(runtime_parent, mode=0o700, exist_ok=True)
    env["HYP_TEST_DAEMON_RUNTIME_PARENT"] = runtime_parent
    # Normal-build cohort escape (see no_project_view): without it a binary
    # built without HYP_ENABLE_TEST_SEAMS cannot run beside an installed
    # daemon at all. HYP_CACHE_DIR is non-default above, which is what makes
    # an isolated rendezvous legal.
    env["HYP_DAEMON_RUNTIME_PARENT"] = runtime_parent

    try:
        # Index the fixture through the CLI so the graph tools have rows to return.
        rc = subprocess.run([binary, "cli", "index_repository", json.dumps({"repo_path": src})],
                            capture_output=True, text=True, env=env)
        if rc.returncode != 0:
            print("mcp-client-view: could not index fixture: %s"
                  % (rc.stderr or rc.stdout)[-300:], file=sys.stderr)
            return 2
        try:
            project = json.loads(rc.stdout.strip().splitlines()[-1])["project"]
        except Exception:
            listing = subprocess.run([binary, "cli", "--json", "list_projects", "{}"],
                                     capture_output=True, text=True, env=env)
            project = json.loads(json.loads(listing.stdout)["content"][0]["text"])["projects"][0]["name"]

        calls = [
            ("list_projects", {}),
            ("query_graph", {"project": project,
                             "query": 'MATCH (f:Function) RETURN f.name LIMIT 5'}),
            ("search_graph", {"project": project, "query": "compute"}),
            ("get_graph_schema", {"project": project}),
            ("index_status", {"project": project}),
            ("ask", {"project": project, "question": "how is a value doubled"}),
            # The memory surface, flipped live. Order is the assertion: a read
            # BEFORE any write must not answer "records: []" — that would say
            # nothing was ever recorded when the truth is that there is nowhere
            # here it could have been. Then a write, then the read that must
            # see it. HYP_CACHE_DIR is the throwaway above, so the store this
            # creates is inside it.
            ("search_memory", {}),
            ("record_memory", {"kind": "decision", "title": "Client view wrote this",
                               "body": "A record written over the real stdio protocol."}),
            ("search_memory", {"query": "Client view", "format": "json"}),
        ]

        c = Client(binary, env)
        c.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                           "clientInfo": {"name": "mcp-client-view", "version": "0"}}})
        if c.read() is None:
            print("FAIL: server sent no initialize response", file=sys.stderr)
            c.close()
            return 1
        c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

        # A tool that declares an outputSchema PROMISES structuredContent. Only
        # declare one where the tool genuinely returns a JSON object.
        c.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        tools = (((c.read() or {}).get("result")) or {}).get("tools") or []
        declaring = [t["name"] for t in tools if "outputSchema" in t]
        schemas = {t["name"]: t["outputSchema"] for t in tools if "outputSchema" in t}
        print("tools advertised: %d, declaring outputSchema: %d" % (len(tools), len(declaring)))

        bad = annotation_view(tools)
        print()
        seen_text = []
        print("%-20s %8s  %-10s %s" % ("tool", "content", "structured", "client sees"))
        print("-" * 74)
        for i, (name, args) in enumerate(calls, start=10):
            c.send({"jsonrpc": "2.0", "id": i, "method": "tools/call",
                    "params": {"name": name, "arguments": args}})
            resp = c.read()
            if resp is None:
                print("%-20s %8s  %-10s %s" % (name, "-", "-", "NO RESPONSE"))
                bad.append(name)
                continue
            res = resp.get("result") or {}
            content = res.get("content") or []
            text = content[0].get("text", "") if content else ""
            has_sc = "structuredContent" in res
            sc = res.get("structuredContent")
            seen_text.append((name, text, bool(res.get("isError"))))

            if has_sc and isinstance(sc, dict) and not sc:
                shape, verdict = "{}", "EMPTY -> renders nothing"
                bad.append(name)
            elif has_sc and sc:
                shape, verdict = "obj", "structured (%d keys)" % len(sc)
            elif text:
                shape, verdict = "absent", "falls back to content: ok"
            else:
                shape, verdict = "-", "no content, no structure"
                bad.append(name)
            print("%-20s %8d  %-10s %s" % (name, len(text), shape, verdict))

            # THE PROMISE, CHECKED AGAINST WHAT THE CLIENT HOLDS. A declared
            # outputSchema says "every `required` field is here". The generic
            # form of the shipped defect is a schema promising structure the
            # text path never sends; asserting the emitted shape cannot see it,
            # so this reads the object the client actually ends up with —
            # structuredContent, or content[0].text when that parses as one.
            schema = schemas.get(name)
            if schema and not res.get("isError"):
                seen = sc if isinstance(sc, dict) and sc else None
                if seen is None and text:
                    try:
                        parsed = json.loads(text)
                        seen = parsed if isinstance(parsed, dict) else None
                    except Exception:
                        seen = None
                if seen is None:
                    print("FAIL: %s declares outputSchema and a client reads no object" % name)
                    bad.append(name)
                else:
                    for key in schema.get("required") or []:
                        if key not in seen:
                            print("FAIL: %s promises required field %r a client never receives"
                                  % (name, key))
                            bad.append(name)

        # The memory surface, as the client held it. Three claims, in order:
        # a read with no store says so and sends NO records list; the write
        # returns an id; the read after it finds the record. The first is the
        # one a unit test keeps getting wrong — absent means "look elsewhere",
        # empty means "there is nothing", and only one is ever true.
        mem = [(n, t, e) for (n, t, e) in seen_text if n in ("search_memory", "record_memory")]
        if len(mem) != 3:
            print("FAIL: the memory surface did not answer three calls")
            bad.append("memory:no-answer")
        else:
            (_, before, before_err), (_, wrote, wrote_err), (_, after, after_err) = mem
            if before_err or '"records"' in before or "records:" in before:
                print("FAIL: search_memory with no store sent a records list; an empty list "
                      "says nothing was ever recorded, which is a different claim")
                bad.append("memory:empty-not-absent")
            if wrote_err or '"id"' not in wrote:
                print("FAIL: record_memory is live and a client gets no id back: %s" % wrote[:200])
                bad.append("memory:write-unreachable")
            if after_err or "Client view wrote this" not in after:
                print("FAIL: search_memory cannot see what record_memory just wrote")
                bad.append("memory:read-after-write")
            if not bad or "memory:" not in " ".join(bad):
                print("memory: no-store answer omits records, write returns an id, "
                      "read finds it")

        # A tool may only promise structure it actually delivers.
        for t in declaring:
            if t in ("query_graph", "search_graph", "ask", "trace_path",
                     "get_code_snippet", "search_code", "get_architecture"):
                print("FAIL: %s declares outputSchema but answers in TOON" % t)
                bad.append(t)

        c.close()
        print()
        bad.extend(no_project_view(binary))
        print()
        bad.extend(profile_view(binary, env, project))
        print()
        if bad:
            print("FAIL: %d tool(s) unreachable to a client: %s" % (len(bad), ", ".join(sorted(set(bad)))))
            return 1
        print("PASS: every probed tool's answer is reachable to a client, the advertised "
              "annotation VALUES match the table that declares them, and ask is on analysis "
              "and not on scout")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
