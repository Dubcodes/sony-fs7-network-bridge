import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LINEAR = (ROOT / 'src' / 'SonyLinear.cpp').read_text(encoding='utf-8')
REMOTE = (ROOT / 'src' / 'SonyRemote.cpp').read_text(encoding='utf-8')
STORE = (ROOT / 'src' / 'ConfigStore.cpp').read_text(encoding='utf-8')
ENGINE = (ROOT / 'src' / 'CommandEngine.cpp').read_text(encoding='utf-8')
WEB = (ROOT / 'src' / 'WebApp.cpp').read_text(encoding='utf-8')
PAGES = (ROOT / 'src' / 'WebPages.h').read_text(encoding='utf-8')
APP = (ROOT / 'include' / 'AppTypes.h').read_text(encoding='utf-8')


class SonyLinearContracts(unittest.TestCase):
    def test_01_transport_bootstrap_supports_fs7_legacy_and_newer_savona_flow(self):
        self.assertNotIn('GET /cgi-bin/getsavonacred.cgi HTTP/1.1', LINEAR)
        self.assertNotIn('Alternate.Authentication.Basic', LINEAR)
        self.assertIn('GET /linear HTTP/1.1', LINEAR)
        self.assertIn('Origin: http://', LINEAR)
        self.assertIn('Authorization: Basic ', LINEAR)
        self.assertIn('Property.GetValue', LINEAR)
        self.assertIn('P.Clip.Mediabox.Status', LINEAR)
        self.assertIn('Sec-WebSocket-Accept', LINEAR)
        self.assertIn('SHA1Builder', LINEAR)

    def test_02_linear_sockets_are_explicitly_source_bound_to_camera_wifi(self):
        self.assertIn('WiFi.localIP()', LINEAR)
        self.assertIn('bind(fd', LINEAR)
        self.assertIn('ESP32 is not connected to the FS7 Wi-Fi network', LINEAR)

    def test_03_rpc_envelope_is_binary_messagepack_request(self):
        # 0x94 = fixarray(4), followed by type 0; method and params are encoded after id.
        self.assertIn('out.push_back(0x94)', LINEAR)
        self.assertIn('out.push_back(0x00)', LINEAR)
        self.assertIn('encodeString(out, method.c_str()', LINEAR)
        self.assertIn('encodeVariant(out, paramsDoc.as<JsonVariantConst>()', LINEAR)
        self.assertIn('sendWsFrame(fd, 0x2', LINEAR)
        self.assertIn('mask[4]', LINEAR)  # clients MUST mask WebSocket frames

    def test_04_notifications_do_not_break_matching_response_wait(self):
        self.assertIn('if (type != 1) return true;', LINEAR)
        self.assertIn('if (id != wantedId) return true;', LINEAR)
        self.assertIn('Sony /linear notifications use a different envelope', LINEAR)

    def test_05_exact_public_record_and_rec_key_mappings_are_built_in(self):
        self.assertIn('setLinearMapping(commands, "record_start", "Clip.Recorder.Start", "[]"', STORE)
        self.assertIn('setLinearMapping(commands, "record_toggle", "Button.SendKeys", "[[\\"Rec\\"]]"', STORE)
        self.assertIn('setLinearMapping(commands, "record_stop", "Clip.Recorder.Stop", "[]"', STORE)
        self.assertIn('"fs7-native-rmt-exact"', STORE)

    def test_06_fs7_playback_and_assign_keys_are_evidence_limited(self):
        self.assertIn('setLinearMapping(commands, "play", "Button.SendKeys", "[[\\"Play\\"]]"', STORE)
        self.assertIn('setLinearMapping(commands, "stop_playback", "Button.SendKeys", "[[\\"Stop\\"]]"', STORE)
        self.assertIn('"Assignable." + String(i)', STORE)
        self.assertIn('Bridge follows Thumbnail with Set to play the latest clip', STORE)
        self.assertIn('setLinearMapping(commands, "rec_review", "Button.SendKeys", "[[\\"Thumbnail\\"]]"', STORE)
        self.assertIn('mapping.rpcParams == "[[\\"Thumbnail\\"]]"', ENGINE)
        self.assertIn('runMapped("cursor_set")', ENGINE)
        self.assertIn('Timed out waiting for Sony RPC response', ENGINE)
        self.assertIn('waitForPlayback', ENGINE)

    def test_07_native_awb_is_exact_but_other_parameter_controls_remain_evidence_limited(self):
        default_section = STORE.split('void applyPublicFs7LinearDefaults', 1)[1].split('bool configSane', 1)[0]
        self.assertIn('setLinearMapping(commands, "awb", "Process.Execute.AutomaticAdjustment"', default_section)
        self.assertIn('[[\\"Camera.WhiteBalance\\"]]', default_section)
        self.assertNotIn('setLinearMapping(commands, "iris_up"', default_section)
        self.assertNotIn('setLinearMapping(commands, "gain_up"', default_section)

    def test_08_map_schema_migrates_only_known_legacy_defaults(self):
        self.assertIn('COMMAND_SCHEMA_VERSION = 5', STORE)
        self.assertIn('migrateFs7NativeRecordMappingsV3', STORE)
        self.assertIn('migrateFs7NativeControlMappingsV4', STORE)
        self.assertIn('migrateFs7NativeWireMappingsV5', STORE)
        self.assertIn('applyPublicFs7LinearDefaults(commands, true)', STORE)
        self.assertIn('path.length() == 0 && rpcMethod.length() == 0', STORE)
        self.assertIn('migrated evidence-backed Sony /linear command mappings', STORE)

    def test_09_command_engine_requires_rpc_success_not_http_101_only(self):
        self.assertIn('if (sonyResult.isLinear)', ENGINE)
        self.assertIn('if (!sonyResult.commandOk)', ENGINE)
        self.assertIn('Sony /linear RPC failed', ENGINE)
        self.assertIn('return linear_.request(mapping.rpcMethod, mapping.rpcParams)', REMOTE)

    def test_10_safe_linear_probe_and_mapping_ui_exist(self):
        self.assertIn('server_.on("/api/v1/sony/linear-test"', WEB)
        self.assertIn('sony_.testLinear()', WEB)
        self.assertIn('Test Sony /linear', PAGES)
        self.assertIn('Sony /linear WebSocket RPC', PAGES)
        self.assertIn('RPC params JSON', PAGES)

    def test_11_no_hardcoded_camera_or_savona_credentials(self):
        self.assertNotIn('admin111', LINEAR)
        self.assertNotIn('8c6976e5b5410415', LINEAR)
        self.assertNotIn('ffd33f5776607938', LINEAR)
        self.assertIn('cfg_.cameraUsername + ":" + cfg_.cameraPassword', LINEAR)
        self.assertNotIn('SHA2Builder', LINEAR)

    def test_12_backend_reuses_one_subscribed_linear_connection(self):
        self.assertIn('persistent subscribed /linear session established', LINEAR)
        self.assertIn('Notify.Subscribe', LINEAR)
        self.assertIn('if (fd_ >= 0 && connectedWifiIp_ == wifiIp)', LINEAR)
        self.assertNotIn('close(fd);\n  return result;', LINEAR)
        self.assertIn('sonyRemote.loop();', (ROOT / 'src' / 'main.cpp').read_text(encoding='utf-8'))

    def test_13_linear_connection_count_is_observable(self):
        self.assertIn('cam["backendLinearConnections"]', WEB)
        self.assertIn('cam["nativeProxyLinearConnections"]', WEB)
        self.assertIn('cam["linearConnections"]', WEB)

    def test_14_socket_timeout_is_normalized_to_rpc_timeout(self):
        self.assertIn('if (error == "Timed out waiting for WebSocket data") break;', LINEAR)
        self.assertIn('error = "Timed out waiting for Sony RPC response";', LINEAR)


if __name__ == '__main__':
    unittest.main()
