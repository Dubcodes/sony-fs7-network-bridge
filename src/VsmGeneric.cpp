#include "VsmGeneric.h"
#include <ArduinoJson.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>

bool VsmGeneric::reload(String &error) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, store_.vsmGenericMapJson());
  if (e) { error = e.c_str(); return false; }

  for (int i = 0; i < 16; ++i) {
    JsonObject o = doc["triggers"][i];
    triggers_[i].label = o["label"].is<const char *>() ? o["label"].as<String>() : ("Trigger " + String(i + 1));
    triggers_[i].command = o["command"] | "";
  }
  const char *keys[] = {"bools", "ints", "floats", "texts"};
  std::array<ValueSlot, 8> *groups[] = {&bools_, &ints_, &floats_, &texts_};
  for (int group = 0; group < 4; ++group) {
    for (int i = 0; i < 8; ++i) {
      JsonObject o = doc[keys[group]][i];
      (*groups[group])[i].label = o["label"].is<const char *>()
          ? o["label"].as<String>()
          : (String(keys[group]) + " " + String(i + 1));
      (*groups[group])[i].source = o["source"] | "";
    }
  }
  return true;
}

CommandResult VsmGeneric::trigger(int slot) {
  if (slot < 1 || slot > 16) return {false, 404, "Invalid trigger slot", 0};
  String command = triggers_[slot - 1].command;
  command.trim();
  if (command.length() == 0) return {false, 409, "Trigger slot is not mapped", 0};
  if (command.startsWith("sequence:")) {
    int seq = command.substring(9).toInt();
    return sequences_.trigger(seq);
  }
  if (!store_.validCommandId(command)) return {false, 409, "Trigger maps to unknown command", 0};
  return engine_.execute(command);
}

String VsmGeneric::sourceText(const String &source) const {
  if (source.startsWith("sequence.")) {
    int firstDot = source.indexOf('.', 9);
    if (firstDot > 9) {
      int slot = source.substring(9, firstDot).toInt();
      String field = source.substring(firstDot + 1);
      if (field == "state") return sequences_.stateTextFor(slot);
      if (field == "state_code") return String(sequences_.stateCodeFor(slot));
      if (field == "phase") return sequences_.phaseFor(slot);
      if (field == "error") return sequences_.errorFor(slot);
      if (field == "busy") return sequences_.busyFor(slot) ? "true" : "false";
    }
    return String();
  }
  if (source.startsWith("camera.")) {
    String id = source.substring(7);
    const CameraStateValue *state = cameraState_.get(id);
    if (!state) return String();
    if (!cameraState_.isFresh(id, store_.config().telemetryFreshMs)) return String();
    return state->raw;
  }
  if (source == "bridge.camera_reachable") return (status_.cameraReachable && network_.wifiConnected()) ? "true" : "false";
  if (source == "bridge.telemetry_fresh") {
    bool fresh = status_.lastTelemetryMs != 0 && (millis() - status_.lastTelemetryMs) <= store_.config().telemetryFreshMs && status_.cameraReachable;
    return fresh ? "true" : "false";
  }
  if (source == "bridge.telemetry_age_ms") return status_.lastTelemetryMs ? String(millis() - status_.lastTelemetryMs) : String(-1);
  if (source == "bridge.macro_busy") return status_.macroBusy ? "true" : "false";
  if (source == "bridge.wifi_connected") return network_.wifiConnected() ? "true" : "false";
  if (source == "bridge.ethernet_connected") return network_.ethernetConnected() ? "true" : "false";
  if (source == "bridge.last_command") return status_.lastCommand;
  if (source == "bridge.last_error") return status_.lastError;
  if (source == "bridge.wifi_rssi") return String(status_.wifiRssi);
  if (source == "bridge.last_sony_http") return String(status_.lastSonyHttpStatus);
  return String();
}

bool VsmGeneric::parseBool(String value, bool &out) {
  value.trim(); value.toLowerCase();
  if (value == "true" || value == "1" || value == "on" || value == "yes" ||
      value == "rec" || value == "recording" || value == "play" || value == "playing") {
    out = true; return true;
  }
  if (value == "false" || value == "0" || value == "off" || value == "no" ||
      value == "stop" || value == "stopped" || value == "idle") {
    out = false; return true;
  }
  return false;
}

bool VsmGeneric::parseInt(String value, long &out) {
  value.trim();
  if (!value.length()) return false;
  errno = 0;
  char *end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (errno == ERANGE || end == value.c_str() || !end || *end != '\0') return false;
  out = parsed;
  return true;
}

bool VsmGeneric::parseFloat(String value, double &out) {
  value.trim();
  if (!value.length()) return false;
  errno = 0;
  char *end = nullptr;
  const double parsed = strtod(value.c_str(), &end);
  if (errno == ERANGE || end == value.c_str() || !end || *end != '\0' || !std::isfinite(parsed)) return false;
  out = parsed;
  return true;
}

String VsmGeneric::feedbackJson() const {
  JsonDocument doc;
  JsonObject boolRoot = doc["Bool"].to<JsonObject>();
  JsonObject intRoot = doc["Int"].to<JsonObject>();
  JsonObject floatRoot = doc["Float"].to<JsonObject>();
  JsonObject textRoot = doc["Text"].to<JsonObject>();
  for (int i = 0; i < 8; ++i) {
    String key = "B" + String(i + 1);
    String value = sourceText(bools_[i].source);
    bool boolValue = false;
    if (parseBool(value, boolValue)) boolRoot[key] = boolValue; else boolRoot[key] = nullptr;
    key = "I" + String(i + 1);
    value = sourceText(ints_[i].source);
    long intValue = 0;
    if (parseInt(value, intValue)) intRoot[key] = intValue; else intRoot[key] = nullptr;
    key = "F" + String(i + 1);
    value = sourceText(floats_[i].source);
    double floatValue = 0;
    if (parseFloat(value, floatValue)) floatRoot[key] = floatValue; else floatRoot[key] = nullptr;
    key = "T" + String(i + 1);
    value = sourceText(texts_[i].source);
    if (value.length()) textRoot[key] = value; else textRoot[key] = nullptr;
  }
  String out; serializeJson(doc, out); return out;
}

String VsmGeneric::slotsJson() const {
  JsonDocument doc;
  JsonArray tr = doc["triggers"].to<JsonArray>();
  for (int i = 0; i < 16; ++i) { JsonObject o = tr.add<JsonObject>(); o["slot"] = i + 1; o["label"] = triggers_[i].label; o["command"] = triggers_[i].command; }
  const char *keys[] = {"bools", "ints", "floats", "texts"};
  const std::array<ValueSlot, 8> *groups[] = {&bools_, &ints_, &floats_, &texts_};
  for (int group = 0; group < 4; ++group) {
    JsonArray arr = doc[keys[group]].to<JsonArray>();
    for (int i = 0; i < 8; ++i) {
      JsonObject o = arr.add<JsonObject>();
      o["slot"] = i + 1;
      o["label"] = (*groups[group])[i].label;
      o["source"] = (*groups[group])[i].source;
    }
  }
  String out; serializeJson(doc, out); return out;
}
