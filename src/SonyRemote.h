#pragma once

#include <Arduino.h>
#include "AppTypes.h"
#include "CameraState.h"
#include "SonyLinear.h"

class SonyRemote {
 public:
  explicit SonyRemote(BridgeConfig &cfg) : cfg_(cfg), linear_(cfg) {}

  SonyResponse send(const SonyCommandMapping &mapping, size_t maxBodyBytes = 16384);
  SonyResponse rawRequest(const String &method, const String &path,
                          const String &body = "",
                          const String &contentType = "text/plain",
                          const String &extraHeaders = "",
                          size_t maxBodyBytes = 32768,
                          uint32_t timeoutOverrideMs = 0);

  SonyResponse testConnection();
  SonyResponse testLinear();
  void begin(CameraStateStore &cameraState, RuntimeStatus &status);
  void loop();
  bool linearConnected() const { return linear_.connected(); }
  SonyResponse linearRequest(const String &method, const String &paramsJson,
                             uint32_t timeoutMs = 0);
  bool wifiConnected() const;

 private:
  BridgeConfig &cfg_;
  SonyLinear linear_;
  bool cameraSessionPrimed_ = false;
  String primedWifiIp_;
  void primeCameraSession();
  String basicAuthorization() const;
  bool resolveCamera(struct sockaddr_in &remote, String &error) const;
};
