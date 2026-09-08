"""Execution tests for FS7 capture timing, redaction, and concurrency.

All endpoints and values here come from the explicitly test-only mock server;
they are not Sony protocol fixtures.
"""
from __future__ import annotations

import concurrent.futures
import http.client
import json
import os
import pathlib
import subprocess
import sys
import threading
import time
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from fs7_proxy import Handler as ProxyHandler, ProxyState  # noqa: E402
from mock_fs7 import Handler as MockHandler  # noqa: E402


class SlowHandler(BaseHTTPRequestHandler):
    def log_message(self, _fmt, *_args):
        pass

    def do_GET(self):
        time.sleep(0.15)
        payload = b"test-only slow response"
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except OSError:
            pass


class QuietProxyHandler(ProxyHandler):
    def log_message(self, _fmt, *_args):
        pass


def start_server(server: ThreadingHTTPServer) -> threading.Thread:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return thread


def fetch(port: int, path: str, headers: dict[str, str] | None = None) -> tuple[int, bytes]:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        connection.request("GET", path, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


class CaptureToolExecutionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        artifact_root = pathlib.Path(os.environ.get("FS7_TEST_ARTIFACT_ROOT", ROOT / "test-artifacts"))
        artifact_root.mkdir(exist_ok=True)
        cls.artifact_root = artifact_root
        cls.artifact_prefix = "capture-tools-" + uuid.uuid4().hex

        cls.mock = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
        cls.mock.user = "admin"  # type: ignore[attr-defined]
        cls.mock.password = "upstream-password-secret"  # type: ignore[attr-defined]
        cls.mock.state = {  # type: ignore[attr-defined]
            "recording": False, "playback": False, "white": {"kelvin": 3200},
            "iris": 2.8, "gainDb": 0.0, "sqFps": 150,
        }
        cls.mock_thread = start_server(cls.mock)

        cls.log = artifact_root / (cls.artifact_prefix + "-capture.jsonl")
        cls.proxy = ThreadingHTTPServer(("127.0.0.1", 0), QuietProxyHandler)
        cls.proxy_port = cls.proxy.server_address[1]
        cls.state = ProxyState(
            "127.0.0.1", cls.mock.server_address[1], "admin", "upstream-password-secret",
            cls.log, f"127.0.0.1:{cls.proxy_port}", upstream_timeout=1.0,
        )
        cls.proxy.state = cls.state  # type: ignore[attr-defined]
        cls.session = artifact_root / (cls.artifact_prefix + "-session.json")
        cls.state.write_session_metadata(cls.session, "127.0.0.1", cls.proxy_port)
        cls.proxy_thread = start_server(cls.proxy)

        status, _ = fetch(cls.proxy_port, "/rm.html", {
            "Authorization": "Basic browser-auth-secret",
            "Proxy-Authorization": "Basic browser-proxy-auth-secret",
            "Cookie": "SESSIONID=browser-cookie-secret",
        })
        if status != 200:
            raise AssertionError(f"mock proxy setup request returned {status}")

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            statuses = list(pool.map(lambda _: fetch(cls.proxy_port, "/mock/status")[0], range(24)))
        if any(status != 200 for status in statuses):
            raise AssertionError(f"concurrent mock requests failed: {statuses}")

        cls.rows = [json.loads(line) for line in cls.log.read_text(encoding="utf-8").splitlines()]
        cls.raw_log = cls.log.read_text(encoding="utf-8")

        cls.slow = ThreadingHTTPServer(("127.0.0.1", 0), SlowHandler)
        cls.slow_thread = start_server(cls.slow)
        cls.failure_log = artifact_root / (cls.artifact_prefix + "-failure.jsonl")
        cls.failure_proxy = ThreadingHTTPServer(("127.0.0.1", 0), QuietProxyHandler)
        failure_port = cls.failure_proxy.server_address[1]
        cls.failure_proxy.state = ProxyState(  # type: ignore[attr-defined]
            "127.0.0.1", cls.slow.server_address[1], "admin", "failure-secret",
            cls.failure_log, f"127.0.0.1:{failure_port}", upstream_timeout=0.03,
        )
        cls.failure_thread = start_server(cls.failure_proxy)
        cls.failure_status, _ = fetch(failure_port, "/slow")
        cls.failure_row = json.loads(cls.failure_log.read_text(encoding="utf-8").strip())

    @classmethod
    def tearDownClass(cls):
        for server in (cls.failure_proxy, cls.slow, cls.proxy, cls.mock):
            server.shutdown()
            server.server_close()
        for thread in (cls.failure_thread, cls.slow_thread, cls.proxy_thread, cls.mock_thread):
            thread.join(timeout=2)
        for path in cls.artifact_root.glob(cls.artifact_prefix + "*"):
            path.unlink(missing_ok=True)

    def test_01_request_capture_ms_exists(self):
        self.assertTrue(all(isinstance(row.get("requestCaptureMs"), (int, float)) for row in self.rows))

    def test_02_response_capture_ms_exists(self):
        self.assertTrue(all(isinstance(row.get("responseCaptureMs"), (int, float)) for row in self.rows))

    def test_03_duration_ms_exists(self):
        self.assertTrue(all(isinstance(row.get("durationMs"), (int, float)) for row in self.rows))
        self.assertTrue(any(row["durationMs"] > 0 for row in self.rows))

    def test_04_response_not_before_request(self):
        self.assertTrue(all(row["responseCaptureMs"] >= row["requestCaptureMs"] for row in self.rows))

    def test_05_duration_matches_difference(self):
        for row in self.rows:
            difference = row["responseCaptureMs"] - row["requestCaptureMs"]
            self.assertAlmostEqual(row["durationMs"], difference, delta=0.002)

    def test_06_event_ids_are_sequential(self):
        self.assertEqual([row["eventId"] for row in self.rows], list(range(1, len(self.rows) + 1)))

    def test_07_timeout_event_retains_timing_and_error(self):
        self.assertEqual(self.failure_status, 502)
        self.assertIn("proxyError", self.failure_row)
        for key in ("requestTime", "requestCaptureMs", "responseCaptureMs", "durationMs"):
            self.assertIn(key, self.failure_row)
        self.assertGreaterEqual(self.failure_row["durationMs"], 20)

    def test_08_authorization_secrets_absent(self):
        self.assertNotIn("upstream-password-secret", self.raw_log)
        self.assertNotIn("browser-auth-secret", self.raw_log)
        self.assertNotIn("browser-proxy-auth-secret", self.raw_log)
        self.assertTrue(self.rows[0]["authorizationPresent"])
        self.assertTrue(self.rows[0]["browserAuthorizationPresent"])

    def test_09_request_cookie_value_absent(self):
        self.assertNotIn("browser-cookie-secret", self.raw_log)

    def test_10_set_cookie_value_absent(self):
        self.assertNotIn("do-not-log-this-secret", self.raw_log)

    def test_11_cookie_names_remain_available(self):
        self.assertEqual(self.rows[0]["requestCookieNames"], ["SESSIONID"])
        self.assertEqual(self.rows[0]["responseSetCookieNames"], ["MOCKSESSION"])

    def test_12_feedback_analyser_parses_new_timing(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/fs7_feedback_analyze.py"), str(self.log), "--all"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("/mock/status", result.stdout)

    def test_13_log_summariser_parses_new_timing(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/fs7_log_summarize.py"), str(self.log)],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Capture:", result.stdout)
        self.assertIn("Duration:", result.stdout)

    def test_14_concurrent_jsonl_is_not_corrupt(self):
        self.assertEqual(len(self.rows), 25)
        self.assertTrue(all(isinstance(row, dict) for row in self.rows))

    def test_15_session_metadata_is_secret_free(self):
        metadata = json.loads(self.session.read_text(encoding="utf-8"))
        raw = self.session.read_text(encoding="utf-8")
        self.assertEqual(metadata["marker"], "CAPTURE SESSION START")
        self.assertIn("monotonicReferenceNs", metadata)
        self.assertIn("utcOffset", metadata)
        self.assertNotIn("upstream-password-secret", raw)

    def test_16_legacy_capture_remains_parseable(self):
        legacy = self.artifact_root / (self.artifact_prefix + "-legacy.jsonl")
        legacy.write_text(json.dumps({
            "eventId": 1, "time": "2026-01-01T00:00:00+00:00", "captureMs": 12.5,
            "method": "GET", "path": "/test-only/status", "body": "",
            "responseStatus": 200, "responseContentType": "application/json",
            "responseBody": "{}",
        }) + "\n", encoding="utf-8")
        for tool in ("fs7_feedback_analyze.py", "fs7_log_summarize.py"):
            result = subprocess.run([sys.executable, str(ROOT / "tools" / tool), str(legacy)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
