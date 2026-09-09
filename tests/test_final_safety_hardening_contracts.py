"""Contracts for hold-only commands, canonical VSM maps, and link truth."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
APP_H = (ROOT / "include/AppTypes.h").read_text(encoding="utf-8")
APP = (ROOT / "src/AppTypes.cpp").read_text(encoding="utf-8")
CFG = (ROOT / "src/ConfigStore.cpp").read_text(encoding="utf-8")
SEQ = (ROOT / "src/SequenceEngine.cpp").read_text(encoding="utf-8")
VSM = (ROOT / "src/VsmGeneric.cpp").read_text(encoding="utf-8")
WEB = (ROOT / "src/WebApp.cpp").read_text(encoding="utf-8")
PAGES = (ROOT / "src/WebPages.h").read_text(encoding="utf-8")
LINEAR = (ROOT / "src/SonyLinear.cpp").read_text(encoding="utf-8")
API = (ROOT / "docs/API.md").read_text(encoding="utf-8")


class FinalSafetyHardeningContracts(unittest.TestCase):
    def test_hold_only_classification_is_central_and_enforced(self):
        self.assertIn("bool isHoldOnlyCommand(const String &id);", APP_H)
        helper = APP[APP.index("bool isHoldOnlyCommand"):APP.index("const std::array<CameraStateDescriptor")]
        for command in ("focus_near", "focus_far", "zoom_in", "zoom_out"):
            self.assertIn(f'id == "{command}"', helper)
        self.assertNotIn("focus_stop", helper)
        self.assertNotIn("zoom_stop", helper)
        self.assertIn('o["holdOnly"] = isHoldOnlyCommand(c.id);', WEB)
        self.assertIn("catalog.commands.filter(c=>c.configured&&!c.holdOnly)", PAGES)
        self.assertIn("catalog.commands.filter(x=>x.configured&&!x.holdOnly", PAGES)
        self.assertIn("commands=c.commands.filter(x=>!x.holdOnly)", PAGES)
        self.assertIn("if(!c||c.holdOnly)return", PAGES)
        self.assertIn("if (isHoldOnlyCommand(command))", CFG)
        self.assertIn("if (isHoldOnlyCommand(step.command))", SEQ)
        self.assertIn("if (isHoldOnlyCommand(command))", VSM)
        self.assertIn("return {false, 409", VSM)
        self.assertIn("responsible for issuing the matching", API)

    def test_empty_or_short_vsm_maps_are_canonicalized_to_fixed_shape(self):
        helper = CFG[CFG.index("void canonicalizeVsmGenericMap"):CFG.index("bool validHttpMethod")]
        self.assertIn("for (int i = 0; i < 16; ++i)", helper)
        self.assertIn('const char *groups[] = {"bools", "ints", "floats", "texts"};', helper)
        self.assertIn("for (int i = 0; i < 8; ++i)", helper)
        self.assertIn('existing["label"].is<const char *>()', helper)
        self.assertIn('existing["command"].as<String>()', helper)
        self.assertIn('existing["source"].as<String>()', helper)
        load = CFG[CFG.index("bool ConfigStore::loadVsmGenericMap"):CFG.index("void ConfigStore::setDefaultSequences")]
        self.assertGreaterEqual(load.count("canonicalizeVsmGenericMap"), 2)
        self.assertNotIn("Missing array:", load)

    def test_closing_persistent_linear_session_clears_reachability(self):
        close = LINEAR[LINEAR.index("void SonyLinear::closeConnection()"):LINEAR.index("void SonyLinear::applyProperty")]
        self.assertIn("fd_ = -1;", close)
        self.assertIn("if (status_) status_->cameraReachable = false;", close)


if __name__ == "__main__":
    unittest.main()
