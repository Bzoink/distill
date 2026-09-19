#!/usr/bin/env python3
"""Measure what a navigation costs, for the Muroxi budgets (muroxi_compat.md, M0).

Runs the engine the way the terminal shell runs it -- --template, a 5 s
settle cap -- over the local corpus, and optionally over live sites and
through the TUI suite, and records per page:

  peak RSS and JS heap        from a run with only DISTILL_STATS set
  peak native stack           from a second run with DISTILL_STACK_PROBE,
                              since painting the stack inflates RSS
  fetches, most at once       transfers started, and the most attached at once
  phase times                 including loading linkedom and Readability, which
                              are embedded bytecode since M2 (parsed from source
                              before; M0's rows are keyed *_compile)

The live sites are a diagnostic, never a gate: they redesign, challenge and
throttle, and a whole-list run degrades partway (see compare_sites.py). They
are here because the local fixtures are small by design and would badly
understate what a real page costs. A live row whose fetch failed says nothing
about budgets and is reported as failed, not as small.

    python3 tools/measure_budgets.py                      local corpus
    python3 tools/measure_budgets.py --live               also live sites
    python3 tools/measure_budgets.py --tui                also the TUI's own stack
    python3 tools/measure_budgets.py --out reports/m0     keep raw lines and a table
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "corpus")
sys.path.insert(0, CORPUS)
import check  # noqa: E402  (the corpus runner's helpers: engine, port, server)

ENGINE_PROBE_KIB = 7 * 1024   # inside Linux's default 8 MiB stack limit
SHELL_PROBE_KIB = 1024
CAP_MS = 5000


def run_engine(engine, url, env_extra, timeout_s):
    """Run one navigation as the TUI would; return (exit, stats line or None)."""
    with tempfile.TemporaryDirectory(prefix="distill-m0-") as work:
        stats = os.path.join(work, "stats.jsonl")
        env = dict(os.environ, DISTILL_STATS=stats, **env_extra)
        # No fetch limit: what is measured is what pages ask for, which is
        # what a platform's cap has to be chosen against.
        args = [engine, "--url", url, "--template", os.path.join(work, "page.html"),
                "--progress", "--script-cap-ms", str(CAP_MS), "--max-fetches", "0"]
        try:
            result = subprocess.run(args, env=env, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL, timeout=timeout_s)
            status = result.returncode
        except subprocess.TimeoutExpired:
            return "timeout", None
        lines = []
        if os.path.exists(stats):
            with open(stats, encoding="utf-8") as handle:
                lines = [json.loads(line) for line in handle if line.strip()]
        engine_lines = [line for line in lines if line.get("process") == "engine"]
        return status, (engine_lines[-1] if engine_lines else None)


def measure(engine, page_id, url, timeout_s):
    status, memory = run_engine(engine, url, {}, timeout_s)
    stack_status, stack = run_engine(engine, url,
                                     {"DISTILL_STACK_PROBE": str(ENGINE_PROBE_KIB)}, timeout_s)
    row = {"id": page_id, "url": url, "exit": status, "stack_exit": stack_status}
    if memory:
        row.update({
            "rss_peak_kib": memory["rss_peak_kib"],
            "js_heap_peak": memory["js_heap_peak"],
            "js_heap_final": memory["js_heap_final"],
            "fetches": memory["fetches"],
            "fetches_at_once": memory["fetches_at_once"],
            "scripts_run": memory["scripts_run"],
            "cap_hit": memory["cap_hit"],
            "readability_loads": memory["readability_loads"],
            "ms": memory["ms"],
            "rss_after_kib": memory.get("rss_after_kib"),
        })
    # The two runs are separate navigations, and a live site can serve one and
    # refuse the other. A stack reading from a failed run measures the failure.
    if stack and stack_status in (0, 4):
        row["stack_peak"] = stack["stack_peak"]
        row["stack_saturated"] = stack["stack_saturated"]
    return row


def local_pages():
    manifest = json.load(open(os.path.join(CORPUS, "manifest.json"), encoding="utf-8"))
    return [(page["id"], page["path"]) for page in manifest["pages"] if page.get("path")]


def live_pages():
    manifest = json.load(open(os.path.join(CORPUS, "manifest.json"), encoding="utf-8"))
    sites = json.load(open(os.path.join(CORPUS, "site-comparison.json"), encoding="utf-8"))["sites"]
    pages = [(entry["id"], entry["url"]) for entry in manifest.get("live", [])]
    pages += [(entry["id"], entry["url"]) for entry in sites]
    return pages


def measure_tui(tui, base):
    """The shell's own stack and memory, across the whole TUI suite."""
    import tui_suite
    with tempfile.TemporaryDirectory(prefix="distill-m0-tui-") as work:
        stats = os.path.join(work, "stats.jsonl")
        os.environ["DISTILL_STATS"] = stats
        os.environ["DISTILL_STACK_PROBE"] = str(SHELL_PROBE_KIB)
        try:
            results = tui_suite.run(tui, base)
        finally:
            del os.environ["DISTILL_STATS"]
            del os.environ["DISTILL_STACK_PROBE"]
        lines = []
        if os.path.exists(stats):
            with open(stats, encoding="utf-8") as handle:
                lines = [json.loads(line) for line in handle if line.strip()]
    failed = [label for status, label, _ in results if status == "fail"]
    shells = [line for line in lines if line.get("process") == "tui"]
    return {
        "runs": len(shells),
        "suite_failures": failed,
        "stack_peak": max((line["stack_peak"] or 0 for line in shells), default=None),
        "stack_saturated": any(line["stack_saturated"] for line in shells),
        "rss_peak_kib": max((line["rss_peak_kib"] for line in shells), default=None),
    }


