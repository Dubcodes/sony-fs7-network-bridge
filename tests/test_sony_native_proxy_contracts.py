import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CPP = (ROOT / "src" / "SonyProxy.cpp").read_text(encoding="utf-8")
HDR = (ROOT / "src" / "SonyProxy.h").read_text(encoding="utf-8")
WEB = (ROOT / "src" / "WebApp.cpp").read_text(encoding="utf-8")
PAGES = (ROOT / "src" / "WebPages.h").read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
PIO = (ROOT / "platformio.ini").read_text(encoding="utf-8")

class SonyNativeProxyContracts(unittest.TestCase):
    def test_version_and_listener(self):
        self.assertIn('FS7B_VERSION=\\"0.2.12-integration-cleanup\\"', PIO)
        self.assertIn('constexpr uint16_t PROXY_PORT = 8081', CPP)
        self.assertIn('sonyProxy.begin();', MAIN)

    def test_camera_side_is_wifi_source_bound(self):
        self.assertIn('WiFi.localIP().toString()', CPP)
        self.assertIn('bind(fd, reinterpret_cast<struct sockaddr *>(&local)', CPP)

    def test_proxy_rewrites_only_network_identity_headers(self):
        self.assertIn('Authorization: Basic', CPP)
        self.assertIn('Origin: http://', CPP)
        self.assertIn('Host: " + cameraHost', CPP)
        self.assertIn('Connection: Upgrade', CPP)
        self.assertIn('Connection: close', CPP)


    def test_native_asset_bursts_queue_instead_of_immediate_503(self):
        self.assertIn('LISTEN_BACKLOG = 8', CPP)
        self.assertIn('SESSION_SLOT_WAIT_MS = 1500', CPP)
        self.assertIn('while (!claimed && millis() - waitStarted < SESSION_SLOT_WAIT_MS)', CPP)
        self.assertIn('vTaskDelay(pdMS_TO_TICKS(10))', CPP)

    def test_websocket_is_raw_bidirectional_tunnel(self):
        self.assertIn('FD_ISSET(clientFd', CPP)
        self.assertIn('sendAll(cameraFd, buf', CPP)
        self.assertIn('FD_ISSET(cameraFd', CPP)
        self.assertIn('sendAll(clientFd, buf', CPP)

    def test_capture_is_opt_in_ram_only_and_bounded(self):
        self.assertIn('volatile bool captureEnabled_ = false', HDR)
        self.assertIn('std::array<CaptureEntry, 16>', HDR)
        self.assertIn('MAX_CAPTURE_BYTES = 256', CPP)
        self.assertNotIn('LittleFS', CPP)
        self.assertIn('if (!fromBrowser) return;', CPP)

    def test_capture_api_and_native_page_exist(self):
        for path in ['/api/v1/sony/capture', '/api/v1/sony/capture/start', '/api/v1/sony/capture/stop', '/api/v1/sony/capture/clear']:
            self.assertIn(path, WEB)
        self.assertIn('/sony-native', WEB)
        self.assertIn('Open Sony /rm.html', PAGES)
        self.assertIn(':8081/rm.html', PAGES)
        self.assertIn('Download JSON', PAGES)

if __name__ == '__main__':
    unittest.main()
