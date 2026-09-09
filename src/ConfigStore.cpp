#include "ConfigStore.h"
#include <algorithm>

namespace {
const char *CONFIG_PATH = "/bridge.json";
const char *LAYOUT_PATH = "/layout.json";
const char *COMMAND_PATH = "/commands.json";
const char *TELEMETRY_PATH = "/telemetry.json";
const char *VSM_GENERIC_PATH = "/vsm_generic.json";
const char *SEQUENCES_PATH = "/sequences.json";
constexpr uint32_t CONFIG_SCHEMA_VERSION = 4;
constexpr uint32_t COMMAND_SCHEMA_VERSION = 5;

String ipOrDefault(JsonVariantConst v, const char *fallback) {
  return v.is<const char *>() ? String(v.as<const char *>()) : String(fallback);
}

bool validIp(const String &text) {
  IPAddress ip;
  return ip.fromString(text);
}

uint32_t ipValue(const IPAddress &ip) {
  return (static_cast<uint32_t>(ip[0]) << 24) |
         (static_cast<uint32_t>(ip[1]) << 16) |
         (static_cast<uint32_t>(ip[2]) << 8) |
         static_cast<uint32_t>(ip[3]);
}

bool validStaticEthernet(const BridgeConfig &cfg) {
  IPAddress address;
  IPAddress subnet;
  if (!address.fromString(cfg.ethernetIp) || !subnet.fromString(cfg.ethernetSubnet)) return false;

  const uint32_t ip = ipValue(address);
  const uint32_t mask = ipValue(subnet);
  const uint32_t inverseMask = ~mask;
  // A usable static address needs a non-zero, unicast host address and a
  // contiguous, non-zero subnet mask. /32 is accepted for routed deployments.
  if (ip == 0 || address[0] == 127 || address[0] >= 224 || mask == 0) return false;
  if ((inverseMask & (inverseMask + 1U)) != 0) return false;
  if (inverseMask != 0) {
    const uint32_t host = ip & inverseMask;
    if (host == 0 || host == inverseMask) return false;
  }
  return true;
}


bool isLegacyDirectEthernetDefault(const BridgeConfig &cfg) {
  return !cfg.ethernetDhcp &&
         cfg.ethernetIp == "10.77.7.2" &&
         cfg.ethernetGateway == "0.0.0.0" &&
         cfg.ethernetSubnet == "255.255.255.0" &&
         cfg.ethernetDns == "0.0.0.0";
}

bool validHost(const String &text) {
  if (text.length() == 0 || text.length() > 253) return false;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
  }
  return true;
}

void canonicalizeVsmGroup(JsonVariantConst source, JsonArray output, int count,
                          const char *labelPrefix, const char *valueKey) {
  JsonArrayConst input = source.as<JsonArrayConst>();
  JsonObjectConst selected[16] = {};
  bool claimed[16] = {};

  // Explicit slots take priority. For duplicates, the first valid explicit
  // entry wins deterministically; later duplicates are ignored.
  for (JsonVariantConst value : input) {
    if (!value.is<JsonObjectConst>()) continue;
    JsonObjectConst entry = value.as<JsonObjectConst>();
    if (!entry["slot"].is<int>()) continue;
    const int slot = entry["slot"].as<int>();
    if (slot < 1 || slot > count || claimed[slot - 1]) continue;
    selected[slot - 1] = entry;
    claimed[slot - 1] = true;
  }

  // Legacy entries without a valid slot retain their array-position meaning,
  // but never displace an explicit assignment.
  for (int i = 0; i < count && i < static_cast<int>(input.size()); ++i) {
    JsonObjectConst entry = input[i].as<JsonObjectConst>();
    if (entry.isNull()) continue;
    const bool validSlot = entry["slot"].is<int>() &&
                           entry["slot"].as<int>() >= 1 &&
                           entry["slot"].as<int>() <= count;
    if (validSlot || claimed[i]) continue;
    selected[i] = entry;
    claimed[i] = true;
  }

  for (int i = 0; i < count; ++i) {
    JsonObjectConst existing = selected[i];
    JsonObject dst = output.add<JsonObject>();
    dst["slot"] = i + 1;
    dst["label"] = existing["label"].is<const char *>()
        ? existing["label"].as<String>() : (String(labelPrefix) + " " + String(i + 1));
    dst[valueKey] = existing[valueKey].is<const char *>()
        ? existing[valueKey].as<String>() : "";
  }
}

void canonicalizeVsmGenericMap(JsonObjectConst source, JsonDocument &clean) {
  JsonArray triggers = clean["triggers"].to<JsonArray>();
  canonicalizeVsmGroup(source["triggers"], triggers, 16, "Trigger", "command");
  const char *groups[] = {"bools", "ints", "floats", "texts"};
  for (const char *group : groups) {
    JsonArray slots = clean[group].to<JsonArray>();
    canonicalizeVsmGroup(source[group], slots, 8, group, "source");
  }
}

bool validHttpMethod(String method) {
  method.toUpperCase();
  return method == "GET" || method == "POST" || method == "PUT" ||
         method == "PATCH" || method == "DELETE" || method == "HEAD";
}

bool validRequestFields(const String &path, const String &contentType,
                        const String &body, const String &extraHeaders) {
  return path.length() <= 2048 && path.indexOf('\r') < 0 && path.indexOf('\n') < 0 &&
         path.indexOf(' ') < 0 && path.indexOf('\t') < 0 &&
         contentType.length() <= 128 && contentType.indexOf('\r') < 0 && contentType.indexOf('\n') < 0 &&
         body.length() <= 32768 && extraHeaders.length() <= 4096;
}


