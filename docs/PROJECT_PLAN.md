# nowtube — Project Plan

## End Goal

A Nextube that is a genuinely better living room device than stock:

- **Self-contained** — no Raspberry Pi, no companion app, no external services required
- **Clock + ambient modes** — auto-cycling display with solid NTP+RTC time sync, current conditions, and richer `TODAY` / `FORECAST` information modes
- **Web configuration UI** — set WiFi, weather, timezone, brightness, and backlight from any browser on the local network
- **OTA firmware updates** — flash new firmware over WiFi, no USB cable required
- **Visual quality** — nixie tube / flip-clock aesthetic via Nixie One Google Font with easy font swapping
- **Configurable backlight** — Normal, Breathable, and Mixed LED modes with color cycling
- **A game** — Tube Invaders: 6-lane vertical shooter spanning all 6 displays using the three front buttons

The ESP32 is the sole controller. Business logic, weather fetching, configuration, and the web interface all live on-device.

---

## What We Are NOT Building

- Raspberry Pi orchestration layer
- Volumio or music integration
- YouTube / social subscriber counters
- Pomodoro timer, countdown timer, or alarm clock
- Album / image slideshow

---

## Architecture

The firmware follows a clean service-oriented architecture. See
[`docs/target-architecture.md`](target-architecture.md) for the design overview.

Key decisions:
1. ESP32 is the sole product controller — no external backend drives display state
2. Web server is a local config/status surface only — no remote orchestration endpoints
3. LVGL is owned by exactly one task
4. Weather and network services publish events; they do not touch UI directly
5. New product logic goes into extracted services/controllers, not `main.cpp`

---

## Hardware Reference

**Buttons (fully working):**

| Button | GPIO | Tap | Long-press |
|---|---|---|---|
| Left | GPIO4 | Cycle display mode | Enter / exit Tube Invaders |
| Middle | GPIO2 | Cycle brightness (100%→70%→40%→20%→5%→0%) | Toggle recovery AP (nowtube-setup / 192.168.4.1) |
| Right | GPIO15 | Cycle LED color palette | Cycle backlight mode |

**Display constants:** 80×160px digits, 76×37px info images, 80×162px physical LCD
**LED cap:** 200/255 max RGB value, 6 individually addressable WS2812
**Default brightness:** 60/100 for both LCD and LEDs

---

## Projects

### Project 0 — Dev Environment ✅ COMPLETE

Build and flash the existing open source firmware, verify everything works.

---

### Project 1 — Mode State Machine + Button Rewiring ✅ COMPLETE

`DisplayMode` enum and `ModeManager` singleton. Left button cycles CLOCK/TODAY/FORECAST; long-press resets.

---

### Project 2 — Enhanced REST API ✅ COMPLETE

REST endpoints for mode, display, LEDs, brightness. The web server serves as a local config/status surface (see Project 6).

---

### Project 3 — Nixie Font Digit Rendering ✅ COMPLETE

Switched from SPIFFS JPEG to compiled-in **Nixie One** Google Font at 120px for digits and 60px for AM/PM. Font `.c` files in `main/fonts/`. Visual result closely matches the nixie tube aesthetic.

---

### Project 4 — On-Device Weather Service ✅ COMPLETE

**Goal:** Weather data is fetched directly by the ESP32. The display auto-cycles between
CLOCK (50s) → TODAY (10s) → FORECAST (10s) → repeat.

**ESP32 side:**
- `weather_service` fetches from Open-Meteo on a configurable schedule (default: every 10 min). No API key required.
- Single combined HTTP request returns both current conditions and 5-day forecast in one response
- Weather config (city name, lat/lon, units) stored in NVS via `config_service`
- Service publishes callbacks for conditions and forecast; `display_controller` handles the UI update
- Mode auto-cycle driven by `esp_timer` in `app_boot.cpp`

**Display cycling:**
- CLOCK → TODAY → FORECAST → repeat (configurable dwell times; set dwell to 0 to skip a mode)
- Button press overrides and holds until next auto-cycle
- If no successful weather fetch, weather modes show last known data

**Files added/modified (actual):**
- `main/weather_service.cpp/.h` — HTTP fetch, timer scheduling, config management
- `main/weather_parser.cpp/.h` — pure Open-Meteo URL construction and combined response parsing (host-testable; no ESP-IDF deps)
- `main/mode_policy.cpp/.h` — pure auto-cycle next-state logic (host-testable)
- `main/app_boot.cpp` — auto-cycle timer and weather callbacks (extracted from main.cpp)
- `main/webserver.cpp` — weather config endpoints

