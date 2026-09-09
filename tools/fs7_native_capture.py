#!/usr/bin/env python3
"""Lossless laptop-side capture of the Sony FS7 native WebUI.

The capture directory is deliberately ignored by Git: it contains Sony assets and
raw camera traffic.  WebSocket payloads are never shortened.  The decoded form is
supplemental; base64, hexadecimal, and byte-array fields preserve the authority.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import mimetypes
import pathlib
import re
import struct
import time
from datetime import datetime, timezone
from typing import Any
from urllib.error import URLError
from urllib.parse import unquote, urljoin, urlparse
from urllib.request import ProxyHandler, Request, build_opener

from playwright.sync_api import Error as PlaywrightError
from playwright.sync_api import Page, sync_playwright


DEFAULT_URL = "http://192.168.0.65:8081/rmt.html"
SECRET_HEADERS = {"authorization", "proxy-authorization", "cookie", "set-cookie"}


def utc_now() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def json_safe(value: Any) -> Any:
    if isinstance(value, bytes):
        return {"binaryBase64": base64.b64encode(value).decode("ascii"), "length": len(value)}
    if isinstance(value, float) and not math.isfinite(value):
        return repr(value)
    if isinstance(value, dict):
        return {str(k): json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_safe(v) for v in value]
    return value


class MsgpackDecoder:
    """Small dependency-free decoder covering the complete MessagePack type set."""

    def __init__(self, payload: bytes):
        self.data = payload
        self.pos = 0

    def take(self, count: int) -> bytes:
        if count < 0 or self.pos + count > len(self.data):
            raise ValueError("truncated MessagePack value")
        out = self.data[self.pos:self.pos + count]
        self.pos += count
        return out

    def integer(self, count: int, signed: bool = False) -> int:
        return int.from_bytes(self.take(count), "big", signed=signed)

    def unpack(self) -> Any:
        code = self.integer(1)
        if code <= 0x7F:
            return code
        if code >= 0xE0:
            return code - 256
        if 0x80 <= code <= 0x8F:
            return self.map(code & 0x0F)
        if 0x90 <= code <= 0x9F:
            return [self.unpack() for _ in range(code & 0x0F)]
        if 0xA0 <= code <= 0xBF:
            return self.take(code & 0x1F).decode("utf-8", "replace")
        if code == 0xC0:
            return None
        if code == 0xC2:
            return False
        if code == 0xC3:
            return True
        if code in (0xC4, 0xC5, 0xC6):
            size = self.integer({0xC4: 1, 0xC5: 2, 0xC6: 4}[code])
            return self.take(size)
        if code in (0xCA, 0xCB):
            return struct.unpack(">f" if code == 0xCA else ">d", self.take(4 if code == 0xCA else 8))[0]
        if 0xCC <= code <= 0xCF:
            return self.integer({0xCC: 1, 0xCD: 2, 0xCE: 4, 0xCF: 8}[code])
        if 0xD0 <= code <= 0xD3:
            return self.integer({0xD0: 1, 0xD1: 2, 0xD2: 4, 0xD3: 8}[code], signed=True)
        if 0xD4 <= code <= 0xD8:
            size = {0xD4: 1, 0xD5: 2, 0xD6: 4, 0xD7: 8, 0xD8: 16}[code]
            return self.extension(size)
        if code in (0xD9, 0xDA, 0xDB):
            size = self.integer({0xD9: 1, 0xDA: 2, 0xDB: 4}[code])
            return self.take(size).decode("utf-8", "replace")
        if code in (0xDC, 0xDD):
            count = self.integer(2 if code == 0xDC else 4)
            return [self.unpack() for _ in range(count)]
        if code in (0xDE, 0xDF):
            return self.map(self.integer(2 if code == 0xDE else 4))
        if code in (0xC7, 0xC8, 0xC9):
            return self.extension(self.integer({0xC7: 1, 0xC8: 2, 0xC9: 4}[code]))
        raise ValueError(f"unsupported/reserved MessagePack type 0x{code:02x}")

    def map(self, count: int) -> dict[Any, Any]:
        result: dict[Any, Any] = {}
        for _ in range(count):
            key = self.unpack()
            value = self.unpack()
            try:
                result[key] = value
            except TypeError:
                result[json.dumps(json_safe(key), sort_keys=True)] = value
        return result

    def extension(self, count: int) -> dict[str, Any]:
        kind = self.integer(1, signed=True)
        raw = self.take(count)
        return {"extensionType": kind, "dataBase64": base64.b64encode(raw).decode("ascii")}

    def decode(self) -> Any:
        result = self.unpack()
        if self.pos != len(self.data):
            raise ValueError(f"{len(self.data) - self.pos} trailing byte(s)")
        return result

    def decode_stream(self) -> list[Any]:
        """Decode every concatenated MessagePack object in one WS payload."""
        result: list[Any] = []
        while self.pos < len(self.data):
            result.append(self.unpack())
        return result


def envelope_fields(decoded: Any) -> dict[str, Any]:
    fields: dict[str, Any] = {
        "envelopeType": None, "requestId": None, "responseId": None,
        "notification": None, "method": None, "params": None,
        "result": None, "error": None,
    }
    if not isinstance(decoded, list) or not decoded:
        return fields
    kind = decoded[0]
    fields["envelopeType"] = {0: "request", 1: "response", 2: "notification"}.get(kind, kind)
    if kind == 0 and len(decoded) == 4:
        fields.update(requestId=decoded[1], method=decoded[2], params=decoded[3])
    elif kind == 1 and len(decoded) == 4:
        fields.update(responseId=decoded[1], error=decoded[2], result=decoded[3])
    elif kind == 2 and len(decoded) == 3:
        fields.update(notification=decoded[1], params=decoded[2])
    return fields


def redact_headers(headers: dict[str, Any]) -> dict[str, Any]:
    clean: dict[str, Any] = {}
    for key, value in headers.items():
        clean[key] = "<redacted-present>" if key.lower() in SECRET_HEADERS else value
    return clean


def safe_resource_path(url: str, content_type: str, request_id: str) -> pathlib.Path:
    parsed = urlparse(url)
    raw = unquote(parsed.path).lstrip("/") or "index.html"
    parts = [re.sub(r"[^A-Za-z0-9._-]+", "_", p)[:120] or "_" for p in raw.split("/")]
    candidate = pathlib.Path(*parts)
    if parsed.query:
        candidate = candidate.with_name(candidate.name + "__" + hashlib.sha256(parsed.query.encode()).hexdigest()[:12])
    if not candidate.suffix:
        ext = {"text/html": ".html", "text/css": ".css", "application/javascript": ".js",
               "text/javascript": ".js", "application/json": ".json"}.get(content_type.split(";", 1)[0], ".bin")
        candidate = candidate.with_suffix(ext)
    return pathlib.Path(parsed.netloc.replace(":", "_")) / candidate.with_name(f"{candidate.stem}__{request_id}{candidate.suffix}")


def precache_live_resources(root: pathlib.Path, start_url: str) -> dict[str, tuple[bytes, str]]:
    """Fetch the live page and its declared resources sequentially.

    The WT32 proxy cannot reliably serve Chromium's normal parallel fan-out of
    100+ legacy Sony assets.  Sequential prefetch keeps the live bytes authoritative,
    then Chromium consumes those exact bytes while its /linear socket remains live.
    """
    opener = build_opener(ProxyHandler({}))
    origin = urlparse(start_url)
    queue = [start_url]
    seen: set[str] = set()
    cache: dict[str, tuple[bytes, str]] = {}
    manifest: list[dict[str, Any]] = []
    attribute_re = re.compile(rb"(?:src|href)\s*=\s*['\"]([^'\"#]+)", re.I)
    css_url_re = re.compile(rb"url\(\s*['\"]?([^)'\"#]+)", re.I)
    while queue:
        url = queue.pop(0)
        if url in seen:
            continue
        seen.add(url)
        parsed = urlparse(url)
        if parsed.scheme not in ("http", "https") or parsed.netloc != origin.netloc:
            continue
        last_error = ""
        for attempt in range(1, 5):
            try:
                with opener.open(Request(url, headers={"User-Agent": "FS7-Native-Capture/1"}), timeout=20) as response:
                    body = response.read()
                    content_type = response.headers.get_content_type()
                    status = response.status
                break
            except (OSError, URLError) as exc:
                last_error = str(exc)
                time.sleep(0.15 * attempt)
        else:
            manifest.append({"url": url, "error": last_error})
            continue
        cache[url] = (body, content_type)
        rel = pathlib.Path("precache") / safe_resource_path(url, content_type, "live")
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
        manifest.append({"url": url, "status": status, "contentType": content_type,
                         "exactBodyLength": len(body), "sha256": hashlib.sha256(body).hexdigest(),
                         "bodyFile": str(rel)})
        if content_type in ("text/html", "text/css", "application/javascript", "text/javascript"):
            matches = list(attribute_re.findall(body))
            if content_type == "text/css":
                matches += list(css_url_re.findall(body))
            for raw_ref in matches:
                ref = raw_ref.decode("utf-8", "replace").strip()
                candidate = urljoin(url, ref)
                if candidate not in seen:
                    queue.append(candidate)
        if content_type in ("application/javascript", "text/javascript") or parsed.path.lower().endswith(".js"):
            mirror = root / "scripts" / safe_resource_path(url, content_type, "live")
            mirror.parent.mkdir(parents=True, exist_ok=True)
            mirror.write_bytes(body)
        elif content_type == "text/css" or parsed.path.lower().endswith(".css"):
            mirror = root / "styles" / safe_resource_path(url, content_type, "live")
            mirror.parent.mkdir(parents=True, exist_ok=True)
            mirror.write_bytes(body)
    (root / "precache.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return cache


class Capture:
    def __init__(self, root: pathlib.Path, url: str):
        self.root = root
        self.url = url
        self.started_ns = time.perf_counter_ns()
        self.action_id: str | None = None
        self.request_meta: dict[str, dict[str, Any]] = {}
        self.pending_errors: list[str] = []
        for name in ("scripts", "styles", "http-bodies", "screenshots", "dom"):
            (root / name).mkdir(parents=True, exist_ok=True)

    def stamp(self, cdp_timestamp: float | None = None) -> dict[str, Any]:
        return {"wallTime": utc_now(), "monotonicNs": time.perf_counter_ns(),
                "elapsedMs": (time.perf_counter_ns() - self.started_ns) / 1_000_000,
                "cdpTimestampSeconds": cdp_timestamp}

    def append(self, name: str, row: dict[str, Any]) -> None:
        with (self.root / name).open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(json_safe(row), ensure_ascii=False, separators=(",", ":")) + "\n")

    def websocket(self, direction: str, event: dict[str, Any]) -> None:
        frame = event["response"]
        opcode = int(frame.get("opcode", -1))
        encoded = frame.get("payloadData", "")
        if opcode == 2:
            try:
                raw = base64.b64decode(encoded, validate=True)
            except Exception as exc:
                raw = encoded.encode("utf-8")
                decode_transport_error = str(exc)
            else:
                decode_transport_error = None
        else:
            raw = encoded.encode("utf-8")
            decode_transport_error = None
        decoded = None
        decoded_items: list[Any] = []
        decode_error = decode_transport_error
        if opcode == 2 and decode_error is None:
            try:
                decoded_items = MsgpackDecoder(raw).decode_stream()
                decoded = decoded_items[0] if len(decoded_items) == 1 else decoded_items
            except Exception as exc:
                decode_error = str(exc)
        row = {
            **self.stamp(event.get("timestamp")), "actionId": self.action_id,
            "direction": direction, "opcode": opcode,
            "type": {1: "text", 2: "binary", 8: "close", 9: "ping", 10: "pong"}.get(opcode, "other"),
            "exactPayloadLength": len(raw), "rawBytes": list(raw),
            "rawBase64": base64.b64encode(raw).decode("ascii"), "rawHex": raw.hex(),
            "rawText": raw.decode("utf-8", "replace") if opcode == 1 else None,
            "decodedMessagePack": decoded, "decodeError": decode_error,
            "envelopes": [envelope_fields(item) for item in decoded_items],
            **envelope_fields(decoded_items[0] if len(decoded_items) == 1 else None),
        }
        self.append("websocket.jsonl", row)

    def request(self, event: dict[str, Any]) -> None:
        request = event["request"]
        request_id = event["requestId"]
        post = request.get("postData")
        post_raw = post.encode("utf-8") if post is not None else b""
        row = {
            **self.stamp(event.get("timestamp")), "event": "request", "requestId": request_id,
            "actionId": self.action_id, "resourceType": event.get("type"), "url": request["url"],
            "method": request["method"], "headers": redact_headers(request.get("headers", {})),
            "postDataLength": len(post_raw), "postDataBase64": base64.b64encode(post_raw).decode("ascii"),
            "postDataHex": post_raw.hex(),
        }
        self.request_meta[request_id] = {"url": request["url"], "resourceType": event.get("type")}
        self.append("http.jsonl", row)

    def response(self, event: dict[str, Any]) -> None:
        response = event["response"]
        request_id = event["requestId"]
        self.request_meta.setdefault(request_id, {}).update(
            url=response["url"], mimeType=response.get("mimeType", ""), status=response.get("status"),
            headers=redact_headers(response.get("headers", {})))
        self.append("http.jsonl", {
            **self.stamp(event.get("timestamp")), "event": "response", "requestId": request_id,
            "actionId": self.action_id, "url": response["url"], "status": response.get("status"),
            "statusText": response.get("statusText"), "mimeType": response.get("mimeType"),
            "headers": redact_headers(response.get("headers", {})), "fromDiskCache": response.get("fromDiskCache"),
            "fromServiceWorker": response.get("fromServiceWorker"),
        })

    def response_body(self, cdp: Any, event: dict[str, Any]) -> None:
        request_id = event["requestId"]
        meta = self.request_meta.get(request_id)
        if not meta or meta.get("status") in (204, 304):
            return
        try:
            body_result = cdp.send("Network.getResponseBody", {"requestId": request_id})
        except PlaywrightError as exc:
            self.append("http.jsonl", {**self.stamp(event.get("timestamp")), "event": "body-error",
                                      "requestId": request_id, "error": str(exc)})
            return
        raw = (base64.b64decode(body_result["body"]) if body_result.get("base64Encoded")
               else body_result["body"].encode("utf-8"))
        rel = safe_resource_path(meta["url"], meta.get("mimeType", ""), request_id)
        body_path = self.root / "http-bodies" / rel
        body_path.parent.mkdir(parents=True, exist_ok=True)
        body_path.write_bytes(raw)
        mime = meta.get("mimeType", "").split(";", 1)[0]
        if mime in ("application/javascript", "text/javascript") or urlparse(meta["url"]).path.lower().endswith(".js"):
            mirror = self.root / "scripts" / rel
            mirror.parent.mkdir(parents=True, exist_ok=True)
            mirror.write_bytes(raw)
        elif mime == "text/css" or urlparse(meta["url"]).path.lower().endswith(".css"):
            mirror = self.root / "styles" / rel
            mirror.parent.mkdir(parents=True, exist_ok=True)
            mirror.write_bytes(raw)
        self.append("http.jsonl", {
            **self.stamp(event.get("timestamp")), "event": "body", "requestId": request_id,
            "url": meta["url"], "exactBodyLength": len(raw),
            "sha256": hashlib.sha256(raw).hexdigest(), "bodyFile": str(body_path.relative_to(self.root)),
        })


DOM_SCRIPT = """() => {
  const css = e => {
    if (e.id) return '#' + CSS.escape(e.id);
    let s = e.tagName.toLowerCase();
    if (e.getAttribute('name')) s += '[name="' + CSS.escape(e.getAttribute('name')) + '"]';
    else if (e.classList.length) s += '.' + [...e.classList].slice(0,3).map(CSS.escape).join('.');
    const peers = [...e.parentElement?.querySelectorAll(':scope > ' + e.tagName.toLowerCase()) || []];
    if (peers.length > 1) s += ':nth-of-type(' + (peers.indexOf(e)+1) + ')';
    return s;
  };
  const primitiveData = e => Object.fromEntries(Object.entries(e.userData||{})
    .filter(([,v]) => ['string','number','boolean'].includes(typeof v)).slice(0,40));
  return [...document.querySelectorAll('*')].map(e => {
    const r=e.getBoundingClientRect(), st=getComputedStyle(e);
    const jq=(window.jQuery && jQuery._data) ? (jQuery._data(e,'events')||{}) : {};
    const events=[...new Set([...Object.keys(jq),...['click','mousedown','mouseup','touchstart','touchend','pointerdown','pointerup']
      .filter(n => typeof e['on'+n] === 'function')])];
    return {selector:css(e),tag:e.tagName.toLowerCase(),type:e.type||null,id:e.id||null,name:e.name||null,
      text:(e.innerText||e.value||'').trim(),value:e.value??null,checked:e.checked??null,
      disabled:!!e.disabled,ariaDisabled:e.getAttribute('aria-disabled'),title:e.title||null,
      classes:[...e.classList],events,userData:primitiveData(e),userDataKeys:Object.keys(e.userData||{}),
      opacity:st.opacity,pointerEvents:st.pointerEvents,backgroundImage:st.backgroundImage,
      visible:r.width>0&&r.height>0&&st.visibility!=='hidden'&&st.display!=='none',
      rect:{x:r.x,y:r.y,width:r.width,height:r.height}};
  }).filter(x => ['button','input','select','textarea','a'].includes(x.tag) || x.events.length ||
    x.userDataKeys.length || /(?:BUTTON|BTN|SLIDER|CLICK|SELECTOR|LOCK)/i.test(x.id||'') ||
    x.classes.some(c => /(?:button|slider|click|selector|lock)/i.test(c)));
}"""


def snapshot(page: Page, capture: Capture, label: str) -> dict[str, Any]:
    controls = page.evaluate(DOM_SCRIPT)
    state = {**capture.stamp(), "label": label, "url": page.url, "title": page.title(), "controls": controls}
    (capture.root / "dom" / f"{label}.json").write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding="utf-8")
    (capture.root / "dom" / f"{label}.html").write_text(page.content(), encoding="utf-8")
    page.screenshot(path=str(capture.root / "screenshots" / f"{label}.png"), full_page=True)
    return state


def run_action(page: Page, capture: Capture, action: dict[str, Any], index: int) -> None:
    action_id = str(action.get("id") or f"action-{index:03d}")
    selector = str(action["selector"])
    # Sony uses many HTML ids beginning with a digit.  Accept the intuitive
    # '#1_...' spelling in plans and translate it to valid CSS.
    if re.match(r"^#[0-9]", selector):
        selector = f'[id="{selector[1:]}"]'
    operation = str(action.get("operation", "click"))
    settle_ms = int(action.get("settleMs", 1200))
    capture.action_id = action_id
    before = snapshot(page, capture, f"{action_id}-before")
    capture.append("actions.jsonl", {**capture.stamp(), "event": "start", "actionId": action_id,
                                      "name": action.get("name", action_id), "selector": selector,
                                      "operation": operation, "beforeFile": f"dom/{action_id}-before.json"})
    locator = page.locator(selector)
    if locator.count() != 1:
        raise RuntimeError(f"{action_id}: selector matched {locator.count()} elements: {selector}")
    if not locator.is_visible() or not locator.is_enabled():
        raise RuntimeError(f"{action_id}: target is not visible/enabled: {selector}")
    if operation == "click":
        locator.click()
    elif operation == "hold":
        locator.hover()
        page.mouse.down()
        page.wait_for_timeout(int(action.get("holdMs", 500)))
        page.mouse.up()
    elif operation == "drag":
        box = locator.bounding_box()
        if box is None:
            raise RuntimeError(f"{action_id}: target has no bounding box: {selector}")
        start_x = box["x"] + box["width"] / 2
        start_y = box["y"] + box["height"] / 2
        page.mouse.move(start_x, start_y)
        page.mouse.down()
        page.mouse.move(start_x + float(action.get("offsetX", 0)),
                        start_y + float(action.get("offsetY", 0)), steps=5)
        page.wait_for_timeout(int(action.get("holdMs", 500)))
        page.mouse.up()
    elif operation == "fill":
        locator.fill(str(action["value"]))
    elif operation == "select":
        locator.select_option(str(action["value"]))
    elif operation == "check":
        locator.set_checked(bool(action["value"]))
    elif operation == "press":
        locator.press(str(action["value"]))
    else:
        raise RuntimeError(f"{action_id}: unsupported operation {operation}")
    page.wait_for_timeout(settle_ms)
    after = snapshot(page, capture, f"{action_id}-after")
    capture.append("actions.jsonl", {**capture.stamp(), "event": "end", "actionId": action_id,
                                      "afterFile": f"dom/{action_id}-after.json",
                                      "beforeControlCount": len(before["controls"]),
                                      "afterControlCount": len(after["controls"])})
    capture.action_id = None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--output-root", type=pathlib.Path, default=pathlib.Path("captures/fs7-native"))
    parser.add_argument("--session-name", help="directory name; defaults to an ISO-like timestamp")
    parser.add_argument("--action-plan", type=pathlib.Path, help="JSON array of selector-based actions")
    parser.add_argument("--observe-seconds", type=float, default=5.0)
    parser.add_argument("--headed", action="store_true")
    parser.add_argument("--no-precache", action="store_true",
                        help="let Chromium fetch assets in parallel (usually unreliable through WT32)")
    args = parser.parse_args()

    session_name = args.session_name or datetime.now().astimezone().strftime("%Y%m%dT%H%M%S%z")
    root = args.output_root.resolve() / session_name
    if root.exists():
        raise SystemExit(f"capture directory already exists: {root}")
    root.mkdir(parents=True)
    capture = Capture(root, args.url)
    plan = json.loads(args.action_plan.read_text(encoding="utf-8")) if args.action_plan else []
    if not isinstance(plan, list):
        raise SystemExit("action plan must be a JSON array")

    resource_cache = {} if args.no_precache else precache_live_resources(root, args.url)
    session = {"schemaVersion": 1, "tool": "fs7_native_capture.py", "url": args.url,
               "startedAt": utc_now(), "monotonicReferenceNs": capture.started_ns,
               "rawWebSocketPayloadsTruncated": False, "proxyMode": "direct/no-proxy",
               "staticResourcesPrecached": len(resource_cache),
               "actionPlan": str(args.action_plan) if args.action_plan else None}
    (root / "session.json").write_text(json.dumps(session, indent=2), encoding="utf-8")

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=not args.headed, args=["--no-proxy-server"])
        context = browser.new_context(ignore_https_errors=False, service_workers="block")
        if resource_cache:
            def serve_precached(route: Any) -> None:
                cached = resource_cache.get(route.request.url)
                if cached is None:
                    route.continue_()
                    return
                body, content_type = cached
                route.fulfill(status=200, body=body, content_type=content_type)
            context.route("**/*", serve_precached)
        page = context.new_page()
        cdp = context.new_cdp_session(page)
        cdp.send("Network.enable", {"maxTotalBufferSize": 0, "maxResourceBufferSize": 0,
                                    "maxPostDataSize": 0, "reportDirectSocketTraffic": True})
        cdp.on("Network.webSocketFrameSent", lambda e: capture.websocket("browser-to-camera", e))
        cdp.on("Network.webSocketFrameReceived", lambda e: capture.websocket("camera-to-browser", e))
        cdp.on("Network.requestWillBeSent", capture.request)
        cdp.on("Network.responseReceived", capture.response)
        cdp.on("Network.loadingFinished", lambda e: capture.response_body(cdp, e))
        cdp.on("Network.loadingFailed", lambda e: capture.append("http.jsonl", {
            **capture.stamp(e.get("timestamp")), "event": "failed", "requestId": e.get("requestId"),
            "errorText": e.get("errorText"), "blockedReason": e.get("blockedReason"),
            "canceled": e.get("canceled"), "actionId": capture.action_id}))
        page.on("console", lambda msg: capture.append("console.jsonl", {
            **capture.stamp(), "type": msg.type, "text": msg.text, "location": msg.location,
            "actionId": capture.action_id}))
        page.on("pageerror", lambda exc: capture.append("console.jsonl", {
            **capture.stamp(), "type": "pageerror", "text": str(exc), "actionId": capture.action_id}))

        try:
            # The 2016 Sony page can keep subresources/persistent connections busy;
            # DOM readiness is the useful capture boundary, not a fragile load event.
            response = page.goto(args.url, wait_until="domcontentloaded", timeout=60_000)
            if response is None or not response.ok:
                raise RuntimeError(f"navigation failed: {response.status if response else 'no response'}")
            page.wait_for_timeout(3000)
            (root / "page-before.html").write_text(page.content(), encoding="utf-8")
            snapshot(page, capture, "page-before")
            for index, action in enumerate(plan, 1):
                run_action(page, capture, action, index)
            page.wait_for_timeout(max(0, int(args.observe_seconds * 1000)))
            snapshot(page, capture, "page-after")
            (root / "page-after.html").write_text(page.content(), encoding="utf-8")
        except Exception as exc:
            capture.append("console.jsonl", {**capture.stamp(), "type": "harness-error", "text": str(exc)})
            try:
                snapshot(page, capture, "failure")
            except Exception:
                pass
            raise
        finally:
            context.close()
            browser.close()

    session.update(completedAt=utc_now(), websocketFrameCount=sum(1 for _ in (root / "websocket.jsonl").open(encoding="utf-8")) if (root / "websocket.jsonl").exists() else 0,
                   actionCount=len(plan))
    (root / "session.json").write_text(json.dumps(session, indent=2), encoding="utf-8")
    print(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
