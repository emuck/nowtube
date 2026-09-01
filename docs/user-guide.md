# nowtube User Guide

nowtube is open-source firmware for the Rotrics Nextube clock. This guide walks
you through everything from your very first USB flash to day-to-day use and
firmware updates — no engineering background required.

---

## Table of Contents

1. [First-Time Setup](#1-first-time-setup)
2. [First Boot and WiFi](#2-first-boot-and-wifi)
3. [The Web Interface](#3-the-web-interface)
4. [Display Modes](#4-display-modes)
5. [Physical Buttons](#5-physical-buttons)
6. [LED Backlight](#6-led-backlight)
7. [Updating Firmware](#7-updating-firmware)
8. [Changing the Display Font](#8-changing-the-display-font)
9. [Recovery Mode](#9-recovery-mode)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. First-Time Setup

This is the one time you will need a USB cable and a Linux computer. This first
install is a **full flash** that replaces the stock firmware and writes the
bootloader, partition table, app image, and SPIFFS assets. After this first
flash, future updates can happen over your home WiFi — no cable needed unless
you want to do another full flash.

> **Using macOS?** The steps are the same. Substitute `/dev/cu.usbserial-*`
> wherever you see `/dev/ttyUSB0`, and follow the
> [ESP-IDF macOS guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/linux-macos-setup.html)
> for the toolchain install.

### What You Need

- A Linux (Ubuntu 22.04 or 24.04 recommended) or macOS computer
- A USB cable that carries data (not charge-only)
- About 20–30 minutes for the one-time toolchain download

### Step 1 — Install System Packages

Open a terminal and run:

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf \
  python3 python3-pip python3-venv python3-serial \
  cmake ninja-build ccache libffi-dev libssl-dev libusb-1.0-0 dfu-util
```

Then grant yourself USB serial access:

```bash
sudo usermod -a -G dialout $USER
```

**Log out and log back in** after this — the permission change doesn't take
effect until you do.

### Step 2 — Install the ESP-IDF Toolchain

This is Espressif's official build system for the ESP32. It downloads once and
stays on your computer.

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.5.3
git submodule update --init --recursive
./install.sh esp32
```

Add a shortcut so you don't have to type the full path every time:

```bash
echo 'alias get_idf=". ~/esp/esp-idf/export.sh"' >> ~/.bashrc
source ~/.bashrc
```

### Step 3 — Get the nowtube Firmware

```bash
git clone --recursive https://github.com/emuck/nowtube.git
cd nowtube
```

### Step 4 — (Optional) Pre-load Your WiFi Credentials

You can bake your WiFi network name and password into the first flash so the
device connects automatically on first boot. This is optional — you can always
set it up through the web interface later.

```bash
cp main/spiffs/wifi.sample.txt main/spiffs/wifi.txt
nano main/spiffs/wifi.txt
```

Edit the file to match your network:

```
ssid=YourNetworkName
psk=YourPassword
```

Save and close. This file is excluded from git so your password won't be
accidentally shared.

### Step 5 — Flash the Device

Plug in your Nextube via USB, then run:

```bash
get_idf
idf.py -p /dev/ttyUSB0 build flash monitor
```

This will:
1. Compile the firmware (takes a few minutes the first time)
2. Flash everything to the device
3. Open a serial log so you can watch it boot

You should see the displays light up and the clock appear. Press **Ctrl+]** to
exit the serial monitor when you're done watching.

> **Tip:** If the flash fails with a connection error, hold the small **BOOT**
> button on the ESP32 module while the flash command is connecting, then release
> it. Most devices auto-reset without needing this.

### Step 6 — Clean Up

If you created a `wifi.txt` file, delete it now so the password doesn't linger
on disk:

```bash
rm main/spiffs/wifi.txt
```

The credentials are now saved inside the device. You won't need to re-enter them.

---

## 2. First Boot and WiFi

### What Happens on First Boot

When the device starts up for the first time it will:

1. Show the time from its internal clock
2. Try to connect to WiFi (using the credentials you pre-loaded, if any)
3. Once connected, sync the time over the internet via NTP
4. Fetch the current weather for your configured location
5. Begin cycling between clock, weather, and forecast displays

### If You Didn't Pre-Load WiFi Credentials

The device will start in **recovery mode** — it creates its own WiFi hotspot
called `nowtube-setup`. The six displays will show setup instructions.

1. On your phone or laptop, connect to the `nowtube-setup` WiFi network
2. Open a browser and go to `http://192.168.4.1/`
3. Enter your home WiFi name and password in the WiFi section
4. Click **Save**
5. The device will reboot and connect to your home network

### Finding the Device on Your Network

Once connected to your home WiFi, you need its IP address to open the web
interface. The easiest ways to find it:

- **Check your router's device list** — look for a device named `nowtube`
- **Check the serial monitor** at boot — it logs the IP address as
  `wifi: IP address: 192.168.x.x`
- **Try `http://nowtube.local/`** in a browser — works on most home networks

---

## 3. The Web Interface

Open `http://<device-ip>/` in any browser on your home network. You'll see a
configuration page with several sections.

> The web interface has no password. It is only accessible on your local network
> — never expose the device's port 80 to the internet.

### Device Status

The top of the page shows a live status panel:

| Field | What it means |
|---|---|
| Uptime | How long the device has been running since last boot |
| WiFi | Connection status and current IP address |
| Free heap | Available memory — should stay above 3.5 MB |
| Last weather sync | When the device last successfully fetched weather |
| Firmware | The installed firmware version |

### WiFi Settings

Change the network name and password here if you move the device or change your
router password. The current password is never displayed — you only see a
placeholder. Enter a new value to change it.

### Weather Settings

| Setting | Description |
|---|---|
| City name | Type a city name (e.g. `San Francisco`) — the device looks up the coordinates automatically |
| Latitude / Longitude | Use these instead of a city name for a precise location |
| Temperature units | Choose °C or °F |
| Timezone | Your local timezone (e.g. `America/Los_Angeles`) |
| Panel 4 metric | Choose between **Humidity %** or **US Air Quality Index** for the fourth panel in TODAY mode |

Weather is fetched from [Open-Meteo](https://open-meteo.com/) — no API key or
account required.

### Display Settings

| Setting | Description |
|---|---|
| Brightness | LCD panel brightness from 0% (off) to 100% |
| Clock dwell | How long the device shows the clock before switching (seconds) |
| TODAY dwell | How long it shows the TODAY screen before switching |
| FORECAST dwell | How long it shows the FORECAST screen before switching |
| Clock font | Choose Nixie One, Space Mono, Atkinson Hyperlegible, or Aldrich; the device applies the saved choice immediately |

Set any dwell time to **0** to skip that mode entirely in the auto-cycle.

### LED Backlight Settings

| Setting | Description |
|---|---|
| Backlight mode | Normal (solid), Breathable (slow pulse), or Mixed (color-cycling) |
| Color | The active LED color |

See [LED Backlight](#6-led-backlight) for a full explanation of the modes.

### Firmware Update

Upload a new `nowtube.bin` firmware file directly from your browser — no USB
cable required. See [Updating Firmware](#7-updating-firmware) for the full
process.

### Reboot

The **Reboot** button performs a clean software restart. The device will be
unreachable for about 10 seconds, then come back up with all settings intact.

---

## 4. Display Modes

The device automatically cycles through up to three information modes and pauses
on each one for a configurable amount of time. Pressing a button overrides the
cycle until the next automatic switch.

### Clock

The default mode. Shows the current time in large digits using the selected
font. The AM/PM indicator appears on the rightmost display.

The clock updates once per second and syncs to internet time (NTP) whenever
WiFi is connected. The built-in real-time clock (RTC) keeps time accurately
even when WiFi is unavailable or the power is out briefly.

### TODAY

A six-panel ambient display showing useful information about today:

| Panel | Shows |
|---|---|
| 1 | Day of the week (MON / TUE / WED …) |
| 2 | Month name |
| 3 | Day of the month + current weather condition icon |
| 4 | Wind speed and direction |
| 5 | Humidity % or US Air Quality Index (your choice) |
| 6 | Next sunrise or sunset time |

All panels show `--` gracefully if data isn't available yet.

### FORECAST

A five-day weather outlook. Each panel shows one day:

- **Panel 1:** Legend showing the high/low temperature scale
- **Panels 2–6:** One forecast day each

Each forecast panel starts by showing the day name and temperatures, then
switches to a weather condition icon halfway through the dwell time — so you
see the numbers first, then the visual.

### Tube Invaders

An arcade game built into the firmware. Each of the six displays is one lane.
Enemies fall from the top; your ship sits at the bottom.

**To enter the game:** Long-press the **left** button (hold for about 1 second)

**To exit the game:** Long-press the **left** button again

**Controls while playing:**

| Button | Action |
|---|---|
| Left (tap) | Move ship one lane left |
| Right (tap) | Move ship one lane right |
| Middle (tap) | Fire |

Shoot the enemies before they reach your ship. The game speeds up every 8 kills.
Your highest score is saved and shown after a game over — it persists across
reboots.

The game is not part of the auto-cycle — it only runs while you're actively
playing.

---

## 5. Physical Buttons

There are three capacitive touch buttons on the front of the device.

### Quick Reference

| Button | Tap | Long Press (hold ~1 s) |
|---|---|---|
| **Left** | Cycle display mode | Enter / exit Tube Invaders |
| **Middle** | Cycle LCD brightness | Enter / exit recovery mode |
| **Right** | Cycle LED color | Cycle LED backlight mode |

### Left Button

- **Tap:** Steps through the display modes in order: CLOCK → TODAY → FORECAST → CLOCK …
- **Long press:** Jumps back to the clock from any mode. If you're in Tube
  Invaders, exits the game.

### Middle Button

- **Tap:** Steps through LCD brightness levels: 100% → 70% → 40% → 20% → 5% →
  0% (off) → 100% …
- **Long press:** Enters recovery mode (starts the `nowtube-setup` hotspot).
  A second long press cancels recovery and returns to normal. See
  [Recovery Mode](#9-recovery-mode).

### Right Button

- **Tap:** In Normal or Breathable mode — cycles the LED color through:
  warm orange → red → green → blue → cyan → magenta → amber → off → …
  In Mixed mode — toggles between all-LEDs-same-color and each-LED-random-color.
- **Long press:** Cycles the LED backlight mode: Normal → Breathable → Mixed →
  Normal …

---

## 6. LED Backlight

The six LEDs behind the displays have three distinct behavior modes.

### Normal

LEDs glow at a solid, constant color. Use the right button (tap) to cycle
through colors, or turn them off entirely.

### Breathable

The LEDs slowly pulse on and off in a 10-second cycle — bright, then fading to
black, then back. You choose the color with the right button tap. Good for a
relaxed ambient glow.

### Mixed

The LEDs rotate through colors automatically with each breath cycle.

- **Uniform** (default): all six LEDs show the same color, changing with each
  breath
- **Random**: each LED gets a different color from the palette on each breath —
  tap the right button to toggle between uniform and random

---

## 7. Updating Firmware

After the first USB flash, all future updates happen over WiFi from your browser.

### Which Update Path Should I Use?

- **Stock firmware → nowtube:** USB full flash required.
- **Older nowtube release → newer release with the same asset set:** browser OTA is enough.
- **Older nowtube release → newer release with updated icons/web assets:** browser OTA
  **plus** a SPIFFS asset update, or a USB full flash.
- **Not sure what is currently installed:** use the USB full-flash path.

### Getting the New Firmware File

Build it locally:

```bash
get_idf
idf.py build
```

The compiled file is at `build/nowtube.bin`.

Or download a release binary from the
[GitHub releases page](https://github.com/emuck/nowtube/releases).

If you are updating from a Windows 11 machine, the usual path is to download
the tagged `nowtube.bin` release file rather than build it locally.

### Flashing Over WiFi

1. Open `http://<device-ip>/` in a browser
2. Scroll to the **Firmware Update** section
3. Click **Choose File** and select `nowtube.bin`
4. Click **Flash Firmware**
5. A progress bar will appear — the upload takes about 30 seconds
6. The device will reboot automatically when the flash is verified

The device verifies the file's integrity before committing it. If something goes
wrong during the upload, it stays on the old firmware — it won't be left in a
broken state.

### OTA vs. SPIFFS Asset Updates

These are currently **different interfaces**:

- The **Firmware Update** section in the browser uploads only `nowtube.bin`.
- The helper script uploads only SPIFFS asset files. It does **not** upload
  `nowtube.bin`.

```bash
python3 tools/upload_spiffs_assets.py <device-ip>
```

The helper script uses only Python 3 standard-library modules and is supported
on macOS, Linux, and Windows 11 with Python 3.

Releases that change `asset_rev` need both steps:

1. Upload the new firmware in the browser
2. Run `python3 tools/upload_spiffs_assets.py <device-ip>`
3. Refresh the web UI and confirm the device reports the expected build and assets

### Using curl Instead

```bash
curl -X POST http://<device-ip>/api/ota/upload \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @build/nowtube.bin
```

Poll `GET /api/ota/status` for progress if you want to watch it:

```bash
watch -n 1 curl -s http://<device-ip>/api/ota/status
```

---

## 8. Changing the Display Font

The clock can use curated, compiled font packs. The device includes Nixie One,
Space Mono, Atkinson Hyperlegible, and Aldrich. The `tools/font_convert.py`
script prepares a candidate Google Font for review and conversion.

### Requirements

```bash
npm install -g lv_font_conv
```

### Convert a Font

```bash
# Preview available recommended fonts
python3 tools/font_convert.py --list-fonts

# Convert a font at all standard clock sizes
python3 tools/font_convert.py "Atkinson Hyperlegible" --sizes 48 60 120 --charset clock+
```

The script prints the exact lines to add to `CMakeLists.txt` and places the
generated `.c` files in `main/fonts/`. Downloaded source fonts are cached
locally under `fonts-src/` and are intentionally not committed.

### Apply the Font

1. Confirm the font's open licence and its fit on all six panels
2. Add the generated `.c` files to `CMakeLists.txt`, then add a catalog entry
   in `main/font_catalog.cpp` and a theme in `main/font_theme.cpp`
3. Build and flash: `idf.py build` then upload via the web UI

See `tools/README.md` for full documentation and a curated list of recommended
fonts.

---

## 9. Recovery Mode

Recovery mode lets you reconfigure WiFi credentials without a USB cable. It
creates a standalone hotspot and serves the full configuration web interface.

### Entering Recovery Mode

There are two ways:

**Option A — Button:** Long-press the **middle** button for about 1 second. The
displays will show setup instructions and the LEDs turn dim blue.

**Option B — Auto:** If the device has no WiFi credentials saved (e.g. after a
factory reset), it boots directly into recovery mode.

### What the Displays Show

When recovery mode is active, each display shows one piece of the connection
instructions across the six panels:

```
SETUP   JOIN    nowtube-  OPEN A    GO TO:  192.168.
 MODE   WI-FI    setup   BROWSER            4.1
```

### Connecting and Configuring

1. On your phone or computer, connect to the **`nowtube-setup`** WiFi network
   (open network, no password)
2. Open a browser and navigate to **`http://192.168.4.1/`**
   (you must type this manually — there is no automatic redirect)
3. Update your WiFi credentials in the WiFi section and click **Save**
4. The device will reboot and connect to your home network

### Cancelling Recovery Mode

Long-press the middle button a second time. The device returns to normal
operation without rebooting.

---

## 10. Troubleshooting

### Device won't connect to WiFi

- Double-check the SSID and password in the web UI (or recovery mode)
- Make sure your router is 2.4 GHz — the ESP32 does not support 5 GHz
- Check that the password contains only standard characters (some special
  characters can cause issues)
- Try a full reboot via the web UI **Reboot** button

### Web interface won't load

- Confirm you're on the same WiFi network as the device
- Find the correct IP address from your router's device list
- Try `http://nowtube.local/` as an alternative
- If the device was recently rebooted, wait 15–20 seconds for it to reconnect

### Weather isn't updating

- Check the **Last weather sync** time in the Device Status panel
- Confirm the location is set correctly in Weather Settings
- Try using lat/lon coordinates instead of a city name for better precision
- Open-Meteo fetches happen every 10 minutes — a brief delay after changing
  settings is normal

### Display shows `--` for all weather values

The device hasn't successfully fetched weather yet. This is normal for the first
few minutes after a reboot or a location change. If it persists, check your
WiFi connection and location settings.

### Clock is wrong by hours (right minutes)

The timezone isn't set. Open the web UI, set your timezone under Weather
Settings (e.g. `America/Chicago`), and save. The clock updates immediately.

### Clock is wrong after a power cut

The RTC keeps time through brief outages. For longer cuts, the clock will be
off until the device reconnects to WiFi and syncs via NTP — usually within
30 seconds of boot.

### Forgot the device's IP address

- Check your router's DHCP lease table
- Plug in via USB and run `idf.py -p /dev/ttyUSB0 monitor` — the IP is logged
  at boot
- Enter recovery mode (middle button long-press) — the web UI is available at
  `http://192.168.4.1/` while in recovery mode, regardless of your home network

### Need to start completely fresh

A full factory reset erases all settings including WiFi credentials:

```bash
get_idf
idf.py -p /dev/ttyUSB0 erase-flash
idf.py -p /dev/ttyUSB0 flash
```

After this the device will boot into recovery mode so you can re-enter your
WiFi credentials.

### First USB flash fails to connect

Hold the small **BOOT** button on the ESP32 module while the flash command is
connecting. Release it once you see `Connecting...` in the terminal. Most
devices enter flash mode automatically, but some need this.

```
Permission denied: /dev/ttyUSB0
```

Run `sudo usermod -a -G dialout $USER`, log out, log back in, and try again.

---

*For deeper technical detail, see `docs/ubuntu-dev-setup.md` (full dev
environment), `docs/hardware-notes.md` (GPIO map and hardware specs), and
`docs/PROJECT_PLAN.md` (feature roadmap).*
