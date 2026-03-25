# nowtube — Key Concepts & Lingo

A plain-English reference for the embedded / ESP-IDF / LVGL concepts that
come up regularly in this codebase. Assumes no prior embedded experience.

---

## Flash Memory and the Partition Table

The ESP32-WROVER-E has **16 MB of flash** — think of it as the device's SSD.
It cannot be written byte-by-byte like RAM; it must be erased in blocks before
new data is written, which is why corrupted flash requires a full erase.

Flash is divided into named **partitions** (defined in `partitions.csv`).
Each partition has a type, a start address (offset from the beginning of
flash), and a size. The bootloader reads the partition table at address
`0x8000` to know where to find everything else.

Our partition layout:

| Name | Purpose |
|------|---------|
| `nvs` | Non-Volatile Storage — small key/value config that survives power loss |
| `phy_init` | Wi-Fi / Bluetooth calibration data written by the factory |
| `otadata` | Tracks which OTA slot should boot next |
| `ota_0` | First firmware slot (2 MB) |
| `ota_1` | Second firmware slot (2 MB) — receives OTA updates |
| `spiffs` | Read-only file system for web assets (~12 MB) |

---

## SPIFFS

**SPIFFS** (SPI Flash File System) is a small file system that lives in the
`spiffs` partition. It lets the firmware read files with normal `fopen` /
`fread` calls, using paths like `/spiffs/index.html`.

We use SPIFFS to store the web configuration UI (`index.html`, `app.css`,
`app.js`) and display assets (JPEG digit images, background PNGs). These files
are bundled into a binary blob at build time by `spiffsgen.py` and flashed to
the `spiffs` partition.

SPIFFS is *not* a full POSIX file system — it has no directories (the `/`
character in filenames is just part of the name), no permissions, and limited
concurrent-write safety. It is optimized for small embedded devices with NOR
flash.

Because SPIFFS is a separate partition from the firmware, updating the
firmware via OTA does not touch SPIFFS — web assets stay in place.

---

## NVS (Non-Volatile Storage)

**NVS** is ESP-IDF's key-value store, backed by the `nvs` flash partition.
Think of it as a tiny persistent dictionary that survives reboots.

We use NVS (via `config_service`) to persist:
- Wi-Fi SSID and password
- Weather city name, lat/lon, units (no API key — Open-Meteo is free)
- Display brightness, boot mode, timezone
- Auto-cycle dwell times per mode
- Boot count (diagnostics)
- Last successful weather value (shown at boot before the first fetch)

NVS stores values as named keys under a named namespace. Values survive
firmware OTA updates because NVS lives in its own partition.

---

## FreeRTOS and Tasks

The ESP32 runs **FreeRTOS** — a real-time operating system. Unlike a desktop
OS, FreeRTOS is cooperative/preemptive at the task level: you create tasks
(lightweight threads) that run concurrently, each with its own stack.

Common FreeRTOS primitives used here:

| Primitive | Purpose |
|-----------|---------|
| `xTaskCreate` | Spawn a new concurrent task (gives it a name, stack size, priority) |
| `vTaskDelay` | Sleep for N milliseconds without burning CPU |
| `SemaphoreHandle_t` | Mutual exclusion (mutex) — only one task can "take" it at a time |
| `portMUX_TYPE` | Spinlock for very brief critical sections (< ~1 µs) — disables interrupts |
| `QueueHandle_t` | Thread-safe message passing between tasks |

**Stack overflow** is a real concern on embedded devices — there's no virtual
memory and no guard page. If a task runs out of stack, the device usually
crashes with a `Guru Meditation Error`. We use heap allocation for large
buffers (the 4 KB OTA download buffer) instead of putting them on the stack.

---

## Spinlocks vs Mutexes

Two ways to protect shared data:

**Spinlock (`portMUX_TYPE`):** Disables interrupts and spins until the lock is
free. Ultra-fast but the critical section must complete in microseconds.
Used for reading/writing a small struct (e.g., `s_ota_status`, the weather
mutex in `status_service`). Never call `malloc`, I/O, or FreeRTOS API inside.

**Mutex (`SemaphoreHandle_t`):** Sleeps the task until the lock is free.
Slower but safe for longer operations. Used in `display_controller` to
protect the weather state that the OTA and HTTP tasks both touch.

