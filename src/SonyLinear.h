#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "AppTypes.h"

class CameraStateStore;

class SonyLinear {
 public:
  explicit SonyLinear(BridgeConfig &cfg) : cfg_(cfg) {}

  SonyResponse request(const String &method, const String &paramsJson = "[]",
                       uint32_t timeoutOverrideMs = 0);
  SonyResponse test();
  void begin(CameraStateStore &cameraState, RuntimeStatus &status);
  void loop();
  bool connected() const { return fd_ >= 0; }

 private:
  BridgeConfig &cfg_;
  uint32_t nextRequestId_ = 1;
  int fd_ = -1;
  String connectedWifiIp_;
  uint32_t nextConnectAttemptMs_ = 0;
  CameraStateStore *cameraState_ = nullptr;
  RuntimeStatus *status_ = nullptr;
  bool resolveCamera(struct sockaddr_in &remote, String &error) const;
  int openBoundSocket(struct sockaddr_in &remote, uint32_t timeoutMs, String &error) const;
  bool websocketHandshake(int fd, uint32_t timeoutMs, String &error, int &httpStatus) const;
  bool ensureConnected(uint32_t timeoutMs, String &error, int &httpStatus);
  bool subscribeAndPrime(uint32_t timeoutMs, String &error);
  void closeConnection();
  void setSocketTimeout(uint32_t timeoutMs) const;
  void handleIncoming(const std::vector<uint8_t> &payload);
  void mergeProperties(JsonVariantConst value);
  void applyProperty(const String &name, JsonVariantConst value);
  bool sendRpc(int fd, uint32_t id, const String &method, const String &paramsJson,
               uint32_t timeoutMs, String &error, String &resultSummary);

  String basicAuthorization() const;
};
