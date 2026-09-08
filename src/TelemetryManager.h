#pragma once

#include <Arduino.h>
#include <array>
#include "AppTypes.h"
#include "CameraState.h"
#include "ConfigStore.h"
#include "SonyRemote.h"

class TelemetryManager {
 public:
  TelemetryManager(ConfigStore &store, SonyRemote &sony, CameraStateStore &cameraState,
                   RuntimeStatus &status)
      : store_(store), sony_(sony), cameraState_(cameraState), status_(status) {}

  void begin();
  void loop();
  bool reload(String &error);
  String diagnosticsJson() const;

 private:
  static constexpr size_t SOURCE_COUNT = 4;

  struct Source {
    String id;
    bool enabled = false;
    String method = "GET";
    String path;
    String contentType = "application/x-www-form-urlencoded";
    String body;
    String extraHeaders;
    uint32_t intervalMs = 500;
    uint32_t timeoutMs = 600;
    uint32_t nextDueMs = 0;
    uint8_t failureCount = 0;
    uint32_t currentDelayMs = 0;
    int lastHttpStatus = 0;
    uint32_t lastResponseMs = 0;
    String lastError;
    String lastBodyPreview;
  };

  struct FieldRule {
    String stateId;
    String sourceId;
    String mode = "json";  // json, between, whole, header
    String selector;
    String startToken;
    String endToken;
    String trueValues;   // comma-separated exact values -> true
    String falseValues;  // comma-separated exact values -> false
  };

  ConfigStore &store_;
  SonyRemote &sony_;
  CameraStateStore &cameraState_;
  RuntimeStatus &status_;
  std::array<Source, SOURCE_COUNT> sources_{};
  std::array<FieldRule, CAMERA_STATE_COUNT> fields_{};

  void poll(Source &source);
  String extract(const FieldRule &rule, const SonyResponse &response, bool &ok) const;
  String extractJson(const String &body, const String &selector, bool &ok) const;
  String extractBetween(const String &body, const String &startToken,
                        const String &endToken, bool &ok) const;
  String extractHeader(const String &headers, const String &name, bool &ok) const;
  String normalize(const FieldRule &rule, const String &value) const;
  bool listContains(const String &csv, const String &value) const;
};
