#include "settings.h"

#include <Preferences.h>

#include "app_config.h"

#ifndef DX_SPOTS_URL
#define DX_SPOTS_URL ""
#endif

namespace {
Preferences preferences;
AppSettings currentSettings;

constexpr char kNamespace[] = "hamclock";
constexpr char kDefaultTimezone[] = "GMT0BST-1,M3.5.0/1,M10.5.0/2";
constexpr char kDefaultTimezoneLabel[] = "UK local";
constexpr char kDefaultDxSpotsUrl[] = "https://web.cluster.iz3mez.it/spots.json";
constexpr char kDefaultDxTelnetHost[] = "dxspots.com";
constexpr uint16_t kDefaultDxTelnetPort = 7300;
constexpr uint16_t kDefaultPropagationRefreshMinutes = 15;
constexpr uint16_t kDefaultDxRefreshMinutes = 5;
constexpr uint8_t kDefaultBrightnessPercent = 100;

bool isPlaceholderCredential(const char* value) {
  return value == nullptr || value[0] == '\0' || String(value).startsWith("your-");
}

String readStringOrDefault(const char* key, const char* fallback) {
  const String value = preferences.getString(key, "");
  return value.length() > 0 ? value : String(fallback);
}

String limitedString(String value, size_t maxLen) {
  value.trim();
  if (value.length() > maxLen) {
    value = value.substring(0, maxLen);
  }
  return value;
}

String limitedUntrimmedString(String value, size_t maxLen) {
  if (value.length() > maxLen) {
    value = value.substring(0, maxLen);
  }
  return value;
}

const char* defaultDxSpotsUrl() {
  return DX_SPOTS_URL[0] == '\0' ? kDefaultDxSpotsUrl : DX_SPOTS_URL;
}

bool isValidMaidenheadLocator(const String& locator) {
  if (locator.length() != 4 && locator.length() != 6) {
    return false;
  }
  if (locator[0] < 'A' || locator[0] > 'R' || locator[1] < 'A' || locator[1] > 'R' ||
      locator[2] < '0' || locator[2] > '9' || locator[3] < '0' || locator[3] > '9') {
    return false;
  }
  return locator.length() == 4 ||
         (locator[4] >= 'A' && locator[4] <= 'X' &&
          locator[5] >= 'A' && locator[5] <= 'X');
}

void normalizeSettings(AppSettings& settings) {
  settings.wifiSsid = limitedUntrimmedString(settings.wifiSsid, 64);
  settings.wifiPassword = limitedUntrimmedString(settings.wifiPassword, 64);
  settings.callsign = limitedString(settings.callsign, 16);
  settings.callsign.toUpperCase();
  settings.timezone = limitedString(settings.timezone, 80);
  settings.timezoneLabel = limitedString(settings.timezoneLabel, 24);
  settings.locator = limitedString(settings.locator, 6);
  settings.locator.toUpperCase();
  settings.propagationJsonUrl = limitedString(settings.propagationJsonUrl, 180);
  settings.dxSpotsUrl = limitedString(settings.dxSpotsUrl, 180);
  settings.dxTelnetHost = limitedString(settings.dxTelnetHost, 64);

  if (settings.timezone.length() == 0) {
    settings.timezone = kDefaultTimezone;
  }
  if (settings.timezoneLabel.length() == 0) {
    settings.timezoneLabel = kDefaultTimezoneLabel;
  }
  if (!isValidMaidenheadLocator(settings.locator)) {
    settings.locator = MAIDENHEAD_LOCATOR;
  }
  if (settings.dxSpotsUrl.length() == 0) {
    settings.dxSpotsUrl = defaultDxSpotsUrl();
  }
  if (settings.dxSourceMode != kDxSourceJson && settings.dxSourceMode != kDxSourceTelnet &&
      settings.dxSourceMode != kDxSourceAuto) {
    settings.dxSourceMode = kDxSourceAuto;
  }
  if (settings.dxTelnetHost.length() == 0) {
    settings.dxTelnetHost = kDefaultDxTelnetHost;
  }
  if (settings.dxTelnetPort == 0) {
    settings.dxTelnetPort = kDefaultDxTelnetPort;
  }
  settings.propagationRefreshMinutes =
      constrain(settings.propagationRefreshMinutes, static_cast<uint16_t>(1),
                static_cast<uint16_t>(120));
  settings.dxRefreshMinutes =
      constrain(settings.dxRefreshMinutes, static_cast<uint16_t>(1),
                static_cast<uint16_t>(120));
  settings.brightnessPercent =
      constrain(settings.brightnessPercent, static_cast<uint8_t>(5),
                static_cast<uint8_t>(100));
}
}

