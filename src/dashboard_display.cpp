#include "dashboard_display.h"

#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <stdlib.h>
#include <time.h>

#include "dx_spots.h"
#include "greyline.h"
#include "greyline_map.h"
#include "propagation.h"
#include "settings.h"

namespace {
TFT_eSPI tft(320, 240);
TFT_eSprite mapSprite(&tft);
SPIClass touchSpi(HSPI);

enum DashboardPage : uint8_t {
  kPageClock = 0,
  kPagePropagation,
  kPageVhf,
  kPageGreyline,
  kPageDx,
  kPageCount
};

constexpr uint8_t kLandscapeRotation = 0;
constexpr uint32_t kTouchDebounceMs = 300;
constexpr uint32_t kRenderIntervalMs = 250;
constexpr int16_t kFooterTop = 214;
constexpr int16_t kFooterHeight = 26;

constexpr int8_t kTouchSclk = 25;
constexpr int8_t kTouchMosi = 32;
constexpr int8_t kTouchMiso = 39;
constexpr int8_t kTouchCs = 33;
constexpr int8_t kTouchIrq = 36;
constexpr uint32_t kTouchFrequency = 2500000;
constexpr uint16_t kTouchMin = 120;
constexpr uint16_t kTouchMax = 3975;
constexpr uint8_t kTouchOffsetRotation = 1;

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kPanel = TFT_DARKGREY;
constexpr uint16_t kText = TFT_WHITE;
constexpr uint16_t kMuted = TFT_LIGHTGREY;
constexpr uint16_t kAccent = TFT_YELLOW;
constexpr uint16_t kWarn = TFT_ORANGE;

constexpr int16_t kMapX = 10;
constexpr int16_t kMapY = 4;
constexpr int16_t kMapW = 300;
constexpr int16_t kMapH = 150;
constexpr uint8_t kIli9341Madctl = 0x36;
// Base orientation for this board's known-good wiring (MX only, no row/column
// exchange). MV genuinely swaps which physical axis is "wide", which is what
// CYD units needing a 90-degree turn are missing; MX/MY together mirror both
// axes for a 180-degree flip within the same wide/tall family. All four
// resulting bytes match TFT_eSPI's own ILI9341 rotation table (rotations
// 0/2/5/7), so none of these combinations are unverified guesses.
constexpr uint8_t kIli9341MadctlMx = 0x40;
constexpr uint8_t kIli9341MadctlMy = 0x80;
constexpr uint8_t kIli9341MadctlMv = 0x20;
constexpr uint8_t kIli9341MadctlBgr = 0x08;

DashboardPage g_currentPage = kPageClock;
bool g_pageDirty = true;
bool g_mapSpriteReady = false;
bool g_touchWasDown = false;
uint32_t g_lastTouchActionMs = 0;
uint32_t g_lastRenderMs = 0;

String g_lastFooter;
String g_lastUtc;
String g_lastLocal;
String g_lastDate;
String g_lastLocator;
String g_lastIp;
String g_lastUptime;
String g_lastPropSfiXray;
String g_lastPropAK;
String g_lastPropSunspots;
String g_lastPropGeomag;
String g_lastPropNoise;
String g_lastPropFof2;
String g_lastPropMuf;
String g_lastPropBandA;
String g_lastPropBandB;
String g_lastPropBandC;
String g_lastPropBandD;
String g_lastPropUpdated;
String g_lastPropStatus;
String g_lastVhfAurora;
String g_lastVhfEsEurope;
String g_lastVhfEsNorthAmerica;
String g_lastVhfEsEurope6m;
String g_lastVhfEsEurope4m;
String g_lastVhfUpdated;
String g_lastVhfStatus;
String g_lastGreyUtc;
String g_lastGreyQth;
String g_lastGreyLatLon;
String g_lastGreySunrise;
String g_lastGreySunset;
String g_lastGreyNoon;
String g_lastGreyDayLength;
String g_lastGreySunLat;
String g_lastGreySunLon;
String g_lastGreyStatus;
String g_lastGreyline;
String g_lastGreyMap;
String g_lastDxRows[kMaxDxSpots];
String g_lastDxEmpty;
String g_lastDxUpdated;
String g_lastDxSource;
String g_lastDxStatus;

char utcBuffer[16];
char localBuffer[24];
char dateBuffer[24];
char uptimeBuffer[16];


int16_t centerX() {
  return tft.width() / 2;
}

String wifiStatusText(bool connected) {
  return connected ? "WiFi OK" : "WiFi --";
}

String ntpStatusText(bool valid) {
  return valid ? "NTP OK" : "NTP --";
}

String pageIndicator() {
  return String("Page ") + String(static_cast<uint8_t>(g_currentPage) + 1) +
         "/" + String(kPageCount);
}

String footerText(const ClockSnapshot& snapshot) {
  return wifiStatusText(snapshot.wifiConnected) + "   " +
         ntpStatusText(snapshot.timeValid) + "   " +
         pageIndicator();
}

void clearPageState() {
  g_lastFooter = "";
  g_lastUtc = "";
  g_lastLocal = "";
  g_lastDate = "";
  g_lastLocator = "";
  g_lastIp = "";
  g_lastUptime = "";
  g_lastPropSfiXray = "";
  g_lastPropAK = "";
  g_lastPropSunspots = "";
  g_lastPropGeomag = "";
  g_lastPropNoise = "";
  g_lastPropFof2 = "";
  g_lastPropMuf = "";
  g_lastPropBandA = "";
  g_lastPropBandB = "";
  g_lastPropBandC = "";
  g_lastPropBandD = "";
  g_lastPropUpdated = "";
  g_lastPropStatus = "";
  g_lastVhfAurora = "";
  g_lastVhfEsEurope = "";
  g_lastVhfEsNorthAmerica = "";
  g_lastVhfEsEurope6m = "";
  g_lastVhfEsEurope4m = "";
  g_lastVhfUpdated = "";
  g_lastVhfStatus = "";
  g_lastGreyUtc = "";
  g_lastGreyQth = "";
  g_lastGreyLatLon = "";
  g_lastGreySunrise = "";
  g_lastGreySunset = "";
  g_lastGreyNoon = "";
  g_lastGreyDayLength = "";
  g_lastGreySunLat = "";
  g_lastGreySunLon = "";
  g_lastGreyStatus = "";
  g_lastGreyline = "";
  g_lastGreyMap = "";
  for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
    g_lastDxRows[i] = "";
  }
  g_lastDxEmpty = "";
  g_lastDxUpdated = "";
  g_lastDxSource = "";
  g_lastDxStatus = "";
}

