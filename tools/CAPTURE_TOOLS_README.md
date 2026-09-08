# FS7 capture tools

Laptop-side tools for observing Sony's real web remote when extending or verifying PXW-FS7 protocol evidence. The proxy capture format remains version 0.2.2.1; these tools do not contain copied Sony assets or user credentials.

## Quick start

With laptop Wi-Fi connected to the FS7 hotspot:

```powershell
python tools\fs7_proxy.py --camera <actual-camera-ip> --user <camera-user>
```

The password is prompted without echo. When the proxy prints `CAPTURE SESSION START`, start Windows screen recording and open the displayed `/rm.html` proxy URL.

Keep these three files together:

- `fs7-requests.jsonl`
- `fs7-session.json`
- the screen recording

Use `requestCaptureMs` as the authoritative event-start timestamp. `responseCaptureMs` marks upstream completion/failure and `durationMs` is the transaction duration. Legacy `captureMs` remains a completion/log-time compatibility alias.

After capture:

```powershell
python tools\fs7_log_summarize.py fs7-requests.jsonl
python tools\fs7_feedback_analyze.py fs7-requests.jsonl
```

For a tool-only self-test:

```powershell
python tools\mock_fs7.py --port 18080
python tools\fs7_proxy.py --camera 127.0.0.1 --camera-port 18080 --port 18081 --log mock-requests.jsonl --password pxw-fs7
python -m unittest discover -s tests -p "test_capture_tools.py" -v
```

Mock endpoints and values are fixtures only. Never copy them into a real bridge map.

Authorization/Proxy-Authorization values, passwords, Cookie values, and Set-Cookie values are excluded from capture logs and session metadata. Only presence flags and cookie names are retained.

See `docs/CAPTURE_SESSION.md` and `docs/SONY_DISCOVERY.md` for the real-session procedure.
