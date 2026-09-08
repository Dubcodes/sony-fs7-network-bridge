# Camera feedback design

Sony documents the PXW-FS7 Wi-Fi Remote main screen as containing shooting-setting fields including S&Q FPS, shutter, white balance, sensitivity/gain/exposure index, gamma, MLUT, iris, focus and zoom, plus status/playback functions. Those values therefore reach Sony's browser UI in some form.

Sony warns that its stock Wi-Fi Remote screen may no longer match camera settings if the camcorder is operated directly and advises reloading the browser. The bridge has read live properties from a real camera, but each property still requires physical-control testing before it can be treated as authoritative external readback.

## Polling sources

The bridge provides four configurable camera polling sources. Each contains:

- enabled
- HTTP method/path
- content type
- request body
- extra headers
- polling interval
- **telemetry-specific timeout**

All requests use the configured FS7 Basic Authentication and the camera HTTP socket is bound to the ESP32 Wi-Fi interface. The short telemetry timeout is independent of normal command timeout, preventing a missing status request from blocking operator commands for an unnecessarily long period.

Only one telemetry HTTP transaction is started per bridge loop pass.

## Extractors

Each fixed camera-state field may reference one source and one extraction rule:

- `json`: dotted object path, e.g. `camera.white.value`
- `between`: text between `startToken` and `endToken`; useful for XML, HTML fragments or form-style data
- `whole`: entire response body
- `header`: HTTP response header named by `selector`

The state store records the raw display value and its age.

## Camera-state fields

The current stable state model contains:

- recording
- playback
- ready
- writing
- slow-motion buffer ready
- white balance display
- white balance Kelvin
- white balance mode
- gain dB
- exposure index
- iris
- ND
- shutter
- S&Q FPS
- gamma
- MLUT
- focus
- zoom
- ATW
- auto iris
- camera mode
- media status
- timecode

These identifiers remain stable even if Sony's private response format changes.

## Webpage

The main operator page polls `/api/v1/camera-state` and can display any selected state fields as tiles with an **age** indicator. The Layout page decides which data tiles are visible and their order.

The Camera Data page shows every parsed field plus polling-source health. The diagnostics endpoint also exposes a bounded preview of the most recent response from each source to make field mapping easier.

## External/VSM feedback

The transport-neutral generic value model reads from exactly the same state store. A field can be mapped to a numbered Bool/Int/Float/Text slot on `/vsm`. Whichever VSM adapter is eventually approved should expose only the points engineering actually wants; it must not create a second camera-state implementation.

## Real versus optimistic transport state

Status retains explicitly labelled local optimistic transport hints after successful commands when confirmed feedback is unavailable. They are not presented as camera truth. When `recording` and `playback` telemetry is fresh, the UI uses real values; when stale, the last raw value remains diagnostic and is not exported as current generic feedback.

For production use, the preferred end state is to have both fields mapped from the camera so a physical REC/Playback operation is visible to the bridge as well.

## Discovery workflow

The updated `fs7_proxy.py` records textual response bodies. `fs7_feedback_analyze.py` groups repeated requests and ranks those with changing responses. A repeated request whose response changes when WB, iris or REC changes is a strong telemetry candidate.

Change one setting at a time during capture. Then repeat the same changes **physically on the camera**, not through Sony's webpage, to establish whether the endpoint provides true external readback.