bool validLinearMethod(const String &method) {
  if (method.length() == 0 || method.length() > 128) return false;
  for (size_t i = 0; i < method.length(); ++i) {
    const char c = method[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_')) return false;
  }
  return true;
}

bool validLinearParams(const String &params) {
  if (params.length() == 0 || params.length() > 4096) return false;
  JsonDocument doc;
  if (deserializeJson(doc, params)) return false;
  JsonVariantConst root = doc.as<JsonVariantConst>();
  return root.is<JsonArrayConst>() || root.is<JsonObjectConst>();
}

bool validTransport(const String &transport) {
  return transport == "http" || transport == "linear";
}

void setLinearMapping(JsonObject commands, const char *id, const char *rpcMethod,
                      const char *rpcParams, const char *evidence, const char *note = "") {
  JsonObject m = commands[id].to<JsonObject>();
  m["transport"] = "linear";
  m["method"] = "POST";
  m["path"] = "";
  m["contentType"] = "application/x-www-form-urlencoded";
  m["body"] = "";
  m["extraHeaders"] = "";
  m["rpcMethod"] = rpcMethod;
  m["rpcParams"] = rpcParams;
  m["evidence"] = evidence;
  if (note && *note) m["note"] = note;
}

void applyPublicFs7LinearDefaults(JsonObject commands, bool onlyWhenBlank) {
  auto available = [commands, onlyWhenBlank](const char *id) mutable {
    if (!onlyWhenBlank) return true;
    JsonObject m = commands[id];
    String transport = m["transport"] | "http";
    String path = m["path"] | "";
    String rpcMethod = m["rpcMethod"] | "";
    return path.length() == 0 && rpcMethod.length() == 0 && transport != "linear";
  };

  // Exact PXW-FS7 native rmt.html evidence. record_connection.js invokes
  // client.clip.recorder.Start/Stop with params=[], and savona.min.js maps those
  // calls directly to Clip.Recorder.Start / Clip.Recorder.Stop with an empty RPC
  // parameter array. Prefer these deterministic commands over a REC toggle.
  if (available("record_start"))
    setLinearMapping(commands, "record_start", "Clip.Recorder.Start", "[]",
                     "fs7-native-rmt-exact");

  // Keep an explicit REC toggle available for generic/legacy panels, but the
  // production start/stop workflow uses the exact recorder methods above/below.
  if (available("record_toggle"))
    setLinearMapping(commands, "record_toggle", "Button.SendKeys", "[[\"Rec\"]]",
                     "fs7-native-live-exact");
  if (available("record_stop"))
    setLinearMapping(commands, "record_stop", "Clip.Recorder.Stop", "[]",
                     "fs7-native-rmt-exact");

  // SKAARHOJ's FS7 websocket integration identifies Play and Stop as FS7
  // one-shot keys. Public Sony-derived code identifies Button.SendKeys as the
  // key transport, and an independent public example names Button.SendKeys Play.
  if (available("play"))
    setLinearMapping(commands, "play", "Button.SendKeys", "[[\"Play\"]]",
                     "fs7-native-live-exact");
  if (available("pause"))
    setLinearMapping(commands, "pause", "Button.SendKeys", "[[\"Pause\"]]",
                     "fs7-native-live-exact", "Play and Pause are distinct FS7 keys");
  if (available("stop_playback"))
    setLinearMapping(commands, "stop_playback", "Button.SendKeys", "[[\"Stop\"]]",
                     "fs7-native-live-exact");

  // A public capture proves the Assignable.2 key form, while SKAARHOJ documents
  // six assign triggers for FS7. Populate only the FS7's six assign buttons.
  for (int i = 1; i <= 6; ++i) {
    String id = "assign_" + String(i);
    if (!available(id.c_str())) continue;
    String params = "[[\"Assignable." + String(i) + "\"]]";
    setLinearMapping(commands, id.c_str(), "Button.SendKeys", params.c_str(),
                     "fs7-native-live-exact");
  }

  // Exact FS7 native Cursor-page behaviour: Thumbnail opens the clip browser
  // and Set selects/plays the current (latest) clip. The command engine follows
  // this mapped Thumbnail key with acknowledgement-driven bounded Set attempts
  // so Rec Review no longer depends on an Assignable Button configuration.
  if (available("rec_review"))
    setLinearMapping(commands, "rec_review", "Button.SendKeys", "[[\"Thumbnail\"]]",
                     "fs7-native-live-exact",
                     "Bridge retries Set until acknowledged, then plays and verifies status");

  // Exact FS7 native Auto White action. rmt.html's CBAWBDialog calls
  // Process.Execute.AutomaticAdjustment with params=["Camera.WhiteBalance"].
  // Savona wraps params for this method, producing the wire array below.
  if (available("awb"))
    setLinearMapping(commands, "awb", "Process.Execute.AutomaticAdjustment",
                     "[[\"Camera.WhiteBalance\"]]", "fs7-native-live-exact");

  if (available("abb"))
    setLinearMapping(commands, "abb", "Process.Execute.AutomaticAdjustment",
                     "[[\"Camera.BlackBalance\"]]", "fs7-native-rmt-exact");

  const struct { const char *id; const char *key; } keys[] = {
      {"previous_clip", "Prev"}, {"rewind", "Rewind"},
      {"fast_forward", "Forward"}, {"next_clip", "Next"},
      {"menu", "Menu"}, {"user_menu", "UserMenu"}, {"status", "Status"},
      {"cursor_up", "UpArrow"}, {"cursor_down", "DownArrow"},
      {"cursor_left", "LeftArrow"}, {"cursor_right", "RightArrow"},
      {"cursor_set", "Set"}, {"cancel", "Cancel"}, {"thumbnail", "Thumbnail"},
  };
  for (const auto &key : keys) {
    if (!available(key.id)) continue;
    String params = "[[\"" + String(key.key) + "\"]]";
    setLinearMapping(commands, key.id, "Button.SendKeys", params.c_str(),
                     "fs7-native-live-exact");
  }

  if (available("focus_near"))
    setLinearMapping(commands, "focus_near", "Property.SetValue",
                     "[{\"Camera.Focus.Velocity\":-4}]", "fs7-native-live-exact");
  if (available("focus_far"))
    setLinearMapping(commands, "focus_far", "Property.SetValue",
                     "[{\"Camera.Focus.Velocity\":4}]", "fs7-native-live-exact");
  if (available("focus_stop"))
    setLinearMapping(commands, "focus_stop", "Property.SetValue",
                     "[{\"Camera.Focus.Velocity\":0}]", "fs7-native-live-exact");
  if (available("zoom_in"))
    setLinearMapping(commands, "zoom_in", "Property.SetValue",
                     "[{\"Camera.Zoom.Velocity\":4}]", "fs7-native-live-exact");
  if (available("zoom_out"))
    setLinearMapping(commands, "zoom_out", "Property.SetValue",
                     "[{\"Camera.Zoom.Velocity\":-4}]", "fs7-native-live-exact");
  if (available("zoom_stop"))
    setLinearMapping(commands, "zoom_stop", "Property.SetValue",
                     "[{\"Camera.Zoom.Velocity\":0}]", "fs7-native-live-exact");

  const struct { const char *id; const char *params; } modes[] = {
      {"auto_shutter_on", "[{\"Camera.Shutter.SettingMethod\":\"Automatic\"}]"},
      {"auto_shutter_off", "[{\"Camera.Shutter.SettingMethod\":\"Manual\"}]"},
      {"agc_on", "[{\"Camera.Gain.SettingMethod\":\"Automatic\"}]"},
      {"agc_off", "[{\"Camera.Gain.SettingMethod\":\"Manual\"}]"},
      {"atw_on", "[{\"Camera.WhiteBalance.SettingMethod\":\"Automatic\"}]"},
      {"atw_off", "[{\"Camera.WhiteBalance.SettingMethod\":\"Manual\"}]"},
      {"sq_150_on", "[{\"Camera.SlowAndQuickMotion.Enabled\":true,\"Camera.SlowAndQuickMotion.HighFrameRate.Enabled\":false,\"Camera.SlowAndQuickMotion.FrameRate\":150}]"},
      {"sq_off", "[{\"Camera.SlowAndQuickMotion.Enabled\":false,\"Camera.SlowAndQuickMotion.HighFrameRate.Enabled\":false,\"Camera.SlowAndQuickMotion.FrameRate\":150}]"},
  };
  for (const auto &mode : modes) {
    if (available(mode.id))
      setLinearMapping(commands, mode.id, "Property.SetValue", mode.params,
                       "fs7-native-live-exact");
  }

  // Other parameter-changing controls stay blank until their exact RPC
  // invocation is evidenced by the FS7 native page/capture.
}

void migrateFs7NativeRecordMappingsV3(JsonObject commands) {
  // Only replace the exact v2 defaults. Any operator-edited mapping is preserved.
  JsonObject start = commands["record_start"];
  String startTransport = start["transport"] | "";
  String startMethod = start["rpcMethod"] | "";
  String startParams = start["rpcParams"] | "";
  if (startTransport == "linear" && startMethod == "Clip.Recorder.Start" &&
      startParams == "[\"main\"]") {
    setLinearMapping(commands, "record_start", "Clip.Recorder.Start", "[]",
                     "fs7-native-rmt-exact");
  }

  JsonObject stop = commands["record_stop"];
  String stopTransport = stop["transport"] | "";
  String stopMethod = stop["rpcMethod"] | "";
  String stopParams = stop["rpcParams"] | "";
  if (stopTransport == "linear" && stopMethod == "Button.SendKeys" &&
      stopParams == "[[\"Rec\"]]") {
    setLinearMapping(commands, "record_stop", "Clip.Recorder.Stop", "[]",
                     "fs7-native-rmt-exact");
  }
}

void migrateFs7NativeControlMappingsV4(JsonObject commands) {
  // v0.2.9 used Assignable.6 for Rec Review. That is not deterministic because
  // operators can map Assign 6 to anything (including SHUTTER). Replace only
  // that exact untouched default with the FS7-native Thumbnail -> Set workflow.
  JsonObject review = commands["rec_review"];
  String reviewTransport = review["transport"] | "";
  String reviewMethod = review["rpcMethod"] | "";
  String reviewParams = review["rpcParams"] | "";
  if (reviewTransport == "linear" && reviewMethod == "Button.SendKeys" &&
      reviewParams == "[[\"Assignable.6\"]]") {
    setLinearMapping(commands, "rec_review", "Button.SendKeys", "[[\"Thumbnail\"]]",
                     "fs7-native-rmt-exact",
                     "Bridge follows Thumbnail with Set to play the latest clip");
  }
}

void migrateFs7NativeWireMappingsV5(JsonObject commands) {
  // Replace only known untouched v4 defaults. Savona wraps SendKeys' argument,
  // so the exact FS7 wire format is a nested params array. Play and Pause are
  // distinct key names.
  for (const auto &cmd : COMMANDS) {
    JsonObject m = commands[cmd.id];
    if (String(m["transport"] | "") != "linear" ||
        String(m["rpcMethod"] | "") != "Button.SendKeys") continue;
    String params = m["rpcParams"] | "";
    if (params.startsWith("[\"") && params.endsWith("\"]")) {
      m["rpcParams"] = "[" + params + "]";
      m["evidence"] = "fs7-native-live-exact";
    }
    if (String(cmd.id) == "pause" && String(m["rpcParams"] | "") == "[[\"Play\"]]") {
      m["rpcParams"] = "[[\"Pause\"]]";
      m["note"] = "Play and Pause are distinct FS7 keys";
    }
  }
}

bool configSane(const BridgeConfig &cfg, String &error) {
  if (cfg.deviceName.length() == 0 || cfg.deviceName.length() > 48) { error = "Device name must be 1-48 characters"; return false; }
  if (!validIp(cfg.ethernetIp) || !validIp(cfg.ethernetGateway) ||
      !validIp(cfg.ethernetSubnet) || !validIp(cfg.ethernetDns)) {
    error = "Ethernet addresses must be valid IPv4 addresses"; return false;
  }
  if (!cfg.ethernetDhcp && !validStaticEthernet(cfg)) {
    error = "Static Ethernet requires a usable unicast IP address and contiguous subnet mask";
    return false;
  }
  if (!validHost(cfg.cameraHost)) { error = "Camera host must be an IPv4 address or DNS hostname"; return false; }
  if (cfg.cameraSsid.length() > 32 || cfg.cameraWifiPassword.length() > 128 ||
      cfg.cameraUsername.length() > 64 || cfg.cameraPassword.length() > 128) {
    error = "Camera network or authentication field is too long"; return false;
  }
  if (cfg.recReviewSetDelayMs < 250 || cfg.recReviewSetDelayMs > 5000) {
    error = "Thumbnail to first Set attempt must be 250-5000 ms"; return false;
  }
  if (cfg.recReviewPlayDelayMs < 250 || cfg.recReviewPlayDelayMs > 5000) {
    error = "Set to Play delay must be 250-5000 ms"; return false;
  }
  return true;
}
}

