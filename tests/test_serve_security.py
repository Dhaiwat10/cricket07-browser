"""HTTP boundary regressions; uses temporary fixtures, never real private files."""
import http.client
import http.server
import importlib.util
from pathlib import Path
import tempfile
import threading
import unittest


spec = importlib.util.spec_from_file_location("serve", Path(__file__).resolve().parents[1] / "web/serve.py")
serve = importlib.util.module_from_spec(spec)
spec.loader.exec_module(serve)


class FileBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.web = root / "web"
        self.build = root / "build_web" / "www"
        self.web.mkdir()
        self.build.mkdir(parents=True)
        (root / "private.txt").write_text("outside-secret", encoding="utf-8")
        (self.web / "index.html").write_text("game-page", encoding="utf-8")
        (self.build / "recomp.js").write_text("game-build", encoding="utf-8")
        (self.build.parent / "private.txt").write_text("outside-secret", encoding="utf-8")
        self.old_roots = serve.HERE, serve.BUILD
        serve.HERE, serve.BUILD = str(self.web), str(self.build)
        self.addCleanup(self.restore_roots)

        class QuietHandler(serve.Handler):
            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), QuietHandler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)

    def restore_roots(self):
        serve.HERE, serve.BUILD = self.old_roots

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def request(self, path, method="GET"):
        conn = http.client.HTTPConnection(*self.server.server_address)
        try:
            conn.request(method, path)
            response = conn.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            conn.close()

    def test_normal_files_and_isolation_headers(self):
        for path, content in [("/", b"game-page"), ("/build/recomp.js?v=1", b"game-build")]:
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertEqual(body, content)
                self.assertEqual(headers["Cross-Origin-Opener-Policy"], "same-origin")
                self.assertEqual(headers["Cross-Origin-Embedder-Policy"], "require-corp")

    def test_traversal_cannot_read_outside_either_root(self):
        for path in ["/../private.txt", "/%2e%2e/private.txt", "/build/../private.txt",
                     "/build/%2e%2e/private.txt", "/build//../private.txt",
                     "/..%5cprivate.txt", "/build/..%5cprivate.txt",
                     "/build/%2f../private.txt", "/build/C:%5cprivate.txt"]:
            for method in ["GET", "HEAD"]:
                with self.subTest(path=path, method=method):
                    status, _, body = self.request(path, method)
                    self.assertIn(status, (400, 403, 404))
                    self.assertNotIn(b"outside-secret", body)

    def test_symlink_cannot_escape_root(self):
        try:
            (self.web / "linked.txt").symlink_to(self.web.parent / "private.txt")
        except OSError as error:
            self.skipTest(f"Symlinks unavailable: {error}")
        status, _, body = self.request("/linked.txt")
        self.assertIn(status, (400, 403, 404))
        self.assertNotIn(b"outside-secret", body)

    def test_directory_index_symlink_cannot_escape_root(self):
        directory = self.web / "nested"
        directory.mkdir()
        try:
            (directory / "index.html").symlink_to(self.web.parent / "private.txt")
        except OSError as error:
            self.skipTest(f"Symlinks unavailable: {error}")
        status, _, body = self.request("/nested/")
        self.assertIn(status, (400, 403, 404))
        self.assertNotIn(b"outside-secret", body)

    def test_file_with_trailing_slash_is_not_served(self):
        status, _, _ = self.request("/build/recomp.js/")
        self.assertEqual(status, 404)


if __name__ == "__main__":
    unittest.main()
