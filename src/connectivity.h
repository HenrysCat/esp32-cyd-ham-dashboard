#pragma once

#include <Arduino.h>

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