void drawCentered(const String& text, int16_t y, uint8_t font, uint16_t color = kText) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(text, centerX(), y, font);
}

void drawCenteredAt(const String& text, int16_t x, int16_t y, uint8_t font,
                    uint16_t color = kText) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(text, x, y, font);
}

void drawLeft(const String& text, int16_t x, int16_t y, uint8_t font, uint16_t color = kText) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(text, x, y, font);
}

void drawCenteredField(String& last, const String& value, int16_t y, uint8_t font,
                       uint16_t color = kText, int16_t x = -1, int16_t w = -1) {
  if (value == last) {
    return;
  }

  if (x < 0) {
    x = 0;
  }
  if (w < 0) {
    w = tft.width();
  }

  const int16_t h = tft.fontHeight(font) + 4;
  if (tft.textWidth(value, font) != tft.textWidth(last, font)) {
    tft.fillRect(x, y - 2, w, h, kBg);
  }
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(value, x + (w / 2), y, font);
  last = value;
}

void drawLeftField(String& last, const String& value, int16_t x, int16_t y,
                   uint8_t font, uint16_t color = kText, int16_t w = -1) {
  if (value == last) {
    return;
  }

  if (w < 0) {
    w = tft.width() - x;
  }

  const int16_t h = tft.fontHeight(font) + 4;
  tft.fillRect(x, y - 2, w, h, kBg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(value, x, y, font);
  last = value;
}

uint16_t conditionColor(const String& condition) {
  if (condition.equalsIgnoreCase("Good")) {
    return TFT_GREEN;
  }
  if (condition.equalsIgnoreCase("Fair")) {
    return TFT_YELLOW;
  }
  if (condition.equalsIgnoreCase("Poor")) {
    return TFT_RED;
  }
  return kMuted;
}

bool numericReading(const String& value, float& result) {
  const char* text = value.c_str();
  char* end = nullptr;
  result = strtof(text, &end);
  return end != text;
}

uint16_t readingColor(const String& reading, const char* parameter) {
  float value = 0.0f;
  if (String(parameter) == "X-Ray") {
    String level = reading;
    level.trim();
    level.toUpperCase();
    if (level.startsWith("A") || level.startsWith("B") || level.startsWith("C")) return TFT_GREEN;
    if (level.startsWith("M")) return TFT_YELLOW;
    if (level.startsWith("X")) return TFT_RED;
    return kMuted;
  }

  if (!numericReading(reading, value)) return kMuted;

  if (String(parameter) == "SFI") {
    return value >= 120.0f ? TFT_GREEN : value >= 70.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "SN") {
    return value >= 70.0f ? TFT_GREEN : value >= 10.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "K") {
    return value <= 5.0f ? TFT_GREEN : value <= 6.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "A") {
    return value <= 49.0f ? TFT_GREEN : value <= 99.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "SW") {
    return value < 500.0f ? TFT_GREEN : value < 600.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "Bz") {
    return value >= -10.0f ? TFT_GREEN : value >= -20.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "Aurora") {
    return value <= 8.0f ? TFT_GREEN : value <= 9.0f ? TFT_YELLOW : TFT_RED;
  }
  return kMuted;
}

uint16_t qualitativeReadingColor(const String& reading, const char* parameter) {
  String level = reading;
  level.trim();
  level.toUpperCase();

  if (String(parameter) == "Geomag") {
    // HamQSL reports: Inactive, Very Quiet, Quiet, Unsettled, Active, Minor Storm,
    // Major Storm, Severe Storm, Extreme Storm - in increasing order of K-index severity.
    if (level.length() == 0 || level == "--") return kMuted;
    if (level.indexOf("SEVERE") >= 0 || level.indexOf("EXTREME") >= 0) return TFT_RED;
    if (level.indexOf("MAJOR") >= 0) return TFT_YELLOW;
    if (level.indexOf("QUIET") >= 0 || level.indexOf("UNSETTLED") >= 0 ||
        level.indexOf("ACTIVE") >= 0 || level.indexOf("STORM") >= 0 ||
        level == "INACTIVE" || level == "NORMAL") return TFT_GREEN;
    return kMuted;
  }
  if (String(parameter) == "Noise") {
    // HamQSL reports noise as an S-meter level or range, e.g. "S0-S1", "S3", "S5-S7".
    // Use the highest S number present to gauge severity.
    int maxS = -1;
    for (size_t i = 0; i < level.length(); ++i) {
      if (level[i] == 'S' && i + 1 < level.length() && isDigit(level[i + 1])) {
        size_t j = i + 1;
        int num = 0;
        while (j < level.length() && isDigit(level[j])) {
          num = num * 10 + (level[j] - '0');
          j++;
        }
        if (num > maxS) maxS = num;
      }
    }
    if (maxS >= 0) {
      return maxS <= 6 ? TFT_GREEN : maxS <= 9 ? TFT_YELLOW : TFT_RED;
    }
    if (level.indexOf("LOW") >= 0 || level == "NORMAL") return TFT_GREEN;
    if (level.indexOf("MODERATE") >= 0 || level.indexOf("MEDIUM") >= 0) return TFT_YELLOW;
    if (level.indexOf("HIGH") >= 0) return TFT_RED;
  }
  if (String(parameter) == "Aurora") {
    return readingColor(reading, "Aurora");
  }
  return kMuted;
}

void drawReading(int16_t& x, int16_t y, const String& label, const String& value,
                 uint16_t color) {
  drawLeft(label, x, y, 2, kText);
  x += tft.textWidth(label, 2);
  drawLeft(value, x, y, 2, color);
  x += tft.textWidth(value, 2);
}

void drawTopReadingRows(String& lastSfiXray, String& lastSunspots, String& lastNoise,
                        const PropagationData& propagation) {
  const String sfiXray = "SFI " + propagation.sfi + "   A " + propagation.aIndex +
                         "   K " + propagation.kIndex + "   X-Ray " + propagation.xray;
  if (sfiXray != lastSfiXray) {
    tft.fillRect(8, 34, 304, tft.fontHeight(2) + 4, kBg);
    int16_t x = 8;
    drawReading(x, 36, "SFI ", propagation.sfi, readingColor(propagation.sfi, "SFI"));
    drawReading(x, 36, "   A ", propagation.aIndex, readingColor(propagation.aIndex, "A"));
    drawReading(x, 36, "   K ", propagation.kIndex, readingColor(propagation.kIndex, "K"));
    drawReading(x, 36, "   X-Ray ", propagation.xray, readingColor(propagation.xray, "X-Ray"));
    lastSfiXray = sfiXray;
  }

  const String sunspots = "Sunspots " + propagation.sunspots + "   Geomag " + propagation.geomag;
  if (sunspots != lastSunspots) {
    tft.fillRect(8, 52, 304, tft.fontHeight(2) + 4, kBg);
    int16_t x = 8;
    drawReading(x, 54, "Sunspots ", propagation.sunspots,
                readingColor(propagation.sunspots, "SN"));
    drawReading(x, 54, "   Geomag ", propagation.geomag,
                qualitativeReadingColor(propagation.geomag, "Geomag"));
    lastSunspots = sunspots;
  }

  const String noise = "Noise " + propagation.signalNoise + "   Aurora " + propagation.aurora +
                       "   SW " + propagation.solarWind + "   Bz " + propagation.bz;
  if (noise != lastNoise) {
    tft.fillRect(8, 70, 304, tft.fontHeight(2) + 4, kBg);
    int16_t x = 8;
    drawReading(x, 72, "Noise ", propagation.signalNoise,
                qualitativeReadingColor(propagation.signalNoise, "Noise"));
    drawReading(x, 72, "   Aurora ", propagation.aurora,
                qualitativeReadingColor(propagation.aurora, "Aurora"));
    drawReading(x, 72, "   SW ", propagation.solarWind,
                readingColor(propagation.solarWind, "SW"));
    drawReading(x, 72, "   Bz ", propagation.bz, readingColor(propagation.bz, "Bz"));
    lastNoise = noise;
  }
}

void drawConditionValue(const String& value, int16_t center, int16_t y) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(conditionColor(value), kBg);
  tft.drawString(value, center, y, 2);
}

