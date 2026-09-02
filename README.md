# nowtube

![build](https://github.com/emuck/nowtube/actions/workflows/build.yml/badge.svg)

Open source firmware for the **Rotrics Nextube** clock — self-contained, configurable,
and a clean alternative to the stock firmware.

Based on [previoustube](https://github.com/previoustube/previoustube) by Ian Levesque. See [ATTRIBUTION.md](ATTRIBUTION.md).

---

## What It Does

- **Clock** — fast static time display with a curated picker: Nixie One, Space Mono, Atkinson Hyperlegible, or Aldrich
- **TODAY** — 6-panel ambient display: weekday, month/day + weather icon, day number, wind, humidity (or AQI), and next sun event
- **FORECAST** — 5-day outlook across 6 panels; day codes first, then condition icons revealed mid-dwell via phase timer
- **Web UI** — browser-based configuration, a live six-panel mirror, and firmware update served directly by the device
- **OTA Updates** — browser-upload and curl-based OTA; no local HTTP server required after first USB flash
- **Google Fonts** — one-command font swapping via `tools/font_convert.py`
- **Game** — Tube Invaders: 6-lane vertical shooter across all 6 displays; long-press left to enter/exit

No Raspberry Pi, no companion app, no external services required. The device works standalone.

---

## Buttons

| Button | Tap | Long Press |
|---|---|---|
| Left (GPIO4) | Cycle display mode | Enter / exit Tube Invaders |
| Middle (GPIO2) | Cycle LCD brightness | — |
| Right (GPIO15) | Cycle LED color palette | Cycle backlight mode |

---

## Architecture

The ESP32 is the sole controller. It fetches weather directly, manages display state and
mode cycling, and serves a local web UI for configuration. No companion device or service
is required.

```
Browser (local)
     │
     │  HTTP  GET / POST /api/config
     │        GET /api/status
     ▼
┌───────────────────────────────────┐
│           Nextube (ESP32)         │
│                                   │
│  display_controller               │
│  weather_service ─────────────────┼──►  Open-Meteo  (weather)
│  config_service (NVS)             │     NOAA         (tides)
│  ui_server (local only)           │     USNO         (astronomical)
│                                   │
│  6× LCD panels                    │
│  6× WS2812 LEDs                   │
│  3× touch buttons                 │
└───────────────────────────────────┘
```

---

## Project Status

| # | Project | Status |
|---|---|---|
| 0 | Dev environment + build | ✅ Complete |
| 1 | Mode state machine + buttons | ✅ Complete |
| 2 | Enhanced REST API | ✅ Complete |
| 3 | Nixie font digit rendering | ✅ Complete |
| 4 | On-device weather service + auto-cycle | ✅ Complete |
| 5 | OTA firmware updates over WiFi | ✅ Complete |
| 6 | Web configuration UI | ✅ Complete |
| 7 | Curated clock-font catalog + browser Look Studio | ✅ Complete |
| 8 | FORECAST mode (5-day ambient display) | ✅ Substantially complete (v0.4) |
| 9 | TODAY mode (current conditions display) | ✅ Substantially complete (v0.4) |
| 10 | Tube Invaders (game) | ✅ Complete |
| 11 | Wi-Fi recovery fallback + offline indicator | ✅ Complete |
| 12 | Live Panel Mirror | ✅ Complete |

See [docs/user-guide.md](docs/user-guide.md) for the full user guide (setup, modes, buttons, OTA, fonts, recovery).
See [docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md) for full detail on each project.
See [docs/PRODUCT_ROADMAP.md](docs/PRODUCT_ROADMAP.md) for the focused product direction and next priorities.

### Hardware Validation

The following areas have been verified on a real device running current firmware:

| Area | Result | Notes |
|---|---|---|
| Cold boot, NTP sync, RTC fallback | ✅ Pass | Boot completes cleanly; RTC used before NTP; `last_reset_reason` and `boot_count` accurate |
| Wi-Fi connect and status reporting | ✅ Pass | `retry_count` increments on failures; `wifi.connected` and `ip` fields accurate |
| Bad Wi-Fi credentials → recovery fallback | ✅ Pass | Device returns to `nowtube-setup` with on-device instructions after a 30-second connection timeout |
| Weather fetch success | ✅ Pass | Open-Meteo fetch (no API key), NVS cache, and display update all working |
| Bad location config → no crash | ✅ Pass | Fetch errors logged; display holds last known value |
| Auto-cycle CLOCK→TODAY→FORECAST | ✅ Pass | After `lv_async_call` race fix (see Known Issues under Web Configuration); 12-hour soak confirmed |
| TODAY mode — all 6 panels | ✅ Pass | Weekday / month+icon / day / wind / humidity / sun-event; Space Mono 44 px |
| FORECAST mode — two-phase icons | ✅ Pass | Day codes first half of dwell; condition icons revealed at dwell/2 |
| AQI panel — fetch and display | ✅ Pass | Open-Meteo air-quality API; EPA color-coded label; immediate fetch on enable |
| AQI/humidity toggle via web UI | ✅ Pass | Switch persists in NVS; icon and value swap correctly |
| OTA success (URL-based) | ✅ Pass | SHA-256 verified; device reboots into new firmware |
| OTA corrupt image → rollback | ✅ Pass | `esp_ota_end()` rejects bad image; current firmware stays intact |
| OTA unreachable URL | ✅ Pass | Returns `failed` state; subsequent requests succeed |
| OTA duplicate request while in progress | ✅ Pass | Returns `busy` while downloading; not blocked after completion |
| Mid-session AP drop → auto-reconnect | ✅ Pass | 28 retries over 71 s; no reboot; reconnects cleanly; weather resumes; NTP re-syncs via internal `IP_EVENT_STA_GOT_IP` handler. Required SNTP double-init fix (see Known Issues). |
| Wi-Fi recovery: middle long-press entry | ✅ Pass | AP starts, SSID visible, config UI reachable at 192.168.4.1; saving credentials restarts into STA mode automatically |
| Wi-Fi recovery: boot with no saved SSID | ✅ Pass | Full erase + comment-only wifi.txt; device boots directly into recovery AP, configures via web UI, and restarts automatically |
| Panel Mirror | ✅ Pass | `/panels` provides a live read-only view of CLOCK, TODAY, and FORECAST data across six virtual tubes |
| GAME mode — enter/exit, play, sounds | ✅ Pass | Long-press left enters/exits; ship moves, fires, collision detected; FIRE/HIT/GAME_OVER/RESTART/BLOCKED sounds confirmed; high score persists across reboot |

---

## Building (Ubuntu / Linux)

### 1. Prerequisites

```bash
sudo apt-get install -y git wget flex bison gperf python3 python3-pip \
  python3-venv cmake ninja-build ccache libffi-dev libssl-dev \
  dfu-util libusb-1.0-0 python3-serial

# Add yourself to the dialout group for USB serial access
sudo usermod -a -G dialout $USER
# Log out and back in after this
```

### 2. Install ESP-IDF v5.5.3

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && git checkout v5.5.3
git submodule update --init --recursive
./install.sh esp32
```

### 3. Clone and configure

```bash
git clone --recursive https://github.com/emuck/nowtube.git
cd nowtube

# Optional one-time local bootstrap credentials (ignored by git, migrated to NVS on first boot)
cp main/spiffs/wifi.sample.txt main/spiffs/wifi.txt
nano main/spiffs/wifi.txt   # set ssid= and psk=
```

### 4. Build and flash

This first flash is the **stock firmware replacement** path. It is a **USB full
flash** and writes the bootloader, partition table, app image, and SPIFFS
assets in one operation. Stock firmware users must use this path first; the
browser OTA workflow does not exist until nowtube is already running on the
device.

```bash
source ~/esp/esp-idf/export.sh

# Find your device port
ls /dev/ttyUSB* /dev/ttyACM*

idf.py -p /dev/ttyUSB0 build flash monitor
```

### 5. First run

The device will:
1. Display the time using the RTC (if previously set)
2. Migrate local bootstrap credentials/config from ignored files into NVS, if present
3. Connect to WiFi and sync via NTP
4. Update the RTC for next boot
5. Fetch weather and begin auto-cycling between CLOCK (50 s), TODAY (10 s), and FORECAST (10 s)

After the first successful boot, remove any local `main/spiffs/wifi.txt` or
`main/spiffs/weather.txt` files from your workspace so real secrets do not linger
on disk or get staged accidentally.

---

### 6. Running the host-side tests

A subset of the firmware logic (config validation, Open-Meteo weather/forecast parsing, mode-cycle policy) is extracted into ESP-IDF-free modules and covered by host-side unit tests. These run on any Linux/macOS machine with a C++17 compiler — no hardware or ESP-IDF toolchain needed.

```bash
# One-time dependency
sudo apt-get install -y libcjson-dev   # Ubuntu/Debian

# Build and run
cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Debug
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

These same tests run automatically in CI on every push (see `.github/workflows/build.yml`).

---

## Building (macOS)

Follow the [ESP-IDF macOS setup guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup.html).
The workflow is identical to Linux above, using `/dev/cu.usbserial-*` instead of `/dev/ttyUSB*`.

---

## Web Configuration

Once connected to WiFi, browse to `http://<device-ip>/` to configure:

- WiFi credentials
- Weather location (city geocoding or manual lat/lon)
- Temperature units (°C / °F)
- Timezone
- Display brightness and ambient cycle dwell times
- Backlight mode (Normal / Breathable / Mixed) and LED color
- **Panel 4 metric** — humidity (default) or US Air Quality Index (AQI)
- Firmware update (browser file upload, no local HTTP server needed)

`GET /api/config` does not echo stored Wi-Fi passwords back to the browser.
Credentials remain in NVS and can only be replaced by posting new values.

API surface:

- `GET /api/status` — live status snapshot (uptime, Wi-Fi, heap, last fetch)
- `GET /panels` — browser Panel Mirror for the six virtual tubes in each display mode
- `GET /api/panels` — read-only JSON data behind the Panel Mirror
- `GET /api/config` / `POST /api/config` — read/write device configuration
- `POST /api/ota/upload` — upload firmware binary directly from browser
- `GET /api/ota/status` — poll OTA progress
- `POST /api/spiffs/upload?name=<filename>.png` — replace a SPIFFS icon asset without USB reflash (see security note below)
- `POST /api/reboot` — reboot the device

**Security note:** The web server binds to all interfaces on port 80 with no authentication. `POST /api/spiffs/upload` accepts `.png`, `.html`, `.js`, and `.css` files (name validated: no path separators, max 32 chars, allowed extension only) and writes them directly to the mounted SPIFFS partition. This is intentionally a local-network tool. Do not expose the device's HTTP port to the internet.

Known issues / gaps:

- **Auto-cycle race fix:** `lv_async_call()` is not safe to call from the `esp_timer` task without holding the LVGL mutex (LVGL v8 inserts the timer into the internal list before writing the callback pointer, creating a window where the LVGL task fires it with uninitialised data). The `auto_cycle_cb` function was rewritten to apply mode transitions directly under `gui_lvgl_lock()` instead. The crash was a deterministic double-free on CLOCK→TODAY→FORECAST transitions; two complete cycles run cleanly after the fix.
- **OTA URL length check:** `OTA_URL_MAX` is set to 496 so that a request carrying a 496-char URL produces a 505-byte body — within the 512-byte `read_body()` limit — and the explicit `url too long` rejection fires correctly. URLs longer than ~503 chars still produce `bad json` (body truncation fires first), but all oversized requests are safely rejected.
- **SNTP double-init on reconnect (fixed):** `on_wifi_connected()` called `sntp_init()` on every reconnect; `esp_netif_sntp_init()` returns `ESP_ERR_INVALID_STATE` if already running, causing a panic and reboot. Fixed with a `s_sntp_initialized` guard flag — SNTP re-syncs automatically on reconnect via its internal `IP_EVENT_STA_GOT_IP` handler without needing a second init.
- **Breathing backlight:** Current breathing profile is tuned for gentler motion (`2s` rise, `3s` fall, `100ms` update cadence).

---

## OTA Firmware Updates

Browser-upload OTA is the primary method for **app image updates** once the
device is already running nowtube — select `build/nowtube.bin` in the web UI's
Firmware Update section and click **Flash Firmware**. No local HTTP server required.

curl-based OTA is also supported:

```bash
curl -X POST http://<device-ip>/api/ota/upload \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @build/nowtube.bin
```

Poll `GET /api/ota/status` for progress (`idle` / `downloading` / `verifying` / `complete` / `failed`).

The OTA implementation:
- Streams the binary chunk-by-chunk into the inactive OTA partition
- Verifies the SHA-256 image hash via `esp_ota_end()` before marking the partition bootable
- Reboots only after a successful write; a corrupted upload leaves the current firmware intact
- ESP-IDF rollback-safe: if the new firmware fails to boot, the previous slot is restored automatically

**Important:** OTA updates only the app binary (`nowtube.bin` at partition
`ota_0/ota_1`). The SPIFFS partition (icons, web assets) is at a separate
address and is **not** updated by OTA. Use `POST /api/spiffs/upload` to replace
individual icon files, `tools/upload_spiffs_assets.py` to push a full release
asset set over Wi-Fi, or perform a full `idf.py flash` via USB to replace the
entire SPIFFS image.

### Which update path should I use?

- **Stock firmware → nowtube:** USB full flash required.
- **Older nowtube release → new release with same `asset_rev`:** app OTA is enough.
- **Older nowtube release → new release with different `asset_rev` (such as `0.6`):**
  app OTA **plus** SPIFFS asset update, or USB full flash.
- **Unsure what is on the device:** use the USB full-flash path.

### OTA vs. SPIFFS asset updates

These are currently **different interfaces**:

- The built-in **Firmware Update** section in the web UI uploads only the app
  binary via `POST /api/ota/upload`.
- The helper script uploads only SPIFFS asset files. It does **not** upload
  `nowtube.bin`.

```bash
python3 tools/upload_spiffs_assets.py <device-ip>
```

The helper script uses only Python 3 standard-library modules and is supported
on macOS, Linux, and Windows 11 with Python 3.

Recommended public upgrade flow for an asset-changing release such as `0.6`:
1. Upload the new firmware via the web UI or `POST /api/ota/upload`
2. Run `python3 tools/upload_spiffs_assets.py <device-ip>`
3. Refresh `GET /api/status` / the web UI and confirm the expected asset set is present

---

## Changing Fonts

The `tools/font_convert.py` script converts any Google Font to LVGL-ready `.c` files in one command:

```bash
# Install once
npm install -g lv_font_conv

# Convert a font at Nowtube's standard clock sizes
python3 tools/font_convert.py "Atkinson Hyperlegible" --sizes 48 60 120 --charset clock+

# See recommended fonts
python3 tools/font_convert.py --list-fonts
```

Output `.c` files land in `main/fonts/`. To expose one in the device picker,
also add it to the curated catalog and theme map; `tools/README.md` documents
the review criteria and workflow.

### Preview the Curated Collection

To compare the current clock fonts before a firmware build, run the local
Look Studio with Python 3—no packages or ESP-IDF setup required:

```bash
# macOS / Linux
python3 tools/look-studio/serve.py

# Windows
py tools\look-studio\serve.py
```

It opens a browser preview automatically. See
[tools/look-studio/README.md](tools/look-studio/README.md) for port and
network options.

---

## Repository Structure

```
nowtube/
├── main/                   # ESP32 firmware (C++17, ESP-IDF)
│   ├── controllers/        # display_controller, input_controller
│   ├── displays/clock/     # Clock display + flapper animation
│   ├── displays/today/     # TODAY ambient mode (6-panel current conditions)
│   ├── displays/forecast/  # FORECAST ambient mode (5-day with phase timer)
│   ├── displays/game/      # Tube Invaders game (logic + display + NVS hi-score)
│   ├── drivers/            # Hardware drivers: LCD, LED, touch, WiFi
│   ├── fonts/              # Pre-compiled LVGL font files
│   ├── models/             # device_config, status_snapshot
│   ├── services/           # config, status, backlight, diagnostics, sound services
│   ├── spiffs/             # Assets flashed to SPIFFS (images, sample config)
│   ├── app_boot.cpp/.h     # Boot coordinator (all subsystem init)
│   ├── mode_policy.cpp/.h  # Pure auto-cycle policy (host-testable)
│   ├── weather_parser.cpp/.h # Pure Open-Meteo URL/parse helpers (host-testable)
│   ├── conditions_parser.cpp/.h # Current-conditions JSON parser (host-testable)
│   ├── forecast_parser.cpp/.h   # Forecast JSON parser (host-testable)
│   └── *.cpp / *.h         # Remaining app code
├── tests/                  # Host-side unit tests (no hardware required)
├── components/             # ESP-IDF components (RTC, NeoPixel, touch, etc.)
├── tools/                  # Developer tools (font converter, etc.)
├── fonts-src/              # Font source files (OTF + licenses)
├── docs/                   # Architecture, hardware, and user docs
└── ATTRIBUTION.md          # Credits and upstream licensing
```

---

## License

MIT License — see [LICENSE](LICENSE).

Portions copyright © 2023 Ian Levesque, used and modified under the MIT License.
New contributions copyright © 2026 Martin Raumann.

See [ATTRIBUTION.md](ATTRIBUTION.md) for full third-party credits.
