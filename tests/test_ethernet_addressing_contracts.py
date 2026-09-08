"""DHCP/static Ethernet addressing contracts for v0.2.4."""
from __future__ import annotations
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
APP = (ROOT / "include/AppTypes.h").read_text(encoding="utf-8")
CFG = (ROOT / "src/ConfigStore.cpp").read_text(encoding="utf-8")
NET = (ROOT / "src/NetworkManager.cpp").read_text(encoding="utf-8")
NET_H = (ROOT / "src/NetworkManager.h").read_text(encoding="utf-8")
WEB = (ROOT / "src/WebApp.cpp").read_text(encoding="utf-8")
PAGES = (ROOT / "src/WebPages.h").read_text(encoding="utf-8")


class EthernetAddressingContracts(unittest.TestCase):
    def test_01_fresh_config_defaults_to_dhcp_with_static_recovery_profile(self):
        self.assertIn("bool ethernetDhcp = true;", APP)
        self.assertIn('String ethernetIp = "10.77.7.2";', APP)
        self.assertIn('String ethernetSubnet = "255.255.255.0";', APP)

    def test_02_only_exact_legacy_factory_static_profile_is_migrated(self):
        block = CFG[CFG.index("bool isLegacyDirectEthernetDefault"):CFG.index("bool validHost")]
        for token in (
            "!cfg.ethernetDhcp",
            'cfg.ethernetIp == "10.77.7.2"',
            'cfg.ethernetGateway == "0.0.0.0"',
            'cfg.ethernetSubnet == "255.255.255.0"',
            'cfg.ethernetDns == "0.0.0.0"',
        ):
            self.assertIn(token, block)
        self.assertIn("schemaVersion < CONFIG_SCHEMA_VERSION && isLegacyDirectEthernetDefault(loaded)", CFG)
        self.assertIn("if (migrateLegacyEthernet) loaded.ethernetDhcp = true;", CFG)
        self.assertIn('doc["configSchema"] = CONFIG_SCHEMA_VERSION;', CFG)

    def test_03_dhcp_has_no_hidden_static_fallback_and_static_mode_remains_available(self):
        self.assertIn('cfg_.ethernetDhcp ? "DHCP" : "STATIC"', NET)
        self.assertIn("if (!cfg_.ethernetDhcp)", NET)
        self.assertIn("ETH.config(ip, gw, mask, dns)", NET)
        self.assertIn("no automatic static fallback is enabled", NET)
        self.assertNotIn("millis() - dhcp", NET.lower())

    def test_04_runtime_diagnostics_report_lease_details_and_ui_disables_static_fields_in_dhcp(self):
        for token in ("ethernetSubnet()", "ethernetGateway()", "ethernetDns()"):
            self.assertIn(token, NET_H)
        for token in ('eth["addressing"]', 'eth["subnet"]', 'eth["gateway"]', 'eth["dns"]'):
            self.assertIn(token, WEB)
        self.assertIn("toggleEthernetFields()", PAGES)
        self.assertIn("DHCP — recommended", PAGES)
        self.assertIn("there is no automatic static fallback", PAGES)


if __name__ == "__main__":
    unittest.main()
