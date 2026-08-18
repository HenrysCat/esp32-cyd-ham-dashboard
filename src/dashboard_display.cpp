#include "dashboard_display.h"

#include <SPI.h>
#include <TFT_eSPI.h>
// TFT_eSPI's gfxfont.h already declares every GFX free font, and the linker
// discards the ones a build does not reference, so FreeSans18pt7b needs no
// include of its own - adding one is a redefinition, as those Adafruit headers
// carry no include guards.
#include <WiFi.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dx_spots.h"
#include "greyline.h"
// The 4.0" board carries a larger map. Both headers define the same width,
// height and pixel-array identifiers, so nothing below has to know which one a
// build selected.
#if defined(ST7796_DRIVER)
#include "greyline_map_460x230.h"
#else
#include "greyline_map.h"
#endif
#include "pota_spots.h"
#include "propagation.h"
#include "psk_reporter.h"
#include "settings.h"

namespace {
TFT_eSPI tft(DISPLAY_W, DISPLAY_H);
TFT_eSprite mapSprite(&tft);
TFT_eSprite dxScrollSprite(&tft);
// The XPT2046 has its own SPI pins on some CYD boards and shares the display's
// on others. Where it shares, it must share the bus *object* too: calling
// begin() on a second peripheral hands the display's SCLK/MOSI/MISO over to the
// touch controller, and every panel write after that comes out as noise.
// SUPPORT_TRANSACTIONS is mandatory on ESP32, so TFT_eSPI reapplies its own bus
// settings after each touch read and the two can interleave safely.
#if TOUCH_SCLK == TFT_SCLK && TOUCH_MOSI == TFT_MOSI && TOUCH_MISO == TFT_MISO
#define TOUCH_SHARES_DISPLAY_BUS
SPIClass& touchSpi = TFT_eSPI::getSPIinstance();
#else
SPIClass touchSpi(HSPI);
#endif

enum DashboardPage : uint8_t {
  kPageClock = 0,
  kPagePropagation,
  kPageVhf,
  kPageGreyline,
  kPagePsk,
  kPageDx,
  kPagePota,
  kPageCount
};

static_assert(kPageCount == kDashboardPageCount,
              "kDashboardPageCount must match the number of dashboard pages");
static_assert(kAutoPageMaskAll == (1u << kPageCount) - 1u,
              "kAutoPageMaskAll must have one bit per dashboard page");

constexpr uint8_t kLandscapeRotation = 0;
constexpr uint32_t kTouchDebounceMs = 300;
constexpr uint32_t kRenderIntervalMs = 250;
// How long a running DX or POTA scroll may hold back an automatic page change.
// Without a cap a busy telnet feed could keep the rotation parked on one page
// indefinitely.
constexpr uint32_t kAutoPageScrollGraceMs = 3000;
constexpr int16_t kFooterHeight = 26;
constexpr int16_t kFooterTop = DISPLAY_H - kFooterHeight;
constexpr int16_t kFooterY = kFooterTop + 7;

// Fixed columns for the footer when it includes the UTC field (pages 2-5).
// Each field's text has a constant character count between states (e.g.
// "WiFi OK" / "WiFi --"), so these boxes never need to shift or resize.
//
// The field widths are the same on both panels because the footer keeps font 2
// either way - it is deliberately the quiet line on the page. Only the gaps
// change: the four boxes total 233px, so the wide panel has 219px to spread
// between them against the small one's 59px, which is 73px a gap rather than
// 21px. Without that the whole row sits in the left two thirds of the glass.
constexpr int16_t kFooterWUtc = 69;
constexpr int16_t kFooterWWifi = 52;
constexpr int16_t kFooterWNtp = 50;
constexpr int16_t kFooterWPage = 62;
#if DISPLAY_W >= 480
constexpr int16_t kFooterXUtc = 14;
constexpr int16_t kFooterXWifi = 156;
constexpr int16_t kFooterXNtp = 281;
constexpr int16_t kFooterXPage = 404;
#else
constexpr int16_t kFooterXUtc = 14;
constexpr int16_t kFooterXWifi = 104;
constexpr int16_t kFooterXNtp = 177;
constexpr int16_t kFooterXPage = 248;
#endif
static_assert(kFooterXUtc + kFooterWUtc <= kFooterXWifi &&
                  kFooterXWifi + kFooterWWifi <= kFooterXNtp &&
                  kFooterXNtp + kFooterWNtp <= kFooterXPage,
              "the footer fields must not overlap each other");
static_assert(kFooterXPage + kFooterWPage <= DISPLAY_W - 10,
              "the last footer field must stay inside the right margin");

constexpr int8_t kTouchSclk = TOUCH_SCLK;
constexpr int8_t kTouchMosi = TOUCH_MOSI;
constexpr int8_t kTouchMiso = TOUCH_MISO;
constexpr int8_t kTouchCs = TOUCH_CS;
constexpr int8_t kTouchIrq = TOUCH_IRQ;
constexpr uint32_t kTouchFrequency = 2500000;
// How the digitiser's raw axes relate to the panel, and the raw range it
// actually swings over. Both are properties of the glass, so each board's
// User_Setup header supplies them; the defaults here are the ESP32-2432S028R's
// measured values. TOUCH_RAW_MIN/MAX double as the validity filter in
// readRawTouch, so they want a little margin outside the measured extremes
// rather than sitting exactly on them, otherwise a hard corner press is
// rejected instead of clamped.
#ifndef TOUCH_RAW_MIN
#define TOUCH_RAW_MIN 120
#endif
#ifndef TOUCH_RAW_MAX
#define TOUCH_RAW_MAX 3975
#endif
// Screen X comes from the raw Y axis when set.
#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 1
#endif
#ifndef TOUCH_INVERT_X
#define TOUCH_INVERT_X 0
#endif
#ifndef TOUCH_INVERT_Y
#define TOUCH_INVERT_Y 1
#endif

constexpr uint16_t kTouchMin = TOUCH_RAW_MIN;
constexpr uint16_t kTouchMax = TOUCH_RAW_MAX;

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kPanel = TFT_DARKGREY;
constexpr uint16_t kText = TFT_WHITE;
constexpr uint16_t kMuted = TFT_LIGHTGREY;
constexpr uint16_t kAccent = TFT_YELLOW;
constexpr uint16_t kWarn = TFT_ORANGE;

// Taking the map's size from the asset itself means these can never drift out
// of step with the pixel array the way a second set of constants would. Both
// maps are 2:1 equirectangular, so latLonToMapXY needs nothing beyond this.
constexpr int16_t kMapW = kGreylineMapWidth;
constexpr int16_t kMapH = kGreylineMapHeight;
constexpr int16_t kMapX = (DISPLAY_W - kMapW) / 2;
constexpr int16_t kMapY = 4;

// A full-size 16-bit sprite for the 460x230 map would need 211,600 bytes in one
// contiguous block, and this board reports a largest free block of about 110KB
// before Wi-Fi is even up, so the map is composed and pushed one horizontal
// strip at a time. That also cuts the peak allocation on the 2.8" board from
// 90KB to 18KB, which is what used to leave no room for a TLS handshake.
#ifndef MAP_BAND_ROWS
#define MAP_BAND_ROWS 30
#endif
constexpr int16_t kMapBandH = MAP_BAND_ROWS;
static_assert(kMapH % kMapBandH == 0,
              "MAP_BAND_ROWS must divide the map height exactly");

// The three text rows under the map on the Greyline and PSKReporter pages.
// Hanging them off the map's bottom edge reproduces the 2.8" layout exactly
// (4 + 150 + 6 = 160) while following the taller map down on the 4" board.
constexpr int16_t kMapTextRow1 = kMapY + kMapH + 6;
constexpr int16_t kMapTextRow2 = kMapTextRow1 + 18;
constexpr int16_t kMapTextRow3 = kMapTextRow2 + 18;
static_assert(kMapTextRow3 + 10 <= kFooterTop,
              "map page text rows must clear the footer");

// Clock page geometry. Both panels work out at roughly 143 PPI, so a given
// pixel size looks the same on each; the 4.0" board is not given bigger text
// because it needs it, but because the extra room lets the headline clock be
// read from further away.
//
// Font 7 is the seven-segment face and stays the headline clock on both
// panels; the wide one simply draws it at double size, which textWidth and
// fontHeight both honour. Font 8 is not a bigger version of it - it is 75px of
// Arial - so scaling font 7 is the only way to keep the digital look. At size
// 2 that is 96px tall and 432px wide for "12:34:56", inside 480 with a margin.
// The local time keeps font 4 whatever the panel, because it is prefixed with
// the timezone label and fonts 6 to 8 carry only digits, colon, dot and minus.
constexpr uint8_t kClockUtcFont = 7;
#if DISPLAY_H >= 320
constexpr uint8_t kClockUtcSize = 2;
constexpr const GFXfont* kClockTextFont = &FreeSans18pt7b;
constexpr int16_t kClockLabelY = 2;
constexpr int16_t kClockUtcY = 22;
constexpr int16_t kClockLocalY = 126;
constexpr int16_t kClockDateY = 176;
constexpr int16_t kClockStationY = 226;
constexpr int16_t kClockIpY = 276;
constexpr int16_t kClockUptimeY = 276;  // shares the row with the IP
#else
constexpr uint8_t kClockUtcSize = 1;
constexpr const GFXfont* kClockTextFont = nullptr;
constexpr int16_t kClockLabelY = 3;
constexpr int16_t kClockUtcY = 20;
constexpr int16_t kClockLocalY = 86;
constexpr int16_t kClockDateY = 120;
constexpr int16_t kClockStationY = 152;
constexpr int16_t kClockIpY = 176;
constexpr int16_t kClockUptimeY = 194;
#endif
// 8 characters of font 7 at this size, against the panel width.
static_assert(kClockUtcSize * 216 <= DISPLAY_W - 16,
              "the UTC readout must fit the panel with a margin");
static_assert(kClockIpY + 16 <= kFooterTop,
              "the clock page diagnostics row must clear the footer");
// The three rows under the clock use a GFX free font on the wide panel rather
// than font 4 at double size. Doubling is nearest-neighbour, so it magnifies
// the jagged edges along with everything else; FreeSans18pt7b is drawn at its
// own size. It is also smaller (42px line against 52px) and narrower, which is
// what lets the callsign row grow at all: "M9LHX   Locator: JO01AB" is 408px
// here against 572px doubled, so it now fits with its label intact.
static_assert(kClockUptimeY + 16 <= kFooterTop,
              "clock page rows must clear the footer");

// Greyline page columns. The 2.8" values are the ones this page has always
// used, irregular widths and all. The 4.0" board spreads the same seven fields
// into three tidy columns across the wider panel, and gives Status enough room
// to clear its longest string: "Status: Location invalid" is 24 characters at
// 6px in font 1 = 144px, which the small board's 134px clear cannot cover, so
// a sliver of it survives when the status later shortens. There is no room to
// fix that at 320px wide - the Greyline field starts at x=154 and the string
// would reach x=158 - so the narrow panel keeps its existing behaviour.
#if DISPLAY_W >= 480
constexpr int16_t kGreyQthX = 14;
constexpr int16_t kGreyQthW = 160;
constexpr int16_t kGreySunX = 180;
constexpr int16_t kGreySunW = 280;
constexpr int16_t kGreyRiseX = 14;
constexpr int16_t kGreyRiseW = 160;
constexpr int16_t kGreySetX = 180;
constexpr int16_t kGreySetW = 150;
constexpr int16_t kGreyUtcX = 340;
constexpr int16_t kGreyUtcW = 120;
constexpr int16_t kGreyStatusX = 14;
constexpr int16_t kGreyStatusW = 160;
constexpr int16_t kGreyGreylineX = 180;
constexpr int16_t kGreyGreylineW = 280;
#else
constexpr int16_t kGreyQthX = 14;
constexpr int16_t kGreyQthW = 88;
constexpr int16_t kGreySunX = 108;
constexpr int16_t kGreySunW = 126;
constexpr int16_t kGreyRiseX = 14;
constexpr int16_t kGreyRiseW = 76;
constexpr int16_t kGreySetX = 96;
constexpr int16_t kGreySetW = 76;
constexpr int16_t kGreyUtcX = 178;
constexpr int16_t kGreyUtcW = 82;
constexpr int16_t kGreyStatusX = 14;
constexpr int16_t kGreyStatusW = 134;
constexpr int16_t kGreyGreylineX = 154;
constexpr int16_t kGreyGreylineW = -1;  // runs to the right edge
#endif
static_assert(DISPLAY_W < 480 || kGreyStatusW >= 24 * 6,
              "Status must clear \"Status: Location invalid\" on the wide panel");

// Propagation and VHF page geometry. The two pages share a skeleton: a title, a
// panel of solar readings, a panel of condition rows, then an updated/status
// line above the footer. The 2.8" numbers are the ones these pages have always
// used and every one of them is reproduced exactly. The 4.0" board moves the
// body text from font 2 to font 4 and opens the row pitch to match, which is
// what the extra height is worth spending on: the readings are the point of
// both pages and they were set at 16px on a panel that can afford 26px.
//
// Font 4 fits horizontally with room to spare. The longest reading row,
// "Noise S9   Aurora 10   SW 999   Bz -99.9", measures 430px against the 464
// usable, and the widest condition label, "Es NorthAm 2m", is 175px inside a
// 192px column.
#if DISPLAY_W >= 480
constexpr uint8_t kPropBodyFont = 4;
constexpr int16_t kPropPanelATop = 34;
constexpr int16_t kPropPanelAH = 96;
constexpr int16_t kPropReadRowY = 40;
constexpr int16_t kPropReadPitch = 30;
constexpr int16_t kPropPanelBTop = 134;
constexpr int16_t kPropPanelBH = 140;
constexpr int16_t kPropHeadY = 138;
constexpr int16_t kPropRowY = 160;
constexpr int16_t kPropRowPitch = 27;
constexpr int16_t kVhfRowY = 138;
constexpr int16_t kVhfRowPitch = 26;
constexpr int16_t kPropStatusY = 276;
constexpr int16_t kPropStatusX = 244;
constexpr int16_t kCondSplit1 = 200;
constexpr int16_t kCondSplit2 = 340;
#else
constexpr uint8_t kPropBodyFont = 2;
constexpr int16_t kPropPanelATop = 30;
constexpr int16_t kPropPanelAH = 58;
constexpr int16_t kPropReadRowY = 36;
constexpr int16_t kPropReadPitch = 18;
constexpr int16_t kPropPanelBTop = 88;
constexpr int16_t kPropPanelBH = 104;
constexpr int16_t kPropHeadY = 92;
constexpr int16_t kPropRowY = 112;
constexpr int16_t kPropRowPitch = 20;
constexpr int16_t kVhfRowY = 92;
constexpr int16_t kVhfRowPitch = 20;
constexpr int16_t kPropStatusY = 194;
constexpr int16_t kPropStatusX = 164;
constexpr int16_t kCondSplit1 = 112;
constexpr int16_t kCondSplit2 = 216;
#endif
// The glyph height of the body font, which the row fitting below is checked
// against. Only fonts 2 and 4 are ever selected here.
constexpr int16_t kPropBodyH = (kPropBodyFont == 4) ? 26 : 16;
constexpr int16_t kPropPanelBBottom = kPropPanelBTop + kPropPanelBH;
// Day and Night are centred in their columns, and this arithmetic reproduces
// the 2.8" board's long standing 164 and 266.
constexpr int16_t kCondDayX = (kCondSplit1 + kCondSplit2) / 2;
constexpr int16_t kCondNightX = (kCondSplit2 + DISPLAY_W - 4) / 2;
constexpr int16_t kPropUpdatedX = 8;
constexpr int16_t kPropUpdatedW = kPropStatusX - kPropUpdatedX - 6;
constexpr int16_t kPropStatusW = DISPLAY_W - kPropStatusX - 4;
static_assert(kPropReadRowY + 2 * kPropReadPitch + kPropBodyH <= kPropPanelATop + kPropPanelAH,
              "the three reading rows must fit the readings panel");
static_assert(kPropRowY + 3 * kPropRowPitch + kPropBodyH <= kPropPanelBBottom,
              "the four band rows must fit the conditions panel");
static_assert(kVhfRowY + 4 * kVhfRowPitch + kPropBodyH <= kPropPanelBBottom,
              "the five VHF rows must fit the conditions panel");
static_assert(kPropHeadY + 16 <= kPropRowY,
              "the Band/Day/Night headings must clear the first band row");
static_assert(kPropPanelBBottom <= kPropStatusY - 2,
              "the conditions panel must clear the updated/status row");
static_assert(kPropStatusY + 16 <= kFooterTop,
              "the updated/status row must clear the footer");
// Status is left aligned at kPropStatusX on the narrow panel and right aligned
// to the margin on the wide one, so its clear box starts in a different place.
static_assert(kPropUpdatedX + kPropUpdatedW <=
                  (DISPLAY_W >= 480 ? DISPLAY_W - 8 - kPropStatusW : kPropStatusX),
              "the Updated and Status boxes must not overlap");
// A row's clear is two pixels taller than its glyphs, so its last painted row
// is y + glyph height + 1. Reaching a panel border rubs that border out for
// good, because only panel B's top edge is repainted on every pass. The 2.8"
// board does overlap panel A's bottom edge, but there the two boxes share that
// edge and the drawFastHLine puts it straight back, so it keeps its geometry.
static_assert(DISPLAY_W < 480 || kPropReadRowY + 2 * kPropReadPitch + kPropBodyH + 1 <=
                                     kPropPanelATop + kPropPanelAH - 2,
              "the last reading row's clear must not reach the readings panel border");
static_assert(DISPLAY_W < 480 ||
                  kPropRowY + 3 * kPropRowPitch + kPropBodyH + 1 <= kPropPanelBBottom - 2,
              "the last band row's clear must not reach the conditions panel border");
static_assert(DISPLAY_W < 480 ||
                  kVhfRowY + 4 * kVhfRowPitch + kPropBodyH + 1 <= kPropPanelBBottom - 2,
              "the last VHF row's clear must not reach the conditions panel border");

// DX spots list geometry. Rows sit on a fixed 17px pitch with font 2 (16px
// tall); the region starts a couple of pixels above the first row's text.
constexpr int16_t kDxRowsX = 10;
constexpr int16_t kDxRowsW = 300;
constexpr int16_t kDxRowPitch = 17;
constexpr int16_t kDxRowPad = 2;
constexpr int16_t kDxRowTextY = 44;
constexpr int16_t kDxRowsTop = kDxRowTextY - kDxRowPad;
// The list region stops exactly at the bottom of the last row's glyphs, so a
// scroll frame can never expose part of the row that is falling off the end.
constexpr int16_t kDxRowsH = kDxRowPitch * (kMaxDxSpots - 1) + 16 + kDxRowPad;
constexpr int16_t kDxColFreq = 14;
constexpr int16_t kDxColCall = 82;
constexpr int16_t kDxColMode = 170;
constexpr int16_t kDxColTime = 230;

// POTA reuses the DX row geometry, but trades the time column for the park
// reference, which is what a hunter needs in order to log the contact.
// POTA shares these columns and the whole row and scroll mechanism, trading
// only the time column for the park reference, which is what a hunter needs in
// order to log the contact. Eight-character references such as US-10473 end
// around x=306, inside the row band, so no separate layout is needed.

// A new telnet spot pushes every row down one pitch. Rather than redrawing the
// list in its new position, the incoming row plus the rows already on screen
// are rendered once into a sprite one pitch taller than the visible region;
// each frame then pushes a window of that sprite shifted by a few pixels.
constexpr int16_t kDxScrollSpriteH = kDxRowsH + kDxRowPitch;
constexpr int16_t kDxScrollStepPx = 3;
constexpr uint32_t kDxScrollFrameMs = 10;
// A JSON refresh arrives as a batch of several new spots. They are held and
// scrolled in one at a time, with this pause between them so each arrival
// reads as its own event rather than one long blur.
constexpr uint32_t kDxQueueGapMs = 200;

// The list only uses four colours, so a 4-bit palette sprite reproduces them
// exactly and costs a quarter of the RAM a 16-bit sprite would.
constexpr uint8_t kDxPalBg = 0;
constexpr uint8_t kDxPalText = 1;
constexpr uint8_t kDxPalAccent = 2;
constexpr uint8_t kDxPalMuted = 3;
uint16_t g_dxScrollPalette[16] = {kBg, kText, kAccent, kMuted};
constexpr uint8_t kIli9341Madctl = 0x36;
// Base orientation for this board's known-good wiring (MX only, no row/column
// exchange). MV genuinely swaps which physical axis is "wide", which is what
// CYD units needing a 90-degree turn are missing; MX/MY together mirror both
// axes for a 180-degree flip within the same wide/tall family. All four
// resulting bytes match TFT_eSPI's own ILI9341 rotation table (rotations
// 0/2/5/7), so none of these combinations are unverified guesses.
constexpr uint8_t kIli9341MadctlMx = 0x40;
constexpr uint8_t kIli9341MadctlMy = 0x80;
constexpr uint8_t kIli9341MadctlMv = 0x20;
constexpr uint8_t kIli9341MadctlBgr = 0x08;

DashboardPage g_currentPage = kPageClock;
bool g_pageDirty = true;
bool g_mapSpriteReady = false;
// Map-space row held by the sprite's first line. Every overlay helper below
// works in map coordinates and offsets by this, so each strip simply draws the
// whole scene and lets the sprite clip away everything that misses.
int16_t g_mapBandTop = 0;
bool g_touchWasDown = false;
uint32_t g_lastTouchActionMs = 0;
uint32_t g_lastRenderMs = 0;
uint32_t g_lastPageChangeMs = 0;
// Backlight percentage currently driven to the panel. Zero is below the
// five percent floor, so it never matches a real target and the first check
// after a settings change always re-applies.
uint8_t g_appliedBrightnessPercent = 0;

String g_lastFooterSimple;
String g_lastFooterUtc;
String g_lastFooterWifi;
String g_lastFooterNtp;
String g_lastFooterPage;
String g_lastUtc;
String g_lastLocal;
String g_lastDate;
String g_lastLocator;
String g_lastIp;
String g_lastUptime;
String g_lastPropSfiXray;
String g_lastPropAK;
String g_lastPropSunspots;
String g_lastPropGeomag;
String g_lastPropNoise;
String g_lastPropFof2;
String g_lastPropMuf;
String g_lastPropBandA;
String g_lastPropBandB;
String g_lastPropBandC;
String g_lastPropBandD;
String g_lastPropUpdated;
String g_lastPropStatus;
String g_lastVhfAurora;
String g_lastVhfEsEurope;
String g_lastVhfEsNorthAmerica;
String g_lastVhfEsEurope6m;
String g_lastVhfEsEurope4m;
String g_lastVhfUpdated;
String g_lastVhfStatus;
String g_lastGreyUtc;
String g_lastGreyQth;
String g_lastGreyLatLon;
String g_lastGreySunrise;
String g_lastGreySunset;
String g_lastGreyNoon;
String g_lastGreyDayLength;
String g_lastGreySunLat;
String g_lastGreySunLon;
String g_lastGreyStatus;
String g_lastGreyline;
String g_lastGreyMap;
String g_lastPskMap;
String g_lastPskHeading;
String g_lastPskBest;
String g_lastPskFooterLine;
String g_lastDxEmpty;
String g_lastDxUpdated;
String g_lastDxSource;
String g_lastDxStatus;
String g_lastPotaUpdated;
String g_lastPotaStatus;
String g_lastPotaEmpty;

// The four fields of a DX row, already truncated to the widths that get drawn,
// so comparing rows compares exactly what is on screen.
struct DxRowText {
  String freq;
  String call;
  String mode;
  String time;
};

DxRowText g_dxShownRows[kMaxDxSpots];
uint8_t g_dxShownCount = 0;
bool g_dxScrollSpriteReady = false;
bool g_dxScrollActive = false;
int16_t g_dxScrollProgress = 0;
uint32_t g_dxScrollFrameMs = 0;
// The list the running scroll is animating towards; adopted as the shown rows
// when it finishes.
DxRowText g_dxScrollEndRows[kMaxDxSpots];
uint8_t g_dxScrollEndCount = 0;

// New spots waiting their turn to scroll in, oldest first, plus the list the
// whole sequence ends on.
DxRowText g_dxQueueRows[kMaxDxSpots];
uint8_t g_dxQueueCount = 0;
uint8_t g_dxQueueIndex = 0;
DxRowText g_dxQueueTargetRows[kMaxDxSpots];
uint8_t g_dxQueueTargetCount = 0;
uint32_t g_dxQueueReadyMs = 0;

char utcBuffer[16];
char localBuffer[24];
char dateBuffer[24];
char uptimeBuffer[16];


int16_t centerX() {
  return tft.width() / 2;
}

String wifiStatusText(bool connected) {
  return connected ? "WiFi OK" : "WiFi --";
}

String ntpStatusText(bool valid) {
  return valid ? "NTP OK" : "NTP --";
}

String pageIndicator() {
  return String("Page ") + String(static_cast<uint8_t>(g_currentPage) + 1) +
         "/" + String(kPageCount);
}

String footerUtcText(const ClockSnapshot& snapshot) {
  if (!snapshot.timeValid) {
    return "UTC --:--";
  }
  tm utcTime;
  gmtime_r(&snapshot.epoch, &utcTime);
  char buffer[16];
  strftime(buffer, sizeof(buffer), "UTC %H:%M", &utcTime);
  return String(buffer);
}

void clearPageState() {
  g_lastFooterSimple = "";
  g_lastFooterUtc = "";
  g_lastFooterWifi = "";
  g_lastFooterNtp = "";
  g_lastFooterPage = "";
  g_lastUtc = "";
  g_lastLocal = "";
  g_lastDate = "";
  g_lastLocator = "";
  g_lastIp = "";
  g_lastUptime = "";
  g_lastPropSfiXray = "";
  g_lastPropAK = "";
  g_lastPropSunspots = "";
  g_lastPropGeomag = "";
  g_lastPropNoise = "";
  g_lastPropFof2 = "";
  g_lastPropMuf = "";
  g_lastPropBandA = "";
  g_lastPropBandB = "";
  g_lastPropBandC = "";
  g_lastPropBandD = "";
  g_lastPropUpdated = "";
  g_lastPropStatus = "";
  g_lastVhfAurora = "";
  g_lastVhfEsEurope = "";
  g_lastVhfEsNorthAmerica = "";
  g_lastVhfEsEurope6m = "";
  g_lastVhfEsEurope4m = "";
  g_lastVhfUpdated = "";
  g_lastVhfStatus = "";
  g_lastGreyUtc = "";
  g_lastGreyQth = "";
  g_lastGreyLatLon = "";
  g_lastGreySunrise = "";
  g_lastGreySunset = "";
  g_lastGreyNoon = "";
  g_lastGreyDayLength = "";
  g_lastGreySunLat = "";
  g_lastGreySunLon = "";
  g_lastGreyStatus = "";
  g_lastGreyline = "";
  g_lastGreyMap = "";
  g_lastPskMap = "";
  g_lastPskHeading = "";
  g_lastPskBest = "";
  g_lastPskFooterLine = "";
  for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
    g_dxShownRows[i] = DxRowText();
  }
  g_dxShownCount = 0;
  g_dxScrollActive = false;
  g_dxQueueCount = 0;
  g_dxQueueIndex = 0;
  g_dxQueueTargetCount = 0;
  g_lastDxEmpty = "";
  g_lastDxUpdated = "";
  g_lastDxSource = "";
  g_lastDxStatus = "";
  g_lastPotaUpdated = "";
  g_lastPotaStatus = "";
  g_lastPotaEmpty = "";
}