bool ConfigStore::begin() {
  mounted_ = LittleFS.begin(true);
  if (!mounted_) {
    Serial.println("[storage] LittleFS mount failed");
    return false;
  }
  if (!loadConfig()) saveConfig();
  if (!loadLayout()) { setDefaultLayout(); writeFileAtomic(LAYOUT_PATH, layout_); }
  if (!loadCommandMap()) { setDefaultCommandMap(); writeFileAtomic(COMMAND_PATH, commandMap_); }
  if (!loadTelemetryMap()) { setDefaultTelemetryMap(); writeFileAtomic(TELEMETRY_PATH, telemetryMap_); }
  if (!loadVsmGenericMap()) { setDefaultVsmGenericMap(); writeFileAtomic(VSM_GENERIC_PATH, vsmGenericMap_); }
  if (!loadSequences()) { setDefaultSequences(); writeFileAtomic(SEQUENCES_PATH, sequences_); }
  return true;
}

bool ConfigStore::readFile(const char *path, String &out) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    // Recover the last complete file if power was lost during replacement.
    String backup = String(path) + ".bak";
    f = LittleFS.open(backup, "r");
    if (!f) return false;
    Serial.printf("[storage] recovering %s from backup\n", path);
  }
  out = f.readString();
  f.close();
  return out.length() > 0;
}

