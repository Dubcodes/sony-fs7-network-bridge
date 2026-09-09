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
      const bool thumbnailMayAlreadyBeOpen =
          id == "rec_review" && mapping.rpcMethod == "Button.SendKeys" &&
          mapping.rpcParams == "[[\"Thumbnail\"]]" &&
          sonyResult.error.indexOf("Timed out waiting for Sony RPC response") >= 0;
      if (!thumbnailMayAlreadyBeOpen) {
        result.statusCode = 502;
        result.message = sonyResult.error.length()
            ? sonyResult.error
            : ("Sony /linear RPC failed for " + actualId);
        status_.lastError = result.message;
        return result;
      }
      Serial.println("[rec-review] Thumbnail key was not acknowledged; probing Set readiness");
    }
  } else if (sonyResult.httpStatus < 200 || sonyResult.httpStatus >= 300) {
    result.statusCode = 502;
    result.message = "Camera returned HTTP " + String(sonyResult.httpStatus) + " for " + actualId;
    if (sonyResult.httpStatus == 401) result.message += " (check FS7 Basic Authentication settings)";
    status_.lastError = result.message;
    return result;
  }

  // Live FS7 qualification showed that latest-clip playback is three distinct
  // operations: Thumbnail, Set, then Play. Thumbnail population is variable.
  // An ignored Set produces no matched RPC response, while an accepted Set is
  // acknowledged immediately. Use that acknowledgement as the readiness gate.
  if (id == "rec_review" && mapping.transport == "linear" &&
      mapping.rpcMethod == "Button.SendKeys" &&
      mapping.rpcParams == "[[\"Thumbnail\"]]") {
    const uint32_t deadlineMs = started + store_.config().replayTimeoutMs;
    delay(store_.config().recReviewSetDelayMs);
    yield();

    uint16_t attempts = 0;
    bool setAccepted = false;
    while (!due(millis(), deadlineMs)) {
      ++attempts;
      CommandResult setResult = runMapped("cursor_set");
      if (setResult.ok) { setAccepted = true; break; }
      const bool ignored = setResult.message.indexOf("Timed out waiting for Sony RPC response") >= 0;
      if (!ignored) {
        result.statusCode = setResult.statusCode;
        result.message = "Thumbnail opened, but Set failed: " + setResult.message;
        status_.lastError = result.message;
        return result;
      }
      if (due(millis() + 350U, deadlineMs)) {
        result.statusCode = 504;
        result.message = "Thumbnail did not become selectable before the overall timeout";
        status_.lastError = result.message;
        return result;
      }
      delay(350);
      yield();
    }
    if (!setAccepted) {
      result.statusCode = 504;
      result.message = "Thumbnail did not become selectable before the overall timeout";
      status_.lastError = result.message;
      return result;
    }

    Serial.printf("[rec-review] Set accepted after %u attempt(s), %lu ms after Thumbnail\n",
                  attempts, static_cast<unsigned long>(millis() - started));
    delay(store_.config().recReviewPlayDelayMs);
    yield();
    if (due(millis(), deadlineMs)) {
      result.statusCode = 504;
      result.message = "Latest clip selected, but the overall timeout expired before Play";
      status_.lastError = result.message;
      return result;
    }
    CommandResult playResult = runMapped("play");
    if (!playResult.ok) {
      result.statusCode = playResult.statusCode;
      result.message = "Latest clip selected, but Play failed: " + playResult.message;
      status_.lastError = result.message;
      return result;
    }
    String playbackError;
    if (!waitForPlayback(deadlineMs, playbackError)) {
      result.statusCode = 504;
      result.message = playbackError;
      status_.lastError = result.message;
      return result;
    }
  }

  result.ok = true;
  result.statusCode = 200;
  result.message = id == "rec_review" ? "latest clip playback verified" : "ok";
  applyOptimisticState(id);
  return result;
}

bool CommandEngine::waitForPlayback(uint32_t deadlineMs, String &error) {
  while (!due(millis(), deadlineMs)) {
    const uint32_t remaining = deadlineMs - millis();
    const uint32_t probeTimeout = remaining < 1200U ? remaining : 1200U;
    SonyResponse probe = sony_.linearRequest(
        "Property.GetValue", "[{\"P.Clip.Mediabox.Status\":null}]", probeTimeout);
    status_.cameraReachable = probe.transportOk;
    status_.lastSonyHttpStatus = probe.httpStatus;
    if (probe.commandOk &&
        (probe.body == "Playing" ||
         probe.body.indexOf("P.Clip.Mediabox.Status=Playing") >= 0)) {
      Serial.println("[rec-review] P.Clip.Mediabox.Status=Playing verified");
      return true;
    }
    if (!probe.transportOk) {
      error = "Play was sent, but camera status verification lost the camera connection";
      return false;
    }
    delay(250);
    yield();
  }
  error = "Play was accepted, but P.Clip.Mediabox.Status did not become Playing before timeout";
  return false;
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

bool CommandEngine::dispatchLatestClipPlayback() {
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
        if (due(now, minReviewMs_)) dispatchLatestClipPlayback();
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
      if (due(now, minReviewMs_)) dispatchLatestClipPlayback();
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
  status_.macroPhase = "PLAYBACK_VERIFIED";
  macroState_ = MacroState::Idle;
  arbiter_.release(OperationOwner::StopReplay);
  Serial.println("[macro] Stop + Replay playback verified");
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
