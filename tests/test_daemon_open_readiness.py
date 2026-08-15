r"""Product guard for ``daemon start --open`` UI readiness.

The daemon intentionally warms and verifies its external UI pack after the
control socket is already available.  ``--open`` is the one synchronous user
request in that flow: it must wait for the *HYP HTTP endpoint*, not merely for
the daemon process, before it reports or opens the URL.

This guard uses a real UI build and isolated daemon generations:

* a temporarily occupied port proves the command waits and then succeeds once
  the verified UI listener becomes available;
* a foreign service returning the exact formerly accepted HTML markers proves
  public page text is not treated as daemon identity;
* a copied binary without its content-addressed pack proves missing/invalid
  runtime assets never result in an opened or reported UI URL.
* a second ``daemon start --open`` proves the already-active daemon path obtains
  the same generation-bound proof over a fresh authenticated IPC client.

Build the fixture with test seams so browser launching is recorded rather than
performed and the two negative cases can use a short deterministic deadline:

    make -f Makefile.hyp hyp-with-ui TEST_SEAMS=1 BUILD_DIR=build/ui-open-test
    python3 tests/test_daemon_open_readiness.py build/ui-open-test/hyponoia

Exit code: 0 == green, 1 == behavior regression, 2 == fixture/setup error.
"""

import os
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time


OPEN_MARKER_ENV = "HYP_TEST_DAEMON_OPEN_MARKER"
READY_TIMEOUT_ENV = "HYP_TEST_DAEMON_UI_READY_TIMEOUT_MS"
RUNTIME_PARENT_ENV = "HYP_TEST_DAEMON_RUNTIME_PARENT"

# ── Budgets, every one derived rather than chosen ──────────────────────────
#
# This file used to carry seven independent magic timeouts, and several of them
# were SMALLER than the budget of the very command they were timing. That is not
# a stricter test — it is a test that cannot observe an answer. It kills the
# product mid-decision and reports its own deadline as the product's verdict,
# which is exactly how "UI endpoint did not become ready within 10000 ms" came
# to be printed as the cause of a failure it had nothing to do with.
#
# The product's own numbers (src/main.c):
#   MAIN_DAEMON_CTL_PROBE_TIMEOUT_MS    =  3000   one control-plane probe, and
#                                                 an ABSENT daemon costs the
#                                                 full amount on POSIX before
#                                                 `status` can say "not running"
#   MAIN_DAEMON_CTL_START_TIMEOUT_MS    = 30000   how long a start may take
#   MAIN_DAEMON_CTL_UI_READY_TIMEOUT_MS = 30000   production UI-ready default
#
# SLACK covers what the product does not count: spawning a ~600 MB binary and
# tearing it down. Measured worst case is the ubuntu-24.04-arm runner pool,
# CPU-equal to ubuntu-22.04-arm and 10-80x slower on filesystem-heavy work.
#
# Anything that waits on a command must allow START + SLACK, plus that
# command's own UI budget when it has one. Read these as "the product's budget
# plus room", never as "how long this ought to take".
PRODUCT_PROBE_S = 3
PRODUCT_START_S = 30
PRODUCT_UI_READY_MS = 30000
SLACK_S = 30

STATUS_PROBE_S = PRODUCT_PROBE_S + SLACK_S // 2   # one `daemon status` call
START_WAIT_S = PRODUCT_START_S + SLACK_S         # a start with a short UI budget
UI_WAIT_S = START_WAIT_S + PRODUCT_UI_READY_MS // 1000  # a start that waits on UI
# A negative case still has to START the daemon before its short UI deadline
# can even be reached, so it needs the full start budget too. Its 900 ms UI
# budget is what makes the ASSERTION fast, not what makes the command fast.
SHORT_UI_READY_MS = 900


def output_text(result):
    return ((result.stdout or b"") + (result.stderr or b"")).decode("utf-8", "replace")


