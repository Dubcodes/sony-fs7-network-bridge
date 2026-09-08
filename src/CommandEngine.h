#pragma once

#include <Arduino.h>
#include "AppTypes.h"
#include "CameraState.h"
#include "ConfigStore.h"
#include "SonyRemote.h"
#include "OperationArbiter.h"

struct CommandResult {
  bool ok = false;
  int statusCode = 500;
  String message;
  int sonyHttpStatus = 0;
};

class CommandEngine {
 public:
  CommandEngine(ConfigStore &store, SonyRemote &sony, CameraStateStore &cameraState,
                RuntimeStatus &status, OperationArbiter &arbiter)
      : store_(store), sony_(sony), cameraState_(cameraState), status_(status), arbiter_(arbiter) {}

  CommandResult execute(const String &id, OperationOwner caller = OperationOwner::Manual);
  CommandResult abortStopReplay();
  void loop();
  bool transportBusy() const { return macroState_ != MacroState::Idle; }
  bool cameraLinkAvailable() const { return store_.config().dryRun || sony_.wifiConnected(); }

 private:
  enum class MacroState { Idle, ObserveStop, WaitReady, FallbackDelay };

  ConfigStore &store_;
  SonyRemote &sony_;
  CameraStateStore &cameraState_;
  RuntimeStatus &status_;
  OperationArbiter &arbiter_;
  MacroState macroState_ = MacroState::Idle;
  uint32_t macroStartedMs_ = 0;
  uint32_t minReviewMs_ = 0;

  CommandResult runMapped(const String &id, const String &fallbackId = "");
  void applyOptimisticState(const String &id);
  void finishMacroError(const String &message);
  void finishMacroSuccess();
  bool freshState(const String &id) const;
  bool freshStateSince(const String &id, uint32_t sinceMs) const;
  bool haveStateDrivenReplay() const;
  bool cameraReadyForReview() const;
  bool dispatchReview();
};