bool ConfigStore::writeFileAtomic(const char *path, const String &data) {
  String tmp = String(path) + ".tmp";
  String backup = String(path) + ".bak";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t written = f.print(data);
  f.flush();
  f.close();
  if (written != data.length()) { LittleFS.remove(tmp); return false; }
  File verify = LittleFS.open(tmp, "r");
  if (!verify || verify.size() != data.length()) {
    if (verify) verify.close();
    LittleFS.remove(tmp);
    return false;
  }
  verify.close();
  if (LittleFS.exists(path)) {
    LittleFS.remove(backup);
    if (!LittleFS.rename(path, backup)) {
      LittleFS.remove(tmp);
      return false;
    }
  }
  if (!LittleFS.rename(tmp, path)) {
    if (LittleFS.exists(backup)) LittleFS.rename(backup, path);
    LittleFS.remove(tmp);
    return false;
  }
  // Retain the previous complete generation for recovery after an interrupted
  // future replacement.
  return true;
}

bool ConfigStore::loadConfig() {
  String raw;
  if (!readFile(CONFIG_PATH, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;

  BridgeConfig loaded;
  const uint32_t schemaVersion = doc["configSchema"] | 1U;
  // Preserve an existing configured name; only missing values receive the
  // fresh-install hostname used by Ethernet/DHCP.
  loaded.deviceName = doc["deviceName"] | "FS7-WiFi-Bridge";
  JsonObject eth = doc["ethernet"];
  loaded.ethernetDhcp = eth["dhcp"] | true;
  loaded.ethernetIp = ipOrDefault(eth["ip"], "10.77.7.2");
  loaded.ethernetGateway = ipOrDefault(eth["gateway"], "0.0.0.0");
  loaded.ethernetSubnet = ipOrDefault(eth["subnet"], "255.255.255.0");
  loaded.ethernetDns = ipOrDefault(eth["dns"], "0.0.0.0");

  JsonObject camera = doc["camera"];
  loaded.cameraSsid = camera["ssid"] | "";
  loaded.cameraWifiPassword = camera["wifiPassword"] | "";
  loaded.cameraHost = camera["host"] | "192.168.1.1";
  loaded.cameraUsername = camera["username"] | "admin";
  loaded.cameraPassword = camera["password"] | "pxw-fs7";
  loaded.cameraTimeoutMs = constrain(camera["timeoutMs"] | 1800U, 250U, 15000U);
  loaded.replayDelayMs = std::min<uint32_t>(doc["replayDelayMs"] | 3000U, 10000U);
  loaded.recReviewSetDelayMs = constrain(doc["recReviewSetDelayMs"] | 4500U, 250U, 5000U);
  loaded.recReviewPlayDelayMs = constrain(doc["recReviewPlayDelayMs"] | 2500U, 250U, 5000U);
  if (schemaVersion < 4) {
    if (loaded.replayDelayMs == 900U || loaded.replayDelayMs == 1500U) loaded.replayDelayMs = 3000U;
    if (loaded.recReviewSetDelayMs == 1200U) loaded.recReviewSetDelayMs = 4500U;
  }
  loaded.replayTimeoutMs = constrain(doc["replayTimeoutMs"] | 25000U, 12000U, 60000U);
  if (schemaVersion < 4 && loaded.replayTimeoutMs == 15000U) loaded.replayTimeoutMs = 25000U;
  loaded.telemetryFreshMs = constrain(doc["telemetryFreshMs"] | 2500U, 250U, 10000U);
  loaded.dryRun = doc["dryRun"] | false;

  // v0.2.3 shipped a fixed 10.77.7.2/24 direct-test profile as its factory
  // default. Migrate only that exact legacy profile to DHCP. Any other static
  // address is treated as an intentional operator configuration and preserved.
  const bool migrateLegacyEthernet =
      schemaVersion < CONFIG_SCHEMA_VERSION && isLegacyDirectEthernetDefault(loaded);
  if (migrateLegacyEthernet) loaded.ethernetDhcp = true;

  String error;
  if (!configSane(loaded, error)) return false;
  cfg_ = loaded;
  const bool migrateConfigSchema = schemaVersion < CONFIG_SCHEMA_VERSION;
  if (migrateConfigSchema) {
    if (saveConfigValue(cfg_)) {
      if (migrateLegacyEthernet) {
        Serial.println("[storage] migrated legacy 10.77.7.2 Ethernet default to DHCP");
      }
      Serial.printf("[storage] migrated bridge config schema to %lu\n",
                    static_cast<unsigned long>(CONFIG_SCHEMA_VERSION));
    } else {
      Serial.println("[storage] warning: config migration active in RAM but could not be persisted");
    }
  }
  return true;
}

bool ConfigStore::saveConfig() {
  return saveConfigValue(cfg_);
}

bool ConfigStore::saveConfigValue(const BridgeConfig &value) {
  JsonDocument doc;
  doc["configSchema"] = CONFIG_SCHEMA_VERSION;
  doc["deviceName"] = value.deviceName;
  JsonObject eth = doc["ethernet"].to<JsonObject>();
  eth["dhcp"] = value.ethernetDhcp;
  eth["ip"] = value.ethernetIp;
  eth["gateway"] = value.ethernetGateway;
  eth["subnet"] = value.ethernetSubnet;
  eth["dns"] = value.ethernetDns;
  JsonObject camera = doc["camera"].to<JsonObject>();
  camera["ssid"] = value.cameraSsid;
  camera["wifiPassword"] = value.cameraWifiPassword;
  camera["host"] = value.cameraHost;
  camera["username"] = value.cameraUsername;
  camera["password"] = value.cameraPassword;
  camera["timeoutMs"] = value.cameraTimeoutMs;
  doc["replayDelayMs"] = value.replayDelayMs;
  doc["recReviewSetDelayMs"] = value.recReviewSetDelayMs;
  doc["recReviewPlayDelayMs"] = value.recReviewPlayDelayMs;
  doc["replayTimeoutMs"] = value.replayTimeoutMs;
  doc["telemetryFreshMs"] = value.telemetryFreshMs;
  doc["dryRun"] = value.dryRun;
  String out; serializeJsonPretty(doc, out);
  return writeFileAtomic(CONFIG_PATH, out);
}

bool ConfigStore::updateConfigJson(const String &json, String &error) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) { error = e.c_str(); return false; }

  BridgeConfig next = cfg_;
  if (doc["deviceName"].is<const char *>()) next.deviceName = doc["deviceName"].as<String>();
  if (doc["ethernet"].is<JsonObject>()) {
    JsonObject eth = doc["ethernet"];
    if (!eth["dhcp"].isNull()) next.ethernetDhcp = eth["dhcp"].as<bool>();
    if (eth["ip"].is<const char *>()) next.ethernetIp = eth["ip"].as<String>();
    if (eth["gateway"].is<const char *>()) next.ethernetGateway = eth["gateway"].as<String>();
    if (eth["subnet"].is<const char *>()) next.ethernetSubnet = eth["subnet"].as<String>();
    if (eth["dns"].is<const char *>()) next.ethernetDns = eth["dns"].as<String>();
  }
  if (doc["camera"].is<JsonObject>()) {
    JsonObject camera = doc["camera"];
    if (camera["ssid"].is<const char *>()) next.cameraSsid = camera["ssid"].as<String>();
    if (camera["wifiPassword"].is<const char *>()) { String v = camera["wifiPassword"].as<String>(); if (v != "********") next.cameraWifiPassword = v; }
    if (camera["host"].is<const char *>()) next.cameraHost = camera["host"].as<String>();
    if (camera["username"].is<const char *>()) next.cameraUsername = camera["username"].as<String>();
    if (camera["password"].is<const char *>()) { String v = camera["password"].as<String>(); if (v != "********") next.cameraPassword = v; }
    if (!camera["timeoutMs"].isNull()) next.cameraTimeoutMs = constrain(camera["timeoutMs"].as<uint32_t>(), 250U, 15000U);
  }
  if (!doc["replayDelayMs"].isNull()) next.replayDelayMs = std::min<uint32_t>(doc["replayDelayMs"].as<uint32_t>(), 10000U);
  if (!doc["recReviewSetDelayMs"].isNull()) next.recReviewSetDelayMs = doc["recReviewSetDelayMs"].as<uint32_t>();
  if (!doc["recReviewPlayDelayMs"].isNull()) next.recReviewPlayDelayMs = doc["recReviewPlayDelayMs"].as<uint32_t>();
  if (!doc["replayTimeoutMs"].isNull()) next.replayTimeoutMs = constrain(doc["replayTimeoutMs"].as<uint32_t>(), 12000U, 60000U);
  if (!doc["telemetryFreshMs"].isNull()) next.telemetryFreshMs = constrain(doc["telemetryFreshMs"].as<uint32_t>(), 250U, 10000U);
  if (!doc["dryRun"].isNull()) next.dryRun = doc["dryRun"].as<bool>();
  if (!configSane(next, error)) return false;
  if (!saveConfigValue(next)) { error = "Could not write configuration to flash"; return false; }
  cfg_ = next;
  return true;
}

