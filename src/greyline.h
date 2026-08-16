#pragma once

#include <Arduino.h>
#include <time.h>

struct GreylineData {
  bool valid;
  // Sun below the horizon at the configured QTH. Twilight counts as down once
  // the sun has actually set, so this is a plain geometric test rather than a
  // reading of the status text below.
  bool sunIsDown;
  double latitudeValue;
  double longitudeValue;
  double sunLatitudeValue;
  double sunLongitudeValue;
  String qth;
  String latitude;
  String longitude;
  String utcTime;
  String sunriseUtc;
  String sunsetUtc;
  String noonUtc;
  String dayLength;
  String sunLatitude;
  String sunLongitude;
  String status;
  String greyline;
};

void greylineBegin();
void requestGreylineRefresh();
bool updateGreylineData(time_t epoch, bool timeValid);
const GreylineData& getGreylineData();
String getConfiguredLocator();
bool getConfiguredLatitude(double& latitude);
bool getConfiguredLongitude(double& longitude);
bool maidenheadToLatLon(const String& locator, double& latitude, double& longitude);
// 0.0 in full daylight through to 1.0 in full darkness, ramping across a window
// of fadeMinutes centred on sunrise and sunset. Takes the epoch rather than
// reading the once-a-minute cached time, so a backlight fade driven from this
// moves smoothly instead of stepping once a minute.
float greylineNightFraction(time_t epoch, uint16_t fadeMinutes);
// Great-circle distance in kilometres, used by the pages that report how far
// away a spot or reception report is.
uint32_t greatCircleKm(double lat1, double lon1, double lat2, double lon2);
