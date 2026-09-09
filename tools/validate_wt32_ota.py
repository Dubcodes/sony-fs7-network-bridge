#!/usr/bin/env python3
"""Focused static validation for the WT32-ETH01 FS7-native Sony /linear build."""
from __future__ import annotations
import pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
checks = []

def need(rel: str, token: str, message: str):
    text = (ROOT / rel).read_text(encoding="utf-8")
    if token not in text:
        checks.append(message)

def forbid(rel: str, token: str, message: str):
    text = (ROOT / rel).read_text(encoding="utf-8")
    if token in text:
        checks.append(message)

need("platformio.ini", "board = wt32-eth01", "PlatformIO board must be wt32-eth01")
need("platformio.ini", 'FS7B_VERSION=\\"0.2.13-field-patch\\"', "firmware version must be v0.2.13 field patch")
for tok in ("FS7B_ETH_PHY_ADDR=1", "FS7B_ETH_MDC=23", "FS7B_ETH_MDIO=18", "FS7B_ETH_POWER=16"):
    need("platformio.ini", tok, f"missing WT32 build flag {tok}")
for tok in ("ETH_PHY_LAN8720", "ETH_CLOCK_GPIO0_IN", "ETH.begin("):
    need("src/NetworkManager.cpp", tok, f"missing LAN8720 Ethernet token {tok}")
forbid("src/NetworkManager.cpp", "ETH_PHY_W5500", "WT32 build must not initialize W5500")
need("src/SonyRemote.cpp", "WiFi.localIP()", "Sony sockets must remain Wi-Fi source-bound")
need("src/SonyRemote.cpp", "bind(fd", "Sony sockets must remain explicitly bound")
need("src/WebApp.cpp", 'server_.on("/api/v1/system"', "missing system diagnostics endpoint")
need("src/WebApp.cpp", 'server_.on("/api/v1/update"', "missing firmware upload endpoint")
need("src/WebApp.cpp", "Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)", "OTA must target application partition only")
need("include/OperationArbiter.h", "FirmwareUpdate", "OTA must participate in operation arbitration")
need("src/WebPages.h", "Firmware &amp; System", "Settings page missing firmware/system section")
need("src/WebPages.h", "uploadProgress", "Settings page missing OTA progress UI")
need("include/AppTypes.h", "bool ethernetDhcp = true;", "Ethernet must default to DHCP")
need("include/AppTypes.h", 'String ethernetIp = "10.77.7.2"', "static recovery profile must remain 10.77.7.2")
need("src/ConfigStore.cpp", "isLegacyDirectEthernetDefault", "legacy 10.77.7.2 default must migrate narrowly to DHCP")
need("src/NetworkManager.cpp", "no automatic static fallback is enabled", "DHCP mode must not silently invent a fallback address")
need("src/ConfigStore.cpp", 'setLinearMapping(commands, "record_start", "Clip.Recorder.Start"', "evidence-backed Sony /linear record mapping is missing")
need("src/ConfigStore.cpp", 'setLinearMapping(commands, "record_toggle", "Button.SendKeys"', "evidence-backed Sony REC key mapping is missing")
need("src/SonyLinear.cpp", 'Origin: http://', "FS7 WebSocket handshake must include browser-origin header")
need("src/SonyLinear.cpp", 'Authorization: Basic ', "FS7 WebSocket handshake must include configured HTTP Basic credentials")
need("src/SonyLinear.cpp", 'GET /linear HTTP/1.1', "Sony /linear WebSocket handshake is missing")
need("src/SonyLinear.cpp", 'Property.GetValue', "safe FS7-native Sony /linear test RPC is missing")
need("src/SonyLinear.cpp", 'P.Clip.Mediabox.Status', "FS7-native recorder-status probe is missing")
forbid("src/SonyLinear.cpp", 'GET /cgi-bin/getsavonacred.cgi HTTP/1.1', "FS7 build must not require the newer Savona credential bootstrap CGI")
forbid("src/SonyLinear.cpp", 'Alternate.Authentication.Basic', "FS7 native page does not issue an application-layer authentication RPC")
need("src/ConfigStore.cpp", 'setLinearMapping(commands, "record_stop", "Clip.Recorder.Stop", "[]"', "FS7-native exact recorder stop mapping is missing")
need("src/ConfigStore.cpp", 'setLinearMapping(commands, "rec_review", "Button.SendKeys", "[[\\"Thumbnail\\"]]"', "FS7-native Thumbnail Rec Review mapping is missing")
need("src/ConfigStore.cpp", 'setLinearMapping(commands, "awb", "Process.Execute.AutomaticAdjustment"', "FS7-native Auto White mapping is missing")
need("src/CommandEngine.cpp", 'runMapped("cursor_set")', "FS7-native Rec Review Set follow-up is missing")
need("src/CommandEngine.cpp", "delay(store_.config().recReviewSetDelayMs)", "Rec Review Set delay must use persisted configuration")
need("src/CommandEngine.cpp", "Timed out waiting for Sony RPC response", "Thumbnail readiness must distinguish an ignored Set")
need("src/CommandEngine.cpp", "P.Clip.Mediabox.Status", "latest-clip playback must verify the camera status")
need("src/SonyRemote.cpp", 'rawRequest("GET", "/rm.html"', "camera-session /rm.html warmup is missing")
need("src/WebApp.cpp", 'text/html; charset=utf-8', "bridge HTML must explicitly declare UTF-8")
need("src/SonyProxy.cpp", "SESSION_SLOT_WAIT_MS = 1500", "Sony native proxy must queue brief asset bursts instead of immediately rejecting them")
need("src/ConfigStore.cpp", 's["enabled"] = false', "telemetry sources must remain disabled by default")
need("src/ConfigStore.cpp", 'seq["enabled"] = false', "sequences must remain disabled by default")

if checks:
    print("WT32/OTA VALIDATION FAILED")
    for c in checks:
        print(" -", c)
    sys.exit(1)
print("WT32/OTA VALIDATION OK: LAN8720, DHCP, Wi-Fi source binding, OTA, FS7-native /linear transport, exact record start/stop mappings, and native-proxy backpressure.")
