#include <Arduino.h>

#include "connectivity.h"
#include "dashboard_display.h"
#include "reset_button.h"
#include "settings.h"
#include "setup_portal.h"

namespace {
constexpr uint32_t kLoopDelayMs = 10;
constexpr uint32_t kWifiSpinnerIntervalMs = 180;

bool g_showWifiStartupStatus = false;
uint8_t g_wifiSpinnerFrame = 0;
uint32_t g_lastWifiSpinnerMs = 0;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  settingsBegin();
  displayBegin();
  // Show this before any Wi-Fi work starts. WiFiMulti's initial multi-network
  // scan is synchronous, so this stays visible on every supported panel until
  // that scan and connection attempt complete.
  if (hasWifiCredentials()) {
    g_showWifiStartupStatus = true;
    displayShowWifiSearching(0);
  }
  resetButtonBegin();
  setupPortalBegin();
  connectivityBegin();
  // Do not render Page 1 over the connection screen. The loop switches to
  // the dashboard as soon as the background Wi-Fi attempt completes.
  if (!g_showWifiStartupStatus || !wifiConnectionInProgress()) {
    displayUpdate(getClockSnapshot());
  }
}

void loop() {
  setupPortalLoop();
  connectivityLoop();
  const bool resettingNow = resetButtonLoop();
  const uint32_t nowMs = millis();
  if (g_showWifiStartupStatus) {
    if (wifiConnectionInProgress()) {
      if (nowMs - g_lastWifiSpinnerMs >= kWifiSpinnerIntervalMs) {
        g_lastWifiSpinnerMs = nowMs;
        displayShowWifiSearching(++g_wifiSpinnerFrame);
      }
      delay(kLoopDelayMs);
      return;
    }

    // Force a complete dashboard paint after the exclusive startup screen.
    g_showWifiStartupStatus = false;
    requestDisplayRedraw();
  }
  if (!resettingNow) {
    displayUpdate(getClockSnapshot());
  }
  delay(kLoopDelayMs);
}
