#pragma once

#include <Arduino.h>
#include <Network.h>
#include "AppTypes.h"
#include "CameraState.h"
#include "CommandEngine.h"
#include "ConfigStore.h"
#include "NetworkManager.h"
#include "SequenceEngine.h"
#include "OperationArbiter.h"

class ControlProtocol {
 public:
  ControlProtocol(CommandEngine &engine, SequenceEngine &sequences, ConfigStore &store,
                  BridgeNetworkManager &network, CameraStateStore &cameraState,
                  RuntimeStatus &status, OperationArbiter &arbiter);
  void begin();
  void loop();

 private:
  NetworkServer server_;
  NetworkClient client_;
  CommandEngine &engine_;
  SequenceEngine &sequences_;
  ConfigStore &store_;
  BridgeNetworkManager &network_;
  CameraStateStore &cameraState_;
  RuntimeStatus &status_;
  OperationArbiter &arbiter_;
  String rx_;
  uint32_t lastClientActivityMs_ = 0;

  String processLine(String line);
  String oneLineStatus();
};
