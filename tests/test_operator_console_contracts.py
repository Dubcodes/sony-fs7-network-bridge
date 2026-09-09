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
        self.assertIn('FS7B_VERSION=\\"0.2.13-field-patch\\"', PIO)

    def test_02_operator_uses_authoritative_backend_state_without_duplicate_savona(self):
        operator = PAGES.split('static const char OPERATOR_HTML', 1)[1].split(
            'static const char LAYOUT_HTML', 1)[0]
        self.assertNotIn('/WebCommon/javascript/savona.min.js', operator)
        self.assertNotIn("port:8081,path:'linear'", operator)
        self.assertIn("jsonFetch('/api/v1/camera-state')", operator)
        self.assertIn("s.camera&&s.camera.linearConnected", operator)
        self.assertIn("P.Clip.Mediabox.Status", PAGES)
        self.assertIn("Camera.WhiteBalance.Mode", PAGES)
        self.assertIn("Camera.Shutter.Value", PAGES)
        self.assertIn("Camera.SlowAndQuickMotion.FrameRate", PAGES)
        self.assertIn("Paint.Gamma.Value", PAGES)
        self.assertIn("Camera.Focus.Distance", PAGES)

    def test_03_programme_controls_and_backup_are_present(self):
        for token in ("record_start", "stop_replay", "rec_review", "assign_5",
                      "awb", "abb", "nativeUrl('/rmt.html')"):
            self.assertIn(token, PAGES)

    def test_04_keyboard_controls_and_input_guard_are_present(self):
        self.assertIn("editableTarget(e.target)", PAGES)
        self.assertIn("e.ctrlKey||e.metaKey||e.altKey", PAGES)
        for token in ("arrowup", "arrowdown", "arrowleft", "arrowright",
                      "'enter':'cursor_set'", "'escape':'cancel'", "'t':'thumbnail'",
                      "'assign_'+k"):
            self.assertIn(token, PAGES)
        self.assertIn("if(!down||e.repeat)return", PAGES)

    def test_05_confirmed_property_controls_are_available(self):
        for token in ("Camera.SlowAndQuickMotion.Enabled", "Camera.Shutter.SettingMethod",
                      "Camera.Gain.SettingMethod", "Camera.Shutter.Value",
                      "id=\"focusNear\"", "data-command=\"awb\""):
            self.assertIn(token, PAGES)

    def test_06_focus_hold_has_release_safety(self):
        self.assertIn("window.addEventListener('blur',endLens)", PAGES)
        self.assertIn("visibilitychange", PAGES)
        self.assertIn("'focus_stop'", PAGES)
        self.assertIn("'zoom_stop'", PAGES)
        self.assertIn("ZOOM_REPEAT_MS=100", PAGES)
        self.assertIn("lensRepeatTimer=setTimeout(move,Math.max(0,ZOOM_REPEAT_MS-", PAGES)
        self.assertIn("clearTimeout(lensRepeatTimer)", PAGES)
        self.assertIn("lensRpcChain=lensRpcChain.then", PAGES)

    def test_07_h_and_j_hold_zoom_until_keyup(self):
        self.assertIn("h:['zoom',-1]", PAGES)
        self.assertIn("j:['zoom',1]", PAGES)
        self.assertIn("if(down){if(!e.repeat)beginLens(...lens)}else if(heldLens===held)endLens()", PAGES)
        self.assertIn("if(heldLens===held)return", PAGES)

    def test_08_one_shot_feedback_is_immediate_and_camera_refresh_is_bounded(self):
        self.assertIn("async function bridgeCommand(id,label){say(label+'…');", PAGES)
        self.assertIn("bridgeTimer=setInterval(pollBridge,500)", PAGES)
        self.assertNotIn("refreshTimer=setInterval(refreshCamera,1800)", PAGES)

    def test_09_capture_filters_menu_housekeeping_spam(self):
        self.assertIn('P.Menu.pmw-f5x.Menu.Opened', PROXY)
        self.assertIn('std::search(payload.begin()', PROXY)

if __name__ == '__main__':
    unittest.main()
