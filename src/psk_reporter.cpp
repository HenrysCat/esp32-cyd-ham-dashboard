#include "psk_reporter.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>

#include "connectivity.h"
#include "greyline.h"
#include "settings.h"

namespace {
constexpr char kQueryHost[] = "https://retrieve.pskreporter.info/query";
constexpr uint32_t kHttpTimeoutMs = 8000;
// PSKReporter asks that reception data is retrieved no more often than once
// every five minutes, and reserves the right to block anyone who loads the
// system. This floor applies to manual refreshes too, so tapping the page
// cannot walk past it.
constexpr uint32_t kMinFetchIntervalMs = 5UL * 60UL * 1000UL;
// The reports come back as flat self-closing elements, so the parser only ever
// holds one of them at a time rather than the whole document.
constexpr size_t kMaxElementChars = 512;
constexpr uint16_t kReportLimit = 150;
// Give up on a stalled response rather than blocking the display loop forever.
constexpr uint32_t kStreamIdleTimeoutMs = 6000;

struct BandInfo {
  const char* label;
  uint32_t lowHz;
  uint32_t highHz;
  uint16_t color;
};

// Colours follow the usual band conventions closely enough to be read at a
// glance; each is distinct in RGB565 on a dark map.
constexpr BandInfo kBands[] = {
    {"160m", 1800000UL, 2000000UL, 0x780F},
    {"80m", 3500000UL, 4000000UL, 0x001F},
    {"60m", 5250000UL, 5450000UL, 0x03EF},
    {"40m", 7000000UL, 7300000UL, 0x07E0},
    {"30m", 10100000UL, 10150000UL, 0x07FF},
    {"20m", 14000000UL, 14350000UL, 0xFFE0},
    {"17m", 18068000UL, 18168000UL, 0xFD20},
    {"15m", 21000000UL, 21450000UL, 0xF800},
    {"12m", 24890000UL, 24990000UL, 0xF81F},
    {"10m", 28000000UL, 29700000UL, 0xFFFF},
    {"6m", 50000000UL, 54000000UL, 0xFC9F},
    {"4m", 70000000UL, 70500000UL, 0xAFE5},
    {"2m", 144000000UL, 148000000UL, 0x86FF},
    {"70cm", 420000000UL, 450000000UL, 0xBDF7},
};
constexpr uint8_t kBandCount = sizeof(kBands) / sizeof(kBands[0]);

PskReporterData g_data;
uint32_t g_lastAttemptMs = 0;
bool g_everAttempted = false;
bool g_refreshRequested = true;

// Grid keys parallel to g_data.reports, used to keep one marker per square.
char g_gridKeys[kMaxPskReports][5];

uint32_t refreshIntervalMs() {
  const AppSettings& settings = getSettings();
  const uint16_t minutes = constrain(settings.pskRefreshMinutes, static_cast<uint16_t>(5),
                                     static_cast<uint16_t>(120));
  return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

uint32_t windowSeconds() {
  const AppSettings& settings = getSettings();
  const uint16_t minutes = constrain(settings.pskWindowMinutes, static_cast<uint16_t>(5),
                                     static_cast<uint16_t>(360));
  return static_cast<uint32_t>(minutes) * 60UL;
}

String configuredCallsign() {
  String callsign = getSettings().callsign;
  callsign.trim();
  callsign.toUpperCase();
  return callsign;
}

String urlEncode(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                            c == '~';
    if (unreserved) {
      out += c;
    } else {
      char buffer[4];
      snprintf(buffer, sizeof(buffer), "%%%02X", static_cast<unsigned char>(c));
      out += buffer;
    }
  }
  return out;
}

String statusTimeUtc() {
  const time_t now = time(nullptr);
  tm utcTime;
  gmtime_r(&now, &utcTime);

  char buffer[16];
  strftime(buffer, sizeof(buffer), "%H:%M UTC", &utcTime);
  return String(buffer);
}

uint8_t bandIndexForHz(double hz) {
  if (hz <= 0.0) {
    return kPskBandUnknown;
  }
  for (uint8_t i = 0; i < kBandCount; ++i) {
    if (hz >= kBands[i].lowHz && hz <= kBands[i].highHz) {
      return i;
    }
  }
  return kPskBandUnknown;
}

// Pulls one attribute out of a self-closing element. The name must sit on a
// whitespace boundary so that senderLocator cannot be matched when
// receiverLocator was asked for.
String xmlAttribute(const String& element, const char* name) {
  const String needle = String(name) + "=\"";
  int from = 0;
  while (true) {
    const int at = element.indexOf(needle, from);
    if (at < 0) {
      return "";
    }
    const bool onBoundary =
        at == 0 || isspace(static_cast<unsigned char>(element.charAt(at - 1)));
    if (onBoundary) {
      const int start = at + needle.length();
      const int end = element.indexOf('"', start);
      return end < 0 ? String("") : element.substring(start, end);
    }
    from = at + 1;
  }
}

// Scans the response for the next <receptionReport .../> and returns its
// attribute text. Nothing larger than one element is ever buffered, so a busy
// callsign returning hundreds of reports costs no more heap than a quiet one.
bool readNextReportElement(WiFiClient& client, String& element) {
  static const char kTag[] = "<receptionReport";
  constexpr size_t kTagLen = sizeof(kTag) - 1;

  size_t matched = 0;
  bool collecting = false;
  bool boundaryChecked = false;
  element = "";

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

    if (!collecting) {
      if (c == kTag[matched]) {
        if (++matched == kTagLen) {
          collecting = true;
          boundaryChecked = false;
        }
      } else {
        matched = (c == '<') ? 1 : 0;
      }
      continue;
    }

    // The root element is <receptionReports>, which shares this prefix. Only a
    // separator after the tag name means a real report has been found.
    if (!boundaryChecked) {
      boundaryChecked = true;
      if (!isspace(static_cast<unsigned char>(c)) && c != '/' && c != '>') {
        collecting = false;
        matched = (c == '<') ? 1 : 0;
        continue;
      }
    }

    if (c == '>') {
      return true;
    }
    if (element.length() < kMaxElementChars) {
      element += c;
    }
  }
}

