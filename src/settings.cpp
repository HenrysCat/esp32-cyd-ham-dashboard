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
// PSKReporter asks for no more than one query every five minutes, so that is
// both the default and the lowest value the settings page will accept.
constexpr uint16_t kMinPskRefreshMinutes = 5;
constexpr uint16_t kDefaultPskRefreshMinutes = 5;
constexpr uint16_t kDefaultPskWindowMinutes = 60;
// POTA publishes no formal rate limit; a minute is the interval existing
// client libraries settle on, so it is the lowest value offered here.
constexpr uint16_t kMinPotaRefreshMinutes = 1;
constexpr uint16_t kDefaultPotaRefreshMinutes = 5;
constexpr uint8_t kDefaultBrightnessPercent = 100;
// Long enough to read a page before it moves on, and the floor keeps the
// rotation from outrunning the map pages, which take a moment to redraw.
constexpr uint16_t kDefaultAutoPageSeconds = 15;
constexpr uint16_t kMinAutoPageSeconds = 3;
constexpr uint16_t kMaxAutoPageSeconds = 600;
constexpr uint8_t kDefaultNightBrightnessPercent = 20;
// Roughly the length of civil twilight at temperate latitudes, so the default
// fade tracks the light outside reasonably closely.
constexpr uint16_t kDefaultNightFadeMinutes = 40;
constexpr uint16_t kMinNightFadeMinutes = 1;
constexpr uint16_t kMaxNightFadeMinutes = 240;

#ifndef SWAP_RED_BLUE_DEFAULT
#define SWAP_RED_BLUE_DEFAULT false
#endif
#ifndef ROTATE90_DEFAULT
#define ROTATE90_DEFAULT false
#endif
#ifndef FLIP180_DEFAULT
#define FLIP180_DEFAULT false
#endif
#ifndef MIRROR_DEFAULT
#define MIRROR_DEFAULT false
#endif
#ifndef INVERT_COLOURS_DEFAULT
#define INVERT_COLOURS_DEFAULT false
#endif
#ifndef SWAP_TOUCH_NAV_DEFAULT
#define SWAP_TOUCH_NAV_DEFAULT false
#endif

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
  for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
    settings.wifiNetworks[i].ssid =
        limitedUntrimmedString(settings.wifiNetworks[i].ssid, 64);
    settings.wifiNetworks[i].password =
        limitedUntrimmedString(settings.wifiNetworks[i].password, 64);
    // A password without a network is not useful and should not linger in
    // preferences after the corresponding entry has been removed.
    if (settings.wifiNetworks[i].ssid.length() == 0) {
      settings.wifiNetworks[i].password = "";
    }
  }
  settings.callsign = limitedString(settings.callsign, 16);
  settings.callsign.toUpperCase();
  settings.timezone = limitedString(settings.timezone, 80);
  settings.timezoneLabel = limitedString(settings.timezoneLabel, 24);
  settings.locator = limitedString(settings.locator, 6);
  settings.locator.toUpperCase();
  settings.propagationJsonUrl = limitedString(settings.propagationJsonUrl, 180);
  settings.dxSpotsUrl = limitedString(settings.dxSpotsUrl, 180);
  settings.dxTelnetHost = limitedString(settings.dxTelnetHost, 64);
  settings.pskAppContact = limitedString(settings.pskAppContact, 64);
  settings.n2yoApiKey = limitedString(settings.n2yoApiKey, 64);

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
  if (settings.pskDirection != kPskWhoHearsMe && settings.pskDirection != kPskWhoIHear) {
    settings.pskDirection = kPskWhoHearsMe;
  }
  settings.pskWindowMinutes =
      constrain(settings.pskWindowMinutes, static_cast<uint16_t>(5),
                static_cast<uint16_t>(360));
  settings.pskRefreshMinutes =
      constrain(settings.pskRefreshMinutes, kMinPskRefreshMinutes, static_cast<uint16_t>(120));
  settings.potaRefreshMinutes =
      constrain(settings.potaRefreshMinutes, kMinPotaRefreshMinutes, static_cast<uint16_t>(120));
  // Zero is meaningful here - it disables the distance filter - so only the
  // upper bound is clamped.
  if (settings.potaMaxDistanceKm > 20000) {
    settings.potaMaxDistanceKm = 20000;
  }
  settings.brightnessPercent =
      constrain(settings.brightnessPercent, static_cast<uint8_t>(5),
                static_cast<uint8_t>(100));
  settings.autoPageSeconds =
      constrain(settings.autoPageSeconds, kMinAutoPageSeconds, kMaxAutoPageSeconds);
  // An empty selection is left as it is rather than being filled back in: the
  // rotation simply has nowhere to go, which is what unticking everything asks
  // for.
  settings.autoPageMask &= kAutoPageMaskAll;
  settings.nightBrightnessPercent =
      constrain(settings.nightBrightnessPercent, static_cast<uint8_t>(5),
                static_cast<uint8_t>(100));
  settings.nightFadeMinutes =
      constrain(settings.nightFadeMinutes, kMinNightFadeMinutes, kMaxNightFadeMinutes);
}
}

