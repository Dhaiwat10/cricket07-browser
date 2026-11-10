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
            root, relative = BUILD, p[len("/build/"):]
        else:
            root, relative = HERE, p.lstrip("/")
        # URL paths use forward slashes. Reject Windows separators, drive/stream syntax and
        # parent components on every platform, then resolve links before checking containment.
        if "\\" in relative or ":" in relative or "\x00" in relative or ".." in relative.split("/"):
            raise ValueError("Path outside served directory")
        root = os.path.realpath(root)
        target = os.path.realpath(os.path.join(root, *relative.split("/")))
        candidates = [target]
        if os.path.isdir(target):
            # SimpleHTTPRequestHandler opens directory indexes without translate_path().
            for index in getattr(self, "index_pages", ("index.html", "index.htm")):
                index_path = os.path.join(target, index)
                if os.path.isfile(index_path):
                    candidates.append(os.path.realpath(index_path))
                    break
        for candidate in candidates:
            if os.path.normcase(os.path.commonpath([root, candidate])) != os.path.normcase(root):
                raise ValueError("Path outside served directory")
        return target + "/" if p.endswith("/") else target

    def send_head(self):
        # Both GET and HEAD use this path, including directory indexes and listings.
        try:
            return super().send_head()
        except ValueError:
            self.send_error(403, "Forbidden path")
            return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8007)
    a = ap.parse_args()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"http://localhost:{a.port}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
