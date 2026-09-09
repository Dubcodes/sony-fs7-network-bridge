"""Regression contract for the final VSM default/selector cleanup."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CFG = (ROOT / "src/ConfigStore.cpp").read_text(encoding="utf-8")
PAGES = (ROOT / "src/WebPages.h").read_text(encoding="utf-8")
CSV = (ROOT / "vsm/control-tree-reference.csv").read_text(encoding="utf-8")


class VsmDefaultCleanupContracts(unittest.TestCase):
    def test_fresh_defaults_and_selector_exclude_unqualified_dead_ends(self):
        defaults = CFG[CFG.index("const char *defaultCommands[16]"):CFG.index("for (int i = 0; i < 16", CFG.index("const char *defaultCommands[16]"))]
        for command in ("record_start", "stop_replay", "record_stop", "rec_review", "awb", "play", "pause", "stop_playback"):
            self.assertIn(f'"{command}"', defaults)
        for command in ("auto_iris", "iris_up", "iris_down", "gain_up", "gain_down", "nd_up", "nd_down"):
            self.assertNotIn(f'"{command}"', defaults)
        for slot in range(9, 17):
            self.assertIn(f"Trigger,Trigger{slot:02d},Spare,,Unmapped by default", CSV)
        self.assertIn("catalog.commands.filter(c=>c.configured&&!c.holdOnly)", PAGES)
        self.assertIn("unavailable (preserved)", PAGES)
        self.assertIn("if (!readFile(VSM_GENERIC_PATH, raw)) return false;", CFG)
        self.assertIn("canonicalizeVsmGenericMap(input.as<JsonObjectConst>(), clean)", CFG)


if __name__ == "__main__":
    unittest.main()
