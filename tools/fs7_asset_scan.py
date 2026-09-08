#!/usr/bin/env python3
"""Download the FS7 remote pages/assets and flag likely control endpoints.

Run while connected to the FS7 Wi-Fi network. Uses only the Python standard
library. This is complementary to fs7_proxy.py: the proxy captures requests at
runtime, while this scanner helps inspect Sony's shipped HTML/JavaScript.
"""
from __future__ import annotations

import argparse
import base64
import getpass
import http.client
import pathlib
import re
from urllib.parse import urljoin, urlsplit

ASSET_RE = re.compile(r'''(?:src|href)=["']([^"'#]+)["']''', re.I)
INTERESTING_RE = re.compile(
    r"XMLHttpRequest|fetch\s*\(|ajax|\.cgi\b|\.xml\b|\.json\b|/api/|POST|PUT|REC|review|assign|white|iris|gain|shutter|play",
    re.I,
)


def fetch(host: str, port: int, path: str, auth: str) -> tuple[int, dict[str, str], bytes]:
    c = http.client.HTTPConnection(host, port, timeout=5)
    c.request("GET", path, headers={
        "Authorization": auth,
        "Accept": "*/*",
        "Accept-Encoding": "identity",
        "Connection": "close",
    })
    r = c.getresponse()
    data = r.read()
    headers = {k.lower(): v for k, v in r.getheaders()}
    status = r.status
    c.close()
    return status, headers, data


def safe_name(path: str) -> str:
    q = path.split("?", 1)[0].strip("/") or "index"
    return re.sub(r"[^A-Za-z0-9._-]", "_", q.replace("/", "__"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--camera", default="192.168.1.1")
    ap.add_argument("--camera-port", type=int, default=80)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", help="FS7 Basic Authentication password (omit to prompt securely)")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("fs7-web-snapshot"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    password = args.password if args.password is not None else getpass.getpass("FS7 Basic Auth password: ")
    auth = "Basic " + base64.b64encode(f"{args.user}:{password}".encode()).decode()

    queue = ["/rm.html", "/rms.html", "/rmt.html"]
    seen: set[str] = set()
    report: list[str] = []

    while queue and len(seen) < 100:
        path = queue.pop(0)
        if path in seen:
            continue
        seen.add(path)
        try:
            status, headers, data = fetch(args.camera, args.camera_port, path, auth)
        except Exception as exc:
            print(f"ERR {path}: {exc}")
            continue
        ctype = headers.get("content-type", "")
        print(f"{status:3} {len(data):7} {path}  {ctype}")
        (args.out / safe_name(path)).write_bytes(data)
        if status >= 400:
            continue
        if not any(x in ctype.lower() for x in ("text", "javascript", "json", "xml")):
            continue
        text = data.decode("utf-8", errors="replace")
        for n, line in enumerate(text.splitlines(), 1):
            if INTERESTING_RE.search(line):
                report.append(f"{path}:{n}: {line[:1000]}")
        for ref in ASSET_RE.findall(text):
            u = urlsplit(urljoin(f"http://{args.camera}{path}", ref))
            if u.hostname == args.camera:
                child = u.path + (("?" + u.query) if u.query else "")
                if child not in seen and child not in queue:
                    queue.append(child)

    report_path = args.out / "interesting-lines.txt"
    report_path.write_text("\n".join(report), encoding="utf-8")
    print(f"\nSaved {len(seen)} resources to {args.out.resolve()}")
    print(f"Candidate control-code lines: {report_path.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
