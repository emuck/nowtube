# Ubuntu Development Environment Setup

Step-by-step guide for setting up the nowtube ESP32 development environment on Ubuntu.

## Prerequisites

Tested on Ubuntu 24.04 LTS with ESP-IDF v5.5.3. The Nextube connects via USB for flashing.

### System packages

```bash
sudo apt-get update
sudo apt-get install -y \
  git wget flex bison gperf \
  python3 python3-pip python3-venv python3-serial \
  cmake ninja-build ccache \
  libffi-dev libssl-dev libusb-1.0-0 dfu-util

# Ubuntu 24.04: pip installs into venv by default (PEP 668)
# ESP-IDF's install.sh handles this automatically — no extra steps needed
```

### USB serial access

The Nextube uses a USB-to-serial adapter (CP2102 or CH340). Grant yourself access:

```bash
sudo usermod -a -G dialout $USER
# Log out and back in — or run: newgrp dialout
```

Verify the device appears when plugged in:
```bash
ls /dev/ttyUSB* /dev/ttyACM*
# Expect something like: /dev/ttyUSB0
```

---

## Install ESP-IDF v5.5.3

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.5.3
git submodule update --init --recursive
./install.sh esp32
```

Add to `~/.bashrc` for convenience:
```bash
echo 'alias get_idf=". ~/esp/esp-idf/export.sh"' >> ~/.bashrc
source ~/.bashrc
```

---

## Clone nowtube

```bash
git clone --recursive https://github.com/emuck/nowtube.git
cd nowtube
```

### Optional local bootstrap WiFi configuration

```bash
cp main/spiffs/wifi.sample.txt main/spiffs/wifi.txt
nano main/spiffs/wifi.txt
```

Format:
```ini
ssid=YourNetworkName
psk=YourPassword
```

`main/spiffs/wifi.txt` is ignored by git and is only used as a one-time bootstrap
source to seed NVS on first boot. After the device is provisioned, delete the
local file.

---

## Build and Flash

```bash
# Activate ESP-IDF (each new terminal session)
get_idf

# First build — also flashes SPIFFS assets
idf.py -p /dev/ttyUSB0 build flash monitor

# Subsequent builds (skip re-flashing unchanged assets)
# Comment out FLASH_IN_PROJECT in CMakeLists.txt first, then:
idf.py -p /dev/ttyUSB0 build flash monitor
```

### Flashing notes

- Hold the **BOOT** button on the ESP32 if it doesn't enter flash mode automatically
- The Nextube has auto-reset wired via DTR/RTS — usually no button needed
- Baud rate: 921600 (default, fastest reliable rate)

### Monitor output

`idf.py monitor` opens a serial console at 115200 baud. Press `Ctrl+]` to exit.

### Menu roadmap

Button and backlight behavior (short/long press, Normal/Breathable/Mixed) is documented in **`docs/menu-roadmap.md`**, including diagrams.

---

## Restoring Stock Firmware

If you want to go back to the Rotrics stock firmware at any point:

```bash
# Restore from the full flash backup (takes ~3 min)
esptool.py --port /dev/ttyUSB0 --baud 921600 \
  write_flash 0 ~/nextube_assets/nextube_full_flash.bin
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Permission denied: /dev/ttyUSB0` | `sudo usermod -a -G dialout $USER` then log out/in |
| **Port busy / Could not exclusively lock** | Another process has the serial port. Close IDE serial monitor, `screen`, `minicom`, or any other terminal using the device. Check with `lsof /dev/ttyUSB0` or `fuser -v /dev/ttyUSB0`; kill the process or close the app, then run flash again. |
| Device not found | Check `dmesg | tail -20` after plugging in — look for `cp210x` or `ch341` |
| Flash fails at connect | Hold BOOT button on device while running flash command |
| `idf.py: command not found` | Run `get_idf` to activate the environment |
| Build errors about missing components | Run `git submodule update --init --recursive` |

---

## Useful Commands

```bash
# Build only (no flash)
idf.py build

# Flash only (no rebuild)
idf.py -p /dev/ttyUSB0 flash

# Monitor only (no build/flash)
idf.py -p /dev/ttyUSB0 monitor

# Erase entire flash (factory reset)
idf.py -p /dev/ttyUSB0 erase-flash

# Show partition table
idf.py -p /dev/ttyUSB0 partition-table

# Size analysis
idf.py size-components
```

---

## Running Host-Side Tests

A subset of the firmware logic is extracted into ESP-IDF-free modules and covered by
host-side unit tests. These run on your dev machine without hardware or the ESP toolchain.

### One-time dependency

```bash
sudo apt-get install -y libcjson-dev
```

### Build and run

```bash
cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Debug
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

### What is tested

| Test binary | Covers |
|---|---|
| `test_config_validation` | Config defaults, validation, units normalisation, lat/lon ranges, legacy migration |
| `test_weather_parser` | OWM URL construction, URL encoding, response parsing, rounding, error paths |
| `test_mode_policy` | Auto-cycle transitions, stale-weather skip, button cycle, reset-to-clock |

All three run in CI on every push via `.github/workflows/build.yml`.

---

## Recommended Editor Setup

**VS Code** with extensions:
- `ESP-IDF` (Espressif official)
- `C/C++` (Microsoft)
- `CMake Tools`

Or use **CLion** with the [ESP-IDF plugin](https://www.jetbrains.com/help/clion/esp-idf.html) as the original author did.

For VS Code ESP-IDF extension, point it at `~/esp/esp-idf` when prompted.
