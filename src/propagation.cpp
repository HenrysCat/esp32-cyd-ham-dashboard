#include "propagation.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "app_config.h"
#include "settings.h"

#ifndef PROPAGATION_JSON_URL
#define PROPAGATION_JSON_URL ""
#endif

namespace {
constexpr char kHamQslUrl[] = "https://www.hamqsl.com/solarxml.php";
constexpr uint32_t kHttpTimeoutMs = 5000;

PropagationData g_data;
uint32_t g_lastAttemptMs = 0;
bool g_refreshRequested = true;

String configuredProxyUrl() {
  const AppSettings& settings = getSettings();
  String url = settings.useJsonPropagationProxy ? settings.propagationJsonUrl : "";
  url.trim();
  return url;
}

uint32_t refreshIntervalMs() {
  const AppSettings& settings = getSettings();
  const uint16_t minutes = constrain(settings.propagationRefreshMinutes,
                                     static_cast<uint16_t>(1),
                                     static_cast<uint16_t>(120));
  return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

String valueOrDash(String value) {
  value.trim();
  return value.length() > 0 ? value : String("--");
}

String statusTimeUtc() {
  const time_t now = time(nullptr);
  tm utcTime;
  gmtime_r(&now, &utcTime);

  char buffer[16];
  strftime(buffer, sizeof(buffer), "%H:%M UTC", &utcTime);
  return String(buffer);
}

bool httpGet(const String& url, String& body, int& httpCode) {
  Serial.print("Propagation fetch start: ");
  Serial.println(url);

  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;

  bool begun = false;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    begun = http.begin(secureClient, url);
  } else {
    begun = http.begin(plainClient, url);
  }

  if (!begun) {
    Serial.println("Propagation fetch failed: http.begin");
    httpCode = -1;
    return false;
  }

  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  httpCode = http.GET();
  Serial.print("Propagation HTTP code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  body = http.getString();
  http.end();
  return body.length() > 0;
}

String getXmlTagValue(const String& xml, const char* tag) {
  const String openTag = String("<") + tag + ">";
  const String closeTag = String("</") + tag + ">";
  int start = xml.indexOf(openTag);
  if (start < 0) {
    return "";
  }

  start += openTag.length();
  const int end = xml.indexOf(closeTag, start);
  if (end < 0) {
    return "";
  }

  String value = xml.substring(start, end);
  value.trim();
  return value;
}

String firstXmlTagValue(const String& xml, const char* firstTag, const char* secondTag = nullptr,
                        const char* thirdTag = nullptr) {
  String value = getXmlTagValue(xml, firstTag);
  if (value.length() > 0 || secondTag == nullptr) {
    return value;
  }

  value = getXmlTagValue(xml, secondTag);
  if (value.length() > 0 || thirdTag == nullptr) {
    return value;
  }

  return getXmlTagValue(xml, thirdTag);
}

String getXmlBandValue(const String& xml, const char* bandName, const char* timeName = nullptr) {
  int pos = 0;
  const String wanted = String("name=\"") + bandName + "\"";
  const String wantedTime = timeName == nullptr ? "" : String("time=\"") + timeName + "\"";
  while (true) {
    const int bandStart = xml.indexOf("<band", pos);
    if (bandStart < 0) {
      return "";
    }

    const int tagEnd = xml.indexOf(">", bandStart);
    if (tagEnd < 0) {
      return "";
    }

    const String attrs = xml.substring(bandStart, tagEnd);
    const int close = xml.indexOf("</band>", tagEnd);
    if (close < 0) {
      return "";
    }

    if (attrs.indexOf(wanted) >= 0 &&
        (timeName == nullptr || attrs.indexOf(wantedTime) >= 0)) {
      String value = xml.substring(tagEnd + 1, close);
      value.trim();
      return value;
    }

    pos = close + 7;
  }
}

String getXmlBandValueWithFallback(const String& xml, const char* bandName,
                                   const char* groupName, const char* timeName = nullptr) {
  String value = getXmlBandValue(xml, bandName, timeName);
  if (value.length() > 0) {
    return value;
  }

  return getXmlBandValue(xml, groupName, timeName);
}

String getJsonField(const String& json, const char* key) {
  const String marker = String("\"") + key + "\"";
  int pos = json.indexOf(marker);
  if (pos < 0) {
    return "";
  }

  pos = json.indexOf(':', pos + marker.length());
  if (pos < 0) {
    return "";
  }
  ++pos;

  while (pos < json.length() && isspace(static_cast<unsigned char>(json[pos]))) {
    ++pos;
  }

  if (pos >= json.length()) {
    return "";
  }

  if (json[pos] == '"') {
    ++pos;
    String value;
    while (pos < json.length() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.length()) {
        ++pos;
      }
      value += json[pos++];
    }
    value.trim();
    return value;
  }

  const int start = pos;
  while (pos < json.length() && json[pos] != ',' && json[pos] != '}' &&
         json[pos] != '\r' && json[pos] != '\n') {
    ++pos;
  }

  String value = json.substring(start, pos);
  value.trim();
  return value;
}

String firstJsonField(const String& json, const char* firstKey, const char* secondKey = nullptr,
                      const char* thirdKey = nullptr) {
  String value = getJsonField(json, firstKey);
  if (value.length() > 0 || secondKey == nullptr) {
    return value;
  }

  value = getJsonField(json, secondKey);
  if (value.length() > 0 || thirdKey == nullptr) {
    return value;
  }

  return getJsonField(json, thirdKey);
}

String getJsonCondition(const String& json, const char* bandName) {
  const int condStart = json.indexOf("\"conditions\"");
  if (condStart < 0) {
    return "";
  }

  const int objectStart = json.indexOf('{', condStart);
  const int objectEnd = json.indexOf('}', objectStart);
  if (objectStart < 0 || objectEnd < 0) {
    return "";
  }

  return getJsonField(json.substring(objectStart, objectEnd + 1), bandName);
}

String isoTimeToDisplay(String iso) {
  iso.trim();
  if (iso.length() >= 16 && iso[10] == 'T') {
    return iso.substring(11, 16) + " UTC";
  }
  return iso.length() > 0 ? iso : statusTimeUtc();
}

bool parseHamQslXml(String& xml, PropagationData& parsed) {
  parsed = PropagationData();
  parsed.sfi = valueOrDash(getXmlTagValue(xml, "solarflux"));
  parsed.aIndex = valueOrDash(getXmlTagValue(xml, "aindex"));
  parsed.kIndex = valueOrDash(getXmlTagValue(xml, "kindex"));
  parsed.sunspots = valueOrDash(getXmlTagValue(xml, "sunspots"));
  parsed.xray = valueOrDash(getXmlTagValue(xml, "xray"));
  parsed.solarWind = valueOrDash(getXmlTagValue(xml, "solarwind"));
  parsed.bz = valueOrDash(getXmlTagValue(xml, "magneticfield"));
  parsed.geomag = valueOrDash(getXmlTagValue(xml, "geomagfield"));
  parsed.signalNoise = valueOrDash(getXmlTagValue(xml, "signalnoise"));
  parsed.aurora = valueOrDash(getXmlTagValue(xml, "aurora"));
  parsed.fof2 = valueOrDash(firstXmlTagValue(xml, "fof2", "foF2"));
  parsed.mufFactor = valueOrDash(firstXmlTagValue(xml, "muffactor", "muf", "mufFactor"));

  parsed.band8040Day = valueOrDash(getXmlBandValue(xml, "80m-40m", "day"));
  parsed.band8040Night = valueOrDash(getXmlBandValue(xml, "80m-40m", "night"));
  parsed.band3020Day = valueOrDash(getXmlBandValue(xml, "30m-20m", "day"));
  parsed.band3020Night = valueOrDash(getXmlBandValue(xml, "30m-20m", "night"));
  parsed.band1715Day = valueOrDash(getXmlBandValue(xml, "17m-15m", "day"));
  parsed.band1715Night = valueOrDash(getXmlBandValue(xml, "17m-15m", "night"));
  parsed.band1210Day = valueOrDash(getXmlBandValue(xml, "12m-10m", "day"));
  parsed.band1210Night = valueOrDash(getXmlBandValue(xml, "12m-10m", "night"));

  parsed.band80m = valueOrDash(getXmlBandValueWithFallback(xml, "80m", "80m-40m", "day"));
  parsed.band40m = valueOrDash(getXmlBandValueWithFallback(xml, "40m", "80m-40m", "day"));
  parsed.band30m = valueOrDash(getXmlBandValueWithFallback(xml, "30m", "30m-20m", "day"));
  parsed.band20m = valueOrDash(getXmlBandValueWithFallback(xml, "20m", "30m-20m", "day"));
  parsed.band17m = valueOrDash(getXmlBandValueWithFallback(xml, "17m", "17m-15m", "day"));
  parsed.band15m = valueOrDash(getXmlBandValueWithFallback(xml, "15m", "17m-15m", "day"));
  parsed.band12m = valueOrDash(getXmlBandValueWithFallback(xml, "12m", "12m-10m", "day"));
  parsed.band10m = valueOrDash(getXmlBandValueWithFallback(xml, "10m", "12m-10m", "day"));

  if (parsed.sfi == "--" || parsed.aIndex == "--" || parsed.kIndex == "--") {
    return false;
  }

  parsed.updatedUtc = statusTimeUtc();
  parsed.status = "OK";
  parsed.hasData = true;
  return true;
}

bool parsePropagationJson(String& json, PropagationData& parsed) {
  if (getJsonField(json, "ok") == "false") {
    return false;
  }

  parsed = PropagationData();
  parsed.sfi = valueOrDash(getJsonField(json, "sfi"));
  parsed.aIndex = valueOrDash(getJsonField(json, "a_index"));
  parsed.kIndex = valueOrDash(getJsonField(json, "k_index"));
  parsed.sunspots = valueOrDash(getJsonField(json, "sunspots"));
  parsed.xray = valueOrDash(getJsonField(json, "xray"));
  parsed.solarWind = valueOrDash(firstJsonField(json, "solar_wind", "solar_wind_speed", "sw"));
  parsed.bz = valueOrDash(firstJsonField(json, "bz", "bz_gsm"));
  parsed.geomag = valueOrDash(getJsonField(json, "geomag"));
  parsed.signalNoise = valueOrDash(getJsonField(json, "signal_noise"));
  parsed.aurora = valueOrDash(getJsonField(json, "aurora"));
  parsed.fof2 = valueOrDash(getJsonField(json, "fof2"));
  parsed.mufFactor = valueOrDash(getJsonField(json, "muf_factor"));
  parsed.band8040Day = valueOrDash(getJsonCondition(json, "80m-40m_day"));
  parsed.band8040Night = valueOrDash(getJsonCondition(json, "80m-40m_night"));
  parsed.band3020Day = valueOrDash(getJsonCondition(json, "30m-20m_day"));
  parsed.band3020Night = valueOrDash(getJsonCondition(json, "30m-20m_night"));
  parsed.band1715Day = valueOrDash(getJsonCondition(json, "17m-15m_day"));
  parsed.band1715Night = valueOrDash(getJsonCondition(json, "17m-15m_night"));
  parsed.band1210Day = valueOrDash(getJsonCondition(json, "12m-10m_day"));
  parsed.band1210Night = valueOrDash(getJsonCondition(json, "12m-10m_night"));
  parsed.band80m = valueOrDash(getJsonCondition(json, "80m"));
  parsed.band40m = valueOrDash(getJsonCondition(json, "40m"));
  parsed.band30m = valueOrDash(getJsonCondition(json, "30m"));
  parsed.band20m = valueOrDash(getJsonCondition(json, "20m"));
  parsed.band17m = valueOrDash(getJsonCondition(json, "17m"));
  parsed.band15m = valueOrDash(getJsonCondition(json, "15m"));
  parsed.band12m = valueOrDash(getJsonCondition(json, "12m"));
  parsed.band10m = valueOrDash(getJsonCondition(json, "10m"));
  if (parsed.band8040Day == "--") parsed.band8040Day = parsed.band80m;
  if (parsed.band3020Day == "--") parsed.band3020Day = parsed.band30m;
  if (parsed.band1715Day == "--") parsed.band1715Day = parsed.band17m;
  if (parsed.band1210Day == "--") parsed.band1210Day = parsed.band12m;
  parsed.updatedUtc = isoTimeToDisplay(getJsonField(json, "updated"));

  if (parsed.sfi == "--" || parsed.aIndex == "--" || parsed.kIndex == "--") {
    return false;
  }

  parsed.status = "OK";
  parsed.hasData = true;
  return true;
}

bool fetchPropagationData() {
  String url = configuredProxyUrl();
  const bool useJsonProxy = url.length() > 0;
  if (!useJsonProxy) {
    url = kHamQslUrl;
  }

  String body;
  int httpCode = 0;
  if (!httpGet(url, body, httpCode)) {
    g_data.status = "Fetch failed";
    Serial.println("Propagation status: Fetch failed");
    return false;
  }

  PropagationData parsed;
  const bool parsedOk = useJsonProxy ? parsePropagationJson(body, parsed)
                                    : parseHamQslXml(body, parsed);
  body = "";

  if (!parsedOk) {
    g_data.status = "Parse failed";
    Serial.println("Propagation status: Parse failed");
    return false;
  }

  g_data = parsed;
  Serial.println("Propagation parse result: OK");
  Serial.println("Propagation status: OK");
  return true;
}

void setEmptyPropagationFields(const String& status) {
  g_data = PropagationData();
  g_data.hasData = false;
  g_data.sfi = "--";
  g_data.aIndex = "--";
  g_data.kIndex = "--";
  g_data.sunspots = "--";
  g_data.xray = "--";
  g_data.solarWind = "--";
  g_data.bz = "--";
  g_data.geomag = "--";
  g_data.signalNoise = "--";
  g_data.aurora = "--";
  g_data.fof2 = "--";
  g_data.mufFactor = "--";
  g_data.band8040Day = "--";
  g_data.band8040Night = "--";
  g_data.band3020Day = "--";
  g_data.band3020Night = "--";
  g_data.band1715Day = "--";
  g_data.band1715Night = "--";
  g_data.band1210Day = "--";
  g_data.band1210Night = "--";
  g_data.band80m = "--";
  g_data.band40m = "--";
  g_data.band30m = "--";
  g_data.band20m = "--";
  g_data.band17m = "--";
  g_data.band15m = "--";
  g_data.band12m = "--";
  g_data.band10m = "--";
  g_data.updatedUtc = "--";
  g_data.status = status;
}
}

void propagationBegin() {
  setEmptyPropagationFields("Waiting");
  g_refreshRequested = true;
}

bool refreshPropagationIfNeeded(bool wifiConnected) {
  const uint32_t nowMs = millis();
  const uint32_t intervalMs = refreshIntervalMs();
  const bool due = g_lastAttemptMs == 0 || nowMs - g_lastAttemptMs >= intervalMs;
  if (!g_refreshRequested && !due) {
    return false;
  }

  if (!wifiConnected) {
    g_lastAttemptMs = nowMs - intervalMs + 5000;
    g_refreshRequested = false;
    g_data.status = "WiFi offline";
    Serial.println("Propagation status: WiFi offline");
    return true;
  }

  g_lastAttemptMs = nowMs;
  g_refreshRequested = false;
  return fetchPropagationData();
}

void requestPropagationRefresh() {
  g_refreshRequested = true;
}

const PropagationData& getPropagationData() {
  return g_data;
}
