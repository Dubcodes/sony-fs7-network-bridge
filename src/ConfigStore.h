#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "AppTypes.h"

class ConfigStore {
 public:
  bool begin();
  bool storageReady() const { return mounted_; }

  const BridgeConfig &config() const { return cfg_; }
  BridgeConfig &configMutable() { return cfg_; }

  bool saveConfig();
  bool updateConfigJson(const String &json, String &error);
  String configJson(bool redactSecrets = true) const;

  String layoutJson() const;
  bool saveLayoutJson(const String &json, String &error);

  String commandMapJson() const;
  bool saveCommandMapJson(const String &json, String &error);
  SonyCommandMapping commandMapping(const String &id) const;
  bool setCommandMapping(const SonyCommandMapping &mapping, String &error);

  String telemetryMapJson() const { return telemetryMap_; }
  bool saveTelemetryMapJson(const String &json, String &error);

  String vsmGenericMapJson() const { return vsmGenericMap_; }
  bool saveVsmGenericMapJson(const String &json, String &error);

  String sequencesJson() const { return sequences_; }
  bool saveSequencesJson(const String &json, String &error);

  String fullExportJson(bool redactSecrets = true) const;
  bool validCommandId(const String &id) const;
  bool validStateId(const String &id) const;

 private:
  bool mounted_ = false;
  BridgeConfig cfg_;
  String layout_;
  String commandMap_;
  String telemetryMap_;
  String vsmGenericMap_;
  String sequences_;

  bool loadConfig();
  bool loadLayout();
  bool loadCommandMap();
  bool loadTelemetryMap();
  bool loadVsmGenericMap();
  bool loadSequences();
  bool saveConfigValue(const BridgeConfig &value);
  void setDefaultLayout();
  void setDefaultCommandMap();
  void setDefaultTelemetryMap();
  void setDefaultVsmGenericMap();
  void setDefaultSequences();
  bool writeFileAtomic(const char *path, const String &data);
  bool readFile(const char *path, String &out);
};
