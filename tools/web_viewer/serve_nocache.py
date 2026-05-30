"""HTTP server that emits Cache-Control: no-store. Drop-in for
`python -m http.server`, needed because Chromium caches ES modules
across hard reloads.

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