void drawCentered(const String& text, int16_t y, uint8_t font, uint16_t color = kText) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(text, centerX(), y, font);
}

void drawCenteredAt(const String& text, int16_t x, int16_t y, uint8_t font,
                    uint16_t color = kText) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(text, x, y, font);
}

void drawLeft(const String& text, int16_t x, int16_t y, uint8_t font, uint16_t color = kText) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(text, x, y, font);
}

void drawCenteredField(String& last, const String& value, int16_t y, uint8_t font,
                       uint16_t color = kText, int16_t x = -1, int16_t w = -1,
                       uint8_t size = 1, const GFXfont* freeFont = nullptr) {
  if (value == last) {
    return;
  }

  if (x < 0) {
    x = 0;
  }
  if (w < 0) {
    w = tft.width();
  }

  // A free font is selected as font 1, and textWidth and fontHeight both
  // report its metrics once it is, so the centring and the clear rectangle
  // below stay correct either way. textsize scales both as well.
  if (freeFont != nullptr) {
    tft.setFreeFont(freeFont);
    font = 1;
  }
  tft.setTextSize(size);
  const int16_t h = tft.fontHeight(font) + 4;
  if (tft.textWidth(value, font) != tft.textWidth(last, font)) {
    tft.fillRect(x, y - 2, w, h, kBg);
  }
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(value, x + (w / 2), y, font);
  tft.setTextSize(1);
  if (freeFont != nullptr) {
    tft.setFreeFont(nullptr);
  }
  last = value;
}

