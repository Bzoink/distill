#!/usr/bin/env python3
"""Run the corpus assertions.

Starts serve.py, drives the Phase 0 engine over each page, and checks the
'dom'-stage assertions in manifest.json against the post-JS DOM. Use
--baseline to validate the fixtures against a reference browser instead.

    ./check.py                 all local gates
    ./check.py --extract       Phase 1 extraction and golden gates
    ./check.py --render DIR    Phase 2 PNG smoke renders
    ./check.py --viewer DIR    Phase 2 media-viewer smoke render
    ./check.py --app           Phase 3 desktop-shell model smoke test
    ./check.py --tui           Phase 6 terminal shell, driven through a pty
    ./check.py --id 03-settle  one page
    ./check.py --live          also record the live rungs (never gates)
    ./check.py --keep-dom DIR  save each dumped DOM for inspection

Exit status is non-zero if any gate fails, so Phase 0's "done when" is a
CI-able condition rather than a judgement call.

Validate the fixtures with --baseline FIRST. A fixture bug and a binding bug
look identical, and these fixtures are written to be hard.
"""

import argparse
import json
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.error
import urllib.request
from html.parser import HTMLParser

try:
    from wcwidth import wcswidth as _wcswidth
except ImportError:  # the fallback below is coarser but needs nothing installed
    _wcswidth = None

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(HERE, "manifest.json")
SERVE = os.path.join(HERE, "serve.py")

BROWSERS = ["chromium", "chromium-browser", "google-chrome", "google-chrome-stable"]
SKIP_TEXT = {"script", "style", "template"}

GREEN, RED, YELLOW, DIM, RESET = "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m"
if not sys.stdout.isatty():
    GREEN = RED = YELLOW = DIM = RESET = ""


class IdText(HTMLParser):
    """Text content of the element with a given id, scripts and styles excluded."""

    def __init__(self, target_id):
        super().__init__(convert_charrefs=True)
        self.target = target_id
        self.depth = 0          # nesting depth inside the target, 0 = outside
        self.skip = 0           # nesting depth inside a script/style
        self.parts = []
        self.found = False

    def handle_starttag(self, tag, attrs):
        d = dict(attrs)
        if self.depth:
            self.depth += 1
        elif d.get("id") == self.target:
            self.depth = 1
            self.found = True
        if self.depth and tag in SKIP_TEXT:
            self.skip += 1

    def handle_endtag(self, tag):
        if self.skip and tag in SKIP_TEXT:
            self.skip -= 1
        if self.depth:
            self.depth -= 1

    def handle_data(self, data):
        if self.depth and not self.skip:
            self.parts.append(data)

    @property
    def text(self):
        return " ".join("".join(self.parts).split())


def element_text(html, selector):
    if not selector.startswith("#"):
        raise ValueError(f"only #id selectors are supported, got {selector!r}")
    p = IdText(selector[1:])
    p.feed(html)
    return (p.text if p.found else None)


# The flags the engine takes without a value; every other option has one.
ENGINE_FLAGS = {"--dump-dom", "--extract", "--native-forms", "--progress", "--https-optional"}


class EngineHost:
    """The engine in-process: one distill-host, every navigation on a thread
    of its own inside it (src/engine_host.h). Runs that leave state behind
    for the next fail the next one, which is part of what this tests."""

    def __init__(self, binary):
        self.binary = binary
        self.scratch = tempfile.mkdtemp(prefix="distill-host-")
        self.process = None
        self.count = 0

    def _start(self):
        self.process = subprocess.Popen([self.binary], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, bufsize=1)

    def run(self, cmd, timeout_s):
        if self.process is None or self.process.poll() is not None:
            self._start()
        self.count += 1
        base = os.path.join(self.scratch, str(self.count))
        lines, i = [], 1
        while i < len(cmd):
            name = cmd[i][2:]
            if cmd[i] in ENGINE_FLAGS or i + 1 >= len(cmd):
                lines.append(name)
                i += 1
            else:
                lines.append(f"{name} {cmd[i + 1]}")
                i += 2
        with open(base + ".request", "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        self.process.stdin.write(f"{base}.request\t{base}.out\t{base}.err\n")
        self.process.stdin.flush()
        ready, _, _ = select.select([self.process.stdout], [], [], timeout_s)
        reply = self.process.stdout.readline() if ready else ""
        if not reply.startswith("status "):
            # A hang or a crash takes the host down with it: that is the cost
            # of in-process, and exactly what this mode is here to show.
            self.process.kill()
            self.process.wait()
            self.process = None
            raise subprocess.TimeoutExpired(cmd, timeout_s) if not ready else \
                OSError(f"the in-process host died on {cmd[2] if len(cmd) > 2 else cmd}")
        status = int(reply.split()[1])

        def read(path):
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    return fh.read()
            except OSError:
                return ""
        return subprocess.CompletedProcess(cmd, status, read(base + ".out"), read(base + ".err"))

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.stdin.close()
            self.process.wait(timeout=10)
        shutil.rmtree(self.scratch, ignore_errors=True)


ENGINE_HOST = None


def run_engine(cmd, timeout_s, env=None):
    """Run the engine as the command, or in-process with --in-process."""
    if ENGINE_HOST is not None and env is None:
        return ENGINE_HOST.run(cmd, timeout_s)
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s, env=env)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def find_browser():
    for b in BROWSERS:
        path = shutil.which(b)
        if path:
            return path
    sys.exit("no chromium/chrome found; install one to validate the corpus")