**Success criteria:** Display cycles clock → conditions → forecast automatically. Weather refreshes on schedule. Device remains on clock if WiFi is down.

---

### Project 4.5 — Architecture Hardening ✅ COMPLETE

A production-readiness pass before Project 5. All issues surfaced by a full codebase audit.

**Completed:**
- Weather globals race condition eliminated: `display_controller` owns all weather state behind a FreeRTOS mutex; `main.cpp` no longer holds shared weather buffers
- `status_service` writes protected by spinlock; reads return a consistent snapshot copy
- Boot sequence fails soft on SPIFFS mount errors (logs and continues); all production `assert()` calls in driver hot paths replaced with logged errors and graceful returns
- `backlight_service` seeds `rand()` from hardware RNG (`esp_random()`) at init
- Lat/lon config validated on save; out-of-range coords clear the location flag and fall back to city lookup
- `clock::~clock()` now cancels pending LVGL delayed-start timers before deleting objects
- LCD `lcd_select()` short-circuits when same display is selected consecutively (avoids SPI device remove/add on repeat calls)
- Boot mode from `config_service` enforced at startup
- Dead code removed: `blinks_enabled`, `blink_led` forward-declaration, `async_tx_in_flight`, `weather_ui_update_cb`
- SPIFFS `max_files` raised to 10; heap stats (`free_heap`, `min_free_heap`) added to `/api/status`
- Weather/auto-cycle race guarded: mode re-checked under LVGL lock before painting weather in non-clock-only mode

**Verified on hardware:** clean boot, NTP sync, weather fetch, `/api/status` returning correct heap stats.

**Runtime reliability fix (post-P4.5, same branch):**

A deterministic crash was found and fixed during hardware validation:

- **Symptom:** `Mode set → UNKNOWN` + `tlsf double-free` assert on CLOCK→TODAY auto-cycle transition; device was in a crash loop (boot_count rising rapidly).
- **Root cause:** `lv_async_call()` called from the `esp_timer` task context without holding the LVGL mutex. LVGL v8 inserts the timer into `_lv_timer_ll` *before* writing `info->cb` and `info->user_data`; the LVGL task fired the callback in this uninitialised window, reading a garbage `DisplayMode` → `"UNKNOWN"` and then `delete`-ing a non-heap pointer.
- **Fix:** Removed the `cycle_arg` / `cycle_async_cb` indirection. `auto_cycle_cb` now applies the mode transition directly under `gui_lvgl_lock()` / `gui_lvgl_unlock()`, matching the established pattern used by `on_weather_updated` and `on_time_changed`. Two complete clean auto-cycle rounds verified on hardware after the fix.
- **File:** `main/app_boot.cpp` — `auto_cycle_cb()`

---

### Project 5 — OTA Firmware Updates ✅ COMPLETE

**Goal:** Flash new firmware over WiFi — no USB cable required after initial flash.

**What was built:**
- `partitions.csv` updated to A/B OTA layout: `otadata` (0x10000) + `ota_0` (0x20000, 2MB) + `ota_1` (0x220000, 2MB) + `spiffs` (0x420000, ~12MB)
- `POST /api/ota/upload` — browser uploads raw binary directly; device streams it chunk-by-chunk into the inactive OTA partition via `esp_ota_ops`, then calls `esp_ota_end()` which verifies the SHA-256 embedded in the image header before marking the partition bootable. No local HTTP server required.
- `GET /api/ota/status` — polls OTA state (`idle` / `downloading` / `verifying` / `complete` / `failed`) and progress percentage; safe to call from any task via spinlock-protected snapshot
- Firmware Update section in the web UI: file picker + upload progress bar via XHR; progress bar auto-hides 5 seconds after completion
- Rollback safety: if the new firmware crashes on boot, ESP-IDF's `otadata` mechanism automatically reverts to the previous slot
- SHA-256 image integrity: `esp_ota_end()` verifies the hash embedded in the image header before `esp_ota_set_boot_partition()` is called; a corrupted upload results in `ESP_ERR_OTA_VALIDATE_FAILED` — the device never boots a bad image

**To flash over WiFi (after first USB flash):**
1. Build: `source ~/esp/esp-idf/export.sh && idf.py build`
2. Open `http://<device-ip>` in a browser
3. Scroll to **Firmware Update**, pick `build/nowtube.bin`, click **Flash Firmware**

