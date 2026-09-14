#!/usr/bin/env python3
"""Corpus server for Phase 0.

Serves corpus/local/ plus two endpoints the settle policy cannot be tested
without:

    /slow?ms=N   responds after N milliseconds
    /hang        accepts the request and never responds

Threaded on purpose: /hang must not block anything else.

HTML is served without a charset parameter, so a document's own (possibly
lying) declaration stays authoritative -- see local/08-messy.html.
"""

import argparse
import json
import os
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "local")

# A hang is only "forever" from the page's point of view. Cap it so the
# server can still be shut down cleanly.
HANG_CAP_SECONDS = 120


class CorpusHandler(SimpleHTTPRequestHandler):
    quiet = False

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def do_GET(self):
        route = urlparse(self.path)

        if route.path == "/slow":
            qs = parse_qs(route.query)
            try:
                ms = max(0, min(30000, int(qs.get("ms", ["500"])[0])))
            except ValueError:
                ms = 500
            time.sleep(ms / 1000.0)
            self._json({"ok": "ok", "delayed_ms": ms})
            return

        if route.path == "/hang":
            # Never respond. The client's settle cap is what ends this.
            time.sleep(HANG_CAP_SECONDS)
            return

        if route.path == "/health":
            self._json({"ok": True, "root": ROOT})
            return

        super().do_GET()

    def _json(self, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def guess_type(self, path):
        ctype = super().guess_type(path)
        # Strip any charset so the document's own declaration governs.
        if ctype.startswith("text/html"):
            return "text/html"
        if path.endswith(".js"):
            return "text/javascript"
        return ctype

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        if not CorpusHandler.quiet:
            super().log_message(fmt, *args)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", type=int, default=8731)
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    CorpusHandler.quiet = args.quiet

    if not os.path.isdir(ROOT):
        sys.exit(f"corpus root not found: {ROOT}")

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), CorpusHandler)
    httpd.daemon_threads = True
    print(f"corpus on http://127.0.0.1:{args.port}/  (root: {ROOT})")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
