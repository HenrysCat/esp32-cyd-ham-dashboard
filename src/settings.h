#pragma once

#include <Arduino.h>

enum DxSourceMode : uint8_t {
  kDxSourceJson = 0,
  kDxSourceTelnet = 1,
  kDxSourceAuto = 2
};

struct AppSettings {
  String wifiSsid;
  String wifiPassword;
  String callsign;
  String timezone;
  String timezoneLabel;
  String locator;
  bool useJsonPropagationProxy;
  String propagationJsonUrl;
  DxSourceMode dxSourceMode;
  String dxSpotsUrl;
  String dxTelnetHost;
  uint16_t dxTelnetPort;
  uint16_t propagationRefreshMinutes;
  uint16_t dxRefreshMinutes;
  uint8_t brightnessPercent;
  bool swapRedBlueChannels;
};

void settingsBegin();
const AppSettings& getSettings();
void saveSettings(const AppSettings& settings);
bool hasWifiCredentials();