void drawConditionRow(String& last, const String& label, const String& day,
                      const String& night, int16_t y) {
  const String value = label + "|" + day + "|" + night;
  if (value == last) {
    return;
  }

  const int16_t rowTop = y - 2;
  const int16_t rowHeight = tft.fontHeight(2) + 4;
  tft.fillRect(5, rowTop, tft.width() - 10, rowHeight, kBg);
  tft.drawFastVLine(112, rowTop, rowHeight, kPanel);
  tft.drawFastVLine(216, rowTop, rowHeight, kPanel);
  drawLeft(label, 8, y, 2, kText);
  drawConditionValue(day, 164, y);
  drawConditionValue(night, 266, y);
  last = value;
}

uint16_t vhfConditionColor(const String& value) {
  String level = value;
  level.trim();
  level.toUpperCase();
  if (level.indexOf("CLOSED") >= 0 || level.indexOf("POOR") >= 0) {
    return TFT_RED;
  }
  if (level.indexOf("OPEN") >= 0 || level.indexOf("HIGH") >= 0 || level.indexOf("GOOD") >= 0) {
    return TFT_GREEN;
  }
  if (level.indexOf("MODERATE") >= 0 || level.indexOf("FAIR") >= 0) {
    return TFT_YELLOW;
  }
  return kMuted;
}