void settingsBegin() {
  preferences.begin(kNamespace, false);

  currentSettings.wifiSsid = preferences.getString("ssid", "");
  currentSettings.wifiPassword = preferences.getString("pass", "");

  if (currentSettings.wifiSsid.length() == 0 && !isPlaceholderCredential(WIFI_SSID)) {
    currentSettings.wifiSsid = WIFI_SSID;
    currentSettings.wifiPassword = WIFI_PASSWORD;
  }

  currentSettings.timezone = readStringOrDefault("tz", kDefaultTimezone);
  currentSettings.timezoneLabel = readStringOrDefault("tzlabel", kDefaultTimezoneLabel);
  currentSettings.locator = readStringOrDefault("locator", MAIDENHEAD_LOCATOR);
  currentSettings.callsign = preferences.getString("callsign", "");
  currentSettings.useJsonPropagationProxy = preferences.getBool("propjson", false);
  currentSettings.propagationJsonUrl = readStringOrDefault("propurl", PROPAGATION_JSON_URL);
  currentSettings.dxSourceMode = static_cast<DxSourceMode>(
      preferences.getUChar("dxmode", static_cast<uint8_t>(kDxSourceAuto)));
  currentSettings.dxSpotsUrl = readStringOrDefault("dxurl", defaultDxSpotsUrl());
  currentSettings.dxTelnetHost = readStringOrDefault("dxhost", kDefaultDxTelnetHost);
  currentSettings.dxTelnetPort = preferences.getUShort("dxport", kDefaultDxTelnetPort);
  currentSettings.propagationRefreshMinutes =
      preferences.getUShort("propmins", kDefaultPropagationRefreshMinutes);
  currentSettings.dxRefreshMinutes = preferences.getUShort("dxmins", kDefaultDxRefreshMinutes);
  currentSettings.brightnessPercent =
      preferences.getUChar("bright", kDefaultBrightnessPercent);
  normalizeSettings(currentSettings);
}

const AppSettings& getSettings() {
  return currentSettings;
}

void saveSettings(const AppSettings& settings) {
  currentSettings = settings;
  normalizeSettings(currentSettings);

  preferences.putString("ssid", currentSettings.wifiSsid);
  preferences.putString("pass", currentSettings.wifiPassword);
  preferences.putString("callsign", currentSettings.callsign);
  preferences.putString("tz", currentSettings.timezone);
  preferences.putString("tzlabel", currentSettings.timezoneLabel);
  preferences.putString("locator", currentSettings.locator);
  preferences.putBool("propjson", currentSettings.useJsonPropagationProxy);
  preferences.putString("propurl", currentSettings.propagationJsonUrl);
  preferences.putUChar("dxmode", static_cast<uint8_t>(currentSettings.dxSourceMode));
  preferences.putString("dxurl", currentSettings.dxSpotsUrl);
  preferences.putString("dxhost", currentSettings.dxTelnetHost);
  preferences.putUShort("dxport", currentSettings.dxTelnetPort);
  preferences.putUShort("propmins", currentSettings.propagationRefreshMinutes);
  preferences.putUShort("dxmins", currentSettings.dxRefreshMinutes);
  preferences.putUChar("bright", currentSettings.brightnessPercent);
}

bool hasWifiCredentials() {
  return currentSettings.wifiSsid.length() > 0;
}
