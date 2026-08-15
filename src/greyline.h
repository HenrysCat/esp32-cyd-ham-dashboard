#pragma once

#include <Arduino.h>
#include <time.h>

struct GreylineData {
  bool valid;
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
// Great-circle distance in kilometres, used by the pages that report how far
// away a spot or reception report is.
uint32_t greatCircleKm(double lat1, double lon1, double lat2, double lon2);
