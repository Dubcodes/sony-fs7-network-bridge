#include "TelemetryManager.h"
#include <ArduinoJson.h>
#include <algorithm>

void TelemetryManager::begin() {
  String error;
  if (!reload(error)) Serial.printf("[telemetry] config load failed: %s\n", error.c_str());
}

bool TelemetryManager::reload(String &error) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, store_.telemetryMapJson());
  if (e) {
    error = e.c_str();
    return false;
  }

  for (size_t i = 0; i < SOURCE_COUNT; ++i) {
    Source src;
    src.id = "source" + String(i + 1);
    JsonObject o = doc["sources"][i];
    if (!o.isNull()) {
      if (o["id"].is<const char *>()) src.id = o["id"].as<String>();
      src.enabled = o["enabled"] | false;
      src.method = o["method"] | "GET";
      src.path = o["path"] | "";
      src.contentType = o["contentType"] | "application/x-www-form-urlencoded";
      src.body = o["body"] | "";
      src.extraHeaders = o["extraHeaders"] | "";
      src.intervalMs = constrain(o["intervalMs"] | 500U, 100U, 10000U);
      src.timeoutMs = constrain(o["timeoutMs"] | 600U, 200U, 1500U);
    }
    src.nextDueMs = millis() + 200 + (i * 50);
    sources_[i] = src;
  }

  for (size_t i = 0; i < CAMERA_STATE_COUNT; ++i) {
    FieldRule rule;
    rule.stateId = CAMERA_STATES[i].id;
    JsonObject o = doc["fields"][rule.stateId];
    if (!o.isNull()) {
      rule.sourceId = o["source"] | "";
      rule.mode = o["mode"] | "json";
      rule.selector = o["selector"] | "";
      rule.startToken = o["startToken"] | "";
      rule.endToken = o["endToken"] | "";
      rule.trueValues = o["trueValues"] | "";
      rule.falseValues = o["falseValues"] | "";
    }
    fields_[i] = rule;
  }
  return true;
}

void TelemetryManager::loop() {
  const uint32_t now = millis();
  for (auto &source : sources_) {
    if (!source.enabled || source.path.length() == 0) continue;
    if (static_cast<int32_t>(now - source.nextDueMs) < 0) continue;
    poll(source);
    // One HTTP transaction per main-loop turn keeps camera control responsive.
    break;
  }
}

void TelemetryManager::poll(Source &source) {
  uint32_t started = millis();
  SonyResponse response = sony_.rawRequest(source.method, source.path, source.body,
                                           source.contentType, source.extraHeaders, 32768, source.timeoutMs);
  source.lastResponseMs = millis() - started;
  source.lastHttpStatus = response.httpStatus;
  source.lastError = response.error;
  source.lastBodyPreview = response.body.substring(0, 1024);
  status_.lastSonyHttpStatus = response.httpStatus;
  status_.lastSonyResponseMs = source.lastResponseMs;
  status_.cameraReachable = response.transportOk;

  if (!response.transportOk || response.httpStatus < 200 || response.httpStatus >= 300) {
    source.failureCount = std::min<uint8_t>(source.failureCount + 1, 6);
    const uint32_t multiplier = 1U << source.failureCount;
    source.currentDelayMs = std::min<uint32_t>(source.intervalMs * multiplier, 30000U);
    source.nextDueMs = millis() + source.currentDelayMs;
    const String reason = response.error.length() ? response.error : ("HTTP " + String(response.httpStatus));
    source.lastError = reason;
    status_.lastError = "Telemetry " + source.id + ": " + reason;
    return;
  }
  source.failureCount = 0;
  source.currentDelayMs = source.intervalMs;
  source.nextDueMs = millis() + source.intervalMs;

  bool updatedAnyState = false;
  for (const auto &rule : fields_) {
    if (rule.sourceId != source.id) continue;
    bool ok = false;
    String value = extract(rule, response, ok);
    if (!ok) continue;
    value = normalize(rule, value);
    if (!cameraState_.set(rule.stateId, value)) continue;
    updatedAnyState = true;
    if (rule.stateId == "recording") status_.optimisticRecording = cameraState_.boolValue("recording", status_.optimisticRecording);
    if (rule.stateId == "playback") {
      status_.optimisticPlayback = cameraState_.boolValue("playback", status_.optimisticPlayback);
      status_.optimisticPlaybackUntilMs = 0;
    }
  }
  // 'Telemetry fresh' means at least one configured semantic state was actually
  // updated from a successful response, not merely that a poll returned HTTP.
  if (updatedAnyState) {
    status_.lastTelemetryMs = millis();
    status_.lastTelemetrySource = source.id;
    if (status_.lastError.startsWith("Telemetry ")) status_.lastError = "";
  }
}

