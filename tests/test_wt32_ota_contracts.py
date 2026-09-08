"""WT32-ETH01 + web OTA contracts for the v0.2.8 Sony native proxy build.

These tests preserve the hardware/OTA contracts while allowing only the
evidence-backed Sony /linear mappings introduced in v0.2.6.
"""
from __future__ import annotations

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")
NET = (ROOT / "src/NetworkManager.cpp").read_text(encoding="utf-8")
SONY = (ROOT / "src/SonyRemote.cpp").read_text(encoding="utf-8")
WEB = (ROOT / "src/WebApp.cpp").read_text(encoding="utf-8")
WEB_H = (ROOT / "src/WebApp.h").read_text(encoding="utf-8")
PAGES = (ROOT / "src/WebPages.h").read_text(encoding="utf-8")
ARB = (ROOT / "include/OperationArbiter.h").read_text(encoding="utf-8")
CFG = (ROOT / "src/ConfigStore.cpp").read_text(encoding="utf-8")
APP = (ROOT / "include/AppTypes.h").read_text(encoding="utf-8")


class Wt32OtaContractTests(unittest.TestCase):
    def test_01_platformio_targets_real_wt32_board(self):
        self.assertRegex(PIO, r"(?m)^default_envs\s*=\s*wt32-eth01\s*$")
        self.assertRegex(PIO, r"(?m)^board\s*=\s*wt32-eth01\s*$")
        self.assertIn('FS7B_VERSION=\\"0.2.12-integration-cleanup\\"', PIO)

    def test_02_lan8720_profile_is_exact_and_w5500_init_is_gone(self):
        for token in (
            "ETH_PHY_LAN8720",
            "FS7B_ETH_PHY_ADDR",
            "FS7B_ETH_MDC",
            "FS7B_ETH_MDIO",
            "FS7B_ETH_POWER",
            "ETH_CLOCK_GPIO0_IN",
        ):
            self.assertIn(token, NET)
        self.assertNotIn("ETH_PHY_W5500", NET)
        self.assertIn("-D FS7B_ETH_PHY_ADDR=1", PIO)
        self.assertIn("-D FS7B_ETH_MDC=23", PIO)
        self.assertIn("-D FS7B_ETH_MDIO=18", PIO)
        self.assertIn("-D FS7B_ETH_POWER=16", PIO)

    def test_03_dhcp_default_recovery_profile_and_wifi_binding_survive(self):
        self.assertIn("bool ethernetDhcp = true;", APP)
        self.assertIn('String ethernetIp = "10.77.7.2"', APP)
        self.assertIn("isLegacyDirectEthernetDefault", CFG)
        self.assertIn("no automatic static fallback is enabled", NET)
        self.assertIn("WiFi.localIP()", SONY)
        self.assertIn("bind(fd", SONY)

    def test_04_ota_is_application_only_and_uses_shared_operation_owner(self):
        self.assertIn("OperationOwner::FirmwareUpdate", WEB)
        self.assertIn("FirmwareUpdate", ARB)
        self.assertIn("Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)", WEB)
        self.assertNotIn("U_LITTLEFS", WEB)
        self.assertNotIn("U_SPIFFS", WEB)

    def test_05_interrupted_ota_has_abort_and_timeout_release_paths(self):
        self.assertIn("UPLOAD_FILE_ABORTED", WEB)
        self.assertIn("Update.abort()", WEB)
        self.assertIn("120000U", WEB)
        self.assertIn("arbiter_.release(OperationOwner::FirmwareUpdate)", WEB)
        self.assertIn("otaLastActivityMs_", WEB_H)

    def test_06_only_success_schedules_reboot(self):
        success_block = WEB[WEB.index("if (upload.status == UPLOAD_FILE_END)"):WEB.index("void WebApp::finishFirmwareUploadRequest")]
        self.assertIn("Update.end(true)", success_block)
        self.assertIn("otaSuccess_ = true", success_block)
        self.assertIn("otaRebootPending_ = true", success_block)
        fail_block = WEB[WEB.index("void WebApp::failFirmwareUpload"):WEB.index("void WebApp::handleFirmwareUpload")]
        self.assertIn("otaRebootPending_ = false", fail_block)

    def test_07_settings_has_system_diagnostics_and_progress_ota(self):
        for token in (
            "Firmware &amp; System",
            "/api/v1/system",
            "/api/v1/update",
            "uploadProgress",
            "firmware.bin",
            "LittleFS",
            "next OTA capacity",
        ):
            self.assertIn(token, PAGES)

    def test_08_system_endpoint_reports_partition_memory_and_network(self):
        for token in (
            "ESP.getFreeHeap()",
            "ESP.getMinFreeHeap()",
            "ESP.getFreeSketchSpace()",
            "esp_ota_get_running_partition()",
            "esp_ota_get_next_update_partition(nullptr)",
            "LittleFS.totalBytes()",
            "ethernetLinkUp()",
        ):
            self.assertIn(token, WEB)

    def test_09_partition_layout_stays_ota_capable_min_spiffs(self):
        self.assertIn("board_build.partitions = min_spiffs.csv", PIO)

    def test_10_http_discovery_stays_blank_and_telemetry_disabled(self):
        # HTTP discovery slots remain blank; only separately marked /linear mappings are populated.
        self.assertIn('m["method"] = "POST"; m["path"] = "";', CFG)
        self.assertRegex(CFG, r's\["enabled"\]\s*=\s*false')
        # Every default sequence stays disabled, including the template.
        self.assertIn('seq["enabled"] = false', CFG)


if __name__ == "__main__":
    unittest.main()
