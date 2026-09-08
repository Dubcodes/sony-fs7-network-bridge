#include "NetworkManager.h"

#ifndef FS7B_BOARD_PROFILE
#define FS7B_BOARD_PROFILE "WT32-ETH01"
#endif
#ifndef FS7B_ETH_PHY_ADDR
#define FS7B_ETH_PHY_ADDR 1
#endif
#ifndef FS7B_ETH_MDC
#define FS7B_ETH_MDC 23
#endif
#ifndef FS7B_ETH_MDIO
#define FS7B_ETH_MDIO 18
#endif
#ifndef FS7B_ETH_POWER
#define FS7B_ETH_POWER 16
#endif

BridgeNetworkManager *BridgeNetworkManager::instance_ = nullptr;

void BridgeNetworkManager::begin() {
  instance_ = this;
  Network.onEvent(onNetworkEvent);

  Serial.printf("[net] board profile: %s\n", FS7B_BOARD_PROFILE);
  Serial.printf("[net] starting LAN8720 RMII: PHY=%d MDC=%d MDIO=%d OSC_EN=%d CLK=GPIO0_IN\n",
                FS7B_ETH_PHY_ADDR, FS7B_ETH_MDC, FS7B_ETH_MDIO, FS7B_ETH_POWER);

  // WT32-ETH01 v1.4:
  //   LAN8720A PHY address 1
  //   MDC GPIO23, MDIO GPIO18
  //   GPIO16 enables the external 50 MHz oscillator
  //   GPIO0 receives that oscillator (RMII external clock input)
  //
  // Arduino-ESP32 3.x argument order:
  // begin(type, phy_addr, mdc, mdio, power, clock_mode)
  ethStarted_ = ETH.begin(ETH_PHY_LAN8720, FS7B_ETH_PHY_ADDR,
                          FS7B_ETH_MDC, FS7B_ETH_MDIO,
                          FS7B_ETH_POWER, ETH_CLOCK_GPIO0_IN);

  if (ethStarted_) {
    ETH.setHostname(cfg_.deviceName.c_str());
    Serial.printf("[net] Ethernet addressing: %s\n", cfg_.ethernetDhcp ? "DHCP" : "STATIC");
    // Ethernet is the preferred general management/control route.
    // Sony camera sockets are separately source-bound to Wi-Fi.
    ETH.setRoutePrio(120);
    if (!cfg_.ethernetDhcp) {
      IPAddress ip, gw, mask, dns;
      if (parseIp(cfg_.ethernetIp, ip) && parseIp(cfg_.ethernetGateway, gw) &&
          parseIp(cfg_.ethernetSubnet, mask) && parseIp(cfg_.ethernetDns, dns)) {
        if (!ETH.config(ip, gw, mask, dns)) {
          Serial.println("[net] warning: ETH static config failed; DHCP may remain active");
        }
      } else {
        Serial.println("[net] warning: invalid static Ethernet address; leaving DHCP active");
      }
    } else {
      Serial.println("[net] waiting for DHCP lease; no automatic static fallback is enabled");
    }
  } else {
    Serial.println("[net] ERROR: WT32 LAN8720 Ethernet failed to start");
  }

  WiFi.mode(WIFI_STA);
  WiFi.STA.setRoutePrio(50);
  WiFi.setSleep(false);
  reconnectCameraWifi();
}

void BridgeNetworkManager::loop() {
  // Reconnect in a bounded way without blocking the web/VSM servers.
  if (cfg_.cameraSsid.length() && WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiAttemptMs_ > 10000) {
    reconnectCameraWifi();
  }
}

void BridgeNetworkManager::reconnectCameraWifi() {
  lastWifiAttemptMs_ = millis();
  WiFi.disconnect(false, false);
  if (!cfg_.cameraSsid.length()) {
    Serial.println("[wifi] camera SSID is not configured");
    return;
  }
  Serial.printf("[wifi] joining camera AP '%s'\n", cfg_.cameraSsid.c_str());
  WiFi.begin(cfg_.cameraSsid.c_str(), cfg_.cameraWifiPassword.c_str());
}

void BridgeNetworkManager::onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (instance_) instance_->handleNetworkEvent(event, info);
}

void BridgeNetworkManager::handleNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("[eth] interface started");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.printf("[eth] link up: %u Mbps %s duplex\n",
                    ETH.linkSpeed(), ETH.fullDuplex() ? "full" : "half");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ethConnected_ = true;
      Serial.printf("[eth] %s IP %s  MAC %s\n",
                    cfg_.ethernetDhcp ? "DHCP" : "static",
                    ETH.localIP().toString().c_str(), ETH.macAddress().c_str());
      Serial.printf("[eth] subnet %s  gateway %s  DNS %s\n",
                    ETH.subnetMask().toString().c_str(),
                    ETH.gatewayIP().toString().c_str(),
                    ETH.dnsIP().toString().c_str());
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      ethConnected_ = false;
      Serial.println("[eth] lost IP");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      ethConnected_ = false;
      Serial.println("[eth] link down");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      ethConnected_ = false;
      Serial.println("[eth] interface stopped");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[wifi] camera network IP %s, RSSI %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (subnetsOverlap()) {
        Serial.println("[net] NOTE: Ethernet and camera Wi-Fi subnets overlap. Sony sockets are source-bound to Wi-Fi.");
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[wifi] camera AP disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
      break;
    default:
      break;
  }
}

bool BridgeNetworkManager::parseIp(const String &s, IPAddress &out) const {
  return out.fromString(s);
}

String BridgeNetworkManager::ethernetIp() const {
  return ethConnected_ ? ETH.localIP().toString() : String();
}

String BridgeNetworkManager::ethernetSubnet() const {
  return ethConnected_ ? ETH.subnetMask().toString() : String();
}

String BridgeNetworkManager::ethernetGateway() const {
  return ethConnected_ ? ETH.gatewayIP().toString() : String();
}

String BridgeNetworkManager::ethernetDns() const {
  return ethConnected_ ? ETH.dnsIP().toString() : String();
}

String BridgeNetworkManager::wifiIp() const {
  return wifiConnected() ? WiFi.localIP().toString() : String();
}

bool BridgeNetworkManager::subnetsOverlap() const {
  if (!ethConnected_ || WiFi.status() != WL_CONNECTED) return false;
  uint32_t ea = static_cast<uint32_t>(ETH.localIP());
  uint32_t em = static_cast<uint32_t>(ETH.subnetMask());
  uint32_t wa = static_cast<uint32_t>(WiFi.localIP());
  uint32_t wm = static_cast<uint32_t>(WiFi.subnetMask());
  // If either interface considers the other address on-link, routing can be ambiguous.
  return ((ea & em) == (wa & em)) || ((ea & wm) == (wa & wm));
}
