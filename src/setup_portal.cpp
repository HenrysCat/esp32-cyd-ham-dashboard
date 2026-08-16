#include "setup_portal.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "connectivity.h"
#include "dashboard_display.h"
#include "dx_spots.h"
#include "greyline.h"
#include "pota_spots.h"
#include "propagation.h"
#include "psk_reporter.h"
#include "settings.h"

namespace {
constexpr byte kDnsPort = 53;
constexpr char kApSsid[] = "CYD-HamClock-Setup";
constexpr char kApPassword[] = "hamclock";

constexpr uint32_t kApAutoOffConfirmMs = 8000;

DNSServer dnsServer;
WebServer server(80);
bool portalStarted = false;
bool mdnsStarted = false;
bool pendingWifiReconnect = false;
uint32_t pendingWifiReconnectAtMs = 0;
bool pendingReboot = false;
uint32_t pendingRebootAtMs = 0;
bool hotspotActive = false;
uint32_t staConfirmedSinceMs = 0;

void startHotspot() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kApSsid, kApPassword);
  delay(100);
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  hotspotActive = true;
  staConfirmedSinceMs = 0;
  Serial.println("Setup hotspot is on");
}

void stopHotspot() {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  hotspotActive = false;
  Serial.println("Setup hotspot turned off (Wi-Fi connection confirmed)");
}

String htmlEscape(const String& input) {
  String out;
  out.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

String boolSelected(bool selected) {
  return selected ? F(" selected") : F("");
}

String checked(bool value) {
  return value ? F(" checked") : F("");
}

String limitedArg(const char* name, size_t maxLen, bool trimWhitespace = true) {
  String value = server.arg(name);
  if (trimWhitespace) {
    value.trim();
  }
  if (value.length() > maxLen) {
    value = value.substring(0, maxLen);
  }
  return value;
}

bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += F("\\n");
    } else if (c == '\r') {
      out += F("\\r");
    } else {
      out += c;
    }
  }
  return out;
}

String uptimeText(uint32_t seconds) {
  char buffer[18];
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  const uint32_t secs = seconds % 60;
  snprintf(buffer, sizeof(buffer), "%lu:%02lu:%02lu",
           static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(secs));
  return String(buffer);
}

String statusJson() {
  const ClockSnapshot snapshot = getClockSnapshot();
  const PropagationData& propagation = getPropagationData();
  const DxSpotsData& dx = getDxSpotsData();

  String json;
  json.reserve(520);
  json += F("{\"project\":\"CYD HamClock\",");
  json += F("\"wifi\":");
  json += snapshot.wifiConnected ? F("true") : F("false");
  json += F(",\"ip\":\"");
  json += snapshot.wifiConnected ? WiFi.localIP().toString() : String("");
  json += F("\",\"uptime\":\"");
  json += uptimeText(snapshot.uptimeSeconds);
  json += F("\",\"free_heap\":");
  json += String(ESP.getFreeHeap());
  // Largest single block available. Free heap can look healthy while this has
  // fallen below what a sprite needs, which is what starves the scroll
  // animation, so it is worth being able to see the two apart.
  json += F(",\"max_alloc\":");
  json += String(ESP.getMaxAllocHeap());
  json += F(",\"ntp\":");
  json += snapshot.timeValid ? F("true") : F("false");
  json += F(",\"page\":");
  json += String(getCurrentDashboardPageNumber());
  json += F(",\"propagation_status\":\"");
  json += jsonEscape(propagation.status);
  json += F("\",\"dx_status\":\"");
  json += jsonEscape(dx.status);
  json += F("\",\"dx_source\":\"");
  json += jsonEscape(dx.source);
  json += F("\"}");
  return json;
}

