#pragma once

// Copy this file to include/app_config.h and enter your Wi-Fi credentials.
// Keep app_config.h private if you later put this project under version control.

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

// Phase 1 fixed locator. Later phases can move this into settings/storage.
#define MAIDENHEAD_LOCATOR "FF46"

// Optional JSON proxy for propagation data. Leave empty to use HamQSL XML direct.
#define PROPAGATION_JSON_URL ""

// Optional JSON endpoint for DX spots. Leave empty to use the default IZ3MEZ feed.
#define DX_SPOTS_URL ""

// Optional display defaults for boards that always need the same orientation
// fix, applied on first boot or after a factory reset. These can otherwise be
// set per-device from the web settings page instead. Uncomment to override.
// #define ROTATE90_DEFAULT true
// #define SWAP_RED_BLUE_DEFAULT true
// #define FLIP180_DEFAULT true
// #define MIRROR_DEFAULT true
// #define INVERT_COLOURS_DEFAULT true
// #define SWAP_TOUCH_NAV_DEFAULT true
