#pragma once

#include <Arduino.h>
#include <ETH.h>
#include <Network.h>
#include <WiFi.h>
#include "AppTypes.h"

// Avoid colliding with Arduino-ESP32 3.x's global NetworkManager class.
class BridgeNetworkManager {
 public:
  explicit BridgeNetworkManager(BridgeConfig &cfg) : cfg_(cfg) {}

  void begin();
  void loop();
  void reconnectCameraWifi();

  bool ethernetStarted() const { return ethStarted_; }
  bool ethernetConnected() const { return ethConnected_; }
  bool ethernetLinkUp() const { return ethStarted_ && ETH.linkUp(); }
  uint16_t ethernetLinkSpeedMbps() const { return ethernetLinkUp() ? ETH.linkSpeed() : 0; }
  bool ethernetFullDuplex() const { return ethernetLinkUp() && ETH.fullDuplex(); }
  String ethernetMac() const { return ethStarted_ ? ETH.macAddress() : String(); }
  bool ethernetDhcpConfigured() const { return cfg_.ethernetDhcp; }
  String ethernetSubnet() const;
  String ethernetGateway() const;
  String ethernetDns() const;
  bool wifiConnected() const { return WiFi.status() == WL_CONNECTED; }
  bool subnetsOverlap() const;
  String ethernetIp() const;
  String wifiIp() const;

 private:
  BridgeConfig &cfg_;
  bool ethStarted_ = false;
  bool ethConnected_ = false;
  uint32_t lastWifiAttemptMs_ = 0;

  static BridgeNetworkManager *instance_;
  static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info);
  void handleNetworkEvent(arduino_event_id_t event, arduino_event_info_t info);
  bool parseIp(const String &s, IPAddress &out) const;
};
