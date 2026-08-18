#pragma once

#include <Arduino.h>

// The 4.0" panel has room for twelve rows of spots against the 2.8" board's
// eight, which is the point of the extra height on this page: more spots beats
// bigger text here. The display keeps these two limits equal, because the POTA
// page renders through the DX row machinery.
#if DISPLAY_H >= 320
constexpr uint8_t kMaxDxSpots = 12;
#else
constexpr uint8_t kMaxDxSpots = 8;
#endif

struct DxSpot {
  String time;
  String freq;
  String call;
  String mode;
  String spotter;
  String comment;
  String band;
  String country;
  String continent;
};

struct DxSpotsData {
  bool hasData;
  String status;
  String updated;
  String source;
  String provider;
  uint8_t spotCount;
  DxSpot spots[kMaxDxSpots];
};

void dxSpotsBegin();
bool refreshDxSpotsIfNeeded(bool wifiConnected);
void requestDxSpotsRefresh();
const DxSpotsData& getDxSpotsData();
String getDxSpotsUrl();