String ConfigStore::configJson(bool redactSecrets) const {
  JsonDocument doc;
  doc["configSchema"] = CONFIG_SCHEMA_VERSION;
  doc["deviceName"] = cfg_.deviceName;
  doc["version"] = FS7B_VERSION;
  JsonObject eth = doc["ethernet"].to<JsonObject>();
  eth["dhcp"] = cfg_.ethernetDhcp;
  eth["ip"] = cfg_.ethernetIp;
  eth["gateway"] = cfg_.ethernetGateway;
  eth["subnet"] = cfg_.ethernetSubnet;
  eth["dns"] = cfg_.ethernetDns;
  JsonObject camera = doc["camera"].to<JsonObject>();
  camera["ssid"] = cfg_.cameraSsid;
  camera["wifiPassword"] = redactSecrets && cfg_.cameraWifiPassword.length() ? "********" : cfg_.cameraWifiPassword;
  camera["host"] = cfg_.cameraHost;
  camera["username"] = cfg_.cameraUsername;
  camera["password"] = redactSecrets && cfg_.cameraPassword.length() ? "********" : cfg_.cameraPassword;
  // The factory Basic Auth password is public Sony documentation, so exposing
  // whether that known default is still in use does not reveal a custom secret.
  // This lets the Setup UI prefill pxw-fs7 only when it is actually correct.
  camera["passwordIsDefault"] = cfg_.cameraPassword == "pxw-fs7";
  camera["timeoutMs"] = cfg_.cameraTimeoutMs;
  doc["replayDelayMs"] = cfg_.replayDelayMs;
  doc["recReviewSetDelayMs"] = cfg_.recReviewSetDelayMs;
  doc["recReviewPlayDelayMs"] = cfg_.recReviewPlayDelayMs;
  doc["replayTimeoutMs"] = cfg_.replayTimeoutMs;
  doc["telemetryFreshMs"] = cfg_.telemetryFreshMs;
  doc["dryRun"] = cfg_.dryRun;
  String out; serializeJson(doc, out); return out;
}

void ConfigStore::setDefaultLayout() {
  JsonDocument doc;
  JsonArray items = doc["items"].to<JsonArray>();
  for (const auto &cmd : COMMANDS) if (cmd.defaultVisible) items.add(cmd.id);
  JsonArray states = doc["stateItems"].to<JsonArray>();
  for (const auto &field : CAMERA_STATES) if (field.defaultVisible) states.add(field.id);
  serializeJsonPretty(doc, layout_);
}

