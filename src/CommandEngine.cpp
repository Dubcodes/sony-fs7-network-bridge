#include "CommandEngine.h"

namespace {
bool due(uint32_t now, uint32_t when) {
  return static_cast<int32_t>(now - when) >= 0;
}
}

CommandResult CommandEngine::runMapped(const String &id, const String &fallbackId) {
  CommandResult result;
  SonyCommandMapping mapping = store_.commandMapping(id);
  String actualId = id;
  if (!mapping.configured() && fallbackId.length()) {
    SonyCommandMapping fallback = store_.commandMapping(fallbackId);
    if (fallback.configured()) {
      mapping = fallback;
      actualId = fallbackId;
    }
  }

  status_.lastCommand = id;
  status_.lastError = "";

  if (store_.config().dryRun) {
    result.ok = true;
    result.statusCode = 200;
    result.message = String("dry-run: ") + id;
    applyOptimisticState(id);
    Serial.printf("[cmd] dry-run %s\n", id.c_str());
    return result;
  }

  if (!mapping.configured()) {
    result.statusCode = 409;
    result.message = "Sony mapping is not configured for '" + id + "'";
    if (fallbackId.length()) result.message += " (nor fallback '" + fallbackId + "')";
    status_.lastError = result.message;
    return result;
  }

  uint32_t started = millis();
  SonyResponse sonyResult = sony_.send(mapping, 4096);
  status_.lastSonyResponseMs = millis() - started;
  status_.lastSonyHttpStatus = sonyResult.httpStatus;
  status_.cameraReachable = sonyResult.transportOk;
  result.sonyHttpStatus = sonyResult.httpStatus;

  if (!sonyResult.transportOk) {
    result.statusCode = 503;
    result.message = sonyResult.error.length() ? sonyResult.error : "No HTTP response from camera";
    status_.lastError = result.message;
    return result;
  }

  if (sonyResult.isLinear) {
    if (!sonyResult.commandOk) {
      result.statusCode = 502;
      result.message = sonyResult.error.length()
          ? sonyResult.error
          : ("Sony /linear RPC failed for " + actualId);
      status_.lastError = result.message;
      return result;
    }
  } else if (sonyResult.httpStatus < 200 || sonyResult.httpStatus >= 300) {
    result.statusCode = 502;
    result.message = "Camera returned HTTP " + String(sonyResult.httpStatus) + " for " + actualId;
    if (sonyResult.httpStatus == 401) result.message += " (check FS7 Basic Authentication settings)";
    status_.lastError = result.message;
    return result;
  }

  // The FS7 native Cursor page reliably plays the latest clip by opening
  // Thumbnail and then pressing Set. When the default Rec Review mapping is the
  // exact native Thumbnail key, complete that two-key operation here. This avoids
  // depending on Assignable.6 (which may be mapped to SHUTTER or anything else).
  if (id == "rec_review" && mapping.transport == "linear" &&
      mapping.rpcMethod == "Button.SendKeys" &&
      mapping.rpcParams == "[[\"Thumbnail\"]]") {
    delay(400);
    yield();
    SonyCommandMapping setKey;
    setKey.id = "rec_review_set";
    setKey.transport = "linear";
    setKey.rpcMethod = "Button.SendKeys";
    setKey.rpcParams = "[[\"Set\"]]";
    uint32_t setStarted = millis();
    SonyResponse setResult = sony_.send(setKey, 4096);
    status_.lastSonyResponseMs = millis() - setStarted;
    status_.lastSonyHttpStatus = setResult.httpStatus;
    status_.cameraReachable = setResult.transportOk;
    result.sonyHttpStatus = setResult.httpStatus;
    if (!setResult.transportOk || !setResult.commandOk) {
      result.statusCode = setResult.transportOk ? 502 : 503;
      result.message = "Thumbnail opened, but Set failed";
      if (setResult.error.length()) result.message += ": " + setResult.error;
      status_.lastError = result.message;
      return result;
    }
  }

  result.ok = true;
  result.statusCode = 200;
  result.message = id == "rec_review" ? "latest clip playback dispatched" : "ok";
  applyOptimisticState(id);
  return result;
}

