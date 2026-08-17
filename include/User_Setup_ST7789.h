#pragma once

// Alternate TFT_eSPI setup for CYD boards that use an ST7789 panel instead
// of ILI9341. Untested placeholder: same pin mapping as include/User_Setup.h
// with the driver swapped and the color inversion ST7789 panels commonly
// need. If colors or alignment are off on real hardware, try toggling
// TFT_INVERSION_ON/OFF or adding row/col offsets here.
//
// PlatformIO uses this file via the esp32-2432s028r-st7789 env in
// platformio.ini:
//   build_flags = -D USER_SETUP_LOADED=1 -include include/User_Setup_ST7789.h

#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_RGB
#define TFT_INVERSION_ON

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// The dashboard works in landscape, so it asks TFT_eSPI for a wide canvas and
// then sets the panel's scan order itself. These are the dimensions every page
// lays out against.
#define DISPLAY_W 320
#define DISPLAY_H 240

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1

#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

// XPT2046 touch controller pins on most CYD boards. Touch is not used in
// phase 1, but defining CS here keeps the setup ready for later page switching.
#define TOUCH_SCLK 25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CS  33
#define TOUCH_IRQ 36

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  16000000
#define SPI_TOUCH_FREQUENCY 2500000
