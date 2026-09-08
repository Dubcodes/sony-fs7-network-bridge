#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "AppTypes.h"
#include "CameraState.h"
#include "CommandEngine.h"
#include "ConfigStore.h"
#include "NetworkManager.h"
#include "SonyRemote.h"
#include "SonyProxy.h"
#include "SequenceEngine.h"
#include "TelemetryManager.h"
#include "VsmGeneric.h"
#include "OperationArbiter.h"

class WebApp {
 public:
  WebApp(ConfigStore &store, BridgeNetworkManager &network, SonyRemote &sony, SonyProxy &proxy,
         CommandEngine &engine, CameraStateStore &cameraState,
         TelemetryManager &telemetry, SequenceEngine &sequences, VsmGeneric &vsm,
         RuntimeStatus &status, OperationArbiter &arbiter);
  void begin();
  void loop();

 private:
  WebServer server_;
  ConfigStore &store_;
  BridgeNetworkManager &network_;
  SonyRemote &sony_;
  SonyProxy &proxy_;
  CommandEngine &engine_;
  CameraStateStore &cameraState_;
  TelemetryManager &telemetry_;
  SequenceEngine &sequences_;
  VsmGeneric &vsm_;
  RuntimeStatus &status_;
  OperationArbiter &arbiter_;

  bool otaInProgress_ = false;
  bool otaSuccess_ = false;
  bool otaRejectedBusy_ = false;
  String otaError_;
  String otaFilename_;
  size_t otaBytesWritten_ = 0;
  uint32_t otaStartedMs_ = 0;
  uint32_t otaLastActivityMs_ = 0;
  bool otaRebootPending_ = false;
  uint32_t otaRebootAtMs_ = 0;

  void sendJson(int code, const String &json);
  void sendMessage(int code, const String &message, bool ok = false);
  bool requireBodyUnder(size_t maxBytes);
  bool requireAutomationIdle(const char *action);
  String statusJson();
  String systemJson();
  String catalogJson();
  void handleFirmwareUpload();
  void finishFirmwareUploadRequest();
  void failFirmwareUpload(const String &message);
  void handleNotFound();
};