String pageHtml(const String& message = "") {
  const AppSettings& settings = getSettings();
  const ClockSnapshot snapshot = getClockSnapshot();
  const PropagationData& propagation = getPropagationData();
  const DxSpotsData& dx = getDxSpotsData();

  String html;
  html.reserve(12288);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>CYD HamClock Settings</title><style>");
  html += F("body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#10151c;color:#f3f7fb}");
  html += F("main{max-width:760px;margin:0 auto;padding:24px}label{display:block;margin:14px 0 6px;color:#aeb8c4}");
  html += F("input,select{box-sizing:border-box;width:100%;padding:11px;border-radius:6px;border:1px solid #3a4653;background:#18212b;color:#fff;font-size:16px}input[type=checkbox]{width:auto;margin-right:8px}");
  html += F("button{margin-top:18px;margin-right:8px;padding:12px 16px;border:0;border-radius:6px;background:#1aa7c8;color:#001018;font-weight:700;font-size:16px}");
  html += F(".danger{background:#ffbd66}.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 16px}.card{border:1px solid #293440;border-radius:8px;padding:16px;margin:16px 0;background:#141b24}.ok{color:#71e58d}.warn{color:#ffbd66}");
  html += F("small{color:#aeb8c4}code{background:#202b36;padding:2px 5px;border-radius:4px}</style></head><body><main>");
  html += F("<h1>CYD HamClock Settings</h1>");

  if (message.length() > 0) {
    html += F("<div class='card ok'>");
    html += htmlEscape(message);
    html += F("</div>");
  }

  html += F("<div class='card'>");
  html += F("<div>Wi-Fi: <strong class='");
  html += snapshot.wifiConnected ? F("ok'>connected") : F("warn'>not connected");
  html += F("</strong></div><div>NTP: <strong class='");
  html += snapshot.timeValid ? F("ok'>synced") : F("warn'>waiting");
  html += F("</strong></div><div>LAN IP: <code>");
  html += snapshot.wifiConnected ? WiFi.localIP().toString() : String("--");
  html += F("</code></div><div>mDNS: <code>");
  html += mdnsStarted ? F("cyd-ham.local") : F("--");
  html += F("</code></div><div>Uptime: <code>");
  html += uptimeText(snapshot.uptimeSeconds);
  html += F("</code></div><div>Free heap: <code>");
  html += String(ESP.getFreeHeap());
  html += F("</code></div><div>Current page: <code>");
  html += String(getCurrentDashboardPageNumber());
  html += F("/");
  html += String(kDashboardPageCount);
  html += F("</code></div><div>Propagation: <code>");
  html += htmlEscape(propagation.status);
  html += F("</code></div><div>DX: <code>");
  html += htmlEscape(dx.source);
  html += F(" / ");
  html += htmlEscape(dx.status);
  html += F("</code></div><div>Setup hotspot: <strong class='");
  html += hotspotActive ? F("warn'>on") : F("ok'>off");
  html += F("</strong></div><div>Setup AP: <code>");
  html += kApSsid;
  html += F("</code>, password <code>");
  html += kApPassword;
  html += F("</code></div><div>Portal IP: <code>");
  html += hotspotActive ? WiFi.softAPIP().toString() : String("--");
  html += F("</code></div></div>");

  html += F("<form method='post' action='/save'><div class='card'><h2>Station</h2>");
  html += F("<label for='callsign'>Callsign</label><input id='callsign' name='callsign' maxlength='16' value='");
  html += htmlEscape(settings.callsign);
  html += F("'><small>This callsign is also used to log in to a Telnet DX Cluster. If blank, <code>NOCALL</code> is used.</small>");
  html += F("<label for='ssid'>Wi-Fi SSID</label><input id='ssid' name='ssid' value='");
  html += htmlEscape(settings.wifiSsid);
  html += F("' autocomplete='off'>");
  html += F("<label for='pass'>Wi-Fi password</label><input id='pass' name='pass' type='password' value='' maxlength='64' autocomplete='new-password' placeholder='Leave blank to keep saved password'>");
  html += F("<label><input name='clearpass' type='checkbox' value='1'>Clear the saved password (for an open network)</label>");
  html += F("<label><input name='keepap' type='checkbox' value='1'");
  html += checked(settings.keepHotspotOn);
  html += F(">Keep the <code>");
  html += kApSsid;
  html += F("</code> setup hotspot switched on</label><small>By default this hotspot switches off a few seconds after the device confirms it has joined your Wi-Fi network. Check this box to leave it running (for example, to reach the settings page again without your router).</small>");
  html += F("</div><div class='card'><h2>Time and Location</h2>");
  html += F("<label for='tzpreset'>Timezone preset</label><select id='tzpreset' onchange='applyPreset(this.value)'>");
  html += F("<option value='CUSTOM'>Custom POSIX TZ</option>");
  html += F("<option value='UTC'>UTC</option>");
  html += F("<option value='UK'>United Kingdom GMT/BST</option>");
  html += F("<option value='IE'>Ireland GMT/IST</option>");
  html += F("<option value='EU_CENTRAL'>Central Europe CET/CEST</option>");
  html += F("<option value='EU_EASTERN'>Eastern Europe EET/EEST</option>");
  html += F("<option value='US_EASTERN'>US Eastern</option>");
  html += F("<option value='US_CENTRAL'>US Central</option>");
  html += F("<option value='US_MOUNTAIN'>US Mountain</option>");
  html += F("<option value='US_PACIFIC'>US Pacific</option>");
  html += F("<option value='CA_ATLANTIC'>Canada Atlantic</option>");
  html += F("<option value='AU_EASTERN'>Australia Eastern</option>");
  html += F("<option value='AU_CENTRAL'>Australia Central</option>");
  html += F("<option value='AU_WESTERN'>Australia Western</option>");
  html += F("<option value='NZ'>New Zealand</option>");
  html += F("<option value='JP'>Japan</option>");
  html += F("<option value='CN'>China</option>");
  html += F("<option value='IN'>India</option>");
  html += F("<option value='BR_EAST'>Brazil East</option>");
  html += F("<option value='ZA'>South Africa</option></select>");
  html += F("<label for='tzlabel'>Timezone label</label><input id='tzlabel' name='tzlabel' value='");
  html += htmlEscape(settings.timezoneLabel);
  html += F("'>");
  html += F("<label for='tz'>POSIX timezone rule</label><input id='tz' name='tz' oninput='syncPreset()' value='");
  html += htmlEscape(settings.timezone);
  html += F("'><small>UK default: <code>GMT0BST-1,M3.5.0/1,M10.5.0/2</code></small>");
  html += F("<label for='locator'>Maidenhead locator</label><input id='locator' name='locator' maxlength='6' pattern='[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?' value='");
  html += htmlEscape(settings.locator);
  html += F("'><small>Use a four- or six-character Maidenhead locator. Changes are applied immediately.</small>");
  html += F("<label><input name='clock12' type='checkbox' value='1'");
  html += checked(settings.clock12Hour);
  html += F(">Show the local clock as 12-hour with AM/PM</label><small>Applies to the local time on the clock page. The UTC readout above it stays 24-hour.</small>");
  html += F("</div><div class='card'><h2>Data Sources</h2>");
  html += F("<label for='propmode'>Propagation source mode</label><select id='propmode' name='propmode'>");
  html += F("<option value='direct'");
  html += boolSelected(!settings.useJsonPropagationProxy);
  html += F(">Direct HamQSL XML</option><option value='json'");
  html += boolSelected(settings.useJsonPropagationProxy);
  html += F(">JSON proxy URL</option></select>");
  html += F("<small>Direct HamQSL XML: <code>https://www.hamqsl.com/solarxml.php</code></small>");
  html += F("<label for='propurl'>Propagation JSON URL</label><input id='propurl' name='propurl' maxlength='180' value='");
  html += htmlEscape(settings.propagationJsonUrl);
  html += F("'>");
  html += F("<label for='dxmode'>DX source mode</label><select id='dxmode' name='dxmode'>");
  html += F("<option value='auto'");
  html += boolSelected(settings.dxSourceMode == kDxSourceAuto);
  html += F(">Auto (JSON then Telnet)</option><option value='json'");
  html += boolSelected(settings.dxSourceMode == kDxSourceJson);
  html += F(">JSON only</option><option value='telnet'");
  html += boolSelected(settings.dxSourceMode == kDxSourceTelnet);
  html += F(">Telnet only</option></select>");
  html += F("<label for='dxurl'>DX JSON URL</label><input id='dxurl' name='dxurl' maxlength='180' value='");
  html += htmlEscape(settings.dxSpotsUrl);
  html += F("'>");
  html += F("<div class='grid'><div><label for='dxhost'>DX Telnet host</label><input id='dxhost' name='dxhost' maxlength='64' value='");
  html += htmlEscape(settings.dxTelnetHost);
  html += F("'></div><div><label for='dxport'>DX Telnet port</label><input id='dxport' name='dxport' type='number' min='1' max='65535' value='");
  html += String(settings.dxTelnetPort);
  html += F("'></div></div>");
  html += F("<div class='grid'><div><label for='propmins'>Propagation refresh minutes</label><input id='propmins' name='propmins' type='number' min='1' max='120' value='");
  html += String(settings.propagationRefreshMinutes);
  html += F("'></div><div><label for='dxmins'>DX refresh minutes</label><input id='dxmins' name='dxmins' type='number' min='1' max='120' value='");
  html += String(settings.dxRefreshMinutes);
  html += F("'></div></div></div><div class='card'><h2>PSKReporter</h2>");
  html += F("<small>Plots reception reports for your callsign on the world map. Uses the callsign set above; leave it blank to switch this page off.</small>");
  html += F("<label for='pskdir'>Direction</label><select id='pskdir' name='pskdir'>");
  html += F("<option value='heard'");
  html += boolSelected(settings.pskDirection == kPskWhoHearsMe);
  html += F(">Who is hearing me</option><option value='hearing'");
  html += boolSelected(settings.pskDirection == kPskWhoIHear);
  html += F(">Who I am hearing</option></select>");
  html += F("<div class='grid'><div><label for='pskwin'>Report window minutes</label><input id='pskwin' name='pskwin' type='number' min='5' max='360' value='");
  html += String(settings.pskWindowMinutes);
  html += F("'></div><div><label for='pskmins'>PSKReporter refresh minutes</label><input id='pskmins' name='pskmins' type='number' min='5' max='120' value='");
  html += String(settings.pskRefreshMinutes);
  html += F("'></div></div><small>PSKReporter asks that reception data is retrieved no more than once every five minutes, so five is the lowest value accepted here.</small>");
  html += F("<label for='pskmail'>Contact email (optional)</label><input id='pskmail' name='pskmail' maxlength='64' value='");
  html += htmlEscape(settings.pskAppContact);
  html += F("'><small>Sent to PSKReporter as <code>appcontact</code> so they can get in touch before rate limiting this device.</small>");
  html += F("</div><div class='card'><h2>POTA</h2>");
  html += F("<small>Live Parks on the Air activator spots from <code>api.pota.app</code>.</small>");
  html += F("<div class='grid'><div><label for='potadist'>Max distance km</label><input id='potadist' name='potadist' type='number' min='0' max='20000' value='");
  html += String(settings.potaMaxDistanceKm);
  html += F("'><small>Great-circle distance from your locator. Use <code>0</code> for no limit.</small></div>");
  html += F("<div><label for='potamins'>POTA refresh minutes</label><input id='potamins' name='potamins' type='number' min='1' max='120' value='");
  html += String(settings.potaRefreshMinutes);
  html += F("'></div></div>");
  html += F("<label><input name='potarbn' type='checkbox' value='1'");
  html += checked(settings.potaExcludeRbn);
  html += F(">Hide RBN spots</label><small>RBN spots are posted automatically by skimmers rather than by a person. Hiding them leaves only human-posted spots.</small>");
  html += F("</div><div class='card'><h2>Automatic Page Change</h2>");
  html += F("<label><input name='autopage' type='checkbox' value='1'");
  html += checked(settings.autoPageChange);
  html += F(">Change pages automatically</label>");
  html += F("<label for='autosecs'>Seconds on each page</label><input id='autosecs' name='autosecs' type='number' min='3' max='600' value='");
  html += String(settings.autoPageSeconds);
  html += F("'>");
  html += F("<label>Pages included</label>");
  for (uint8_t i = 0; i < kDashboardPageCount; ++i) {
    html += F("<label><input name='pg");
    html += String(i);
    html += F("' type='checkbox' value='1'");
    html += checked((settings.autoPageMask & (1u << i)) != 0);
    html += F(">");
    html += String(i + 1);
    html += F(". ");
    html += dashboardPageName(i);
    html += F("</label>");
  }
  html += F("<small>Unticked pages are skipped by the automatic change; tapping the left or right edge of the screen still reaches them. Any tap restarts the countdown, so a page being read is not pulled away.</small>");
  html += F("</div><div class='card'><h2>Display</h2>");
  html += F("<label for='bright'>Backlight brightness percent</label><input id='bright' name='bright' type='number' min='5' max='100' value='");
  html += String(settings.brightnessPercent);
  html += F("'><label><input name='swaprb' type='checkbox' value='1'");
  if (settings.swapRedBlueChannels) {
    html += F(" checked");
  }
  html += F(">Swap red/blue display channels</label><small>Enable this only when red appears blue and yellow appears cyan. It is applied immediately and saved for this board.</small>");
  html += F("<label><input name='rot90' type='checkbox' value='1'");
  if (settings.rotate90) {
    html += F(" checked");
  }
  html += F(">Rotate display 90&deg;</label><small>Enable this if the screen shows portrait and cropped on first start.</small>");
  html += F("<label><input name='flip180' type='checkbox' value='1'");
  if (settings.flip180) {
    html += F(" checked");
  }
  html += F(">Flip display 180&deg;</label><small>Enable this if the screen is upside down.</small>");
  html += F("<label><input name='mirror' type='checkbox' value='1'");
  if (settings.mirror) {
    html += F(" checked");
  }
  html += F(">Mirror display</label><small>Enable this if the screen shows text and images left-right reversed, as on some CYD panel variants.</small>");
  html += F("<label><input name='invert' type='checkbox' value='1'");
  if (settings.invertColours) {
    html += F(" checked");
  }
  html += F(">Invert display colours</label><small>Enable this if colours appear as their negative/inverse, as on some CYD panel variants.</small>");
  html += F("<label><input name='touchswap' type='checkbox' value='1'");
  if (settings.swapTouchNav) {
    html += F(" checked");
  }
  html += F(">Swap left/right page navigation</label><small>Enable this if tapping the left/right edge of the screen changes pages in the wrong direction, as on some touch controller variants.</small></div>");
  html += F("<button type='submit'>Save settings</button></form>");
  html += F("<form method='post' action='/reboot'><button class='danger' type='submit'>Restart device</button></form>");
  html += F("<p><small>This page is intended for trusted LAN use only. No admin password is configured in this project.</small></p>");
  html += F("<script>");
  html += F("const presets={");
  html += F("UTC:['UTC0','UTC'],");
  html += F("UK:['GMT0BST-1,M3.5.0/1,M10.5.0/2','UK local'],");
  html += F("IE:['IST-1GMT0,M10.5.0,M3.5.0/1','Ireland local'],");
  html += F("EU_CENTRAL:['CET-1CEST-2,M3.5.0/2,M10.5.0/3','Central Europe'],");
  html += F("EU_EASTERN:['EET-2EEST-3,M3.5.0/3,M10.5.0/4','Eastern Europe'],");
  html += F("US_EASTERN:['EST5EDT,M3.2.0/2,M11.1.0/2','US Eastern'],");
  html += F("US_CENTRAL:['CST6CDT,M3.2.0/2,M11.1.0/2','US Central'],");
  html += F("US_MOUNTAIN:['MST7MDT,M3.2.0/2,M11.1.0/2','US Mountain'],");
  html += F("US_PACIFIC:['PST8PDT,M3.2.0/2,M11.1.0/2','US Pacific'],");
  html += F("CA_ATLANTIC:['AST4ADT,M3.2.0/2,M11.1.0/2','Canada Atlantic'],");
  html += F("AU_EASTERN:['AEST-10AEDT-11,M10.1.0/2,M4.1.0/3','Australia East'],");
  html += F("AU_CENTRAL:['ACST-9:30ACDT-10:30,M10.1.0/2,M4.1.0/3','Australia Central'],");
  html += F("AU_WESTERN:['AWST-8','Australia West'],");
  html += F("NZ:['NZST-12NZDT-13,M9.5.0/2,M4.1.0/3','New Zealand'],");
  html += F("JP:['JST-9','Japan'],CN:['CST-8','China'],IN:['IST-5:30','India'],");
  html += F("BR_EAST:['BRT3','Brazil East'],ZA:['SAST-2','South Africa']};");
  html += F("function applyPreset(v){if(!presets[v])return;document.getElementById('tz').value=presets[v][0];document.getElementById('tzlabel').value=presets[v][1];}");
  html += F("function syncPreset(){const rule=document.getElementById('tz').value;let selected='CUSTOM';for(const key in presets){if(presets[key][0]===rule){selected=key;break;}}document.getElementById('tzpreset').value=selected;}");
  html += F("syncPreset();");
  html += F("</script>");
  html += F("</main></body></html>");
  return html;
}

