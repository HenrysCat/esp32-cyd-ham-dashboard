# CYD Ham Dashboard

A HamClock-inspired ham radio dashboard for the ESP32-2432S028R Cheap Yellow Display.

**[Flash it in your browser with the Web Flasher](https://henryscat.github.io/)**

It provides a touch-controlled 320x240 landscape dashboard with UTC/local time, HamQSL propagation data, a greyline map, DX spots, Wi-Fi setup, and a local web settings page.

![ESP32 CYD Ham Dashboard showing the clock, HF propagation, greyline, and DX spots pages](assets/esp32-cyd-ham-dashboard.jpg)

## Features

- ESP32-2432S028R / CYD ILI9341 display support
- XPT2046 touch navigation
- Four dashboard pages:
  - Clock
  - HF Propagation from HamQSL
  - Greyline map with QTH marker, sun marker, terminator, sunrise/sunset, and day/night status
  - DX spots from JSON and/or a persistent Telnet DX Cluster connection
- Captive portal Wi-Fi setup, which automatically switches off a few seconds after the device confirms it has joined your Wi-Fi network (it can be switched back on from the web settings page if you need it again)
- Local web settings page on the device IP
- Hold the BOOT button on the back of the board for 5 seconds to factory reset all settings
- Optional mDNS address: `http://cyd-ham.local/`
- NTP time sync
- Configurable callsign, locator, timezone, data URLs, refresh intervals, and brightness
- Settings stored in ESP32 non-volatile preferences
- No LVGL, SD card, or external filesystem required

## Hardware

Tested target:

- ESP32-2432S028R “Cheap Yellow Display”
- ESP32-WROOM based board
- ILI9341 320x240 TFT
- XPT2046 resistive touch controller

The included TFT_eSPI setup uses this common CYD wiring:

| Signal | GPIO |
| --- | ---: |
| TFT MISO | 12 |
| TFT MOSI | 13 |
| TFT SCLK | 14 |
| TFT CS | 15 |
| TFT DC | 2 |
| TFT RST | -1 |
| TFT backlight | 21 |
| Touch SCLK | 25 |
| Touch MOSI | 32 |
| Touch MISO | 39 |
| Touch CS | 33 |
| Touch IRQ | 36 |

Some CYD variants use different pins. If the display is blank, white, mirrored, or touch is wrong, check your board revision and adjust `include/User_Setup.h` and the touch constants in `src/dashboard_display.cpp`.

### Setup hotspot and factory reset

On first boot (or whenever no Wi-Fi is configured), the device broadcasts a `CYD-HamClock-Setup` access point (password `hamclock`) so you can join it and open the captive portal to enter your Wi-Fi details. Once the device confirms it has joined your network, the hotspot automatically switches off. To bring it back later — for example to reach the settings page again without your router — check "Keep the setup hotspot switched on" in the Station section of the web settings page.

The board's BOOT button (GPIO0, on the back next to the USB connector) doubles as a factory reset button: hold it down for 5 seconds while the dashboard is running to wipe all saved settings (Wi-Fi credentials, callsign, locator, timezone, data sources, brightness) and reboot to defaults. The screen shows a countdown while the button is held; release early to cancel.

## Flashing A Release Binary

If a `.bin` firmware file is attached to a GitHub release, you can flash it without building from source.

Install `esptool`:

```sh
python -m pip install esptool
```

Put the board into normal USB flashing mode, then flash the release binary. Replace `COM5` and the filename as needed:

```sh
esptool.py --chip esp32 --port COM5 --baud 460800 write_flash -z 0x10000 firmware.bin
```

If the release includes bootloader and partition binaries, use the release instructions for those exact offsets. For PlatformIO-built firmware, the application binary normally goes at `0x10000`.

After flashing, the board will start its setup access point if Wi-Fi is not configured.

## Building From Source

Install:

- VS Code
- PlatformIO extension
- USB serial driver for your ESP32 board if required

Clone the repository and open it in VS Code.

Copy the example config:

```sh
copy include\app_config.example.h include\app_config.h
```

On macOS/Linux:

```sh
cp include/app_config.example.h include/app_config.h
```

You may optionally edit `include/app_config.h` before flashing, but Wi-Fi and dashboard settings can also be configured from the captive portal or local web page.

Build:

```sh
pio run
```

Upload:

```sh
pio run -t upload
```

Open Serial Monitor:

```sh
pio device monitor
```

## First Boot And Wi-Fi Setup

The ESP32 starts a setup access point while also trying to connect to saved Wi-Fi credentials.

- AP name: `CYD-HamClock-Setup`
- AP password: `hamclock`
- Portal address: `http://192.168.4.1`

Join the setup AP from a phone or computer. The captive portal may open automatically. If it does not, browse to:

```text
http://192.168.4.1
```

The setup page lets you configure:

- Callsign
- Wi-Fi SSID and password
- Timezone preset, label, and POSIX timezone rule
- Maidenhead locator
- Propagation data source
- DX source mode, JSON URL, and Telnet host/port
- Refresh intervals
- Backlight brightness

Settings are saved to ESP32 Preferences and persist after reboot.
The saved Wi-Fi password is never displayed in the settings page. Leave the password field blank to keep it, or use the checkbox to clear it for an open network.

## Local Web Settings

When connected to Wi-Fi, the same settings page is available on your LAN:

```text
http://device-ip/
```

The clock page shows the current IP address. If mDNS starts successfully, this address may also work:

```text
http://cyd-ham.local/
```

Available routes:

- `GET /` - settings and status page
- `POST /save` - save settings
- `GET /status` - JSON status
- `POST /reboot` - restart the ESP32

This web UI is intended for a trusted local network. It does not include authentication.

## Touch Controls

- Tap left side: next page
- Tap right side: previous page
- Tap centre on HF Propagation page: manual propagation refresh
- Tap centre on DX Spots page: manual DX refresh

The footer shows Wi-Fi status, NTP status, and current page number.

## Dashboard Pages

### Clock

Shows:

- Large UTC time
- Configured local time
- Date
- Callsign and Maidenhead locator
- Device IP
- Uptime
- Wi-Fi/NTP/footer status

### HF Propagation

Fetches HamQSL data directly by default:

```text
https://www.hamqsl.com/solarxml.php
```

Displays:

- SFI
- A index
- K index
- X-Ray
- Sunspots
- Solar wind speed (SW) and magnetic-field Bz
- Geomag
- Noise
- Aurora
- HamQSL band condition groups for day/night
- Last update time and status

An optional JSON proxy URL can be configured from the web page.

### Greyline

Calculates locally using the configured Maidenhead locator and NTP time.

Displays:

- Bitmap-style world map
- QTH marker
- Sun/subsolar marker
- Day/night terminator
- Night-side shading
- Sunrise, sunset, UTC time
- Location/daylight/greyline status

Changing the Maidenhead locator refreshes the Greyline calculations and map immediately; no reboot is required.

### DX Spots

Supports three source modes:

- `Auto` (default): try JSON first, then use Telnet if JSON fails or has no usable spots
- `JSON`: use only the configured JSON feed
- `Telnet`: maintain a connection to the configured DX Cluster

The default JSON endpoint is:

```text
https://web.cluster.iz3mez.it/spots.json
```

Displays recent spots with:

- Frequency
- Callsign
- Mode, inferred where possible
- UTC time
- Last update time and status

The default Telnet cluster is `dxspots.com:7300`. The configured station callsign is used for login, or `NOCALL` if no callsign is set. Both sources are configurable from the web settings page.

The device keeps the last good spot list when a refresh or connection fails. The DX page shows whether the active data came from JSON, Telnet, or the last good result.

## Configuration Files

### `include/app_config.h`

Local default settings. Copy from `include/app_config.example.h`.

Useful defaults:

```cpp
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#define MAIDENHEAD_LOCATOR "FF46"
#define PROPAGATION_JSON_URL ""
#define DX_SPOTS_URL ""
```

Values saved through the captive portal or web settings page override most defaults at runtime.
When no DX URL has been saved, `DX_SPOTS_URL` is used if configured; otherwise the public IZ3MEZ endpoint is used.

### `include/User_Setup.h`

TFT_eSPI display configuration for the CYD hardware.

PlatformIO includes it automatically via:

```ini
build_flags =
  -D USER_SETUP_LOADED=1
  -include include/User_Setup.h
```

## Timezones

The web settings page includes common timezone presets. The firmware uses POSIX timezone strings.

UK default:

```text
GMT0BST-1,M3.5.0/1,M10.5.0/2
```

UTC:

```text
UTC0
```

If your country is not listed, choose `Custom POSIX TZ` and enter a POSIX rule manually.

## Data Refresh

Default refresh intervals:

- HF Propagation: 15 minutes
- DX JSON: 5 minutes
- DX Telnet: persistent connection with reconnect attempts limited to once every 30 seconds
- Greyline calculations: once per minute
- Clock: once per second

Propagation and DX refresh intervals can be changed in the web settings page.

## Project Structure

```text
include/
  User_Setup.h          TFT_eSPI CYD pin setup
  app_config.example.h  Example local config
  greyline_map.h        Embedded Greyline map bitmap

src/
  main.cpp              App entry point
  connectivity.*        Wi-Fi, NTP, timezone handling
  settings.*            Preferences-backed settings
  setup_portal.*        Captive portal and LAN web settings
  dashboard_display.*   TFT UI, touch, page rendering
  propagation.*         HamQSL fetch and parsing
  greyline.*            Solar and greyline calculations
  dx_spots.*            DX JSON fetch plus Telnet connection and parsing
```

## Notes And Limits

- The setup AP remains available while the device runs.
- The web UI is for trusted LAN use only.
- Telnet reading is non-blocking; connection attempts use a short bounded timeout.
- SD card storage is not required.
- LVGL is not used.
- The embedded Greyline map uses flash space; current firmware size is close to the default app partition limit.

## Troubleshooting

### Display is blank or white

Check that your CYD uses the same display pins as `include/User_Setup.h`.

### Touch is inaccurate

The touch calibration constants are in `src/dashboard_display.cpp`. CYD touch panels vary slightly.

### Time does not sync

Check Wi-Fi status and confirm your network allows NTP. The footer shows `NTP OK` when synced.

### Web page does not open

Check the IP shown on the clock page and browse to:

```text
http://that-ip/
```

If `cyd-ham.local` does not resolve, use the IP address instead.

### Propagation or DX shows fetch failed

Confirm Wi-Fi is connected and that your network allows HTTPS requests to the configured data source.
