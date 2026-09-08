"""Host-side safety contract tests for hardware-independent sequencing rules.

These tests deliberately contain no Sony URL/body fixtures. They exercise the
required state-machine contract and also assert that the firmware source keeps
the corresponding freshness, timeout, disconnect, and fail-closed hooks.
"""
from __future__ import annotations

import dataclasses
import pathlib
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@dataclasses.dataclass
class Sample:
    value: str
    at: int


class SequenceContract:
    def __init__(self, *, enabled=True, overall=1000):
        self.enabled = enabled
        self.overall = overall
        self.state = "READY" if enabled else "DISABLED"
        self.started = 0
        self.step_started = 0
        self.active = False

    def trigger(self, now=0, another_active=False):
        if not self.enabled or another_active or self.active:
            return False
        self.active = True
        self.state = "ACTIVE"
        self.started = self.step_started = now
        return True

    def wait_ms(self, now, duration):
        if now - self.started >= self.overall:
            self.state, self.active = "ERROR", False
        elif now - self.step_started >= duration:
            self.state = "ACTIVE"
            self.step_started = now
        else:
            self.state = "WAITING"

    def wait_state(self, now, sample, expected, timeout, fresh):
        if now - self.started >= self.overall or now - self.step_started >= timeout:
            self.state, self.active = "ERROR", False
        elif sample and sample.at >= self.step_started and now - sample.at <= fresh and sample.value == expected:
            self.state = "ACTIVE"
            self.step_started = now
        else:
            self.state = "WAITING"

    def abort(self):
        self.state, self.active = "ABORTED", False

    def complete(self, now):
        self.state, self.active, self.step_started = "COMPLETE", False, now

    def settle(self, now):
        if self.state == "COMPLETE" and now - self.step_started >= 2500:
            self.state = "READY"


class ReplayContract:
    def __init__(self, started=100, delay=900, timeout=15000):
        self.started, self.minimum, self.timeout = started, started + delay, timeout

    def decision(self, now, samples, connected=True):
        if not connected or now - self.started > self.timeout:
            return "ERROR"
        post = {k: v for k, v in samples.items() if v.at >= self.started and now - v.at <= 2500}
        if "writing" in post and post["writing"].value == "true":
            return "WAIT"
        ready = (post.get("ready") and post["ready"].value == "true") or \
                (post.get("writing") and post["writing"].value == "false") or \
                (post.get("recording") and post["recording"].value == "false")
        if ready and now >= self.minimum:
            return "REVIEW"
        if not post and now >= self.minimum:
            return "REVIEW"  # documented no-telemetry fallback
        return "WAIT"


class SequenceSafetyTests(unittest.TestCase):
    def test_disabled_and_exclusive_trigger(self):
        self.assertFalse(SequenceContract(enabled=False).trigger())
        self.assertFalse(SequenceContract().trigger(another_active=True))

    def test_wait_progression_and_timeouts(self):
        s = SequenceContract(overall=100)
        self.assertTrue(s.trigger())
        s.wait_ms(9, 10); self.assertEqual(s.state, "WAITING")
        s.wait_ms(10, 10); self.assertEqual(s.state, "ACTIVE")
        s.wait_state(99, None, "true", 20, 10); self.assertEqual(s.state, "ERROR")

    def test_fresh_post_step_sample_required(self):
        s = SequenceContract(); s.trigger(100); s.step_started = 200
        s.wait_state(210, Sample("true", 199), "true", 100, 50)
        self.assertEqual(s.state, "WAITING")
        s.wait_state(300, Sample("true", 220), "true", 100, 50)
        self.assertEqual(s.state, "ERROR")  # sample is stale and wait timed out
        s = SequenceContract(); s.trigger(100); s.step_started = 200
        s.wait_state(220, Sample("true", 210), "true", 100, 50)
        self.assertEqual(s.state, "ACTIVE")

    def test_abort_and_complete_lifecycle(self):
        s = SequenceContract(); s.trigger(); s.abort(); self.assertEqual(s.state, "ABORTED")
        s = SequenceContract(); s.trigger(); s.complete(10); s.settle(2509)
        self.assertEqual(s.state, "COMPLETE")
        s.settle(2510); self.assertEqual(s.state, "READY")


class ReplaySafetyTests(unittest.TestCase):
    def test_stale_pre_stop_ready_rejected_but_fallback_is_bounded(self):
        r = ReplayContract()
        self.assertEqual(r.decision(500, {"ready": Sample("true", 99)}), "WAIT")
        self.assertEqual(r.decision(1000, {"ready": Sample("true", 99)}), "REVIEW")

    def test_writing_and_ready_transitions(self):
        r = ReplayContract()
        self.assertEqual(r.decision(1000, {"writing": Sample("true", 900)}), "WAIT")
        self.assertEqual(r.decision(1000, {"writing": Sample("false", 950)}), "REVIEW")
        self.assertEqual(r.decision(1000, {"ready": Sample("true", 950)}), "REVIEW")

    def test_disconnect_and_overall_timeout_fail(self):
        r = ReplayContract()
        self.assertEqual(r.decision(200, {}, connected=False), "ERROR")
        self.assertEqual(r.decision(15101, {}), "ERROR")


class FirmwareSourceContractTests(unittest.TestCase):
    def test_production_hooks_match_contract(self):
        seq = (ROOT / "src/SequenceEngine.cpp").read_text(encoding="utf-8")
        cmd = (ROOT / "src/CommandEngine.cpp").read_text(encoding="utf-8")
        config = (ROOT / "src/ConfigStore.cpp").read_text(encoding="utf-8")
        web = (ROOT / "src/WebApp.cpp").read_text(encoding="utf-8")
        sony = (ROOT / "src/SonyRemote.cpp").read_text(encoding="utf-8")
        for token in ("value->updatedMs - r.stepStartedMs", "Sequence overall timeout", "RunState::Aborted", "cameraLinkAvailable"):
            self.assertIn(token, seq)
        for token in ("freshStateSince(\"ready\", macroStartedMs_)", "FallbackDelay", "replayTimeoutMs", "cameraLinkAvailable"):
            self.assertIn(token, cmd)
        self.assertIn('seq["enabled"] = false', config)
        self.assertIn('fullExportJson(true)', web)
        self.assertIn('forbiddenHeader', sony)

    def test_proxy_event_ids_follow_file_order_under_threads(self):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        from fs7_proxy import ProxyState
        artifact_root = ROOT / "test-artifacts"
        artifact_root.mkdir(exist_ok=True)
        log = artifact_root / "proxy-thread-events.jsonl"
        log.unlink(missing_ok=True)
        state = ProxyState("127.0.0.1", 80, "u", "p", log, "127.0.0.1:1")
        threads = [threading.Thread(target=state.write_log, args=({"time": str(i)},)) for i in range(50)]
        for thread in threads: thread.start()
        for thread in threads: thread.join()
        import json
        ids = [json.loads(line)["eventId"] for line in log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(ids, list(range(1, 51)))
        log.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
