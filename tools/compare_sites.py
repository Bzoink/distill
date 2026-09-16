#!/usr/bin/env python3
"""Capture and compare a live URL in Chromium and Distill.

This is deliberately a diagnostic harness. It records both raw outputs and
semantic summaries, but it does not claim that a live site is a deterministic
CI assertion. Use the generated report to decide whether a Distill change is
needed, then rerun the same site and compare the saved artifacts.

Examples:

    python3 tools/compare_sites.py --id 01-example-domain
    python3 tools/compare_sites.py https://example.com/ --out reports/site-comparison/example
    python3 tools/compare_sites.py --screenshots --id 06-bbc-news
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urljoin, urlparse


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_MANIFEST = ROOT / "corpus" / "site-comparison.json"
DEFAULT_OUT = ROOT / "reports" / "site-comparison"
BROWSERS = ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable")
SKIP_TAGS = {"script", "style", "template", "noscript", "svg"}
HEADING_TAGS = {"h1", "h2", "h3", "h4", "h5", "h6"}
VOID_TAGS = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr",
}


def fail(message):
    raise SystemExit(message)


def executable(value, label):
    path = shutil.which(value) if value else shutil.which(label)
    if not path:
        fail(f"no {label} executable found")
    return path


def find_browser():
    for name in BROWSERS:
        path = shutil.which(name)
        if path:
            return path
    fail("no Chromium/Chrome executable found")


def valid_url(url):
    parsed = urlparse(url)
    return parsed.scheme in {"http", "https"} and bool(parsed.netloc)


def slug_for_url(url):
    parsed = urlparse(url)
    host = parsed.netloc.replace(":", "-")
    path = re.sub(r"[^A-Za-z0-9]+", "-", parsed.path).strip("-")
    slug = "-".join(part for part in (host, path) if part)
    return slug.lower() or "site"


def load_sites(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"unable to read site manifest {path}: {exc}")
    sites = data.get("sites")
    if not isinstance(sites, list):
        fail(f"site manifest {path} has no sites array")
    for site in sites:
        if not valid_url(site.get("url", "")):
            fail(f"invalid URL for site {site.get('id', '<unknown>')!r}")
    return sites


class VisibleSummary(HTMLParser):
    """Extract a conservative, style-independent summary from dumped HTML."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.frames = []
        self.title_parts = []
        self.text_parts = []
        self.headings = []
        self._heading = None
        self._link_stack = []
        self.links = []
        self.images = []
        self.forms = []

    def _active(self):
        return not self.frames or not self.frames[-1][0] and not self.frames[-1][1]

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        attrs = dict(attrs)
        parent_skip = self.frames[-1][0] if self.frames else False
        parent_hidden = self.frames[-1][1] if self.frames else False
        hidden = (
            "hidden" in attrs
            or (attrs.get("aria-hidden") or "").lower() == "true"
            or "display:none" in (attrs.get("style") or "").replace(" ", "").lower()
            or "visibility:hidden" in (attrs.get("style") or "").replace(" ", "").lower()
        )
        skip = parent_skip or tag in SKIP_TAGS
        hidden = parent_hidden or hidden
        if not skip and not hidden:
            if tag == "a":
                link = {"href": attrs.get("href", ""), "text": []}
                self.links.append(link)
                self._link_stack.append(link)
            elif tag == "img":
                self.images.append({"src": attrs.get("src", ""), "alt": attrs.get("alt", "")})
            elif tag == "form":
                self.forms.append({"action": attrs.get("action", ""), "method": attrs.get("method", "get")})
            if tag in HEADING_TAGS:
                self._heading = [tag, []]
        if tag not in VOID_TAGS:
            self.frames.append((skip, hidden))

    def handle_endtag(self, tag):
        tag = tag.lower()
        if self._active() and tag in HEADING_TAGS and self._heading:
            text = clean_text(" ".join(self._heading[1]))
            if text:
                self.headings.append(text)
            self._heading = None
        if tag == "a" and self._link_stack:
            self._link_stack.pop()
        if self.frames:
            self.frames.pop()

    def handle_data(self, data):
        if not self._active():
            return
        if data.strip():
            self.text_parts.append(data)
            if self._heading:
                self._heading[1].append(data)
            if self._link_stack:
                self._link_stack[-1]["text"].append(data)

    def handle_startendtag(self, tag, attrs):
        tag = tag.lower()
        if tag == "img" and self._active():
            attrs = dict(attrs)
            self.images.append({"src": attrs.get("src", ""), "alt": attrs.get("alt", "")})

    def handle_comment(self, data):
        del data

    def summary(self):
        return {
            "title": clean_text(" ".join(self.title_parts)),
            "text": clean_text(" ".join(self.text_parts)),
            "headings": self.headings,
            "links": self.links,
            "images": self.images,
            "forms": self.forms,
        }


