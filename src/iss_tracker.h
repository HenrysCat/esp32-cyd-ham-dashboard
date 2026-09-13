#pragma once

#include <Arduino.h>
#include <time.h>

// The 4.0" board's Then row has enough width to list more upcoming passes
// than the 2.8" board's, matching the DX/POTA pages' precedent of showing
// more rows rather than just bigger text on the taller panel.
#if DISPLAY_H >= 320
constexpr uint8_t kMaxIssPasses = 5;
#else
constexpr uint8_t kMaxIssPasses = 4;
#endif

// Ground track window: the SGP4 propagator can be asked for any point in
// time at no network cost, so this covers roughly half the ISS's ~93 minute
// orbit either side of now, sampled every 90 seconds - dense enough for the
// map's pixel resolution, sparse enough to keep the point count (and the RAM
// behind it) small.
constexpr uint16_t kIssTrackWindowMinutes = 45;
constexpr uint16_t kIssTrackStepSeconds = 90;
constexpr uint8_t kMaxIssTrackPoints = static_cast<uint8_t>(
    (static_cast<uint32_t>(kIssTrackWindowMinutes) * 60UL * 2UL) / kIssTrackStepSeconds + 1UL);

struct IssTrackPoint {
  double latitude;
  double longitude;
};

struct IssPass {
  time_t aosUtc;
  time_t maxElUtc;
  time_t losUtc;
  float maxElevationDeg;
};

struct IssTrackerData {
  bool hasPosition;
  double latitude;
  double longitude;
  double altitudeKm;
  // Topocentric look angles from the configured QTH, i.e. where to actually
  // point at the sky to see it right now.
  double azimuthDeg;
  double elevationDeg;
  time_t positionUpdatedUtc;

  // Ground track either side of "now", oldest first.
  uint8_t trackCount;
  IssTrackPoint track[kMaxIssTrackPoints];

  uint8_t passCount;
  IssPass passes[kMaxIssPasses];
  time_t passesUpdatedUtc;

  String status;
};

void issTrackerBegin();
void requestIssTrackerRefresh();
bool refreshIssTrackerIfNeeded(bool wifiConnected, time_t epoch, bool timeValid);
const IssTrackerData& getIssTrackerData();
// On once the feature is switched on in settings and an N2YO API key is set;
// the dashboard uses this to keep the page out of both manual and automatic
// navigation until it is actually usable.
bool issTrackerActive();