**Success criteria:** New firmware flashes and boots over WiFi. Failed or corrupted flash rolls back cleanly. ✓

---

### Project 6 — Web Configuration UI ✅ COMPLETE

**Goal:** A clean, responsive web UI served by the device itself for all user-facing configuration.

**Shipped:**
- Plain HTML/CSS/JS, no framework — assets served from SPIFFS
- `GET /api/config` / `POST /api/config` — config read/write
- `GET /api/status` — live status panel (uptime, WiFi, IP, last weather sync, firmware version, heap)
- **UI sections:** device status, WiFi credentials, weather location (city geocoding or manual lat/lon), temperature units (°C/°F), timezone, brightness slider, ambient cycle dwell times, LED backlight (Normal/Breathable/Mixed + color), **Panel 4 metric (humidity or US AQI)**, firmware update (browser upload, no local server), reboot
- `POST /api/spiffs/upload?name=<file>.png` — replace SPIFFS icon assets OTA without USB
- `POST /api/reboot` — soft reboot

**Security note:** No authentication. All endpoints are local-network only. Do not expose port 80 to the internet.

---

### Project 7 — Google Fonts Pipeline (Formalize Tooling)

**Goal:** Make it trivial to swap the display font to any Google Font with a single command.

`tools/font_convert.py` already exists and works. This project:
- Ensures the tool is documented, tested, and works reliably with current lv_font_conv
- Adds a curated list of recommended fonts with preview notes
- Verifies the generated files integrate cleanly into the build
- Documents the process in `tools/README.md`

```bash
# Example: swap to DSEG7 Classic
python3 tools/font_convert.py "DSEG7 Classic" --sizes 40 60 100 120 --charset clock
# Output: main/fonts/dseg7_classic_120.c, etc.
# Prints the CMakeLists.txt lines to add
```

**Success criteria:** Any developer can swap the clock font to a Google Font in under 5 minutes following the docs.


---

### Project 8 — 5-Day Forecast Display ✅ SUBSTANTIALLY COMPLETE (v0.4)

**Shipped (v0.4):**
- `FORECAST` mode shows 5-day outlook across 6 panels: panel 0 = HI/LO legend, panels 1–5 = one day each
- Each day panel shows: day code top half, then condition icon revealed at dwell/2 via one-shot LVGL phase timer
- High (orange) / low (cyan) temperatures; Space Mono 44 px throughout
- Data: Open-Meteo `/v1/forecast` combined request (no API key); same fetch as current conditions
- Auto-cycle: CLOCK 50 s → TODAY 10 s → FORECAST 10 s → repeat; 12-hour soak passed (rev 0.2)
- Condition icons: sunny, partly cloudy, cloudy, foggy, snowy, rainy, thunder, wind — PNG assets in SPIFFS
- WMO code mapping covers all Open-Meteo `weather_code` values

**Deferred (planned):**
- LED alert strip (one LED per day — thunderstorm/freeze/rain/snow/heat alerts)
- Precipitation % per panel
- "Best day" highlight (panel 5 scoring)
- 24-hour precip sub-view


---

### Project 9 — TODAY Mode ✅ SUBSTANTIALLY COMPLETE (v0.4)

**Shipped (v0.4):**

6-panel fixed layout, Space Mono 44 px throughout:

| Panel | Content | Icon |
|-------|---------|------|
| 0 | Weekday (MON/TUE/…) | — |
| 1 | Month name | — |
| 2 | Day number + current weather icon | condition PNG (sunny/pcloudy/cloudy/foggy/snowy/rainy/thunder/wind) |
| 3 | Wind speed + direction abbreviation | wind.png |
| 4 | Humidity % **or** US AQI (user-selectable) | drop.png or air.png |
| 5 | Next sun event (sunrise or sunset time) | sunset.png |

- Data: Open-Meteo `/v1/forecast` combined request (same as FORECAST, no extra API call)
- AQI: separate Open-Meteo `/v1/air-quality` call, made only when `panel_humidity_metric = "aqi"` is configured
- AQI label color-coded via EPA breakpoints (green/yellow/orange/red/purple/maroon) using LVGL recolor
- Icons: 64×64 px PNG assets in SPIFFS; replaceable OTA via `POST /api/spiffs/upload`
- Config field `panel_humidity_metric` ("humidity" | "aqi") — NVS-persisted, web UI toggle
- Font: Space Mono 44 px compiled in (nixie still used for CLOCK digits)

**Product principle:** Calm living-room briefing — one fact per panel, readable from across the room.