void settingsBegin() {
  preferences.begin(kNamespace, false);

  // wifi0* is the new multi-network format.  Fall back to the original
  // ssid/pass keys so installed devices retain their configured network.
  for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
    const String suffix = String(i);
    currentSettings.wifiNetworks[i].ssid = preferences.getString(("wifi" + suffix + "s").c_str(), "");
    currentSettings.wifiNetworks[i].password = preferences.getString(("wifi" + suffix + "p").c_str(), "");
  }
  if (currentSettings.wifiNetworks[0].ssid.length() == 0) {
    currentSettings.wifiNetworks[0].ssid = preferences.getString("ssid", "");
    currentSettings.wifiNetworks[0].password = preferences.getString("pass", "");
  }

  if (currentSettings.wifiNetworks[0].ssid.length() == 0 && !isPlaceholderCredential(WIFI_SSID)) {
    currentSettings.wifiNetworks[0].ssid = WIFI_SSID;
    currentSettings.wifiNetworks[0].password = WIFI_PASSWORD;
  }

  currentSettings.timezone = readStringOrDefault("tz", kDefaultTimezone);
  currentSettings.timezoneLabel = readStringOrDefault("tzlabel", kDefaultTimezoneLabel);
  currentSettings.locator = readStringOrDefault("locator", MAIDENHEAD_LOCATOR);
  currentSettings.callsign = preferences.getString("callsign", "");
  currentSettings.clock12Hour = preferences.getBool("clock12", false);
  currentSettings.swapUtcLocal = preferences.getBool("swaputc", false);
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
  currentSettings.pskDirection = static_cast<PskDirection>(
      preferences.getUChar("pskdir", static_cast<uint8_t>(kPskWhoHearsMe)));
  currentSettings.pskWindowMinutes = preferences.getUShort("pskwin", kDefaultPskWindowMinutes);
  currentSettings.pskRefreshMinutes = preferences.getUShort("pskmins", kDefaultPskRefreshMinutes);
  currentSettings.pskAppContact = preferences.getString("pskmail", "");
  currentSettings.potaRefreshMinutes =
      preferences.getUShort("potamins", kDefaultPotaRefreshMinutes);
  currentSettings.potaMaxDistanceKm = preferences.getUShort("potadist", 0);
  currentSettings.potaExcludeRbn = preferences.getBool("potarbn", false);
  currentSettings.issEnabled = preferences.getBool("issenabled", false);
  currentSettings.n2yoApiKey = preferences.getString("n2yokey", "");
  currentSettings.autoPageChange = preferences.getBool("autopage", false);
  currentSettings.autoPageSeconds =
      preferences.getUShort("autosecs", kDefaultAutoPageSeconds);
  currentSettings.autoPageMask = preferences.getUChar("autopages", kAutoPageMaskAll);
  currentSettings.brightnessPercent =
      preferences.getUChar("bright", kDefaultBrightnessPercent);
  currentSettings.nightDimEnabled = preferences.getBool("nightdim", false);
  currentSettings.nightBrightnessPercent =
      preferences.getUChar("nightpct", kDefaultNightBrightnessPercent);
  currentSettings.nightFadeMinutes =
      preferences.getUShort("nightfade", kDefaultNightFadeMinutes);
  currentSettings.swapRedBlueChannels =
      preferences.getBool("swaprb", SWAP_RED_BLUE_DEFAULT);
  currentSettings.rotate90 = preferences.getBool("rot90", ROTATE90_DEFAULT);
  currentSettings.flip180 = preferences.getBool("flip180", FLIP180_DEFAULT);
  currentSettings.mirror = preferences.getBool("mirror", MIRROR_DEFAULT);
  currentSettings.invertColours = preferences.getBool("invert", INVERT_COLOURS_DEFAULT);
  currentSettings.swapTouchNav = preferences.getBool("touchswap", SWAP_TOUCH_NAV_DEFAULT);
  currentSettings.keepHotspotOn = preferences.getBool("apalwayson", false);
  normalizeSettings(currentSettings);
}

