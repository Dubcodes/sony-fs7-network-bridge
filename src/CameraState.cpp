#include "CameraState.h"

namespace {
bool textBool(String v, bool fallback) {
  v.trim();
  v.toLowerCase();
  if (v == "1" || v == "true" || v == "on" || v == "yes" || v == "rec" || v == "recording" || v == "play" || v == "playing" || v == "ready" || v == "writing" || v == "buffer_ready") return true;
  if (v == "0" || v == "false" || v == "off" || v == "no" || v == "stby" || v == "standby" || v == "stop" || v == "stopped") return false;
  return fallback;
}
}

int CameraStateStore::indexOf(const String &id) const {
  for (size_t i = 0; i < CAMERA_STATES.size(); ++i) {
    if (id == CAMERA_STATES[i].id) return static_cast<int>(i);
  }
  return -1;
}

bool CameraStateStore::validStateId(const String &id) const { return indexOf(id) >= 0; }

bool CameraStateStore::set(const String &id, const String &rawIn) {
  int idx = indexOf(id);
  if (idx < 0) return false;
  String raw = rawIn;
  raw.trim();
  if (raw.length() >= 2 && ((raw[0] == '"' && raw[raw.length() - 1] == '"') || (raw[0] == '\'' && raw[raw.length() - 1] == '\''))) {
    raw = raw.substring(1, raw.length() - 1);
  }
  values_[idx].raw = raw;
  values_[idx].valid = true;
  values_[idx].updatedMs = millis();
  return true;
}

bool CameraStateStore::clear(const String &id) {
  int idx = indexOf(id);
  if (idx < 0) return false;
  values_[idx] = CameraStateValue{};
  return true;
}

const CameraStateValue *CameraStateStore::get(const String &id) const {
  int idx = indexOf(id);
  if (idx < 0) return nullptr;
  return &values_[idx];
}

String CameraStateStore::valueOr(const String &id, const String &fallback) const {
  const CameraStateValue *v = get(id);
  return (v && v->valid) ? v->raw : fallback;
}

bool CameraStateStore::boolValue(const String &id, bool fallback) const {
  const CameraStateValue *v = get(id);
  return (v && v->valid) ? textBool(v->raw, fallback) : fallback;
}

long CameraStateStore::intValue(const String &id, long fallback) const {
  const CameraStateValue *v = get(id);
  if (!v || !v->valid) return fallback;
  char *end = nullptr;
  long n = strtol(v->raw.c_str(), &end, 10);
  return (end && end != v->raw.c_str()) ? n : fallback;
}

double CameraStateStore::floatValue(const String &id, double fallback) const {
  const CameraStateValue *v = get(id);
  if (!v || !v->valid) return fallback;
  char *end = nullptr;
  double n = strtod(v->raw.c_str(), &end);
  return (end && end != v->raw.c_str()) ? n : fallback;
}

bool CameraStateStore::isFresh(const String &id, uint32_t freshMs) const {
  const CameraStateValue *v = get(id);
  return v && v->valid && (millis() - v->updatedMs <= freshMs);
}

String CameraStateStore::json(uint32_t freshMs) const {
  JsonDocument doc;
  JsonObject root = doc["cameraState"].to<JsonObject>();
  for (size_t i = 0; i < CAMERA_STATES.size(); ++i) {
    JsonObject item = root[CAMERA_STATES[i].id].to<JsonObject>();
    item["label"] = CAMERA_STATES[i].label;
    item["group"] = CAMERA_STATES[i].group;
    item["type"] = CAMERA_STATES[i].type;
    item["valid"] = values_[i].valid;
    item["fresh"] = values_[i].valid && (millis() - values_[i].updatedMs <= freshMs);
    if (values_[i].valid) item["value"] = values_[i].raw;
    else item["value"] = nullptr;
    item["ageMs"] = values_[i].valid ? (millis() - values_[i].updatedMs) : 0;
  }
  String out;
  serializeJson(doc, out);
  return out;
}