void drawVhfConditionRow(String& last, const String& label, const String& value, int16_t y) {
  const String combined = label + "|" + value;
  if (combined == last) {
    return;
  }

  const int16_t rowTop = y - 2;
  const int16_t rowHeight = tft.fontHeight(2) + 4;
  tft.fillRect(5, rowTop, tft.width() - 10, rowHeight, kBg);
  drawLeft(label, 8, y, 2, kText);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(vhfConditionColor(value), kBg);
  tft.drawString(value, tft.width() - 12, y, 2);
  last = combined;
}

uint16_t auroraLatColor(const String& value) {
  float lat = 0.0f;
  if (!numericReading(value, lat)) {
    return kMuted;
  }
  // Unlike HF, VHF operators chase aurora backscatter, so a lower latitude
  // (aurora expanded further south, more active/workable) is favourable and
  // the ~67.5 baseline (aurora confined near the pole, effectively closed) is not.
  if (lat >= 65.0f) return TFT_RED;
  if (lat >= 55.0f) return TFT_YELLOW;
  return TFT_GREEN;
}

void drawVhfAuroraRow(String& last, const String& status, const String& lat, int16_t y) {
  const String combined = status + "|" + lat;
  if (combined == last) {
    return;
  }

  const int16_t rowTop = y - 2;
  const int16_t rowHeight = tft.fontHeight(2) + 4;
  tft.fillRect(5, rowTop, tft.width() - 10, rowHeight, kBg);

  int16_t x = 8;
  drawLeft("VHF Aurora", x, y, 2, kText);
  x += tft.textWidth("VHF Aurora", 2);
  if (lat.length() > 0 && lat != "--") {
    const String latText = " (Lat " + lat + ")";
    drawLeft(latText, x, y, 2, auroraLatColor(lat));
  }

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(vhfConditionColor(status), kBg);
  tft.drawString(status, tft.width() - 12, y, 2);
  last = combined;
}

bool ensureMapSprite() {
  if (g_mapSpriteReady) {
    return true;
  }

  mapSprite.setColorDepth(16);
  g_mapSpriteReady = mapSprite.createSprite(kMapW, kMapH) != nullptr;
  if (!g_mapSpriteReady) {
    Serial.println("Greyline map sprite allocation failed");
  }
  return g_mapSpriteReady;
}

void latLonToMapXY(double latitude, double longitude, int16_t& x, int16_t& y) {
  longitude = constrain(longitude, -180.0, 180.0);
  latitude = constrain(latitude, -90.0, 90.0);
  x = static_cast<int16_t>(((longitude + 180.0) * (kMapW - 1)) / 360.0);
  y = static_cast<int16_t>(((90.0 - latitude) * (kMapH - 1)) / 180.0);
}

void drawMapBackground() {
  mapSprite.setSwapBytes(true);
  mapSprite.pushImage(0, 0, kGreylineMapWidth, kGreylineMapHeight,
                      const_cast<uint16_t*>(kGreylineMapRgb565));
}

uint16_t darkenRgb565(uint16_t color, uint8_t percent) {
  percent = constrain(percent, static_cast<uint8_t>(0), static_cast<uint8_t>(100));
  const uint8_t keep = 100 - percent;
  uint8_t r = ((color >> 11) & 0x1F) << 3;
  uint8_t g = ((color >> 5) & 0x3F) << 2;
  uint8_t b = (color & 0x1F) << 3;

  r = (static_cast<uint16_t>(r) * keep) / 100;
  g = (static_cast<uint16_t>(g) * keep) / 100;
  b = (static_cast<uint16_t>(b) * keep) / 100;
  return mapSprite.color565(r, g, b);
}

void drawNightShading(double subsolarLatitude, double subsolarLongitude) {
  const double sunLatRad = subsolarLatitude * DEG_TO_RAD;
  const double sinSunLat = sin(sunLatRad);
  const double cosSunLat = cos(sunLatRad);

  for (int16_t y = 1; y < kMapH - 1; ++y) {
    const double latitude = 90.0 - ((static_cast<double>(y) * 180.0) / (kMapH - 1));
    const double latRad = latitude * DEG_TO_RAD;
    const double sinLat = sin(latRad);
    const double cosLat = cos(latRad);

    for (int16_t x = 1; x < kMapW - 1; ++x) {
      const double longitude = ((static_cast<double>(x) * 360.0) / (kMapW - 1)) - 180.0;
      const double hourAngle = (longitude - subsolarLongitude) * DEG_TO_RAD;
      const double sunAltitude = (sinLat * sinSunLat) + (cosLat * cosSunLat * cos(hourAngle));
      if (sunAltitude < 0.0) {
        const uint8_t shadePercent = sunAltitude > -0.08 ? 28 : 48;
        mapSprite.drawPixel(x, y, darkenRgb565(mapSprite.readPixel(x, y), shadePercent));
      }
    }
  }
}

