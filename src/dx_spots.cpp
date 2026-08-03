#include "dx_spots.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>

#include "app_config.h"
#include "settings.h"

#ifndef DX_SPOTS_URL
#define DX_SPOTS_URL ""
#endif

namespace {
#define DEFAULT_DX_SPOTS_URL "https://web.cluster.iz3mez.it/spots.json/"

#ifndef DEBUG_DX_TELNET
#define DEBUG_DX_TELNET 0
#endif

constexpr uint32_t kHttpTimeoutMs = 5000;
constexpr size_t kMaxDxObjectChars = 4096;
constexpr uint32_t kTelnetReconnectIntervalMs = 30000;
constexpr uint32_t kTelnetLoginDelayMs = 1500;
constexpr int32_t kTelnetConnectTimeoutMs = 5000;
constexpr size_t kMaxTelnetLineChars = 180;
constexpr size_t kMaxTelnetBytesPerLoop = 512;

enum TelnetConnectState : uint8_t {
  kTelnetConnectIdle,
  kTelnetConnectRunning,
  kTelnetConnectSucceeded,
  kTelnetConnectFailed,
  kTelnetConnectCancelled
};

struct TelnetConnectRequest {
  char host[65];
  uint16_t port;
};

DxSpotsData g_data;
uint32_t g_lastAttemptMs = 0;
bool g_refreshRequested = true;
WiFiClient g_telnetClient;
String g_telnetLineBuffer;
bool g_telnetLineOverflow = false;
bool g_telnetConnected = false;
bool g_telnetLoggedIn = false;
bool g_telnetHasCurrentSpots = false;
bool g_autoUsingTelnet = false;
uint32_t g_lastTelnetConnectAttemptMs = 0;
uint32_t g_telnetConnectedAtMs = 0;
uint32_t g_lastTelnetDataMs = 0;
uint8_t g_lastSourceMode = 0xFF;
TelnetConnectRequest g_telnetConnectRequest;
volatile TelnetConnectState g_telnetConnectState = kTelnetConnectIdle;
volatile bool g_telnetConnectCancelRequested = false;
portMUX_TYPE g_telnetConnectMux = portMUX_INITIALIZER_UNLOCKED;

String valueOrDash(String value) {
  value.trim();
  if (value == "null") {
    return "--";
  }
  return value.length() > 0 ? value : String("--");
}

String providerNameFromAddress(String address) {
  address.trim();
  const int schemeEnd = address.indexOf("://");
  if (schemeEnd >= 0) {
    address = address.substring(schemeEnd + 3);
  }

  const int pathStart = address.indexOf('/');
  if (pathStart >= 0) {
    address = address.substring(0, pathStart);
  }
  const int credentialsEnd = address.lastIndexOf('@');
  if (credentialsEnd >= 0) {
    address = address.substring(credentialsEnd + 1);
  }
  const int portStart = address.indexOf(':');
  if (portStart >= 0) {
    address = address.substring(0, portStart);
  }

  address.toLowerCase();
  const char* prefixes[] = {"www.", "web.", "cluster.", "telnet.", "dxcluster."};
  bool removedPrefix = true;
  while (removedPrefix) {
    removedPrefix = false;
    for (const char* prefix : prefixes) {
      if (address.startsWith(prefix)) {
        address = address.substring(strlen(prefix));
        removedPrefix = true;
        break;
      }
    }
  }

  const int domainEnd = address.indexOf('.');
  if (domainEnd > 0) {
    address = address.substring(0, domainEnd);
  }
  address.toUpperCase();
  return valueOrDash(address);
}

String jsonProviderName() {
  return providerNameFromAddress(getDxSpotsUrl());
}

String telnetProviderName() {
  return providerNameFromAddress(getSettings().dxTelnetHost);
}

String isoTimeToDisplay(String iso) {
  iso.trim();
  if (iso.length() >= 16 && iso[10] == 'T') {
    return iso.substring(11, 16) + " UTC";
  }
  if (iso.length() >= 16 && iso[10] == ' ') {
    return iso.substring(11, 16) + " UTC";
  }
  return valueOrDash(iso);
}

String formatSpotTime(String value) {
  value.trim();
  if (value.length() >= 5 && value[2] == ':') {
    return value.substring(0, 2) + value.substring(3, 5) + "Z";
  }
  if (value.length() >= 4) {
    return value.substring(0, 4) + "Z";
  }
  return valueOrDash(value);
}

String upperCopy(String value) {
  value.toUpperCase();
  return value;
}

bool containsModeToken(const String& comment, const char* token) {
  return comment.indexOf(token) >= 0;
}

bool frequencyIsNear(const String& freq, double targetMhz) {
  double mhz = freq.toDouble();
  if (mhz > 1000.0) {
    mhz /= 1000.0;
  }
  return fabs(mhz - targetMhz) < 0.002;
}

String formatFrequency(String value) {
  value.trim();
  if (value.length() == 0) {
    return "--";
  }

  double mhz = value.toDouble();
  if (mhz <= 0.0) {
    return valueOrDash(value);
  }
  if (mhz > 1000.0) {
    mhz /= 1000.0;
  }

  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%.3f", mhz);
  return String(buffer);
}

String deriveMode(const String& freq, const String& comment) {
  const String upperComment = upperCopy(comment);
  const char* modes[] = { "FT8", "FT4", "CW", "SSB", "USB", "LSB", "RTTY", "SSTV", "PSK" };
  for (const char* mode : modes) {
    if (containsModeToken(upperComment, mode)) {
      return String(mode);
    }
  }

  if (frequencyIsNear(freq, 7.074) || frequencyIsNear(freq, 14.074) ||
      frequencyIsNear(freq, 21.074) || frequencyIsNear(freq, 28.074)) {
    return "FT8";
  }
  if (frequencyIsNear(freq, 7.047) || frequencyIsNear(freq, 14.080) ||
      frequencyIsNear(freq, 21.080) || frequencyIsNear(freq, 28.080)) {
    return "FT4";
  }

  return "--";
}

bool beginHttp(const String& url, HTTPClient& http, WiFiClient& plainClient,
               WiFiClientSecure& secureClient) {
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    return http.begin(secureClient, url);
  }
  return http.begin(plainClient, url);
}