class TextSummary(HTMLParser):
    """Visible text parser used for HTML fragments in extraction JSON."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.skip = 0
        self.parts = []

    def handle_starttag(self, tag, attrs):
        del attrs
        if tag.lower() in SKIP_TAGS:
            self.skip += 1

    def handle_endtag(self, tag):
        if self.skip and tag.lower() in SKIP_TAGS:
            self.skip -= 1

    def handle_data(self, data):
        if not self.skip:
            self.parts.append(data)


class CanonicalParser(HTMLParser):
    """Find a document's canonical URL for nondeterministic redirects."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.url = ""

    def handle_starttag(self, tag, attrs):
        if self.url or tag.lower() != "link":
            return
        attributes = dict(attrs)
        rel = (attributes.get("rel") or "").casefold().split()
        if "canonical" in rel:
            self.url = attributes.get("href", "")


def clean_text(value):
    return " ".join(str(value).split())


def summarize_html(html):
    parser = VisibleSummary()
    parser.feed(html)
    summary = parser.summary()
    # HTMLParser does not expose a title-only callback, so title is obtained
    # with a small second pass. This keeps the structural parser uncomplicated.
    title = re.search(r"<title[^>]*>(.*?)</title\s*>", html, re.I | re.S)
    if title:
        title_parser = TextSummary()
        title_parser.feed(title.group(1))
        summary["title"] = clean_text(" ".join(title_parser.parts))
    for link in summary["links"]:
        link["text"] = clean_text(" ".join(link["text"]))
    return summary


def canonical_url(html, base_url):
    parser = CanonicalParser()
    parser.feed(html)
    return urljoin(base_url, parser.url) if parser.url else ""


def fragment_text(value):
    if not isinstance(value, str):
        return ""
    parser = TextSummary()
    parser.feed(value)
    return clean_text(" ".join(parser.parts))


def extraction_information(data):
    """Collect the information-bearing strings from Distill's extraction schema."""
    if not isinstance(data, dict):
        return ""
    parts = []
    readability = data.get("readability")
    if isinstance(readability, dict):
        parts.extend((readability.get("title", ""), fragment_text(readability.get("content", ""))))
    for block in data.get("blocks", []):
        if isinstance(block, dict):
            parts.append(block.get("text", ""))
    for table in data.get("tables", []):
        if isinstance(table, dict):
            parts.append(table.get("caption", ""))
            for row in table.get("rows", []):
                if isinstance(row, list):
                    parts.extend(row)
    for result in data.get("search_results", []):
        if isinstance(result, dict):
            parts.extend((result.get("title", ""), result.get("snippet", "")))
    for form in data.get("forms", []):
        if isinstance(form, dict):
            for field in form.get("fields", []):
                if isinstance(field, dict) and field.get("type") != "hidden":
                    parts.extend((field.get("label", ""), field.get("value", "")))
    for video in data.get("videos", []):
        if isinstance(video, dict):
            parts.append(video.get("title", ""))
    for media in data.get("media", []):
        if isinstance(media, dict):
            parts.append(media.get("alt", ""))
    if data.get("extractor") in {"media-reference", "video-metadata"}:
        for link in data.get("links", []):
            if isinstance(link, dict):
                parts.append(link.get("text", ""))
    return clean_text(" ".join(str(part) for part in parts if part))


