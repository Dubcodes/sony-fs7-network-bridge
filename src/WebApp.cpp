#include "WebApp.h"
#include "WebPages.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#ifndef FS7B_HTTP_PORT
#define FS7B_HTTP_PORT 80
#endif
#ifndef FS7B_BOARD_PROFILE
#define FS7B_BOARD_PROFILE "WT32-ETH01"
#endif

namespace {
const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK_WATCHDOG";
    case ESP_RST_WDT: return "OTHER_WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "POWER_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "OTHER";
  }
}
}

WebApp::WebApp(ConfigStore &store, BridgeNetworkManager &network, SonyRemote &sony, SonyProxy &proxy,
               CommandEngine &engine, CameraStateStore &cameraState,
               TelemetryManager &telemetry, SequenceEngine &sequences, VsmGeneric &vsm,
               RuntimeStatus &status, OperationArbiter &arbiter)
    : server_(FS7B_HTTP_PORT), store_(store), network_(network), sony_(sony), proxy_(proxy),
      engine_(engine), cameraState_(cameraState), telemetry_(telemetry), sequences_(sequences), vsm_(vsm),
      status_(status), arbiter_(arbiter) {}

void WebApp::begin() {
  server_.on("/", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", CONTROL_HTML); });
  server_.on("/operator", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", OPERATOR_HTML); });
  server_.on("/layout", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", LAYOUT_HTML); });
  server_.on("/network", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", NETWORK_HTML); });
  server_.on("/setup", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", SETUP_HTML); });
  server_.on("/discovery", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", DISCOVERY_HTML); });
  server_.on("/sony-native", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", SONY_NATIVE_HTML); });
  server_.on("/telemetry", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", TELEMETRY_HTML); });
  server_.on("/sequences", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", SEQUENCES_HTML); });
  server_.on("/vsm", HTTP_GET, [this]() { server_.send_P(200, "text/html; charset=utf-8", VSM_HTML); });
  server_.on("/app.css", HTTP_GET, [this]() { server_.send_P(200, "text/css; charset=utf-8", APP_CSS); });

  server_.on("/api/v1/status", HTTP_GET, [this]() { sendJson(200, statusJson()); });
  server_.on("/api/v1/system", HTTP_GET, [this]() { sendJson(200, systemJson()); });
  server_.on("/api/v1/catalog", HTTP_GET, [this]() { sendJson(200, catalogJson()); });
  server_.on("/api/v1/camera-state", HTTP_GET, [this]() {
    sendJson(200, cameraState_.json(store_.config().telemetryFreshMs));
  });

  server_.on("/api/v1/layout", HTTP_GET, [this]() { sendJson(200, store_.layoutJson()); });
  server_.on("/api/v1/layout", HTTP_POST, [this]() {
    if (!requireBodyUnder(8192)) return;
    String err;
    if (store_.saveLayoutJson(server_.arg("plain"), err)) sendMessage(200, "layout saved", true);
    else sendMessage(400, err);
  });

  server_.on("/api/v1/config", HTTP_GET, [this]() { sendJson(200, store_.configJson(true)); });
  server_.on("/api/v1/config", HTTP_POST, [this]() {
    if (!requireAutomationIdle("change configuration")) return;
    if (!requireBodyUnder(8192)) return;
    String err;
    if (store_.updateConfigJson(server_.arg("plain"), err)) sendMessage(200, "configuration saved", true);
    else sendMessage(400, err);
  });

  server_.on("/api/v1/network-config", HTTP_GET, [this]() {
    const BridgeConfig &cfg = store_.config();
    JsonDocument doc;
    doc["dhcp"] = cfg.ethernetDhcp;
    doc["ip"] = cfg.ethernetIp;
    doc["gateway"] = cfg.ethernetGateway;
    doc["subnet"] = cfg.ethernetSubnet;
    doc["dns"] = cfg.ethernetDns;
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
  });
  server_.on("/api/v1/network-config", HTTP_POST, [this]() {
    if (!requireAutomationIdle("change Ethernet configuration")) return;
    if (!requireBodyUnder(2048)) return;
    JsonDocument input;
    if (deserializeJson(input, server_.arg("plain")) || !input.is<JsonObject>()) {
      sendMessage(400, "Invalid network configuration JSON");
      return;
    }
    JsonDocument update;
    update["ethernet"].set(input.as<JsonObjectConst>());
    String body;
    serializeJson(update, body);
    String err;
    if (store_.updateConfigJson(body, err)) {
      sendMessage(200, "Ethernet settings saved; reboot required", true);
    } else {
      sendMessage(400, err);
    }
  });

  server_.on("/api/v1/command-map", HTTP_GET, [this]() { sendJson(200, store_.commandMapJson()); });
  server_.on("/api/v1/command-map", HTTP_POST, [this]() {
    if (!requireAutomationIdle("change command mappings")) return;
    if (!requireBodyUnder(65536)) return;
    String err;
    if (store_.saveCommandMapJson(server_.arg("plain"), err)) sendMessage(200, "command map saved", true);
    else sendMessage(400, err);
  });

  server_.on("/api/v1/telemetry-map", HTTP_GET, [this]() { sendJson(200, store_.telemetryMapJson()); });
  server_.on("/api/v1/telemetry-map", HTTP_POST, [this]() {
    if (!requireAutomationIdle("change telemetry mappings")) return;
    if (!requireBodyUnder(65536)) return;
    String err;
    if (!store_.saveTelemetryMapJson(server_.arg("plain"), err)) { sendMessage(400, err); return; }
    if (!telemetry_.reload(err)) { sendMessage(500, "saved, but telemetry reload failed: " + err); return; }
    sendMessage(200, "telemetry map saved and reloaded", true);
  });
  server_.on("/api/v1/telemetry-diag", HTTP_GET, [this]() { sendJson(200, telemetry_.diagnosticsJson()); });

  server_.on("/api/v1/sequences", HTTP_GET, [this]() { sendJson(200, store_.sequencesJson()); });
  server_.on("/api/v1/sequences", HTTP_POST, [this]() {
    if (!requireAutomationIdle("change sequences")) return;
    if (!requireBodyUnder(32768)) return;
    String err;
    if (!store_.saveSequencesJson(server_.arg("plain"), err)) { sendMessage(400, err); return; }
    if (!sequences_.reload(err)) { sendMessage(500, "saved, but sequence reload failed: " + err); return; }
    sendMessage(200, "sequences saved and reloaded", true);
  });
  server_.on("/api/v1/sequences/status", HTTP_GET, [this]() { sendJson(200, sequences_.statusJson()); });

  server_.on("/api/v1/vsm-map", HTTP_GET, [this]() { sendJson(200, store_.vsmGenericMapJson()); });
  server_.on("/api/v1/vsm-map", HTTP_POST, [this]() {
    if (!requireAutomationIdle("change VSM mappings")) return;
    if (!requireBodyUnder(32768)) return;
    String err;
    if (!store_.saveVsmGenericMapJson(server_.arg("plain"), err)) { sendMessage(400, err); return; }
    if (!vsm_.reload(err)) { sendMessage(500, "saved, but VSM slot reload failed: " + err); return; }
    sendMessage(200, "VSM generic slots saved", true);
  });
  server_.on("/api/v1/vsm/feedback", HTTP_GET, [this]() { sendJson(200, vsm_.feedbackJson()); });
  server_.on("/api/v1/vsm/slots", HTTP_GET, [this]() { sendJson(200, vsm_.slotsJson()); });

  server_.on("/api/v1/sony/test", HTTP_POST, [this]() {
    if (!arbiter_.acquire(OperationOwner::Manual)) {
      sendMessage(409, "Camera operation busy: " + String(OperationArbiter::name(arbiter_.owner())));
      return;
    }
    uint32_t start = millis();
    SonyResponse r = sony_.testConnection();
    arbiter_.release(OperationOwner::Manual);
    status_.lastSonyResponseMs = millis() - start;
    status_.lastSonyHttpStatus = r.httpStatus;
    status_.cameraReachable = r.transportOk;
    if (r.transportOk) status_.lastError = ""; else status_.lastError = r.error;
    JsonDocument doc;
    doc["ok"] = r.transportOk && r.httpStatus >= 200 && r.httpStatus < 400;
    doc["httpStatus"] = r.httpStatus; doc["error"] = r.error;
    String out; serializeJson(doc, out); sendJson(r.transportOk ? 200 : 503, out);
  });

  server_.on("/api/v1/sony/linear-test", HTTP_POST, [this]() {
    if (!arbiter_.acquire(OperationOwner::Manual)) {
      sendMessage(409, "Camera operation busy: " + String(OperationArbiter::name(arbiter_.owner())));
      return;
    }
    uint32_t start = millis();
    SonyResponse r = sony_.testLinear();
    arbiter_.release(OperationOwner::Manual);
    status_.lastSonyResponseMs = millis() - start;
    status_.lastSonyHttpStatus = r.httpStatus;
    status_.cameraReachable = r.transportOk;
    if (r.commandOk) status_.lastError = ""; else status_.lastError = r.error;
    JsonDocument doc;
    doc["ok"] = r.transportOk && r.commandOk;
    doc["handshakeStatus"] = r.httpStatus;
    doc["rpc"] = r.rpcMethod.length() ? r.rpcMethod : "Property.GetValue";
    doc["result"] = r.body;
    doc["error"] = r.error;
    String out; serializeJson(doc, out);
    sendJson(r.transportOk && r.commandOk ? 200 : 503, out);
  });

  server_.on("/api/v1/sony/capture", HTTP_GET, [this]() {
    sendJson(200, proxy_.captureJson());
  });
  server_.on("/api/v1/sony/capture/start", HTTP_POST, [this]() {
    proxy_.startCapture();
    sendMessage(200, "Sony native capture started and previous RAM capture cleared", true);
  });
  server_.on("/api/v1/sony/capture/stop", HTTP_POST, [this]() {
    proxy_.stopCapture();
    sendMessage(200, "Sony native capture stopped", true);
  });
  server_.on("/api/v1/sony/capture/clear", HTTP_POST, [this]() {
    proxy_.clearCapture();
    sendMessage(200, "Sony native capture cleared", true);
  });

  server_.on("/api/v1/sony/raw", HTTP_POST, [this]() {
    if (!requireBodyUnder(49152)) return;
    JsonDocument req;
    if (deserializeJson(req, server_.arg("plain"))) { sendMessage(400, "invalid JSON"); return; }
    String method = req["method"] | "GET";
    String path = req["path"] | "/rm.html";
    String body = req["body"] | "";
    String contentType = req["contentType"] | "text/plain";
    String extraHeaders = req["extraHeaders"] | "";
    if (!arbiter_.acquire(OperationOwner::Manual)) {
      sendMessage(409, "Camera operation busy: " + String(OperationArbiter::name(arbiter_.owner())));
      return;
    }
    uint32_t start = millis();
    SonyResponse r = sony_.rawRequest(method, path, body, contentType, extraHeaders, 32768);
    arbiter_.release(OperationOwner::Manual);
    status_.lastSonyResponseMs = millis() - start;
    status_.lastSonyHttpStatus = r.httpStatus;
    status_.cameraReachable = r.transportOk;
    JsonDocument doc;
    doc["ok"] = r.transportOk && r.httpStatus >= 200 && r.httpStatus < 400;
    doc["httpStatus"] = r.httpStatus; doc["headers"] = r.headers;
    doc["body"] = r.body; doc["error"] = r.error;
    String out; serializeJson(doc, out); sendJson(r.transportOk ? 200 : 503, out);
  });

  server_.on("/api/v1/wifi-scan", HTTP_GET, [this]() {
    if (!requireAutomationIdle("scan Wi-Fi")) return;
    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    int count = WiFi.scanNetworks(false, true);
    for (int i = 0; i < count; ++i) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i); o["rssi"] = WiFi.RSSI(i); o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    String out; serializeJson(doc, out); sendJson(200, out);
  });

  server_.on("/api/v1/reconnect-camera", HTTP_POST, [this]() {
    if (!requireAutomationIdle("reconnect camera Wi-Fi")) return;
    network_.reconnectCameraWifi(); sendMessage(202, "camera Wi-Fi reconnect started", true);
  });
  server_.on("/api/v1/export", HTTP_GET, [this]() {
    // Never expose stored Wi-Fi or camera credentials over the unauthenticated lab API.
    sendJson(200, store_.fullExportJson(true));
  });
  server_.on("/api/v1/reboot", HTTP_POST, [this]() {
    if (!requireAutomationIdle("reboot")) return;
    sendMessage(202, "rebooting", true);
    delay(150);
    ESP.restart();
  });

  // Application-only web OTA. The first UART flash must still install the
  // bootloader + partition table. Later updates upload PlatformIO firmware.bin.
  server_.on("/api/v1/update", HTTP_POST,
             [this]() { finishFirmwareUploadRequest(); },
             [this]() { handleFirmwareUpload(); });
  server_.on("/api/v1/stop-replay/abort", HTTP_POST, [this]() {
    CommandResult r = engine_.abortStopReplay();
    sendMessage(r.statusCode, r.message, r.ok);
  });

  server_.onNotFound([this]() { handleNotFound(); });
  server_.begin();
  Serial.printf("[web] HTTP server listening on port %d\n", FS7B_HTTP_PORT);
}

void WebApp::loop() {
  server_.handleClient();
  // Do not let a dropped browser connection leave the bridge permanently
  // locked in firmware-update ownership. Normal WebServer abort callbacks still
  // fail immediately; this is a conservative inactivity backstop.
  if (otaInProgress_ && otaLastActivityMs_ && millis() - otaLastActivityMs_ > 120000U) {
    failFirmwareUpload("Firmware upload timed out after 120 seconds without data");
  }
  if (otaRebootPending_ && static_cast<int32_t>(millis() - otaRebootAtMs_) >= 0) {
    Serial.println("[ota] rebooting into newly written application");
    delay(20);
    ESP.restart();
  }
}

void WebApp::sendJson(int code, const String &json) {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(code, "application/json; charset=utf-8", json);
}

void WebApp::sendMessage(int code, const String &message, bool ok) {
  JsonDocument doc; doc["ok"] = ok; doc["message"] = message;
  String out; serializeJson(doc, out); sendJson(code, out);
}

bool WebApp::requireBodyUnder(size_t maxBytes) {
  if (server_.arg("plain").length() <= maxBytes) return true;
  sendMessage(413, "request body is too large");
  return false;
}

bool WebApp::requireAutomationIdle(const char *action) {
  if (!arbiter_.busy()) return true;
  sendMessage(409, "Cannot " + String(action) + " while operation " +
                   String(OperationArbiter::name(arbiter_.owner())) + " is active");
  return false;
}

String WebApp::statusJson() {
  status_.ethernetStarted = network_.ethernetStarted();
  status_.ethernetConnected = network_.ethernetConnected();
  status_.ethernetIp = network_.ethernetIp();
  status_.wifiConnected = network_.wifiConnected();
  status_.wifiIp = network_.wifiIp();
  status_.wifiRssi = status_.wifiConnected ? WiFi.RSSI() : -127;
  status_.subnetOverlap = network_.subnetsOverlap();

  const CameraStateValue *rec = cameraState_.get("recording");
  const CameraStateValue *play = cameraState_.get("playback");
  const bool recordingFresh = cameraState_.isFresh("recording", store_.config().telemetryFreshMs);
  const bool playbackFresh = cameraState_.isFresh("playback", store_.config().telemetryFreshMs);

  JsonDocument doc;
  doc["deviceName"] = store_.config().deviceName;
  doc["version"] = FS7B_VERSION;
  doc["apiVersion"] = "v1";
  JsonObject eth = doc["ethernet"].to<JsonObject>();
  eth["started"] = status_.ethernetStarted;
  eth["linkUp"] = network_.ethernetLinkUp();
  eth["connected"] = status_.ethernetConnected;
  eth["ip"] = status_.ethernetIp;
  eth["mac"] = network_.ethernetMac();
  eth["speedMbps"] = network_.ethernetLinkSpeedMbps();
  eth["fullDuplex"] = network_.ethernetFullDuplex();
  eth["dhcp"] = network_.ethernetDhcpConfigured();
  eth["addressing"] = network_.ethernetDhcpConfigured() ? "DHCP" : "STATIC";
  eth["subnet"] = network_.ethernetSubnet();
  eth["gateway"] = network_.ethernetGateway();
  eth["dns"] = network_.ethernetDns();
  JsonObject wifi = doc["wifi"].to<JsonObject>(); wifi["connected"] = status_.wifiConnected; wifi["ip"] = status_.wifiIp; wifi["rssi"] = status_.wifiRssi;
  JsonObject cam = doc["camera"].to<JsonObject>(); cam["reachable"] = status_.cameraReachable && status_.wifiConnected;
  if (recordingFresh) cam["recording"] = cameraState_.boolValue("recording"); else cam["recording"] = nullptr;
  if (playbackFresh) cam["playback"] = cameraState_.boolValue("playback"); else cam["playback"] = nullptr;
  cam["optimisticRecording"] = status_.optimisticRecording;
  cam["optimisticPlayback"] = status_.optimisticPlayback;
  cam["macroBusy"] = status_.macroBusy;
  cam["macroPhase"] = status_.macroPhase; cam["macroElapsedMs"] = status_.macroBusy && status_.macroStartedMs ? millis() - status_.macroStartedMs : 0;
  cam["linearConnected"] = sony_.linearConnected();
  cam["backendLinearConnections"] = sony_.linearConnected() ? 1 : 0;
  cam["nativeProxyLinearConnections"] = proxy_.activeLinearSessions();
  cam["linearConnections"] = (sony_.linearConnected() ? 1 : 0) + proxy_.activeLinearSessions();
  cam["recordingFeedback"] = rec && rec->valid; cam["playbackFeedback"] = play && play->valid;
  cam["recordingFresh"] = recordingFresh; cam["playbackFresh"] = playbackFresh;
  JsonObject net = doc["network"].to<JsonObject>(); net["subnetOverlap"] = status_.subnetOverlap;
  doc["lastCommand"] = status_.lastCommand; doc["lastError"] = status_.lastError;
  doc["lastSonyHttpStatus"] = status_.lastSonyHttpStatus; doc["lastSonyResponseMs"] = status_.lastSonyResponseMs;
  doc["lastTelemetryMs"] = status_.lastTelemetryMs;
  doc["telemetryAgeMs"] = status_.lastTelemetryMs ? millis() - status_.lastTelemetryMs : 0;
  doc["telemetryFresh"] = status_.lastTelemetryMs && millis() - status_.lastTelemetryMs <= store_.config().telemetryFreshMs;
  doc["lastTelemetrySource"] = status_.lastTelemetrySource;
  JsonObject operation = doc["operation"].to<JsonObject>();
  operation["busy"] = arbiter_.busy();
  operation["owner"] = OperationArbiter::name(arbiter_.owner());
  operation["elapsedMs"] = arbiter_.elapsedMs();
  doc["uptimeSeconds"] = millis() / 1000;
  String out; serializeJson(doc, out); return out;
}


String WebApp::systemJson() {
  JsonDocument doc;
  doc["version"] = FS7B_VERSION;
  doc["boardProfile"] = FS7B_BOARD_PROFILE;
  doc["uptimeSeconds"] = millis() / 1000U;
  doc["resetReason"] = resetReasonName(esp_reset_reason());

  JsonObject chip = doc["chip"].to<JsonObject>();
  chip["model"] = ESP.getChipModel();
  chip["revision"] = ESP.getChipRevision();
  chip["cores"] = ESP.getChipCores();
  chip["cpuMHz"] = ESP.getCpuFreqMHz();
  chip["sdkVersion"] = ESP.getSdkVersion();
  chip["arduinoCoreVersion"] = ESP.getCoreVersion();

  JsonObject memory = doc["memory"].to<JsonObject>();
  memory["heapTotal"] = ESP.getHeapSize();
  memory["heapFree"] = ESP.getFreeHeap();
  memory["heapMinimumFree"] = ESP.getMinFreeHeap();
  memory["heapLargestBlock"] = ESP.getMaxAllocHeap();

  JsonObject flash = doc["flash"].to<JsonObject>();
  flash["chipBytes"] = ESP.getFlashChipSize();
  flash["applicationBytes"] = ESP.getSketchSize();
  flash["nextOtaCapacityBytes"] = ESP.getFreeSketchSpace();

  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  JsonObject partitions = doc["partitions"].to<JsonObject>();
  if (running) {
    JsonObject r = partitions["running"].to<JsonObject>();
    r["label"] = running->label;
    r["address"] = running->address;
    r["size"] = running->size;
    r["subtype"] = running->subtype;
  } else {
    partitions["running"] = nullptr;
  }
  if (next) {
    JsonObject n = partitions["nextOta"].to<JsonObject>();
    n["label"] = next->label;
    n["address"] = next->address;
    n["size"] = next->size;
    n["subtype"] = next->subtype;
  } else {
    partitions["nextOta"] = nullptr;
  }

  JsonObject fs = doc["filesystem"].to<JsonObject>();
  fs["mounted"] = store_.storageReady();
  if (store_.storageReady()) {
    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    fs["totalBytes"] = total;
    fs["usedBytes"] = used;
    fs["freeBytes"] = total >= used ? total - used : 0;
  } else {
    fs["totalBytes"] = 0;
    fs["usedBytes"] = 0;
    fs["freeBytes"] = 0;
  }

  JsonObject eth = doc["ethernet"].to<JsonObject>();
  eth["started"] = network_.ethernetStarted();
  eth["linkUp"] = network_.ethernetLinkUp();
  eth["hasIp"] = network_.ethernetConnected();
  eth["ip"] = network_.ethernetIp();
  eth["mac"] = network_.ethernetMac();
  eth["speedMbps"] = network_.ethernetLinkSpeedMbps();
  eth["fullDuplex"] = network_.ethernetFullDuplex();
  eth["dhcp"] = network_.ethernetDhcpConfigured();
  eth["addressing"] = network_.ethernetDhcpConfigured() ? "DHCP" : "STATIC";
  eth["subnet"] = network_.ethernetSubnet();
  eth["gateway"] = network_.ethernetGateway();
  eth["dns"] = network_.ethernetDns();
  JsonObject configured = eth["configuredStatic"].to<JsonObject>();
  configured["ip"] = store_.config().ethernetIp;
  configured["subnet"] = store_.config().ethernetSubnet;
  configured["gateway"] = store_.config().ethernetGateway;
  configured["dns"] = store_.config().ethernetDns;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = network_.wifiConnected();
  wifi["ip"] = network_.wifiIp();
  wifi["mac"] = WiFi.macAddress();
  wifi["rssi"] = network_.wifiConnected() ? WiFi.RSSI() : -127;

  JsonObject operation = doc["operation"].to<JsonObject>();
  operation["busy"] = arbiter_.busy();
  operation["owner"] = OperationArbiter::name(arbiter_.owner());
  operation["elapsedMs"] = arbiter_.elapsedMs();

  JsonObject ota = doc["ota"].to<JsonObject>();
  ota["inProgress"] = otaInProgress_;
  ota["success"] = otaSuccess_;
  ota["filename"] = otaFilename_;
  ota["bytesWritten"] = otaBytesWritten_;
  ota["elapsedMs"] = otaStartedMs_ ? millis() - otaStartedMs_ : 0;
  ota["error"] = otaError_;
  ota["rebootPending"] = otaRebootPending_;

  String out;
  serializeJson(doc, out);
  return out;
}

void WebApp::failFirmwareUpload(const String &message) {
  otaError_ = message;
  otaSuccess_ = false;
  otaInProgress_ = false;
  otaLastActivityMs_ = 0;
  otaRebootPending_ = false;
  if (Update.isRunning()) Update.abort();
  if (arbiter_.ownedBy(OperationOwner::FirmwareUpdate)) {
    arbiter_.release(OperationOwner::FirmwareUpdate);
  }
  Serial.printf("[ota] failed: %s\n", message.c_str());
}

void WebApp::handleFirmwareUpload() {
  HTTPUpload &upload = server_.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaError_ = "";
    otaFilename_ = upload.filename;
    otaBytesWritten_ = 0;
    otaStartedMs_ = millis();
    otaLastActivityMs_ = otaStartedMs_;
    otaSuccess_ = false;
    otaRejectedBusy_ = false;
    otaRebootPending_ = false;

    if (arbiter_.busy() || !arbiter_.acquire(OperationOwner::FirmwareUpdate)) {
      otaRejectedBusy_ = true;
      otaError_ = "System busy: " + String(OperationArbiter::name(arbiter_.owner()));
      Serial.printf("[ota] rejected: %s\n", otaError_.c_str());
      return;
    }

    if (!upload.filename.length() || !upload.filename.endsWith(".bin")) {
      failFirmwareUpload("Select a PlatformIO application firmware.bin file");
      return;
    }

    const uint32_t otaCapacity = ESP.getFreeSketchSpace();
    if (otaCapacity == 0) {
      failFirmwareUpload("No OTA application partition is available");
      return;
    }

    // U_FLASH selects the next OTA application partition only. It does not
    // overwrite the bootloader, partition table, NVS, or LittleFS.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      failFirmwareUpload("Update.begin failed: " + String(Update.errorString()));
      return;
    }

    otaInProgress_ = true;
    Serial.printf("[ota] receiving %s; OTA slot capacity=%lu bytes\n",
                  upload.filename.c_str(), static_cast<unsigned long>(otaCapacity));
    return;
  }

  if (otaRejectedBusy_ || otaError_.length()) return;

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaInProgress_) return;
    otaLastActivityMs_ = millis();
    const size_t written = Update.write(upload.buf, upload.currentSize);
    otaBytesWritten_ += written;
    if (written != upload.currentSize) {
      failFirmwareUpload("Flash write failed: " + String(Update.errorString()));
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    failFirmwareUpload("Firmware upload was aborted");
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (!otaInProgress_) return;
    otaLastActivityMs_ = millis();
    // UPDATE_SIZE_UNKNOWN requires end(true); the Update library still
    // validates the ESP application image/header before enabling the partition.
    if (!Update.end(true)) {
      failFirmwareUpload("Firmware validation/finalization failed: " + String(Update.errorString()));
      return;
    }
    otaInProgress_ = false;
    otaSuccess_ = true;
    otaBytesWritten_ = upload.totalSize;
    otaRebootPending_ = true;
    otaRebootAtMs_ = millis() + 1500U;
    Serial.printf("[ota] update accepted: %lu bytes; reboot scheduled\n",
                  static_cast<unsigned long>(otaBytesWritten_));
  }
}

void WebApp::finishFirmwareUploadRequest() {
  if (otaRejectedBusy_) {
    sendMessage(409, otaError_.length() ? otaError_ : "System is busy");
    otaRejectedBusy_ = false;
    otaError_ = "";
    return;
  }

  if (!otaSuccess_) {
    const String error = otaError_.length() ? otaError_ : "Firmware upload did not complete";
    sendMessage(400, error);
    if (arbiter_.ownedBy(OperationOwner::FirmwareUpdate)) {
      arbiter_.release(OperationOwner::FirmwareUpdate);
    }
    return;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = "Firmware accepted; rebooting";
  doc["bytesWritten"] = otaBytesWritten_;
  doc["versionBeforeReboot"] = FS7B_VERSION;
  doc["rebootInMs"] = 1500;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}


String WebApp::catalogJson() {
  JsonDocument doc;
  doc["apiVersion"] = "v1";
  doc["firmwareVersion"] = FS7B_VERSION;
  doc["deviceName"] = store_.config().deviceName;
  doc["sequenceSlots"] = SequenceEngine::SLOT_COUNT;
  JsonArray arr = doc["commands"].to<JsonArray>();
  for (const auto &c : COMMANDS) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = c.id; o["label"] = c.label; o["group"] = c.group; o["style"] = c.style; o["defaultVisible"] = c.defaultVisible;
    if (String(c.id) == "stop_replay") {
      bool stop = store_.commandMapping("record_stop").configured() || store_.commandMapping("record_toggle").configured();
      o["configured"] = stop && store_.commandMapping("rec_review").configured();
    } else if (String(c.id) == "record_start") {
      o["configured"] = store_.commandMapping("record_start").configured() || store_.commandMapping("record_toggle").configured();
    } else if (String(c.id) == "record_stop") {
      o["configured"] = store_.commandMapping("record_stop").configured() || store_.commandMapping("record_toggle").configured();
    } else o["configured"] = store_.commandMapping(c.id).configured();
  }
  JsonArray states = doc["states"].to<JsonArray>();
  for (const auto &s : CAMERA_STATES) {
    JsonObject o = states.add<JsonObject>();
    o["id"] = s.id; o["label"] = s.label; o["group"] = s.group; o["type"] = s.type; o["defaultVisible"] = s.defaultVisible;
  }
  String out; serializeJson(doc, out); return out;
}

void WebApp::handleNotFound() {
  String uri = server_.uri();
  if (server_.method() == HTTP_POST && uri.startsWith("/api/v1/sequences/") && uri.endsWith("/trigger")) {
    String mid = uri.substring(String("/api/v1/sequences/").length(), uri.length() - String("/trigger").length());
    int slot = mid.toInt();
    CommandResult r = sequences_.trigger(slot);
    sendMessage(r.statusCode, r.message, r.ok);
    return;
  }
  if (server_.method() == HTTP_POST && uri.startsWith("/api/v1/sequences/") && uri.endsWith("/abort")) {
    String mid = uri.substring(String("/api/v1/sequences/").length(), uri.length() - String("/abort").length());
    int slot = mid.toInt();
    CommandResult r = sequences_.abort(slot);
    sendMessage(r.statusCode, r.message, r.ok);
    return;
  }
  const String commandPrefix = "/api/v1/command/";
  const String triggerPrefix = "/api/v1/vsm/trigger/";
  if (server_.method() == HTTP_POST && uri.startsWith(commandPrefix)) {
    String id = uri.substring(commandPrefix.length());
    if (!store_.validCommandId(id)) { sendMessage(404, "unknown command"); return; }
    CommandResult r = engine_.execute(id);
    JsonDocument doc; doc["ok"] = r.ok; doc["message"] = r.message; doc["sonyHttpStatus"] = r.sonyHttpStatus;
    String out; serializeJson(doc, out); sendJson(r.statusCode, out); return;
  }
  if (server_.method() == HTTP_POST && uri.startsWith(triggerPrefix)) {
    int slot = uri.substring(triggerPrefix.length()).toInt();
    CommandResult r = vsm_.trigger(slot);
    JsonDocument doc; doc["ok"] = r.ok; doc["message"] = r.message; doc["sonyHttpStatus"] = r.sonyHttpStatus;
    String out; serializeJson(doc, out); sendJson(r.statusCode, out); return;
  }
  sendMessage(404, "not found");
}
