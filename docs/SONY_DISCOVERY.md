# Extending FS7 web-control and feedback evidence

The bridge already has evidence-backed native connectivity, `/linear`, live-property, recording, and replay behavior. Use this workflow only to confirm remaining properties or investigate a concrete camera/firmware difference.

Sony documents the PXW-FS7 Wi-Fi remote at:

```text
http://<camera IP>/rm.html
```

with the username/password configured under **System -> Basic Authentication**. A commonly documented camera IP is `192.168.1.1`.

The goal of a new session should be narrowly defined:

1. reproduce and preserve the exact traffic for the specific unconfirmed control;
2. determine whether its response/state remains correct when the physical camera is operated.

## Preferred capture method

1. For the intended discovery session, keep laptop Ethernet connected to the ESP32 and connect laptop Wi-Fi to the FS7 hotspot. If the hotspot cannot support both clients, perform a separate laptop-to-FS7 discovery pass.
2. From this project's root run:

   ```powershell
   py tools\fs7_proxy.py --camera 192.168.1.1 --user admin
   ```

3. Open `http://127.0.0.1:8080/rm.html`.
4. Note the printed `CAPTURE SESSION START` reference and the generated `fs7-session.json` path.
5. Start Windows screen recording and verify Sony's own remote page works through the proxy.
6. Leave the page/camera idle for about 20 seconds, then perform **one operation/change at a time**, with a few seconds between deliberate first-pass changes.
7. Include changes made from the physical camera controls as well as browser controls.
8. Stop the recording, stop the logger, and preserve JSONL + session JSON + video together.

The password is prompted without echo. The proxy injects Basic Authentication upstream and deliberately excludes Authorization/Proxy-Authorization values and cookie values from its log. Cookie names and authorization presence remain available. It records request details, bounded response bodies, and explicit request-start/response-completion monotonic timing even for failed transactions.

For video synchronisation, use `requestCaptureMs` as the authoritative network-event start. `responseCaptureMs` is the upstream completion/failure time and `durationMs` is their difference. The old `captureMs` field remains compatible and means completion/log time; old JSONL captures remain accepted by both analysis tools.

## Find command requests

Exercise:

- REC start
- REC stop
- Rec Review / an Assign button configured as Rec Review
- playback Play/Pause/Stop
- Auto White
- iris/gain/ND controls you care about

Then run:

```powershell
py tools\fs7_log_summarize.py fs7-requests.jsonl
```

This reduces the log to likely state-changing requests.

## Find feedback/status requests

With the proxy still running, change one camera value at a time and let Sony's own page sit for a few seconds after each change:

- white balance / color temperature
- gain or EI
- iris
- ND
- shutter
- S&Q FPS
- gamma / MLUT
- recording state
- playback/Rec Review state

Then run:

```powershell
py tools\fs7_feedback_analyze.py fs7-requests.jsonl
```

The analyser groups identical request signatures and ranks requests whose response body changes between calls. A frequently repeated GET with several response variants is a strong candidate for the camera-status feed.

### Critical physical-control test

Sony's manual warns that the stock Wi-Fi Remote page can stop matching the camcorder after the **camcorder is operated directly** and recommends reloading the browser.

Therefore, once a likely feedback endpoint is found, repeat the test by changing WB/iris/gain/ND using the physical FS7 controls while watching the raw response. This tells us whether:

- the endpoint itself always reports live camera state (ideal), or
- Sony's web session can return stale values until a refresh/reinitialisation request is made.

The bridge is deliberately designed to poll the camera endpoint itself, so browser JavaScript caching is irrelevant; the remaining question is the behavior of the FS7 endpoint, which only the real capture can answer.

## Static source scan

The companion tool:

```powershell
py tools\fs7_asset_scan.py --camera 192.168.1.1 --user admin
```

downloads `rm.html`, `rms.html`, `rmt.html`, linked local assets and JavaScript, then produces `interesting-lines.txt` containing likely request/control/status code. This can reveal command or telemetry endpoints even before every control is exercised.

## Enter mappings without rebuilding

### Commands

Open the bridge's **Sony Discovery** page and enter the captured:

- HTTP method
- path/query string
- Content-Type
- body
- required extra request headers

### Feedback

Open **Camera Data** (`/telemetry`):

1. configure up to four authenticated polling sources;
2. set interval and a short telemetry-specific timeout;
3. map each wanted field to a source;
4. extract using:
   - `json` dotted selector;
   - `between` start/end tokens (also useful for XML or form-style responses);
   - `whole` response;
   - `header` name.

The operator webpage and VSM then read from the same camera-state store.

## Existing proof and remaining evidence

The current working baseline includes native camera connection, `/linear`, live Sony properties, record start/stop, Rec Review/latest-clip workflows, and Auto White/Auto Black. Remaining operator properties must still be added or promoted only when captured and tested. In particular, verify:

- physical-control readback and staleness behavior for each mapped property;
- camera firmware/model differences;
- failure and reconnect behavior during commands;
- any property still marked unconfigured in `/api/v1/catalog`.

## Self-test without a camera

The tools include a mock server:

```powershell
py tools\mock_fs7.py --port 18080
```

Then:

```powershell
py tools\fs7_asset_scan.py --camera 127.0.0.1 --camera-port 18080 --out mock-scan
py tools\fs7_proxy.py --camera 127.0.0.1 --camera-port 18080 --port 18081 --log mock-requests.jsonl
```

The mock server has a changing status response. `fs7_feedback_analyze.py` has been tested against it and correctly identifies that status request as a feedback candidate. Mock endpoints are **not Sony endpoints** and must never be copied into a real bridge map.
