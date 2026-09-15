#!/usr/bin/env python3
"""Run the corpus assertions.

Starts serve.py, drives the Phase 0 engine over each page, and checks the
'dom'-stage assertions in manifest.json against the post-JS DOM. Use
--baseline to validate the fixtures against a reference browser instead.

    ./check.py                 all local gates
    ./check.py --extract       Phase 1 extraction and golden gates
    ./check.py --render DIR    Phase 2 PNG smoke renders
    ./check.py --viewer DIR    Phase 2 media-viewer smoke render
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


def dump_extract(engine, url, cap_ms, timeout_s):
    """Structured extraction JSON for a URL, or (None, reason)."""
    cmd = [engine, "--url", url, "--extract", "--script-cap-ms", str(cap_ms)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return None, f"extractor timed out after {timeout_s}s"
    if r.returncode != 0:
        return None, f"extractor exit {r.returncode}: {r.stderr.strip()[:300]}"
    try:
        return json.loads(r.stdout), None
    except json.JSONDecodeError as exc:
        return None, f"extractor returned invalid JSON: {exc}"


def render_page(engine, url, output_path, cap_ms, timeout_s, viewer=False):
    """Render a reader PNG and verify its basic PNG dimensions."""
    cmd = [engine, "--url", url, "--viewer" if viewer else "--render", output_path,
           "--script-cap-ms", str(cap_ms)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return None, f"renderer timed out after {timeout_s}s"
    if r.returncode != 0:
        return None, f"renderer exit {r.returncode}: {r.stderr.strip()[:300]}"
    try:
        with open(output_path, "rb") as fh:
            header = fh.read(24)
        if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
            return None, "renderer did not write a PNG"
        width = int.from_bytes(header[16:20], "big")
        height = int.from_bytes(header[20:24], "big")
        if width <= 0 or height <= 0:
            return None, "renderer wrote an empty PNG"
        return (width, height), None
    except OSError as exc:
        return None, f"renderer output unavailable: {exc}"


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


def check_page(page, html=None, baseline=False, extraction=None):
    """(results, n_failed) for DOM or extraction assertions."""
    results, failed = [], 0
    for a in page["assertions"]:
        stage = a.get("stage")
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
        if extraction is not None:
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
    ap.add_argument("--bundles", action="store_true",
                    help="also run saved local bundles as recorded, non-gating checks")
    ap.add_argument("--arm", choices=("a", "b", "b-pruned"), default="a",
                    help="engine arm: a is the C23 runtime with Lexbor extraction (default), b is the JS-side linkedom comparison, b-pruned is the no-Lexbor/litehtml baseline")
    ap.add_argument("--keep-dom", metavar="DIR", help="save each dumped DOM")
    ap.add_argument("--budget", type=int, default=6000, help="virtual time budget, ms")
    ap.add_argument("--timeout", type=int, default=45, help="real-time cap per page, s")
    args = ap.parse_args()

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
            if args.render:
                output_path = os.path.join(args.render, page["id"] + ".png")
                render_info, err = render_page(engine, url, output_path,
                                               manifest.get("settle_cap_ms", 5000), args.timeout)
                html = None
                extraction = None
            elif args.viewer:
                output_path = os.path.join(args.viewer, page["id"] + ".png")
                render_info, err = render_page(engine, url, output_path,
                                               manifest.get("settle_cap_ms", 5000), args.timeout,
                                               viewer=True)
                html = None
                extraction = None
            elif args.extract:
                extraction, err = dump_extract(engine, url, manifest.get("settle_cap_ms", 5000), args.timeout)
                html = None
            elif args.baseline:
                html, err = dump_dom(browser, url, args.budget, args.timeout)
                extraction = None
            elif args.arm in ("b", "b-pruned"):
                html, err = dump_arm_b(arm_b, url, manifest.get("settle_cap_ms", 5000), args.timeout)
                extraction = None
            else:
                html, err = dump_engine(engine, url, manifest.get("settle_cap_ms", 5000), args.timeout)
                extraction = None
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
                print(f"  {GREEN}pass{RESET}  PNG {width}x{height} -> {output_path}")
                print(f"  {DIM}{elapsed:.1f}s{RESET}\n")
                continue

            if args.keep_dom:
                keep_name = page["id"] + (".json" if args.extract else ".html")
                with open(os.path.join(args.keep_dom, keep_name), "w",
                          encoding="utf-8") as fh:
                    fh.write(json.dumps(extraction, ensure_ascii=False, indent=2) if args.extract else html)

            results, failed = check_page(page, html, baseline=args.baseline, extraction=extraction)
            if args.extract:
                golden_results, golden_failed = check_golden(page, extraction)
                results.extend(golden_results)
                failed += golden_failed
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
