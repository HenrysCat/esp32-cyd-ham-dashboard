#include <Arduino.h>

#include "connectivity.h"
#include "dashboard_display.h"
#include "settings.h"
#include "setup_portal.h"

namespace {
constexpr uint32_t kLoopDelayMs = 10;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  settingsBegin();
  displayBegin();
  setupPortalBegin();
  connectivityBegin();
  displayUpdate(getClockSnapshot());
}

void loop() {
  setupPortalLoop();
  connectivityLoop();
  displayUpdate(getClockSnapshot());
  delay(kLoopDelayMs);
}
