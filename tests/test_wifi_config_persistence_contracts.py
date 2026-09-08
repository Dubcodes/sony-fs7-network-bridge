import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGES = (ROOT / 'src' / 'WebPages.h').read_text(encoding='utf-8')
STORE = (ROOT / 'src' / 'ConfigStore.cpp').read_text(encoding='utf-8')
APP = (ROOT / 'include' / 'AppTypes.h').read_text(encoding='utf-8')
PLATFORM = (ROOT / 'platformio.ini').read_text(encoding='utf-8')


class WifiConfigPersistenceContracts(unittest.TestCase):
    def test_01_reconnect_saves_and_verifies_before_reconnect(self):
        self.assertIn('async function saveSetup(showMessage=true)', PAGES)
        self.assertIn("const verify=await j('/api/v1/config')", PAGES)
        self.assertIn("Save verification failed: camera SSID did not persist", PAGES)
        reconnect = PAGES.split('async function reconnect(){', 1)[1].split('}async function test()', 1)[0]
        self.assertIn('await saveSetup(false)', reconnect)
        self.assertIn("j('/api/v1/reconnect-camera',{method:'POST'})", reconnect)
        self.assertLess(reconnect.index('await saveSetup(false)'), reconnect.index("j('/api/v1/reconnect-camera'"))

    def test_02_password_autofill_is_suppressed_and_wifi_password_not_faked(self):
        self.assertIn('id="wifiPass" type="password" autocomplete="new-password"', PAGES)
        self.assertIn("g('wifiPass').value=''", PAGES)
        self.assertIn("c.camera.wifiPassword==='********'?'configured — leave blank to keep':'enter camera Wi-Fi password'", PAGES)

    def test_03_sony_default_basic_auth_password_is_real_config_and_visible_only_when_default(self):
        self.assertIn('String cameraPassword = "pxw-fs7";', APP)
        self.assertIn('loaded.cameraPassword = camera["password"] | "pxw-fs7";', STORE)
        self.assertIn('camera["passwordIsDefault"] = cfg_.cameraPassword == "pxw-fs7";', STORE)
        self.assertIn("g('cameraPass').value=c.camera.passwordIsDefault?'pxw-fs7':''", PAGES)
        self.assertIn('autocomplete="new-password"', PAGES)

    def test_04_release_identity_advanced(self):
        self.assertIn('FS7B_VERSION=\\"0.2.12-integration-cleanup\\"', PLATFORM)


if __name__ == '__main__':
    unittest.main()
