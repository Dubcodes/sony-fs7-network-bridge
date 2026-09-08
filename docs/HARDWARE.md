# Hardware

## Current reference target

The only current PlatformIO target is the Wireless-Tag WT32-ETH01 v1.4:

- classic dual-core ESP32, 240 MHz
- 320 KiB addressable SRAM reported by the build
- 4 MB flash
- onboard LAN8720A RMII Ethernet PHY
- onboard 2.4 GHz Wi-Fi used as STA to the camera

| Function | WT32-ETH01 connection |
|---|---|
| LAN8720 PHY address | 1 |
| MDC | GPIO23 |
| MDIO | GPIO18 |
| external 50 MHz oscillator enable | GPIO16 |
| RMII reference clock input | GPIO0 |
| UART RX/TX | GPIO3/GPIO1 |

The values are explicit in `platformio.ini` and `src/NetworkManager.cpp`. Earlier W5500 development hardware is historical and is not a supported build environment in this tree.

## Bootloader caution

GPIO0 is dual-purpose. Hold GPIO0 low only during reset/power-up to enter the ROM serial bootloader. Remove the connection to ground before normal boot because the LAN8720 uses GPIO0 for the external RMII clock input.

Use 3.3 V UART logic. The board may be powered at its specified 5 V input from a suitable regulated supply, but do not apply 5 V logic to ESP32 pins. Share ground between the supply, adapter, and board.

## Network roles

Ethernet is the trusted control/management side. Wi-Fi is a short camera-side station link to the FS7. DHCP is the Ethernet default; an explicit static profile is available for isolated direct-cable recovery. Camera sockets are source-bound to Wi-Fi.

No status LED, GPIO button, LANC interface, PoE power stage, or custom carrier is part of this release. Those require separate electrical and operational design work.
