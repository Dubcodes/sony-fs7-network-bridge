"""Focused contracts for the 0.2.13 Ethernet and Stop + Replay field patch."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / "include/AppTypes.h").read_text(encoding="utf-8")
CFG = (ROOT / "src/ConfigStore.cpp").read_text(encoding="utf-8")
ENGINE = (ROOT / "src/CommandEngine.cpp").read_text(encoding="utf-8")
NET = (ROOT / "src/NetworkManager.cpp").read_text(encoding="utf-8")
PAGES = (ROOT / "src/WebPages.h").read_text(encoding="utf-8")
WEB = (ROOT / "src/WebApp.cpp").read_text(encoding="utf-8")
TCP = (ROOT / "src/ControlProtocol.cpp").read_text(encoding="utf-8")
VSM = (ROOT / "vsm/control-tree-reference.csv").read_text(encoding="utf-8")


class FieldPatchContracts(unittest.TestCase):
    def test_01_old_configs_and_fresh_installs_default_to_dhcp(self):
        self.assertIn("bool ethernetDhcp = true;", APP)
        self.assertIn('loaded.ethernetDhcp = eth["dhcp"] | true;', CFG)

    def test_01b_fresh_hostname_is_network_safe_without_overwriting_saved_names(self):
        self.assertIn('String deviceName = "FS7-WiFi-Bridge";', APP)
        self.assertIn('loaded.deviceName = doc["deviceName"] | "FS7-WiFi-Bridge";', CFG)
        self.assertIn("ETH.setHostname(cfg_.deviceName.c_str())", NET)

    def test_02_all_ethernet_fields_are_loaded_updated_and_persisted(self):
        for field in ("dhcp", "ip", "subnet", "gateway", "dns"):
            self.assertIn(f'eth["{field}"]', CFG)
        self.assertIn("BridgeConfig next = cfg_;", CFG)
        self.assertIn("if (!saveConfigValue(next))", CFG)
        self.assertIn("cfg_ = next;", CFG)

    def test_03_static_address_validation_rejects_bad_ip_and_masks(self):
        self.assertIn("validStaticEthernet", CFG)
        self.assertIn("address.fromString(cfg.ethernetIp)", CFG)
        self.assertIn("subnet.fromString(cfg.ethernetSubnet)", CFG)
        self.assertIn("(inverseMask & (inverseMask + 1U)) != 0", CFG)
        self.assertIn("host == 0 || host == inverseMask", CFG)

    def test_04_recovery_profile_and_exact_legacy_migration_remain(self):
        for token in ('"10.77.7.2"', '"255.255.255.0"', "isLegacyDirectEthernetDefault"):
            self.assertIn(token, CFG + APP)
        self.assertIn("if (migrateLegacyEthernet) loaded.ethernetDhcp = true;", CFG)

    def test_05_static_configuration_targets_ethernet_not_camera_wifi(self):
        static_block = NET[NET.index("if (!cfg_.ethernetDhcp)"):NET.index("} else {", NET.index("if (!cfg_.ethernetDhcp)"))]
        self.assertIn("ETH.config(ip, gw, mask, dns)", static_block)
        self.assertNotIn("WiFi.config", static_block)

    def test_06_dedicated_network_page_and_api_exist(self):
        self.assertIn('server_.on("/network", HTTP_GET', WEB)
        self.assertIn('server_.on("/api/v1/network-config", HTTP_GET', WEB)
        self.assertIn('server_.on("/api/v1/network-config", HTTP_POST', WEB)
        self.assertIn("Save Network Settings", PAGES)
        self.assertIn("Network changes take effect after reboot", PAGES)
        self.assertGreaterEqual(PAGES.count('href="/network"'), 10)

    def test_07_network_page_is_wired_only_and_validates_before_post(self):
        page = PAGES[PAGES.index("static const char NETWORK_HTML"):PAGES.index("static const char SETUP_HTML")]
        self.assertIn("function ipv4", page)
        self.assertIn("Save &amp; Reboot", page)
        self.assertNotIn("Camera SSID", page)
        self.assertNotIn("wifiPassword", page)

    def test_08_stop_replay_uses_recorder_stop_then_shared_latest_clip_path(self):
        self.assertIn('setLinearMapping(commands, "record_stop", "Clip.Recorder.Stop", "[]"', CFG)
        stop_block = ENGINE[ENGINE.index('if (id == "stop_replay")'):ENGINE.index("const bool transientManual")]
        self.assertIn('runMapped("record_stop", "record_toggle")', stop_block)
        self.assertIn("dispatchLatestClipPlayback()", ENGINE)
        self.assertIn('runMapped("rec_review")', ENGINE)
        self.assertIn('status_.macroPhase = "PLAYBACK_VERIFIED"', ENGINE)

    def test_09_latest_clip_keeps_ack_retry_explicit_play_and_playing_gate(self):
        review_start = ENGINE.index("// Live FS7 qualification")
        review = ENGINE[review_start:ENGINE.index("result.ok = true", review_start)]
        self.assertIn('runMapped("cursor_set")', review)
        self.assertIn("Timed out waiting for Sony RPC response", review)
        self.assertIn('runMapped("play")', review)
        self.assertNotIn('runMapped("pause")', review)
        self.assertIn("waitForPlayback(deadlineMs", review)
        self.assertIn("P.Clip.Mediabox.Status=Playing", ENGINE)
        self.assertIn('"play", "Button.SendKeys", "[[\\"Play\\"]]"', CFG)
        self.assertIn('"pause", "Button.SendKeys", "[[\\"Pause\\"]]"', CFG)

    def test_10_stop_replay_external_semantics_remain_compatible(self):
        self.assertIn('upper == "STOP_REPLAY") id = "stop_replay"', TCP)
        self.assertIn('const String commandPrefix = "/api/v1/command/"', WEB)
        self.assertIn("Stop + Replay,stop_replay", VSM)
        self.assertIn('"stop_replay"', CFG)


if __name__ == "__main__":
    unittest.main()