void drawTerminator(double subsolarLatitude, double subsolarLongitude) {
  const uint16_t terminator = mapSprite.color565(185, 198, 204);
  const double subsolarLatRad = subsolarLatitude * DEG_TO_RAD;
  if (fabs(tan(subsolarLatRad)) < 0.02) {
    for (int8_t direction = -1; direction <= 1; direction += 2) {
      double longitude = subsolarLongitude + (direction * 90.0);
      if (longitude > 180.0) longitude -= 360.0;
      if (longitude < -180.0) longitude += 360.0;
      int16_t x1, y1;
      int16_t x2, y2;
      latLonToMapXY(90.0, longitude, x1, y1);
      latLonToMapXY(-90.0, longitude, x2, y2);
      mapSprite.drawLine(x1, y1, x2, y2, terminator);
    }
    return;
  }

  bool havePrevious = false;
  int16_t previousX = 0;
  int16_t previousY = 0;
  for (int lon = -180; lon <= 180; lon += 4) {
    const double deltaLonRad = (lon - subsolarLongitude) * DEG_TO_RAD;
    const double latRad = atan(-cos(deltaLonRad) / tan(subsolarLatRad));
    const double latitude = latRad * RAD_TO_DEG;

    int16_t x;
    int16_t y;
    latLonToMapXY(latitude, lon, x, y);
    if (havePrevious) {
      mapSprite.drawLine(previousX, previousY, x, y, terminator);
    }
    previousX = x;
    previousY = y;
    havePrevious = true;
  }
}

void drawQthMarker(double latitude, double longitude) {
  int16_t x, y;
  latLonToMapXY(latitude, longitude, x, y);
  mapSprite.drawCircle(x, y, 3, kAccent);
  mapSprite.drawFastHLine(max<int16_t>(0, x - 5), y, min<int16_t>(11, kMapW - max<int16_t>(0, x - 5)), kAccent);
  mapSprite.drawFastVLine(x, max<int16_t>(0, y - 5), min<int16_t>(11, kMapH - max<int16_t>(0, y - 5)), kAccent);
}

void drawSunMarker(double latitude, double longitude) {
  int16_t x, y;
  latLonToMapXY(latitude, longitude, x, y);
  mapSprite.fillCircle(x, y, 3, TFT_YELLOW);
  mapSprite.drawCircle(x, y, 5, TFT_YELLOW);
  mapSprite.setTextDatum(TL_DATUM);
  mapSprite.setTextColor(TFT_YELLOW, mapSprite.color565(2, 12, 22));
  mapSprite.drawString("S", min<int16_t>(x + 6, kMapW - 9), max<int16_t>(0, y - 6), 1);
}

void drawGreylineMap(const GreylineData& greyline) {
  if (!ensureMapSprite()) {
    tft.fillRect(kMapX, kMapY, kMapW, kMapH, kBg);
    tft.drawRect(kMapX, kMapY, kMapW, kMapH, kPanel);
    return;
  }

  drawMapBackground();
  if (greyline.valid) {
    drawNightShading(greyline.sunLatitudeValue, greyline.sunLongitudeValue);
    drawTerminator(greyline.sunLatitudeValue, greyline.sunLongitudeValue);
    drawQthMarker(greyline.latitudeValue, greyline.longitudeValue);
    drawSunMarker(greyline.sunLatitudeValue, greyline.sunLongitudeValue);
  }
  mapSprite.pushSprite(kMapX, kMapY);
}

String truncateText(const String& value, uint8_t maxLen) {
  if (value.length() <= maxLen) {
    return value;
  }
  return value.substring(0, maxLen);
}

void drawDxSpotRow(String& last, const DxSpot& spot, int16_t y) {
  const String rowKey = spot.freq + "|" + spot.call + "|" + spot.mode + "|" + spot.time;
  if (rowKey == last) {
    return;
  }

  tft.fillRect(10, y - 2, tft.width() - 20, tft.fontHeight(2) + 4, kBg);
  drawLeft(truncateText(spot.freq, 7), 14, y, 2, kText);
  drawLeft(truncateText(spot.call, 9), 82, y, 2, kAccent);
  drawLeft(truncateText(spot.mode, 5), 170, y, 2, kText);
  drawLeft(truncateText(spot.time, 5), 230, y, 2, kMuted);
  last = rowKey;
}

void drawFooter(const ClockSnapshot& snapshot) {
  const String value = footerText(snapshot);
  if (value == g_lastFooter) {
    return;
  }

  tft.fillRect(0, kFooterTop, tft.width(), kFooterHeight, kBg);
  tft.drawFastHLine(0, kFooterTop, tft.width(), kPanel);
  drawCentered(value, kFooterTop + 7, 2, kMuted);
  g_lastFooter = value;
}

void formatTimes(const ClockSnapshot& snapshot) {
  if (!snapshot.timeValid) {
    strlcpy(utcBuffer, "--:--:--", sizeof(utcBuffer));
    strlcpy(localBuffer, "--:--:--", sizeof(localBuffer));
    strlcpy(dateBuffer, "Waiting for NTP", sizeof(dateBuffer));
    return;
  }

  tm utcTime;
  tm localTime;
  gmtime_r(&snapshot.epoch, &utcTime);
  localtime_r(&snapshot.epoch, &localTime);

  strftime(utcBuffer, sizeof(utcBuffer), "%H:%M:%S", &utcTime);
  strftime(localBuffer, sizeof(localBuffer), "%H:%M:%S", &localTime);
  strftime(dateBuffer, sizeof(dateBuffer), "%d %b %Y", &localTime);
}

String formatUptime(uint32_t seconds) {
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  const uint32_t secs = seconds % 60;
  snprintf(uptimeBuffer, sizeof(uptimeBuffer), "%02lu:%02lu:%02lu",
           static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(secs));
  return String("Uptime: ") + uptimeBuffer;
}