**Deferred (planned):**
- Moon phase panel
- Tide level (NOAA CO-OPS data source)
- Configurable panel layout via web UI


---

### Project 10 — Tube Invaders ✓ COMPLETE

**Goal:** A 6-lane vertical shooter where each portrait display is one independent lane. Enemies fall from the top; the player ship sits at the bottom of the active lane, moves left/right between lanes with the outer buttons, and fires upward with the middle button.

The vertical orientation is a natural fit for the portrait panels and gives the middle button a direct gameplay role.

**Canvas layout — one lane per display:**

```
Display 0   Display 1   Display 2   Display 3   Display 4   Display 5
┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
│ enemy ↓ │ │ enemy ↓ │ │ enemy ↓ │ │ enemy ↓ │ │ enemy ↓ │ │ enemy ↓ │
│         │ │         │ │         │ │         │ │         │ │         │
│  80×162 │ │         │ │         │ │         │ │         │ │         │
│         │ │         │ │         │ │         │ │         │ │         │
│ ship ▲  │ │         │ │  ←active lane→      │ │         │ │         │
└─────────┘ └─────────┘ └─────────┘ └─────────┘ └─────────┘ └─────────┘
```

**Button mapping:**

| Button | Action |
|---|---|
| Left | Move ship one lane left |
| Right | Move ship one lane right |
| Middle | Fire projectile upward |

**MVP rules:**
- One ship, locked to the bottom row of the active display
- One projectile in-flight at a time; BLOCKED sound plays if fire attempted while active
- One enemy, spawns at top, falls at constant speed
- Hit = enemy removed, score +1; enemy reaches bottom = game over
- Speed increases every 8 kills (4 levels, capped); restart on any button after game over
- LEDs: active lane lit, hit = flash, game over = red sweep
- High score persisted in NVS namespace `"game"`, key `"hi_score"` (u32)

**Collision:** Lane-index match + y-coordinate overlap. No pixel-perfect needed.

**Architecture (shipped):** `game_task` (FreeRTOS, priority 4) runs pure game logic at 20 fps, snapshots state under a mutex, then renders directly under `gui_lvgl_lock()`. Sound managed by `sound_manager` service (FreeRTOS task, DAC_CHAN_0/GPIO25).

**Files:**
```
main/displays/game/
  game_logic.h / .cpp     ← pure logic (host-testable, no ESP-IDF/LVGL)
  game_display.h / .cpp   ← FreeRTOS task, LVGL rendering, LED updates, NVS hi-score
main/services/
  sound_manager.h / .cpp  ← DAC cosine tone sequencer
```

**Success criteria:** Player moves ship across 6 lanes, fires projectiles, enemies fall and die or trigger game over. Score increments. High score persists across reboots. LEDs react. Runs without watchdog resets. ✓

---

---

## Developer Tools

### tools/font_convert.py — Google Font → LVGL converter

```bash
# Install once
npm install -g lv_font_conv

# Convert DSEG7 Classic at all standard sizes
python3 tools/font_convert.py "DSEG7 Classic" --sizes 40 60 100 120 --charset clock

# Full ASCII for future scrolling text support
python3 tools/font_convert.py "Share Tech Mono" --sizes 80 --charset full

# See recommended fonts
python3 tools/font_convert.py --list-fonts
```

Output `.c` files land in `main/fonts/`. See `tools/README.md` for full documentation.

---

## Repository Structure

```
nowtube/
  tests/                       ← host-side unit tests (no hardware required)
  main/
    app_boot.cpp / .h          ← boot coordinator
    mode_policy.cpp / .h       ← pure auto-cycle policy (host-testable)
    weather_parser.cpp / .h    ← pure Open-Meteo URL/parse helpers (host-testable)
    config_validation.cpp / .h ← pure config validation/migration (host-testable)
    models/                    ← plain data structs (no ESP-IDF/LVGL deps)
    services/                  ← config, weather, backlight, status, sound
    controllers/
      display_controller.h / .cpp
    displays/
      clock/
      game/
        game_logic.h / .cpp    ← pure logic (host-testable)
        game_display.h / .cpp  ← FreeRTOS task + LVGL rendering
    fonts/                     ← compiled LVGL font .c files
    drivers/                   ← lcds, leds, wifi, touchpads, rtc
  components/                  ← ESP-IDF components
  tools/
  docs/
    target-architecture.md
    hardware-notes.md
    menu-roadmap.md
  docs/PROJECT_PLAN.md         ← this file
```