def words(text):
    return re.findall(r"[\w\u0080-\uffff]{3,}", text.casefold(), re.UNICODE)


def contains_text(haystack, needle):
    return clean_text(needle).casefold() in clean_text(haystack).casefold()


def compare(reference, distill_dom, extraction):
    reference_text = reference.get("text", "")
    distill_dom_text = distill_dom.get("text", "") if distill_dom else ""
    distill_info = extraction_information(extraction)
    ref_words = set(words(reference_text))
    info_words = set(words(distill_info))
    shared = len(ref_words & info_words)
    ref_total = len(ref_words)
    info_total = len(info_words)
    missing_headings = [heading for heading in reference.get("headings", [])
                        if not contains_text(distill_info, heading)]
    return {
        "reference_unique_words": ref_total,
        "distill_information_unique_words": info_total,
        "shared_unique_words": shared,
        "reference_coverage": round(shared / ref_total, 4) if ref_total else None,
        "distill_information_fidelity": round(shared / info_total, 4) if info_total else None,
        "reference_headings": len(reference.get("headings", [])),
        "headings_missing_from_distill_information": missing_headings[:100],
        "reference_links": len(reference.get("links", [])),
        "distill_dom_links": len(distill_dom.get("links", [])) if distill_dom else 0,
        "reference_images": len(reference.get("images", [])),
        "distill_dom_images": len(distill_dom.get("images", [])) if distill_dom else 0,
        "reference_forms": len(reference.get("forms", [])),
        "distill_dom_forms": len(distill_dom.get("forms", [])) if distill_dom else 0,
        "distill_dom_text_sample": distill_dom_text[:500],
        "distill_information_sample": distill_info[:1000],
    }


# Distill reports a recognised bot challenge with its own status, so the
# harness can record "we were never given content" separately from "the
# capture failed". Neither is a content comparison, but only one is a defect.
CHALLENGE_EXIT = 4


def run_command(command, timeout):
    started = time.monotonic()
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        if result.returncode == 0:
            status = "ok"
        elif result.returncode == CHALLENGE_EXIT:
            status = "challenge"
        else:
            status = "error"
        return {
            "status": status,
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "seconds": round(time.monotonic() - started, 3),
            "command": command,
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "status": "timeout",
            "returncode": None,
            "stdout": exc.stdout or "",
            "stderr": exc.stderr or "",
            "seconds": round(time.monotonic() - started, 3),
            "command": command,
        }


def browser_command(browser, url, budget, profile, dump_path=None, screenshot_path=None):
    command = [
        browser, "--headless=new", "--disable-gpu", "--no-sandbox",
        "--disable-dev-shm-usage", "--hide-scrollbars", "--window-size=1280,1024",
        f"--user-data-dir={profile}", f"--virtual-time-budget={budget}",
        "--run-all-compositor-stages-before-draw",
    ]
    if dump_path:
        command.append("--dump-dom")
    if screenshot_path:
        command.append(f"--screenshot={screenshot_path}")
    command.append(url)
    return command


