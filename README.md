# Sony FS7 Network Bridge

WT32-ETH01 Ethernet-to-Wi-Fi control bridge for Sony PXW-FS7 cameras. It gives an isolated wired control network a browser operator console, a versioned REST API, simple ASCII TCP control, sequences, and a transport-neutral integration model while keeping the camera on its own Wi-Fi link.

Current firmware: `0.2.12-integration-cleanup`

This is an engineering/field-test prototype, not a production-certified appliance. It has connected to a real FS7, proxied Sony's native remote, established the `/linear` WebSocket transport, read live properties, and started/stopped recording. Broader property mapping, long-duration soak testing, and facility-specific integration remain ongoing.

## Topology

```text
browser / VSM / automation
            |
   wired Ethernet (control LAN)
            |
  WT32-ETH01 + LAN8720
            |
       Wi-Fi STA
            |
       Sony PXW-FS7
```

The bridge explicitly binds camera-side HTTP and `/linear` sockets to Wi-Fi. This matters when the wired and camera networks use overlapping IPv4 ranges. The bridge is not a router or NAT gateway.

## What works

- WT32-ETH01 LAN8720 Ethernet with DHCP by default and optional static addressing
- FS7 camera Wi-Fi configuration and reconnect
- custom operator console at `/operator`, including keyboard control
- Sony native remote fallback at `/sony-native`, proxied from the connected camera on TCP 8081
- native Sony HTTP and evidence-backed `/linear` WebSocket/MessagePack RPC
- record start/stop, Rec Review/latest-clip workflows, Stop + Replay, Auto White, and Auto Black
- eight non-blocking sequences with arbitration, waits, timeouts, and abort
- REST API under `/api/v1`
- line-oriented ASCII TCP control on port 5000
- generic trigger and feedback slots for external-control adapters
- application-only web OTA and system diagnostics
- laptop-side discovery/capture tools that redact credentials and cookie values

Sony's native page remains available as an operational fallback; the custom console is the intended normal interface. The Sony backend is isolated from REST, TCP, VSM, and future adapter code so every controller reaches the same Command Engine, Sequence Engine, Camera State model, and Operation Arbiter.

## Hardware

The supported reference target is a Wireless-Tag WT32-ETH01 with classic ESP32, onboard LAN8720A RMII Ethernet, and 4 MB flash. The exact build profile is in `platformio.ini`:

- PHY address 1
- MDC GPIO23
- MDIO GPIO18
- external oscillator enable GPIO16
- RMII clock input GPIO0

GPIO0 must be held low only while entering the ROM serial bootloader and released for normal boot because Ethernet uses it as the RMII clock input. See `docs/HARDWARE.md` and `docs/FIRST_FLASH.md`.

Earlier ESP32 DevKit/W5500 work is historical and is not the current hardware target.

## First use

1. Install Python 3, Node.js, and PlatformIO Core.
2. Run the validators, host tests, and build shown below.
3. Flash the WT32 over a 3.3 V logic USB-to-TTL adapter using the GPIO0 boot procedure in `docs/FIRST_FLASH.md`.
4. Connect Ethernet to a DHCP-enabled control LAN and find the leased address in the 115200-baud serial log.
5. Open `/setup`, select the FS7 SSID, enter camera Wi-Fi and Basic Auth credentials, save, and reconnect.
6. Verify the camera connection, then use `/operator`. Use `/sony-native` if the custom console lacks a needed control.

For an isolated direct cable without DHCP, select static addressing in setup. The retained recovery profile is bridge `10.77.7.2/24` and laptop `10.77.7.1/24`; it is not an automatic fallback.

## Operator and configuration pages

- `/operator` — primary operational console
- `/sony-native` — live Sony remote fallback through the bridge proxy
- `/` — configurable command/status surface
- `/layout` — operator button and data-tile layout
- `/setup` — network, camera authentication, diagnostics, and OTA
- `/telemetry` — camera-state extraction and diagnostics
- `/sequences` — sequence configuration, trigger, status, and abort
- `/discovery` — safe Sony transport probes and mapping tools
- `/vsm` — transport-neutral trigger/value slot configuration

The stock web/control surfaces are unauthenticated. Deploy only on a trusted, isolated control network. Configuration exports redact Wi-Fi and camera passwords, but anyone with network access can operate the camera, change configuration, reboot the bridge, or submit OTA firmware.

## External control

REST clients use the stable `/api/v1` surface. TCP clients connect to port 5000 and send one LF- or CRLF-terminated command per line. `PING` returns `PONG`; quiet single-client sessions are retained for five minutes, after which clients reconnect. Port 5000 remains the default and existing commands and IDs are unchanged.

The most practical initial Lawo VSM path is an outbound generic TCP connection to the bridge. REST is also available where an HTTP-capable VSM/GadgetServer workflow is approved. Legacy Simple REST material in `vsm/legacy/` is historical and unverified against the current API.

See `docs/API.md` for endpoint semantics and `docs/INTEGRATION.md` for TCP, VSM, future Ember+, CyanView, and other third-party integration boundaries.

## Build and test

```powershell
python tools\validate_project.py
python tools\validate_wt32_ota.py
python -m unittest discover -s tests -v
pio run -e wt32-eth01
```

The PlatformIO environment pins the WT32 board, Arduino-ESP32/ESP-IDF platform bundle, ArduinoJson, warning flags, and OTA-capable `min_spiffs.csv` partition layout. GitHub Actions repeats both validators, all host tests, and the same WT32 build on pushes and pull requests.

## OTA

After the initial serial flash, upload `.pio/build/wt32-eth01/firmware.bin` from the Firmware & System section on `/setup`. OTA updates the application slot only; it does not intentionally replace the bootloader, partition table, NVS, or LittleFS. Keep a serial recovery path available.

## Discovery tools

`tools/fs7_proxy.py`, `fs7_asset_scan.py`, `fs7_log_summarize.py`, and `fs7_feedback_analyze.py` support evidence-driven mapping from a camera the operator owns. Captures and downloaded camera assets are local-only and ignored by Git. This repository does not distribute captured Sony web assets.

See `docs/SONY_DISCOVERY.md` and `docs/CAPTURE_SESSION.md` before collecting new evidence. Mock endpoints are test fixtures only and must never be treated as Sony protocol facts.

## Current limits

- one ASCII TCP client is served at a time; reconnect after an idle close or link loss
- management and control have no authentication or TLS
- Sony HTTP telemetry is synchronous and can briefly delay the main loop
- not all FS7 properties have confirmed mappings or physical-control readback
- overlapping subnets are mitigated by explicit Wi-Fi source binding, but distinct subnets are preferable
- native Ember+ and CyanView integrations are future adapters, not current features
- mDNS is deliberately omitted until it can be hardware/stack tested without adding discovery instability
- no GPIO LEDs or physical buttons are implemented in this release

## Independence and license

This independent open-source project is not affiliated with, sponsored by, or endorsed by Sony, Lawo, or CyanView. Product and company names identify interoperability targets only. No vendor web assets or firmware are included.

Licensed under the existing MIT License; see `LICENSE`.