void drawClockPage(const ClockSnapshot& snapshot) {
  const AppSettings& settings = getSettings();
  formatTimes(snapshot);

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("UTC", 3, 2, kMuted);
  }

  drawCenteredField(g_lastUtc, utcBuffer, 20, 7, kAccent);
  drawCenteredField(g_lastLocal, settings.timezoneLabel + " " + localBuffer, 86, 4);
  drawCenteredField(g_lastDate, dateBuffer, 120, 4);
  const String stationText = settings.callsign.length() > 0
                                 ? settings.callsign + "   Locator: " + settings.locator
                                 : String("Locator: ") + settings.locator;
  drawCenteredField(g_lastLocator, stationText, 152, 4);
  const String ipText = snapshot.wifiConnected ? String("IP: ") + WiFi.localIP().toString()
                                               : String("IP: --");
  drawCenteredField(g_lastIp, ipText, 176, 2, kMuted);
  drawCenteredField(g_lastUptime, formatUptime(snapshot.uptimeSeconds), 194, 2, kMuted);
  drawFooter(snapshot);
}

void drawPropagationPage(const ClockSnapshot& snapshot) {
  const PropagationData& propagation = getPropagationData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("HF Propagation", 4, 4, kAccent);
    tft.drawRect(4, 30, tft.width() - 8, 58, kPanel);
    tft.drawRect(4, 88, tft.width() - 8, 104, kPanel);
    tft.drawFastVLine(112, 88, 104, kPanel);
    tft.drawFastVLine(216, 88, 104, kPanel);
    drawLeft("Band", 8, 92, 2, kMuted);
    drawCenteredAt("Day", 164, 92, 2, kMuted);
    drawCenteredAt("Night", 266, 92, 2, kMuted);
  }

  drawTopReadingRows(g_lastPropSfiXray, g_lastPropSunspots, g_lastPropNoise, propagation);
  tft.drawFastHLine(4, 88, tft.width() - 8, kPanel);

  drawConditionRow(g_lastPropBandA, "80m-40m", propagation.band8040Day, propagation.band8040Night, 112);
  drawConditionRow(g_lastPropBandB, "30m-20m", propagation.band3020Day, propagation.band3020Night, 132);
  drawConditionRow(g_lastPropBandC, "17m-15m", propagation.band1715Day, propagation.band1715Night, 152);
  drawConditionRow(g_lastPropBandD, "12m-10m", propagation.band1210Day, propagation.band1210Night, 172);

  drawLeftField(g_lastPropUpdated, "Updated: " + propagation.updatedUtc, 8, 194, 2, kMuted, 150);
  drawLeftField(g_lastPropStatus, "Status: " + propagation.status, 164, 194, 2,
                propagation.status == "OK" ? kAccent : kWarn, 152);
  drawFooter(snapshot);
}

void drawVhfPage(const ClockSnapshot& snapshot) {
  const PropagationData& propagation = getPropagationData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("VHF Conditions", 4, 4, kAccent);
    tft.drawRect(4, 30, tft.width() - 8, 58, kPanel);
    tft.drawRect(4, 88, tft.width() - 8, 104, kPanel);
  }

  drawTopReadingRows(g_lastPropSfiXray, g_lastPropSunspots, g_lastPropNoise, propagation);
  tft.drawFastHLine(4, 88, tft.width() - 8, kPanel);

  drawVhfAuroraRow(g_lastVhfAurora, propagation.vhfAurora, propagation.vhfAuroraLat, 92);
  drawVhfConditionRow(g_lastVhfEsEurope6m, "Es EU 6m", propagation.vhfEsEurope6m, 112);
  drawVhfConditionRow(g_lastVhfEsEurope4m, "Es EU 4m", propagation.vhfEsEurope4m, 132);
  drawVhfConditionRow(g_lastVhfEsEurope, "Es EU 2m", propagation.vhfEsEurope, 152);
  drawVhfConditionRow(g_lastVhfEsNorthAmerica, "Es NA 2m", propagation.vhfEsNorthAmerica, 172);

  drawLeftField(g_lastVhfUpdated, "Updated: " + propagation.updatedUtc, 8, 194, 2, kMuted, 150);
  drawLeftField(g_lastVhfStatus, "Status: " + propagation.status, 164, 194, 2,
                propagation.status == "OK" ? kAccent : kWarn, 152);
  drawFooter(snapshot);
}

void drawGreylinePage(const ClockSnapshot& snapshot) {
  const GreylineData& greyline = getGreylineData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
  }

  const String mapSignature = greyline.qth + "|" + greyline.latitude + "|" + greyline.longitude +
                              "|" + greyline.sunLatitude + "|" + greyline.sunLongitude +
                              "|" + String(greyline.valid ? "1" : "0");
  if (mapSignature != g_lastGreyMap) {
    drawGreylineMap(greyline);
    g_lastGreyMap = mapSignature;
  }
  drawLeftField(g_lastGreyQth, "QTH: " + greyline.qth, 14, 160, 1, kText, 88);
  drawLeftField(g_lastGreySunLat, "Sun: " + greyline.sunLatitude + "," + greyline.sunLongitude, 108, 160, 1, kText, 126);
  drawLeftField(g_lastGreySunrise, "Rise: " + greyline.sunriseUtc.substring(0, 5), 14, 178, 1, kText, 76);
  drawLeftField(g_lastGreySunset, "Set: " + greyline.sunsetUtc.substring(0, 5), 96, 178, 1, kText, 76);
  drawLeftField(g_lastGreyUtc, "UTC: " + greyline.utcTime.substring(0, 5), 178, 178, 1, kMuted, 82);
  drawLeftField(g_lastGreyStatus, "Status: " + greyline.status, 14, 196, 1,
                greyline.status == "Location invalid" ? kWarn : kText, 134);
  String greylineLabel = greyline.greyline;
  greylineLabel.replace(" greyline", "");
  drawLeftField(g_lastGreyline, "Greyline: " + greylineLabel, 154, 196, 1,
                greyline.greyline == "Not near greyline" ? kMuted : kAccent);
  drawFooter(snapshot);
}

