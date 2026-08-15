#pragma once

#include <Arduino.h>

#include "psk_reporter.h"

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
  PskDirection pskDirection;
  uint16_t pskWindowMinutes;
  String pskAppContact;
  // 0 means no distance limit on POTA spots.
  uint16_t potaMaxDistanceKm;
  bool potaExcludeRbn;
  uint16_t propagationRefreshMinutes;
  uint16_t dxRefreshMinutes;
  uint16_t pskRefreshMinutes;
  uint16_t potaRefreshMinutes;
  uint8_t brightnessPercent;
  bool swapRedBlueChannels;
  bool rotate90;
  bool flip180;
  bool mirror;
  bool invertColours;
  bool swapTouchNav;
  bool keepHotspotOn;
};

void settingsBegin();
const AppSettings& getSettings();
void saveSettings(const AppSettings& settings);
bool hasWifiCredentials();
void factoryResetSettings();
