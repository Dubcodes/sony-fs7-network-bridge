#pragma once

#include <Arduino.h>
#include "AppTypes.h"
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
  bool wifiConnected() const;

 private:
  BridgeConfig &cfg_;
  SonyLinear linear_;
  String basicAuthorization() const;
  bool resolveCamera(struct sockaddr_in &remote, String &error) const;
};
