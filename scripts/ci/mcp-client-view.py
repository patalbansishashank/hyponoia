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

Exit 0 if every probed tool is reachable, 1 otherwise.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

BINARY = sys.argv[1] if len(sys.argv) > 1 else "build/c/hyponoia"

FIXTURE = """\
#include <stdio.h>
static int helper(int a) { return a * 2; }
int compute(int a) { return helper(a) + 1; }
int main(void) { printf("%d\\n", compute(3)); return 0; }
"""


class Client:
    def __init__(self, binary, env):
        self.p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env)

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
        print("tools advertised: %d, declaring outputSchema: %d" % (len(tools), len(declaring)))

        bad = []
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

        # A tool may only promise structure it actually delivers.
        for t in declaring:
            if t in ("query_graph", "search_graph", "ask", "trace_path",
                     "get_code_snippet", "search_code", "get_architecture"):
                print("FAIL: %s declares outputSchema but answers in TOON" % t)
                bad.append(t)

        c.close()
        print()
        if bad:
            print("FAIL: %d tool(s) unreachable to a client: %s" % (len(bad), ", ".join(sorted(set(bad)))))
            return 1
        print("PASS: every probed tool's answer is reachable to a client")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