def find_engine():
    candidates = [
        os.environ.get("DISTILL_ENGINE"),
        os.path.join(os.path.dirname(HERE), "build", "distill"),
        os.path.join(os.path.dirname(HERE), "build", "bin", "distill"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    path = shutil.which("distill")
    if path:
        return path
    sys.exit("no distill engine found; build it first (cmake -S . -B build && cmake --build build)")


def find_arm_b(pruned=False):
    candidates = [
        os.environ.get("DISTILL_ARM_B_PRUNED" if pruned else "DISTILL_ARM_B"),
        os.path.join(os.path.dirname(HERE), "build", "distill-arm-b-pruned" if pruned else "distill-arm-b"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    label = "Arm B'" if pruned else "Arm B"
    sys.exit(f"no {label} engine found; build it first (cmake -S . -B build && cmake --build build)")


def find_tui():
    candidates = [
        os.environ.get("DISTILL_TUI"),
        os.path.join(os.path.dirname(HERE), "build", "distill-tui"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    sys.exit("no distill-tui found; build it first (cmake -S . -B build && cmake --build build)")


def find_app():
    candidates = [
        os.environ.get("DISTILL_APP"),
        os.path.join(os.path.dirname(HERE), "build", "distill-gtk"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    path = shutil.which("distill-gtk")
    if path:
        return path
    sys.exit("no distill-gtk app found; build it first (cmake -S . -B build && cmake --build build)")


def dump_dom(browser, url, budget_ms, timeout_s):
    """Post-JS DOM for a URL, or (None, reason)."""
    cmd = [
        browser, "--headless=new", "--disable-gpu", "--no-sandbox",
        "--hide-scrollbars", "--window-size=1280,1024",
        f"--virtual-time-budget={budget_ms}",
        "--run-all-compositor-stages-before-draw",
        "--dump-dom", url,
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return None, f"browser timed out after {timeout_s}s"
    if r.returncode != 0:
        return None, f"browser exit {r.returncode}: {r.stderr.strip()[:200]}"
    return r.stdout, None


def run_session_steps(engine, base, steps, session_path, cap_ms, timeout_s):
    """Perform a page's session prerequisites against one cookie file.

    A login is two requests that only validate together: the GET that issues
    the form and cookie, then the POST that returns them. Running them as
    separate processes sharing only the session file is the point -- it is
    what proves the session outlived the run that created it.
    """
    for step in steps:
        cmd = [engine, "--url", base + step["path"], "--dump-dom",
               "--script-cap-ms", str(cap_ms), "--session-file", session_path]
        body_path = None
        if step.get("method"):
            cmd += ["--request-method", step["method"]]
        if step.get("data") is not None:
            # Credentials go through a private file, never the argument
            # vector, which is exactly what the engine flag exists for.
            handle, body_path = tempfile.mkstemp(prefix="corpus-body-")
            with os.fdopen(handle, "w") as fh:
                fh.write(step["data"])
            os.chmod(body_path, 0o600)
            cmd += ["--request-data-file", body_path]
        try:
            r = run_engine(cmd, timeout_s)
        except subprocess.TimeoutExpired:
            return f"session step {step['path']} timed out after {timeout_s}s"
        finally:
            if body_path:
                os.unlink(body_path)
        if r.returncode != step.get("expect_exit", 0):
            return (f"session step {step['path']} exit {r.returncode}, "
                    f"expected {step.get('expect_exit', 0)}: {r.stderr.strip()[:200]}")
    return None


def dump_engine(engine, url, cap_ms, timeout_s, expect_exit=0, session_path=None):
    """Post-JS DOM from the Phase 0 engine, or (None, reason).

    A page may declare a non-zero expected status: a recognised bot challenge
    still serializes its shell, but reports a status that separates it from a
    content run.
    """
    cmd = [
        engine, "--url", url, "--dump-dom", "--script-cap-ms", str(cap_ms),
    ]
    if session_path:
        cmd += ["--session-file", session_path]
    try:
        r = run_engine(cmd, timeout_s)
    except subprocess.TimeoutExpired:
        return None, f"engine timed out after {timeout_s}s"
    if r.returncode != expect_exit:
        return None, (f"engine exit {r.returncode}, expected {expect_exit}: "
                      f"{r.stderr.strip()[:200]}")
    if not r.stdout.strip():
        return None, f"engine returned no DOM: {r.stderr.strip()[:200]}"
    return r.stdout, None


def dump_extract(engine, url, cap_ms, timeout_s, expect_exit=0, stats=None):
    """Structured extraction JSON for a URL, or (None, reason).

    As with dump_engine, a page may declare a non-zero expected status: a
    recognised challenge shell is still extracted, under its own status.

    Pass a dict as stats to have it filled with the engine's DISTILL_STATS
    line: what the run cost, for pages that assert a budget.
    """
    cmd = [engine, "--url", url, "--extract", "--script-cap-ms", str(cap_ms)]
    env = None
    stats_file = None
    # In-process there is no process to measure; the budgets say so.
    if stats is not None and ENGINE_HOST is None:
        handle, stats_file = tempfile.mkstemp(prefix="distill-stats-", suffix=".jsonl")
        os.close(handle)
        env = dict(os.environ, DISTILL_STATS=stats_file)
    try:
        r = run_engine(cmd, timeout_s, env)
    except subprocess.TimeoutExpired:
        return None, f"extractor timed out after {timeout_s}s"
    finally:
        if stats_file:
            with open(stats_file, encoding="utf-8") as fh:
                lines = [json.loads(line) for line in fh if line.strip()]
            os.unlink(stats_file)
            if lines:
                stats.update(lines[-1])
    if r.returncode != expect_exit:
        return None, (f"extractor exit {r.returncode}, expected {expect_exit}: "
                      f"{r.stderr.strip()[:300]}")
    try:
        return json.loads(r.stdout), None
    except json.JSONDecodeError as exc:
        return None, f"extractor returned invalid JSON: {exc}"


def render_page(engine, url, output_path, cap_ms, timeout_s, viewer=False, expect_exit=0,
                fit_height=False):
    """Render a reader PNG and verify its basic PNG dimensions.

    As with the DOM and extract stages, a page may declare a non-zero expected
    status: a recognised challenge shell still renders, under its own status.
    """
    cmd = [engine, "--url", url, "--viewer" if viewer else "--render", output_path,
           "--script-cap-ms", str(cap_ms)]
    saved = None
    if viewer:
        # Without --save-media the viewer shows a placeholder and the gate
        # proves only that a dark rectangle can be drawn. With it, the run
        # covers the whole media path: fetch, sniff, decode, paint.
        saved = output_path + ".media"
        cmd += ["--save-media", saved]
    if fit_height:
        # 0 asks the renderer to be as tall as the content instead of cropping.
        cmd += ["--height", "0"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return None, f"renderer timed out after {timeout_s}s"
    if r.returncode != expect_exit:
        return None, (f"renderer exit {r.returncode}, expected {expect_exit}: "
                      f"{r.stderr.strip()[:300]}")
    try:
        with open(output_path, "rb") as fh:
            header = fh.read(24)
        if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
            return None, "renderer did not write a PNG"
        width = int.from_bytes(header[16:20], "big")
        height = int.from_bytes(header[20:24], "big")
        if width <= 0 or height <= 0:
            return None, "renderer wrote an empty PNG"
        if saved:
            with open(saved, "rb") as fh:
                magic = fh.read(12)
            known = (magic.startswith(b"\x89PNG\r\n\x1a\n")
                     or magic[:3] == b"\xff\xd8\xff"
                     or (magic[:4] == b"RIFF" and magic[8:12] == b"WEBP"))
            if not known:
                return None, "the viewer saved something that is not an image"
            os.unlink(saved)
        return (width, height), None
    except OSError as exc:
        return None, f"renderer output unavailable: {exc}"


def dump_markdown(engine, url, cap_ms, timeout_s):
    """The reader page exported as Markdown.

    The export is a conversion of the reader template, so this is the stage
    that notices when the two drift apart -- an export that no longer says
    what the page says is the one failure the other stages cannot see.
    """
    with tempfile.NamedTemporaryFile(suffix=".md", delete=False) as fh:
        path = fh.name
    cmd = [engine, "--url", url, "--markdown", path, "--script-cap-ms", str(cap_ms)]
    try:
        r = run_engine(cmd, timeout_s)
        if r.returncode != 0:
            return None, f"markdown exit {r.returncode}: {r.stderr.strip()[:200]}"
        with open(path, encoding="utf-8") as fh:
            return fh.read(), None
    except subprocess.TimeoutExpired:
        return None, f"markdown timed out after {timeout_s}s"
    except OSError as exc:
        return None, f"markdown unavailable: {exc}"
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def dump_template(engine, url, cap_ms, timeout_s):
    """The reader HTML the shell lays out.

    Extraction assertions cannot see this stage, and neither can the render
    stage, which only checks that a PNG came out with a plausible size. What
    the reader actually offers a reader -- whether a placeholder is something
    you can activate, whether a URL is printed in a form that fits -- lives
    only here.
    """
    with tempfile.NamedTemporaryFile(suffix=".html", delete=False) as fh:
        path = fh.name
    cmd = [engine, "--url", url, "--template", path, "--script-cap-ms", str(cap_ms)]
    try:
        r = run_engine(cmd, timeout_s)
        if r.returncode != 0:
            return None, f"template exit {r.returncode}: {r.stderr.strip()[:200]}"
        with open(path, encoding="utf-8") as fh:
            return fh.read(), None
    except subprocess.TimeoutExpired:
        return None, f"template timed out after {timeout_s}s"
    except OSError as exc:
        return None, f"template unavailable: {exc}"
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def dump_arm_b(engine, url, cap_ms, timeout_s):
    """Post-JS DOM from the JS-side-DOM comparison arm."""
    cmd = [engine, "--url", url, "--dump-dom", "--script-cap-ms", str(cap_ms)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return None, f"Arm B timed out after {timeout_s}s"
    if r.returncode != 0:
        return None, f"Arm B exit {r.returncode}: {r.stderr.strip()[:200]}"
    if not r.stdout.strip():
        return None, f"Arm B returned no DOM: {r.stderr.strip()[:200]}"
    return r.stdout, None


def check_transport(engine, port, cap_ms, timeout_s):
    """A host that serves no TLS at all.

    This is the shape kp.se has: the address bar guesses https:// for a bare
    hostname, the handshake never completes, and so there is no response to
    read and no redirect to follow -- the page is unreachable for the one
    reason that has nothing to do with the page. The corpus server speaks
    http and nothing else, which is that shape exactly.

    The guess is the engine's to correct only when it was told the scheme was
    a guess. Without --https-optional the run must still fail, or an explicit
    https:// address could be stepped down to plaintext by anyone able to
    break the handshake.
    """
    secure = f"https://127.0.0.1:{port}/09-article.html"
    plain = f"http://127.0.0.1:{port}/09-article.html"
    failures = []
    try:
        r = subprocess.run([engine, "--url", secure, "--extract",
                            "--script-cap-ms", str(cap_ms)],
                           capture_output=True, text=True, timeout=timeout_s)
        if r.returncode != 2:
            failures.append(f"https to an http-only host exited {r.returncode}, expected 2")
    except (subprocess.TimeoutExpired, OSError) as exc:
        failures.append(f"https to an http-only host: {exc}")

    # An untrusted clock: TLS refusing is then the clock's fault, which the
    # user can fix, and it has its own status (5) and message -- also when the
    # scheme was only a guess, which must not be stepped down to plaintext to
    # get round it. DISTILL_TEST_UNTRUSTED_TIME is the platform's test seam.
    clockless = dict(os.environ, DISTILL_TEST_UNTRUSTED_TIME="1")
    for extra, label in (([], "https"), (["--https-optional"], "a guessed https")):
        try:
            r = subprocess.run([engine, "--url", secure, "--extract",
                                "--script-cap-ms", str(cap_ms)] + extra,
                               capture_output=True, text=True, timeout=timeout_s, env=clockless)
            if r.returncode != 5:
                failures.append(f"{label} with an untrusted clock exited {r.returncode}, expected 5")
            elif "clock" not in r.stderr:
                failures.append(f"{label} with an untrusted clock did not say the clock was the cause")
        except (subprocess.TimeoutExpired, OSError) as exc:
            failures.append(f"{label} with an untrusted clock: {exc}")

    with tempfile.NamedTemporaryFile(suffix=".html", delete=False) as fh:
        path = fh.name
    try:
        r = subprocess.run([engine, "--url", secure, "--template", path,
                            "--progress", "--https-optional",
                            "--script-cap-ms", str(cap_ms)],
                           capture_output=True, text=True, timeout=timeout_s)
        if r.returncode != 0:
            failures.append(f"--https-optional exited {r.returncode}, expected 0: "
                            f"{r.stderr.strip()[:200]}")
        else:
            with open(path, encoding="utf-8") as fh:
                template = fh.read()
            if "A careful reader" not in template:
                failures.append("the fallback loaded something other than the page")
            # The shell puts this in the address bar and in history; a reload
            # of the address that only pointed here is a different request.
            if f"location {plain}" not in r.stdout:
                failures.append("the run did not report where the page came from")
    except (subprocess.TimeoutExpired, OSError) as exc:
        failures.append(f"--https-optional: {exc}")
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    return failures


def run_stack_gate(engine, port, manifest, timeout_s):
    """Muroxi's stacks, modelled on Linux (muroxi_compat.md, --stack).

    Muroxi reserves 8 MiB for a process's main stack but guarantees only the
    first 64 KiB; growth past that can be refused, which kills the process.
    The engine may go deep -- a separate process pays for its own failure --
    but a shell holds the user's history, forms and playback and must stay
    within the guarantee. So:

      engine  every corpus page under an 8 MiB stack, with the JS limit the
              engine derives from it; a page may only fail by QuickJS's own
              RangeError, which is a result, not a crash
      shell   the whole terminal suite with the shell in 64 KiB, each engine
              it starts given 8 MiB of its own

    Each half has a negative control that must fail, or it models nothing:
    the deep-recursion page with the JS limit set to the whole 8 MiB must
    crash, and the shell must not run in 16 KiB.
    """
    import resource
    import tui_suite
    cap_ms = manifest.get("settle_cap_ms", 5000)
    base = f"http://127.0.0.1:{port}"
    hard = resource.getrlimit(resource.RLIMIT_STACK)[1]
    resource.setrlimit(resource.RLIMIT_STACK, (8 * 1024 * 1024, hard))
    failures = []
    pages = [p for p in manifest["pages"] if p.get("gate") and p.get("path")]
    for page in pages:
        extraction, err = dump_extract(engine, f"{base}/{page['path']}", cap_ms, timeout_s,
                                       page.get("expect_exit", 0))
        if err:
            failures.append(f"engine  {page['id']}: {err}")
    print(f"  engine in 8 MiB: {len(pages) - len([f for f in failures if f.startswith('engine')])}"
          f" of {len(pages)} pages")

    r = subprocess.run([engine, "--url", f"{base}/43-deep-recursion.html", "--dump-dom",
                        "--js-stack-kib", "8192", "--script-cap-ms", str(cap_ms)],
                       capture_output=True, text=True, timeout=timeout_s)
    if r.returncode >= 0:
        failures.append("negative control: a JS limit of the whole 8 MiB stack did not crash "
                        f"(exit {r.returncode}), so the engine run models nothing")
    else:
        print(f"  negative control: the whole stack as the JS limit crashed (signal {-r.returncode})")

    with tempfile.TemporaryDirectory(prefix="distill-stack-") as scratch:
        wrapper = os.path.join(scratch, "engine")
        with open(wrapper, "w", encoding="utf-8") as fh:
            fh.write(f'#!/bin/sh\nulimit -s 8192\nexec "{engine}" "$@"\n')
        os.chmod(wrapper, 0o700)
        os.environ["DISTILL_ENGINE"] = wrapper
        tui = find_tui()
        try:
            tui_suite.SHELL_STACK_KIB = 64
            results = tui_suite.run(tui, base)
            broken = [label for status, label, _ in results if status != "pass"]
            for label in broken:
                failures.append(f"shell in 64 KiB: {label}")
            print(f"  shell in 64 KiB: {len(results) - len(broken)} of {len(results)} terminal checks")

            tui_suite.SHELL_STACK_KIB = 16
            home = tempfile.mkdtemp(prefix="distill-stack-home-", dir=scratch)
            os.makedirs(os.path.join(home, "config"), exist_ok=True)
            terminal = tui_suite.Terminal(tui, f"{base}/09-article.html", home=home)
            rendered = terminal.wait_for("A careful reader", 5)
            terminal.send("q")
            status = terminal.finish()
            if rendered:
                failures.append("negative control: the shell ran in 16 KiB of stack, "
                                "so the 64 KiB run models nothing")
            else:
                print(f"  negative control: the shell did not run in 16 KiB (status {status})")
        finally:
            tui_suite.SHELL_STACK_KIB = None
            del os.environ["DISTILL_ENGINE"]
    print()
    for failure in failures:
        print(f"  {RED}FAIL{RESET}  {failure}")
    if failures:
        print(f"{RED}FAILED{RESET}  {len(failures)} stack check(s)")
        return 1
    print(f"{GREEN}OK{RESET}  the engine runs in Muroxi's reservation and the shell in its guarantee")
    return 0


def check_engine_files(engine, port, cap_ms, timeout_s):
    """The engine writes nothing of its own to disk.

    Cookies are kept in memory, and in the session file when there is one;
    no run makes a temporary cookie jar. A jar in /tmp outlived any run that
    crashed, and Muroxi's /tmp is volatile besides. The old jar was removed at
    exit, so this looks during the run: 03b-hang holds it open for the whole
    settle budget.
    """
    failures = []

    def in_tmp():
        # An engine that ignores TMPDIR writes to /tmp itself.
        try:
            return {name for name in os.listdir("/tmp") if name.startswith("distill")}
        except OSError:
            return set()

    with tempfile.TemporaryDirectory(prefix="distill-files-") as scratch:
        env = dict(os.environ, TMPDIR=scratch)
        tmp_before = in_tmp()
        try:
            child = subprocess.Popen([engine, "--url", f"http://127.0.0.1:{port}/03b-hang.html",
                                      "--extract", "--script-cap-ms", str(cap_ms)],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
            time.sleep(min(1.5, cap_ms / 2000))
            during = os.listdir(scratch) + sorted(in_tmp() - tmp_before)
            child.wait(timeout=timeout_s)
            after = os.listdir(scratch)
        except (subprocess.TimeoutExpired, OSError) as exc:
            return [f"engine run: {exc}"]
        if during:
            failures.append(f"the engine made temporary files during a run: {', '.join(sorted(during))}")
        if after:
            failures.append(f"the engine left files behind: {', '.join(sorted(after))}")
    return failures


def json_path(value, path):
    """Read a small dotted JSON path, with optional numeric array indexes."""
    current = value
    for part in path.split(".") if path else []:
        while part.endswith("]") and "[" in part:
            name, index = part.rsplit("[", 1)
            index = index[:-1]
            if name:
                if not isinstance(current, dict) or name not in current:
                    return None, False
                current = current[name]
            try:
                current = current[int(index)]
            except (IndexError, TypeError, ValueError):
                return None, False
            part = ""
        if part:
            if not isinstance(current, dict) or part not in current:
                return None, False
            current = current[part]
    return current, True


def extraction_text(extraction, path=None):
    value, found = json_path(extraction, path) if path else (extraction, True)
    if not found:
        return None, False
    if isinstance(value, str):
        return value, True
    return json.dumps(value, ensure_ascii=False, sort_keys=True), True


def dump_cells(engine, url, columns, cap_ms, timeout_s):
    """The reader page laid out in character cells, as the terminal shows it.

    Produced by the same pure renderer the terminal front end draws with, with
    no terminal involved -- which is what makes terminal rendering something a
    gate can assert on at all.
    """
    with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as fh:
        path = fh.name
    cmd = [engine, "--url", url, "--dump-cells", path, "--columns", str(columns),
           "--script-cap-ms", str(cap_ms)]
    try:
        r = run_engine(cmd, timeout_s)
        if r.returncode != 0:
            return None, f"cells exit {r.returncode}: {r.stderr.strip()[:200]}"
        with open(path, encoding="utf-8") as fh:
            return fh.read(), None
    except subprocess.TimeoutExpired:
        return None, f"cells timed out after {timeout_s}s"
    except OSError as exc:
        return None, f"cells unavailable: {exc}"
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def cell_columns(a):
    """The widths a cells assertion runs at: one, or a sweep."""
    columns = a.get("columns", 80)
    return columns if isinstance(columns, list) else [columns]


def line_cells(line):
    """Terminal cells a line occupies, measured independently of the engine's
    own table so the width check is not the renderer grading itself."""
    if _wcswidth is not None:
        width = _wcswidth(line)
        if width >= 0:
            return width
    return sum(0 if unicodedata.category(ch) in ("Mn", "Me", "Cf")
               else 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
               for ch in line)


def check_cells_fit(cells):
    """Every dump must fit its terminal. Wrapping bugs hide at one width, so
    this runs at every width a page asks for, not only where its content
    assertions look."""
    results, failed = [], 0
    for columns in sorted(cells):
        text = cells[columns]
        if text is None:
            continue
        widest, where = 0, 0
        for number, line in enumerate(text.splitlines(), 1):
            width = line_cells(line)
            if width > widest:
                widest, where = width, number
        ok = widest <= columns
        results.append(("pass" if ok else "fail", f"cells@{columns} fits the terminal",
                        "" if ok else f"line {where} is {widest} cells"))
        failed += 0 if ok else 1
    return results, failed


def check_budgets(page, stats):
    if ENGINE_HOST is not None:
        return [("pending", "budgets measure a process; not checked in-process", "")], 0
    return check_process_budgets(page, stats)


def check_process_budgets(page, stats):
    """A page may assert what its extraction run costs: max_rss_mib for peak
    memory, max_run_ms for wall time, max_fetches_at_once for concurrency. Both regressions are invisible to every
    content assertion -- the output is identical, only the process is larger
    or slower -- so they need their own."""
    results, failed = [], 0
    checks = []
    if page.get("max_rss_mib"):
        rss = stats.get("rss_peak_kib")
        checks.append((f"peak RSS within {page['max_rss_mib']} MiB",
                       None if rss is None else rss <= page["max_rss_mib"] * 1024,
                       None if rss is None else f"{rss / 1024:.1f} MiB"))
    if page.get("max_run_ms"):
        total = (stats.get("ms") or {}).get("total")
        checks.append((f"run within {page['max_run_ms']} ms",
                       None if total is None else total <= page["max_run_ms"],
                       None if total is None else f"{total} ms"))
    if page.get("max_fetches_at_once"):
        at_once = stats.get("fetches_at_once")
        checks.append((f"at most {page['max_fetches_at_once']} fetches at once",
                       None if at_once is None else at_once <= page["max_fetches_at_once"],
                       None if at_once is None else f"{at_once} at once, {stats.get('fetches')} in all"))
    for label, ok, detail in checks:
        if ok is None:
            results.append(("fail", label, "the engine wrote no stats line"))
            failed += 1
        else:
            results.append(("pass" if ok else "fail", label, detail))
            failed += 0 if ok else 1
    return results, failed


def check_substrings(a, text, label):
    """Shared expect / expect_absent checking over one blob of text."""
    results, failed = [], 0
    if "expect" in a:
        want = a["expect"]
        ok = want in text
        results.append(("pass" if ok else "fail", f'{label} \u2283 "{want}"',
                        "" if ok else f"got {len(text)} chars"))
        failed += 0 if ok else 1
    for bad in a.get("expect_absent", []):
        ok = bad not in text
        results.append(("pass" if ok else "fail", f'{label} \u2285 "{bad}"',
                        "" if ok else "present"))
        failed += 0 if ok else 1
    return results, failed


def check_page(page, html=None, baseline=False, extraction=None, template=None,
               markdown=None, cells=None):
    """(results, n_failed) for DOM, extraction or reader-template assertions."""
    results, failed = [], 0
    for a in page["assertions"]:
        stage = a.get("stage")
        if stage == "cells":
            for columns in cell_columns(a):
                text = cells.get(columns) if cells else None
                if text is None:
                    results.append(("pending", f"cells@{columns}-stage, use --extract", ""))
                    continue
                more, bad = check_substrings(a, text, f"cells@{columns}")
                results.extend(more)
                failed += bad
            continue
        if stage == "markdown":
            if markdown is None:
                results.append(("pending", "markdown-stage, use --extract", ""))
                continue
            more, bad = check_substrings(a, markdown, "markdown")
            results.extend(more)
            failed += bad
            continue
        if stage == "template":
            if template is None:
                results.append(("pending", "template-stage, use --extract", ""))
                continue
            more, bad = check_substrings(a, template, "template")
            results.extend(more)
            failed += bad
            continue
        if stage == "extract":
            if extraction is None:
                results.append(("pending", "extract-stage, use --extract", ""))
                continue
            text, found = extraction_text(extraction, a.get("path"))
            if not found:
                results.append(("fail", f'extract path {a.get("path")!r} not found', ""))
                failed += 1
                continue
            if "expect" in a:
                want = a["expect"]
                ok = want in text
                results.append(("pass" if ok else "fail",
                                f'extract {a.get("path", "$")} ⊃ "{want}"',
                                "" if ok else f"got: {text[:240]}"))
                failed += 0 if ok else 1
            for bad in a.get("expect_absent", []):
                ok = bad not in text
                results.append(("pass" if ok else "fail",
                                f'extract {a.get("path", "$")} ⊅ "{bad}"',
                                "" if ok else f"got: {text[:240]}"))
                failed += 0 if ok else 1
            continue
        if stage != "dom":
            results.append(("pending", "unknown assertion stage", ""))
            continue
        if extraction is not None or template is not None:
            continue

        sel = a.get("selector")
        text = element_text(html, sel) if sel else html

        if text is None:
            results.append(("fail", f"{sel} not found in DOM", ""))
            failed += 1
            continue

        if "expect" in a:
            want = a.get("browser_expect", a["expect"]) if baseline else a["expect"]
            ok = want in text
            note = "  (diverges)" if baseline and "browser_expect" in a else ""
            results.append(("pass" if ok else "fail",
                            f'{sel} ⊃ "{want}"{note}',
                            "" if ok else f"got: {text[:160]}"))
            failed += 0 if ok else 1

        for bad in a.get("expect_absent", []):
            ok = bad not in text
            results.append(("pass" if ok else "fail",
                            f'{sel} ⊅ "{bad}"',
                            "" if ok else f"got: {text[:160]}"))
            failed += 0 if ok else 1

    return results, failed


def check_golden(page, extraction):
    """Check stable golden fields while ignoring request-specific URLs."""
    golden_name = page.get("golden")
    if not golden_name:
        return [], 0
    golden_path = os.path.join(HERE, golden_name)
    try:
        golden = json.load(open(golden_path, encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [("fail", f"golden {golden_name} unavailable", str(exc))], 1

    results, failed = [], 0
    for check in golden.get("checks", []):
        path = check.get("path", "")
        actual, found = extraction_text(extraction, path)
        want = str(check.get("contains", ""))
        ok = found and want in actual
        results.append(("pass" if ok else "fail",
                        f'golden {path or "$"} ⊃ "{want}"',
                        "" if ok else ("path not found" if not found else f"got: {actual[:240]}")))
        failed += 0 if ok else 1
    return results, failed


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--id", help="run one page by id")
    ap.add_argument("--live", action="store_true", help="also record live rungs")
    ap.add_argument("--baseline", action="store_true",
                    help="validate the fixtures against the reference browser: honour "
                         "browser_expect where we deliberately diverge from a browser")
    ap.add_argument("--extract", action="store_true",
                    help="run the Phase 1 extractor and check extract-stage assertions")
    ap.add_argument("--render", metavar="DIR",
                    help="render each corpus page to a Phase 2 PNG in DIR")
    ap.add_argument("--viewer", metavar="DIR",
                    help="render the gallery fixture through the Phase 2 media viewer")
    ap.add_argument("--app", action="store_true",
                    help="run the Phase 3 desktop-shell model smoke test")
    ap.add_argument("--stack", action="store_true",
                    help="model Muroxi's stacks: engine in 8 MiB, shell in 64 KiB")
    ap.add_argument("--in-process", action="store_true",
                    help="run the engine in-process, every page in one distill-host")
    ap.add_argument("--tui", action="store_true",
                    help="Phase 6: drive the terminal shell through a pty")
    ap.add_argument("--bundles", action="store_true",
                    help="also run saved local bundles as recorded, non-gating checks")
    ap.add_argument("--arm", choices=("a", "b", "b-pruned"), default="a",
                    help="engine arm: a is the C23 runtime with Lexbor extraction (default), b is the JS-side linkedom comparison, b-pruned is the no-Lexbor/litehtml baseline")
    ap.add_argument("--keep-dom", metavar="DIR", help="save each dumped DOM")
    ap.add_argument("--budget", type=int, default=6000, help="virtual time budget, ms")
    ap.add_argument("--timeout", type=int, default=45, help="real-time cap per page, s")
    args = ap.parse_args()

    if args.app:
        incompatible = args.id or args.live or args.baseline or args.extract or args.render or args.viewer or args.bundles or args.keep_dom or args.tui
        if incompatible:
            ap.error("--app is a standalone Phase 3 shell smoke test")
        app = find_app()
        result = subprocess.run([app, "--self-test"], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stderr.strip() or result.stdout.strip())
            return 1
        print(f"app:    {app}\nmode:   Phase 3 desktop shell\n{result.stdout.strip()}\nOK  desktop-shell gate passes")
        return 0

    # The corpus has one profile-only fixture. Keep its profile files separate
    # from the installed site profiles so tests do not make loopback URLs look
    # like production sites.
    if not args.baseline:
        os.environ.setdefault("DISTILL_PROFILE_DIR", os.path.join(HERE, "profiles"))

    manifest = json.load(open(MANIFEST, encoding="utf-8"))
    if args.baseline and args.arm != "a":
        ap.error("--baseline is only valid with the default reference-browser comparison")
    if args.baseline and args.extract:
        ap.error("--extract is only valid with the engine")
    if args.baseline and args.render:
        ap.error("--render is only valid with the engine")
    if args.baseline and args.viewer:
        ap.error("--viewer is only valid with the engine")
    if args.extract and args.arm != "a":
        ap.error("--extract currently targets the selected Arm A implementation")
    if args.render and args.arm != "a":
        ap.error("--render currently targets the selected Arm A implementation")
    if args.viewer and args.arm != "a":
        ap.error("--viewer currently targets the selected Arm A implementation")
    if args.baseline and args.bundles:
        ap.error("--bundles is only valid with the engine")
    if args.bundles and args.arm != "a":
        ap.error("--bundles currently targets the selected Arm A implementation")
    if (args.render or args.viewer) and (args.extract or args.bundles):
        ap.error("--render/--viewer cannot be combined with --extract or --bundles")
    if args.render and args.viewer:
        ap.error("--render and --viewer are mutually exclusive")
    browser = find_browser() if args.baseline else None
    engine = find_engine() if not args.baseline and args.arm == "a" else None
    if args.in_process:
        if args.baseline or args.arm != "a" or args.render or args.viewer or args.tui:
            ap.error("--in-process runs the engine's DOM and extraction gates")
        global ENGINE_HOST
        host = os.path.join(os.path.dirname(HERE), "build", "distill-host")
        if not os.access(host, os.X_OK):
            sys.exit("no distill-host found; build it first (cmake --build build)")
        ENGINE_HOST = EngineHost(host)
    arm_b = find_arm_b(args.arm == "b-pruned") if not args.baseline and args.arm in ("b", "b-pruned") else None
    port = free_port()

    if args.keep_dom:
        os.makedirs(args.keep_dom, exist_ok=True)
    if args.render:
        os.makedirs(args.render, exist_ok=True)
    if args.viewer:
        os.makedirs(args.viewer, exist_ok=True)

    server = subprocess.Popen([sys.executable, SERVE, "-p", str(port), "-q"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=0.5).read()
                break
            except (urllib.error.URLError, ConnectionError, OSError):
                time.sleep(0.1)
        else:
            sys.exit("corpus server did not come up")

        if args.stack:
            return run_stack_gate(engine, port, manifest, args.timeout)

        if args.tui:
            import tui_suite
            tui = find_tui()
            print(f"{DIM}tui:     {tui}\nserver:  http://127.0.0.1:{port}/\nmode:    Phase 6 terminal shell (pty){RESET}\n")
            results = tui_suite.run(tui, f"http://127.0.0.1:{port}")
            failed = 0
            for status, label, detail in results:
                mark = f"{GREEN}pass{RESET}" if status == "pass" else f"{RED}FAIL{RESET}"
                print(f"  {mark}  {label}" + (f"  {DIM}{detail[:300]}{RESET}" if detail else ""))
                failed += status != "pass"
            print()
            if failed:
                print(f"{RED}FAILED{RESET}  {failed} terminal check(s)")
                return 1
            print(f"{GREEN}OK{RESET}  all {len(results)} terminal checks pass")
            return 0

        pages = manifest["pages"]
        if args.id:
            pages = [p for p in pages if p["id"] == args.id]
            if not pages:
                sys.exit(f"no page with id {args.id!r}")

        mode = "baseline (reference browser)" if args.baseline else ("Arm B' (no-Lexbor + litehtml)" if args.arm == "b-pruned" else ("Arm B (JS-side linkedom)" if args.arm == "b" else ("Phase 2 media viewer" if args.viewer else ("Phase 2 reader PNG" if args.render else ("Arm A extraction (C23)" if args.extract else "Arm A (C23 runtime + Lexbor extraction)")))))
        driver = browser if args.baseline else (arm_b if args.arm in ("b", "b-pruned") else engine)
        print(f"{DIM}{'browser' if args.baseline else 'driver'}: {driver}\nserver:  http://127.0.0.1:{port}/\nmode:    {mode}{RESET}\n")

        total_failed, gates_failed = 0, []
        if args.viewer:
            pages = [p for p in pages if p["id"] == "15-gallery"]
            if not pages:
                sys.exit("--viewer requires the 15-gallery fixture")
        for page in pages:
            url = f"http://127.0.0.1:{port}/{page['path']}"
            t0 = time.time()
            template = None
            markdown = None
            cells = None
            session_path = None
            session_error = None
            if page.get("session"):
                handle, session_path = tempfile.mkstemp(prefix="corpus-session-")
                os.close(handle)
                os.chmod(session_path, 0o600)
                session_error = run_session_steps(
                    engine, f"http://127.0.0.1:{port}", page["session"],
                    session_path, manifest.get("settle_cap_ms", 5000), args.timeout)
            if args.render:
                output_path = os.path.join(args.render, page["id"] + ".png")
                render_info, err = render_page(engine, url, output_path,
                                               manifest.get("settle_cap_ms", 5000), args.timeout,
                                               expect_exit=page.get("expect_exit", 0),
                                               fit_height=bool(page.get("render_fit")))
                html = None
                extraction = None
            elif args.viewer:
                output_path = os.path.join(args.viewer, page["id"] + ".png")
                render_info, err = render_page(engine, url, output_path,
                                               manifest.get("settle_cap_ms", 5000), args.timeout,
                                               viewer=True, expect_exit=page.get("expect_exit", 0))
                html = None
                extraction = None
            elif args.extract:
                run_stats = {} if any(page.get(k) for k in
                                      ("max_rss_mib", "max_run_ms", "max_fetches_at_once")) else None
                extraction, err = dump_extract(engine, url, manifest.get("settle_cap_ms", 5000),
                                               args.timeout, page.get("expect_exit", 0),
                                               stats=run_stats)
                html = None
                # The reader template is produced in the same pass, so a page
                # that asserts on it costs one extra engine run and only when
                # it asks for one.
                if any(a.get("stage") == "template" for a in page["assertions"]):
                    template, template_error = dump_template(
                        engine, url, manifest.get("settle_cap_ms", 5000), args.timeout)
                    if template_error and not err:
                        err = template_error
                if any(a.get("stage") == "markdown" for a in page["assertions"]):
                    markdown, markdown_error = dump_markdown(
                        engine, url, manifest.get("settle_cap_ms", 5000), args.timeout)
                    if markdown_error and not err:
                        err = markdown_error
                widths = sorted({c for a in page["assertions"] if a.get("stage") == "cells"
                                 for c in cell_columns(a)})
                if widths:
                    cells = {}
                    for columns in widths:
                        cells[columns], cells_error = dump_cells(
                            engine, url, columns, manifest.get("settle_cap_ms", 5000), args.timeout)
                        if cells_error and not err:
                            err = cells_error
            elif args.baseline:
                html, err = dump_dom(browser, url, args.budget, args.timeout)
                extraction = None
            elif args.arm in ("b", "b-pruned"):
                html, err = dump_arm_b(arm_b, url, manifest.get("settle_cap_ms", 5000), args.timeout)
                extraction = None
            else:
                html, err = dump_engine(engine, url, manifest.get("settle_cap_ms", 5000),
                                        args.timeout, page.get("expect_exit", 0),
                                        session_path)
                extraction = None
            if session_error:
                err = session_error
            if session_path:
                os.unlink(session_path)
            elapsed = time.time() - t0

            print(f"{page['id']}  {DIM}{page['plan_section']}{RESET}")
            if err:
                if args.baseline and page.get("browser_expect_timeout") and "timed out" in err:
                    print(f"  {GREEN}pass{RESET}  browser times out, as this fixture requires"
                          f"\n        {DIM}the engine must instead cap and extract{RESET}\n")
                    continue
                print(f"  {RED}ERROR{RESET}  {err}")
                total_failed += 1
                gates_failed.append(page["id"])
                print()
                continue

            if args.render or args.viewer:
                width, height = render_info
                # A page that declares a minimum height is asserting that the
                # render grew to fit its content rather than cropping it.
                floor = page.get("min_render_height", 0)
                if args.render and height < floor:
                    print(f"  {RED}FAIL{RESET}  PNG {width}x{height}, expected at least {floor} tall")
                    total_failed += 1
                    gates_failed.append(page["id"])
                    print(f"  {DIM}{elapsed:.1f}s{RESET}\n")
                    continue
                print(f"  {GREEN}pass{RESET}  PNG {width}x{height} -> {output_path}")
                print(f"  {DIM}{elapsed:.1f}s{RESET}\n")
                continue

            if args.keep_dom:
                keep_name = page["id"] + (".json" if args.extract else ".html")
                with open(os.path.join(args.keep_dom, keep_name), "w",
                          encoding="utf-8") as fh:
                    fh.write(json.dumps(extraction, ensure_ascii=False, indent=2) if args.extract else html)

            results, failed = check_page(page, html, baseline=args.baseline,
                                         extraction=extraction, template=template,
                                         markdown=markdown, cells=cells)
            if cells:
                fit_results, fit_failed = check_cells_fit(cells)
                results.extend(fit_results)
                failed += fit_failed
            if args.extract:
                golden_results, golden_failed = check_golden(page, extraction)
                results.extend(golden_results)
                failed += golden_failed
                if run_stats is not None:
                    budget_results, budget_failed = check_budgets(page, run_stats)
                    results.extend(budget_results)
                    failed += budget_failed
            for status, label, detail in results:
                mark = {"pass": f"{GREEN}pass{RESET}",
                        "fail": f"{RED}FAIL{RESET}",
                        "pending": f"{YELLOW}····{RESET}"}[status]
                print(f"  {mark}  {label}")
                if detail:
                    print(f"        {DIM}{detail}{RESET}")

            print(f"  {DIM}{elapsed:.1f}s{RESET}\n")
            total_failed += failed
            if failed and page.get("gate"):
                gates_failed.append(page["id"])

        if args.bundles:
            bundle_dir = os.path.join(HERE, "local", "bundles")
            print(f"{DIM}--- saved bundles: recorded, never gates ---{RESET}")
            for name in sorted(os.listdir(bundle_dir)):
                if not name.endswith((".html", ".htm")):
                    continue
                path = os.path.join(bundle_dir, name)
                t0 = time.time()
                extraction, err = dump_extract(engine, path, manifest.get("settle_cap_ms", 5000), args.timeout)
                if err:
                    print(f"  {name}  {YELLOW}recorded error{RESET}  {err}")
                    continue
                print(f"  {name}  {extraction['shape']['kind']} / {extraction['extractor']}"
                      f"  {DIM}{time.time() - t0:.1f}s{RESET}")

        if not (args.baseline or args.extract or args.render or args.viewer
                or args.bundles) and args.arm == "a":
            problems = check_transport(engine, port,
                                       manifest.get("settle_cap_ms", 5000), args.timeout)
            label = "transport  Scheme fallback"
            if problems:
                total_failed += len(problems)
                gates_failed.append("transport")
                print(f"  {label}  {RED}fail{RESET}")
                for problem in problems:
                    print(f"    {RED}{problem}{RESET}")
            else:
                print(f"  {label}  {GREEN}pass{RESET}\n")
            problems = check_engine_files(engine, port,
                                          manifest.get("settle_cap_ms", 5000), args.timeout)
            label = "engine     No temporary files"
            if problems:
                total_failed += len(problems)
                gates_failed.append("engine-files")
                print(f"  {label}  {RED}fail{RESET}")
                for problem in problems:
                    print(f"    {RED}{problem}{RESET}")
            else:
                print(f"  {label}  {GREEN}pass{RESET}\n")

        if args.live:
            print(f"{DIM}--- live rungs: recorded, never gates ---{RESET}")
            for rung in sorted(manifest["live"], key=lambda r: r["rung"]):
                flag = " (diagnostic)" if rung.get("diagnostic") else ""
                print(f"  {rung['rung']}. {rung['id']}{flag}\n     {DIM}{rung['url']}{RESET}")
            print(f"  {DIM}run these by hand against the engine; record which failure fired{RESET}\n")

        if gates_failed:
            print(f"{RED}FAILED{RESET}  {total_failed} assertion(s); gates: {', '.join(gates_failed)}")
            return 1
        print(f"{GREEN}OK{RESET}  all gate assertions pass")
        return 0
    finally:
        if ENGINE_HOST is not None:
            ENGINE_HOST.close()
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    sys.exit(main())