// Draws "label value" with the gap between the two centred on the panel, so
// the value is left aligned from a fixed x and cannot drift sideways as its
// width changes. A centred clock does drift: 12 hour time drops the leading
// zero, so "7:24:31 AM" is a digit narrower than "12:24:31 AM".
void drawSplitField(String& last, const String& label, const String& value, int16_t y,
                    uint8_t font, uint16_t color = kText, uint8_t size = 1,
                    const GFXfont* freeFont = nullptr) {
  const String combined = label + '\t' + value;
  if (combined == last) {
    return;
  }

  if (freeFont != nullptr) {
    tft.setFreeFont(freeFont);
    font = 1;
  }
  tft.setTextSize(size);

  // A lone space measures zero, because textWidth() uses the glyph outline
  // rather than the advance for the last character of a string and a space has
  // no outline. Measuring it between two glyphs returns the advance we want.
  const int16_t gap = tft.textWidth(" x", font) - tft.textWidth("x", font);
  const int16_t centre = tft.width() / 2;
  const int16_t valueX = centre + (gap / 2);
  const int16_t valueW = tft.width() - valueX;

  // yAdvance is the font's own line height, so it allows for descenders however
  // little of the string happens to use them. fontHeight() would not: the free
  // font metrics it reports are those of the last string measured.
  const int16_t h = (freeFont != nullptr)
                        ? static_cast<int16_t>(pgm_read_byte(&freeFont->yAdvance) * size + 4)
                        : static_cast<int16_t>(tft.fontHeight(font) + 4);

  const int16_t split = last.indexOf('\t');
  const String lastLabel = (split >= 0) ? last.substring(0, split) : String();

  // Redrawing the label every second is what makes the line blink, since a free
  // font fills its whole bounding box before it draws the glyphs. It only needs
  // touching when it actually changes, which is at most twice a year.
  if (label != lastLabel) {
    if (tft.textWidth(label, font) < tft.textWidth(lastLabel, font)) {
      tft.fillRect(0, y - 2, centre, h, kBg);
    }
    tft.setTextColor(color, kBg);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(label, centre - (gap / 2), y, font);
  }

  // The value is built off screen and pushed in one blit, so that background
  // fill never appears on the panel. The sprite spans the full width to the
  // right edge, which clears a longer previous value on the way past.
  TFT_eSprite field(&tft);
  field.setColorDepth(16);
  if (field.createSprite(valueW, h) != nullptr) {
    field.fillSprite(kBg);
    if (freeFont != nullptr) {
      field.setFreeFont(freeFont);
    }
    field.setTextSize(size);
    field.setTextColor(color, kBg);
    field.setTextDatum(TL_DATUM);
    field.drawString(value, 0, 0, font);
    field.pushSprite(valueX, y);
    field.deleteSprite();
  } else {
    tft.fillRect(valueX, y - 2, valueW, h, kBg);
    tft.setTextColor(color, kBg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(value, valueX, y, font);
  }

  tft.setTextSize(1);
  if (freeFont != nullptr) {
    tft.setFreeFont(nullptr);
  }
  last = combined;
}

void drawLeftField(String& last, const String& value, int16_t x, int16_t y,
                   uint8_t font, uint16_t color = kText, int16_t w = -1) {
  if (value == last) {
    return;
  }

  if (w < 0) {
    w = tft.width() - x;
  }

  const int16_t h = tft.fontHeight(font) + 4;
  tft.fillRect(x, y - 2, w, h, kBg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(value, x, y, font);
  last = value;
}

// As drawLeftField, but the text ends at right rather than starting at x. The
// box still clears w pixels, so it is given the same width its left aligned
// counterpart would have had.
void drawRightField(String& last, const String& value, int16_t right, int16_t y,
                    uint8_t font, uint16_t color = kText, int16_t w = -1) {
  if (value == last) {
    return;
  }

  if (w < 0) {
    w = right;
  }

  const int16_t h = tft.fontHeight(font) + 4;
  tft.fillRect(right - w, y - 2, w, h, kBg);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(value, right, y, font);
  last = value;
}

// The Status field on the propagation and VHF pages. It stays left aligned in
// its box on the narrow panel, which is how it has always sat there, and is
// pushed out to the right margin on the wide one. Both fields are short, so
// left aligning them at the half way mark leaves the right third of a 480px
// row empty; anchoring one to each edge fills the line the way it does at 320.
void drawPropStatusField(String& last, const String& value, uint16_t color) {
#if DISPLAY_W >= 480
  drawRightField(last, value, DISPLAY_W - 8, kPropStatusY, 2, color, kPropStatusW);
#else
  drawLeftField(last, value, kPropStatusX, kPropStatusY, 2, color, kPropStatusW);
#endif
}

uint16_t conditionColor(const String& condition) {
  if (condition.equalsIgnoreCase("Good")) {
    return TFT_GREEN;
  }
  if (condition.equalsIgnoreCase("Fair")) {
    return TFT_YELLOW;
  }
  if (condition.equalsIgnoreCase("Poor")) {
    return TFT_RED;
  }
  return kMuted;
}

bool numericReading(const String& value, float& result) {
  const char* text = value.c_str();
  char* end = nullptr;
  result = strtof(text, &end);
  return end != text;
}

uint16_t readingColor(const String& reading, const char* parameter) {
  float value = 0.0f;
  if (String(parameter) == "X-Ray") {
    String level = reading;
    level.trim();
    level.toUpperCase();
    if (level.startsWith("A") || level.startsWith("B") || level.startsWith("C")) return TFT_GREEN;
    if (level.startsWith("M")) return TFT_YELLOW;
    if (level.startsWith("X")) return TFT_RED;
    return kMuted;
  }

  if (!numericReading(reading, value)) return kMuted;

  if (String(parameter) == "SFI") {
    return value >= 120.0f ? TFT_GREEN : value >= 70.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "SN") {
    return value >= 70.0f ? TFT_GREEN : value >= 10.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "K") {
    return value <= 5.0f ? TFT_GREEN : value <= 6.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "A") {
    return value <= 49.0f ? TFT_GREEN : value <= 99.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "SW") {
    return value < 500.0f ? TFT_GREEN : value < 600.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "Bz") {
    return value >= -10.0f ? TFT_GREEN : value >= -20.0f ? TFT_YELLOW : TFT_RED;
  }
  if (String(parameter) == "Aurora") {
    return value <= 8.0f ? TFT_GREEN : value <= 9.0f ? TFT_YELLOW : TFT_RED;
  }
  return kMuted;
}

uint16_t qualitativeReadingColor(const String& reading, const char* parameter) {
  String level = reading;
  level.trim();
  level.toUpperCase();

  if (String(parameter) == "Geomag") {
    // HamQSL reports: Inactive, Very Quiet, Quiet, Unsettled, Active, Minor Storm,
    // Major Storm, Severe Storm, Extreme Storm - in increasing order of K-index severity.
    if (level.length() == 0 || level == "--") return kMuted;
    if (level.indexOf("SEVERE") >= 0 || level.indexOf("EXTREME") >= 0) return TFT_RED;
    if (level.indexOf("MAJOR") >= 0) return TFT_YELLOW;
    if (level.indexOf("QUIET") >= 0 || level.indexOf("UNSETTLED") >= 0 ||
        level.indexOf("ACTIVE") >= 0 || level.indexOf("STORM") >= 0 ||
        level == "INACTIVE" || level == "NORMAL") return TFT_GREEN;
    return kMuted;
  }
  if (String(parameter) == "Noise") {
    // HamQSL reports noise as an S-meter level or range, e.g. "S0-S1", "S3", "S5-S7".
    // Use the highest S number present to gauge severity.
    int maxS = -1;
    for (size_t i = 0; i < level.length(); ++i) {
      if (level[i] == 'S' && i + 1 < level.length() && isDigit(level[i + 1])) {
        size_t j = i + 1;
        int num = 0;
        while (j < level.length() && isDigit(level[j])) {
          num = num * 10 + (level[j] - '0');
          j++;
        }
        if (num > maxS) maxS = num;
      }
    }
    if (maxS >= 0) {
      return maxS <= 6 ? TFT_GREEN : maxS <= 9 ? TFT_YELLOW : TFT_RED;
    }
    if (level.indexOf("LOW") >= 0 || level == "NORMAL") return TFT_GREEN;
    if (level.indexOf("MODERATE") >= 0 || level.indexOf("MEDIUM") >= 0) return TFT_YELLOW;
    if (level.indexOf("HIGH") >= 0) return TFT_RED;
  }
  if (String(parameter) == "Aurora") {
    return readingColor(reading, "Aurora");
  }
  return kMuted;
}

void drawReading(int16_t& x, int16_t y, const String& label, const String& value,
                 uint16_t color) {
  drawLeft(label, x, y, kPropBodyFont, kText);
  x += tft.textWidth(label, kPropBodyFont);
  drawLeft(value, x, y, kPropBodyFont, color);
  x += tft.textWidth(value, kPropBodyFont);
}

void drawTopReadingRows(String& lastSfiXray, String& lastSunspots, String& lastNoise,
                        const PropagationData& propagation) {
  constexpr int16_t kRow1 = kPropReadRowY;
  constexpr int16_t kRow2 = kPropReadRowY + kPropReadPitch;
  constexpr int16_t kRow3 = kPropReadRowY + 2 * kPropReadPitch;
  constexpr int16_t kClearW = DISPLAY_W - 16;

  const String sfiXray = "SFI " + propagation.sfi + "   A " + propagation.aIndex +
                         "   K " + propagation.kIndex + "   X-Ray " + propagation.xray;
  if (sfiXray != lastSfiXray) {
    tft.fillRect(8, kRow1 - 2, kClearW, tft.fontHeight(kPropBodyFont) + 4, kBg);
    int16_t x = 8;
    drawReading(x, kRow1, "SFI ", propagation.sfi, readingColor(propagation.sfi, "SFI"));
    drawReading(x, kRow1, "   A ", propagation.aIndex, readingColor(propagation.aIndex, "A"));
    drawReading(x, kRow1, "   K ", propagation.kIndex, readingColor(propagation.kIndex, "K"));
    drawReading(x, kRow1, "   X-Ray ", propagation.xray, readingColor(propagation.xray, "X-Ray"));
    lastSfiXray = sfiXray;
  }

  const String sunspots = "Sunspots " + propagation.sunspots + "   Geomag " + propagation.geomag;
  if (sunspots != lastSunspots) {
    tft.fillRect(8, kRow2 - 2, kClearW, tft.fontHeight(kPropBodyFont) + 4, kBg);
    int16_t x = 8;
    drawReading(x, kRow2, "Sunspots ", propagation.sunspots,
                readingColor(propagation.sunspots, "SN"));
    drawReading(x, kRow2, "   Geomag ", propagation.geomag,
                qualitativeReadingColor(propagation.geomag, "Geomag"));
    lastSunspots = sunspots;
  }

  const String noise = "Noise " + propagation.signalNoise + "   Aurora " + propagation.aurora +
                       "   SW " + propagation.solarWind + "   Bz " + propagation.bz;
  if (noise != lastNoise) {
    tft.fillRect(8, kRow3 - 2, kClearW, tft.fontHeight(kPropBodyFont) + 4, kBg);
    int16_t x = 8;
    drawReading(x, kRow3, "Noise ", propagation.signalNoise,
                qualitativeReadingColor(propagation.signalNoise, "Noise"));
    drawReading(x, kRow3, "   Aurora ", propagation.aurora,
                qualitativeReadingColor(propagation.aurora, "Aurora"));
    drawReading(x, kRow3, "   SW ", propagation.solarWind,
                readingColor(propagation.solarWind, "SW"));
    drawReading(x, kRow3, "   Bz ", propagation.bz, readingColor(propagation.bz, "Bz"));
    lastNoise = noise;
  }
}

void drawConditionValue(const String& value, int16_t center, int16_t y) {
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(conditionColor(value), kBg);
  tft.drawString(value, center, y, kPropBodyFont);
}

void drawConditionRow(String& last, const String& label, const String& day,
                      const String& night, int16_t y) {
  const String value = label + "|" + day + "|" + night;
  if (value == last) {
    return;
  }

  const int16_t rowTop = y - 2;
  const int16_t rowHeight = tft.fontHeight(kPropBodyFont) + 4;
  tft.fillRect(5, rowTop, tft.width() - 10, rowHeight, kBg);
  tft.drawFastVLine(kCondSplit1, rowTop, rowHeight, kPanel);
  tft.drawFastVLine(kCondSplit2, rowTop, rowHeight, kPanel);
  drawLeft(label, 8, y, kPropBodyFont, kText);
  drawConditionValue(day, kCondDayX, y);
  drawConditionValue(night, kCondNightX, y);
  last = value;
}

uint16_t vhfConditionColor(const String& value) {
  String level = value;
  level.trim();
  level.toUpperCase();
  if (level.indexOf("CLOSED") >= 0 || level.indexOf("POOR") >= 0) {
    return TFT_RED;
  }
  if (level.indexOf("OPEN") >= 0 || level.indexOf("HIGH") >= 0 || level.indexOf("GOOD") >= 0) {
    return TFT_GREEN;
  }
  if (level.indexOf("MODERATE") >= 0 || level.indexOf("FAIR") >= 0) {
    return TFT_YELLOW;
  }
  return kMuted;
}

void drawVhfConditionRow(String& last, const String& label, const String& value, int16_t y) {
  const String combined = label + "|" + value;
  if (combined == last) {
    return;
  }

  const int16_t rowTop = y - 2;
  const int16_t rowHeight = tft.fontHeight(kPropBodyFont) + 4;
  tft.fillRect(5, rowTop, tft.width() - 10, rowHeight, kBg);
  drawLeft(label, 8, y, kPropBodyFont, kText);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(vhfConditionColor(value), kBg);
  tft.drawString(value, tft.width() - 12, y, kPropBodyFont);
  last = combined;
}

uint16_t auroraLatColor(const String& value) {
  float lat = 0.0f;
  if (!numericReading(value, lat)) {
    return kMuted;
  }
  // Unlike HF, VHF operators chase aurora backscatter, so a lower latitude
  // (aurora expanded further south, more active/workable) is favourable and
  // the ~67.5 baseline (aurora confined near the pole, effectively closed) is not.
  if (lat >= 65.0f) return TFT_RED;
  if (lat >= 55.0f) return TFT_YELLOW;
  return TFT_GREEN;
}

void drawVhfAuroraRow(String& last, const String& status, const String& lat, int16_t y) {
  const String combined = status + "|" + lat;
  if (combined == last) {
    return;
  }

  const int16_t rowTop = y - 2;
  const int16_t rowHeight = tft.fontHeight(kPropBodyFont) + 4;
  tft.fillRect(5, rowTop, tft.width() - 10, rowHeight, kBg);

  int16_t x = 8;
  drawLeft("VHF Aurora", x, y, kPropBodyFont, kText);
  x += tft.textWidth("VHF Aurora", kPropBodyFont);
  if (lat.length() > 0 && lat != "--") {
    const String latText = " (Lat " + lat + ")";
    drawLeft(latText, x, y, kPropBodyFont, auroraLatColor(lat));
  }

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(vhfConditionColor(status), kBg);
  tft.drawString(status, tft.width() - 12, y, kPropBodyFont);
  last = combined;
}

bool ensureMapSprite() {
  if (g_mapSpriteReady) {
    return true;
  }

  mapSprite.setColorDepth(16);
  g_mapSpriteReady = mapSprite.createSprite(kMapW, kMapBandH) != nullptr;
  if (!g_mapSpriteReady) {
    Serial.println("Greyline map sprite allocation failed");
  }
  return g_mapSpriteReady;
}

// The map sprite is 90KB, and holding it for the life of the run leaves no
// contiguous block large enough for a TLS handshake, so every HTTPS fetch
// starts failing once a map page has been visited. It is only actually needed
// while a frame is being composed, so it is handed straight back after the
// push. Nothing is allocated between the create and the release, so the same
// block is simply reused each time rather than fragmenting the heap.
void releaseMapSprite() {
  if (!g_mapSpriteReady) {
    return;
  }
  mapSprite.deleteSprite();
  g_mapSpriteReady = false;
}

void latLonToMapXY(double latitude, double longitude, int16_t& x, int16_t& y) {
  longitude = constrain(longitude, -180.0, 180.0);
  latitude = constrain(latitude, -90.0, 90.0);
  x = static_cast<int16_t>(((longitude + 180.0) * (kMapW - 1)) / 360.0);
  y = static_cast<int16_t>(((90.0 - latitude) * (kMapH - 1)) / 180.0);
}

// Map row -> row within the strip the sprite currently holds.
inline int16_t bandY(int16_t mapY) { return mapY - g_mapBandTop; }

void drawMapBandBackground() {
  mapSprite.setSwapBytes(true);
  mapSprite.pushImage(0, 0, kGreylineMapWidth, kMapBandH,
                      const_cast<uint16_t*>(
                          kGreylineMapRgb565 +
                          (static_cast<uint32_t>(g_mapBandTop) * kGreylineMapWidth)));
}

void pushMapBand() { mapSprite.pushSprite(kMapX, kMapY + g_mapBandTop); }

void drawMapPlaceholder() {
  tft.fillRect(kMapX, kMapY, kMapW, kMapH, kBg);
  tft.drawRect(kMapX, kMapY, kMapW, kMapH, kPanel);
}

uint16_t darkenRgb565(uint16_t color, uint8_t percent) {
  percent = constrain(percent, static_cast<uint8_t>(0), static_cast<uint8_t>(100));
  const uint8_t keep = 100 - percent;
  uint8_t r = ((color >> 11) & 0x1F) << 3;
  uint8_t g = ((color >> 5) & 0x3F) << 2;
  uint8_t b = (color & 0x1F) << 3;

  r = (static_cast<uint16_t>(r) * keep) / 100;
  g = (static_cast<uint16_t>(g) * keep) / 100;
  b = (static_cast<uint16_t>(b) * keep) / 100;
  return mapSprite.color565(r, g, b);
}

void drawNightShading(double subsolarLatitude, double subsolarLongitude) {
  const double sunLatRad = subsolarLatitude * DEG_TO_RAD;
  const double sinSunLat = sin(sunLatRad);
  const double cosSunLat = cos(sunLatRad);

  const int16_t firstRow = max<int16_t>(1, g_mapBandTop);
  const int16_t lastRow = min<int16_t>(kMapH - 1, g_mapBandTop + kMapBandH);

  for (int16_t y = firstRow; y < lastRow; ++y) {
    const double latitude = 90.0 - ((static_cast<double>(y) * 180.0) / (kMapH - 1));
    const double latRad = latitude * DEG_TO_RAD;
    const double sinLat = sin(latRad);
    const double cosLat = cos(latRad);

    for (int16_t x = 1; x < kMapW - 1; ++x) {
      const double longitude = ((static_cast<double>(x) * 360.0) / (kMapW - 1)) - 180.0;
      const double hourAngle = (longitude - subsolarLongitude) * DEG_TO_RAD;
      const double sunAltitude = (sinLat * sinSunLat) + (cosLat * cosSunLat * cos(hourAngle));
      if (sunAltitude < 0.0) {
        const uint8_t shadePercent = sunAltitude > -0.08 ? 28 : 48;
        const int16_t sy = bandY(y);
        mapSprite.drawPixel(x, sy, darkenRgb565(mapSprite.readPixel(x, sy), shadePercent));
      }
    }
  }
}

void drawTerminator(double subsolarLatitude, double subsolarLongitude) {
  const uint16_t terminator = mapSprite.color565(185, 198, 204);
  const double subsolarLatRad = subsolarLatitude * DEG_TO_RAD;
  if (fabs(tan(subsolarLatRad)) < 0.02) {
    for (int8_t direction = -1; direction <= 1; direction += 2) {
      double longitude = subsolarLongitude + (direction * 90.0);
      if (longitude > 180.0) longitude -= 360.0;
      if (longitude < -180.0) longitude += 360.0;
      int16_t x1, y1;
      int16_t x2, y2;
      latLonToMapXY(90.0, longitude, x1, y1);
      latLonToMapXY(-90.0, longitude, x2, y2);
      mapSprite.drawLine(x1, bandY(y1), x2, bandY(y2), terminator);
    }
    return;
  }

  bool havePrevious = false;
  int16_t previousX = 0;
  int16_t previousY = 0;
  for (int lon = -180; lon <= 180; lon += 4) {
    const double deltaLonRad = (lon - subsolarLongitude) * DEG_TO_RAD;
    const double latRad = atan(-cos(deltaLonRad) / tan(subsolarLatRad));
    const double latitude = latRad * RAD_TO_DEG;

    int16_t x;
    int16_t y;
    latLonToMapXY(latitude, lon, x, y);
    if (havePrevious) {
      mapSprite.drawLine(previousX, bandY(previousY), x, bandY(y), terminator);
    }
    previousX = x;
    previousY = y;
    havePrevious = true;
  }
}

void drawQthMarker(double latitude, double longitude) {
  int16_t x, y;
  latLonToMapXY(latitude, longitude, x, y);
  mapSprite.drawCircle(x, bandY(y), 3, kAccent);
  mapSprite.drawFastHLine(max<int16_t>(0, x - 5), bandY(y),
                          min<int16_t>(11, kMapW - max<int16_t>(0, x - 5)), kAccent);
  mapSprite.drawFastVLine(x, bandY(max<int16_t>(0, y - 5)),
                          min<int16_t>(11, kMapH - max<int16_t>(0, y - 5)), kAccent);
}

void drawSunMarker(double latitude, double longitude) {
  int16_t x, y;
  latLonToMapXY(latitude, longitude, x, y);
  mapSprite.fillCircle(x, bandY(y), 3, TFT_YELLOW);
  mapSprite.drawCircle(x, bandY(y), 5, TFT_YELLOW);
  mapSprite.setTextDatum(TL_DATUM);
  mapSprite.setTextColor(TFT_YELLOW, mapSprite.color565(2, 12, 22));
  mapSprite.drawString("S", min<int16_t>(x + 6, kMapW - 9), bandY(max<int16_t>(0, y - 6)), 1);
}

void drawGreylineMap(const GreylineData& greyline) {
  if (!ensureMapSprite()) {
    drawMapPlaceholder();
    return;
  }

  for (g_mapBandTop = 0; g_mapBandTop < kMapH; g_mapBandTop += kMapBandH) {
    drawMapBandBackground();
    if (greyline.valid) {
      drawNightShading(greyline.sunLatitudeValue, greyline.sunLongitudeValue);
      drawTerminator(greyline.sunLatitudeValue, greyline.sunLongitudeValue);
      drawQthMarker(greyline.latitudeValue, greyline.longitudeValue);
      drawSunMarker(greyline.sunLatitudeValue, greyline.sunLongitudeValue);
    }
    pushMapBand();
  }
  g_mapBandTop = 0;
  releaseMapSprite();
}

// Reception reports are plotted with a dark halo so that the pale band colours
// stay visible over the light land and ocean pixels of the map image.
void drawPskMarker(double latitude, double longitude, uint8_t bandIndex) {
  int16_t x;
  int16_t y;
  latLonToMapXY(latitude, longitude, x, y);
  mapSprite.drawCircle(x, bandY(y), 2, TFT_BLACK);
  mapSprite.fillCircle(x, bandY(y), 1, pskBandColor(bandIndex));
}

void drawPskMap(const PskReporterData& psk) {
  if (!ensureMapSprite()) {
    drawMapPlaceholder();
    return;
  }

  // Night shading and the day/night line both go down before the markers, so
  // reports stay at full brightness on top of them rather than being dimmed
  // along with the map. Reception reports bunch along the terminator during
  // greyline propagation, which is the point of showing it here. The shading
  // pass costs a cos() per map pixel, but the subsolar point only moves once a
  // minute, so this page redraws no more often than the Greyline page that has
  // always carried the same cost.
  const GreylineData& greyline = getGreylineData();

  double qthLat;
  double qthLon;
  const bool haveQth =
      getConfiguredLatitude(qthLat) && getConfiguredLongitude(qthLon);

  for (g_mapBandTop = 0; g_mapBandTop < kMapH; g_mapBandTop += kMapBandH) {
    drawMapBandBackground();
    if (greyline.valid) {
      drawNightShading(greyline.sunLatitudeValue, greyline.sunLongitudeValue);
      drawTerminator(greyline.sunLatitudeValue, greyline.sunLongitudeValue);
    }

    for (uint8_t i = 0; i < psk.reportCount; ++i) {
      const PskReport& report = psk.reports[i];
      drawPskMarker(report.latitude, report.longitude, report.bandIndex);
    }

    if (haveQth) {
      drawQthMarker(qthLat, qthLon);
    }
    pushMapBand();
  }
  g_mapBandTop = 0;
  releaseMapSprite();
}

// Names only the bands that actually appear in the current reports, each drawn
// in the colour its markers use, so the map needs no separate key.
void drawPskBandLegend(const PskReporterData& psk, int16_t y) {
  const int16_t rowTop = y - 2;
  const int16_t rowHeight = tft.fontHeight(1) + 4;
  tft.fillRect(8, rowTop, tft.width() - 16, rowHeight, kBg);

  if (psk.status != "OK") {
    drawLeft("Status: " + psk.status, 14, y, 1, kWarn);
    return;
  }
  if (psk.bandMask == 0) {
    drawLeft("Bands: --", 14, y, 1, kMuted);
    return;
  }

  int16_t x = 14;
  drawLeft("Bands:", x, y, 1, kMuted);
  x += tft.textWidth("Bands:", 1) + 6;
  for (uint8_t band = 0; band < pskBandCount(); ++band) {
    if ((psk.bandMask & static_cast<uint16_t>(1u << band)) == 0) {
      continue;
    }
    const String label = pskBandLabel(band);
    const int16_t width = tft.textWidth(label, 1);
    if (x + width > tft.width() - 14) {
      break;
    }
    drawLeft(label, x, y, 1, pskBandColor(band));
    x += width + 7;
  }
}

String truncateText(const String& value, uint8_t maxLen) {
  if (value.length() <= maxLen) {
    return value;
  }
  return value.substring(0, maxLen);
}

DxRowText makeDxRowText(const DxSpot& spot) {
  DxRowText row;
  row.freq = truncateText(spot.freq, 7);
  row.call = truncateText(spot.call, 9);
  row.mode = truncateText(spot.mode, 5);
  row.time = truncateText(spot.time, 5);
  return row;
}

bool sameDxRow(const DxRowText& a, const DxRowText& b) {
  return a.freq == b.freq && a.call == b.call && a.mode == b.mode && a.time == b.time;
}

bool sameDxRowList(const DxRowText* a, uint8_t aCount, const DxRowText* b, uint8_t bCount) {
  if (aCount != bCount) {
    return false;
  }
  for (uint8_t i = 0; i < aCount; ++i) {
    if (!sameDxRow(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

// Clears exactly the glyph box of one row. The 17px pitch leaves only a single
// pixel between rows, so a taller clear would eat into its neighbour.
void clearDxRowBand(int16_t y) {
  tft.fillRect(kDxRowsX, y, kDxRowsW, tft.fontHeight(2), kBg);
}

void drawDxRowToTft(const DxRowText& row, int16_t y) {
  clearDxRowBand(y);
  drawLeft(row.freq, kDxColFreq, y, 2, kText);
  drawLeft(row.call, kDxColCall, y, 2, kAccent);
  drawLeft(row.mode, kDxColMode, y, 2, kText);
  drawLeft(row.time, kDxColTime, y, 2, kMuted);
}

void drawDxRowToSprite(const DxRowText& row, int16_t y) {
  dxScrollSprite.setTextDatum(TL_DATUM);
  dxScrollSprite.setTextColor(kDxPalText, kDxPalBg);
  dxScrollSprite.drawString(row.freq, kDxColFreq - kDxRowsX, y, 2);
  dxScrollSprite.setTextColor(kDxPalAccent, kDxPalBg);
  dxScrollSprite.drawString(row.call, kDxColCall - kDxRowsX, y, 2);
  dxScrollSprite.setTextColor(kDxPalText, kDxPalBg);
  dxScrollSprite.drawString(row.mode, kDxColMode - kDxRowsX, y, 2);
  dxScrollSprite.setTextColor(kDxPalMuted, kDxPalBg);
  dxScrollSprite.drawString(row.time, kDxColTime - kDxRowsX, y, 2);
}

bool ensureDxScrollSprite() {
  if (g_dxScrollSpriteReady) {
    return true;
  }

  dxScrollSprite.setColorDepth(4);
  g_dxScrollSpriteReady = dxScrollSprite.createSprite(kDxRowsW, kDxScrollSpriteH) != nullptr;
  if (!g_dxScrollSpriteReady) {
    Serial.println("DX scroll sprite allocation failed");
    return false;
  }
  dxScrollSprite.createPalette(g_dxScrollPalette, 16);
  return true;
}

// The scroll sprite is claimed once at startup and never handed back. Releasing
// it between pages used to look like good housekeeping, but the heap it was
// returned to is churned by the TLS clients behind four periodic fetches, and
// after a while no contiguous 23KB block was left. Every scroll then fell back
// to an instant redraw and the animation silently stopped for good.
//
// Only this sprite is reserved. The map sprite is left to allocate on demand:
// taking its 90KB up front splits the ESP32's segmented DRAM badly enough that
// TLS can no longer find a contiguous block and every HTTPS fetch fails.
void reserveDisplaySprites() {
  ensureDxScrollSprite();
  Serial.print("Scroll sprite reserved. Free heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.print(", largest block: ");
  Serial.println(ESP.getMaxAllocHeap());
}

// Pushes the visible window of the scroll sprite. Sprite rows are stored one
// after another at 4 bits per pixel, so a vertical window is just a byte offset
// into the buffer - no per-frame redraw of the text is needed.
void pushDxScrollFrame(int16_t progress) {
  const int32_t topRow = kDxRowPitch - progress;
  uint8_t* buffer = static_cast<uint8_t*>(dxScrollSprite.getPointer());
  if (buffer == nullptr) {
    return;
  }
  tft.pushImage(kDxRowsX, kDxRowsTop, kDxRowsW, kDxRowsH,
                buffer + ((topRow * kDxRowsW) >> 1), false, g_dxScrollPalette);
}

void finishDxScroll() {
  for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
    g_dxShownRows[i] = i < g_dxScrollEndCount ? g_dxScrollEndRows[i] : DxRowText();
  }
  g_dxShownCount = g_dxScrollEndCount;
  g_dxScrollActive = false;
  // Starts the gap before the next queued row, if any.
  g_dxQueueReadyMs = millis();
}

void stepDxScroll() {
  if (!g_dxScrollActive) {
    return;
  }

  const uint32_t nowMs = millis();
  if (nowMs - g_dxScrollFrameMs < kDxScrollFrameMs) {
    return;
  }
  g_dxScrollFrameMs = nowMs;

  g_dxScrollProgress += kDxScrollStepPx;
  if (g_dxScrollProgress > kDxRowPitch) {
    g_dxScrollProgress = kDxRowPitch;
  }
  pushDxScrollFrame(g_dxScrollProgress);
  if (g_dxScrollProgress == kDxRowPitch) {
    // The last frame lands on the final layout, so the screen already matches
    // the rows being adopted here.
    finishDxScroll();
  }
}

// Renders the incoming row above the rows currently on screen, then starts the
// frame-by-frame push. Row n of the sprite ends up one pitch lower on screen
// than row n-1 started, which is what makes the whole list appear to slide.
bool startDxScroll(const DxRowText* rows, uint8_t count) {
  if (!ensureDxScrollSprite()) {
    return false;
  }

  dxScrollSprite.fillSprite(kDxPalBg);
  drawDxRowToSprite(rows[0], kDxRowPad);
  for (uint8_t i = 0; i < g_dxShownCount; ++i) {
    drawDxRowToSprite(g_dxShownRows[i], kDxRowPad + (i + 1) * kDxRowPitch);
  }

  for (uint8_t i = 0; i < count; ++i) {
    g_dxScrollEndRows[i] = rows[i];
  }
  g_dxScrollEndCount = count;
  g_dxScrollProgress = 0;
  g_dxScrollActive = true;
  g_dxScrollFrameMs = millis() - kDxScrollFrameMs;
  return true;
}

// How many rows the new list has pushed onto the top of the shown list: 1 for a
// single telnet spot, more for a JSON batch. Returns -1 when the new list is not
// the shown list pushed down (rows reordered or edited in place), which has to
// be redrawn rather than scrolled.
int8_t dxRowShiftCount(const DxRowText* rows, uint8_t count) {
  if (g_dxShownCount == 0) {
    return -1;
  }

  // The smallest shift that lines the two lists up is the real one; a larger
  // shift would match too by simply pushing the overlap off the bottom.
  for (uint8_t shift = 0; shift <= count; ++shift) {
    const uint8_t overlap = count - shift;
    if (overlap > g_dxShownCount) {
      continue;
    }
    bool matches = true;
    for (uint8_t i = 0; i < overlap; ++i) {
      if (!sameDxRow(rows[shift + i], g_dxShownRows[i])) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return static_cast<int8_t>(shift);
    }
  }
  return -1;
}

void clearDxQueue() {
  g_dxQueueCount = 0;
  g_dxQueueIndex = 0;
  g_dxQueueTargetCount = 0;
}

bool dxQueuePending() {
  return g_dxQueueIndex < g_dxQueueCount;
}

// Holds the new rows back so they can be shown one at a time. They are queued
// oldest first, so the newest spot is the last to slide in and ends up on top.
void startDxQueue(const DxRowText* rows, uint8_t count, uint8_t shift) {
  for (uint8_t i = 0; i < shift; ++i) {
    g_dxQueueRows[i] = rows[shift - 1 - i];
  }
  g_dxQueueCount = shift;
  g_dxQueueIndex = 0;
  for (uint8_t i = 0; i < count; ++i) {
    g_dxQueueTargetRows[i] = rows[i];
  }
  g_dxQueueTargetCount = count;
  // Let the first row start straight away; the gap applies between rows.
  g_dxQueueReadyMs = millis() - kDxQueueGapMs;
}

void applyDxRowsInstantly(const DxRowText* rows, uint8_t count) {
  for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
    const int16_t y = kDxRowTextY + (i * kDxRowPitch);
    if (i < count) {
      if (!sameDxRow(rows[i], g_dxShownRows[i])) {
        drawDxRowToTft(rows[i], y);
      }
    } else if (i < g_dxShownCount) {
      clearDxRowBand(y);
    }
  }

  for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
    g_dxShownRows[i] = i < count ? rows[i] : DxRowText();
  }
  g_dxShownCount = count;
}

// Drives whatever the list is currently doing: stepping a scroll, waiting out
// the gap, or starting the next queued row. False means nothing is animating
// and the caller should put the list up without it.
bool serviceDxQueue() {
  if (g_dxScrollActive) {
    stepDxScroll();
    return true;
  }
  if (!dxQueuePending()) {
    return false;
  }
  if (millis() - g_dxQueueReadyMs < kDxQueueGapMs) {
    return true;
  }

  // Each queued row is a one-row shift of what is on screen, which is exactly
  // what the scroll animates.
  DxRowText next[kMaxDxSpots];
  next[0] = g_dxQueueRows[g_dxQueueIndex];
  uint8_t count = 1;
  for (uint8_t i = 0; i < g_dxShownCount && count < kMaxDxSpots; ++i) {
    next[count++] = g_dxShownRows[i];
  }

  if (!startDxScroll(next, count)) {
    return false;
  }
  ++g_dxQueueIndex;
  stepDxScroll();
  return true;
}

void finishDxQueueInstantly() {
  if (g_dxQueueTargetCount > 0) {
    applyDxRowsInstantly(g_dxQueueTargetRows, g_dxQueueTargetCount);
  }
  clearDxQueue();
}

// Works out whether the incoming list is the old one shifted down, and if so
// animates it in a row at a time; otherwise redraws. Shared by both spot pages
// - only one of them can be on screen, so they share this row state too.
void applyRowList(const DxRowText* rows, uint8_t count) {
  // A sequence already heading for this list just needs to keep running.
  if (dxQueuePending() && sameDxRowList(rows, count, g_dxQueueTargetRows, g_dxQueueTargetCount)) {
    if (!serviceDxQueue()) {
      finishDxQueueInstantly();
    }
    return;
  }

  if (g_dxScrollActive) {
    if (!dxQueuePending() && sameDxRowList(rows, count, g_dxScrollEndRows, g_dxScrollEndCount)) {
      stepDxScroll();
      return;
    }
    // Spots landed while the list was still moving. Snap to where the current
    // scroll was going and start again from there rather than queueing frames
    // behind a stale list.
    finishDxScroll();
  }
  clearDxQueue();

  if (sameDxRowList(rows, count, g_dxShownRows, g_dxShownCount)) {
    return;
  }

  const int8_t shift = dxRowShiftCount(rows, count);
  if (shift >= 1) {
    startDxQueue(rows, count, static_cast<uint8_t>(shift));
    if (serviceDxQueue()) {
      return;
    }
    clearDxQueue();
  }

  applyDxRowsInstantly(rows, count);
}

void updateDxRows(const DxSpotsData& dx) {
  if (dx.spotCount == 0) {
    g_dxScrollActive = false;
    clearDxQueue();
    drawCenteredField(g_lastDxEmpty, "No spots loaded", 92, 4, kMuted);
    for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
      g_dxShownRows[i] = DxRowText();
    }
    g_dxShownCount = 0;
    return;
  }

  if (g_lastDxEmpty.length() > 0) {
    tft.fillRect(0, 78, tft.width(), 40, kBg);
    g_lastDxEmpty = "";
  }

  const uint8_t count = dx.spotCount < kMaxDxSpots ? dx.spotCount : kMaxDxSpots;
  DxRowText rows[kMaxDxSpots];
  for (uint8_t i = 0; i < count; ++i) {
    rows[i] = makeDxRowText(dx.spots[i]);
  }
  applyRowList(rows, count);
}

// The park reference takes the slot the DX list uses for time, so POTA spots
// flow through exactly the same row and scroll machinery.
DxRowText makePotaRowText(const PotaSpot& spot) {
  DxRowText row;
  row.freq = truncateText(spot.frequency, 7);
  row.call = truncateText(spot.activator, 9);
  row.mode = truncateText(spot.mode, 4);
  row.time = truncateText(spot.reference, 8);
  return row;
}

void updatePotaRows(const PotaSpotsData& pota) {
  if (pota.spotCount == 0) {
    g_dxScrollActive = false;
    clearDxQueue();
    drawCenteredField(g_lastPotaEmpty, "No spots loaded", 92, 4, kMuted);
    for (uint8_t i = 0; i < kMaxDxSpots; ++i) {
      g_dxShownRows[i] = DxRowText();
    }
    g_dxShownCount = 0;
    return;
  }

  if (g_lastPotaEmpty.length() > 0) {
    tft.fillRect(0, 78, tft.width(), 40, kBg);
    g_lastPotaEmpty = "";
  }

  const uint8_t count = pota.spotCount < kMaxDxSpots ? pota.spotCount : kMaxDxSpots;
  DxRowText rows[kMaxDxSpots];
  for (uint8_t i = 0; i < count; ++i) {
    rows[i] = makePotaRowText(pota.spots[i]);
  }
  applyRowList(rows, count);
}

void drawFooter(const ClockSnapshot& snapshot) {
  tft.drawFastHLine(0, kFooterTop, tft.width(), kPanel);

  if (g_currentPage == kPageClock) {
    // No UTC field here - the clock page already shows a full UTC readout above.
    const String value = wifiStatusText(snapshot.wifiConnected) + "   " +
                         ntpStatusText(snapshot.timeValid) + "   " +
                         pageIndicator();
    if (value != g_lastFooterSimple) {
      tft.fillRect(0, kFooterTop + 1, tft.width(), kFooterHeight - 1, kBg);
      drawCentered(value, kFooterY, 2, kMuted);
      g_lastFooterSimple = value;
    }
    return;
  }

  // Each field redraws only its own fixed-width box, so a once-a-minute UTC
  // tick no longer blanks and repaints the whole footer line.
  drawLeftField(g_lastFooterUtc, footerUtcText(snapshot), kFooterXUtc, kFooterY, 2, kMuted,
               kFooterWUtc);
  drawLeftField(g_lastFooterWifi, wifiStatusText(snapshot.wifiConnected), kFooterXWifi, kFooterY,
               2, kMuted, kFooterWWifi);
  drawLeftField(g_lastFooterNtp, ntpStatusText(snapshot.timeValid), kFooterXNtp, kFooterY, 2,
               kMuted, kFooterWNtp);
  drawLeftField(g_lastFooterPage, pageIndicator(), kFooterXPage, kFooterY, 2, kMuted,
               kFooterWPage);
}

void formatTimes(const ClockSnapshot& snapshot, bool use12Hour) {
  if (!snapshot.timeValid) {
    strlcpy(utcBuffer, "--:--:--", sizeof(utcBuffer));
    strlcpy(localBuffer, "--:--:--", sizeof(localBuffer));
    strlcpy(dateBuffer, "Waiting for NTP", sizeof(dateBuffer));
    return;
  }

  tm utcTime;
  tm localTime;
  gmtime_r(&snapshot.epoch, &utcTime);
  localtime_r(&snapshot.epoch, &localTime);

  // The UTC readout stays 24-hour whatever the setting says; only the local
  // clock below it follows the 12/24 hour choice.
  strftime(utcBuffer, sizeof(utcBuffer), "%H:%M:%S", &utcTime);
  if (use12Hour) {
    strftime(localBuffer, sizeof(localBuffer), "%I:%M:%S %p", &localTime);
    // %I pads to two digits, which reads oddly before ten, so drop the zero.
    if (localBuffer[0] == '0') {
      memmove(localBuffer, localBuffer + 1, strlen(localBuffer));
    }
  } else {
    strftime(localBuffer, sizeof(localBuffer), "%H:%M:%S", &localTime);
  }
  strftime(dateBuffer, sizeof(dateBuffer), "%d %b %Y", &localTime);
}

String formatUptime(uint32_t seconds) {
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  const uint32_t secs = seconds % 60;
  snprintf(uptimeBuffer, sizeof(uptimeBuffer), "%02lu:%02lu:%02lu",
           static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(secs));
  return String("Uptime: ") + uptimeBuffer;
}

void drawClockPage(const ClockSnapshot& snapshot) {
  const AppSettings& settings = getSettings();
  formatTimes(snapshot, settings.clock12Hour);

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("UTC", kClockLabelY, 2, kMuted);
  }

  drawCenteredField(g_lastUtc, utcBuffer, kClockUtcY, kClockUtcFont, kAccent, -1, -1,
                    kClockUtcSize);
  drawSplitField(g_lastLocal, settings.timezoneLabel, localBuffer, kClockLocalY, 4, kText, 1,
                 kClockTextFont);
  drawCenteredField(g_lastDate, dateBuffer, kClockDateY, 4, kText, -1, -1, 1, kClockTextFont);
  const String stationText = settings.callsign.length() > 0
                                 ? settings.callsign + "   Locator: " + settings.locator
                                 : String("Locator: ") + settings.locator;
  drawCenteredField(g_lastLocator, stationText, kClockStationY, 4, kText, -1, -1, 1,
                    kClockTextFont);
  const String ipText = snapshot.wifiConnected ? String("IP: ") + WiFi.localIP().toString()
                                               : String("IP: --");
#if DISPLAY_W >= 480
  // Two short diagnostics share one row on the wide panel. That buys back the
  // vertical space the double-size local time and date rows need, and putting
  // them side by side is what the extra width is for.
  drawCenteredField(g_lastIp, ipText + "    " + formatUptime(snapshot.uptimeSeconds),
                    kClockIpY, 2, kMuted);
#else
  drawCenteredField(g_lastIp, ipText, kClockIpY, 2, kMuted);
  drawCenteredField(g_lastUptime, formatUptime(snapshot.uptimeSeconds), kClockUptimeY, 2,
                    kMuted);
#endif
  drawFooter(snapshot);
}

void drawPropagationPage(const ClockSnapshot& snapshot) {
  const PropagationData& propagation = getPropagationData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("HF Propagation", 4, 4, kAccent);
    tft.drawRect(4, kPropPanelATop, tft.width() - 8, kPropPanelAH, kPanel);
    tft.drawRect(4, kPropPanelBTop, tft.width() - 8, kPropPanelBH, kPanel);
    tft.drawFastVLine(kCondSplit1, kPropPanelBTop, kPropPanelBH, kPanel);
    tft.drawFastVLine(kCondSplit2, kPropPanelBTop, kPropPanelBH, kPanel);
    drawLeft("Band", 8, kPropHeadY, 2, kMuted);
    drawCenteredAt("Day", kCondDayX, kPropHeadY, 2, kMuted);
    drawCenteredAt("Night", kCondNightX, kPropHeadY, 2, kMuted);
  }

  drawTopReadingRows(g_lastPropSfiXray, g_lastPropSunspots, g_lastPropNoise, propagation);
  tft.drawFastHLine(4, kPropPanelBTop, tft.width() - 8, kPanel);

  drawConditionRow(g_lastPropBandA, "80m-40m", propagation.band8040Day, propagation.band8040Night,
                   kPropRowY);
  drawConditionRow(g_lastPropBandB, "30m-20m", propagation.band3020Day, propagation.band3020Night,
                   kPropRowY + kPropRowPitch);
  drawConditionRow(g_lastPropBandC, "17m-15m", propagation.band1715Day, propagation.band1715Night,
                   kPropRowY + 2 * kPropRowPitch);
  drawConditionRow(g_lastPropBandD, "12m-10m", propagation.band1210Day, propagation.band1210Night,
                   kPropRowY + 3 * kPropRowPitch);

  drawLeftField(g_lastPropUpdated, "Updated: " + propagation.updatedUtc, kPropUpdatedX,
                kPropStatusY, 2, kMuted, kPropUpdatedW);
  drawPropStatusField(g_lastPropStatus, "Status: " + propagation.status,
                      propagation.status == "OK" ? kAccent : kWarn);
  drawFooter(snapshot);
}

void drawVhfPage(const ClockSnapshot& snapshot) {
  const PropagationData& propagation = getPropagationData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("VHF Conditions", 4, 4, kAccent);
    tft.drawRect(4, kPropPanelATop, tft.width() - 8, kPropPanelAH, kPanel);
    tft.drawRect(4, kPropPanelBTop, tft.width() - 8, kPropPanelBH, kPanel);
  }

  drawTopReadingRows(g_lastPropSfiXray, g_lastPropSunspots, g_lastPropNoise, propagation);
  tft.drawFastHLine(4, kPropPanelBTop, tft.width() - 8, kPanel);

  drawVhfAuroraRow(g_lastVhfAurora, propagation.vhfAurora, propagation.vhfAuroraLat, kVhfRowY);
  drawVhfConditionRow(g_lastVhfEsEurope6m, "Es EU 6m", propagation.vhfEsEurope6m,
                      kVhfRowY + kVhfRowPitch);
  drawVhfConditionRow(g_lastVhfEsEurope4m, "Es EU 4m", propagation.vhfEsEurope4m,
                      kVhfRowY + 2 * kVhfRowPitch);
  drawVhfConditionRow(g_lastVhfEsEurope, "Es EU 2m", propagation.vhfEsEurope,
                      kVhfRowY + 3 * kVhfRowPitch);
  drawVhfConditionRow(g_lastVhfEsNorthAmerica, "Es NA 2m", propagation.vhfEsNorthAmerica,
                      kVhfRowY + 4 * kVhfRowPitch);

  drawLeftField(g_lastVhfUpdated, "Updated: " + propagation.updatedUtc, kPropUpdatedX,
                kPropStatusY, 2, kMuted, kPropUpdatedW);
  drawPropStatusField(g_lastVhfStatus, "Status: " + propagation.status,
                      propagation.status == "OK" ? kAccent : kWarn);
  drawFooter(snapshot);
}

