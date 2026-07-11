#pragma once

#include <Arduino.h>

constexpr uint8_t kMaxDxSpots = 8;

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
  uint8_t spotCount;
  DxSpot spots[kMaxDxSpots];
};

void dxSpotsBegin();
bool refreshDxSpotsIfNeeded(bool wifiConnected);
void requestDxSpotsRefresh();
const DxSpotsData& getDxSpotsData();
String getDxSpotsUrl();
