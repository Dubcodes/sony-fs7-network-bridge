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
TCP = (ROOT / "src/ControlProtocol.cpp").read_text(encoding="utf-8")
API = (ROOT / "docs/API.md").read_text(encoding="utf-8")
INTEGRATION = (ROOT / "docs/INTEGRATION.md").read_text(encoding="utf-8")


def canonical_slots(entries, count):
    """Host model of the explicit-first firmware assignment contract."""
    selected = [None] * count
    for entry in entries:
        slot = entry.get("slot") if isinstance(entry, dict) else None
        if isinstance(slot, int) and not isinstance(slot, bool) and 1 <= slot <= count:
            if selected[slot - 1] is None:
                selected[slot - 1] = entry
    for index, entry in enumerate(entries[:count]):
        if not isinstance(entry, dict):
            continue
        slot = entry.get("slot")
        valid_slot = isinstance(slot, int) and not isinstance(slot, bool) and 1 <= slot <= count
        if not valid_slot and selected[index] is None:
            selected[index] = entry
    return selected


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

    def test_ascii_tcp_rejects_hold_only_but_not_stop_commands(self):
        dispatch = TCP[TCP.index('if (upper.startsWith("CMD "))'):TCP.index("String ControlProtocol::oneLineStatus")]
        self.assertIn("if (isHoldOnlyCommand(id))", dispatch)
        self.assertIn('"ERR " + id + " 409 hold-only lens movement requires an explicit hold/release controller"', dispatch)
        self.assertNotIn('id == "focus_stop"', dispatch)
        self.assertNotIn('id == "zoom_stop"', dispatch)
        self.assertIn("Generic TCP and VSM controllers must not use", INTEGRATION)

    def test_empty_or_short_vsm_maps_are_canonicalized_to_fixed_shape(self):
        helper = CFG[CFG.index("void canonicalizeVsmGroup"):CFG.index("bool validHttpMethod")]
        self.assertIn('canonicalizeVsmGroup(source["triggers"], triggers, 16', helper)
        self.assertIn('const char *groups[] = {"bools", "ints", "floats", "texts"};', helper)
        self.assertIn('canonicalizeVsmGroup(source[group], slots, 8', helper)
        self.assertIn('existing["label"].is<const char *>()', helper)
        self.assertIn('existing[valueKey].as<String>()', helper)
        load = CFG[CFG.index("bool ConfigStore::loadVsmGenericMap"):CFG.index("void ConfigStore::setDefaultSequences")]
        self.assertGreaterEqual(load.count("canonicalizeVsmGenericMap"), 2)
        self.assertNotIn("Missing array:", load)

    def test_vsm_slot_assignment_is_explicit_first_and_deterministic(self):
        helper = CFG[CFG.index("void canonicalizeVsmGroup"):CFG.index("bool validHttpMethod")]
        self.assertIn("for (JsonVariantConst value : input)", helper)
        self.assertIn("if (slot < 1 || slot > count || claimed[slot - 1]) continue;", helper)
        self.assertIn("selected[slot - 1] = entry;", helper)
        self.assertIn("for (int i = 0; i < count && i < static_cast<int>(input.size()); ++i)", helper)
        self.assertIn("if (validSlot || claimed[i]) continue;", helper)
        self.assertIn("selected[i] = entry;", helper)
        self.assertIn("first explicit entry for a duplicate slot wins", INTEGRATION)

    def test_sparse_explicit_trigger_stays_in_slot_16(self):
        entry = {"slot": 16, "label": "Last", "command": "play"}
        slots = canonical_slots([entry], 16)
        self.assertIs(slots[15], entry)
        self.assertIsNone(slots[0])

    def test_out_of_order_entries_stay_in_explicit_slots(self):
        slot_eight = {"slot": 8, "label": "Eight", "source": "camera.playback"}
        slot_two = {"slot": 2, "label": "Two", "source": "camera.recording"}
        slots = canonical_slots([slot_eight, slot_two], 8)
        self.assertIs(slots[7], slot_eight)
        self.assertIs(slots[1], slot_two)

    def test_missing_slot_uses_positional_fallback(self):
        first = {"label": "Legacy first", "command": "record_start"}
        slots = canonical_slots([first], 16)
        self.assertIs(slots[0], first)

    def test_duplicate_and_out_of_range_slots_are_deterministic(self):
        first = {"slot": 3, "label": "First", "source": "camera.recording"}
        duplicate = {"slot": 3, "label": "Duplicate", "source": "camera.playback"}
        out_of_range = {"slot": 99, "label": "Fallback", "source": "bridge.camera_reachable"}
        slots = canonical_slots([first, duplicate, None, out_of_range], 8)
        self.assertIs(slots[2], first)
        self.assertNotIn(duplicate, slots)
        self.assertIs(slots[3], out_of_range)

    def test_closing_persistent_linear_session_clears_reachability(self):
        close = LINEAR[LINEAR.index("void SonyLinear::closeConnection()"):LINEAR.index("void SonyLinear::applyProperty")]
        self.assertIn("fd_ = -1;", close)
        self.assertIn("if (status_) status_->cameraReachable = false;", close)


if __name__ == "__main__":
    unittest.main()
