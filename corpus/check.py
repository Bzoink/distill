#!/usr/bin/env python3
"""Run the corpus assertions.

Starts serve.py, drives the Phase 0 engine over each page, and checks the
'dom'-stage assertions in manifest.json against the post-JS DOM. Use
--baseline to validate the fixtures against a reference browser instead.

    ./check.py                 all local gates
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
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from html.parser import HTMLParser

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


def dump_engine(engine, url, cap_ms, timeout_s):
    """Post-JS DOM from the Phase 0 engine, or (None, reason)."""
    cmd = [
        engine, "--url", url, "--dump-dom", "--script-cap-ms", str(cap_ms),
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return None, f"engine timed out after {timeout_s}s"
    if r.returncode != 0:
        return None, f"engine exit {r.returncode}: {r.stderr.strip()[:200]}"
    if not r.stdout.strip():
        return None, f"engine returned no DOM: {r.stderr.strip()[:200]}"
    return r.stdout, None


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


def check_page(page, html, baseline=False):
    """(results, n_failed) for one page's dom-stage assertions."""
    results, failed = [], 0
    for a in page["assertions"]:
        if a.get("stage") != "dom":
            results.append(("pending", "extract-stage, Phase 1", ""))
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--id", help="run one page by id")
    ap.add_argument("--live", action="store_true", help="also record live rungs")
    ap.add_argument("--baseline", action="store_true",
                    help="validate the fixtures against the reference browser: honour "
                         "browser_expect where we deliberately diverge from a browser")
    ap.add_argument("--arm", choices=("a", "b", "b-pruned"), default="a",
                    help="engine arm: a is Lexbor+C bindings (default), b is JS-side linkedom, b-pruned is the no-Lexbor baseline")
    ap.add_argument("--keep-dom", metavar="DIR", help="save each dumped DOM")
    ap.add_argument("--budget", type=int, default=6000, help="virtual time budget, ms")
    ap.add_argument("--timeout", type=int, default=45, help="real-time cap per page, s")
    args = ap.parse_args()

    manifest = json.load(open(MANIFEST, encoding="utf-8"))
    if args.baseline and args.arm != "a":
        ap.error("--baseline is only valid with the default reference-browser comparison")
    browser = find_browser() if args.baseline else None
    engine = find_engine() if not args.baseline and args.arm == "a" else None
    arm_b = find_arm_b(args.arm == "b-pruned") if not args.baseline and args.arm in ("b", "b-pruned") else None
    port = free_port()

    if args.keep_dom:
        os.makedirs(args.keep_dom, exist_ok=True)

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

        pages = manifest["pages"]
        if args.id:
            pages = [p for p in pages if p["id"] == args.id]
            if not pages:
                sys.exit(f"no page with id {args.id!r}")

        mode = "baseline (reference browser)" if args.baseline else ("Arm B' (no-Lexbor + litehtml)" if args.arm == "b-pruned" else ("Arm B (JS-side linkedom)" if args.arm == "b" else "Arm A (Lexbor+C bindings)"))
        driver = browser if args.baseline else (arm_b if args.arm in ("b", "b-pruned") else engine)
        print(f"{DIM}{'browser' if args.baseline else 'driver'}: {driver}\nserver:  http://127.0.0.1:{port}/\nmode:    {mode}{RESET}\n")

        total_failed, gates_failed = 0, []
        for page in pages:
            url = f"http://127.0.0.1:{port}/{page['path']}"
            t0 = time.time()
            if args.baseline:
                html, err = dump_dom(browser, url, args.budget, args.timeout)
            elif args.arm in ("b", "b-pruned"):
                html, err = dump_arm_b(arm_b, url, manifest.get("settle_cap_ms", 5000), args.timeout)
            else:
                html, err = dump_engine(engine, url, manifest.get("settle_cap_ms", 5000), args.timeout)
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

            if args.keep_dom:
                with open(os.path.join(args.keep_dom, page["id"] + ".html"), "w",
                          encoding="utf-8") as fh:
                    fh.write(html)

            results, failed = check_page(page, html, baseline=args.baseline)
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
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    sys.exit(main())
