#include "SequenceEngine.h"
#include <algorithm>
#include <memory>
#include <new>

namespace {
bool elapsed(uint32_t now, uint32_t start, uint32_t duration) {
  return static_cast<int32_t>(now - (start + duration)) >= 0;
}
}

bool SequenceEngine::reload(String &error) {
  if (activeSlot_ >= 0 || arbiter_.automationActive()) {
    error = "Cannot reload sequences while automation is active";
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, store_.sequencesJson());
  if (e || !doc["sequences"].is<JsonArray>()) {
    error = e ? String(e.c_str()) : "Missing sequences array";
    return false;
  }

  // The complete 8-slot/16-step table is several KiB because each Step owns
  // multiple Arduino String objects. Keeping that table as a local automatic
  // variable overflows the default Arduino loopTask stack on a real ESP32.
  // Allocate the transactional reload buffer on the heap instead; defs_ is not
  // touched until the entire new configuration has parsed successfully.
  auto next = std::unique_ptr<std::array<Definition, SLOT_COUNT>>(
      new (std::nothrow) std::array<Definition, SLOT_COUNT>());
  if (!next) {
    error = "Not enough heap to reload sequences";
    return false;
  }

  size_t index = 0;
  for (JsonVariantConst v : doc["sequences"].as<JsonArrayConst>()) {
    if (index >= SLOT_COUNT) break;
    JsonObjectConst o = v.as<JsonObjectConst>();
    Definition &d = (*next)[index];
    d.enabled = o["enabled"] | false;
    d.name = o["name"].is<const char *>() ? o["name"].as<String>() : ("Sequence " + String(index + 1));
    d.timeoutMs = constrain(o["timeoutMs"] | 60000U, 1000U, 600000U);

    if (o["steps"].is<JsonArrayConst>()) {
      for (JsonVariantConst sv : o["steps"].as<JsonArrayConst>()) {
        if (d.stepCount >= MAX_STEPS) break;
        JsonObjectConst so = sv.as<JsonObjectConst>();
        String type = so["type"] | "";
        type.toLowerCase();
        Step step;
        if (type == "command") {
          step.type = StepType::Command;
          step.command = so["command"] | "";
          if (!store_.validCommandId(step.command)) continue;
          if (isHoldOnlyCommand(step.command)) {
            error = "Hold-only lens movement commands cannot be used as sequence steps";
            return false;
          }
          if (step.command == "stop_replay") {
            error = "stop_replay is asynchronous and cannot be used as a sequence step";
            return false;
          }
        } else if (type == "wait_ms") {
          step.type = StepType::WaitMs;
          step.waitMs = std::min<uint32_t>(so["ms"] | 0U, 300000U);
        } else if (type == "wait_state") {
          step.type = StepType::WaitState;
          step.stateId = so["state"] | "";
          step.equals = so["equals"] | "true";
          step.timeoutMs = constrain(so["timeoutMs"] | 15000U, 250U, 300000U);
          step.freshMs = constrain(so["freshMs"] | 2500U, 100U, 60000U);
          if (!store_.validStateId(step.stateId)) continue;
        } else {
          continue;
        }
        d.steps[d.stepCount++] = step;
      }
    }
    ++index;
  }

  defs_ = *next;
  activeSlot_ = -1;
  for (size_t i = 0; i < SLOT_COUNT; ++i) resetRuntime(i);
  return true;
}

void SequenceEngine::resetRuntime(size_t index) {
  Runtime r;
  r.state = defs_[index].enabled && defs_[index].stepCount ? RunState::Ready : RunState::Disabled;
  r.phase = stateName(r.state);
  r.stateChangedMs = millis();
  runtime_[index] = r;
}