uint32_t refreshIntervalMs() {
  const AppSettings& settings = getSettings();
  const uint16_t minutes = constrain(settings.dxRefreshMinutes, static_cast<uint16_t>(1),
                                     static_cast<uint16_t>(120));
  return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

String currentUtcDisplay(const String& spotTime = "") {
  const time_t now = time(nullptr);
  if (now >= 1704067200) {
    tm utc;
    gmtime_r(&now, &utc);
    char buffer[16];
    strftime(buffer, sizeof(buffer), "%H:%M UTC", &utc);
    return String(buffer);
  }
  if (spotTime.length() == 5 && spotTime[4] == 'Z') {
    return spotTime.substring(0, 2) + ":" + spotTime.substring(2, 4) + " UTC";
  }
  return "--";
}

bool nextToken(const String& text, int& position, String& token, int* tokenStart = nullptr) {
  while (position < static_cast<int>(text.length()) &&
         isspace(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (position >= static_cast<int>(text.length())) {
    return false;
  }

  const int start = position;
  while (position < static_cast<int>(text.length()) &&
         !isspace(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (tokenStart != nullptr) {
    *tokenStart = start;
  }
  token = text.substring(start, position);
  return true;
}

bool isUtcTimeToken(const String& token) {
  if (token.length() != 5 || (token[4] != 'Z' && token[4] != 'z')) {
    return false;
  }
  for (uint8_t i = 0; i < 4; ++i) {
    if (!isdigit(static_cast<unsigned char>(token[i]))) {
      return false;
    }
  }
  const int hour = token.substring(0, 2).toInt();
  const int minute = token.substring(2, 4).toInt();
  return hour <= 23 && minute <= 59;
}

bool parseDxClusterSpotLine(const String& rawLine, DxSpot& out) {
  String line = rawLine;
  line.trim();
  if (!line.startsWith("DX de ")) {
    return false;
  }

  const int colon = line.indexOf(':', 6);
  if (colon < 7) {
    return false;
  }

  out = DxSpot();
  out.spotter = line.substring(6, colon);
  out.spotter.trim();
  out.spotter.toUpperCase();

  const String remainder = line.substring(colon + 1);
  int position = 0;
  String frequencyToken;
  String callToken;
  if (!nextToken(remainder, position, frequencyToken) ||
      !nextToken(remainder, position, callToken) || frequencyToken.toDouble() <= 0.0) {
    return false;
  }

  const int commentStart = position;
  int timeStart = -1;
  String timeToken;
  String token;
  int tokenStart = 0;
  while (nextToken(remainder, position, token, &tokenStart)) {
    if (isUtcTimeToken(token)) {
      timeStart = tokenStart;
      timeToken = token;
      break;
    }
  }

  out.freq = formatFrequency(frequencyToken);
  out.call = callToken;
  out.call.toUpperCase();
  out.time = timeToken.length() > 0 ? formatSpotTime(timeToken) : String("--");
  out.comment = timeStart >= 0 ? remainder.substring(commentStart, timeStart)
                               : remainder.substring(commentStart);
  out.comment.trim();
  out.comment = valueOrDash(out.comment);
  out.mode = deriveMode(out.freq, out.comment);
  out.band = "--";
  out.country = "--";
  out.continent = "--";
  return out.spotter.length() > 0 && out.freq != "--" && out.call.length() > 0;
}

bool isDuplicateDxSpot(const DxSpot& spot) {
  for (uint8_t i = 0; i < g_data.spotCount; ++i) {
    const DxSpot& existing = g_data.spots[i];
    if (existing.call.equalsIgnoreCase(spot.call) && existing.freq == spot.freq &&
        existing.time == spot.time) {
      return true;
    }
  }
  return false;
}

void setTelnetStatus(const String& status) {
  g_data.status = status;
  g_data.provider = telnetProviderName();
  if (g_telnetHasCurrentSpots) {
    g_data.source = "Telnet";
  } else if (g_data.hasData) {
    g_data.source = "Last good";
  } else {
    g_data.source = "Telnet";
  }
}

void stopDxTelnet(bool resetReconnectTimer) {
  if (g_telnetConnected || g_telnetClient.connected()) {
    Serial.println("DX Telnet disconnected");
  }

  bool stopClient = false;
  portENTER_CRITICAL(&g_telnetConnectMux);
  if (g_telnetConnectState == kTelnetConnectRunning) {
    g_telnetConnectCancelRequested = true;
  } else {
    g_telnetConnectState = kTelnetConnectIdle;
    stopClient = true;
  }
  portEXIT_CRITICAL(&g_telnetConnectMux);
  if (stopClient) {
    g_telnetClient.stop();
  }
  g_telnetConnected = false;
  g_telnetLoggedIn = false;
  g_telnetLineBuffer = "";
  g_telnetLineOverflow = false;
  if (resetReconnectTimer) {
    g_lastTelnetConnectAttemptMs = 0;
  }
}

void sendDxTelnetLoginIfNeeded() {
  if (!g_telnetConnected || g_telnetLoggedIn || !g_telnetClient.connected()) {
    return;
  }

  String callsign = getSettings().callsign;
  callsign.trim();
  callsign.toUpperCase();
  if (callsign.length() == 0) {
    callsign = "NOCALL"; // Safe placeholder when the station callsign has not been configured.
  }
  g_telnetClient.println(callsign);
  g_telnetLoggedIn = true;
  setTelnetStatus("Login sent");
  Serial.print("DX Telnet login sent: ");
  Serial.println(callsign);
}

bool addDxSpotToList(const DxSpot& spot) {
  if (isDuplicateDxSpot(spot)) {
#if DEBUG_DX_TELNET
    Serial.println("DX Telnet duplicate ignored");
#endif
    return false;
  }

  if (!g_telnetHasCurrentSpots) {
    g_data = DxSpotsData();
    g_telnetHasCurrentSpots = true;
  }

  const uint8_t last = min<uint8_t>(g_data.spotCount, kMaxDxSpots - 1);
  for (uint8_t i = last; i > 0; --i) {
    g_data.spots[i] = g_data.spots[i - 1];
  }
  g_data.spots[0] = spot;
  if (g_data.spotCount < kMaxDxSpots) {
    ++g_data.spotCount;
  }
  g_data.hasData = true;
  g_data.source = "Telnet";
  g_data.provider = telnetProviderName();
  g_data.status = "Reading";
  g_data.updated = currentUtcDisplay(spot.time);
  Serial.print("DX Telnet spot: ");
  Serial.print(spot.freq);
  Serial.print(" ");
  Serial.print(spot.call);
  Serial.print(" ");
  Serial.println(spot.mode);
  return true;
}

bool handleDxTelnetLine(const String& line) {
#if DEBUG_DX_TELNET
  Serial.print("DX Telnet line: ");
  Serial.println(line);
#endif

  String lower = line;
  lower.toLowerCase();
  if (!g_telnetLoggedIn &&
      (lower.indexOf("login:") >= 0 || lower.indexOf("callsign:") >= 0 ||
       lower.indexOf("call:") >= 0 || lower.indexOf("please enter your call") >= 0)) {
    sendDxTelnetLoginIfNeeded();
    return true;
  }

  DxSpot spot;
  if (parseDxClusterSpotLine(line, spot)) {
    return addDxSpotToList(spot);
  }

#if DEBUG_DX_TELNET
  if (line.startsWith("DX de ")) {
    Serial.println("DX Telnet spot parse failed");
  }
#endif
  if (g_telnetLoggedIn && g_data.status != "Reading") {
    setTelnetStatus("Reading");
    return true;
  }
  return false;
}

void dxTelnetConnectTask(void*) {
  WiFiClient pendingClient;
  const bool connected = pendingClient.connect(g_telnetConnectRequest.host,
                                                g_telnetConnectRequest.port,
                                                kTelnetConnectTimeoutMs);

  bool cancelled = false;
  portENTER_CRITICAL(&g_telnetConnectMux);
  if (g_telnetConnectCancelRequested) {
    cancelled = true;
    g_telnetConnectState = kTelnetConnectCancelled;
  } else if (connected) {
    g_telnetClient = pendingClient;
    g_telnetConnectState = kTelnetConnectSucceeded;
  } else {
    g_telnetConnectState = kTelnetConnectFailed;
  }
  portEXIT_CRITICAL(&g_telnetConnectMux);
  if (cancelled) {
    pendingClient.stop();
  }
  vTaskDelete(nullptr);
}

bool connectDxTelnet() {
  const AppSettings& settings = getSettings();
  portENTER_CRITICAL(&g_telnetConnectMux);
  if (g_telnetConnectState == kTelnetConnectRunning) {
    portEXIT_CRITICAL(&g_telnetConnectMux);
    return false;
  }
  strlcpy(g_telnetConnectRequest.host, settings.dxTelnetHost.c_str(),
          sizeof(g_telnetConnectRequest.host));
  g_telnetConnectRequest.port = settings.dxTelnetPort;
  g_telnetConnectCancelRequested = false;
  g_telnetConnectState = kTelnetConnectRunning;
  portEXIT_CRITICAL(&g_telnetConnectMux);

  g_lastTelnetConnectAttemptMs = millis();
  setTelnetStatus("Connecting");
  Serial.print("DX Telnet connect attempt: ");
  Serial.print(settings.dxTelnetHost);
  Serial.print(":");
  Serial.println(settings.dxTelnetPort);

  if (xTaskCreate(dxTelnetConnectTask, "dx-telnet-connect", 4096, nullptr, 1, nullptr) != pdPASS) {
    portENTER_CRITICAL(&g_telnetConnectMux);
    g_telnetConnectState = kTelnetConnectIdle;
    portEXIT_CRITICAL(&g_telnetConnectMux);
    setTelnetStatus("Failed");
    Serial.println("DX Telnet connection task failed");
  }
  return true;
}

bool pollDxTelnetConnect() {
  TelnetConnectState state;
  portENTER_CRITICAL(&g_telnetConnectMux);
  state = g_telnetConnectState;
  if (state == kTelnetConnectSucceeded || state == kTelnetConnectFailed ||
      state == kTelnetConnectCancelled) {
    g_telnetConnectState = kTelnetConnectIdle;
  }
  portEXIT_CRITICAL(&g_telnetConnectMux);

  if (state == kTelnetConnectSucceeded) {
    g_telnetConnected = true;
    g_telnetLoggedIn = false;
    g_telnetConnectedAtMs = millis();
    g_lastTelnetDataMs = g_telnetConnectedAtMs;
    g_telnetLineBuffer = "";
    g_telnetLineOverflow = false;
    setTelnetStatus("Connected");
    Serial.println("DX Telnet connected");
    return true;
  }
  if (state == kTelnetConnectFailed) {
    g_telnetConnected = false;
    setTelnetStatus("Failed");
    Serial.println("DX Telnet connection failed");
    return true;
  }
  return false;
}

bool loopDxTelnet() {
  bool changed = pollDxTelnetConnect();
  const uint32_t nowMs = millis();

  if (g_telnetConnected && !g_telnetClient.connected()) {
    stopDxTelnet(false);
    g_telnetHasCurrentSpots = false;
    g_lastTelnetConnectAttemptMs = nowMs;
    setTelnetStatus("Disconnected");
    changed = true;
  }

  if (!g_telnetConnected) {
    if (g_lastTelnetConnectAttemptMs == 0 ||
        nowMs - g_lastTelnetConnectAttemptMs >= kTelnetReconnectIntervalMs) {
      changed |= connectDxTelnet();
    }
    return changed;
  }

  size_t bytesRead = 0;
  while (g_telnetClient.available() > 0 && bytesRead < kMaxTelnetBytesPerLoop) {
    const int incoming = g_telnetClient.read();
    if (incoming < 0) {
      break;
    }
    ++bytesRead;
    g_lastTelnetDataMs = nowMs;
    const char c = static_cast<char>(incoming);
    if (c == '\n') {
      if (!g_telnetLineOverflow && g_telnetLineBuffer.length() > 0) {
        changed |= handleDxTelnetLine(g_telnetLineBuffer);
      }
      g_telnetLineBuffer = "";
      g_telnetLineOverflow = false;
    } else if (c != '\r' && static_cast<uint8_t>(c) >= 32 && static_cast<uint8_t>(c) < 127) {
      if (g_telnetLineBuffer.length() < kMaxTelnetLineChars) {
        g_telnetLineBuffer += c;
      } else {
        g_telnetLineOverflow = true;
      }
    }
  }

  if (!g_telnetLoggedIn && nowMs - g_telnetConnectedAtMs >= kTelnetLoginDelayMs) {
    sendDxTelnetLoginIfNeeded();
    changed = true;
  }
  return changed;
}

void markDxFailure(const String& status, const String& attemptedSource,
                   const String& attemptedProvider) {
  g_data.status = status;
  g_data.source = g_data.hasData ? String("Last good") : attemptedSource;
  if (!g_data.hasData) {
    g_data.provider = attemptedProvider;
  }
}

bool readDxArrayPrefix(Stream& stream, String& arrayJson) {
  arrayJson = "[";
  bool foundArray = false;
  bool readingObject = false;
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  uint8_t objectsRead = 0;
  const uint32_t startMs = millis();

  while (millis() - startMs < kHttpTimeoutMs) {
    while (stream.available() > 0) {
      const char c = static_cast<char>(stream.read());

      if (!foundArray) {
        if (c == '[') {
          foundArray = true;
        }
        continue;
      }

      if (!readingObject) {
        if (c == '{') {
          readingObject = true;
          depth = 1;
          if (objectsRead > 0) {
            arrayJson += ',';
          }
          arrayJson += "{";
        } else if (c == ']') {
          arrayJson += "]";
          return objectsRead > 0;
        }
        continue;
      }

      if (arrayJson.length() >= kMaxDxObjectChars * kMaxDxSpots) {
        Serial.println("DX JSON parse result: trimmed array too large");
        return false;
      }
      arrayJson += c;

      if (inString) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          inString = false;
        }
        continue;
      }

      if (c == '"') {
        inString = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
        if (depth == 0) {
          ++objectsRead;
          readingObject = false;
          if (objectsRead >= kMaxDxSpots) {
            arrayJson += "]";
            Serial.print("DX spot objects buffered: ");
            Serial.println(objectsRead);
            return true;
          }
        }
      }
    }
    delay(1);
  }

  Serial.println("DX JSON parse result: stream timeout");
  return false;
}

bool parseIz3mezJsonArray(const String& arrayJson, DxSpotsData& parsed) {
  DynamicJsonDocument doc(24576);
  DeserializationError error = deserializeJson(doc, arrayJson);
  if (error) {
    Serial.print("DX JSON parse result: ");
    Serial.println(error.c_str());
    Serial.print("DX buffered JSON length: ");
    Serial.println(arrayJson.length());
    return false;
  }
  if (!doc.is<JsonArray>()) {
    Serial.println("DX JSON parse result: expected buffered array");
    return false;
  }

  parsed = DxSpotsData();
  JsonArray spots = doc.as<JsonArray>();
  for (JsonObject spotObject : spots) {
    if (parsed.spotCount >= kMaxDxSpots) {
      break;
    }

    DxSpot& spot = parsed.spots[parsed.spotCount];
    spot.time = formatSpotTime(spotObject["spot_time"].as<String>());
    spot.freq = formatFrequency(spotObject["frequency"].as<String>());
    spot.call = valueOrDash(spotObject["spotted"].as<String>());
    spot.spotter = valueOrDash(spotObject["spotter"].as<String>());
    spot.comment = valueOrDash(spotObject["spotter_comment"].as<String>());
    spot.band = valueOrDash(spotObject["band"].as<String>());
    spot.country = valueOrDash(spotObject["spotted_country"].as<String>());
    spot.continent = valueOrDash(spotObject["spotted_continent"].as<String>());
    spot.mode = deriveMode(spot.freq, spot.comment);

    if (spot.freq != "--" && spot.call != "--") {
      if (parsed.updated == "") {
        parsed.updated = isoTimeToDisplay(spotObject["spot_datetime"].as<String>());
      }
      ++parsed.spotCount;
    }
  }

  parsed.updated = valueOrDash(parsed.updated);
  parsed.source = "JSON";
  parsed.provider = jsonProviderName();
  parsed.hasData = parsed.spotCount > 0;
  parsed.status = "OK";
  Serial.println("DX JSON parse result: OK");
  Serial.print("DX buffered JSON length: ");
  Serial.println(arrayJson.length());
  Serial.print("DX spots parsed: ");
  Serial.println(parsed.spotCount);
  return parsed.hasData;
}

bool fetchDxSpots() {
  const String url = getDxSpotsUrl();
  if (url.length() == 0) {
    Serial.println("DX fetch skipped: DX URL not set");
    markDxFailure("DX URL not set", "JSON", jsonProviderName());
    return false;
  }

  Serial.println("DX fetch start");
  Serial.print("DX URL used: ");
  Serial.println(url);

  HTTPClient http;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  if (!beginHttp(url, http, plainClient, secureClient)) {
    Serial.println("DX fetch failure reason: http.begin");
    markDxFailure("Fetch failed", "JSON", jsonProviderName());
    return false;
  }

  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int httpCode = http.GET();
  Serial.print("DX HTTP status code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    http.end();
    Serial.println("DX fetch failure reason: HTTP status");
    markDxFailure("Fetch failed", "JSON", jsonProviderName());
    return false;
  }

  String arrayJson;
  arrayJson.reserve(kMaxDxObjectChars * 3);
  const bool gotArray = readDxArrayPrefix(http.getStream(), arrayJson);
  // Free the TLS connection's own buffers before asking the allocator for a
  // large contiguous JsonDocument block: holding both at once is what was
  // pushing the parse into "NoMemory" under heap fragmentation.
  http.end();

  if (!gotArray) {
    Serial.println("DX JSON parse result: no buffered spots");
    markDxFailure("Parse failed", "JSON", jsonProviderName());
    return false;
  }

  DxSpotsData parsed;
  if (!parseIz3mezJsonArray(arrayJson, parsed)) {
    markDxFailure("Parse failed", "JSON", jsonProviderName());
    return false;
  }

  g_data = parsed;
  Serial.print("DX status message: ");
  Serial.println(g_data.status);
  return true;
}
}

void dxSpotsBegin() {
  g_data = DxSpotsData();
  g_data.status = "Waiting";
  g_data.updated = "--";
  g_data.source = "--";
  g_data.provider = "--";
  g_telnetLineBuffer.reserve(kMaxTelnetLineChars);
  g_lastSourceMode = 0xFF;
  g_autoUsingTelnet = false;
  g_telnetHasCurrentSpots = false;
  stopDxTelnet(true);
  g_refreshRequested = true;
}

String getDxSpotsUrl() {
  String url = getSettings().dxSpotsUrl;
  url.trim();
  if (url.length() == 0) {
    url = DEFAULT_DX_SPOTS_URL;
    url.trim();
  }
  return url;
}

bool refreshDxSpotsIfNeeded(bool wifiConnected) {
  const uint32_t nowMs = millis();
  const uint32_t intervalMs = refreshIntervalMs();
  const bool due = g_lastAttemptMs == 0 || nowMs - g_lastAttemptMs >= intervalMs;
  const DxSourceMode mode = getSettings().dxSourceMode;
  bool changed = false;

  if (g_lastSourceMode != static_cast<uint8_t>(mode)) {
    stopDxTelnet(true);
    g_telnetHasCurrentSpots = false;
    g_autoUsingTelnet = false;
    g_lastAttemptMs = 0;
    g_refreshRequested = true;
    g_lastSourceMode = static_cast<uint8_t>(mode);
    changed = true;
  }

  if (!wifiConnected) {
    stopDxTelnet(true);
    g_telnetHasCurrentSpots = false;
    g_lastAttemptMs = nowMs - intervalMs + 5000UL;
    g_refreshRequested = false;
    const bool telnetOnly = mode == kDxSourceTelnet;
    markDxFailure("WiFi offline", telnetOnly ? String("Telnet") : String("JSON"),
                  telnetOnly ? telnetProviderName() : jsonProviderName());
    return true;
  }

  if (mode == kDxSourceJson) {
    stopDxTelnet(false);
    if (!g_refreshRequested && !due) {
      return changed;
    }
    g_lastAttemptMs = nowMs;
    g_refreshRequested = false;
    return fetchDxSpots() || changed;
  }

  if (mode == kDxSourceTelnet) {
    if (g_refreshRequested) {
      stopDxTelnet(true);
      g_telnetHasCurrentSpots = false;
      setTelnetStatus("Disconnected");
      g_refreshRequested = false;
      changed = true;
    }
    return loopDxTelnet() || changed;
  }

  if (g_refreshRequested || due) {
    const bool telnetWasActive = g_autoUsingTelnet && g_telnetConnected;
    // A manual refresh tap re-tries JSON in case it's back, but if Telnet is
    // already connected and working it should be left alone: tearing it down
    // here only to reconnect from scratch means it never gets an
    // uninterrupted window to actually receive spots from the cluster.
    const bool reconnectFallback = g_refreshRequested && !telnetWasActive;
    g_lastAttemptMs = nowMs;
    g_refreshRequested = false;
    if (fetchDxSpots()) {
      g_autoUsingTelnet = false;
      g_telnetHasCurrentSpots = false;
      stopDxTelnet(true);
      return true;
    }

    g_autoUsingTelnet = true;
    if (reconnectFallback) {
      stopDxTelnet(true);
      g_telnetHasCurrentSpots = false;
    } else if (telnetWasActive) {
      setTelnetStatus("Reading");
    }
    changed = true;
  }

  if (g_autoUsingTelnet) {
    changed |= loopDxTelnet();
  }
  return changed;
}

void requestDxSpotsRefresh() {
  g_refreshRequested = true;
}

const DxSpotsData& getDxSpotsData() {
  return g_data;
}
