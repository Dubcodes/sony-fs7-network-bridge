import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGES = (ROOT / "src" / "WebPages.h").read_text(encoding="utf-8")
WEB = (ROOT / "src" / "WebApp.cpp").read_text(encoding="utf-8")
PROXY = (ROOT / "src" / "SonyProxy.cpp").read_text(encoding="utf-8")
PIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")

class OperatorConsoleContracts(unittest.TestCase):
    def test_01_operator_route_and_navigation_exist(self):
        self.assertIn('server_.on("/operator"', WEB)
        self.assertIn('OPERATOR_HTML', WEB)
        self.assertGreaterEqual(PAGES.count('href="/operator"'), 9)
        self.assertIn('FS7B_VERSION=\\"0.2.12-integration-cleanup\\"', PIO)

    def test_02_operator_uses_sony_native_savona_over_proxy(self):
        self.assertIn('/WebCommon/javascript/savona.min.js', PAGES)
        self.assertIn("port:8081,path:'linear'", PAGES)
        self.assertIn("Notify.Properties", PAGES)
        self.assertIn("Notify.Property.Value.Changed", PAGES)
        self.assertIn("P.Clip.Mediabox.Status", PAGES)
        self.assertIn("Camera.WhiteBalance.Mode", PAGES)
        self.assertIn("Camera.Shutter.Value", PAGES)
        self.assertIn("Camera.SlowAndQuickMotion.FrameRate", PAGES)
        self.assertIn("Paint.Gamma.Value", PAGES)
        self.assertIn("Camera.Focus.Distance", PAGES)

    def test_03_programme_controls_and_backup_are_present(self):
        for token in ("record_start", "stop_replay", "rec_review", "Assignable.5",
                      "Camera.WhiteBalance", "Camera.BlackBalance", "nativeUrl('/rmt.html')"):
            self.assertIn(token, PAGES)

    def test_04_keyboard_controls_and_input_guard_are_present(self):
        self.assertIn("editableTarget(e.target)", PAGES)
        self.assertIn("e.ctrlKey||e.metaKey||e.altKey", PAGES)
        for token in ("arrowup", "arrowdown", "arrowleft", "arrowright",
                      "sendKey('Set')", "sendKey('Cancel')", "sendKey('Thumbnail')",
                      "sendKey('Assignable.'+k)"):
            self.assertIn(token, PAGES)
        self.assertIn("if(e.repeat)return", PAGES)

    def test_05_confirmed_property_controls_are_available(self):
        for token in ("Camera.SlowAndQuickMotion.Enabled", "Camera.Shutter.SettingMethod",
                      "Camera.Gain.SettingMethod", "Camera.Shutter.Value",
                      "Camera.Focus.Velocity", "process.execute.AutomaticAdjustment"):
            self.assertIn(token, PAGES)

    def test_06_focus_hold_has_release_safety(self):
        self.assertIn("window.addEventListener('blur',endFocus)", PAGES)
        self.assertIn("visibilitychange", PAGES)
        self.assertIn("focusVelocity(0)", PAGES)

    def test_07_capture_filters_menu_housekeeping_spam(self):
        self.assertIn('P.Menu.pmw-f5x.Menu.Opened', PROXY)
        self.assertIn('std::search(payload.begin()', PROXY)

if __name__ == '__main__':
    unittest.main()
