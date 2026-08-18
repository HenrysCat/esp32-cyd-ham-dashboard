#pragma once

#include <Arduino.h>

// The 4.0" panel has room for twelve rows of spots against the 2.8" board's
// eight, which is the point of the extra height on this page: more spots beats
// bigger text here. The display keeps these two limits equal, because the POTA
// page renders through the DX row machinery.
#if DISPLAY_H >= 320
constexpr uint8_t kMaxPotaSpots = 12;
#else
constexpr uint8_t kMaxPotaSpots = 8;
#endif

struct PotaSpot {
  String activator;
  String frequency;  // MHz, already formatted for display
  String mode;
  String reference;  // park reference, e.g. US-1882
  String locationDesc;
  String timeUtc;      // HH:MM taken from the spot timestamp
  String spotTimeRaw;  // ISO 8601, kept because it sorts lexicographically
  uint32_t distanceKm;
};

struct PotaSpotsData {
  bool hasData;
  String status;
  String updated;
  uint8_t spotCount;
  // Everything the feed returned, and how many of those the distance or RBN
  // filters rejected, so the page can say why a short list is short.
  uint16_t totalSpots;
  uint16_t filteredOut;
  PotaSpot spots[kMaxPotaSpots];
};

void potaSpotsBegin();
bool refreshPotaSpotsIfNeeded(bool wifiConnected);
void requestPotaSpotsRefresh();
const PotaSpotsData& getPotaSpotsData();