void drawGreylinePage(const ClockSnapshot& snapshot) {
  const GreylineData& greyline = getGreylineData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
  }

  const String mapSignature = greyline.qth + "|" + greyline.latitude + "|" + greyline.longitude +
                              "|" + greyline.sunLatitude + "|" + greyline.sunLongitude +
                              "|" + String(greyline.valid ? "1" : "0");
  if (mapSignature != g_lastGreyMap) {
    drawGreylineMap(greyline);
    g_lastGreyMap = mapSignature;
  }
  drawLeftField(g_lastGreyQth, "QTH: " + greyline.qth, kGreyQthX, kMapTextRow1, 1, kText,
                kGreyQthW);
  drawLeftField(g_lastGreySunLat, "Sun: " + greyline.sunLatitude + "," + greyline.sunLongitude, kGreySunX, kMapTextRow1, 1, kText, kGreySunW);
  drawLeftField(g_lastGreySunrise, "Rise: " + greyline.sunriseUtc.substring(0, 5), kGreyRiseX, kMapTextRow2, 1, kText,
                kGreyRiseW);
  drawLeftField(g_lastGreySunset, "Set: " + greyline.sunsetUtc.substring(0, 5), kGreySetX, kMapTextRow2, 1, kText,
                kGreySetW);
  drawLeftField(g_lastGreyUtc, "UTC: " + greyline.utcTime.substring(0, 5), kGreyUtcX, kMapTextRow2, 1, kMuted,
                kGreyUtcW);
  drawLeftField(g_lastGreyStatus, "Status: " + greyline.status, kGreyStatusX, kMapTextRow3, 1,
                greyline.status == "Location invalid" ? kWarn : kText, kGreyStatusW);
  String greylineLabel = greyline.greyline;
  greylineLabel.replace(" greyline", "");
  drawLeftField(g_lastGreyline, "Greyline: " + greylineLabel, kGreyGreylineX, kMapTextRow3, 1,
                greyline.greyline == "Not near greyline" ? kMuted : kAccent, kGreyGreylineW);
  drawFooter(snapshot);
}

