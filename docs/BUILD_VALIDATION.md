# Build and validation

## Qualified release

Release `0.2.12-integration-cleanup` was locally validated on 2026-09-08 with:

- Python 3.12
- PlatformIO Core 6.1.19
- pioarduino Espressif32 platform 55.3.38
- Arduino-ESP32 3.3.8 / ESP-IDF libraries 5.5.4
- ArduinoJson 7.4.3
- Xtensa ESP GCC 14.2.0
- WT32-ETH01 board profile and `min_spiffs.csv`

Run:

```powershell
python tools\validate_project.py
python tools\validate_wt32_ota.py
python -m unittest discover -s tests -v
pio run -e wt32-eth01
```

GitHub Actions runs the same validators, host suite, and physical-target compile on pushes and pull requests. The validators also parse embedded browser JavaScript with Node.js.

## Latest local result

- both static validators: pass
- host tests: 89/89 pass
- WT32-ETH01 build: success with `-Wall -Wextra`
- static RAM: 74,592 / 327,680 bytes (22.8%)
- application flash: 1,367,336 / 1,966,080 bytes (69.5%)
- `firmware.bin`: 1,367,744 bytes (SHA-256 `3f18a18af3adbd5841836e32de86faf484a2bfb85327464eb30159f83f0b02e6`)

## What this proves

The checks cover source/configuration invariants, Ethernet and Wi-Fi binding, OTA ownership/failure paths, native proxy behavior, `/linear` framing contracts, evidence-backed command mappings, operator-console contracts, sequence/arbitration safety, typed generic feedback, API identity, TCP idle/keepalive behavior, capture redaction, and a real WT32 compiler/linker build.

Host tests are structural/model contracts, not a substitute for hardware integration. Existing real-camera evidence covers connectivity, the native proxy/remote, `/linear`, live properties, and record start/stop. Long-duration Ethernet/Wi-Fi coexistence, all property mappings, failure recovery under field conditions, and facility controller integrations still require operational testing.