bool ConfigStore::loadLayout() {
  String raw;
  if (!readFile(LAYOUT_PATH, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  if (!doc["items"].is<JsonArray>()) return false;
  // Upgrade older v0.1.0 layouts by adding default camera-data fields.
  if (!doc["stateItems"].is<JsonArray>()) {
    JsonArray states = doc["stateItems"].to<JsonArray>();
    for (const auto &field : CAMERA_STATES) if (field.defaultVisible) states.add(field.id);
    serializeJsonPretty(doc, raw);
    writeFileAtomic(LAYOUT_PATH, raw);
  }
  layout_ = raw;
  return true;
}

String ConfigStore::layoutJson() const { return layout_; }

bool ConfigStore::saveLayoutJson(const String &json, String &error) {
  JsonDocument in;
  DeserializationError e = deserializeJson(in, json);
  if (e || !in["items"].is<JsonArray>()) { error = "Expected JSON object with an items array"; return false; }

  JsonDocument clean;
  JsonArray outItems = clean["items"].to<JsonArray>();
  for (JsonVariant v : in["items"].as<JsonArray>()) {
    if (!v.is<const char *>()) continue;
    String id = v.as<String>();
    if (!validCommandId(id) || isHoldOnlyCommand(id)) continue;
    bool duplicate = false;
    for (JsonVariant existing : outItems) if (id == existing.as<String>()) { duplicate = true; break; }
    if (!duplicate) outItems.add(id);
  }
  if (outItems.size() == 0) { error = "Layout must contain at least one known command"; return false; }

  JsonArray outStates = clean["stateItems"].to<JsonArray>();
  if (in["stateItems"].is<JsonArray>()) {
    for (JsonVariant v : in["stateItems"].as<JsonArray>()) {
      if (!v.is<const char *>()) continue;
      String id = v.as<String>();
      if (!validStateId(id)) continue;
      bool duplicate = false;
      for (JsonVariant existing : outStates) if (id == existing.as<String>()) { duplicate = true; break; }
      if (!duplicate) outStates.add(id);
    }
  }

  String cleanText; serializeJsonPretty(clean, cleanText);
  if (!writeFileAtomic(LAYOUT_PATH, cleanText)) { error = "Could not write layout"; return false; }
  layout_ = cleanText;
  return true;
}

void ConfigStore::setDefaultCommandMap() {
  JsonDocument doc;
  doc["commandSchema"] = COMMAND_SCHEMA_VERSION;
  JsonObject commands = doc["commands"].to<JsonObject>();
  for (const auto &cmd : COMMANDS) {
    JsonObject m = commands[cmd.id].to<JsonObject>();
    m["transport"] = "http";
    m["method"] = "POST"; m["path"] = "";
    m["contentType"] = "application/x-www-form-urlencoded";
    m["body"] = ""; m["extraHeaders"] = "";
    m["rpcMethod"] = ""; m["rpcParams"] = "[]";
  }
  applyPublicFs7LinearDefaults(commands, false);
  serializeJsonPretty(doc, commandMap_);
}

bool ConfigStore::loadCommandMap() {
  String raw;
  if (!readFile(COMMAND_PATH, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) || !doc["commands"].is<JsonObject>()) return false;
  JsonObject commands = doc["commands"];
  const uint32_t schema = doc["commandSchema"] | 1U;
  bool migrated = schema < COMMAND_SCHEMA_VERSION;

  // Ensure every catalog command has a structurally complete entry before
  // applying evidence-backed mappings to previously blank entries.
  for (const auto &cmd : COMMANDS) {
    JsonObject m = commands[cmd.id].is<JsonObject>()
        ? commands[cmd.id].as<JsonObject>()
        : commands[cmd.id].to<JsonObject>();
    if (!m["transport"].is<const char *>()) m["transport"] = "http";
    if (!m["method"].is<const char *>()) m["method"] = "POST";
    if (!m["path"].is<const char *>()) m["path"] = "";
    if (!m["contentType"].is<const char *>()) m["contentType"] = "application/x-www-form-urlencoded";
    if (!m["body"].is<const char *>()) m["body"] = "";
    if (!m["extraHeaders"].is<const char *>()) m["extraHeaders"] = "";
    if (!m["rpcMethod"].is<const char *>()) m["rpcMethod"] = "";
    if (!m["rpcParams"].is<const char *>()) m["rpcParams"] = "[]";
  }
  if (migrated) {
    // v3 corrects the two record mappings using the actual FS7 rmt.html source.
    // First repair known untouched v2 defaults, then fill any remaining blanks.
    if (schema < 3) migrateFs7NativeRecordMappingsV3(commands);
    if (schema < 4) migrateFs7NativeControlMappingsV4(commands);
    if (schema < 5) migrateFs7NativeWireMappingsV5(commands);
    applyPublicFs7LinearDefaults(commands, true);
    doc["commandSchema"] = COMMAND_SCHEMA_VERSION;
  }

  String clean;
  serializeJsonPretty(doc, clean);
  commandMap_ = clean;
  if (migrated) {
    if (writeFileAtomic(COMMAND_PATH, commandMap_))
      Serial.println("[storage] migrated evidence-backed Sony /linear command mappings");
    else
      Serial.println("[storage] warning: Sony /linear command-map migration active in RAM but could not be persisted");
  }
  return true;
}

String ConfigStore::commandMapJson() const { return commandMap_; }

bool ConfigStore::saveCommandMapJson(const String &json, String &error) {
  JsonDocument in;
  DeserializationError e = deserializeJson(in, json);
  if (e || !in["commands"].is<JsonObject>()) { error = "Expected JSON object with a commands object"; return false; }
  JsonDocument clean;
  clean["commandSchema"] = COMMAND_SCHEMA_VERSION;
  JsonObject cleanCommands = clean["commands"].to<JsonObject>();
  for (const auto &cmd : COMMANDS) {
    JsonObject src = in["commands"][cmd.id];
    JsonObject dst = cleanCommands[cmd.id].to<JsonObject>();
    String transport = src["transport"] | "http"; transport.toLowerCase();
    String method = src["method"] | "POST"; method.toUpperCase();
    String path = src["path"] | "";
    String contentType = src["contentType"] | "application/x-www-form-urlencoded";
    String body = src["body"] | "";
    String headers = src["extraHeaders"] | "";
    String rpcMethod = src["rpcMethod"] | "";
    String rpcParams = src["rpcParams"] | "[]";
    if (!validTransport(transport)) { error = "Invalid transport for '" + String(cmd.id) + "'"; return false; }
    if (transport == "http") {
      if (!validHttpMethod(method) || !validRequestFields(path, contentType, body, headers)) {
        error = "Invalid or oversized HTTP mapping for '" + String(cmd.id) + "'"; return false;
      }
    } else {
      if (!validLinearMethod(rpcMethod) || !validLinearParams(rpcParams)) {
        error = "Invalid Sony /linear RPC mapping for '" + String(cmd.id) + "'"; return false;
      }
    }
    dst["transport"] = transport;
    dst["method"] = method; dst["path"] = path; dst["contentType"] = contentType;
    dst["body"] = body; dst["extraHeaders"] = headers;
    dst["rpcMethod"] = rpcMethod; dst["rpcParams"] = rpcParams;
    if (src["evidence"].is<const char *>()) dst["evidence"] = src["evidence"].as<String>();
    if (src["note"].is<const char *>()) dst["note"] = src["note"].as<String>();
  }
  String text; serializeJsonPretty(clean, text);
  if (!writeFileAtomic(COMMAND_PATH, text)) { error = "Could not write command map"; return false; }
  commandMap_ = text;
  return true;
}

SonyCommandMapping ConfigStore::commandMapping(const String &id) const {
  SonyCommandMapping result; result.id = id;
  JsonDocument doc;
  if (deserializeJson(doc, commandMap_)) return result;
  JsonObject m = doc["commands"][id];
  if (m.isNull()) return result;
  result.transport = m["transport"] | "http";
  result.method = m["method"] | "POST";
  result.path = m["path"] | "";
  result.contentType = m["contentType"] | "application/x-www-form-urlencoded";
  result.body = m["body"] | "";
  result.extraHeaders = m["extraHeaders"] | "";
  result.rpcMethod = m["rpcMethod"] | "";
  result.rpcParams = m["rpcParams"] | "[]";
  return result;
}

bool ConfigStore::setCommandMapping(const SonyCommandMapping &mapping, String &error) {
  if (!validCommandId(mapping.id)) { error = "Unknown command id"; return false; }
  JsonDocument doc;
  if (deserializeJson(doc, commandMap_)) { error = "Stored command map is corrupt"; return false; }
  doc["commandSchema"] = COMMAND_SCHEMA_VERSION;
  JsonObject m = doc["commands"][mapping.id].to<JsonObject>();
  String transport = mapping.transport; transport.toLowerCase();
  String method = mapping.method; method.toUpperCase();
  if (!validTransport(transport)) { error = "Unsupported mapping transport"; return false; }
  if (transport == "http") {
    if (!validHttpMethod(method)) { error = "Unsupported HTTP method"; return false; }
    if (!validRequestFields(mapping.path, mapping.contentType, mapping.body, mapping.extraHeaders)) {
      error = "Invalid or oversized HTTP request mapping"; return false;
    }
  } else if (!validLinearMethod(mapping.rpcMethod) || !validLinearParams(mapping.rpcParams)) {
    error = "Invalid Sony /linear RPC mapping"; return false;
  }
  m["transport"] = transport;
  m["method"] = method; m["path"] = mapping.path; m["contentType"] = mapping.contentType;
  m["body"] = mapping.body; m["extraHeaders"] = mapping.extraHeaders;
  m["rpcMethod"] = mapping.rpcMethod; m["rpcParams"] = mapping.rpcParams;
  String text; serializeJsonPretty(doc, text);
  if (!writeFileAtomic(COMMAND_PATH, text)) { error = "Could not write command map"; return false; }
  commandMap_ = text;
  return true;
}

void ConfigStore::setDefaultTelemetryMap() {
  JsonDocument doc;
  JsonArray sources = doc["sources"].to<JsonArray>();
  for (int i = 0; i < 4; ++i) {
    JsonObject s = sources.add<JsonObject>();
    s["id"] = "source" + String(i + 1);
    s["enabled"] = false;
    s["method"] = "GET";
    s["path"] = "";
    s["contentType"] = "application/x-www-form-urlencoded";
    s["body"] = "";
    s["extraHeaders"] = "";
    s["intervalMs"] = 500;
    s["timeoutMs"] = 600;
  }
  JsonObject fields = doc["fields"].to<JsonObject>();
  for (const auto &field : CAMERA_STATES) {
    JsonObject f = fields[field.id].to<JsonObject>();
    f["source"] = "";
    f["mode"] = "json";
    f["selector"] = "";
    f["startToken"] = "";
    f["endToken"] = "";
    f["trueValues"] = "";
    f["falseValues"] = "";
  }
  serializeJsonPretty(doc, telemetryMap_);
}

bool ConfigStore::loadTelemetryMap() {
  String raw;
  if (!readFile(TELEMETRY_PATH, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) || !doc["sources"].is<JsonArray>() || !doc["fields"].is<JsonObject>()) return false;
  telemetryMap_ = raw;
  return true;
}

bool ConfigStore::saveTelemetryMapJson(const String &json, String &error) {
  JsonDocument in;
  if (deserializeJson(in, json) || !in["sources"].is<JsonArray>() || !in["fields"].is<JsonObject>()) {
    error = "Expected sources array and fields object"; return false;
  }
  JsonDocument clean;
  JsonArray sources = clean["sources"].to<JsonArray>();
  size_t count = 0;
  for (JsonVariant v : in["sources"].as<JsonArray>()) {
    if (count++ >= 4) break;
    JsonObject src = v.as<JsonObject>();
    JsonObject dst = sources.add<JsonObject>();
    String method = src["method"] | "GET"; method.toUpperCase();
    String path = src["path"] | "";
    String contentType = src["contentType"] | "application/x-www-form-urlencoded";
    String body = src["body"] | "";
    String headers = src["extraHeaders"] | "";
    if (!validHttpMethod(method) || !validRequestFields(path, contentType, body, headers)) {
      error = "Invalid or oversized telemetry source " + String(count);
      return false;
    }
    dst["id"] = src["id"].is<const char *>() ? src["id"].as<String>() : ("source" + String(count));
    dst["enabled"] = src["enabled"] | false;
    dst["method"] = method; dst["path"] = path; dst["contentType"] = contentType;
    dst["body"] = body; dst["extraHeaders"] = headers;
    dst["intervalMs"] = constrain(src["intervalMs"] | 500U, 100U, 10000U);
    dst["timeoutMs"] = constrain(src["timeoutMs"] | 600U, 200U, 1500U);
  }
  while (sources.size() < 4) {
    JsonObject dst = sources.add<JsonObject>();
    dst["id"] = "source" + String(sources.size()); dst["enabled"] = false; dst["method"] = "GET";
    dst["path"] = ""; dst["contentType"] = "application/x-www-form-urlencoded"; dst["body"] = ""; dst["extraHeaders"] = ""; dst["intervalMs"] = 500; dst["timeoutMs"] = 600;
  }
  JsonObject fields = clean["fields"].to<JsonObject>();
  for (const auto &field : CAMERA_STATES) {
    JsonObject src = in["fields"][field.id];
    JsonObject dst = fields[field.id].to<JsonObject>();
    dst["source"] = src["source"] | "";
    dst["mode"] = src["mode"] | "json";
    dst["selector"] = src["selector"] | "";
    dst["startToken"] = src["startToken"] | "";
    dst["endToken"] = src["endToken"] | "";
    dst["trueValues"] = src["trueValues"] | "";
    dst["falseValues"] = src["falseValues"] | "";
  }
  String text; serializeJsonPretty(clean, text);
  if (!writeFileAtomic(TELEMETRY_PATH, text)) { error = "Could not write telemetry map"; return false; }
  telemetryMap_ = text;
  return true;
}

void ConfigStore::setDefaultVsmGenericMap() {
  JsonDocument doc;
  JsonArray triggers = doc["triggers"].to<JsonArray>();
  const char *defaultCommands[16] = {
      "record_start", "stop_replay", "record_stop", "rec_review",
      "awb", "play", "pause", "stop_playback",
      "", "", "", "", "", "", "", ""};
  for (int i = 0; i < 16; ++i) {
    JsonObject o = triggers.add<JsonObject>();
    o["slot"] = i + 1; o["label"] = "Trigger " + String(i + 1); o["command"] = defaultCommands[i];
  }

  auto addSlots = [&doc](const char *key, const char *defaults[], int n) {
    JsonArray arr = doc[key].to<JsonArray>();
    for (int i = 0; i < 8; ++i) {
      JsonObject o = arr.add<JsonObject>();
      o["slot"] = i + 1; o["label"] = String(key) + " " + String(i + 1); o["source"] = i < n ? defaults[i] : "";
    }
  };
  const char *boolDefaults[] = {"bridge.camera_reachable","camera.recording","camera.playback","bridge.macro_busy","bridge.telemetry_fresh","bridge.wifi_connected","bridge.ethernet_connected","camera.atw"};
  const char *intDefaults[] = {"camera.white_balance_k","camera.exposure_index","camera.sq_fps"};
  const char *floatDefaults[] = {"camera.iris_f","camera.gain_db"};
  const char *textDefaults[] = {"camera.white_balance","camera.shutter","camera.nd","camera.gamma","camera.mlut","camera.camera_mode","bridge.last_command","bridge.last_error"};
  addSlots("bools", boolDefaults, 8);
  addSlots("ints", intDefaults, 3);
  addSlots("floats", floatDefaults, 2);
  addSlots("texts", textDefaults, 8);
  serializeJsonPretty(doc, vsmGenericMap_);
}

bool ConfigStore::loadVsmGenericMap() {
  String raw;
  if (!readFile(VSM_GENERIC_PATH, raw)) return false;
  JsonDocument input;
  if (deserializeJson(input, raw) || !input.is<JsonObject>()) return false;
  JsonDocument clean;
  canonicalizeVsmGenericMap(input.as<JsonObjectConst>(), clean);
  String text; serializeJsonPretty(clean, text);
  vsmGenericMap_ = text;
  if (text != raw && !writeFileAtomic(VSM_GENERIC_PATH, text)) {
    Serial.println("[storage] warning: canonical VSM slot map active in RAM but could not be persisted");
  }
  return true;
}

bool ConfigStore::saveVsmGenericMapJson(const String &json, String &error) {
  JsonDocument in;
  if (deserializeJson(in, json) || !in.is<JsonObject>()) { error = "Invalid JSON object"; return false; }
  JsonDocument clean;
  canonicalizeVsmGenericMap(in.as<JsonObjectConst>(), clean);
  String text; serializeJsonPretty(clean, text);
  if (!writeFileAtomic(VSM_GENERIC_PATH, text)) { error = "Could not write VSM generic map"; return false; }
  vsmGenericMap_ = text;
  return true;
}


void ConfigStore::setDefaultSequences() {
  JsonDocument doc;
  JsonArray sequences = doc["sequences"].to<JsonArray>();
  for (int i = 0; i < 8; ++i) {
    JsonObject seq = sequences.add<JsonObject>();
    seq["slot"] = i + 1;
    seq["name"] = "Sequence " + String(i + 1);
    seq["enabled"] = false;
    seq["timeoutMs"] = 60000;
    JsonArray steps = seq["steps"].to<JsonArray>();
    if (i == 0) {
      seq["name"] = "10s Record + State Replay (template)";
      JsonObject a = steps.add<JsonObject>(); a["type"] = "command"; a["command"] = "record_start";
      JsonObject b = steps.add<JsonObject>(); b["type"] = "wait_ms"; b["ms"] = 10000;
      JsonObject c = steps.add<JsonObject>(); c["type"] = "command"; c["command"] = "record_stop";
      JsonObject d = steps.add<JsonObject>(); d["type"] = "wait_state"; d["state"] = "ready"; d["equals"] = "true"; d["timeoutMs"] = 15000; d["freshMs"] = 2500;
      JsonObject e = steps.add<JsonObject>(); e["type"] = "command"; e["command"] = "rec_review";
    }
  }
  serializeJsonPretty(doc, sequences_);
}

bool ConfigStore::loadSequences() {
  String raw;
  if (!readFile(SEQUENCES_PATH, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) || !doc["sequences"].is<JsonArray>()) return false;
  sequences_ = raw;
  return true;
}

bool ConfigStore::saveSequencesJson(const String &json, String &error) {
  JsonDocument in;
  if (deserializeJson(in, json) || !in["sequences"].is<JsonArray>()) {
    error = "Expected a sequences array";
    return false;
  }
  JsonDocument clean;
  JsonArray out = clean["sequences"].to<JsonArray>();
  size_t slot = 0;
  for (JsonVariantConst v : in["sequences"].as<JsonArrayConst>()) {
    if (slot >= 8) break;
    JsonObjectConst src = v.as<JsonObjectConst>();
    JsonObject dst = out.add<JsonObject>();
    dst["slot"] = slot + 1;
    String seqName = src["name"].is<const char *>() ? src["name"].as<String>() : ("Sequence " + String(slot + 1));
    dst["name"] = seqName;
    dst["enabled"] = src["enabled"] | false;
    dst["timeoutMs"] = constrain(src["timeoutMs"] | 60000U, 1000U, 600000U);
    JsonArray steps = dst["steps"].to<JsonArray>();
    size_t stepCount = 0;
    if (src["steps"].is<JsonArrayConst>()) {
      for (JsonVariantConst sv : src["steps"].as<JsonArrayConst>()) {
        if (stepCount >= 16) break;
        JsonObjectConst so = sv.as<JsonObjectConst>();
        String type = so["type"] | ""; type.toLowerCase();
        if (type == "command") {
          String command = so["command"] | "";
          if (!validCommandId(command)) continue;
          if (isHoldOnlyCommand(command)) {
            error = "Hold-only lens movement commands cannot be used as sequence steps";
            return false;
          }
          if (command == "stop_replay") {
            error = "stop_replay is asynchronous and cannot be used as a sequence step";
            return false;
          }
          JsonObject st = steps.add<JsonObject>(); st["type"] = "command"; st["command"] = command;
        } else if (type == "wait_ms") {
          JsonObject st = steps.add<JsonObject>(); st["type"] = "wait_ms"; st["ms"] = std::min<uint32_t>(so["ms"] | 0U, 300000U);
        } else if (type == "wait_state") {
          String state = so["state"] | "";
          if (!validStateId(state)) continue;
          JsonObject st = steps.add<JsonObject>(); st["type"] = "wait_state"; st["state"] = state;
          st["equals"] = so["equals"] | "true";
          st["timeoutMs"] = constrain(so["timeoutMs"] | 15000U, 250U, 300000U);
          st["freshMs"] = constrain(so["freshMs"] | 2500U, 100U, 60000U);
        } else continue;
        ++stepCount;
      }
    }
    ++slot;
  }
  while (out.size() < 8) {
    size_t i = out.size();
    JsonObject seq = out.add<JsonObject>(); seq["slot"] = i + 1; seq["name"] = "Sequence " + String(i + 1);
    seq["enabled"] = false; seq["timeoutMs"] = 60000; seq["steps"].to<JsonArray>();
  }
  String text; serializeJsonPretty(clean, text);
  if (!writeFileAtomic(SEQUENCES_PATH, text)) { error = "Could not write sequences"; return false; }
  sequences_ = text;
  return true;
}

bool ConfigStore::validCommandId(const String &id) const {
  for (const auto &cmd : COMMANDS) if (id == cmd.id) return true;
  return false;
}

bool ConfigStore::validStateId(const String &id) const {
  for (const auto &field : CAMERA_STATES) if (id == field.id) return true;
  return false;
}

String ConfigStore::fullExportJson(bool redactSecrets) const {
  JsonDocument out, cfgDoc, layoutDoc, cmdDoc, telemetryDoc, vsmDoc, sequencesDoc;
  deserializeJson(cfgDoc, configJson(redactSecrets));
  deserializeJson(layoutDoc, layout_);
  deserializeJson(cmdDoc, commandMap_);
  deserializeJson(telemetryDoc, telemetryMap_);
  deserializeJson(vsmDoc, vsmGenericMap_);
  deserializeJson(sequencesDoc, sequences_);
  out["config"] = cfgDoc;
  out["layout"] = layoutDoc;
  out["commandMap"] = cmdDoc;
  out["telemetry"] = telemetryDoc;
  out["vsmGeneric"] = vsmDoc;
  out["sequences"] = sequencesDoc;
  String text; serializeJsonPretty(out, text); return text;
}
