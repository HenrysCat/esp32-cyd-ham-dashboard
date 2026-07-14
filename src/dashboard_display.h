#pragma once

#include <Arduino.h>

#include "connectivity.h"

void displayBegin();
void displayUpdate(const ClockSnapshot& snapshot);
void applyDisplaySettings();
uint8_t getCurrentDashboardPageNumber();
void displayShowMessage(const String& title, const String& subtitle);
void requestDisplayRedraw();
