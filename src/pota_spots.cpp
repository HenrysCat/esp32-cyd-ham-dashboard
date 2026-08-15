#include "pota_spots.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "connectivity.h"
#include "greyline.h"
#include "settings.h"

namespace {
constexpr char kPotaSpotsUrl[] = "https://api.pota.app/spot/activator";
constexpr uint32_t kHttpTimeoutMs = 8000;
// POTA publishes no formal rate limit, but existing client libraries settle on
// one request a minute, so that is treated as the floor here.
constexpr uint32_t kMinFetchIntervalMs = 60UL * 1000UL;
// The feed returns around a hundred spots. They are read one object at a time
// so the whole array is never held in RAM.
constexpr size_t kMaxObjectChars = 1024;
constexpr uint32_t kStreamIdleTimeoutMs = 6000;
// Identifies this firmware to POTA rather than leaving the default library
// string, so they can see what is calling if they ever need to.
constexpr char kUserAgent[] = "CYD-HamDashboard (esp32-cyd-ham-dashboard)";

PotaSpotsData g_data;
uint32_t g_lastAttemptMs = 0;
bool g_everAttempted = false;
bool g_refreshRequested = true;

// Dedup keys parallel to g_data.spots: one entry per activator at a park.
String g_spotKeys[kMaxPotaSpots];

uint32_t refreshIntervalMs() {
  const AppSettings& settings = getSettings();
  const uint16_t minutes = constrain(settings.potaRefreshMinutes, static_cast<uint16_t>(1),
                                     static_cast<uint16_t>(120));
  return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

String statusTimeUtc() {
  const time_t now = time(nullptr);
  tm utcTime;
  gmtime_r(&now, &utcTime);

  char buffer[16];
  strftime(buffer, sizeof(buffer), "%H:%M UTC", &utcTime);
  return String(buffer);
}

// POTA reports frequency in kHz as a string, e.g. "14050.0" or "7083".
String formatFrequency(const String& kHzText) {
  const double kHz = kHzText.toDouble();
  if (kHz <= 0.0) {
    return "--";
  }
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%.3f", kHz / 1000.0);
  return String(buffer);
}

// "2026-08-14T18:19:35" -> "18:19"
String formatSpotTime(const String& iso) {
  if (iso.length() >= 16 && iso.charAt(10) == 'T') {
    return iso.substring(11, 16);
  }
  return "--";
}

void resetData(const String& status) {
  g_data.hasData = false;
  g_data.status = status;
  g_data.spotCount = 0;
  g_data.totalSpots = 0;
  g_data.filteredOut = 0;
}

// Keeps the newest spots. A repeat spot of the same activator at the same park
// updates in place, so a station spotted twenty times does not crowd out
// everyone else.
void storeSpot(const PotaSpot& spot, const String& key) {
  for (uint8_t i = 0; i < g_data.spotCount; ++i) {
    if (g_spotKeys[i] == key) {
      if (spot.spotTimeRaw >= g_data.spots[i].spotTimeRaw) {
        g_data.spots[i] = spot;
      }
      return;
    }
  }

  if (g_data.spotCount < kMaxPotaSpots) {
    g_data.spots[g_data.spotCount] = spot;
    g_spotKeys[g_data.spotCount] = key;
    ++g_data.spotCount;
    return;
  }

  uint8_t oldest = 0;
  for (uint8_t i = 1; i < g_data.spotCount; ++i) {
    if (g_data.spots[i].spotTimeRaw < g_data.spots[oldest].spotTimeRaw) {
      oldest = i;
    }
  }
  if (spot.spotTimeRaw > g_data.spots[oldest].spotTimeRaw) {
    g_data.spots[oldest] = spot;
    g_spotKeys[oldest] = key;
  }
}

// ISO 8601 timestamps of the same length sort correctly as plain strings, so
// no date parsing is needed to put the newest spot at the top.
void sortNewestFirst() {
  for (uint8_t i = 1; i < g_data.spotCount; ++i) {
    PotaSpot spot = g_data.spots[i];
    String key = g_spotKeys[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && g_data.spots[j].spotTimeRaw < spot.spotTimeRaw) {
      g_data.spots[j + 1] = g_data.spots[j];
      g_spotKeys[j + 1] = g_spotKeys[j];
      --j;
    }
    g_data.spots[j + 1] = spot;
    g_spotKeys[j + 1] = key;
  }
}

// Pulls the next {...} out of the response, tracking string literals so a brace
// inside a park name or comment cannot end the object early. Returns false at
// the end of the array or on timeout.
bool readNextObject(WiFiClient& client, String& object) {
  object = "";
  bool inObject = false;
  bool inString = false;
  bool escaped = false;
  uint16_t depth = 0;
  bool overflowed = false;

  uint32_t lastDataMs = millis();
  while (true) {
    const int next = client.read();
    if (next < 0) {
      if (!client.connected() && client.available() == 0) {
        return false;
      }
      if (millis() - lastDataMs > kStreamIdleTimeoutMs) {
        return false;
      }
      delay(1);
      continue;
    }
    lastDataMs = millis();
    const char c = static_cast<char>(next);

    if (!inObject) {
      if (c == ']') {
        return false;
      }
      if (c != '{') {
        continue;
      }
      inObject = true;
      depth = 1;
      overflowed = false;
      object = "{";
      continue;
    }

    if (object.length() < kMaxObjectChars) {
      object += c;
    } else {
      overflowed = true;
    }

    if (escaped) {
      escaped = false;
      continue;
    }
    if (inString) {
      if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) {
        if (overflowed) {
          // Too long to parse; skip it and pick up the next one.
          inObject = false;
          object = "";
          continue;
        }
        return true;
      }
    }
  }
}

bool fetchSpots(bool haveQth, double qthLat, double qthLon) {
  const AppSettings& settings = getSettings();
  Serial.print("POTA fetch start: ");
  Serial.println(kPotaSpotsUrl);

  HTTPClient http;
  WiFiClientSecure secureClient;
  configureSecureClient(secureClient);
  if (!http.begin(secureClient, kPotaSpotsUrl)) {
    g_data.status = "Connect failed";
    return true;
  }

  http.setUserAgent(kUserAgent);
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int httpCode = http.GET();
  Serial.print("POTA HTTP code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    // Keep whatever is on screen; only the status line reports the failure.
    g_data.status = "HTTP " + String(httpCode);
    return true;
  }

  resetData("OK");

  // Only the fields the page needs are kept, so a long park name or comment
  // cannot overflow the per-object document.
  StaticJsonDocument<256> filter;
  filter["activator"] = true;
  filter["frequency"] = true;
  filter["mode"] = true;
  filter["reference"] = true;
  filter["spotTime"] = true;
  filter["locationDesc"] = true;
  filter["latitude"] = true;
  filter["longitude"] = true;
  filter["source"] = true;

  const uint32_t maxDistanceKm = settings.potaMaxDistanceKm;
  WiFiClient& client = http.getStream();
  String object;
  StaticJsonDocument<640> doc;

  while (readNextObject(client, object)) {
    if (g_data.totalSpots < 0xFFFF) {
      ++g_data.totalSpots;
    }

    doc.clear();
    if (deserializeJson(doc, object, DeserializationOption::Filter(filter))) {
      continue;
    }

    const String activator = doc["activator"] | "";
    const String reference = doc["reference"] | "";
    if (activator.length() == 0 || reference.length() == 0) {
      continue;
    }

    const String source = doc["source"] | "";
    if (settings.potaExcludeRbn && source == "RBN") {
      ++g_data.filteredOut;
      continue;
    }

    PotaSpot spot;
    spot.activator = activator;
    spot.reference = reference;
    spot.frequency = formatFrequency(doc["frequency"] | "");
    spot.mode = doc["mode"] | "";
    spot.locationDesc = doc["locationDesc"] | "";
    spot.spotTimeRaw = doc["spotTime"] | "";
    spot.timeUtc = formatSpotTime(spot.spotTimeRaw);
    spot.distanceKm = 0;

    if (haveQth && doc["latitude"].is<double>() && doc["longitude"].is<double>()) {
      spot.distanceKm = greatCircleKm(qthLat, qthLon, doc["latitude"].as<double>(),
                                      doc["longitude"].as<double>());
      if (maxDistanceKm > 0 && spot.distanceKm > maxDistanceKm) {
        ++g_data.filteredOut;
        continue;
      }
    }

    storeSpot(spot, activator + "|" + reference);
  }
  http.end();

  sortNewestFirst();
  g_data.hasData = g_data.spotCount > 0;
  g_data.updated = statusTimeUtc();
  if (g_data.totalSpots == 0) {
    g_data.status = "No data";
  } else if (g_data.spotCount == 0) {
    g_data.status = "All filtered out";
  }

  Serial.print("POTA spots: ");
  Serial.print(g_data.totalSpots);
  Serial.print(" returned, ");
  Serial.print(g_data.filteredOut);
  Serial.print(" filtered, ");
  Serial.print(g_data.spotCount);
  Serial.println(" shown");
  return true;
}
}  // namespace

void potaSpotsBegin() {
  resetData("Waiting");
  g_data.updated = "--";
}

void requestPotaSpotsRefresh() {
  g_refreshRequested = true;
}

bool refreshPotaSpotsIfNeeded(bool wifiConnected) {
  if (!wifiConnected) {
    if (g_data.status != "No Wi-Fi" && !g_data.hasData) {
      resetData("No Wi-Fi");
      return true;
    }
    return false;
  }

  const uint32_t nowMs = millis();
  const uint32_t sinceLastMs = nowMs - g_lastAttemptMs;
  if (g_everAttempted) {
    if (sinceLastMs < kMinFetchIntervalMs) {
      return false;
    }
    if (!g_refreshRequested && sinceLastMs < refreshIntervalMs()) {
      return false;
    }
  }

  g_lastAttemptMs = millis();
  g_everAttempted = true;
  g_refreshRequested = false;

  double qthLat = 0.0;
  double qthLon = 0.0;
  const bool haveQth = maidenheadToLatLon(getConfiguredLocator(), qthLat, qthLon);
  return fetchSpots(haveQth, qthLat, qthLon);
}

const PotaSpotsData& getPotaSpotsData() {
  return g_data;
}
