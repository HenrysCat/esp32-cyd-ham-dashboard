#include "greyline.h"

#include <math.h>

#include "settings.h"

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;
constexpr double kDeg = 180.0 / kPi;
constexpr uint32_t kUpdateIntervalMs = 60000;
constexpr double kSunriseZenithDeg = 90.833;
constexpr int kGreylineWindowMinutes = 45;

enum SunTimeResult : uint8_t {
  kSunTimesNormal,
  kPolarDay,
  kPolarNight
};

GreylineData g_data;
uint32_t g_lastUpdateMs = 0;
time_t g_lastEpochMinute = 0;

String formatDouble(double value, uint8_t decimals) {
  char buffer[20];
  dtostrf(value, 0, decimals, buffer);
  return String(buffer);
}

double normalizeLongitude(double longitude) {
  while (longitude < -180.0) longitude += 360.0;
  while (longitude > 180.0) longitude -= 360.0;
  return longitude;
}

String formatUtcMinutes(double minutes) {
  while (minutes < 0.0) minutes += 1440.0;
  while (minutes >= 1440.0) minutes -= 1440.0;

  const int total = static_cast<int>(minutes + 0.5) % 1440;
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%02d:%02d UTC", total / 60, total % 60);
  return String(buffer);
}

String formatDayLength(double minutes) {
  if (minutes < 0.0) {
    minutes += 1440.0;
  }

  const int total = static_cast<int>(minutes + 0.5);
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%dh %02dm", total / 60, total % 60);
  return String(buffer);
}

String formatUtcClock(time_t epoch) {
  tm utc;
  gmtime_r(&epoch, &utc);

  char buffer[16];
  strftime(buffer, sizeof(buffer), "%H:%M UTC", &utc);
  return String(buffer);
}

int dayOfYear(const tm& utc) {
  return utc.tm_yday + 1;
}

double fractionalYearRad(int dayOfYearValue, double hourUtc) {
  return (2.0 * kPi / 365.0) * (dayOfYearValue - 1 + ((hourUtc - 12.0) / 24.0));
}

double equationOfTimeMinutes(double gamma) {
  return 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma) -
                   0.014615 * cos(2.0 * gamma) - 0.040849 * sin(2.0 * gamma));
}

double solarDeclinationRad(double gamma) {
  return 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma) -
         0.006758 * cos(2.0 * gamma) + 0.000907 * sin(2.0 * gamma) -
         0.002697 * cos(3.0 * gamma) + 0.00148 * sin(3.0 * gamma);
}

int normalizeMinute(int minute) {
  minute %= 1440;
  return minute < 0 ? minute + 1440 : minute;
}

int circularMinuteDiff(int a, int b) {
  a = normalizeMinute(a);
  b = normalizeMinute(b);
  int diff = abs(a - b);
  return diff > 720 ? 1440 - diff : diff;
}

SunTimeResult calculateSunTimes(time_t epoch, double latitude, double longitude,
                                double& sunrise, double& sunset, double& solarNoon,
                                double& dayLength) {
  tm utc;
  gmtime_r(&epoch, &utc);

  const double gamma = fractionalYearRad(dayOfYear(utc), 12.0);
  const double eqTime = equationOfTimeMinutes(gamma);
  const double decl = solarDeclinationRad(gamma);
  const double latRad = latitude * kRad;
  const double zenithRad = kSunriseZenithDeg * kRad;
  solarNoon = 720.0 - (4.0 * longitude) - eqTime;

  const double hourAngleArg =
      (cos(zenithRad) / (cos(latRad) * cos(decl))) - tan(latRad) * tan(decl);
  if (hourAngleArg <= -1.0) {
    dayLength = 1440.0;
    return kPolarDay;
  }
  if (hourAngleArg >= 1.0) {
    dayLength = 0.0;
    return kPolarNight;
  }

  const double hourAngleDeg = acos(hourAngleArg) * kDeg;
  sunrise = solarNoon - (4.0 * hourAngleDeg);
  sunset = solarNoon + (4.0 * hourAngleDeg);
  dayLength = sunset - sunrise;
  return kSunTimesNormal;
}

