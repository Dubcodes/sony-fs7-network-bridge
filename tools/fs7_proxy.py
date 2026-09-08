#!/usr/bin/env python3
"""Reverse proxy/logger for discovering Sony PXW-FS7 /rm.html requests.

Connect the laptop directly to the FS7 Wi-Fi network, then run e.g.:

  python fs7_proxy.py --camera 192.168.1.1 --user admin

Open http://127.0.0.1:8080/rm.html and operate Sony's own remote UI. Every
browser request is logged to fs7-requests.jsonl, with Basic Auth injected only
on the upstream camera connection. The password is prompted securely.

This tool is for protocol discovery. It does not modify the camera firmware.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import getpass
import http.client
import json
import pathlib
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOP_BY_HOP = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
    "te", "trailers", "transfer-encoding", "upgrade", "content-length",
    "host", "authorization", "accept-encoding",
}
MAX_REQUEST_BYTES = 1 * 1024 * 1024
MAX_RESPONSE_BYTES = 8 * 1024 * 1024
MAX_LOG_BODY_CHARS = 65536
PROXY_VERSION = "0.2.2.1"


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).astimezone().isoformat(timespec="milliseconds")


def cookie_names(values: list[str]) -> list[str]:
    """Return cookie names only; values must never enter capture logs."""
    names: set[str] = set()
    for value in values:
        for item in value.split(";"):
            name, sep, _ = item.strip().partition("=")
            if sep and name:
                names.add(name)
    return sorted(names)


def set_cookie_names(headers: list[tuple[str, str]]) -> list[str]:
    names: set[str] = set()
    for key, value in headers:
        if key.lower() != "set-cookie":
            continue
        name, sep, _ = value.partition("=")
        if sep and name.strip():
            names.add(name.strip())
    return sorted(names)


class ProxyState:
    def __init__(self, camera: str, camera_port: int, user: str, password: str, log: pathlib.Path,
                 public_host: str, upstream_timeout: float = 5.0):
        self.camera = camera
        self.camera_port = camera_port
        self.user = user
        self.password = password
        self.log = log
        self.public_host = public_host
        self.upstream_timeout = upstream_timeout
        self.start_wall = dt.datetime.now(dt.timezone.utc).astimezone()
        # perf_counter_ns is monotonic and high-resolution on Windows; ordinary
        # time.monotonic_ns may only advance at the coarse system tick there.
        self.start_mono_ns = time.perf_counter_ns()
        self._counter = 0
        self._lock = threading.Lock()
        token = base64.b64encode(f"{user}:{password}".encode()).decode()
        self.auth = f"Basic {token}"

    def capture_ms(self) -> float:
        return round((time.perf_counter_ns() - self.start_mono_ns) / 1_000_000, 3)

    def write_log(self, event: dict) -> None:
        """Complete timing and atomically append one transaction event.

        `captureMs` is retained as a compatibility alias for the completion/log
        timestamp. New analysis should use requestCaptureMs and responseCaptureMs.
        """
        response_ms = self.capture_ms()
        event.setdefault("requestTime", now_iso())
        event.setdefault("time", event["requestTime"])
        event.setdefault("requestCaptureMs", response_ms)
        event["responseCaptureMs"] = response_ms
        event["durationMs"] = round(response_ms - float(event["requestCaptureMs"]), 3)
        event["captureMs"] = response_ms
        # Assign and write under one lock so JSONL order always matches eventId.
        with self._lock:
            self._counter += 1
            event["eventId"] = self._counter
            with self.log.open("a", encoding="utf-8") as f:
                f.write(json.dumps(event, ensure_ascii=False) + "\n")
                f.flush()

    def session_metadata(self, listen: str, port: int) -> dict:
        offset = self.start_wall.strftime("%z")
        if len(offset) == 5:
            offset = offset[:3] + ":" + offset[3:]
        return {
            "marker": "CAPTURE SESSION START",
            "schemaVersion": 1,
            "tool": "FS7 Capture Tools",
            "proxyVersion": PROXY_VERSION,
            "captureStartTime": self.start_wall.isoformat(timespec="milliseconds"),
            "localTimezone": self.start_wall.tzname() or "",
            "utcOffset": offset,
            "monotonicReferenceNs": self.start_mono_ns,
            "monotonicClock": "Python time.perf_counter_ns (monotonic, high-resolution)",
            "requestCaptureMsReference": "0.000 ms at proxy session start (process-local monotonic clock)",
            "cameraHost": self.camera,
            "cameraPort": self.camera_port,
            "proxyListenAddress": listen,
            "proxyListenPort": port,
            "authenticationMode": "Basic Auth injected upstream",
            "authorizationPresent": bool(self.auth),
            "logFile": str(self.log.resolve()),
        }

    def write_session_metadata(self, path: pathlib.Path, listen: str, port: int) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8", newline="\n") as f:
            json.dump(self.session_metadata(listen, port), f, ensure_ascii=False, indent=2)
            f.write("\n")
            f.flush()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = f"FS7DiscoveryProxy/{PROXY_VERSION}"

    def log_message(self, fmt: str, *args) -> None:
        sys.stdout.write("[proxy] " + (fmt % args) + "\n")

    @property
    def state(self) -> ProxyState:
        return self.server.state  # type: ignore[attr-defined]

    def do_GET(self): self._proxy()
    def do_POST(self): self._proxy()
    def do_PUT(self): self._proxy()
    def do_PATCH(self): self._proxy()
    def do_DELETE(self): self._proxy()
    def do_HEAD(self): self._proxy()
    def do_OPTIONS(self): self._proxy()

    def _base_event(self, request_time: str, request_capture_ms: float) -> dict:
        return {
            "requestTime": request_time,
            # Backward-compatible wall-clock field; requestTime is authoritative.
            "time": request_time,
            "requestCaptureMs": request_capture_ms,
            "method": self.command,
            "path": self.path,
            "contentType": self.headers.get("Content-Type", ""),
            "body": "",
            # The upstream request always has the injected Basic credential;
            # record presence only, never its value.
            "authorizationPresent": True,
            "browserAuthorizationPresent": bool(self.headers.get("Authorization")),
            "requestCookieNames": cookie_names(self.headers.get_all("Cookie", [])),
            "requestHeaders": {
                k: v for k, v in self.headers.items()
                if k.lower() not in {"authorization", "proxy-authorization", "cookie"}
            },
        }

    def _proxy(self) -> None:
        request_time = now_iso()
        request_capture_ms = self.state.capture_ms()
        event = self._base_event(request_time, request_capture_ms)
        try:
            length = int(self.headers.get("Content-Length", "0") or "0")
        except ValueError:
            event["proxyError"] = "Invalid browser Content-Length"
            self.state.write_log(event)
            self.send_error(400, "Invalid Content-Length")
            return
        if length < 0 or length > MAX_REQUEST_BYTES:
            event["proxyError"] = "Browser request body exceeds capture limit"
            self.state.write_log(event)
            self.send_error(413, "Request body too large")
            return
        body = self.rfile.read(length) if length else b""
        event["body"] = body.decode("utf-8", errors="replace")

        req_headers = {}
        for key, value in self.headers.items():
            if key.lower() in HOP_BY_HOP:
                continue
            req_headers[key] = value
        req_headers["Authorization"] = self.state.auth
        req_headers["Host"] = self.state.camera
        req_headers["Accept-Encoding"] = "identity"
        req_headers["Connection"] = "close"
        if body:
            req_headers["Content-Length"] = str(len(body))

        conn: http.client.HTTPConnection | None = None
        logged = False
        try:
            conn = http.client.HTTPConnection(
                self.state.camera, self.state.camera_port, timeout=self.state.upstream_timeout
            )
            conn.request(self.command, self.path, body=body, headers=req_headers)
            resp = conn.getresponse()
            payload = resp.read(MAX_RESPONSE_BYTES + 1)
            if len(payload) > MAX_RESPONSE_BYTES:
                raise RuntimeError(f"upstream response exceeded {MAX_RESPONSE_BYTES} bytes")
            response_headers = resp.getheaders()
            event["responseStatus"] = resp.status
            event["responseContentType"] = resp.getheader("Content-Type", "")
            event["responseHeaders"] = {
                k: v for k, v in response_headers
                if k.lower() not in {"set-cookie", "authorization"}
            }
            event["responseSetCookieNames"] = set_cookie_names(response_headers)

            ctype = (resp.getheader("Content-Type") or "").lower()
            if any(x in ctype for x in ("text/", "javascript", "json", "xml", "x-www-form-urlencoded")):
                text = payload.decode("utf-8", errors="replace")
                event["responseBody"] = text[:MAX_LOG_BODY_CHARS]
                event["responseBodyTruncated"] = len(text) > MAX_LOG_BODY_CHARS
                # Keep absolute references inside the proxy when possible.
                text = text.replace(f"http://{self.state.camera}", f"http://{self.state.public_host}")
                text = text.replace(f"//{self.state.camera}/", f"//{self.state.public_host}/")
                payload = text.encode("utf-8")

            self.state.write_log(event)
            logged = True
            self.send_response(resp.status, resp.reason)
            for key, value in response_headers:
                lk = key.lower()
                if lk in HOP_BY_HOP or lk == "content-encoding":
                    continue
                if lk == "location":
                    value = value.replace(f"http://{self.state.camera}", f"http://{self.state.public_host}")
                self.send_header(key, value)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(payload)
        except Exception as exc:  # discovery utility: surface details
            if logged:
                self.log_message("downstream response failed after capture: %r", exc)
                return
            event["proxyError"] = repr(exc)
            self.state.write_log(event)
            logged = True
            payload = f"FS7 proxy error: {exc}\n".encode()
            self.send_response(502)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(payload)
        finally:
            if conn is not None:
                conn.close()


def default_session_path(log: pathlib.Path) -> pathlib.Path:
    stem = log.stem
    if stem.endswith("-requests"):
        stem = stem[:-len("-requests")]
    return log.with_name(stem + "-session.json")


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--camera", default="192.168.1.1", help="FS7 Wi-Fi IP/host")
    p.add_argument("--camera-port", type=int, default=80, help="FS7 HTTP port (normally 80)")
    p.add_argument("--user", default="admin", help="FS7 Basic Authentication user")
    p.add_argument("--password", help="FS7 Basic Authentication password (omit to prompt securely)")
    p.add_argument("--listen", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--log", type=pathlib.Path, default=pathlib.Path("fs7-requests.jsonl"))
    p.add_argument("--session-metadata", type=pathlib.Path,
                   help="session metadata path (default: alongside the JSONL)")
    p.add_argument("--no-session-metadata", action="store_true",
                   help="do not write the one-time capture session metadata file")
    args = p.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)

    password = args.password if args.password is not None else getpass.getpass("FS7 Basic Auth password: ")
    public = f"{args.listen}:{args.port}"
    server = ThreadingHTTPServer((args.listen, args.port), Handler)
    server.state = ProxyState(args.camera, args.camera_port, args.user, password, args.log, public)  # type: ignore[attr-defined]
    session_path = args.session_metadata or default_session_path(args.log)
    if not args.no_session_metadata:
        server.state.write_session_metadata(session_path, args.listen, args.port)  # type: ignore[attr-defined]
    print(f"FS7 discovery proxy -> http://{args.camera}:{args.camera_port}")
    print(f"Open: http://{public}/rm.html")
    print(f"Logging requests to: {args.log.resolve()}")
    print(f"CAPTURE SESSION START: {server.state.start_wall.isoformat(timespec='milliseconds')}")  # type: ignore[attr-defined]
    print("requestCaptureMs zero/reference established from the process monotonic clock")
    if not args.no_session_metadata:
        print(f"Session metadata: {session_path.resolve()}")
    print("Operate REC, playback, Assign buttons, AWB etc. in Sony's page, then Ctrl+C.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