void drawPskPage(const ClockSnapshot& snapshot) {
  const PskReporterData& psk = getPskReporterData();
  const AppSettings& settings = getSettings();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
  }

  // The subsolar position is part of the signature so the terminator keeps up
  // with the sun rather than sitting still until the next report arrives.
  const GreylineData& greyline = getGreylineData();
  const String mapSignature = psk.callsign + "|" + String(psk.reportCount) + "|" +
                              String(psk.totalReports) + "|" + psk.updated + "|" + psk.status +
                              "|" + greyline.sunLatitude + "|" + greyline.sunLongitude;
  if (mapSignature != g_lastPskMap) {
    drawPskMap(psk);
    g_lastPskMap = mapSignature;
  }

  String heading;
  if (psk.callsign.length() == 0) {
    heading = "Set your callsign on the web settings page";
  } else {
    heading = psk.callsign + (settings.pskDirection == kPskWhoIHear ? " hears " : " heard by ");
    heading += String(psk.reportCount) + " grids / " + String(psk.totalReports) + " rpts";
    heading += ", last " + String(settings.pskWindowMinutes) + "m";
  }
  drawLeftField(g_lastPskHeading, heading, 14, kMapTextRow1, 1, kText, 292);

  String bestLine;
  if (psk.bestDistanceKm > 0) {
    bestLine = "Best: " + psk.bestCallsign + " " + psk.bestLocator + " " +
               String(psk.bestDistanceKm) + " km";
  } else {
    bestLine = "Best: --";
  }
  bestLine += "   Upd " + (psk.updated.length() > 0 ? psk.updated.substring(0, 5) : String("--"));
  drawLeftField(g_lastPskBest, bestLine, 14, kMapTextRow2, 1,
                psk.bestDistanceKm > 0 ? kAccent : kMuted,
                292);

  const String legendSignature = String(psk.bandMask) + "|" + psk.status;
  if (legendSignature != g_lastPskFooterLine) {
    drawPskBandLegend(psk, kMapTextRow3);
    g_lastPskFooterLine = legendSignature;
  }
  drawFooter(snapshot);
}

