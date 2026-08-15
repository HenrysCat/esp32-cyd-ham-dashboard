#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

struct ClockSnapshot {
  bool wifiConnected;
  bool timeValid;
  time_t epoch;
  uint32_t uptimeSeconds;
};

void connectivityBegin();
void connectivityLoop();
void reconnectWifi();
void applyTimezoneSettings();
ClockSnapshot getClockSnapshot();

// Single place that sets up every outgoing TLS client. This core's
// WiFiClientSecure exposes no buffer sizing, so mbedTLS keeps its compiled-in
// 16KB record buffers and TLS memory cannot be tuned from here.
void configureSecureClient(WiFiClientSecure& client);