void drawDxPage(const ClockSnapshot& snapshot) {
  const DxSpotsData& dx = getDxSpotsData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("DX Spots", 4, 4, kAccent);
    drawLeft("Freq", 14, 24, 2, kMuted);
    drawLeft("Call", 82, 24, 2, kMuted);
    drawLeft("Mode", 170, 24, 2, kMuted);
    drawLeft("UTC", 230, 24, 2, kMuted);
  }

  if (dx.spotCount == 0) {
    drawCenteredField(g_lastDxEmpty, "No spots loaded", 92, 4, kMuted);
    for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
      g_lastDxRows[i] = "";
    }
  } else {
    if (g_lastDxEmpty.length() > 0) {
      tft.fillRect(0, 78, tft.width(), 40, kBg);
      g_lastDxEmpty = "";
    }
    for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
      const int16_t y = 44 + (i * 17);
      if (i < dx.spotCount) {
        drawDxSpotRow(g_lastDxRows[i], dx.spots[i], y);
      } else if (g_lastDxRows[i].length() > 0) {
        tft.fillRect(10, y - 2, tft.width() - 20, tft.fontHeight(2) + 4, kBg);
        g_lastDxRows[i] = "";
      }
    }
  }

  drawLeftField(g_lastDxUpdated, "Updated: " + dx.updated, 8, 186, 2, kMuted, 144);
  drawLeftField(g_lastDxSource, "Source: " + dx.provider, 158, 186, 2,
                dx.source == "Last good" ? kWarn : kAccent, 158);
  drawLeftField(g_lastDxStatus, "Status: " + dx.status, 8, 204, 1,
                dx.status == "OK" || dx.status == "Connected" || dx.status == "Reading"
                    ? kAccent
                    : kWarn,
                308);
  drawFooter(snapshot);
}

void drawCurrentPage(const ClockSnapshot& snapshot) {
  switch (g_currentPage) {
    case kPageClock:
      drawClockPage(snapshot);
      break;
    case kPagePropagation:
      drawPropagationPage(snapshot);
      break;
    case kPageVhf:
      drawVhfPage(snapshot);
      break;
    case kPageGreyline:
      drawGreylinePage(snapshot);
      break;
    case kPageDx:
      drawDxPage(snapshot);
      break;
    default:
      g_currentPage = kPageClock;
      drawClockPage(snapshot);
      break;
  }

  g_pageDirty = false;
}

void nextPage() {
  g_currentPage = static_cast<DashboardPage>((static_cast<uint8_t>(g_currentPage) + 1) % kPageCount);
  clearPageState();
  g_pageDirty = true;
}

void previousPage() {
  const uint8_t page = static_cast<uint8_t>(g_currentPage);
  g_currentPage = static_cast<DashboardPage>(page == 0 ? kPageCount - 1 : page - 1);
  clearPageState();
  g_pageDirty = true;
}

uint16_t readTouchAxis(uint8_t command) {
  touchSpi.transfer(command);
  const uint16_t high = touchSpi.transfer(0x00);
  const uint16_t low = touchSpi.transfer(0x00);
  return ((high << 8) | low) >> 3;
}

bool readRawTouch(uint16_t& rawX, uint16_t& rawY) {
  if (digitalRead(kTouchIrq) == HIGH) {
    return false;
  }

  touchSpi.beginTransaction(SPISettings(kTouchFrequency, MSBFIRST, SPI_MODE0));
  digitalWrite(kTouchCs, LOW);
  delayMicroseconds(2);

  uint32_t xTotal = 0;
  uint32_t yTotal = 0;
  constexpr uint8_t kSamples = 4;
  for (uint8_t i = 0; i < kSamples; ++i) {
    xTotal += readTouchAxis(0xD0);
    yTotal += readTouchAxis(0x90);
  }

  digitalWrite(kTouchCs, HIGH);
  touchSpi.endTransaction();

  rawX = xTotal / kSamples;
  rawY = yTotal / kSamples;
  return rawX >= kTouchMin && rawX <= kTouchMax &&
         rawY >= kTouchMin && rawY <= kTouchMax;
}

int16_t scaleTouch(uint16_t value, int16_t size) {
  value = constrain(value, kTouchMin, kTouchMax);
  return static_cast<int16_t>(
      (static_cast<uint32_t>(value - kTouchMin) * (size - 1)) /
      (kTouchMax - kTouchMin));
}