void calculateSubsolarPoint(time_t epoch, double& latitude, double& longitude) {
  tm utc;
  gmtime_r(&epoch, &utc);

  const double hourUtc = utc.tm_hour + (utc.tm_min / 60.0) + (utc.tm_sec / 3600.0);
  const double gamma = fractionalYearRad(dayOfYear(utc), hourUtc);
  const double eqTime = equationOfTimeMinutes(gamma);
  const double decl = solarDeclinationRad(gamma);
  const double utcMinutes = utc.tm_hour * 60.0 + utc.tm_min + utc.tm_sec / 60.0;

  latitude = decl * kDeg;
  longitude = normalizeLongitude((720.0 - eqTime - utcMinutes) / 4.0);
}

String daylightStatus(int nowMinutes, double sunrise, double sunset) {
  const int sunriseMinute = normalizeMinute(static_cast<int>(sunrise + 0.5));
  const int sunsetMinute = normalizeMinute(static_cast<int>(sunset + 0.5));
  nowMinutes = normalizeMinute(nowMinutes);
  if (circularMinuteDiff(nowMinutes, sunriseMinute) <= kGreylineWindowMinutes ||
      circularMinuteDiff(nowMinutes, sunsetMinute) <= kGreylineWindowMinutes) {
    return "Twilight";
  }

  const bool daylight = sunriseMinute <= sunsetMinute
                            ? nowMinutes > sunriseMinute && nowMinutes < sunsetMinute
                            : nowMinutes > sunriseMinute || nowMinutes < sunsetMinute;
  return daylight ? "Daylight" : "Darkness";
}

String greylineStatus(int nowMinutes, double sunrise, double sunset) {
  const int sunriseMinute = normalizeMinute(static_cast<int>(sunrise + 0.5));
  const int sunsetMinute = normalizeMinute(static_cast<int>(sunset + 0.5));
  nowMinutes = normalizeMinute(nowMinutes);
  if (circularMinuteDiff(nowMinutes, sunriseMinute) <= kGreylineWindowMinutes) {
    return "Morning greyline";
  }
  if (circularMinuteDiff(nowMinutes, sunsetMinute) <= kGreylineWindowMinutes) {
    return "Evening greyline";
  }
  return "Not near greyline";
}

void setInvalidData(const String& qth, const String& status) {
  g_data.valid = false;
  g_data.latitudeValue = 0.0;
  g_data.longitudeValue = 0.0;
  g_data.sunLatitudeValue = 0.0;
  g_data.sunLongitudeValue = 0.0;
  g_data.qth = qth;
  g_data.latitude = "--";
  g_data.longitude = "--";
  g_data.utcTime = "--";
  g_data.sunriseUtc = "--";
  g_data.sunsetUtc = "--";
  g_data.noonUtc = "--";
  g_data.dayLength = "--";
  g_data.sunLatitude = "--";
  g_data.sunLongitude = "--";
  g_data.status = status;
  g_data.greyline = "--";
}
}

String getConfiguredLocator() {
  String locator = getSettings().locator;
  locator.trim();
  locator.toUpperCase();
  if (locator.length() == 0) {
    locator = "FF46"; // Default example; configurable through existing settings.
  }
  return locator;
}

bool getConfiguredLatitude(double& latitude) {
  double longitude;
  return maidenheadToLatLon(getConfiguredLocator(), latitude, longitude);
}

bool getConfiguredLongitude(double& longitude) {
  double latitude;
  return maidenheadToLatLon(getConfiguredLocator(), latitude, longitude);
}

bool maidenheadToLatLon(const String& locatorInput, double& latitude, double& longitude) {
  String locator = locatorInput;
  locator.trim();
  locator.toUpperCase();

  if (locator.length() != 4 && locator.length() != 6) {
    return false;
  }
  if (locator[0] < 'A' || locator[0] > 'R' || locator[1] < 'A' || locator[1] > 'R' ||
      locator[2] < '0' || locator[2] > '9' || locator[3] < '0' || locator[3] > '9') {
    return false;
  }
  if (locator.length() == 6 &&
      (locator[4] < 'A' || locator[4] > 'X' || locator[5] < 'A' || locator[5] > 'X')) {
    return false;
  }

  longitude = -180.0 + ((locator[0] - 'A') * 20.0) + ((locator[2] - '0') * 2.0);
  latitude = -90.0 + ((locator[1] - 'A') * 10.0) + (locator[3] - '0');

  if (locator.length() == 6) {
    longitude += (locator[4] - 'A') * (5.0 / 60.0) + (2.5 / 60.0);
    latitude += (locator[5] - 'A') * (2.5 / 60.0) + (1.25 / 60.0);
  } else {
    longitude += 1.0;
    latitude += 0.5;
  }

  return true;
}

