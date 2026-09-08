#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include "CameraState.h"
#include "CommandEngine.h"
#include "ConfigStore.h"
#include "OperationArbiter.h"

class SequenceEngine {
 public:
  static constexpr size_t SLOT_COUNT = 8;
  static constexpr size_t MAX_STEPS = 16;

  SequenceEngine(ConfigStore &store, CommandEngine &commands, CameraStateStore &cameraState,
                 OperationArbiter &arbiter)
      : store_(store), commands_(commands), cameraState_(cameraState), arbiter_(arbiter) {}

  bool begin(String &error) { return reload(error); }
  bool reload(String &error);
  CommandResult trigger(int slot);
  CommandResult abort(int slot = 0);
  void loop();
  String statusJson() const;
  int stateCodeFor(int slot) const;
  String stateTextFor(int slot) const;
  String phaseFor(int slot) const;
  String errorFor(int slot) const;
  bool busyFor(int slot) const;

 private:
  enum class RunState : uint8_t {
    Disabled = 0,
    Ready = 1,
    Active = 2,
    Waiting = 3,
    Writing = 4,
    Playback = 5,
    Complete = 6,
    Error = 7,
    Aborted = 8,
  };

  enum class StepType : uint8_t { None, Command, WaitMs, WaitState };

  struct Step {
    StepType type = StepType::None;
    String command;
    uint32_t waitMs = 0;
    String stateId;
    String equals;
    uint32_t timeoutMs = 15000;
    uint32_t freshMs = 2500;
  };

  struct Definition {
    bool enabled = false;
    String name;
    uint32_t timeoutMs = 60000;
    size_t stepCount = 0;
    std::array<Step, MAX_STEPS> steps{};
  };

  struct Runtime {
    RunState state = RunState::Disabled;
    size_t currentStep = 0;
    uint32_t startedMs = 0;
    uint32_t stepStartedMs = 0;
    uint32_t stateChangedMs = 0;
    String phase;
    String lastError;
  };

  ConfigStore &store_;
  CommandEngine &commands_;
  CameraStateStore &cameraState_;
  OperationArbiter &arbiter_;
  std::array<Definition, SLOT_COUNT> defs_{};
  std::array<Runtime, SLOT_COUNT> runtime_{};
  int activeSlot_ = -1;

  void resetRuntime(size_t index);
  void enterState(size_t index, RunState state, const String &phase = "");
  void fail(size_t index, const String &error);
  void complete(size_t index);
  void advance(size_t index);
  bool stateMatches(const Step &step) const;
  static bool textEquals(String a, String b);
  static const char *stateName(RunState state);
  static int stateCode(RunState state) { return static_cast<int>(state); }
};