**LVGL lock (`gui_lvgl_lock`):** A special mutex that gates all LVGL
(GUI-drawing) calls. LVGL is single-threaded by design — only the task that
holds the LVGL lock may call any `lv_*` function. Other tasks must use
`lv_async_call` to post work to the LVGL task, or acquire the lock first.

---

## SPI and the LCD Displays

**SPI** (Serial Peripheral Interface) is a high-speed 4-wire bus:
- MOSI — Master Out, Slave In (data to display)
- MISO — Master In, Slave Out (not used for write-only displays)
- SCLK — clock
- CS — Chip Select (one per device, tells the device "this byte is for you")

The 6 ST7735 LCD panels share a single SPI bus (`SPI2_HOST`) but each has its
own `CS` pin. The ESP32 SPI master driver supports only 3 active CS slots
simultaneously (`SOC_SPI_MAX_CS_NUM = 3`), so `lcds.cpp` adds and removes
device handles at runtime, with a short-circuit optimization to skip the
remove/add when the same display is selected twice in a row.

---

## RMT (Remote Control Transceiver)

The **RMT** peripheral is a flexible pulse-generator / decoder originally
designed for TV infrared remotes. It outputs precisely-timed pulses in
hardware, making it ideal for driving **WS2812** LEDs, which use a 1-wire
protocol where each bit is encoded as a specific on/off pulse ratio.

We use RMT to drive all 6 WS2812 LEDs on a single GPIO pin. The hardware
handles the tight timing requirements (300 ns precision) without blocking the
CPU.

---

## I2C and the RTC

**I2C** (Inter-Integrated Circuit) is a 2-wire bus (SDA + SCL) for slower
peripheral communication. The PCF8563 real-time clock chip is connected via
I2C. It keeps accurate time even when the ESP32 is powered off (backed by a
coin cell or supercap on the board).

On boot the firmware reads the RTC to set the system clock immediately, before
NTP sync completes. When NTP syncs, the corrected time is written back to the
RTC for the next boot.

---

## NTP

**NTP** (Network Time Protocol) syncs the ESP32's software clock to internet
time servers (pool.ntp.org). ESP-IDF's `sntp` component handles this in the
background. The system calls `settimeofday()` when a sync succeeds, which
triggers our `DISPATCH_EVENT_TIME_CHANGED` event — the clock display then
updates and the corrected time is written to the RTC.

---

## LVGL

**LVGL** (Light and Versatile Graphics Library, v8.3.9) is the GUI framework.
It manages:
- **Objects** (`lv_obj_t *`) — widgets like labels, images, buttons
- **Styles** — positioning, color, padding
- **Timers** (`lv_timer_t`) — periodic callbacks that run in the LVGL tick
- **Animations** — smooth transitions

Key rule: **only one task may call LVGL at a time**. We use `gui_lvgl_lock()`
before any `lv_*` call and `gui_lvgl_unlock()` after. This prevents race
conditions on the widget tree.

LVGL drives the displays through a **flush callback** — when LVGL finishes
rendering a region, it calls `lcd_flush_cb`, which calls `lcd_blit_rect` to
DMA the pixel buffer over SPI to the correct LCD.

---

## ESP-IDF Components and CMake

**ESP-IDF** is Espressif's official framework for the ESP32. It provides
drivers (SPI, I2C, RMT, GPIO), networking (Wi-Fi, HTTP client/server, SNTP),
storage (NVS, SPIFFS, FAT), and more — all as **components**.

Each component lives in `esp-idf/components/<name>` and exposes headers via
its own `CMakeLists.txt`. Our firmware component (`main/CMakeLists.txt`) lists
the source files; the `main` component has implicit access to all ESP-IDF
components by default.

Build commands:
```bash
source ~/esp/esp-idf/export.sh   # sets up PATH, IDF_PATH, etc.
idf.py build                     # compile
idf.py -p /dev/ttyUSB0 flash     # flash to device
idf.py -p /dev/ttyUSB0 monitor   # serial console (Ctrl+] to exit)
idf.py -p /dev/ttyUSB0 flash monitor  # flash then monitor in one command
```