bool getTouchPoint(uint16_t& x, uint16_t& y) {
  uint16_t rawX;
  uint16_t rawY;
  if (!readRawTouch(rawX, rawY)) {
    return false;
  }

  int16_t baseX = scaleTouch(rawX, tft.width());
  int16_t baseY = scaleTouch(rawY, tft.height());

  if (kTouchOffsetRotation == 1) {
    x = constrain(baseY * tft.width() / tft.height(), 0, tft.width() - 1);
    y = constrain(tft.height() - 1 - (baseX * tft.height() / tft.width()), 0, tft.height() - 1);
  } else {
    x = baseX;
    y = baseY;
  }

  // The touch controller is wired independently of the display, so flipping
  // the screen via MADCTL does not change what a physical tap reports here.
  // Mirror the point to match what is now visually on screen.
  if (getSettings().flip180) {
    x = tft.width() - 1 - x;
    y = tft.height() - 1 - y;
  }

  return true;
}

void handleTouch() {
  uint16_t x;
  uint16_t y;
  const bool touched = getTouchPoint(x, y);
  const uint32_t nowMs = millis();

  if (touched && !g_touchWasDown && nowMs - g_lastTouchActionMs >= kTouchDebounceMs) {
    g_lastTouchActionMs = nowMs;
    if ((g_currentPage == kPagePropagation || g_currentPage == kPageVhf) &&
        x >= tft.width() / 3 && x <= (tft.width() * 2) / 3) {
      requestPropagationRefresh();
      if (g_currentPage == kPagePropagation) {
        g_lastPropStatus = "";
        drawLeftField(g_lastPropStatus, "Status: Refreshing", 166, 194, 2, kMuted);
      } else {
        g_lastVhfStatus = "";
        drawLeftField(g_lastVhfStatus, "Status: Refreshing", 166, 194, 2, kMuted);
      }
    } else if (g_currentPage == kPageDx &&
               x >= tft.width() / 3 && x <= (tft.width() * 2) / 3) {
      requestDxSpotsRefresh();
      g_lastDxStatus = "";
      drawLeftField(g_lastDxStatus, "Status: Refreshing", 8, 204, 1, kMuted, 308);
    } else if (x < tft.width() / 2) {
      nextPage();
    } else {
      previousPage();
    }
  }

  g_touchWasDown = touched;
}
}

void displayBegin() {
  tft.init();
  tft.setRotation(kLandscapeRotation);
  dxSpotsBegin();
  greylineBegin();
  propagationBegin();

  pinMode(kTouchCs, OUTPUT);
  digitalWrite(kTouchCs, HIGH);
  pinMode(kTouchIrq, INPUT);
  touchSpi.begin(kTouchSclk, kTouchMiso, kTouchMosi, kTouchCs);

  applyDisplaySettings();

  tft.fillScreen(kBg);
  clearPageState();
  g_pageDirty = true;
}

void displayUpdate(const ClockSnapshot& snapshot) {
  handleTouch();
  const bool dataChanged = refreshDxSpotsIfNeeded(snapshot.wifiConnected) |
                           refreshPropagationIfNeeded(snapshot.wifiConnected) |
                           updateGreylineData(snapshot.epoch, snapshot.timeValid);
  const uint32_t nowMs = millis();
  if (g_pageDirty || dataChanged || nowMs - g_lastRenderMs >= kRenderIntervalMs) {
    drawCurrentPage(snapshot);
    g_lastRenderMs = nowMs;
  }
}

void applyDisplaySettings() {
  const AppSettings& settings = getSettings();

  // TFT_eSPI's RGB/BGR order and orientation are normally fixed at compile
  // time. Write the ILI9341 MADCTL byte directly here so differently wired
  // CYD panels (wrong colour order, upside down, or needing a 90-degree
  // turn) can be corrected from the web settings page without rebuilding
  // firmware.
  uint8_t madctl = kIli9341MadctlMx;
  if (settings.flip180) {
    madctl ^= (kIli9341MadctlMx | kIli9341MadctlMy);
  }
  if (settings.rotate90) {
    madctl |= kIli9341MadctlMv;
  }
  madctl |= (settings.swapRedBlueChannels ? kIli9341MadctlBgr : 0);

  tft.startWrite();
  tft.writecommand(kIli9341Madctl);
  tft.writedata(madctl);
  tft.endWrite();

#ifdef TFT_BL
  constexpr uint8_t kBacklightChannel = 0;
  constexpr uint32_t kBacklightFrequency = 5000;
  constexpr uint8_t kBacklightResolution = 8;

  const uint8_t brightness = constrain(settings.brightnessPercent, static_cast<uint8_t>(5),
                                       static_cast<uint8_t>(100));
  uint8_t duty = map(brightness, 0, 100, 0, 255);
#if TFT_BACKLIGHT_ON == LOW
  duty = 255 - duty;
#endif
  ledcSetup(kBacklightChannel, kBacklightFrequency, kBacklightResolution);
  ledcAttachPin(TFT_BL, kBacklightChannel);
  ledcWrite(kBacklightChannel, duty);
#endif

  clearPageState();
  g_pageDirty = true;
}

uint8_t getCurrentDashboardPageNumber() {
  return static_cast<uint8_t>(g_currentPage) + 1;
}

void displayShowMessage(const String& title, const String& subtitle) {
  tft.fillScreen(kBg);
  drawCentered(title, 96, 4, kAccent);
  drawCentered(subtitle, 132, 2, kMuted);
}

void requestDisplayRedraw() {
  clearPageState();
  g_pageDirty = true;
}
