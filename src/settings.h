#pragma once

#include <Arduino.h>

#include "psk_reporter.h"

enum DxSourceMode : uint8_t {
  kDxSourceJson = 0,
  kDxSourceTelnet = 1,
  kDxSourceAuto = 2
};

// Every dashboard page taking part in the automatic page change. One bit per
// page, bit 0 being page 1; the static_assert in dashboard_display.cpp keeps
// this in step with the number of pages the dashboard actually has.
constexpr uint8_t kAutoPageMaskAll = 0x7F;

struct AppSettings {
  String wifiSsid;
  String wifiPassword;
  String callsign;
  String timezone;
  String timezoneLabel;
  String locator;
  // Show the page 1 local clock as 12-hour with AM/PM rather than 24-hour.
  bool clock12Hour;
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
  bool autoPageChange;
  uint16_t autoPageSeconds;
  // Pages included in the automatic rotation, as a kAutoPageMaskAll bitmask.
  uint8_t autoPageMask;
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
