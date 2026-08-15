#pragma once

#include <Arduino.h>

// Unique grid squares plotted on the map. Reports beyond this are still counted
// and still considered for the best-DX line; only the map markers are capped.
constexpr uint8_t kMaxPskReports = 48;

constexpr uint8_t kPskBandUnknown = 0xFF;

// Which end of the path the configured callsign is on.
enum PskDirection : uint8_t {
  kPskWhoHearsMe = 0,  // senderCallsign=<call>, so the far end is the receiver
  kPskWhoIHear = 1     // receiverCallsign=<call>, so the far end is the sender
};

struct PskReport {
  double latitude;
  double longitude;
  String callsign;
  String locator;
  uint8_t bandIndex;  // index into the band table, or kPskBandUnknown
  uint32_t distanceKm;
  uint32_t flowStartSeconds;
};

struct PskReporterData {
  bool hasData;
  String status;
  String updated;
  String callsign;
  // Reports kept for plotting, deduplicated to one per four-character grid.
  uint8_t reportCount;
  PskReport reports[kMaxPskReports];
  // Every reception report the query returned, including duplicate grids.
  uint16_t totalReports;
  // Bands seen across all reports, as a bit per band table index.
  uint16_t bandMask;
  String bestCallsign;
  String bestLocator;
  uint32_t bestDistanceKm;
};

void pskReporterBegin();
bool refreshPskReporterIfNeeded(bool wifiConnected);
void requestPskReporterRefresh();
const PskReporterData& getPskReporterData();

// Band table lookups, used by the display to colour markers and label the
// bands that are currently being heard.
uint8_t pskBandCount();
const char* pskBandLabel(uint8_t bandIndex);
uint16_t pskBandColor(uint8_t bandIndex);