void drawDxPage(const ClockSnapshot& snapshot) {
  const DxSpotsData& dx = getDxSpotsData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("DX Spots", 4, 4, kAccent);
    drawLeft("Freq", 14, 24, 2, kMuted);
    drawLeft("Call", 82, 24, 2, kMuted);
    drawLeft("Mode", 170, 24, 2, kMuted);
    drawLeft("UTC", 230, 24, 2, kMuted);
  }

  updateDxRows(dx);

  drawLeftField(g_lastDxUpdated, "Updated: " + dx.updated, 8, 186, 2, kMuted, 144);
  drawLeftField(g_lastDxSource, "Source: " + dx.provider, 158, 186, 2,
                dx.source == "Last good" ? kWarn : kAccent, 158);
  drawLeftField(g_lastDxStatus, "Status: " + dx.status, 8, 204, 1,
                dx.status == "OK" || dx.status == "Connected" || dx.status == "Reading"
                    ? kAccent
                    : kWarn,
                308);
  drawFooter(snapshot);
}

void drawPotaPage(const ClockSnapshot& snapshot) {
  const PotaSpotsData& pota = getPotaSpotsData();

  if (g_pageDirty) {
    tft.fillScreen(kBg);
    drawCentered("POTA Spots", 4, 4, kAccent);
    drawLeft("Freq", kDxColFreq, 24, 2, kMuted);
    drawLeft("Call", kDxColCall, 24, 2, kMuted);
    drawLeft("Mode", kDxColMode, 24, 2, kMuted);
    drawLeft("Park", kDxColTime, 24, 2, kMuted);
  }

  updatePotaRows(pota);

  drawLeftField(g_lastPotaUpdated, "Updated: " + pota.updated, 8, 186, 2, kMuted, 144);

  String summary = "Status: " + pota.status;
  if (pota.totalSpots > 0) {
    summary += "   " + String(pota.spotCount) + "/" + String(pota.totalSpots);
    if (pota.filteredOut > 0) {
      summary += "   " + String(pota.filteredOut) + " filtered";
    }
  }
  drawLeftField(g_lastPotaStatus, summary, 8, 204, 1, pota.status == "OK" ? kAccent : kWarn, 308);
  drawFooter(snapshot);
}

