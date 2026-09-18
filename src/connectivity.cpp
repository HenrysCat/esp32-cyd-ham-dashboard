#include "connectivity.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <time.h>

#include "settings.h"

namespace {
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";
constexpr char kNtpServer3[] = "time.google.com";

constexpr time_t kValidTimeThreshold = 1704067200; // 2024-01-01 00:00:00 UTC
constexpr uint32_t kReconnectIntervalMs = 10000;

uint32_t g_lastReconnectAttemptMs = 0;
WiFiMulti* g_wifiMulti = nullptr;

bool isTimeValid(time_t now) {
  return now >= kValidTimeThreshold;
}

void configureWifiNetworks() {
  const AppSettings& settings = getSettings();
  // WiFiMulti keeps its own AP list. Recreate it when settings are saved so a
  // changed password or a removed network takes effect without a reboot.
  delete g_wifiMulti;
  g_wifiMulti = new WiFiMulti();
  for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
    const WifiNetwork& network = settings.wifiNetworks[i];
    if (network.ssid.length() > 0) {
      g_wifiMulti->addAP(network.ssid.c_str(), network.password.c_str());
    }
  }
}

void beginWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  configureWifiNetworks();
  if (hasWifiCredentials()) {
    g_wifiMulti->run();
  }
}
}

void connectivityBegin() {
  Serial.println();
  Serial.println("CYD HamClock phase 1 starting");
  const AppSettings& settings = getSettings();
  uint8_t networkCount = 0;
  for (uint8_t i = 0; i < kMaxWifiNetworks; ++i) {
    networkCount += settings.wifiNetworks[i].ssid.length() > 0;
  }
  Serial.printf("Configured Wi-Fi networks: %u\n", networkCount);

  beginWifi();
  configTzTime(settings.timezone.c_str(), kNtpServer1, kNtpServer2, kNtpServer3);
}

void connectivityLoop() {
  const uint32_t nowMs = millis();
  if (hasWifiCredentials() &&
      WiFi.status() != WL_CONNECTED &&
      nowMs - g_lastReconnectAttemptMs >= kReconnectIntervalMs) {
    g_lastReconnectAttemptMs = nowMs;
    reconnectWifi();
  }
}

void reloadWifiNetworks() {
  configureWifiNetworks();
}

void reconnectWifi() {
  // Mode is owned by setup_portal (it decides when the setup hotspot is on
  // or off), so only touch the STA side here and leave the mode alone.
  if (hasWifiCredentials()) {
    WiFi.disconnect(false);
    g_wifiMulti->run();
  }
}

void applyTimezoneSettings() {
  const AppSettings& settings = getSettings();
  setenv("TZ", settings.timezone.c_str(), 1);
  tzset();
}

ClockSnapshot getClockSnapshot() {
  const time_t now = time(nullptr);

  ClockSnapshot snapshot;
  snapshot.wifiConnected = WiFi.status() == WL_CONNECTED;
  snapshot.timeValid = isTimeValid(now);
  snapshot.epoch = now;
  snapshot.uptimeSeconds = millis() / 1000;
  return snapshot;
}

void configureSecureClient(WiFiClientSecure& client) {
  client.setInsecure();
}
