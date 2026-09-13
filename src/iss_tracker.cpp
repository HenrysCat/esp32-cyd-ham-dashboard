#include "iss_tracker.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Sgp4.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "connectivity.h"
#include "greyline.h"
#include "settings.h"

namespace {
constexpr char kSatelliteId[] = "25544";  // ISS (ZARYA)
constexpr uint32_t kHttpTimeoutMs = 8000;
// Orbital elements drift slowly enough that a day-old TLE still propagates a
// position accurate to well within what this map can show.
constexpr uint32_t kTleRefreshIntervalMs = 24UL * 60UL * 60UL * 1000UL;
// Position and track are pure local computation once a TLE is loaded, no
// network cost at all, so this can run often enough to look live.
constexpr uint32_t kPositionRefreshIntervalMs = 20UL * 1000UL;
// Pass geometry barely changes minute to minute, so this is refreshed far
// less often than the position, and it is the one piece still asked of N2YO.
constexpr uint32_t kPassesRefreshIntervalMs = 60UL * 60UL * 1000UL;
constexpr uint8_t kPassSearchDays = 2;
constexpr uint8_t kMinElevationDeg = 10;
constexpr char kTleUrl[] = "https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE";

IssTrackerData g_data;
Sgp4 g_sat;
bool g_haveTle = false;
uint32_t g_lastTleAttemptMs = 0;
uint32_t g_lastPositionAttemptMs = 0;
uint32_t g_lastPassesAttemptMs = 0;
bool g_everAttemptedTle = false;
bool g_everComputedPosition = false;
bool g_everAttemptedPasses = false;
bool g_refreshRequested = true;

void resetData(const String& status) {
  g_data.hasPosition = false;
  g_data.latitude = 0.0;
  g_data.longitude = 0.0;
  g_data.altitudeKm = 0.0;
  g_data.azimuthDeg = 0.0;
  g_data.elevationDeg = 0.0;
  g_data.positionUpdatedUtc = 0;
  g_data.trackCount = 0;
  g_data.passCount = 0;
  g_data.passesUpdatedUtc = 0;
  g_data.status = status;
}

String formatCoord(double value) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%.4f", value);
  return String(buffer);
}

// radiopasses rather than visualpasses: hams care about any pass above the
// elevation threshold for RF contact opportunities (APRS, voice repeater,
// SSTV), not just the rarer passes where the ISS happens to be sunlit
// against a dark sky.
String buildPassesUrl(double lat, double lon, const String& apiKey) {
  String url = "https://api.n2yo.com/rest/v1/satellite/radiopasses/";
  url += kSatelliteId;
  url += "/" + formatCoord(lat) + "/" + formatCoord(lon) + "/0/" + String(kPassSearchDays) +
         "/" + String(kMinElevationDeg) + "/&apiKey=" + apiKey;
  return url;
}

// Both the TLE and the N2YO passes call return a small, bounded body, so it
// is read whole rather than streamed the way the larger DX/POTA feeds are.
bool fetchBody(const String& url, String& body) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  configureSecureClient(secureClient);
  if (!http.begin(secureClient, url)) {
    return false;
  }
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    body = "HTTP " + String(httpCode);
    return false;
  }
  body = http.getString();
  http.end();
  return true;
}

// Celestrak's GP.php TLE format is three lines: satellite name, then the two
// numbered element lines. Sgp4::init wants each as a mutable buffer rather
// than a String, hence the copies.
bool fetchTle() {
  String body;
  if (!fetchBody(kTleUrl, body)) {
    return false;
  }

  const int firstBreak = body.indexOf('\n');
  if (firstBreak < 0) {
    return false;
  }
  const int secondBreak = body.indexOf('\n', firstBreak + 1);
  if (secondBreak < 0) {
    return false;
  }
  const int thirdBreak = body.indexOf('\n', secondBreak + 1);

  String name = body.substring(0, firstBreak);
  String line1 = body.substring(firstBreak + 1, secondBreak);
  String line2 = thirdBreak > 0 ? body.substring(secondBreak + 1, thirdBreak)
                                 : body.substring(secondBreak + 1);
  name.trim();
  line1.trim();
  line2.trim();
  if (!line1.startsWith("1 ") || !line2.startsWith("2 ")) {
    return false;
  }

  char nameBuf[25];
  char line1Buf[130];
  char line2Buf[130];
  name.toCharArray(nameBuf, sizeof(nameBuf));
  line1.toCharArray(line1Buf, sizeof(line1Buf));
  line2.toCharArray(line2Buf, sizeof(line2Buf));

  g_haveTle = g_sat.init(nameBuf, line1Buf, line2Buf);
  return g_haveTle;
}

