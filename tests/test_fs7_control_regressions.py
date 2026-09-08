import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG = (ROOT / "src" / "ConfigStore.cpp").read_text(encoding="utf-8")
ENGINE = (ROOT / "src" / "CommandEngine.cpp").read_text(encoding="utf-8")
WEB = (ROOT / "src" / "WebApp.cpp").read_text(encoding="utf-8")
PAGES = (ROOT / "src" / "WebPages.h").read_text(encoding="utf-8")
PIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")

class Fs7ControlRegressionTests(unittest.TestCase):
    def test_01_utf8_is_explicit_for_bridge_pages(self):
        self.assertIn('text/html; charset=utf-8', WEB)
        self.assertIn('application/json; charset=utf-8', WEB)
        self.assertGreaterEqual(PAGES.count('<meta charset="utf-8">'), 8)

    def test_02_rec_review_no_longer_depends_on_assignable_6(self):
        self.assertIn('[[\\"Thumbnail\\"]]', CFG)
        self.assertIn('[[\\"Set\\"]]', ENGINE)
        self.assertIn('migrateFs7NativeControlMappingsV4', CFG)
        self.assertNotIn('Requires FS7 Assignable Button 6 = Rec Review', CFG)

    def test_03_auto_white_uses_native_fs7_process_rpc(self):
        self.assertIn('Process.Execute.AutomaticAdjustment', CFG)
        self.assertIn('Camera.WhiteBalance', CFG)

    def test_04_version_is_controls_release(self):
        self.assertIn('FS7B_VERSION=\\"0.2.12-integration-cleanup\\"', PIO)

if __name__ == '__main__':
    unittest.main()