// Leaves the plotted reports alone so a single failed query does not blank a
// map that was good a few minutes ago. The status line carries the failure.
void setStatusOnly(const String& callsign, const String& status) {
  g_data.callsign = callsign;
  g_data.status = status;
}

void resetData(const String& callsign, const String& status) {
  g_data.hasData = false;
  g_data.status = status;
  g_data.callsign = callsign;
  g_data.reportCount = 0;
  g_data.totalReports = 0;
  g_data.bandMask = 0;
  g_data.bestCallsign = "";
  g_data.bestLocator = "";
  g_data.bestDistanceKm = 0;
}

// Keeps one marker per four-character grid. Once the array is full the oldest
// report is displaced, so the map follows current activity rather than
// freezing on whatever arrived first.
void storeReport(const PskReport& report, const char* gridKey) {
  for (uint8_t i = 0; i < g_data.reportCount; ++i) {
    if (strncmp(g_gridKeys[i], gridKey, 4) == 0) {
      if (report.flowStartSeconds >= g_data.reports[i].flowStartSeconds) {
        g_data.reports[i] = report;
      }
      return;
    }
  }

  if (g_data.reportCount < kMaxPskReports) {
    g_data.reports[g_data.reportCount] = report;
    strncpy(g_gridKeys[g_data.reportCount], gridKey, 4);
    g_gridKeys[g_data.reportCount][4] = '\0';
    ++g_data.reportCount;
    return;
  }

  uint8_t oldest = 0;
  for (uint8_t i = 1; i < g_data.reportCount; ++i) {
    if (g_data.reports[i].flowStartSeconds < g_data.reports[oldest].flowStartSeconds) {
      oldest = i;
    }
  }
  if (report.flowStartSeconds > g_data.reports[oldest].flowStartSeconds) {
    g_data.reports[oldest] = report;
    strncpy(g_gridKeys[oldest], gridKey, 4);
    g_gridKeys[oldest][4] = '\0';
  }
}

String buildQueryUrl(const String& callsign) {
  const AppSettings& settings = getSettings();
  String url = kQueryHost;
  url += settings.pskDirection == kPskWhoIHear ? "?receiverCallsign=" : "?senderCallsign=";
  url += urlEncode(callsign);
  // Negative flowStartSeconds asks for a window ending now.
  url += "&flowStartSeconds=-";
  url += String(windowSeconds());
  // Reception reports only; the active-monitor list would dwarf them.
  url += "&rronly=1&noactive=1&rptlimit=";
  url += String(kReportLimit);

  String contact = settings.pskAppContact;
  contact.trim();
  if (contact.length() > 0) {
    // PSKReporter uses this to make contact before blocking a busy client.
    url += "&appcontact=";
    url += urlEncode(contact);
  }
  return url;
}

