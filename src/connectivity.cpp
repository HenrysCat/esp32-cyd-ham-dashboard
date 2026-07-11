#include "connectivity.h"

#include <WiFi.h>
#include <time.h>

#include "settings.h"

namespace {
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";
constexpr char kNtpServer3[] = "time.google.com";

constexpr time_t kValidTimeThreshold = 1704067200; // 2024-01-01 00:00:00 UTC
constexpr uint32_t kReconnectIntervalMs = 10000;

uint32_t g_lastReconnectAttemptMs = 0;

bool isTimeValid(time_t now) {
  return now >= kValidTimeThreshold;
}

void beginWifi() {
  const AppSettings& settings = getSettings();
  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  if (settings.wifiSsid.length() > 0) {
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
  }
}
}

void connectivityBegin() {
  Serial.println();
  Serial.println("CYD HamClock phase 1 starting");
  const AppSettings& settings = getSettings();
  Serial.print("Configured Wi-Fi SSID: ");
  Serial.println(settings.wifiSsid.length() > 0 ? settings.wifiSsid : "(none)");

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

void reconnectWifi() {
  const AppSettings& settings = getSettings();
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false);
  if (settings.wifiSsid.length() == 0) {
    return;
  }

  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
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