def write_text(path, value):
    if isinstance(value, bytes):
        value = value.decode("utf-8", errors="replace")
    path.write_text(value or "", encoding="utf-8")


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def site_report(site, browser, engine, output, timeout, budget, cap_ms, screenshots):
    output.mkdir(parents=True, exist_ok=True)
    url = site["url"]
    # Chromium may leave a profile lock briefly after a timeout (notably on
    # bot-protected pages). The capture result is still useful, so do not let
    # best-effort temporary-profile cleanup hide that diagnostic.
    with tempfile.TemporaryDirectory(prefix="distill-reference-", ignore_cleanup_errors=True) as profile:
        reference_result = run_command(
            browser_command(browser, url, budget, profile, dump_path=output / "reference.dom.html"),
            timeout,
        )
        write_text(output / "reference.dom.html", reference_result["stdout"])
        write_text(output / "reference.stderr.txt", reference_result["stderr"])
        if screenshots:
            reference_screenshot = run_command(
                browser_command(browser, url, budget, profile,
                                screenshot_path=output / "reference.png"),
                timeout,
            )
        else:
            reference_screenshot = None

    comparison_url = url
    if "Special:Random" in url:
        comparison_url = canonical_url(reference_result["stdout"], url) or url

    distill_dom_result = run_command(
        [engine, "--url", comparison_url, "--dump-dom", "--script-cap-ms", str(cap_ms)], timeout
    )
    write_text(output / "distill.dom.html", distill_dom_result["stdout"])
    write_text(output / "distill-dom.stderr.txt", distill_dom_result["stderr"])

    extract_result = run_command(
        [engine, "--url", comparison_url, "--extract", "--script-cap-ms", str(cap_ms)], timeout
    )
    write_text(output / "distill-extract.stderr.txt", extract_result["stderr"])
    extraction = None
    # A challenge run still emits a valid extraction of the challenge shell,
    # which is the evidence for the classification; parse it like any other.
    if extract_result["status"] in ("ok", "challenge"):
        try:
            extraction = json.loads(extract_result["stdout"])
            write_json(output / "distill.extract.json", extraction)
        except json.JSONDecodeError as exc:
            extract_result["status"] = "invalid-json"
            extract_result["parse_error"] = str(exc)
            write_text(output / "distill.extract.stdout.txt", extract_result["stdout"])
    if screenshots:
        render_result = run_command(
            [engine, "--url", comparison_url, "--render", str(output / "distill.png"),
             "--script-cap-ms", str(cap_ms)], timeout
        )
    else:
        render_result = None

    reference_summary = summarize_html(reference_result["stdout"])
    distill_summary = summarize_html(distill_dom_result["stdout"])
    comparison = compare(reference_summary, distill_summary, extraction)
    report = {
        "schema": 1,
        "site": site,
        "requested_url": url,
        "comparison_url": comparison_url,
        "reference": {
            "driver": browser,
            "capture": {key: value for key, value in reference_result.items() if key not in {"stdout", "stderr"}},
            "summary": reference_summary,
        },
        "distill": {
            "driver": engine,
            "dom_capture": {key: value for key, value in distill_dom_result.items() if key not in {"stdout", "stderr"}},
            "extract_capture": {key: value for key, value in extract_result.items() if key not in {"stdout", "stderr"}},
            "summary": distill_summary,
            "extracted_information": extraction_information(extraction),
        },
        "comparison": comparison,
    }
    if reference_screenshot:
        report["reference"]["screenshot_capture"] = {
            key: value for key, value in reference_screenshot.items() if key not in {"stdout", "stderr"}
        }
    if render_result:
        report["distill"]["render_capture"] = {
            key: value for key, value in render_result.items() if key not in {"stdout", "stderr"}
        }
        write_text(output / "distill-render.stderr.txt", render_result["stderr"])
    write_json(output / "comparison.json", report)
    write_report(output / "report.md", report)
    return report