bool CommandEngine::freshState(const String &id) const {
  const CameraStateValue *v = cameraState_.get(id);
  return v && v->valid && (millis() - v->updatedMs <= store_.config().telemetryFreshMs);
}

bool CommandEngine::freshStateSince(const String &id, uint32_t sinceMs) const {
  const CameraStateValue *v = cameraState_.get(id);
  if (!v || !v->valid || millis() - v->updatedMs > store_.config().telemetryFreshMs) return false;
  // A pre-command READY/WRITING/REC sample must never satisfy a post-command wait.
  return static_cast<int32_t>(v->updatedMs - sinceMs) >= 0;
}

bool CommandEngine::haveStateDrivenReplay() const {
  return freshState("ready") || freshState("writing") || freshState("recording");
}

bool CommandEngine::cameraReadyForReview() const {
  // Require a telemetry sample received after the STOP command completed. This prevents
  // an old READY=true value from advancing the macro before the camera has processed STOP.
  if (freshStateSince("ready", macroStartedMs_)) return cameraState_.boolValue("ready", false);
  // If media-writing feedback exists, it is safe once a post-STOP sample says writing=false.
  if (freshStateSince("writing", macroStartedMs_)) return !cameraState_.boolValue("writing", true);
  // Recording=false is weaker but still better than a blind timer, provided it is post-STOP.
  if (freshStateSince("recording", macroStartedMs_)) return !cameraState_.boolValue("recording", true);
  return false;
}

CommandResult CommandEngine::execute(const String &id, OperationOwner caller) {
  if (id == "stop_replay") {
    CommandResult result;
    if (OperationArbiter::isSequence(caller)) {
      result.statusCode = 409;
      result.message = "stop_replay is asynchronous and is not allowed inside a sequence";
      return result;
    }
    if (!arbiter_.acquire(OperationOwner::StopReplay)) {
      result.statusCode = 409;
      result.message = "Camera operation busy: " + String(OperationArbiter::name(arbiter_.owner()));
      return result;
    }

    CommandResult stop = runMapped("record_stop", "record_toggle");
    if (!stop.ok) {
      arbiter_.release(OperationOwner::StopReplay);
      return stop;
    }

    const uint32_t now = millis();
    macroStartedMs_ = now;
    minReviewMs_ = now + store_.config().replayDelayMs;
    macroState_ = haveStateDrivenReplay() ? MacroState::ObserveStop : MacroState::FallbackDelay;
    status_.macroBusy = true;
    status_.macroStartedMs = now;
    status_.macroPhase = macroState_ == MacroState::FallbackDelay ? "WAIT_DELAY" : "WAIT_CAMERA_STATE";
    status_.optimisticRecording = false;

    result.ok = true;
    result.statusCode = 200;
    result.message = macroState_ == MacroState::FallbackDelay
        ? "record stop accepted; waiting fallback delay before Rec Review"
        : "record stop accepted; waiting for camera state before Rec Review";
    return result;
  }

  const bool transientManual = caller == OperationOwner::Manual;
  if (transientManual) {
    if (!arbiter_.acquire(OperationOwner::Manual)) {
      return {false, 409, "Camera operation busy: " + String(OperationArbiter::name(arbiter_.owner())), 0};
    }
  } else if (!arbiter_.ownedBy(caller)) {
    return {false, 409, "Camera operation ownership was lost", 0};
  }

  CommandResult result;
  if (id == "record_start") result = runMapped(id, "record_toggle");
  else if (id == "record_stop") result = runMapped(id, "record_toggle");
  else result = runMapped(id);
  if (transientManual) arbiter_.release(OperationOwner::Manual);
  return result;
}

