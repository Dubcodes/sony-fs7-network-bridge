#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "AppTypes.h"

class SonyLinear {
 public:
  explicit SonyLinear(BridgeConfig &cfg) : cfg_(cfg) {}

  SonyResponse request(const String &method, const String &paramsJson = "[]",
                       uint32_t timeoutOverrideMs = 0);
  SonyResponse test();

 private:
  BridgeConfig &cfg_;
  uint32_t nextRequestId_ = 1;
  bool resolveCamera(struct sockaddr_in &remote, String &error) const;
  int openBoundSocket(struct sockaddr_in &remote, uint32_t timeoutMs, String &error) const;
  bool websocketHandshake(int fd, uint32_t timeoutMs, String &error, int &httpStatus) const;
  bool sendRpc(int fd, uint32_t id, const String &method, const String &paramsJson,
               uint32_t timeoutMs, String &error, String &resultSummary);

  String basicAuthorization() const;
};
