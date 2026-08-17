#pragma once

// TFT_eSPI setup for the 4.0" 320x480 ST7796S CYD variant (ESP32-WROOM-32E).
// PlatformIO uses this file via:
//   build_flags = -D USER_SETUP_LOADED=1 -include include/User_Setup_ST7796.h
//
// The display pins below are confirmed on real hardware. The one that differs
// from the 2.8" ESP32-2432S028R is the backlight, which moves from GPIO 21 to
// GPIO 27.
//
// SPI_FREQUENCY matters here: this panel produces nothing but random coloured
// pixels at the 40MHz the 2.8" board runs at, because the controller never
// completes its init sequence. 27MHz is stable. If a future board of this type
// shows the same noise, drop the clock before suspecting anything else.

#define ST7796_DRIVER

#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// The dashboard works in landscape, so it asks TFT_eSPI for a wide canvas and
// then sets the panel's scan order itself. These are the dimensions every page
// lays out against.
#define DISPLAY_W 480
#define DISPLAY_H 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1

#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH

// XPT2046 touch controller, confirmed on real hardware. Unlike the 2.8" board,
// it shares the display's SCLK/MOSI/MISO rather than having its own bus, which
// dashboard_display.cpp detects from these three defines matching the TFT ones.
// Rows per strip when the greyline map is composed. Must divide the map height
// (230) exactly; 46 gives five strips of 460x46x2 = 42,320 bytes, well inside
// the ~110KB largest free block this board reports.
#define MAP_BAND_ROWS 46

#define TOUCH_SCLK 14
#define TOUCH_MOSI 13
#define TOUCH_MISO 12
#define TOUCH_CS  33
#define TOUCH_IRQ 36

// Measured on hardware 2026-08-17 by pressing all four corners:
//   top-left (236,330)  top-right (283,3836)
//   bottom-left (3675,374)  bottom-right (3800,3900)
// Raw X therefore tracks screen Y and raw Y tracks screen X, so the axes are
// transposed, and both need inverting to land in the panel's unrotated frame.
// The range is widened past the measured 236-3938 so a hard corner press is
// clamped rather than rejected by readRawTouch's validity check.
#define TOUCH_RAW_MIN 200
#define TOUCH_RAW_MAX 3980
#define TOUCH_SWAP_XY 1
#define TOUCH_INVERT_X 1
#define TOUCH_INVERT_Y 1

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY  16000000
#define SPI_TOUCH_FREQUENCY 2500000