CommandResult CommandEngine::abortStopReplay() {
  if (macroState_ == MacroState::Idle || !arbiter_.ownedBy(OperationOwner::StopReplay)) {
    return {true, 200, "Stop + Replay is not running", 0};
  }
  macroState_ = MacroState::Idle;
  status_.macroBusy = false;
  status_.macroPhase = "ABORTED";
  status_.lastError = "Stop + Replay aborted by operator";
  arbiter_.release(OperationOwner::StopReplay);
  return {true, 200, "Stop + Replay aborted", 0};
}

bool CommandEngine::dispatchReview() {
  CommandResult review = runMapped("rec_review");
  if (!review.ok) {
    finishMacroError("Stop succeeded but Rec Review failed: " + review.message);
    return false;
  }
  status_.optimisticPlayback = true;
  finishMacroSuccess();
  return true;
}

void CommandEngine::loop() {
  if (status_.optimisticPlayback && status_.optimisticPlaybackUntilMs &&
      due(millis(), status_.optimisticPlaybackUntilMs)) {
    status_.optimisticPlayback = false;
    status_.optimisticPlaybackUntilMs = 0;
  }
  if (macroState_ == MacroState::Idle) return;
  const uint32_t now = millis();

  if (!cameraLinkAvailable()) {
    finishMacroError("Camera Wi-Fi disconnected during Stop + Replay");
    return;
  }

  if (now - macroStartedMs_ > store_.config().replayTimeoutMs) {
    finishMacroError("Stop + Replay timed out waiting for camera readiness");
    return;
  }

  switch (macroState_) {
    case MacroState::ObserveStop:
    case MacroState::WaitReady:
      if (cameraReadyForReview()) {
        macroState_ = MacroState::WaitReady;
        status_.macroPhase = "CAMERA_READY";
        // Respect the configured minimum delay even when the camera reports ready immediately.
        if (due(now, minReviewMs_)) dispatchReview();
      } else {
        status_.macroPhase = freshStateSince("writing", macroStartedMs_) && cameraState_.boolValue("writing", false)
            ? "WRITING" : "WAIT_CAMERA_STATE";
        // If telemetry goes stale during the macro, fail over to the bounded delay path.
        if (!haveStateDrivenReplay()) {
          macroState_ = MacroState::FallbackDelay;
          status_.macroPhase = "WAIT_DELAY";
        }
      }
      break;

    case MacroState::FallbackDelay:
      status_.macroPhase = "WAIT_DELAY";
      if (due(now, minReviewMs_)) dispatchReview();
      break;

    case MacroState::Idle:
      break;
  }
}

void CommandEngine::finishMacroError(const String &message) {
  status_.lastError = message;
  status_.macroBusy = false;
  status_.macroPhase = "ERROR";
  macroState_ = MacroState::Idle;
  arbiter_.release(OperationOwner::StopReplay);
  Serial.printf("[macro] %s\n", message.c_str());
}

void CommandEngine::finishMacroSuccess() {
  status_.macroBusy = false;
  status_.macroPhase = "REVIEW_DISPATCHED";
  macroState_ = MacroState::Idle;
  arbiter_.release(OperationOwner::StopReplay);
  Serial.println("[macro] Stop + Replay sequence dispatched");
}

void CommandEngine::applyOptimisticState(const String &id) {
  if (id == "record_start") {
    status_.optimisticRecording = true;
    status_.optimisticPlayback = false;
    status_.optimisticPlaybackUntilMs = 0;
  } else if (id == "record_stop") {
    status_.optimisticRecording = false;
  } else if (id == "record_toggle") {
    status_.optimisticRecording = !status_.optimisticRecording;
    if (status_.optimisticRecording) {
      status_.optimisticPlayback = false;
      status_.optimisticPlaybackUntilMs = 0;
    }
  } else if (id == "rec_review" || id == "play") {
    status_.optimisticPlayback = true;
    status_.optimisticPlaybackUntilMs = millis() + 10000U;
  } else if (id == "stop_playback") {
    status_.optimisticPlayback = false;
    status_.optimisticPlaybackUntilMs = 0;
  }
}
