"""HTTP server that emits Cache-Control: no-store. Drop-in for
`python -m http.server`, needed because Chromium caches ES modules
across hard reloads.

Also exposes a `/hive-proxy/<path>` endpoint that forwards to
`https://www.hiveworkshop.com/<path>` server-side. Hive's
/repository-files/ (user-uploaded models + textures) are public but send
no CORS headers, so a browser can't read them cross-origin — routing
them through this same-origin proxy sidesteps that. /casc-contents/ and
/assets/ do send CORS, so the app fetches those directly. No auth needed
for production www; the optional HIVE_USER/HIVE_PASS are forwarded if set.

Usage:
    cd build-web/web
    python ../../tools/web_viewer/serve_nocache.py 8080
"""

import argparse
import base64
import os
import sys
import urllib.error
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HIVE_PROD_ORIGIN = "https://www.hiveworkshop.com"
HIVE_PROXY_PREFIX = "/hive-proxy/"

# Populated from CLI flags / env vars in __main__.
HIVE_USER = ""
HIVE_PASS = ""


def _hive_auth_header() -> str:
    if not HIVE_USER:
        return ""
    raw = f"{HIVE_USER}:{HIVE_PASS}".encode("utf-8")
    return "Basic " + base64.b64encode(raw).decode("ascii")


class NoCacheHandler(SimpleHTTPRequestHandler):
    extensions_map = {
        **SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js":   "text/javascript",
        ".mjs":  "text/javascript",
    }

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def do_GET(self) -> None:
        if self.path.startswith(HIVE_PROXY_PREFIX):
            self._proxy_hive(self.path[len(HIVE_PROXY_PREFIX):])
            return
        super().do_GET()

    def do_HEAD(self) -> None:
        if self.path.startswith(HIVE_PROXY_PREFIX):
            # HEAD-on-proxy not worth the round-trip; report 405 cheaply.
            self.send_error(405, "HEAD not supported on /hive-proxy/")
            return
        super().do_HEAD()

    def _proxy_hive(self, rest: str) -> None:
        # Strip the optional cache-buster appended by the JS modules
        # (`?t=…`) so identical requests share a cache key upstream.
        url = f"{HIVE_PROD_ORIGIN}/{rest}"
        req = urllib.request.Request(url)
        auth = _hive_auth_header()
        if auth:
            req.add_header("Authorization", auth)
        # Hive's beta gates on a sensible UA; default urllib UA is rejected.
        req.add_header("User-Agent", "whiteout-js-viewer dev-proxy/1")
        try:
            with urllib.request.urlopen(req, timeout=30) as upstream:
                self.send_response(upstream.status)
                ctype = upstream.headers.get("Content-Type")
                if ctype:
                    self.send_header("Content-Type", ctype)
                clen = upstream.headers.get("Content-Length")
                if clen:
                    self.send_header("Content-Length", clen)
                # NoCacheHandler.end_headers adds no-store on top — fine,
                # the SW handles long-term caching client-side.
                self.end_headers()
                while True:
                    chunk = upstream.read(64 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        except urllib.error.HTTPError as e:
            self.send_response(e.code)
            self.end_headers()
            self.wfile.write(e.reason.encode("utf-8", "replace"))
        except urllib.error.URLError as e:
            self.send_response(502)
            self.end_headers()
            self.wfile.write(("upstream URLError: " + str(e.reason)).encode("utf-8"))


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("port", nargs="?", type=int, default=8080)
    p.add_argument("--hive-user", default=os.environ.get("HIVE_USER", ""))
    p.add_argument("--hive-pass", default=os.environ.get("HIVE_PASS", ""))
    return p.parse_args()


if __name__ == "__main__":
    args = _parse_args()
    HIVE_USER = args.hive_user
    HIVE_PASS = args.hive_pass
    with ThreadingHTTPServer(("", args.port), NoCacheHandler) as httpd:
        print(f"no-cache HTTP server on http://localhost:{args.port}/")
        auth_note = f"auth as '{HIVE_USER}'" if HIVE_USER else "no auth"
        print(f"  /hive-proxy/* -> {HIVE_PROD_ORIGIN}/* ({auth_note})")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print()