CommandResult SequenceEngine::trigger(int slot) {
  if (slot < 1 || slot > static_cast<int>(SLOT_COUNT)) return {false, 404, "Unknown sequence slot", 0};
  const size_t i = static_cast<size_t>(slot - 1);
  if (!defs_[i].enabled || defs_[i].stepCount == 0) return {false, 409, "Sequence slot is disabled or empty", 0};
  if (activeSlot_ >= 0) {
    if (activeSlot_ == static_cast<int>(i)) return {false, 409, "Sequence is already running", 0};
    return {false, 409, "Another camera sequence is already running", 0};
  }
  const OperationOwner owner = OperationArbiter::sequenceOwner(i);
  if (!arbiter_.acquire(owner)) {
    return {false, 409, "Camera operation busy: " + String(OperationArbiter::name(arbiter_.owner())), 0};
  }

  Runtime r;
  r.state = RunState::Active;
  r.currentStep = 0;
  r.startedMs = millis();
  r.stepStartedMs = r.startedMs;
  r.stateChangedMs = r.startedMs;
  r.phase = "Starting";
  runtime_[i] = r;
  activeSlot_ = static_cast<int>(i);
  Serial.printf("[sequence] slot %d '%s' started\n", slot, defs_[i].name.c_str());
  return {true, 200, "sequence started", 0};
}

CommandResult SequenceEngine::abort(int slot) {
  if (activeSlot_ < 0) return {true, 200, "no sequence is running", 0};
  if (slot > 0 && activeSlot_ != slot - 1) return {false, 409, "Requested sequence is not active", 0};
  size_t i = static_cast<size_t>(activeSlot_);
  enterState(i, RunState::Aborted, "Aborted");
  runtime_[i].lastError = "Aborted by operator";
  activeSlot_ = -1;
  arbiter_.release(OperationArbiter::sequenceOwner(i));
  Serial.printf("[sequence] slot %u aborted\n", static_cast<unsigned>(i + 1));
  return {true, 200, "sequence aborted", 0};
}

void SequenceEngine::enterState(size_t index, RunState state, const String &phase) {
  runtime_[index].state = state;
  runtime_[index].phase = phase.length() ? phase : stateName(state);
  runtime_[index].stateChangedMs = millis();
}

void SequenceEngine::fail(size_t index, const String &error) {
  runtime_[index].lastError = error;
  enterState(index, RunState::Error, "ERROR");
  activeSlot_ = -1;
  arbiter_.release(OperationArbiter::sequenceOwner(index));
  Serial.printf("[sequence] slot %u ERROR: %s\n", static_cast<unsigned>(index + 1), error.c_str());
}

void SequenceEngine::complete(size_t index) {
  enterState(index, RunState::Complete, "Complete");
  activeSlot_ = -1;
  arbiter_.release(OperationArbiter::sequenceOwner(index));
  Serial.printf("[sequence] slot %u complete\n", static_cast<unsigned>(index + 1));
}

void SequenceEngine::advance(size_t index) {
  ++runtime_[index].currentStep;
  runtime_[index].stepStartedMs = millis();
  enterState(index, RunState::Active, "Running");
  if (runtime_[index].currentStep >= defs_[index].stepCount) complete(index);
}

bool SequenceEngine::textEquals(String a, String b) {
  a.trim(); b.trim(); a.toLowerCase(); b.toLowerCase();
  return a == b;
}

bool SequenceEngine::stateMatches(const Step &step) const {
  const CameraStateValue *value = cameraState_.get(step.stateId);
  if (!value || !value->valid) return false;
  if (millis() - value->updatedMs > step.freshMs) return false;
  // Require a telemetry sample taken after this wait step began. Otherwise a stale
  // pre-command READY=true value could satisfy a post-command wait immediately.
  const Runtime &r = runtime_[static_cast<size_t>(activeSlot_)];
  if (static_cast<int32_t>(value->updatedMs - r.stepStartedMs) < 0) return false;
  return textEquals(value->raw, step.equals);
}

