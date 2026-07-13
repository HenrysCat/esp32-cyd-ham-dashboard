#pragma once

// TFT_eSPI setup for the common ESP32-2432S028R "Cheap Yellow Display".
// PlatformIO uses this file via:
//   build_flags = -D USER_SETUP_LOADED=1 -include include/User_Setup.h
//
// Arduino IDE users can copy these settings into the TFT_eSPI library's
// User_Setup.h, or configure User_Setup_Select.h to include an equivalent file.

#define ILI9341_DRIVER
// This CYD panel uses RGB channel order; without this, yellow renders as cyan
// and red as blue (green is unaffected).
#define TFT_RGB_ORDER TFT_RGB

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

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
