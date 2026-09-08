#pragma once

#include <Arduino.h>
#include <array>
#include "AppTypes.h"
#include "CameraState.h"
#include "CommandEngine.h"
#include "ConfigStore.h"
#include "NetworkManager.h"
#include "SequenceEngine.h"

class VsmGeneric {
 public:
  VsmGeneric(ConfigStore &store, CommandEngine &engine, SequenceEngine &sequences, CameraStateStore &cameraState,
             BridgeNetworkManager &network, RuntimeStatus &status)
      : store_(store), engine_(engine), sequences_(sequences), cameraState_(cameraState), network_(network), status_(status) {}

  bool reload(String &error);
  CommandResult trigger(int slot);
  String feedbackJson() const;
  String slotsJson() const;

 private:
  struct TriggerSlot { String label; String command; };
  struct ValueSlot { String label; String source; };

  ConfigStore &store_;
  CommandEngine &engine_;
  SequenceEngine &sequences_;
  CameraStateStore &cameraState_;
  BridgeNetworkManager &network_;
  RuntimeStatus &status_;
  std::array<TriggerSlot, 16> triggers_{};
  std::array<ValueSlot, 8> bools_{};
  std::array<ValueSlot, 8> ints_{};
  std::array<ValueSlot, 8> floats_{};
  std::array<ValueSlot, 8> texts_{};

  String sourceText(const String &source) const;
  static bool parseBool(String value, bool &out);
  static bool parseInt(String value, long &out);
  static bool parseFloat(String value, double &out);
};
