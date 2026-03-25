# Hardware Notes

Detailed hardware reference for the Rotrics Nextube (hardware rev 1.31, 2022/01/19).
Compiled from open source reverse engineering + stock firmware binary analysis + full flash extraction.

## GPIO Pin Map

| GPIO | Function | Notes |
|---|---|---|
| 0 | LCD4 CS | Boot-sensitive — must be HIGH at boot |
| 2 | Touch middle button | TOUCH_PAD_NUM2 |
| 4 | Touch left button | TOUCH_PAD_NUM0 |
| 5 | LCD5 CS | |
| 12 | SPI SCK (LCD) | Boot-sensitive on some revisions |
| 13 | SPI MOSI (LCD) | |
| 14 | SPI DC (LCD) | |
| 15 | Touch right button | TOUCH_PAD_NUM3 |
| 18 | LCD6 CS | |
| 19 | LCD backlight PWM | LEDC channel |
| 21 | LCD3 CS | |
| 22 | I2C SCL (RTC) | PCF8563 |
| 23 | I2C SDA (RTC) | PCF8563 |
| 25 | DAC / Speaker out | LTK8002D amp — confirmed in stock firmware |
| 26 | LCD2 CS | |
| 27 | LCD Reset | Shared across all 6 displays |
| 32 | WS2812 LED data | RMT peripheral |
| 33 | LCD1 CS | |
| 34–39 | **Unknown / candidate** | Input-only pins — I2S mic candidates |

GPIO 16/17 are used for PSRAM on ESP32-WROVER-E — do not use.

## Display

- **Panel:** ST7735-based, 80×162 pixels, 16-bit color (RGB565)
- **Interface:** SPI at 40MHz
- **Count:** 6 independent displays, shared SPI bus, individual CS lines
- **Backlight:** Hardware PWM via LEDC on GPIO19 (shared across all panels)
- **Optimal brightness:** 60% — vendor default, prevents glare in room lighting
- **Asset resolution:** 80×160px for digit images, 76×37px for info panel images

## LEDs

- **Type:** WS2812 (NeoPixel-compatible) RGB
- **Count:** 6, individually addressable
- **Protocol:** RMT (Remote Control peripheral) on GPIO32
- **Max RGB value:** 200/255 — vendor cap to prevent heat and colour shift
- **Default color:** Warm orange `[228, 112, 37]` across all 6 LEDs
- **Color palette (Right tap cycles):** warm orange → red → green → blue → cyan → magenta → amber
- **Order:** Logical index 0 = first display, 5 = last. If LEDs appear in the wrong order or “dislocated” vs the displays, edit `main/drivers/leds.cpp` and change `PHYSICAL_POS_TO_LOGICAL` (e.g. `{1,0,3,2,5,4}`).

## Touch Buttons

- **Type:** Capacitive touch (ESP32 native touch peripheral)
- **Count:** 3 — left, middle, right
- **Events:** push (hold start), release (hold end), tap (short touch)
- **Threshold:** 0.6f sensitivity (set in `touchpads.cpp`)
- **Current button mapping:**

| Button | Tap | Long-press (>1s) |
|--------|-----|-----------------|
| Left | Cycle display mode (no-op in clock-only build) | Reset to CLOCK |
| Middle | Cycle brightness (100% → 70% → 40% → 20% → 5% → 0% → …) | Toggle recovery AP (enter/cancel) |
| Right | Cycle LED color / toggle uniform↔random (Mixed mode) | Cycle backlight mode (Normal → Breathable → Mixed → Normal) |

## RTC

- **Chip:** PCF8563
- **Interface:** I2C at 400kHz
- **Battery backup:** Yes — retains time through power cycles
- **Boot behavior:** Loads saved time immediately; NTP sync updates and saves back to RTC

## Audio / Microphone

- **Amplifier:** LTK8002D class-D, SOP-8 (confirmed via stock firmware binary; datasheet reviewed)
- **Speaker output:** GPIO25 → LTK8002D Pin 4 (IN-) → BTL speaker output
  - GPIO25 = ESP32 DAC1 (hardwired)
  - Audio is AC-coupled into the inverting input via Ci and Ri; IN+ biased at VDD/2 via BYPASS cap
- **LTK8002D SHUTDOWN pin (Pin 1): active-HIGH**
  - HIGH = amp off (<0.5 µA); LOW = amp on
  - **Almost certainly tied directly to GND** — exhaustive GPIO audit of the ESP32-D0WD shows zero free output-capable GPIOs available for SHUTDOWN control; the board designer had no spare pin
  - Confirm with multimeter: Pin 1 should read ~0 V when board is powered
  - If confirmed, no firmware GPIO action is needed to enable the amp
- **BYPASS cap (Pin 2):** External 1 µF capacitor required for VDD/2 reference and pop suppression; adds ~100–150 ms soft-start delay after amp enable
- **Microphone:** I2S interface — confirmed working in stock firmware (spectrum analyzer mode); **mic GPIO pins are not yet identified**
- **Mic GPIO:** Unknown — candidates are GPIO34, GPIO35, GPIO36, GPIO39 (input-only); standard I2S 3-wire (Phase 3) not yet attempted
- **Custom firmware status:** No audio implemented. Speaker output is unblocked (GPIO25 free, DAC1 available) pending SHUTDOWN GPIO identification. Microphone remains blocked.

## "Indoor" Sensor

The stock firmware config includes `"Indoor"` in the info display list, suggesting an
onboard or I2C-connected temperature/humidity sensor. This has not been identified.
The I2C bus (GPIO22/23) also carries the RTC — a second I2C device could share the bus.
**Candidates:** SHT30, SHT31, AHT10, or similar common I2C temp/humidity ICs.

## PSRAM

- **Size:** 8MB
- **Interface:** Quad-SPI at 80MHz (configured in sdkconfig.defaults)
- **Usage:** LVGL frame buffers, JPEG decode buffers

## Flash

- **Size:** 16MB
- **Partition layout** (current firmware — A/B OTA layout, see `partitions.csv`):

| Partition | Offset | Size | Type |
|---|---|---|---|
| nvs | 0x009000 | 24KB | NVS |
| phy_init | 0x00F000 | 4KB | PHY calibration |
| otadata | 0x010000 | 8KB | OTA data (active slot selector) |
| ota_0 | 0x020000 | 2MB | App slot 0 |
| ota_1 | 0x220000 | 2MB | App slot 1 |
| spiffs | 0x420000 | ~12MB | SPIFFS assets |

> **Note:** The stock Nextube firmware uses a different partition layout (single app partition at 0x010000, no A/B OTA). The layout above reflects the custom firmware. Do not flash the custom firmware's partition table onto a device where you want to preserve stock firmware recoverability without first backing up the flash.

## Stock Firmware Asset Themes

Extracted from full flash dump. All digit images 80×160px JPEG.

| Theme | Style |
|---|---|
| NixieOY | Classic orange/yellow nixie tube — most complete, includes alarm variants |
| LightFuture | Futuristic blue/white — device default |
| FlipClock | Physical split-flap card aesthetic |
| DotMatrixRG | Red/green dot matrix |
| DotMatrixY | Yellow dot matrix |
| WireMesh | Wireframe overlay |
| DarkSlate | Dark, minimal |
| Formula1 | F1 timing display style |
| GlitchGR | Green glitch/scanline |
| NotionRain | Matrix rain inspired |
| RedDigits | Classic red LED |
| RetroPaper | Paper/e-ink aesthetic |
| Custom/01/02/03 | User-uploadable slots |
