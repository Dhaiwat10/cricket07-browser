#!/usr/bin/env python3
"""Local server for the browser build (this machine only).

Threads in WebAssembly need SharedArrayBuffer, which browsers only enable on cross-origin isolated
pages, so every response carries COOP/COEP headers.

  /            web/ (the page)
  /build/      build_web/www (recomp.js / .wasm from build_web.sh)

The page never downloads game files: the player picks their own game folder, which the browser copies
into its private storage.

Usage: python3 web/serve.py [--port 8007]
"""
import argparse
import http.server
import os
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(os.path.dirname(HERE), "build_web", "www")


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def translate_path(self, path):
        p = urllib.parse.unquote(urllib.parse.urlparse(path).path)
        if p.startswith("/build/"):
            return os.path.join(BUILD, p[len("/build/"):])
        return os.path.join(HERE, p.lstrip("/"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8007)
    a = ap.parse_args()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"http://localhost:{a.port}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