String TelemetryManager::extract(const FieldRule &rule, const SonyResponse &response, bool &ok) const {
  String mode = rule.mode;
  mode.toLowerCase();
  if (mode == "whole") {
    String out = response.body;
    out.trim();
    ok = out.length() > 0;
    return out;
  }
  if (mode == "between") return extractBetween(response.body, rule.startToken, rule.endToken, ok);
  if (mode == "header") return extractHeader(response.headers, rule.selector, ok);
  return extractJson(response.body, rule.selector, ok);
}

String TelemetryManager::extractJson(const String &body, const String &selector, bool &ok) const {
  ok = false;
  if (selector.length() == 0) return String();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return String();
  JsonVariantConst cur = doc.as<JsonVariantConst>();
  int start = 0;
  while (start <= selector.length()) {
    int dot = selector.indexOf('.', start);
    if (dot < 0) dot = selector.length();
    String key = selector.substring(start, dot);
    if (key.length() == 0 || !cur.is<JsonObjectConst>()) return String();
    cur = cur[key.c_str()];
    if (cur.isNull()) return String();
    if (dot == selector.length()) break;
    start = dot + 1;
  }

  String out;
  if (cur.is<const char *>()) out = cur.as<String>();
  else if (cur.is<bool>()) out = cur.as<bool>() ? "true" : "false";
  else if (cur.is<long>()) out = String(cur.as<long>());
  else if (cur.is<double>()) out = String(cur.as<double>(), 4);
  else serializeJson(cur, out);
  out.trim();
  ok = out.length() > 0;
  return out;
}

String TelemetryManager::extractBetween(const String &body, const String &startToken,
                                        const String &endToken, bool &ok) const {
  ok = false;
  if (startToken.length() == 0) return String();
  int start = body.indexOf(startToken);
  if (start < 0) return String();
  start += startToken.length();
  int end = endToken.length() ? body.indexOf(endToken, start) : body.length();
  if (end < 0) return String();
  String out = body.substring(start, end);
  out.trim();
  ok = out.length() > 0;
  return out;
}

String TelemetryManager::extractHeader(const String &headers, const String &nameIn, bool &ok) const {
  ok = false;
  String name = nameIn;
  name.toLowerCase();
  int start = 0;
  while (start < headers.length()) {
    int end = headers.indexOf("\r\n", start);
    if (end < 0) end = headers.length();
    String line = headers.substring(start, end);
    int colon = line.indexOf(':');
    if (colon > 0) {
      String key = line.substring(0, colon);
      key.trim();
      key.toLowerCase();
      if (key == name) {
        String value = line.substring(colon + 1);
        value.trim();
        ok = true;
        return value;
      }
    }
    start = end + 2;
  }
  return String();
}


bool TelemetryManager::listContains(const String &csv, const String &valueIn) const {
  String wanted = valueIn; wanted.trim(); wanted.toLowerCase();
  int start = 0;
  while (start <= csv.length()) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    String item = csv.substring(start, comma); item.trim(); item.toLowerCase();
    if (item.length() && item == wanted) return true;
    if (comma == csv.length()) break;
    start = comma + 1;
  }
  return false;
}

String TelemetryManager::normalize(const FieldRule &rule, const String &value) const {
  if (rule.trueValues.length() && listContains(rule.trueValues, value)) return "true";
  if (rule.falseValues.length() && listContains(rule.falseValues, value)) return "false";
  return value;
}

String TelemetryManager::diagnosticsJson() const {
  JsonDocument doc;
  JsonArray arr = doc["sources"].to<JsonArray>();
  for (const auto &source : sources_) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = source.id;
    o["enabled"] = source.enabled;
    o["configured"] = source.path.length() > 0;
    o["path"] = source.path;
    o["intervalMs"] = source.intervalMs; o["timeoutMs"] = source.timeoutMs;
    o["failureCount"] = source.failureCount; o["currentDelayMs"] = source.currentDelayMs;
    o["lastHttpStatus"] = source.lastHttpStatus;
    o["lastResponseMs"] = source.lastResponseMs;
    o["lastError"] = source.lastError;
    o["lastBodyPreview"] = source.lastBodyPreview;
  }
  String out;
  serializeJson(doc, out);
  return out;
}
