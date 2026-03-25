// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

#pragma once

// Pure, host-testable validation and legacy-migration helpers.
// No ESP-IDF or NVS dependencies — safe to compile and test on the host.

#include "models/device_config.h"

// Trim leading and trailing ASCII whitespace (' ', '\t', '\r', '\n') in-place.
void config_trim_string(char *value);

// Return a known-good DisplayMode, clamping unrecognised values to CLOCK.
DisplayMode config_validate_mode(DisplayMode mode);

// Normalise and range-check all fields of |config| in-place:
//   - Trims whitespace from all string fields.
//   - Forces weather_units to "imperial" if not "metric" or "imperial".
//   - Fills in defaults for weather_provider and timezone if blank.
//   - Clamps display_brightness_pct to [0, 100].
//   - Clamps boot_mode to a known value.
//   - Clears has_weather_location (and zeroes lat/lon) if either is out of range.
void config_validate(device_config &config);

// Apply a single key=value pair from a legacy weather.txt file to |config|.
// Recognised keys: owm_key, city, units, lat, lon.
void config_apply_legacy_weather_pair(device_config &config,
                                      const char *key, const char *value);

// Apply a single key=value pair from a legacy wifi.txt file to |config|.
// Recognised keys: ssid, psk.
void config_apply_legacy_wifi_pair(device_config &config,
                                   const char *key, const char *value);

// Parse a key=value text file, calling |cb(key, value, user_data)| for each
// valid pair. Blank lines and lines starting with '#' are skipped.
// Returns false if the file cannot be opened; true otherwise.
bool config_parse_kv_file(const char *path,
                          void (*cb)(const char *key, const char *value,
                                     void *user_data),
                          void *user_data);

// Load legacy /spiffs/wifi.txt and /spiffs/weather.txt into |config|.
// Paths are parameterised so callers (and tests) can point at arbitrary files.
// Sets config.has_weather_location only when both lat and lon are present.
// Returns true if at least one file was opened successfully.
bool config_migrate_legacy(device_config &config,
                           const char *weather_path, const char *wifi_path);