def kib(value):
    return "—" if value is None else f"{value / 1024:.0f}"


def mib(value):
    return "—" if value is None else f"{value / (1024 * 1024):.1f}"


def table(rows):
    out = ["| Page | Exit | RSS MiB | JS heap MiB | Stack KiB | Fetches | At once | "
           "Total ms | linkedom load ms | Readability load ms |",
           "|---|---|---|---|---|---|---|---|---|---|"]
    for row in rows:
        stack = kib(row.get("stack_peak"))
        if row.get("stack_saturated"):
            stack = "≥" + stack
        ms = row.get("ms") or {}
        rss = row.get("rss_peak_kib")
        out.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            row["id"], row["exit"], "—" if rss is None else f"{rss / 1024:.1f}",
            mib(row.get("js_heap_peak")), stack, row.get("fetches", "—"),
            row.get("fetches_at_once", "—"), ms.get("total", "—"),
            ms.get("linkedom_load", ms.get("linkedom_compile", "—")),
            ms.get("readability_load", ms.get("readability_compile", "—"))))
    return "\n".join(out)


def summary(rows, label):
    usable = [row for row in rows if row["exit"] in (0, 4) and row.get("rss_peak_kib")]
    if not usable:
        return f"{label}: no usable runs"

    def top(key, scale=1):
        best = max(usable, key=lambda row: row.get(key) or 0)
        return f"{(best.get(key) or 0) / scale:.1f} ({best['id']})"

    return "\n".join([
        f"{label}: {len(usable)} of {len(rows)} runs usable",
        f"  peak RSS MiB       max {top('rss_peak_kib', 1024)}",
        f"  JS heap MiB        max {top('js_heap_peak', 1024 * 1024)}",
        f"  stack KiB          max {top('stack_peak', 1024)}",
        f"  fetches            max {top('fetches')}",
        f"  fetches at once    max {top('fetches_at_once')}",
    ])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--live", action="store_true", help="also measure live sites (diagnostic)")
    ap.add_argument("--live-only", action="store_true", help="measure live sites only")
    ap.add_argument("--tui", action="store_true", help="also measure the TUI through its suite")
    ap.add_argument("--id", action="append", help="measure only these ids (repeatable)")
    ap.add_argument("--pause", type=float, default=2.0,
                    help="seconds between live sites, to stay clear of throttling")
    ap.add_argument("--timeout", type=int, default=60, help="real-time cap per run, s")
    ap.add_argument("--out", metavar="DIR", help="write rows.json and table.md here")
    ap.add_argument("--report", metavar="ROWS",
                    help="re-render the tables from a saved rows.json instead of measuring")
    args = ap.parse_args()

    if args.report:
        report = json.load(open(args.report, encoding="utf-8"))
        for row in report["local"] + report["live"]:
            if row.get("stack_exit") not in (0, 4):
                row.pop("stack_peak", None)
                row.pop("stack_saturated", None)
        print(render(report))
        return 0

    engine = check.find_engine()
    os.environ.setdefault("DISTILL_PROFILE_DIR", os.path.join(CORPUS, "profiles"))
    port = check.free_port()
    server = subprocess.Popen([sys.executable, check.SERVE, "-p", str(port), "-q"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    base = f"http://127.0.0.1:{port}"
    report = {"engine": engine, "local": [], "live": [], "tui": None}
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(f"{base}/health", timeout=0.5).read()
                break
            except (urllib.error.URLError, ConnectionError, OSError):
                time.sleep(0.1)
        else:
            sys.exit("corpus server did not come up")

        if not args.live_only:
            for page_id, path in local_pages():
                if args.id and page_id not in args.id:
                    continue
                row = measure(engine, page_id, f"{base}/{path}", args.timeout)
                report["local"].append(row)
                print(f"local {page_id}: exit {row['exit']}", file=sys.stderr)
        if args.live or args.live_only:
            for page_id, url in live_pages():
                if args.id and page_id not in args.id:
                    continue
                row = measure(engine, page_id, url, args.timeout)
                report["live"].append(row)
                print(f"live  {page_id}: exit {row['exit']}", file=sys.stderr)
                time.sleep(args.pause)
        if args.tui:
            report["tui"] = measure_tui(check.find_tui(), base)
    finally:
        server.terminate()
        server.wait()

    text = render(report)
    print(text)
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "rows.json"), "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2)
        with open(os.path.join(args.out, "table.md"), "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    return 0


def render(report):
    parts = []
    if report["local"]:
        parts += ["## Local corpus", "", table(report["local"]), ""]
    if report["live"]:
        parts += ["## Live sites (diagnostic)", "", table(report["live"]), ""]
    parts += ["```", summary(report["local"], "local") if report["local"] else "",
              summary(report["live"], "live") if report["live"] else "", "```"]
    if report["tui"]:
        tui = report["tui"]
        parts += ["", "## Terminal shell", "",
                  f"{tui['runs']} shell runs; peak stack {kib(tui['stack_peak'])} KiB"
                  f"{' (saturated)' if tui['stack_saturated'] else ''}; "
                  f"peak RSS {kib(tui['rss_peak_kib'] and tui['rss_peak_kib'] * 1024)} KiB; "
                  f"suite failures: {len(tui['suite_failures'])}"]
    return "\n".join(parts)


if __name__ == "__main__":
    sys.exit(main())