uint32_t greatCircleKm(double lat1, double lon1, double lat2, double lon2) {
  constexpr double kEarthRadiusKm = 6371.0;
  const double phi1 = lat1 * kRad;
  const double phi2 = lat2 * kRad;
  const double deltaPhi = (lat2 - lat1) * kRad;
  const double deltaLambda = (lon2 - lon1) * kRad;

  const double a = sin(deltaPhi / 2.0) * sin(deltaPhi / 2.0) +
                   cos(phi1) * cos(phi2) * sin(deltaLambda / 2.0) * sin(deltaLambda / 2.0);
  return static_cast<uint32_t>((kEarthRadiusKm * 2.0 * atan2(sqrt(a), sqrt(1.0 - a))) + 0.5);
}

void greylineBegin() {
  setInvalidData(getConfiguredLocator(), "Waiting for NTP");
}

void requestGreylineRefresh() {
  g_lastUpdateMs = 0;
}

bool updateGreylineData(time_t epoch, bool timeValid) {
  const uint32_t nowMs = millis();
  const time_t epochMinute = epoch / 60;
  if (g_lastUpdateMs != 0 && epochMinute == g_lastEpochMinute &&
      nowMs - g_lastUpdateMs < kUpdateIntervalMs) {
    return false;
  }

  g_lastUpdateMs = nowMs;
  g_lastEpochMinute = epochMinute;

  const String locator = getConfiguredLocator();
  if (!timeValid) {
    setInvalidData(locator, "Waiting for NTP");
    return true;
  }

  double latitude;
  double longitude;
  if (!maidenheadToLatLon(locator, latitude, longitude)) {
    setInvalidData(locator, "Location invalid");
    Serial.print("Greyline locator invalid: ");
    Serial.println(locator);
    return true;
  }

  double sunrise;
  double sunset;
  double solarNoon;
  double dayLength;
  const SunTimeResult sunTimeResult =
      calculateSunTimes(epoch, latitude, longitude, sunrise, sunset, solarNoon, dayLength);

  double sunLat;
  double sunLon;
  calculateSubsolarPoint(epoch, sunLat, sunLon);

  tm utc;
  gmtime_r(&epoch, &utc);
  const int nowMinutes = utc.tm_hour * 60 + utc.tm_min;

  g_data.valid = true;
  g_data.latitudeValue = latitude;
  g_data.longitudeValue = longitude;
  g_data.sunLatitudeValue = sunLat;
  g_data.sunLongitudeValue = sunLon;
  g_data.qth = locator;
  g_data.latitude = formatDouble(latitude, 1);
  g_data.longitude = formatDouble(longitude, 1);
  g_data.utcTime = formatUtcClock(epoch);
  g_data.sunLatitude = formatDouble(sunLat, 1);
  g_data.sunLongitude = formatDouble(sunLon, 1);

  if (sunTimeResult == kSunTimesNormal) {
    g_data.sunriseUtc = formatUtcMinutes(sunrise);
    g_data.sunsetUtc = formatUtcMinutes(sunset);
    g_data.noonUtc = formatUtcMinutes(solarNoon);
    g_data.dayLength = formatDayLength(dayLength);
    g_data.status = daylightStatus(nowMinutes, sunrise, sunset);
    g_data.greyline = greylineStatus(nowMinutes, sunrise, sunset);
  } else {
    g_data.sunriseUtc = "--";
    g_data.sunsetUtc = "--";
    g_data.noonUtc = formatUtcMinutes(solarNoon);
    g_data.dayLength = formatDayLength(dayLength);
    g_data.status = sunTimeResult == kPolarDay ? "Polar daylight" : "Polar darkness";
    g_data.greyline = "Not near greyline";
  }

  Serial.print("Greyline locator: ");
  Serial.println(locator);
  Serial.print("Greyline lat/lon: ");
  Serial.print(latitude, 4);
  Serial.print(", ");
  Serial.println(longitude, 4);
  Serial.print("Greyline sunrise/sunset/noon: ");
  Serial.print(g_data.sunriseUtc);
  Serial.print(" / ");
  Serial.print(g_data.sunsetUtc);
  Serial.print(" / ");
  Serial.println(g_data.noonUtc);
  Serial.print("Greyline sun lat/lon: ");
  Serial.print(sunLat, 2);
  Serial.print(", ");
  Serial.println(sunLon, 2);

  return true;
}

const GreylineData& getGreylineData() {
  return g_data;
}
