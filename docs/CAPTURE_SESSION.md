# FS7 protocol capture session

## Goal

Produce three files that can be correlated later without slowing down camera operation:

```text
fs7-requests.jsonl
fs7-session.json
FS7-discovery-screen-recording.mp4
```

The network log is the machine-readable truth. The screen recording supplies human context about what changed and when.

## Network requirement

`tools/fs7_proxy.py` runs on the laptop and must be able to reach the FS7 camera IP.

The ESP does not currently route its camera-side Wi-Fi network onto Ethernet. Use either:

### Option A — simultaneous connections (intended first session)

- ESP joins FS7 hotspot.
- Laptop Wi-Fi also joins FS7 hotspot.
- Laptop Ethernet remains connected to the ESP through the control LAN (normally DHCP; use the documented static profile only for a direct cable).

If the camera hotspot does not permit both clients reliably, use Option B.

### Option B — separate discovery pass

- Put the laptop directly on the FS7 hotspot.
- Run the logger and capture the Sony UI.
- Stop the capture.
- Reconnect the ESP workflow afterwards.

Nothing about the command-map analysis requires the ESP to be connected during the discovery recording.

## Start the logger

From the repository:

```text
python tools/fs7_proxy.py --camera 192.168.1.1 --user <camera-user>
```

Open:

```text
http://127.0.0.1:8080/rm.html
```

The logger prompts for the Basic Auth password so it is not exposed in the shell command line. `Authorization`, `Proxy-Authorization`, cookies, and `Set-Cookie` values are excluded from the JSONL log. It records `authorizationPresent`, `browserAuthorizationPresent`, `requestCookieNames`, and `responseSetCookieNames` so session dependencies remain visible without storing secret values. Request bodies are capped at 1 MiB, upstream responses at 8 MiB, and textual response bodies stored in the log at 64 KiB.

The logger records:

- timezone-aware wall-clock `requestTime`, captured when request handling starts
- monotonic `requestCaptureMs`, captured at request start and authoritative for video correlation
- monotonic `responseCaptureMs`, captured after the upstream response completes or fails
- `durationMs`, equal to response time minus request time
- legacy `time`, retained as a wall-clock request-time alias
- legacy `captureMs`, retained as a response-completion/log-time alias
- sequential `eventId`
- request method/path/content type/body
- non-secret request headers
- response status/headers
- textual response body (bounded)

At startup the proxy prints `CAPTURE SESSION START` and establishes monotonic zero. By default it also writes `fs7-session.json` beside `fs7-requests.jsonl`, containing the start wall clock, local timezone/UTC offset, monotonic reference, proxy version, camera/listen addresses, authentication-presence metadata, and log filename. It never contains credentials. Use `--session-metadata <path>` to choose another name or `--no-session-metadata` to disable it.

## Screen recording

After the logger prints its start marker, start a normal Windows screen recording before operating controls. You do **not** need to stop and type annotations.

If convenient, speak the important transitions aloud, e.g. "WB 3200 to 4300", but this is optional.

## Recommended real capture sequence

1. Connect laptop Ethernet to the bridge control LAN and laptop Wi-Fi to the FS7 hotspot.
2. Start `fs7_proxy.py`; note the printed `CAPTURE SESSION START` reference.
3. Start Windows screen recording.
4. Leave the camera/remote idle for about 20 seconds.
5. Operate Sony's browser controls naturally, changing one deliberately tested item at a time.
6. During the first pass, pause 3–5 seconds between deliberate changes.
7. Repeat important changes using the physical camera controls while the remote page remains open.
8. Stop the screen recording.
9. Stop the proxy/logger with Ctrl+C.
10. Preserve the JSONL, session JSON, and video together.

Speaking the important transitions aloud in the recording can help later, but is optional. No typed live annotation workflow is required. Once a family of requests is obvious, you can move faster.

Useful groups:

### Transport

- Record start
- Record stop
- Rec Review / play-last if available
- Play / pause / stop

### Exposure / look

- WB preset/Kelvin/memory modes
- AWB / ATW
- iris
- gain/EI
- shutter
- ND
- gamma / MLUT

### Slow motion

- S&Q on/off if exposed
- several S&Q FPS values

### Physical-camera readback test

With the Sony remote page open but untouched, change the same settings physically on the camera. This determines whether the underlying status response is genuinely camera-state-driven rather than only browser-local.

## After capture

Run:

```text
python tools/fs7_log_summarize.py fs7-requests.jsonl
python tools/fs7_feedback_analyze.py fs7-requests.jsonl
```

Then supply the JSONL, session metadata, and screen recording together for protocol mapping.