void handleRoot() {
  String message = "";
  if (server.hasArg("saved")) {
    message = "Settings saved.";
  } else if (server.hasArg("rebooting")) {
    message = "Restart requested. The device will be back shortly.";
  }
  server.send(200, "text/html", pageHtml(message));
}

void handleSave() {
  const AppSettings previousSettings = getSettings();
  AppSettings settings = previousSettings;
  settings.callsign = limitedArg("callsign", 16);
  settings.wifiSsid = limitedArg("ssid", 64, false);
  const String submittedPassword = limitedArg("pass", 64, false);
  if (server.hasArg("clearpass")) {
    settings.wifiPassword = "";
  } else if (submittedPassword.length() > 0) {
    settings.wifiPassword = submittedPassword;
  }
  settings.timezone = limitedArg("tz", 80);
  settings.timezoneLabel = limitedArg("tzlabel", 24);
  settings.locator = limitedArg("locator", 6);
  settings.clock12Hour = server.hasArg("clock12");
  settings.useJsonPropagationProxy = server.arg("propmode") == "json";
  settings.propagationJsonUrl = limitedArg("propurl", 180);
  const String dxMode = server.arg("dxmode");
  settings.dxSourceMode = dxMode == "json" ? kDxSourceJson
                          : dxMode == "telnet" ? kDxSourceTelnet
                                                : kDxSourceAuto;
  settings.dxSpotsUrl = limitedArg("dxurl", 180);
  settings.dxTelnetHost = limitedArg("dxhost", 64);
  settings.dxTelnetPort = static_cast<uint16_t>(
      constrain(server.arg("dxport").toInt(), 1L, 65535L));
  settings.propagationRefreshMinutes = static_cast<uint16_t>(
      constrain(server.arg("propmins").toInt(), 1L, 120L));
  settings.dxRefreshMinutes = static_cast<uint16_t>(
      constrain(server.arg("dxmins").toInt(), 1L, 120L));
  settings.pskDirection =
      server.arg("pskdir") == "hearing" ? kPskWhoIHear : kPskWhoHearsMe;
  settings.pskWindowMinutes = static_cast<uint16_t>(
      constrain(server.arg("pskwin").toInt(), 5L, 360L));
  settings.pskRefreshMinutes = static_cast<uint16_t>(
      constrain(server.arg("pskmins").toInt(), 5L, 120L));
  settings.pskAppContact = limitedArg("pskmail", 64);
  settings.potaMaxDistanceKm = static_cast<uint16_t>(
      constrain(server.arg("potadist").toInt(), 0L, 20000L));
  settings.potaRefreshMinutes = static_cast<uint16_t>(
      constrain(server.arg("potamins").toInt(), 1L, 120L));
  settings.potaExcludeRbn = server.hasArg("potarbn");
  settings.autoPageChange = server.hasArg("autopage");
  settings.autoPageSeconds = static_cast<uint16_t>(
      constrain(server.arg("autosecs").toInt(), 3L, 600L));
  uint8_t autoPageMask = 0;
  for (uint8_t i = 0; i < kDashboardPageCount; ++i) {
    if (server.hasArg(String("pg") + String(i))) {
      autoPageMask |= static_cast<uint8_t>(1u << i);
    }
  }
  settings.autoPageMask = autoPageMask;
  settings.brightnessPercent = static_cast<uint8_t>(
      constrain(server.arg("bright").toInt(), 5L, 100L));
  settings.swapRedBlueChannels = server.hasArg("swaprb");
  settings.rotate90 = server.hasArg("rot90");
  settings.flip180 = server.hasArg("flip180");
  settings.mirror = server.hasArg("mirror");
  settings.invertColours = server.hasArg("invert");
  settings.swapTouchNav = server.hasArg("touchswap");
  settings.keepHotspotOn = server.hasArg("keepap");
  saveSettings(settings);
  applyTimezoneSettings();
  applyDisplaySettings();
  requestPropagationRefresh();
  requestDxSpotsRefresh();
  requestGreylineRefresh();
  requestPskReporterRefresh();
  requestPotaSpotsRefresh();

  if (settings.wifiSsid != previousSettings.wifiSsid ||
      settings.wifiPassword != previousSettings.wifiPassword) {
    pendingWifiReconnect = true;
    pendingWifiReconnectAtMs = millis() + 1500;
  }

  server.sendHeader("Location", "/?saved=1", true);
  server.send(303, "text/plain", "Settings saved");
}

