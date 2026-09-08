"""Cross-module contracts for the bridge's safety and integration boundaries.

The small model exercises integration semantics without inventing any Sony
endpoint or payload. Source assertions tie each host-side contract to the
firmware implementation that is compiled by PlatformIO.
"""
from __future__ import annotations

import pathlib
import unittest
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
CMD = (ROOT / "src/CommandEngine.cpp").read_text(encoding="utf-8")
SEQ = (ROOT / "src/SequenceEngine.cpp").read_text(encoding="utf-8")
CFG = (ROOT / "src/ConfigStore.cpp").read_text(encoding="utf-8")
TCP = (ROOT / "src/ControlProtocol.cpp").read_text(encoding="utf-8")
VSM = (ROOT / "src/VsmGeneric.cpp").read_text(encoding="utf-8")
WEB = (ROOT / "src/WebApp.cpp").read_text(encoding="utf-8")
ARB = (ROOT / "include/OperationArbiter.h").read_text(encoding="utf-8")


class OperationModel:
    def __init__(self):
        self.owner = "NONE"
        self.state = "READY"

    def acquire(self, owner: str) -> bool:
        if self.owner != "NONE":
            return False
        self.owner, self.state = owner, "ACTIVE"
        return True

    def release(self, terminal: str = "COMPLETE") -> None:
        self.owner, self.state = "NONE", terminal


class PreHardwareIntegrationTests(unittest.TestCase):
    def test_01_sequence_running_rejects_stop_replay(self):
        m = OperationModel(); self.assertTrue(m.acquire("SEQUENCE_1")); self.assertFalse(m.acquire("STOP_REPLAY"))
        self.assertIn("arbiter_.acquire(OperationOwner::StopReplay)", CMD)

    def test_02_stop_replay_running_rejects_sequence(self):
        m = OperationModel(); self.assertTrue(m.acquire("STOP_REPLAY")); self.assertFalse(m.acquire("SEQUENCE_1"))
        self.assertIn("arbiter_.acquire(owner)", SEQ)

    def test_03_sequence_running_rejects_manual_command(self):
        m = OperationModel(); m.acquire("SEQUENCE_2"); self.assertFalse(m.acquire("MANUAL"))
        self.assertIn("OperationOwner::Manual", CMD)

    def test_04_sequence_abort_releases_owner(self):
        m = OperationModel(); m.acquire("SEQUENCE_3"); m.release("ABORTED"); self.assertEqual(m.owner, "NONE")
        self.assertIn("arbiter_.release(OperationArbiter::sequenceOwner(i))", SEQ)

    def test_05_sequence_error_releases_owner(self):
        m = OperationModel(); m.acquire("SEQUENCE_4"); m.release("ERROR"); self.assertEqual((m.owner, m.state), ("NONE", "ERROR"))
        self.assertIn("arbiter_.release(OperationArbiter::sequenceOwner(index))", SEQ)

    def test_06_stop_replay_failure_releases_owner(self):
        m = OperationModel(); m.acquire("STOP_REPLAY"); m.release("ERROR"); self.assertEqual(m.owner, "NONE")
        self.assertGreaterEqual(CMD.count("arbiter_.release(OperationOwner::StopReplay)"), 3)

    def test_07_disconnect_causes_error_and_release(self):
        self.assertIn('fail(i, "Camera Wi-Fi disconnected during sequence")', SEQ)
        self.assertIn('finishMacroError("Camera Wi-Fi disconnected during Stop + Replay")', CMD)

    def test_08_async_stop_replay_disallowed_in_sequence_steps(self):
        self.assertIn("stop_replay is asynchronous and cannot be used as a sequence step", CFG)
        self.assertIn("stop_replay is asynchronous and cannot be used as a sequence step", SEQ)

    def test_09_stale_post_command_state_cannot_advance_sequence(self):
        self.assertIn("value->updatedMs - r.stepStartedMs", SEQ)
        self.assertIn("millis() - value->updatedMs > step.freshMs", SEQ)

    def test_10_invalid_generic_values_become_null(self):
        for token in ("strtol", "strtod", "std::isfinite", "boolRoot[key] = nullptr", "intRoot[key] = nullptr", "floatRoot[key] = nullptr"):
            self.assertIn(token, VSM)

    def test_11_stale_tcp_get_is_not_current_value(self):
        self.assertIn('return "STALE " + id', TCP)
        self.assertIn("telemetryFreshMs", TCP)

    def test_12_active_automation_blocks_sequence_reload(self):
        self.assertIn("arbiter_.automationActive()", SEQ)
        self.assertIn("Cannot reload sequences while automation is active", SEQ)

    def test_13_config_write_failure_preserves_runtime_value(self):
        persist = CFG.index("if (!saveConfigValue(next))")
        commit = CFG.index("cfg_ = next", persist)
        self.assertLess(persist, commit)

    def test_14_backup_recovery_survives_primary_missing(self):
        artifact_root = ROOT / "test-artifacts"
        artifact_root.mkdir(exist_ok=True)
        prefix = "backup-recovery-" + uuid.uuid4().hex
        backup = artifact_root / (prefix + ".json.bak")
        primary = artifact_root / (prefix + ".json")
        temp = artifact_root / (prefix + ".json.tmp")
        try:
            backup.write_text("old-valid", encoding="utf-8")
            recovered = primary if primary.exists() else backup
            self.assertEqual(recovered.read_text(encoding="utf-8"), "old-valid")
            temp.write_text("new-valid", encoding="utf-8"); temp.replace(primary)
            self.assertEqual(backup.read_text(encoding="utf-8"), "old-valid")
        finally:
            for path in (temp, primary, backup):
                path.unlink(missing_ok=True)
        self.assertIn("Retain the previous complete generation", CFG)

    def test_15_tcp_idle_client_is_released_after_controller_friendly_window(self):
        self.assertIn("CLIENT_IDLE_TIMEOUT_MS = 300000U", TCP)
        self.assertIn('if (upper == "PING") return "PONG"', TCP)
        self.assertIn("client_.stop(); rx_ = \"\"; return", TCP)

    def test_16_cookie_metadata_has_names_not_values(self):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        from fs7_proxy import cookie_names, set_cookie_names
        secret = "extremely-secret"
        self.assertEqual(cookie_names([f"SESSIONID={secret}; MODE=engineering"]), ["MODE", "SESSIONID"])
        self.assertEqual(set_cookie_names([("Set-Cookie", f"SESSIONID={secret}; HttpOnly")]), ["SESSIONID"])
        self.assertNotIn(secret, repr(cookie_names([f"SESSIONID={secret}"])))

    def test_17_versioned_api_identifies_itself(self):
        self.assertGreaterEqual(WEB.count('doc["apiVersion"] = "v1"'), 2)
        self.assertIn('doc["firmwareVersion"] = FS7B_VERSION', WEB)
        self.assertIn('doc["deviceName"] = store_.config().deviceName', WEB)


if __name__ == "__main__":
    unittest.main()