void drawCurrentPage(const ClockSnapshot& snapshot) {
  if (g_currentPage != kPageDx && g_currentPage != kPagePota) {
    // A part-finished animation must not resume when the page comes back, but
    // the sprite itself is kept - see reserveDisplaySprites.
    g_dxScrollActive = false;
    clearDxQueue();
  }

  switch (g_currentPage) {
    case kPageClock:
      drawClockPage(snapshot);
      break;
    case kPagePropagation:
      drawPropagationPage(snapshot);
      break;
    case kPageVhf:
      drawVhfPage(snapshot);
      break;
    case kPageGreyline:
      drawGreylinePage(snapshot);
      break;
    case kPagePsk:
      drawPskPage(snapshot);
      break;
    case kPageDx:
      drawDxPage(snapshot);
      break;
    case kPagePota:
      drawPotaPage(snapshot);
      break;
    default:
      g_currentPage = kPageClock;
      drawClockPage(snapshot);
      break;
  }

  g_pageDirty = false;
}

void nextPage() {
  g_currentPage = static_cast<DashboardPage>((static_cast<uint8_t>(g_currentPage) + 1) % kPageCount);
  clearPageState();
  g_pageDirty = true;
}

void previousPage() {
  const uint8_t page = static_cast<uint8_t>(g_currentPage);
  g_currentPage = static_cast<DashboardPage>(page == 0 ? kPageCount - 1 : page - 1);
  clearPageState();
  g_pageDirty = true;
}

// Moves to the next page the rotation mask includes. Pages left out are
// skipped, the current one included, so a rotation still moves on from a page
// that was reached by hand. Stops short of a full lap so a mask holding only
// the page already showing leaves the screen alone rather than repainting it
// on every interval.
void advanceToNextIncludedPage(uint8_t mask) {
  for (uint8_t step = 1; step < kPageCount; ++step) {
    const uint8_t candidate =
        static_cast<uint8_t>((static_cast<uint8_t>(g_currentPage) + step) % kPageCount);
    if (mask & (1u << candidate)) {
      g_currentPage = static_cast<DashboardPage>(candidate);
      clearPageState();
      g_pageDirty = true;
      return;
    }
  }
}

