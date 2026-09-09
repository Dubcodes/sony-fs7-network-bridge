#pragma once

#include <Arduino.h>
#include <array>

#ifndef FS7B_VERSION
#define FS7B_VERSION "dev"
#endif

struct BridgeConfig {
  String deviceName = "FS7-WiFi-Bridge";

  // Normal LAN default: DHCP. The static fields remain as an explicit
  // direct-connect/recovery profile and are only applied when DHCP is disabled.
  bool ethernetDhcp = true;
  String ethernetIp = "10.77.7.2";
  String ethernetGateway = "0.0.0.0";
  String ethernetSubnet = "255.255.255.0";
  String ethernetDns = "0.0.0.0";

  String cameraSsid;
  String cameraWifiPassword;
  String cameraHost = "192.168.1.1";
  String cameraUsername = "admin";
  String cameraPassword = "pxw-fs7";
  uint32_t cameraTimeoutMs = 1800;

  uint32_t replayDelayMs = 3000;
  uint32_t recReviewSetDelayMs = 4500;
  uint32_t recReviewPlayDelayMs = 2500;
  uint32_t replayTimeoutMs = 25000;
  uint32_t telemetryFreshMs = 2500;
  bool dryRun = false;
};

struct SonyCommandMapping {
  String id;
  // "http" retains the original discovery/manual HTTP transport. "linear"
  // uses Sony's public-WebUI-derived MessagePack RPC over WebSocket /linear.
  String transport = "http";
  String method = "POST";
  String path;
  String contentType = "application/x-www-form-urlencoded";
  String body;
  String extraHeaders;
  String rpcMethod;
  String rpcParams = "[]";

  bool configured() const {
    return transport == "linear" ? rpcMethod.length() > 0 : path.length() > 0;
  }
};

struct CommandDescriptor {
  const char *id;
  const char *label;
  const char *group;
  const char *style;
  bool defaultVisible;
};

constexpr size_t COMMAND_COUNT = 57;
extern const std::array<CommandDescriptor, COMMAND_COUNT> COMMANDS;
bool isHoldOnlyCommand(const String &id);

struct CameraStateDescriptor {
  const char *id;
  const char *label;
  const char *group;
  const char *type;  // bool, int, float, string
  bool defaultVisible;
};

constexpr size_t CAMERA_STATE_COUNT = 23;
extern const std::array<CameraStateDescriptor, CAMERA_STATE_COUNT> CAMERA_STATES;

struct SonyResponse {
  bool transportOk = false;
  bool commandOk = false;
  bool isLinear = false;
  int httpStatus = 0;
  String headers;
  String body;
  String rpcMethod;
  String error;
};

struct RuntimeStatus {
  bool ethernetStarted = false;
  bool ethernetConnected = false;
  String ethernetIp;

  bool wifiConnected = false;
  String wifiIp;
  int wifiRssi = -127;

  bool cameraReachable = false;
  bool optimisticRecording = false;
  bool optimisticPlayback = false;
  uint32_t optimisticPlaybackUntilMs = 0;
  bool macroBusy = false;
  String macroPhase = "READY";
  uint32_t macroStartedMs = 0;
  bool subnetOverlap = false;

  String lastCommand;
  String lastError;
  int lastSonyHttpStatus = 0;
  uint32_t lastSonyResponseMs = 0;
  uint32_t lastTelemetryMs = 0;
  String lastTelemetrySource;
};
