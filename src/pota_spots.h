#pragma once

#include <Arduino.h>

constexpr uint8_t kMaxPotaSpots = 8;

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
