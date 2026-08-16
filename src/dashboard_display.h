#pragma once

#include <Arduino.h>

#include "connectivity.h"

// Number of pages the dashboard cycles through. Exposed so the settings page
// can offer one auto-change checkbox per page.
constexpr uint8_t kDashboardPageCount = 7;

// Display name of a page, indexed from zero. Returns "" past the last page.
const char* dashboardPageName(uint8_t pageIndex);

void displayBegin();
void displayUpdate(const ClockSnapshot& snapshot);
void applyDisplaySettings();
uint8_t getCurrentDashboardPageNumber();
void displayShowMessage(const String& title, const String& subtitle);
void requestDisplayRedraw();