def write_report(path, report):
    site = report["site"]
    ref = report["reference"]
    distill = report["distill"]
    comparison = report["comparison"]
    lines = [
        f"# {site.get('name', site['id'])}",
        "",
        "Diagnostic live-site comparison; not a CI verdict.",
        "",
        f"- Requested URL: `{report['requested_url']}`",
        f"- Comparison URL: `{report['comparison_url']}`",
        f"- Tier: {site.get('tier', '?')} / {site.get('class', 'unspecified')}",
        f"- Focus: {site.get('focus', '')}",
        f"- Chromium capture: `{ref['capture']['status']}`",
        f"- Distill DOM capture: `{distill['dom_capture']['status']}`",
        f"- Distill extraction: `{distill['extract_capture']['status']}`",
        "",
        "## Information comparison",
        "",
        f"- Reference unique words: {comparison['reference_unique_words']}",
        f"- Distill information unique words: {comparison['distill_information_unique_words']}",
        f"- Shared unique words: {comparison['shared_unique_words']}",
        f"- Reference coverage: {comparison['reference_coverage']}",
        f"- Distill information fidelity: {comparison['distill_information_fidelity']}",
        f"- Reference headings: {comparison['reference_headings']}",
        f"- Reference links/images/forms: {comparison['reference_links']}/"
        f"{comparison['reference_images']}/{comparison['reference_forms']}",
        f"- Distill DOM links/images/forms: {comparison['distill_dom_links']}/"
        f"{comparison['distill_dom_images']}/{comparison['distill_dom_forms']}",
        "",
        "## Reference headings missing from Distill information",
        "",
    ]
    missing = comparison["headings_missing_from_distill_information"]
    lines.extend(f"- {heading}" for heading in missing) if missing else lines.append("None recorded.")
    lines.extend([
        "",
        "## Artifacts",
        "",
        "- `reference.dom.html` and `reference.stderr.txt`",
        "- `distill.dom.html`, `distill-dom.stderr.txt`, and `distill.extract.json` when available",
        "- `comparison.json` contains the complete machine-readable report",
    ])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("urls", nargs="*", help="live HTTP(S) URLs; omit to use the manifest")
    parser.add_argument("--id", action="append", help="manifest site id; may be repeated")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--browser", help="Chromium/Chrome executable")
    parser.add_argument("--engine", help="Distill executable")
    parser.add_argument("--timeout", type=int, default=45, help="real-time limit per capture, seconds")
    parser.add_argument("--browser-budget", type=int, default=10000, help="Chromium virtual-time budget, ms")
    parser.add_argument("--script-cap-ms", type=int, default=5000, help="Distill settle cap, ms")
    parser.add_argument("--screenshots", action="store_true", help="also capture Chromium and Distill PNGs")
    parser.add_argument("--strict", action="store_true", help="exit 1 if any capture fails")
    args = parser.parse_args()

    if args.urls and args.id:
        parser.error("use positional URLs or --id, not both")
    if args.timeout <= 0 or args.browser_budget <= 0 or args.script_cap_ms <= 0:
        parser.error("timeouts and budgets must be positive")

    browser = executable(args.browser, "chromium") if args.browser else find_browser()
    engine = args.engine or str(ROOT / "build" / "distill")
    if not os.path.isfile(engine) or not os.access(engine, os.X_OK):
        fail(f"Distill executable not found or not executable: {engine}")

    manifest_sites = load_sites(args.manifest)
    if args.urls:
        for url in args.urls:
            if not valid_url(url):
                parser.error(f"custom URL must be HTTP(S): {url!r}")
        sites = [{"id": slug_for_url(url), "name": url, "tier": "custom", "class": "custom", "url": url,
                  "focus": "user-supplied live-site comparison"} for url in args.urls]
    elif args.id:
        wanted = set(args.id)
        sites = [site for site in manifest_sites if site["id"] in wanted]
        missing = wanted - {site["id"] for site in sites}
        if missing:
            fail(f"unknown manifest site id(s): {', '.join(sorted(missing))}")
    else:
        sites = manifest_sites

    args.out.mkdir(parents=True, exist_ok=True)
    failures = 0
    print(f"reference: {browser}")
    print(f"engine:    {engine}")
    print(f"output:    {args.out}")
    print("live captures are diagnostic; no live-site result is a CI gate")
    for site in sites:
        print(f"\n{site['id']}  {site['url']}")
        report = site_report(site, browser, engine, args.out / site["id"], args.timeout,
                             args.browser_budget, args.script_cap_ms, args.screenshots)
        statuses = (
            report["reference"]["capture"]["status"],
            report["distill"]["dom_capture"]["status"],
            report["distill"]["extract_capture"]["status"],
        )
        print(f"  chromium={statuses[0]} distill-dom={statuses[1]} distill-extract={statuses[2]}")
        # A challenge is a recorded access result, not a capture failure, so
        # it must not fail a --strict run the way a broken capture does.
        if any(status not in ("ok", "challenge") for status in statuses):
            failures += 1
        print(f"  report={args.out / site['id'] / 'report.md'}")
    if args.strict and failures:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
