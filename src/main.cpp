#include <Arduino.h>
#include "AppTypes.h"
#include "CameraState.h"
#include "CommandEngine.h"
#include "ConfigStore.h"
#include "ControlProtocol.h"
#include "NetworkManager.h"
#include "SonyRemote.h"
#include "SonyProxy.h"
#include "SequenceEngine.h"
#include "TelemetryManager.h"
#include "VsmGeneric.h"
#include "WebApp.h"
#include "OperationArbiter.h"

ConfigStore configStore;
RuntimeStatus runtimeStatus;
CameraStateStore cameraState;
OperationArbiter operationArbiter;
BridgeNetworkManager networkManager(configStore.configMutable());
SonyRemote sonyRemote(configStore.configMutable());
SonyProxy sonyProxy(configStore.configMutable());
CommandEngine commandEngine(configStore, sonyRemote, cameraState, runtimeStatus, operationArbiter);
TelemetryManager telemetryManager(configStore, sonyRemote, cameraState, runtimeStatus);
SequenceEngine sequenceEngine(configStore, commandEngine, cameraState, operationArbiter);
VsmGeneric vsmGeneric(configStore, commandEngine, sequenceEngine, cameraState, networkManager, runtimeStatus);
WebApp webApp(configStore, networkManager, sonyRemote, sonyProxy, commandEngine, cameraState,
              telemetryManager, sequenceEngine, vsmGeneric, runtimeStatus, operationArbiter);
ControlProtocol controlProtocol(commandEngine, sequenceEngine, configStore, networkManager, cameraState,
                                runtimeStatus, operationArbiter);

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Sony Camera Network Bridge");
  Serial.printf("Version: %s\n", FS7B_VERSION);
#ifdef FS7B_BOARD_PROFILE
  Serial.printf("Board profile: %s\n", FS7B_BOARD_PROFILE);
#else
  Serial.println("Board profile: WT32-ETH01-v1.4");
#endif
  Serial.println("Ethernet: LAN8720A RMII (PHY 1, MDC 23, MDIO 18, OSC_EN 16, CLK GPIO0_IN)");
  Serial.println("----------------------------------------");

  if (!configStore.begin()) Serial.println("FATAL: persistent storage could not be initialized");

  String error;
  telemetryManager.begin();
  if (!sequenceEngine.begin(error)) Serial.printf("[sequence] config load failed: %s\n", error.c_str());
  if (!vsmGeneric.reload(error)) Serial.printf("[vsm] generic map load failed: %s\n", error.c_str());
  networkManager.begin();
  sonyProxy.begin();
  webApp.begin();
  controlProtocol.begin();

  Serial.println("[system] bridge started");
  Serial.println("[system] setup: browse to the Ethernet IP shown above");
}

void loop() {
  networkManager.loop();
  commandEngine.loop();
  sequenceEngine.loop();
  webApp.loop();
  controlProtocol.loop();
  telemetryManager.loop();
  delay(1);
}