void handleStatusJson() {
  server.send(200, "application/json", statusJson());
}

void handleReboot() {
  pendingReboot = true;
  pendingRebootAtMs = millis() + 1500;
  server.sendHeader("Location", "/?rebooting=1", true);
  server.send(303, "text/plain", "Restart requested");
}

void handleRebootGet() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleCaptiveRedirect() {
  if (!hotspotActive) {
    handleRoot();
    return;
  }
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
  server.send(302, "text/plain", "");
}
}

void setupPortalBegin() {
  startHotspot();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatusJson);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/reboot", HTTP_GET, handleRebootGet);
  server.on("/generate_204", HTTP_GET, handleCaptiveRedirect);
  server.on("/gen_204", HTTP_GET, handleCaptiveRedirect);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/ncsi.txt", HTTP_GET, []() { server.send(200, "text/plain", "Microsoft NCSI"); });
  server.onNotFound(handleCaptiveRedirect);
  server.begin();
  portalStarted = true;

  Serial.print("Setup portal started: ");
  Serial.println(kApSsid);
  Serial.print("Portal IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupPortalLoop() {
  if (!portalStarted) {
    return;
  }
  if (!mdnsStarted && WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin("cyd-ham")) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.println("mDNS started: http://cyd-ham.local/");
    } else {
      Serial.println("mDNS start failed");
    }
  }

  const AppSettings& settings = getSettings();
  const bool staConnected =
      WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
  const uint32_t nowMs = millis();

  if (settings.keepHotspotOn) {
    staConfirmedSinceMs = 0;
    if (!hotspotActive) {
      startHotspot();
    }
  } else if (staConnected) {
    if (staConfirmedSinceMs == 0) {
      staConfirmedSinceMs = nowMs;
    } else if (hotspotActive && deadlineReached(nowMs, staConfirmedSinceMs + kApAutoOffConfirmMs)) {
      stopHotspot();
    }
  } else {
    staConfirmedSinceMs = 0;
  }

  if (hotspotActive) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  if (pendingWifiReconnect && deadlineReached(nowMs, pendingWifiReconnectAtMs)) {
    pendingWifiReconnect = false;
    reconnectWifi();
  }
  if (pendingReboot && deadlineReached(nowMs, pendingRebootAtMs)) {
    pendingReboot = false;
    ESP.restart();
  }
}