def pid_from(text):
    match = re.search(r"pid[: ]+(\d+)", text)
    return int(match.group(1)) if match else 0


def force_kill(pid):
    if not pid:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True, timeout=30)
    else:
        subprocess.run(["kill", "-9", str(pid)], capture_output=True, timeout=30)


def stop_daemon(binary, env, pid):
    stopped = False
    try:
        subprocess.run([binary, "daemon", "stop"], capture_output=True,
                       timeout=START_WAIT_S, env=env)
    except (OSError, subprocess.TimeoutExpired):
        pass
    deadline = time.monotonic() + START_WAIT_S
    while time.monotonic() < deadline:
        try:
            # Same arithmetic as wait_daemon_active: a probe budget under the
            # product's own 3000 ms can never see "not running", which is the
            # exact string this loop is waiting for. It would fall through to
            # force_kill on every slow leg.
            status = subprocess.run([binary, "daemon", "status"], capture_output=True,
                                    timeout=STATUS_PROBE_S, env=env)
            if status.returncode != 0 and "not running" in output_text(status):
                stopped = True
                break
        except (OSError, subprocess.TimeoutExpired):
            pass
        time.sleep(0.1)
    if not stopped:
        force_kill(pid)


def occupied_loopback_port():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if os.name != "nt":
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(16)
    return listener, listener.getsockname()[1]


class OldMarkerResponder:
    """Foreign HTTP service that exactly matches the former acceptance rule."""

    BODY = b"<!doctype html><html><head><title>Hyponoia</title></head></html>"

    def __init__(self):
        self.listener, self.port = occupied_loopback_port()
        self.listener.settimeout(0.1)
        self.stop_event = threading.Event()
        self.request_count = 0
        self.thread = threading.Thread(target=self._serve, name="old-marker-responder",
                                       daemon=True)
        self.thread.start()

    def _serve(self):
        response = (
            b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/html; charset=utf-8\r\n"
            b"Cache-Control: no-cache\r\n"
            b"Content-Length: " + str(len(self.BODY)).encode("ascii") + b"\r\n"
            b"Connection: close\r\n\r\n" + self.BODY
        )
        while not self.stop_event.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                connection.settimeout(0.5)
                self.request_count += 1
                try:
                    connection.recv(4096)
                except (OSError, socket.timeout):
                    pass
                connection.sendall(response)
            except OSError:
                pass
            finally:
                connection.close()

    def close(self):
        self.stop_event.set()
        self.listener.close()
        self.thread.join(timeout=2)


