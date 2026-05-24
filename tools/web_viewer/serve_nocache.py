"""Minimal HTTP server that sends Cache-Control: no-store on every response.

Chromium aggressively caches ES modules across page reloads — even with
Ctrl+Shift+R, even with `?t=...` query-string busters. The only reliable
fix during bring-up is a server that explicitly tells the browser to never
cache. Drop-in replacement for `python -m http.server 8080`.

Usage:
    cd build-web/web
    python ../../tools/web_viewer/serve_nocache.py 8080
"""

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import sys


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


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    with ThreadingHTTPServer(("", port), NoCacheHandler) as httpd:
        print(f"no-cache HTTP server on http://localhost:{port}/")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print()
