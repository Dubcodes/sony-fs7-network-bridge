# First flash and first use

## Build

From the repository root:

```powershell
python tools\validate_project.py
python tools\validate_wt32_ota.py
python -m unittest discover -s tests -v
pio run -e wt32-eth01
```

The OTA-capable `min_spiffs.csv` layout produces `.pio/build/wt32-eth01/firmware.bin`, `bootloader.bin`, and `partitions.bin`. The first flash must establish the matching bootloader and partition table; application-only `firmware.bin` is for later OTA.

## Serial wiring and upload

Use a USB-to-TTL adapter with 3.3 V logic:

| Adapter | WT32-ETH01 |
|---|---|
| GND | GND |
| TX | RXD/GPIO3 |
| RX | TXD/GPIO1 |

1. Power off the board and disconnect the camera.
2. Hold GPIO0 to GND.
3. Power/reset the board to enter the ROM bootloader.
4. Upload with `pio run -e wt32-eth01 -t upload --upload-port COMx`.
5. Power off and remove GPIO0 from GND.
6. Power on normally and monitor with `pio device monitor --port COMx --baud 115200`.

A normal boot identifies `0.2.13-field-patch`, the WT32-ETH01 profile, and LAN8720 RMII Ethernet.

## Ethernet and camera setup

The bridge requests an Ethernet DHCP lease. Read the leased address from serial and open `http://<bridge-ip>/setup`. For a direct cable without DHCP, deliberately choose static mode; the retained profile is bridge `10.77.7.2/24` and laptop `10.77.7.1/24` with no gateway.

In setup, scan/select the FS7 SSID, enter its Wi-Fi password and current Basic Auth credentials, confirm the camera address, save, and reconnect. Test the connection before using `/operator`. The management surface is unauthenticated, so keep it on a trusted isolated network.

## OTA after first flash

Build the new source, open Firmware & System on `/setup`, and upload `.pio/build/wt32-eth01/firmware.bin`. Wait for acceptance and reboot. Interrupted uploads are aborted and do not intentionally boot the incomplete slot. Retain serial recovery access.
