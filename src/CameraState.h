#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include "AppTypes.h"

struct CameraStateValue {
  String raw;
  bool valid = false;
  uint32_t updatedMs = 0;
};

class CameraStateStore {
 public:
  bool set(const String &id, const String &raw);
  bool clear(const String &id);
  const CameraStateValue *get(const String &id) const;
  String valueOr(const String &id, const String &fallback = "") const;
  bool boolValue(const String &id, bool fallback = false) const;
  long intValue(const String &id, long fallback = 0) const;
  double floatValue(const String &id, double fallback = 0.0) const;
  bool isFresh(const String &id, uint32_t freshMs) const;
  String json(uint32_t freshMs) const;
  bool validStateId(const String &id) const;

 private:
  std::array<CameraStateValue, CAMERA_STATE_COUNT> values_{};
  int indexOf(const String &id) const;
};