def fixture_environment(work, cache, marker, timeout_ms):
    env = dict(os.environ)
    env["HYP_CACHE_DIR"] = cache
    env[OPEN_MARKER_ENV] = marker
    env[READY_TIMEOUT_ENV] = str(timeout_ms)
    runtime_parent = os.path.join(work, "runtime-" + os.path.basename(cache))
    os.makedirs(runtime_parent, mode=0o700, exist_ok=True)
    env[RUNTIME_PARENT_ENV] = runtime_parent
    env["HYP_TEST_FAKE_BROWSER_MARKER"] = marker

    # Before the production fix exists, POSIX builds do not know the marker
    # seam.  A PATH-local opener lets the very same behavior test demonstrate
    # the original premature-open RED instead of failing during setup.
    if os.name != "nt":
        fake_bin = os.path.join(work, "fake-bin")
        os.makedirs(fake_bin, exist_ok=True)
        opener = "open" if sys.platform == "darwin" else "xdg-open"
        opener_path = os.path.join(fake_bin, opener)
        if not os.path.exists(opener_path):
            with open(opener_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("#!/bin/sh\nprintf '%s' \"$1\" > \"$HYP_TEST_FAKE_BROWSER_MARKER\"\n")
            os.chmod(opener_path, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        env["PATH"] = fake_bin + os.pathsep + env.get("PATH", "")
    return env


def launch_start(binary, work, cache, marker, port, timeout_ms):
    os.makedirs(cache, exist_ok=True)
    env = fixture_environment(work, cache, marker, timeout_ms)
    process = subprocess.Popen(
        [binary, "daemon", "start", "--port=%d" % port, "--open"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    return process, env


def collect(process, timeout=START_WAIT_S):
    stdout, stderr = process.communicate(timeout=timeout)
    result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
    return result, output_text(result)


def wait_daemon_active(binary, env, process, timeout=START_WAIT_S):
    # BOTH numbers here are bounded by the product, not chosen for taste.
    #
    # PROBE_TIMEOUT must strictly exceed the product's own control-probe budget,
    # MAIN_DAEMON_CTL_PROBE_TIMEOUT_MS = 3000 (src/main.c). On POSIX an absent
    # daemon costs the FULL budget before `daemon status` can answer, because
    # hyp_daemon_ipc_connect retries an ENOENT socket every 10 ms until the
    # deadline rather than concluding absence (src/daemon/ipc.c). At the old 2 s
    # every probe issued before the daemon was connectable was killed mid-answer,
    # so this loop could never observe "not running" — only "timed out" with an
    # EMPTY partial. That is why every failing log showed a bare
    # "diagnostic: status timed out:" and no evidence at all.
    #
    # The window must not be tighter than the product's own start budget,
    # MAIN_DAEMON_CTL_START_TIMEOUT_MS = 30000. A harness that gives up sooner
    # than the product is allowed to take fails runs the product considers
    # healthy. The old 10 s window was five 2 s probes, i.e. ~8 s of real
    # waiting, and the ubuntu-24.04-arm runner pool — measured 10-80x slower on
    # filesystem-heavy work than ubuntu-22.04-arm at equal CPU — needs about 15.
    # This costs nothing on a healthy leg: the loop returns the moment status
    # says active, or the moment the starter exits.
    probe_timeout = STATUS_PROBE_S
    deadline = time.monotonic() + timeout
    last_probe = "status probe was not attempted"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        try:
            status = subprocess.run([binary, "daemon", "status"], capture_output=True,
                                    timeout=probe_timeout, env=env)
            last_probe = "status rc=%d: %s" % (status.returncode, output_text(status)[:600])
        except subprocess.TimeoutExpired as timeout_error:
            # The starter may still own the short bootstrap handoff. A timed
            # status probe is not evidence either way; retry within this
            # function's independent bounded deadline. Back off like every other
            # path — the bare `continue` here spun the whole window into probe
            # attempts with no pause between them.
            partial = (timeout_error.stdout or b"") + (timeout_error.stderr or b"")
            last_probe = "status timed out: %s" % partial.decode("utf-8", "replace")[:600]
            time.sleep(0.1)
            continue
        if status.returncode == 0 and "daemon: active" in output_text(status):
            return True
        time.sleep(0.1)
    print("diagnostic: %s" % last_probe)
    return False


def assert_delayed_success(binary, work):
    cache = os.path.join(work, "cache-delayed")
    marker = os.path.join(work, "browser-delayed.txt")
    blocker, port = occupied_loopback_port()
    # 30000 is the PRODUCTION default (MAIN_DAEMON_CTL_UI_READY_TIMEOUT_MS in
    # src/main.c), and the seam clamps to it — so this asserts the behaviour a
    # real user gets rather than a tightened test-only version of it. The old
    # 10000 was below the daemon's own HTTP bind-retry schedule (1 s doubling to
    # 30 s, src/daemon/host.c), so releasing the port mid-backoff meant the next
    # attempt landed after the client had already given up. That is a budget
    # losing a race with a backoff curve, not a readiness bug.
    process, env = launch_start(binary, work, cache, marker, port, PRODUCT_UI_READY_MS)
    daemon_pid = 0
    try:
        if not wait_daemon_active(binary, env, process):
            result, text = collect(process)
            daemon_pid = pid_from(text)
            print("RED: daemon control service never became active during --open:\n%s"
                  % text[:600])
            return False
        # Generic daemon readiness is explicitly not UI readiness. Once status
        # is active, leave enough time for the buggy command to report/open;
        # the fixed command remains blocked on the authenticated HTTP probe.
        time.sleep(0.4)
        if process.poll() is not None or os.path.exists(marker):
            result, text = collect(process)
            daemon_pid = pid_from(text)
            print("RED: --open returned/opened while another listener still owned the UI port:\n%s"
                  % text[:600])
            return False

        # The daemon's first bind failed without delaying its control service.
        # Releasing the port lets the background retry publish the real HYP UI.
        blocker.close()
        blocker = None
        result, text = collect(process, timeout=UI_WAIT_S)
        daemon_pid = pid_from(text)
        expected_url = "http://127.0.0.1:%d" % port
        marker_text = ""
        if os.path.exists(marker):
            with open(marker, "r", encoding="utf-8") as handle:
                marker_text = handle.read()
        if result.returncode != 0 or expected_url not in text or marker_text != expected_url:
            print("RED: --open did not wait through the background UI retry and then open the "
                  "verified endpoint:\n%s\nmarker=%r" % (text[:700], marker_text))
            return False
        print("PASS: --open waited for the verified UI endpoint before reporting/opening it")
        return True
    finally:
        if blocker is not None:
            blocker.close()
        if process.poll() is None:
            process.kill()
            _, text = collect(process)
            daemon_pid = daemon_pid or pid_from(text)
        stop_daemon(binary, env, daemon_pid)


def assert_bounded_foreign_port_failure(binary, work):
    cache = os.path.join(work, "cache-occupied")
    marker = os.path.join(work, "browser-occupied.txt")
    blocker = OldMarkerResponder()
    port = blocker.port
    process, env = launch_start(binary, work, cache, marker, port, SHORT_UI_READY_MS)
    daemon_pid = 0
    try:
        result, text = collect(process, timeout=START_WAIT_S)
        daemon_pid = pid_from(text)
        url = "http://127.0.0.1:%d" % port
        if (result.returncode == 0 or os.path.exists(marker) or url in text or
                "UI endpoint did not become ready" not in text or "--port" not in text):
            print("RED: --open must reject a bounded wait on a foreign/occupied endpoint "
                  "without reporting or opening its URL:\n%s" % text[:700])
            return False
        request_count = blocker.request_count
        status = subprocess.run([binary, "daemon", "status"], capture_output=True,
                                timeout=STATUS_PROBE_S, env=env)
        status_text = output_text(status)
        if (status.returncode != 0 or "daemon: active" not in status_text or
                blocker.request_count != request_count):
            print("RED: daemon status must remain a bounded control-plane probe and must not "
                  "contact a configured foreign HTTP listener:\n%s\nrequests=%d->%d" %
                  (status_text[:700], request_count, blocker.request_count))
            return False
        print("PASS: exact legacy HTML markers cannot spoof daemon-generation readiness")
        return True
    finally:
        blocker.close()
        if process.poll() is None:
            process.kill()
            _, text = collect(process)
            daemon_pid = daemon_pid or pid_from(text)
        stop_daemon(binary, env, daemon_pid)


def assert_active_daemon_open(binary, work):
    cache = os.path.join(work, "cache-active")
    marker = os.path.join(work, "browser-active.txt")
    probe, port = occupied_loopback_port()
    probe.close()
    os.makedirs(cache, exist_ok=True)
    env = fixture_environment(work, cache, marker, PRODUCT_UI_READY_MS)
    daemon_pid = 0
    try:
        first = subprocess.run([binary, "daemon", "start", "--port=%d" % port],
                               capture_output=True, timeout=START_WAIT_S, env=env)
        first_text = output_text(first)
        daemon_pid = pid_from(first_text)
        if first.returncode != 0 or "daemon: started" not in first_text:
            print("RED: initial asynchronous daemon start failed:\n%s" % first_text[:700])
            return False

        second = subprocess.run([binary, "daemon", "start", "--open"], capture_output=True,
                                timeout=UI_WAIT_S, env=env)
        second_text = output_text(second)
        expected_url = "http://127.0.0.1:%d" % port
        marker_text = ""
        if os.path.exists(marker):
            with open(marker, "r", encoding="utf-8") as handle:
                marker_text = handle.read()
        if (second.returncode != 0 or "daemon: already active" not in second_text or
                expected_url not in second_text or marker_text != expected_url):
            print("RED: active-daemon --open did not authenticate and open its exact "
                  "generation:\n%s\nmarker=%r" % (second_text[:700], marker_text))
            return False
        print("PASS: an already-active daemon is opened only after generation-bound proof")
        return True
    finally:
        stop_daemon(binary, env, daemon_pid)


def assert_missing_pack_failure(binary, work):
    isolated = os.path.join(work, "without-pack")
    os.makedirs(isolated, exist_ok=True)
    copied_binary = os.path.join(isolated, os.path.basename(binary))
    shutil.copy2(binary, copied_binary)

    cache = os.path.join(work, "cache-missing-pack")
    marker = os.path.join(work, "browser-missing-pack.txt")
    probe, port = occupied_loopback_port()
    probe.close()  # reserve an unused port number without keeping it occupied
    process, env = launch_start(copied_binary, work, cache, marker, port, SHORT_UI_READY_MS)
    daemon_pid = 0
    try:
        result, text = collect(process, timeout=START_WAIT_S)
        daemon_pid = pid_from(text)
        url = "http://127.0.0.1:%d" % port
        if (result.returncode == 0 or os.path.exists(marker) or url in text or
                "UI endpoint did not become ready" not in text or
                "matching runtime assets" not in text):
            print("RED: a UI binary without its verified pack must fail --open without "
                  "reporting/opening a URL:\n%s" % text[:700])
            return False
        print("PASS: missing verified UI assets fail --open without opening/reporting a URL")
        return True
    finally:
        if process.poll() is None:
            process.kill()
            _, text = collect(process)
            daemon_pid = daemon_pid or pid_from(text)
        stop_daemon(copied_binary, env, daemon_pid)


def main():
    if len(sys.argv) != 2:
        print("usage: python3 test_daemon_open_readiness.py <ui-binary>")
        return 2
    binary = os.path.abspath(sys.argv[1])
    if not os.path.isfile(binary):
        print("SETUP FAIL: binary not found: %s" % binary)
        return 2
    binary_dir = os.path.dirname(binary)
    if not any(name.startswith("hyp-ui-") and name.endswith(".pack")
               for name in os.listdir(binary_dir)):
        print("SETUP FAIL: no hyp-ui-*.pack adjacent to UI binary: %s" % binary_dir)
        return 2
    if os.name == "nt":
        with open(binary, "rb") as handle:
            if OPEN_MARKER_ENV.encode("ascii") not in handle.read():
                print("SETUP FAIL: Windows fixture must be built with TEST_SEAMS=1")
                return 2

    # macOS sockaddr_un paths are capped at 104 bytes. Anchor the isolated
    # runtime directly under /private/tmp rather than the much longer per-user
    # Darwin temporary root so the product endpoint itself remains valid.
    short_temp_root = "/private/tmp" if sys.platform == "darwin" else tempfile.gettempdir()
    with tempfile.TemporaryDirectory(prefix="hyp_uiopen_", dir=short_temp_root) as work:
        if not assert_delayed_success(binary, work):
            return 1
        if not assert_bounded_foreign_port_failure(binary, work):
            return 1
        if not assert_active_daemon_open(binary, work):
            return 1
        if not assert_missing_pack_failure(binary, work):
            return 1
    print("\nGREEN: daemon --open is bound to verified UI endpoint readiness.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