void serviceAutoPageChange(uint32_t nowMs) {
  const AppSettings& settings = getSettings();
  if (!settings.autoPageChange || settings.autoPageMask == 0) {
    // Held at now while off, so switching it on starts a whole fresh interval
    // rather than firing straight away.
    g_lastPageChangeMs = nowMs;
    return;
  }
  const uint32_t dwellMs = static_cast<uint32_t>(settings.autoPageSeconds) * 1000UL;
  if (nowMs - g_lastPageChangeMs < dwellMs) {
    return;
  }
  // A row is part way through scrolling in. Cutting that off mid-slide reads
  // as a glitch, so the change waits for the list to settle. The dwell is
  // deliberately not restarted, so the page moves on the moment it does.
  if ((g_currentPage == kPageDx || g_currentPage == kPagePota) &&
      (g_dxScrollActive || dxQueuePending()) &&
      nowMs - g_lastPageChangeMs < dwellMs + kAutoPageScrollGraceMs) {
    return;
  }
  g_lastPageChangeMs = nowMs;
  advanceToNextIncludedPage(settings.autoPageMask);
}

// Backlight PWM is driven from two places now - a settings change and the
// night fade - so the ledc plumbing lives here rather than inside either.
void applyBacklight(uint8_t percent) {
#ifdef TFT_BL
  constexpr uint8_t kBacklightChannel = 0;
  constexpr uint32_t kBacklightFrequency = 5000;
  constexpr uint8_t kBacklightResolution = 8;

  const uint8_t brightness =
      constrain(percent, static_cast<uint8_t>(5), static_cast<uint8_t>(100));
  uint8_t duty = map(brightness, 0, 100, 0, 255);
#if TFT_BACKLIGHT_ON == LOW
  duty = 255 - duty;
#endif
  ledcSetup(kBacklightChannel, kBacklightFrequency, kBacklightResolution);
  ledcAttachPin(TFT_BL, kBacklightChannel);
  ledcWrite(kBacklightChannel, duty);
#else
  (void)percent;
#endif
}

// Blends the day and night levels by how far through the sunset (or sunrise)
// fade the sun is, so the backlight follows the light outside rather than
// stepping at one particular minute. Before NTP, or with no night dimming
// asked for, this is simply the day level.
uint8_t targetBrightnessPercent(time_t epoch) {
  const AppSettings& settings = getSettings();
  if (!settings.nightDimEnabled) {
    return settings.brightnessPercent;
  }
  const float night = greylineNightFraction(epoch, settings.nightFadeMinutes);
  const float day = static_cast<float>(settings.brightnessPercent);
  const float dim = static_cast<float>(settings.nightBrightnessPercent);
  return static_cast<uint8_t>(day + (dim - day) * night + 0.5f);
}

// Cheap enough to run every loop: the PWM is only touched on the whole-percent
// steps the fade actually crosses.
void serviceNightDimming(time_t epoch) {
  const uint8_t target = targetBrightnessPercent(epoch);
  if (target == g_appliedBrightnessPercent) {
    return;
  }
  g_appliedBrightnessPercent = target;
  applyBacklight(target);
}

uint16_t readTouchAxis(uint8_t command) {
  touchSpi.transfer(command);
  const uint16_t high = touchSpi.transfer(0x00);
  const uint16_t low = touchSpi.transfer(0x00);
  return ((high << 8) | low) >> 3;
}

bool readRawTouch(uint16_t& rawX, uint16_t& rawY) {
  if (digitalRead(kTouchIrq) == HIGH) {
    return false;
  }

  touchSpi.beginTransaction(SPISettings(kTouchFrequency, MSBFIRST, SPI_MODE0));
  digitalWrite(kTouchCs, LOW);
  delayMicroseconds(2);

  uint32_t xTotal = 0;
  uint32_t yTotal = 0;
  constexpr uint8_t kSamples = 4;
  for (uint8_t i = 0; i < kSamples; ++i) {
    xTotal += readTouchAxis(0xD0);
    yTotal += readTouchAxis(0x90);
  }

  digitalWrite(kTouchCs, HIGH);
  touchSpi.endTransaction();

  rawX = xTotal / kSamples;
  rawY = yTotal / kSamples;
  return rawX >= kTouchMin && rawX <= kTouchMax &&
         rawY >= kTouchMin && rawY <= kTouchMax;
}

int16_t scaleTouch(uint16_t value, int16_t size) {
  value = constrain(value, kTouchMin, kTouchMax);
  return static_cast<int16_t>(
      (static_cast<uint32_t>(value - kTouchMin) * (size - 1)) /
      (kTouchMax - kTouchMin));
}

bool getTouchPoint(uint16_t& x, uint16_t& y) {
  uint16_t rawX;
  uint16_t rawY;
  if (!readRawTouch(rawX, rawY)) {
    return false;
  }

  // Map the raw pair onto the panel in its unrotated orientation. The flip and
  // mirror compensation below then accounts for however MADCTL has since been
  // told to scan, which is a display setting the digitiser knows nothing about.
#if TOUCH_SWAP_XY
  int16_t screenX = scaleTouch(rawY, tft.width());
  int16_t screenY = scaleTouch(rawX, tft.height());
#else
  int16_t screenX = scaleTouch(rawX, tft.width());
  int16_t screenY = scaleTouch(rawY, tft.height());
#endif
#if TOUCH_INVERT_X
  screenX = tft.width() - 1 - screenX;
#endif
#if TOUCH_INVERT_Y
  screenY = tft.height() - 1 - screenY;
#endif
  x = constrain(screenX, 0, tft.width() - 1);
  y = constrain(screenY, 0, tft.height() - 1);

  // The touch controller is wired independently of the display, so flipping
  // the screen via MADCTL does not change what a physical tap reports here.
  // Mirror the point to match what is now visually on screen.
  if (getSettings().flip180) {
    x = tft.width() - 1 - x;
    y = tft.height() - 1 - y;
  }

  if (getSettings().mirror) {
    x = tft.width() - 1 - x;
  }

  return true;
}

void handleTouch() {
  uint16_t x;
  uint16_t y;
  const bool touched = getTouchPoint(x, y);
  const uint32_t nowMs = millis();

  if (touched && !g_touchWasDown && nowMs - g_lastTouchActionMs >= kTouchDebounceMs) {
    g_lastTouchActionMs = nowMs;
    // Any tap, whether it changes page or refreshes one, restarts the dwell so
    // an automatic change cannot pull the page away as it is being read.
    g_lastPageChangeMs = nowMs;
    if ((g_currentPage == kPagePropagation || g_currentPage == kPageVhf) &&
        x >= tft.width() / 3 && x <= (tft.width() * 2) / 3) {
      requestPropagationRefresh();
      if (g_currentPage == kPagePropagation) {
        g_lastPropStatus = "";
        drawLeftField(g_lastPropStatus, "Status: Refreshing", 166, 194, 2, kMuted);
      } else {
        g_lastVhfStatus = "";
        drawLeftField(g_lastVhfStatus, "Status: Refreshing", 166, 194, 2, kMuted);
      }
    } else if (g_currentPage == kPageDx &&
               x >= tft.width() / 3 && x <= (tft.width() * 2) / 3) {
      requestDxSpotsRefresh();
      g_lastDxStatus = "";
      drawLeftField(g_lastDxStatus, "Status: Refreshing", 8, 204, 1, kMuted, 308);
    } else if (g_currentPage == kPagePota &&
               x >= tft.width() / 3 && x <= (tft.width() * 2) / 3) {
      requestPotaSpotsRefresh();
      g_lastPotaStatus = "";
      drawLeftField(g_lastPotaStatus, "Status: Refreshing", 8, 204, 1, kMuted, 308);
    } else if (g_currentPage == kPagePsk &&
               x >= tft.width() / 3 && x <= (tft.width() * 2) / 3) {
      // The request is queued rather than run now: the PSKReporter module holds
      // its own five minute floor and will pick this up when that has elapsed.
      requestPskReporterRefresh();
    } else {
      const bool tappedLeft = x < tft.width() / 2;
      if (tappedLeft != getSettings().swapTouchNav) {
        previousPage();
      } else {
        nextPage();
      }
    }
  }

  g_touchWasDown = touched;
}
}

void displayBegin() {
  tft.init();
  tft.setRotation(kLandscapeRotation);
  reserveDisplaySprites();
  dxSpotsBegin();
  greylineBegin();
  propagationBegin();
  pskReporterBegin();
  potaSpotsBegin();

  pinMode(kTouchCs, OUTPUT);
  digitalWrite(kTouchCs, HIGH);
  pinMode(kTouchIrq, INPUT);
#if !defined(TOUCH_SHARES_DISPLAY_BUS)
  touchSpi.begin(kTouchSclk, kTouchMiso, kTouchMosi, kTouchCs);
#endif

  applyDisplaySettings();

  tft.fillScreen(kBg);
  clearPageState();
  g_pageDirty = true;
}

void displayUpdate(const ClockSnapshot& snapshot) {
  handleTouch();
  const bool dataChanged = refreshDxSpotsIfNeeded(snapshot.wifiConnected) |
                           refreshPropagationIfNeeded(snapshot.wifiConnected) |
                           refreshPskReporterIfNeeded(snapshot.wifiConnected) |
                           refreshPotaSpotsIfNeeded(snapshot.wifiConnected) |
                           updateGreylineData(snapshot.epoch, snapshot.timeValid);
  const uint32_t nowMs = millis();
  serviceNightDimming(snapshot.epoch);
  serviceAutoPageChange(nowMs);
  if (g_pageDirty || dataChanged || nowMs - g_lastRenderMs >= kRenderIntervalMs) {
    drawCurrentPage(snapshot);
    g_lastRenderMs = nowMs;
  } else if ((g_currentPage == kPageDx || g_currentPage == kPagePota) &&
             (g_dxScrollActive || dxQueuePending())) {
    // Advance the DX list between full page renders so the animation runs at
    // its own pace without blocking touch or the telnet reader.
    if (!serviceDxQueue()) {
      finishDxQueueInstantly();
    }
  }
}

void applyDisplaySettings() {
  const AppSettings& settings = getSettings();

  // TFT_eSPI's RGB/BGR order and orientation are normally fixed at compile
  // time. Write the ILI9341 MADCTL byte directly here so differently wired
  // CYD panels (wrong colour order, upside down, or needing a 90-degree
  // turn) can be corrected from the web settings page without rebuilding
  // firmware.
  // MV (row/column exchange) is a transpose, which on its own is a diagonal
  // mirror rather than a clean rotation. Pairing it with MX (this board's
  // base orientation) keeps that mirror; using MV alone instead gives a
  // proper 90-degree turn, so rotate90 swaps the base bit rather than adding
  // to it.
  uint8_t madctl = settings.rotate90 ? kIli9341MadctlMv : kIli9341MadctlMx;
  if (settings.flip180) {
    madctl ^= (kIli9341MadctlMx | kIli9341MadctlMy);
  }
  // A mirror is a single-axis flip, so it toggles whichever bit currently
  // controls the screen's horizontal axis. MV (rotate90) swaps row/column
  // meaning, so that axis is MY when rotated and MX otherwise.
  if (settings.mirror) {
    madctl ^= (settings.rotate90 ? kIli9341MadctlMy : kIli9341MadctlMx);
  }
  madctl |= (settings.swapRedBlueChannels ? kIli9341MadctlBgr : 0);

  tft.startWrite();
  tft.writecommand(kIli9341Madctl);
  tft.writedata(madctl);
  tft.endWrite();


  tft.invertDisplay(settings.invertColours);

  // Re-apply the backlight unconditionally: the panel registers were just
  // rewritten, and the night fade may want a different level than before.
  g_appliedBrightnessPercent = 0;
  serviceNightDimming(getClockSnapshot().epoch);

  clearPageState();
  g_pageDirty = true;
  g_lastPageChangeMs = millis();
}

uint8_t getCurrentDashboardPageNumber() {
  return static_cast<uint8_t>(g_currentPage) + 1;
}

uint8_t getAppliedBrightnessPercent() {
  return g_appliedBrightnessPercent;
}

const char* dashboardPageName(uint8_t pageIndex) {
  switch (pageIndex) {
    case kPageClock: return "Clock";
    case kPagePropagation: return "HF Propagation";
    case kPageVhf: return "VHF Conditions";
    case kPageGreyline: return "Greyline";
    case kPagePsk: return "PSKReporter";
    case kPageDx: return "DX Spots";
    case kPagePota: return "POTA Spots";
    default: return "";
  }
}

void displayShowMessage(const String& title, const String& subtitle) {
  tft.fillScreen(kBg);
  drawCentered(title, 96, 4, kAccent);
  drawCentered(subtitle, 132, 2, kMuted);
}

void requestDisplayRedraw() {
  clearPageState();
  g_pageDirty = true;
}