// Propagates the loaded TLE to "now" for the current position, then walks
// outward either side of it to build the ground track. Sgp4::findsat is a
// pure computation (no I/O), so kMaxIssTrackPoints extra calls per refresh
// cost microseconds, not anything worth economising on.
void updateSgp4Position(double qthLat, double qthLon, time_t nowEpoch) {
  g_sat.site(qthLat, qthLon, 0.0);

  g_sat.findsat(static_cast<unsigned long>(nowEpoch));
  g_data.latitude = g_sat.satLat;
  g_data.longitude = g_sat.satLon;
  g_data.altitudeKm = g_sat.satAlt;
  // site() above is what makes these topocentric rather than geocentric.
  g_data.azimuthDeg = g_sat.satAz;
  g_data.elevationDeg = g_sat.satEl;
  g_data.positionUpdatedUtc = nowEpoch;
  g_data.hasPosition = true;

  g_data.trackCount = 0;
  const long windowSeconds = static_cast<long>(kIssTrackWindowMinutes) * 60L;
  for (long offset = -windowSeconds;
       offset <= windowSeconds && g_data.trackCount < kMaxIssTrackPoints;
       offset += kIssTrackStepSeconds) {
    g_sat.findsat(static_cast<unsigned long>(nowEpoch + offset));
    g_data.track[g_data.trackCount].latitude = g_sat.satLat;
    g_data.track[g_data.trackCount].longitude = g_sat.satLon;
    ++g_data.trackCount;
  }
}

bool fetchPasses(double qthLat, double qthLon, const String& apiKey) {
  String body;
  if (!fetchBody(buildPassesUrl(qthLat, qthLon, apiKey), body)) {
    // A position may already be on screen from the local propagation, so
    // only clobber the status line if there is nothing better to show.
    if (!g_data.hasPosition) {
      g_data.status = body.length() > 0 ? body : "Connect failed";
    }
    return true;
  }

  DynamicJsonDocument filter(256);
  filter["passes"][0]["startUTC"] = true;
  filter["passes"][0]["maxUTC"] = true;
  filter["passes"][0]["maxEl"] = true;
  filter["passes"][0]["endUTC"] = true;

  DynamicJsonDocument doc(2048);
  const DeserializationError error =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  g_data.passesUpdatedUtc = time(nullptr);
  if (error) {
    g_data.passCount = 0;
    if (!g_data.hasPosition) {
      g_data.status = "Parse failed";
    }
    return true;
  }

  JsonArray passes = doc["passes"].as<JsonArray>();
  g_data.passCount = 0;
  if (!passes.isNull()) {
    for (JsonObject pass : passes) {
      if (g_data.passCount >= kMaxIssPasses) {
        break;
      }
      IssPass& out = g_data.passes[g_data.passCount];
      out.aosUtc = static_cast<time_t>(pass["startUTC"] | 0L);
      out.maxElUtc = static_cast<time_t>(pass["maxUTC"] | 0L);
      out.losUtc = static_cast<time_t>(pass["endUTC"] | 0L);
      out.maxElevationDeg = pass["maxEl"] | 0.0;
      ++g_data.passCount;
    }
  }
  return true;
}
}  // namespace

void issTrackerBegin() {
  resetData("Waiting");
  g_haveTle = false;
  g_everAttemptedTle = false;
  g_everComputedPosition = false;
  g_everAttemptedPasses = false;
  g_refreshRequested = true;
}

void requestIssTrackerRefresh() {
  g_refreshRequested = true;
}

bool issTrackerActive() {
  const AppSettings& settings = getSettings();
  return settings.issEnabled && settings.n2yoApiKey.length() > 0;
}

bool refreshIssTrackerIfNeeded(bool wifiConnected, time_t epoch, bool timeValid) {
  if (!issTrackerActive()) {
    if (g_data.status != "Off") {
      resetData("Off");
      g_haveTle = false;
      return true;
    }
    return false;
  }

  double qthLat = 0.0;
  double qthLon = 0.0;
  if (!getConfiguredLatitude(qthLat) || !getConfiguredLongitude(qthLon)) {
    if (g_data.status != "Locator invalid") {
      resetData("Locator invalid");
      return true;
    }
    return false;
  }

  const AppSettings& settings = getSettings();
  const uint32_t nowMs = millis();
  bool changed = false;

  if (wifiConnected) {
    const bool wantTle = !g_everAttemptedTle || g_refreshRequested ||
                          (nowMs - g_lastTleAttemptMs >= kTleRefreshIntervalMs);
    if (wantTle) {
      g_lastTleAttemptMs = nowMs;
      g_everAttemptedTle = true;
      changed = fetchTle() || changed;
    }
  }

  if (g_haveTle && timeValid) {
    if (!g_everComputedPosition || g_refreshRequested ||
        nowMs - g_lastPositionAttemptMs >= kPositionRefreshIntervalMs) {
      g_lastPositionAttemptMs = nowMs;
      g_everComputedPosition = true;
      updateSgp4Position(qthLat, qthLon, epoch);
      g_data.status = "OK";
      changed = true;
    }
  } else if (!g_data.hasPosition) {
    g_data.status = !timeValid ? "No NTP" : "Fetching TLE";
  }

  if (wifiConnected) {
    const bool wantPasses = !g_everAttemptedPasses || g_refreshRequested ||
                             (nowMs - g_lastPassesAttemptMs >= kPassesRefreshIntervalMs);
    if (wantPasses) {
      g_lastPassesAttemptMs = nowMs;
      g_everAttemptedPasses = true;
      changed = fetchPasses(qthLat, qthLon, settings.n2yoApiKey) || changed;
    }
  }

  g_refreshRequested = false;
  return changed;
}

const IssTrackerData& getIssTrackerData() {
  return g_data;
}
