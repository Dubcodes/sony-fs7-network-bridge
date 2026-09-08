# Bridge API v1

The bridge serves JSON over HTTP on the wired management interface. Existing `/api/v1` paths and semantic command IDs are compatibility interfaces. JSON responses use `application/json; charset=utf-8` and `Cache-Control: no-store`.

The API has no authentication or TLS. Use it only on a trusted control network.

## Discovery and state

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/v1/status` | Device/version/API identity, Ethernet, Wi-Fi, camera, current operation, telemetry, last command/error |
| GET | `/api/v1/system` | Board, reset, memory, flash, partitions, filesystem, network, operation, and OTA diagnostics |
| GET | `/api/v1/camera-state` | All semantic camera values with validity, age, and freshness |
| GET | `/api/v1/catalog` | API/firmware/device identity plus supported command and state descriptors |

`status.apiVersion` and `catalog.apiVersion` are `v1`. `catalog.commands[].configured` distinguishes supported semantic commands from commands whose camera mapping is ready. Unknown/stale values are JSON `null` or explicitly marked invalid/stale; optimistic command intent is separate from confirmed camera state.

## Commands and sequences

```text
POST /api/v1/command/<command-id>
POST /api/v1/stop-replay/abort
GET  /api/v1/sequences
POST /api/v1/sequences
GET  /api/v1/sequences/status
POST /api/v1/sequences/<1..8>/trigger
POST /api/v1/sequences/<1..8>/abort
```

Command and VSM-trigger results are deterministic objects containing `ok`, `message`, and `sonyHttpStatus`. Successful commands return 2xx. Invalid input or mappings return 4xx. A camera-operation or configuration conflict returns 409 and does not bypass the Operation Arbiter.

## Generic external-control model

```text
GET  /api/v1/vsm-map
POST /api/v1/vsm-map
GET  /api/v1/vsm/feedback
GET  /api/v1/vsm/slots
POST /api/v1/vsm/trigger/<1..16>
```

The historical `/vsm` name is retained for compatibility. The implementation is transport-neutral: numbered triggers call semantic commands/sequences, and typed slots read the shared Camera State/bridge state. Invalid typed values serialize as `null`.

## Camera diagnostics and mapping

```text
GET  /api/v1/command-map
POST /api/v1/command-map
GET  /api/v1/telemetry-map
POST /api/v1/telemetry-map
GET  /api/v1/telemetry-diag
POST /api/v1/sony/raw
POST /api/v1/sony/test
POST /api/v1/sony/linear-test
GET  /api/v1/sony/capture
POST /api/v1/sony/capture/start
POST /api/v1/sony/capture/stop
POST /api/v1/sony/capture/clear
```

Raw requests and mapping updates are bounded and validated. Capture is opt-in, RAM-only, bounded, and intended for diagnostics. These endpoints do not authorize guessing Sony methods or values.

## Setup, layout, and OTA

```text
GET  /api/v1/config
POST /api/v1/config
GET  /api/v1/wifi-scan
POST /api/v1/reconnect-camera
GET  /api/v1/layout
POST /api/v1/layout
GET  /api/v1/export
POST /api/v1/reboot
POST /api/v1/update
```

Credential fields are redacted in normal config/export responses. Runtime-changing updates are rejected with 409 while automation owns the camera. OTA accepts an application image and only schedules reboot after a successful update.

## TCP control

TCP port 5000 is documented in `INTEGRATION.md`. It calls the same engines and preserves the same conflict behavior, but uses compact line responses instead of HTTP/JSON.
