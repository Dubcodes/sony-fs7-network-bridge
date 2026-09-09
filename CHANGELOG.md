# Changelog

## 0.2.13-field-patch

- Set the fresh-install Ethernet/DHCP hostname to `FS7-WiFi-Bridge` while preserving explicitly stored device names during upgrades.
- Added a dedicated `/network` page and minimal Ethernet-only API for persistent DHCP or static WT32-ETH01 addressing, with client/server IPv4 validation and optional save-and-reboot.
- Preserved DHCP as the backward-compatible default, the exact legacy recovery-profile migration, and the retained 10.77.7.2/24 direct-connect values.
- Made Stop + Replay's existing shared qualified latest-clip path explicit and report `PLAYBACK_VERIFIED` only after its acknowledged Set, explicit Play, and `P.Clip.Mediabox.Status=Playing` gate complete.
- Added focused regression coverage without changing the camera-qualified Thumbnail, Set-retry, Set-to-Play, timeout, persistent `/linear`, telemetry, or 100 ms zoom behavior.

## 0.2.13-operator-polish

- Added the compact `/operator` console with complete live-qualified recording, playback, menu/cursor, assignable, automatic-mode, focus, and zoom controls.
- Routed operator controls through the shared Command Engine so REST, TCP, sequences, VSM, and the web UI use the same qualified Sony mappings.
- Replaced the fixed Thumbnail-to-Set assumption with bounded Set retries keyed to the FS7 RPC acknowledgement, followed by explicit `P.Clip.Mediabox.Status=Playing` verification.
- Added a 25-second default overall replay timeout while retaining conservative minimum Stop, first-Set, and Play delays.
- Corrected Savona wire shapes: nested `Button.SendKeys`/automatic-adjustment parameters and array-wrapped property maps.
- Added one `/rm.html` warmup request per camera Wi-Fi association for FS7 sessions that require the native remote handshake.
- Expanded the bounded RAM-only bridge capture to both WebSocket directions and added the Playwright/CDP raw-capture tool.
- Refined the Sequences page to offer only configured, qualified commands and clearly identify unavailable saved mappings.
- Made H/J and the Zoom Wide/Tele buttons sustain repeated zoom velocity pulses while held, while retaining immediate stop on keyup, pointer release, blur, page hide, disconnect, and lost pointer capture.
- Reused one authoritative subscribed Sony `/linear` connection for commands and camera state, removing per-command WebSocket setup and duplicate operator subscriptions.
- Matched Sony's measured 100 ms zoom-write cadence with serialized acknowledgements; a clean live three-second hold produced 30 movement writes plus one release stop in each direction.
- Preserved Stop + Replay's bounded Thumbnail readiness retries by normalizing persistent-socket receive expiry to the established Sony RPC-timeout result.

## 0.2.12-integration-cleanup

- Refreshed the repository and documentation around the current WT32-ETH01/LAN8720 hardware and real-camera-tested Sony native workflow.
- Added `apiVersion`, firmware version, and device identity metadata to the versioned status/catalog API.
- Extended the single ASCII TCP client's idle window from 30 seconds to five minutes while preserving port 5000, `PING`/`PONG`, framing, commands, and error behavior.
- Added current REST/TCP/VSM integration guidance and clean future boundaries for native Ember+ and CyanView/other RCP systems.
- Added GitHub Actions for validators, all host tests, and the actual WT32-ETH01 PlatformIO build.
- Removed generated firmware, test/capture output, caches, source backups, obsolete patch/package material, and development review archaeology from the public source tree.
- No Sony HTTP or `/linear` protocol behavior changed.

## 0.2.2-pre-hardware

