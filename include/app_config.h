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