---

## OTA (Over-the-Air Updates)

**OTA** lets us flash new firmware over Wi-Fi — no USB cable required after
the first flash. ESP-IDF implements this with an A/B slot scheme:

1. The device always boots from one slot (`ota_0` or `ota_1`), indicated by
   the `otadata` partition.
2. When a new firmware is downloaded, it is written to the *inactive* slot.
3. `esp_ota_end()` verifies the **SHA-256** hash embedded in the image header.
   If the hash doesn't match (corrupted download), the function returns an
   error and the inactive slot is never activated.
4. If verification passes, `esp_ota_set_boot_partition()` updates `otadata`
   to point at the new slot, then the device reboots.
5. If the new firmware crashes on boot (e.g., watchdog reset within the first
   few seconds), the bootloader detects this and reverts `otadata` back to the
   previous slot — **automatic rollback**.

The first flash (via USB) must use `idf.py erase-flash flash` whenever the
partition table changes. Subsequent updates can be done entirely over Wi-Fi.

---

## esp_timer

**`esp_timer`** is ESP-IDF's high-resolution timer. It fires callbacks in a
dedicated high-priority task (not an ISR), so it is safe to call most ESP-IDF
APIs from within a timer callback.

We use `esp_timer` for:
- Auto-cycling display modes (CLOCK → TODAY → FORECAST → repeat, with configurable dwell times)
- The weather fetch schedule (default every 10 minutes)
- The backlight breathing animation tick (100 ms interval)

Do not call LVGL functions directly from an `esp_timer` callback — use
`lv_async_call` to post work to the LVGL task instead.

---

## REST API Quick Reference

All endpoints are served by `esp_http_server` on port 80.

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/` | Web config UI (HTML) |
| GET | `/api/status` | Full device status |
| GET | `/api/config` | Read all config |
| POST | `/api/config` | Write config (partial updates OK) |
| GET | `/api/mode` | Current display mode |
| POST | `/api/mode` | Set display mode (`CLOCK`/`TODAY`/`FORECAST`) |
| GET | `/api/brightness` | Current brightness % |
| POST | `/api/brightness` | Set brightness % |
| POST | `/api/ota/upload` | Upload firmware binary directly from browser |
| GET | `/api/ota/status` | Poll OTA progress |
| GET | `/api/backlight` | Read backlight mode and LED color |
| POST | `/api/backlight` | Set backlight mode / LED color |
| POST | `/api/weather/refresh` | Trigger an immediate weather fetch |
| POST | `/api/reboot` | Reboot the device |

---

## Glossary

| Term | Meaning |
|------|---------|
| **BSS** | Uninitialized static data segment — global/static variables default to 0 |
| **DMA** | Direct Memory Access — hardware copies data without CPU involvement (used for SPI LCD blits) |
| **GPIO** | General-Purpose Input/Output — a configurable digital pin |
| **Guru Meditation Error** | ESP32's crash screen (equivalent of a kernel panic) |
| **IRAM** | Instruction RAM — code placed here executes faster; required for ISRs on SPI flash cache miss |
| **ISR** | Interrupt Service Routine — runs asynchronously when hardware signals an event; must be very fast |
| **JTAG** | Hardware debug interface for stepping through code; not used here |
| **LEDC** | LED Control peripheral — generates PWM for LCD backlight brightness |
| **OTA** | Over-the-Air firmware update |
| **PSRAM** | Pseudo-static RAM — external 8 MB SRAM on the WROVER module; used for LVGL draw buffers |
| **PWM** | Pulse-Width Modulation — rapid on/off switching to simulate analog brightness |
| **RMT** | Remote Control Transceiver peripheral — drives WS2812 LED data signal |
| **RTC** | Real-Time Clock — the PCF8563 chip; keeps time without power |
| **SNTP** | Simple NTP — the protocol used to sync the ESP32 clock to internet time |
| **SPIFFS** | SPI Flash File System — where web assets live |
| **SPI** | Serial Peripheral Interface — the bus connecting the 6 LCD panels |
| **Task** | FreeRTOS thread — has its own stack and priority |
| **WS2812** | Addressable RGB LED with a 1-wire protocol driven by RMT |