void SequenceEngine::loop() {
  const uint32_t now = millis();

  // COMPLETE returns to READY after a short confidence indication. ERROR and
  // ABORTED remain latched until the next trigger/reload so an operator cannot miss them.
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    if (runtime_[i].state == RunState::Complete && elapsed(now, runtime_[i].stateChangedMs, 2500)) {
      resetRuntime(i);
    }
  }

  if (activeSlot_ < 0) return;
  const size_t i = static_cast<size_t>(activeSlot_);
  Definition &d = defs_[i];
  Runtime &r = runtime_[i];

  if (!commands_.cameraLinkAvailable()) {
    fail(i, "Camera Wi-Fi disconnected during sequence");
    return;
  }

  if (elapsed(now, r.startedMs, d.timeoutMs)) {
    fail(i, "Sequence overall timeout");
    return;
  }
  if (r.currentStep >= d.stepCount) {
    complete(i);
    return;
  }

  Step &step = d.steps[r.currentStep];
  switch (step.type) {
    case StepType::Command: {
      enterState(i, RunState::Active, "Command: " + step.command);
      CommandResult cr = commands_.execute(step.command, OperationArbiter::sequenceOwner(i));
      if (!cr.ok) {
        fail(i, "Command '" + step.command + "' failed: " + cr.message);
        return;
      }
      advance(i);
      break;
    }
    case StepType::WaitMs:
      enterState(i, RunState::Waiting, "Wait " + String(step.waitMs) + " ms");
      if (elapsed(now, r.stepStartedMs, step.waitMs)) advance(i);
      break;
    case StepType::WaitState: {
      if (step.stateId == "writing" && textEquals(step.equals, "true"))
        enterState(i, RunState::Writing, "Waiting: writing=true");
      else if (step.stateId == "playback" && textEquals(step.equals, "true"))
        enterState(i, RunState::Playback, "Waiting: playback=true");
      else
        enterState(i, RunState::Waiting, "Waiting: " + step.stateId + "=" + step.equals);
      if (stateMatches(step)) {
        advance(i);
      } else if (elapsed(now, r.stepStartedMs, step.timeoutMs)) {
        fail(i, "Timed out waiting for " + step.stateId + "=" + step.equals);
      }
      break;
    }
    default:
      fail(i, "Invalid sequence step");
      break;
  }
}

const char *SequenceEngine::stateName(RunState state) {
  switch (state) {
    case RunState::Disabled: return "DISABLED";
    case RunState::Ready: return "READY";
    case RunState::Active: return "ACTIVE";
    case RunState::Waiting: return "WAITING";
    case RunState::Writing: return "WRITING";
    case RunState::Playback: return "PLAYBACK";
    case RunState::Complete: return "COMPLETE";
    case RunState::Error: return "ERROR";
    case RunState::Aborted: return "ABORTED";
  }
  return "UNKNOWN";
}


int SequenceEngine::stateCodeFor(int slot) const {
  if (slot < 1 || slot > static_cast<int>(SLOT_COUNT)) return 0;
  return stateCode(runtime_[static_cast<size_t>(slot - 1)].state);
}

String SequenceEngine::stateTextFor(int slot) const {
  if (slot < 1 || slot > static_cast<int>(SLOT_COUNT)) return "UNKNOWN";
  return String(stateName(runtime_[static_cast<size_t>(slot - 1)].state));
}

String SequenceEngine::phaseFor(int slot) const {
  if (slot < 1 || slot > static_cast<int>(SLOT_COUNT)) return String();
  return runtime_[static_cast<size_t>(slot - 1)].phase;
}

String SequenceEngine::errorFor(int slot) const {
  if (slot < 1 || slot > static_cast<int>(SLOT_COUNT)) return String();
  return runtime_[static_cast<size_t>(slot - 1)].lastError;
}

bool SequenceEngine::busyFor(int slot) const {
  if (slot < 1 || slot > static_cast<int>(SLOT_COUNT)) return false;
  RunState s = runtime_[static_cast<size_t>(slot - 1)].state;
  return s == RunState::Active || s == RunState::Waiting || s == RunState::Writing || s == RunState::Playback;
}

String SequenceEngine::statusJson() const {
  JsonDocument doc;
  doc["activeSlot"] = activeSlot_ >= 0 ? activeSlot_ + 1 : 0;
  JsonArray arr = doc["sequences"].to<JsonArray>();
  for (size_t i = 0; i < SLOT_COUNT; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["slot"] = i + 1;
    o["name"] = defs_[i].name;
    o["enabled"] = defs_[i].enabled;
    o["stepCount"] = defs_[i].stepCount;
    o["state"] = stateName(runtime_[i].state);
    o["stateCode"] = stateCode(runtime_[i].state);
    o["phase"] = runtime_[i].phase;
    o["currentStep"] = runtime_[i].currentStep < defs_[i].stepCount ? runtime_[i].currentStep + 1 : defs_[i].stepCount;
    o["lastError"] = runtime_[i].lastError;
    o["elapsedMs"] = runtime_[i].startedMs ? millis() - runtime_[i].startedMs : 0;
  }
  String out;
  serializeJson(doc, out);
  return out;
}