- Added one central camera-operation arbiter for manual commands, Stop + Replay, and all eight sequences, including conflict diagnostics and lifecycle-safe release.
- Rejected asynchronous `stop_replay` sequence steps and blocked runtime-changing configuration while automation is active.
- Added strict generic bool/integer/float parsing; invalid data now exports as JSON `null` rather than false or zero.
- Made TCP state reads freshness-aware and added a 30-second idle-client timeout.
- Hardened Sony HTTP framing for complete headers, Content-Length, no-body, chunked, close-delimited, malformed, truncated, oversized, and timeout cases.
- Added telemetry timeout capping and exponential failure backoff while retaining disabled-by-default synchronous polling.
- Made Stop + Replay report `REVIEW_DISPATCHED`, bounded optimistic playback intent, and an explicit abort path.
- Removed wildcard CORS and escaped or safely constructed remaining dynamic web UI content.
- Fixed config persist-before-live-commit order and primary-missing/backup-valid recovery ordering.
- Pinned ArduinoJson 7.4.3 and added 16 cross-module pre-hardware contract tests.
- Added cookie-name and authorization-presence capture metadata without logging secret values.
- Sony command and telemetry maps remain deliberately blank.

## 0.2.1-codex-reviewed

- Compiler-verified against Arduino-ESP32 3.3.8 / ESP-IDF 5.5.4 and fixed the project/framework `NetworkManager` class collision.
- Moved to the standard `min_spiffs.csv` layout, increasing application headroom from 2.1% to 34.5% while retaining LittleFS.
- Added explicit fresh/stale camera-state output and stopped stale values from appearing as confirmed REST/TCP/generic-control feedback.
- Redacted credentials from every configuration export and hardened raw Sony requests against method/header injection and oversized payloads.
- Added configuration bounds checking and power-loss recovery for atomic LittleFS file replacement.
- Hardened proxy capture limits and made event IDs match JSONL write order under concurrent requests.
- Changed discovery tools to prompt for camera passwords by default.

## 0.2.0-pre-codex — state-driven handoff build

- Changed the direct-test Ethernet defaults to a deterministic static link:
  - bridge `10.77.7.2/24`
  - suggested laptop address `10.77.7.1/24`
- Kept the FS7 camera network separate on Wi-Fi and retained explicit Wi-Fi source binding for Sony HTTP sockets.
- Hardened `STOP + REPLAY`:
  - prefers semantic `ready`, `writing`, then `recording` feedback;
  - requires a telemetry sample received **after** STOP before state can advance the macro;
  - retains a bounded delay fallback while Sony telemetry is not yet mapped;
  - adds overall replay timeout and phase reporting.
- Added semantic camera states `ready`, `writing`, and `buffer_ready` (23 total states).
- Added an eight-slot non-blocking sequence engine with:
  - camera commands;
  - timed waits;
  - waits on fresh camera state;
  - per-step and overall timeouts;
  - abort;
  - `DISABLED / READY / ACTIVE / WAITING / WRITING / PLAYBACK / COMPLETE / ERROR / ABORTED` status.
- Added a disabled example sequence: record → 10 s → stop → wait for fresh `ready=true` → Rec Review.
- Added `/sequences` editor and sequence REST/TCP endpoints.
- Generic external-control slots can now target sequence triggers and sequence status values, without locking the project to a particular VSM transport.
- Added first-flash and capture-session procedures for the planned FS7 discovery workflow.
- Added Wi-Fi scanning to setup.
- Sony command and feedback maps remain deliberately blank until captured from a physical FS7.
- Moved old Generic REST VSM definitions under `vsm/legacy/`; production VSM transport is intentionally undecided pending engineering approval.
- Added `CODEX_REVIEW.md` with compile/review requirements and non-negotiable protocol-safety rules.
- Enhanced the FS7 proxy log with sequential event IDs and monotonic capture timestamps for easier alignment to a screen recording.

## 0.1.1-alpha — generic VSM + camera feedback foundation

- Added generic trigger/value slots, semantic camera feedback, configurable telemetry extraction, and live web data tiles.
- Added response-body capture and feedback endpoint analysis tools.

## 0.1.0-alpha

- Initial ESP32/W5500 + FS7 Wi-Fi bridge skeleton.
- Browser control/layout/setup/discovery pages.
- Configurable Sony command mappings and Stop + Replay state machine.
- REST and ASCII TCP control interfaces.
- Initial Lawo integration experiments and FS7 protocol-discovery tools.
