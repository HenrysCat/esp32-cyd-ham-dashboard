#pragma once

// Enter your Wi-Fi credentials here.
// If this project is committed to git later, do not commit real credentials.

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// Phase 1 fixed locator. Later phases can move this into settings/storage.
#define MAIDENHEAD_LOCATOR "FF46"

// Optional JSON proxy for propagation data. Leave empty to use HamQSL XML direct.
#define PROPAGATION_JSON_URL ""

// Optional JSON endpoint for DX spots. Leave empty to use the default IZ3MEZ feed.
#define DX_SPOTS_URL ""

// Display defaults for each board's wiring, applied on first boot or after a
// factory reset. Once saved via the web settings page these are overridden by
// the value stored on the device, so changing these later has no effect until
// the next factory reset.
//
// The two panels need different values, and the driver macro from the
// User_Setup header that platformio.ini force-includes says which board this
// build targets, so the right set is picked automatically.
#if defined(ST7796_DRIVER)
// 4.0" 320x480 ST7796S, landscape with the USB socket on the left. Gives
// MADCTL 0xE8 (MY | MX | MV | BGR), which is TFT_eSPI's own rotation 3 for this
// controller. Confirmed on hardware 2026-08-17.
#define ROTATE90_DEFAULT true
#define FLIP180_DEFAULT true
#define SWAP_RED_BLUE_DEFAULT true
#else
// 2.8" 240x320 ILI9341 (ESP32-2432S028R).
#define ROTATE90_DEFAULT true
#define MIRROR_DEFAULT true
#define INVERT_COLOURS_DEFAULT true
#endif
