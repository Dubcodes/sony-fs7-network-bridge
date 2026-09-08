#pragma once

#include <Arduino.h>
#include <array>
#include <vector>
#include "AppTypes.h"

class SonyProxy {
 public:
  explicit SonyProxy(BridgeConfig &cfg) : cfg_(cfg) {}

  void begin();

  bool captureEnabled() const { return captureEnabled_; }
  void startCapture();
  void stopCapture();
  void clearCapture();
  String captureJson();
  uint16_t port() const { return 8081; }

 private:
  struct CaptureEntry {
    uint32_t seq = 0;
    uint32_t ms = 0;
    char direction[8] = {0};
    char kind[12] = {0};
    char summary[176] = {0};
    char hex[513] = {0};
    uint16_t payloadBytes = 0;
    bool truncated = false;
  };

  struct SessionContext {
    SonyProxy *self = nullptr;
    int clientFd = -1;
  };

  struct WsSniffer {
    SonyProxy *owner = nullptr;
    bool fromBrowser = false;
    std::vector<uint8_t> buffer;
    void feed(const uint8_t *data, size_t len);
  };

  BridgeConfig &cfg_;
  volatile bool captureEnabled_ = false;
  std::array<CaptureEntry, 16> capture_{};
  uint8_t captureHead_ = 0;
  uint8_t captureCount_ = 0;
  uint32_t captureSeq_ = 0;
  portMUX_TYPE captureMux_ = portMUX_INITIALIZER_UNLOCKED;
  portMUX_TYPE sessionMux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t activeSessions_ = 0;
  TaskHandle_t listenerTask_ = nullptr;

  static void listenerTaskThunk(void *arg);
  static void sessionTaskThunk(void *arg);
  void listenerTask();
  void sessionTask(int clientFd);

  bool claimSession();
  void releaseSession();
  int connectCamera(uint32_t timeoutMs, String &error) const;
  String basicAuthorization() const;

  void captureEvent(const char *direction, const char *kind, const String &summary,
                    const uint8_t *payload = nullptr, size_t payloadLen = 0);
  void captureWsPayload(bool fromBrowser, uint8_t opcode, const std::vector<uint8_t> &payload);
  void captureHttpRequest(const String &method, const String &path,
                          const uint8_t *body, size_t bodyLen);
};
