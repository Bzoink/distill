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
from html import escape
import json
import os
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, parse_qsl

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "local")

# A hang is only "forever" from the page's point of view. Cap it so the
# server can still be shut down cleanly.
HANG_CAP_SECONDS = 120

# Fixed credentials for the login gate. These are corpus fixtures served from
# localhost and guard nothing; they are not secrets.
LOGIN_USER = "corpus"
LOGIN_PASSWORD = "fixture-password"
LOGIN_TOKEN = "form-token-1"
FORM_COOKIE = "login_form"
SESSION_COOKIE = "corpus_session"
SESSION_VALUE = "session-ok"


class CorpusHandler(SimpleHTTPRequestHandler):
    quiet = False

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def do_GET(self):
        route = urlparse(self.path)

        if route.path == "/network-contract":
            self._request_contract("")
            return

        if route.path == "/login":
            # The form carries a one-time token and sets the cookie that the
            # POST must send back with it.
            page = (
                "<!doctype html><meta charset=\"utf-8\"><title>Sign in</title>"
                "<form method=\"post\" action=\"/login\">"
                f"<input type=\"hidden\" name=\"token\" value=\"{LOGIN_TOKEN}\">"
                "<label for=\"user\">User</label><input id=\"user\" name=\"user\">"
                "<label for=\"password\">Password</label>"
                "<input id=\"password\" name=\"password\" type=\"password\">"
                "<button type=\"submit\">Sign in</button></form>"
                "<p id=\"content\">LOGIN-FORM</p>"
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Set-Cookie", f"{FORM_COOKIE}={LOGIN_TOKEN}; Path=/")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)
            return

        if route.path == "/logout":
            self.send_error(405, "Sign out requires POST")
            return

        if route.path == "/account":
            cookies = self.headers.get("Cookie", "")
            signed_in = f"{SESSION_COOKIE}={SESSION_VALUE}" in cookies
            body = (
                "<!doctype html><meta charset=\"utf-8\"><title>Account</title><main>"
                "<h1>Account</h1>"
                + (f"<p id=\"content\">SIGNED-IN-AS-{LOGIN_USER}</p>"
                   "<p>The session cookie sent with this request was issued to a "
                   "different process. Reaching this page at all is the assertion: "
                   "it means the login outlived the run that performed it.</p>"
                   "<form method=\"post\" action=\"/logout\">"
                   "<button type=\"submit\">Sign out</button></form>"
                   if signed_in else
                   "<p id=\"content\">SIGNED-OUT</p>"
                   "<p>No valid session cookie accompanied this request.</p>")
                + "</main>"
            ).encode("utf-8")
            self.send_response(200 if signed_in else 401)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return

        if route.path == "/network-redirect":
            self.send_response(302)
            self.send_header("Location", "/network-contract")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

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

        if route.path == "/forms-echo":
            self._form_echo("GET", route.query)
            return

        if route.path == "/redirect":
            target = parse_qs(route.query).get("to", ["/"])[0]
            if not target.startswith("/"):
                target = "/" + target
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        super().do_GET()

    def do_POST(self):
        route = urlparse(self.path)
        if route.path == "/login":
            try:
                length = max(0, int(self.headers.get("Content-Length", "0")))
            except ValueError:
                length = 0
            body = self.rfile.read(length).decode("utf-8", errors="replace")
            fields = parse_qs(body)
            user = (fields.get("user") or [""])[0]
            password = (fields.get("password") or [""])[0]
            token = (fields.get("token") or [""])[0]
            # The token ties the POST to the GET that issued the form, which is
            # the reason a login needs cookies to survive between two requests
            # at all: the pair only validates together.
            if user == LOGIN_USER and password == LOGIN_PASSWORD and token == LOGIN_TOKEN:
                self.send_response(303)
                self.send_header("Location", "/account")
                self.send_header("Set-Cookie",
                                 f"{SESSION_COOKIE}={SESSION_VALUE}; Path=/; HttpOnly")
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                self.send_response(403)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                page = b"<!doctype html><title>Denied</title><p id=\"content\">LOGIN-REJECTED</p>"
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
            return
        if route.path == "/logout":
            # Expiring the same path-scoped cookie makes the following /account
            # request prove that logout survived a separate reader process.
            self.send_response(303)
            self.send_header("Location", "/login")
            self.send_header("Set-Cookie",
                             f"{SESSION_COOKIE}=; Path=/; Max-Age=0; HttpOnly")
            self.send_header("Content-Length", "0")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        if route.path == "/network-contract":
            try:
                length = max(0, int(self.headers.get("Content-Length", "0")))
            except ValueError:
                length = 0
            body = self.rfile.read(length).decode("utf-8", errors="replace")
            self._request_contract(body)
            return
        if route.path == "/forms-echo":
            try:
                length = max(0, int(self.headers.get("Content-Length", "0")))
            except ValueError:
                length = 0
            body = self.rfile.read(length).decode("utf-8", errors="replace")
            self._form_echo("POST", body)
            return
        self.send_error(405)

    def _request_contract(self, body):
        payload = {
            "method": self.command,
            "body": body,
            "content_type": self.headers.get("Content-Type", ""),
            "x_contract": self.headers.get("X-Contract", ""),
            "accept": self.headers.get("Accept", ""),
            "language": self.headers.get("Accept-Language", ""),
            "referer": self.headers.get("Referer", ""),
            "cookie": self.headers.get("Cookie", ""),
        }
        response = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("X-Contract-Response", "response-ok")
        self.send_header("Set-Cookie", "server_cookie=server; Path=/")
        self.send_header("Content-Length", str(len(response)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(response)

    def _form_echo(self, method, encoded):
        """Show successful form controls exactly as the server received them.

        This is a manual test endpoint, not an API: an HTML response makes the
        GTK reader's normal navigation path prove both GET and POST submission.
        """
        pairs = parse_qsl(encoded, keep_blank_values=True)
        rows = "".join(
            f"<tr><td>{escape(name)}</td><td>{escape(value)}</td></tr>"
            for name, value in pairs
        ) or "<tr><td colspan=\"2\">No successful controls were submitted.</td></tr>"
        page = f"""<!doctype html><meta charset=\"utf-8\">
<title>Form submission received</title><main>
<h1>Form submission received</h1>
<p id=\"content\">{escape(method)} request received by the local form lab.</p>
<table><caption>Successful controls</caption><thead><tr><th>Name</th><th>Value</th></tr></thead>
<tbody>{rows}</tbody></table>
<h2>Encoded request</h2><pre>{escape(encoded or "(empty)")}</pre>
<p><a href=\"/forms-lab.html\">Return to the form lab</a></p>
</main>"""
        body = page.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

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