bool fetchReports(const String& callsign, double qthLat, double qthLon) {
  const String url = buildQueryUrl(callsign);
  Serial.print("PSK fetch start: ");
  Serial.println(url);

  HTTPClient http;
  WiFiClientSecure secureClient;
  configureSecureClient(secureClient);
  if (!http.begin(secureClient, url)) {
    setStatusOnly(callsign, "Connect failed");
    return true;
  }

  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int httpCode = http.GET();
  Serial.print("PSK HTTP code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    // 503 is how PSKReporter turns away a client that is querying too often.
    setStatusOnly(callsign, httpCode == 503 ? "Rate limited" : "HTTP " + String(httpCode));
    return true;
  }

  resetData(callsign, "OK");

  const bool wantSender = getSettings().pskDirection == kPskWhoIHear;
  const char* callAttribute = wantSender ? "senderCallsign" : "receiverCallsign";
  const char* locatorAttribute = wantSender ? "senderLocator" : "receiverLocator";

  WiFiClient& client = http.getStream();
  String element;
  while (readNextReportElement(client, element)) {
    if (g_data.totalReports < 0xFFFF) {
      ++g_data.totalReports;
    }

    const String locator = xmlAttribute(element, locatorAttribute);
    double latitude = 0.0;
    double longitude = 0.0;
    if (locator.length() < 4 || !maidenheadToLatLon(locator, latitude, longitude)) {
      continue;
    }

    PskReport report;
    report.latitude = latitude;
    report.longitude = longitude;
    report.callsign = xmlAttribute(element, callAttribute);
    report.locator = locator;
    report.bandIndex = bandIndexForHz(xmlAttribute(element, "frequency").toDouble());
    report.distanceKm = greatCircleKm(qthLat, qthLon, latitude, longitude);
    report.flowStartSeconds =
        static_cast<uint32_t>(xmlAttribute(element, "flowStartSeconds").toInt());

    if (report.bandIndex != kPskBandUnknown) {
      g_data.bandMask |= static_cast<uint16_t>(1u << report.bandIndex);
    }
    // Best DX is tracked over everything returned, not just what fits on the map.
    if (report.distanceKm > g_data.bestDistanceKm) {
      g_data.bestDistanceKm = report.distanceKm;
      g_data.bestCallsign = report.callsign;
      g_data.bestLocator = report.locator;
    }

    char gridKey[5];
    strncpy(gridKey, locator.c_str(), 4);
    gridKey[4] = '\0';
    storeReport(report, gridKey);
  }
  http.end();

  g_data.hasData = g_data.reportCount > 0;
  g_data.updated = statusTimeUtc();
  if (g_data.totalReports == 0) {
    g_data.status = "No reports";
  }

  Serial.print("PSK reports: ");
  Serial.print(g_data.totalReports);
  Serial.print(" total, ");
  Serial.print(g_data.reportCount);
  Serial.print(" grids, best ");
  Serial.print(g_data.bestDistanceKm);
  Serial.println(" km");
  return true;
}
}  // namespace

void pskReporterBegin() {
  resetData(configuredCallsign(), "Waiting");
  g_data.updated = "--";
}

void requestPskReporterRefresh() {
  g_refreshRequested = true;
}

bool refreshPskReporterIfNeeded(bool wifiConnected) {
  const String callsign = configuredCallsign();
  if (callsign.length() == 0) {
    if (g_data.status != "No callsign") {
      resetData("", "No callsign");
      g_data.updated = "--";
      return true;
    }
    return false;
  }

  if (!wifiConnected) {
    if (g_data.status != "No Wi-Fi" && !g_data.hasData) {
      resetData(callsign, "No Wi-Fi");
      return true;
    }
    return false;
  }

  const uint32_t nowMs = millis();
  const uint32_t sinceLastMs = nowMs - g_lastAttemptMs;
  if (g_everAttempted) {
    // The five minute floor is deliberately checked before the refresh request,
    // so a manual refresh waits its turn instead of hammering the service.
    if (sinceLastMs < kMinFetchIntervalMs) {
      return false;
    }
    if (!g_refreshRequested && sinceLastMs < refreshIntervalMs()) {
      return false;
    }
  }

  // The configured callsign may have changed since the last fetch.
  if (g_data.callsign != callsign) {
    g_data.callsign = callsign;
  }

  g_lastAttemptMs = millis();
  g_everAttempted = true;
  g_refreshRequested = false;

  double qthLat = 0.0;
  double qthLon = 0.0;
  if (!maidenheadToLatLon(getConfiguredLocator(), qthLat, qthLon)) {
    resetData(callsign, "Locator invalid");
    return true;
  }

  return fetchReports(callsign, qthLat, qthLon);
}

const PskReporterData& getPskReporterData() {
  return g_data;
}

uint8_t pskBandCount() {
  return kBandCount;
}

const char* pskBandLabel(uint8_t bandIndex) {
  return bandIndex < kBandCount ? kBands[bandIndex].label : "?";
}

uint16_t pskBandColor(uint8_t bandIndex) {
  return bandIndex < kBandCount ? kBands[bandIndex].color : 0xC618;
}
