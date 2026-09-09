#include "ControlProtocol.h"
#include <ArduinoJson.h>

#ifndef FS7B_TCP_PORT
#define FS7B_TCP_PORT 5000
#endif

namespace {
// A broadcast controller may hold a quiet session between production events.
// PING refreshes this timer; clients should reconnect after an idle close.
constexpr uint32_t CLIENT_IDLE_TIMEOUT_MS = 300000U;
}

ControlProtocol::ControlProtocol(CommandEngine &engine, SequenceEngine &sequences, ConfigStore &store,
                                 BridgeNetworkManager &network, CameraStateStore &cameraState,
                                 RuntimeStatus &status, OperationArbiter &arbiter)
    : server_(FS7B_TCP_PORT), engine_(engine), sequences_(sequences), store_(store), network_(network),
      cameraState_(cameraState), status_(status), arbiter_(arbiter) {}

void ControlProtocol::begin() {
  server_.begin();
  Serial.printf("[tcp] ASCII control listening on port %d\n", FS7B_TCP_PORT);
}

void ControlProtocol::loop() {
  if (!client_ || !client_.connected()) {
    NetworkClient next = server_.accept();
    if (next) {
      client_.stop(); client_ = next; rx_ = ""; lastClientActivityMs_ = millis();
      client_.printf("FS7-BRIDGE %s READY\r\n", FS7B_VERSION);
    }
  }
  if (!client_ || !client_.connected()) return;
  if (millis() - lastClientActivityMs_ > CLIENT_IDLE_TIMEOUT_MS) {
    client_.stop(); rx_ = ""; return;
  }
  while (client_.available()) {
    char c = static_cast<char>(client_.read());
    lastClientActivityMs_ = millis();
    if (c == '\n') { String line = rx_; rx_ = ""; line.trim(); if (line.length()) client_.print(processLine(line) + "\r\n"); }
    else if (c != '\r' && rx_.length() < 512) rx_ += c;
  }
}

String ControlProtocol::processLine(String line) {
  String upper = line; upper.toUpperCase();
  if (upper == "PING") return "PONG";
  if (upper == "STATUS") return oneLineStatus();
  if (upper == "STATE") return cameraState_.json(store_.config().telemetryFreshMs);
  if (upper == "HELP") return "COMMANDS: PING STATUS STATE SEQ_STATUS SEQ <1..8> SEQ_ABORT [1..8] STOP_REPLAY_ABORT GET <state-id> CMD <id> REC_START REC_STOP STOP_REPLAY REC_REVIEW PLAY PAUSE STOP_PLAYBACK AWB";
  if (upper == "SEQ_STATUS") return sequences_.statusJson();
  if (upper == "STOP_REPLAY_ABORT") { CommandResult r = engine_.abortStopReplay(); return r.ok ? "OK " + r.message : "ERR " + String(r.statusCode) + " " + r.message; }
  if (upper.startsWith("SEQ_ABORT")) { int slot = line.substring(9).toInt(); CommandResult r = sequences_.abort(slot); return r.ok ? "OK " + r.message : "ERR " + String(r.statusCode) + " " + r.message; }
  if (upper.startsWith("SEQ ")) { int slot = line.substring(4).toInt(); CommandResult r = sequences_.trigger(slot); return r.ok ? "OK sequence " + String(slot) + " " + r.message : "ERR sequence " + String(slot) + " " + String(r.statusCode) + " " + r.message; }
  if (upper.startsWith("GET ")) {
    String id = line.substring(4); id.trim();
    const CameraStateValue *v = cameraState_.get(id);
    if (!v) return "ERR unknown state";
    if (!v->valid) return "UNKNOWN " + id;
    const uint32_t age = millis() - v->updatedMs;
    if (age > store_.config().telemetryFreshMs) return "STALE " + id + " " + v->raw + " " + String(age);
    return "VALUE " + id + " " + v->raw;
  }

  String id;
  if (upper.startsWith("CMD ")) id = line.substring(4);
  else if (upper == "REC_START") id = "record_start";
  else if (upper == "REC_STOP") id = "record_stop";
  else if (upper == "STOP_REPLAY") id = "stop_replay";
  else if (upper == "REC_REVIEW") id = "rec_review";
  else if (upper == "PLAY") id = "play";
  else if (upper == "PAUSE") id = "pause";
  else if (upper == "STOP_PLAYBACK") id = "stop_playback";
  else if (upper == "AWB") id = "awb";
  else return "ERR unknown command";

  id.trim();
  if (isHoldOnlyCommand(id)) {
    return "ERR " + id + " 409 hold-only lens movement requires an explicit hold/release controller";
  }
  CommandResult r = engine_.execute(id);
  if (r.ok) return "OK " + id + " " + r.message;
  return "ERR " + id + " " + String(r.statusCode) + " " + r.message;
}

String ControlProtocol::oneLineStatus() {
  JsonDocument doc;
  doc["device"] = store_.config().deviceName; doc["version"] = FS7B_VERSION;
  doc["ethernet"] = network_.ethernetConnected(); doc["ethernetIp"] = network_.ethernetIp();
  doc["wifi"] = network_.wifiConnected(); doc["wifiIp"] = network_.wifiIp();
  doc["camera"] = status_.cameraReachable && network_.wifiConnected();
  const bool recFresh = cameraState_.isFresh("recording", store_.config().telemetryFreshMs);
  const bool playFresh = cameraState_.isFresh("playback", store_.config().telemetryFreshMs);
  if (recFresh) doc["recording"] = cameraState_.boolValue("recording"); else doc["recording"] = nullptr;
  if (playFresh) doc["playback"] = cameraState_.boolValue("playback"); else doc["playback"] = nullptr;
  doc["optimisticRecording"] = status_.optimisticRecording;
  doc["optimisticPlayback"] = status_.optimisticPlayback;
  doc["recordingFresh"] = recFresh; doc["playbackFresh"] = playFresh;
  doc["macroBusy"] = status_.macroBusy; doc["macroPhase"] = status_.macroPhase; doc["lastCommand"] = status_.lastCommand; doc["lastError"] = status_.lastError;
  doc["operationBusy"] = arbiter_.busy(); doc["operationOwner"] = OperationArbiter::name(arbiter_.owner());
  doc["operationElapsedMs"] = arbiter_.elapsedMs();
  String out; serializeJson(doc, out); return out;
}