const AppSettings& getSettings() {
  return currentSettings;
}

void saveSettings(const AppSettings& settings) {
  currentSettings = settings;
  normalizeSettings(currentSettings);

  for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
    const String suffix = String(i);
    preferences.putString(("wifi" + suffix + "s").c_str(), currentSettings.wifiNetworks[i].ssid);
    preferences.putString(("wifi" + suffix + "p").c_str(), currentSettings.wifiNetworks[i].password);
  }
  preferences.putString("callsign", currentSettings.callsign);
  preferences.putBool("clock12", currentSettings.clock12Hour);
  preferences.putBool("swaputc", currentSettings.swapUtcLocal);
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
  preferences.putUChar("pskdir", static_cast<uint8_t>(currentSettings.pskDirection));
  preferences.putUShort("pskwin", currentSettings.pskWindowMinutes);
  preferences.putUShort("pskmins", currentSettings.pskRefreshMinutes);
  preferences.putString("pskmail", currentSettings.pskAppContact);
  preferences.putUShort("potamins", currentSettings.potaRefreshMinutes);
  preferences.putUShort("potadist", currentSettings.potaMaxDistanceKm);
  preferences.putBool("potarbn", currentSettings.potaExcludeRbn);
  preferences.putBool("issenabled", currentSettings.issEnabled);
  preferences.putString("n2yokey", currentSettings.n2yoApiKey);
  preferences.putBool("autopage", currentSettings.autoPageChange);
  preferences.putUShort("autosecs", currentSettings.autoPageSeconds);
  preferences.putUChar("autopages", currentSettings.autoPageMask);
  preferences.putUChar("bright", currentSettings.brightnessPercent);
  preferences.putBool("nightdim", currentSettings.nightDimEnabled);
  preferences.putUChar("nightpct", currentSettings.nightBrightnessPercent);
  preferences.putUShort("nightfade", currentSettings.nightFadeMinutes);
  preferences.putBool("swaprb", currentSettings.swapRedBlueChannels);
  preferences.putBool("rot90", currentSettings.rotate90);
  preferences.putBool("flip180", currentSettings.flip180);
  preferences.putBool("mirror", currentSettings.mirror);
  preferences.putBool("invert", currentSettings.invertColours);
  preferences.putBool("touchswap", currentSettings.swapTouchNav);
  preferences.putBool("apalwayson", currentSettings.keepHotspotOn);
}

bool hasWifiCredentials() {
  for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
    if (currentSettings.wifiNetworks[i].ssid.length() > 0) {
      return true;
    }
  }
  return false;
}

void factoryResetSettings() {
  preferences.clear();
  settingsBegin();
}
