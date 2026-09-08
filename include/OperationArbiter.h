#pragma once

#include <Arduino.h>

enum class OperationOwner : uint8_t {
  None = 0,
  Manual,
  StopReplay,
  Sequence1,
  Sequence2,
  Sequence3,
  Sequence4,
  Sequence5,
  Sequence6,
  Sequence7,
  Sequence8,
  FirmwareUpdate,
};

class OperationArbiter {
 public:
  bool acquire(OperationOwner owner) {
    if (owner == OperationOwner::None || owner_ != OperationOwner::None) return false;
    owner_ = owner;
    acquiredMs_ = millis();
    return true;
  }

  bool release(OperationOwner owner) {
    if (owner_ != owner) return false;
    owner_ = OperationOwner::None;
    acquiredMs_ = 0;
    return true;
  }

  OperationOwner owner() const { return owner_; }
  bool busy() const { return owner_ != OperationOwner::None; }
  bool ownedBy(OperationOwner owner) const { return owner_ == owner; }
  bool automationActive() const { return owner_ == OperationOwner::StopReplay || isSequence(owner_); }
  bool firmwareUpdateActive() const { return owner_ == OperationOwner::FirmwareUpdate; }
  uint32_t elapsedMs() const { return busy() ? millis() - acquiredMs_ : 0; }

  static bool isSequence(OperationOwner owner) {
    return owner >= OperationOwner::Sequence1 && owner <= OperationOwner::Sequence8;
  }
  static OperationOwner sequenceOwner(size_t zeroBasedSlot) {
    return zeroBasedSlot < 8
        ? static_cast<OperationOwner>(static_cast<uint8_t>(OperationOwner::Sequence1) + zeroBasedSlot)
        : OperationOwner::None;
  }
  static const char *name(OperationOwner owner) {
    switch (owner) {
      case OperationOwner::None: return "NONE";
      case OperationOwner::Manual: return "MANUAL";
      case OperationOwner::StopReplay: return "STOP_REPLAY";
      case OperationOwner::Sequence1: return "SEQUENCE_1";
      case OperationOwner::Sequence2: return "SEQUENCE_2";
      case OperationOwner::Sequence3: return "SEQUENCE_3";
      case OperationOwner::Sequence4: return "SEQUENCE_4";
      case OperationOwner::Sequence5: return "SEQUENCE_5";
      case OperationOwner::Sequence6: return "SEQUENCE_6";
      case OperationOwner::Sequence7: return "SEQUENCE_7";
      case OperationOwner::Sequence8: return "SEQUENCE_8";
      case OperationOwner::FirmwareUpdate: return "FIRMWARE_UPDATE";
    }
    return "UNKNOWN";
  }

 private:
  OperationOwner owner_ = OperationOwner::None;
  uint32_t acquiredMs_ = 0;
};
