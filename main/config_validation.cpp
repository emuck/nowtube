// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

#include "config_validation.h"
#include "font_catalog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Logging is emitted on the device but silently dropped in host test builds.
#ifdef ESP_PLATFORM
#include <esp_log.h>
static constexpr const char *TAG = "config_validation";
#define CV_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#else
#define CV_LOGW(fmt, ...) ((void)0)
#endif

// ---------------------------------------------------------------------------

void config_trim_string(char *value) {
  if (value == nullptr) return;

  char *start = value;
  while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
    start++;

  if (start != value)
    memmove(value, start, strlen(start) + 1);

  size_t len = strlen(value);
  while (len > 0) {
    char c = value[len - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    value[--len] = '\0';
  }
}

DisplayMode config_validate_mode(DisplayMode mode) {
  switch (mode) {
  case DisplayMode::CLOCK:
  case DisplayMode::TODAY:
  case DisplayMode::FORECAST:
    return mode;
  case DisplayMode::DATE:
    return DisplayMode::CLOCK;  // legacy NVS value — coerce to CLOCK
  case DisplayMode::GAME:
    return DisplayMode::CLOCK;  // GAME is not persisted; always boot to CLOCK
  }
  return DisplayMode::CLOCK;
}

void config_validate(device_config &config) {
  config.schema_version = device_config::SCHEMA_VERSION;

  // Clamp numeric fields.
  if (config.display_brightness_pct > 100)
    config.display_brightness_pct = 100;
  // Clamp clock_font to an approved compiled font pack.
  if (!clock_font_supported(config.clock_font)) {
    config.clock_font = ClockFont::NIXIE;
  }

  // Cycle dwell times. CLOCK is always active: clamp to [5, 300].
  // TODAY and FORECAST: 0 = skip that mode; non-zero clamped to [5, 300].
  static constexpr uint16_t CYCLE_MIN = 5, CYCLE_MAX = 300;
  if (config.cycle_clock_s < CYCLE_MIN) config.cycle_clock_s = CYCLE_MIN;
  if (config.cycle_clock_s > CYCLE_MAX) config.cycle_clock_s = CYCLE_MAX;
  auto clamp_optional = [](uint16_t v) -> uint16_t {
    if (v == 0) return 0;
    if (v < CYCLE_MIN) return CYCLE_MIN;
    if (v > CYCLE_MAX) return CYCLE_MAX;
    return v;
  };
  config.cycle_today_s    = clamp_optional(config.cycle_today_s);
  config.cycle_forecast_s = clamp_optional(config.cycle_forecast_s);
  // FORECAST dwell must be even (two-phase display splits it in half).
  if (config.cycle_forecast_s != 0 && (config.cycle_forecast_s % 2) != 0) {
    config.cycle_forecast_s += 1;
    if (config.cycle_forecast_s > CYCLE_MAX) config.cycle_forecast_s = CYCLE_MAX;
  }

  // Conditions refresh: must be one of {5, 10, 15, 30} minutes.
  {
    static constexpr uint8_t VALID[] = {5, 10, 15, 30};
    bool ok = false;
    for (uint8_t v : VALID) { if (config.conditions_refresh_minutes == v) { ok = true; break; } }
    if (!ok) {
      CV_LOGW("conditions_refresh_minutes=%u is not in {5,10,15,30} — resetting to 10",
              config.conditions_refresh_minutes);
      config.conditions_refresh_minutes = 10;
    }
  }

  // Trim all string fields.
  config_trim_string(config.wifi_ssid);
  config_trim_string(config.wifi_psk);
  config_trim_string(config.timezone);
  config_trim_string(config.weather_units);
  config_trim_string(config.panel_humidity_metric);

  // Normalise weather units to exactly "metric" or "imperial".
  if (strcmp(config.weather_units, "metric") != 0 &&
      strcmp(config.weather_units, "imperial") != 0) {
    snprintf(config.weather_units, sizeof(config.weather_units), "imperial");
  }

  // Normalise panel_humidity_metric to exactly "humidity" or "aqi".
  if (strcmp(config.panel_humidity_metric, "aqi") != 0) {
    snprintf(config.panel_humidity_metric, sizeof(config.panel_humidity_metric), "humidity");
  }

  if (config.timezone[0] == '\0') {
    snprintf(config.timezone, sizeof(config.timezone),
             "PST8PDT,M3.2.0,M11.1.0");
  }

  // Validate lat/lon — clear location flag if either coordinate is out of range
  // so the firmware falls back to city-based lookup instead of sending garbage.
  if (config.has_weather_location) {
    bool lat_ok = config.weather_lat >= -90.0  && config.weather_lat <= 90.0;
    bool lon_ok = config.weather_lon >= -180.0 && config.weather_lon <= 180.0;
    if (!lat_ok || !lon_ok) {
      CV_LOGW("Weather lat/lon out of range (%.4f, %.4f) — disabling location",
              config.weather_lat, config.weather_lon);
      config.has_weather_location = false;
      config.weather_lat = 0.0;
      config.weather_lon = 0.0;
    }
  }
}

// ---------------------------------------------------------------------------

void config_apply_legacy_weather_pair(device_config &config,
                                      const char *key, const char *value) {
  if (strcmp(key, "units") == 0) {
    snprintf(config.weather_units, sizeof(config.weather_units), "%s", value);
  } else if (strcmp(key, "lat") == 0) {
    config.weather_lat = strtod(value, nullptr);
  } else if (strcmp(key, "lon") == 0) {
    config.weather_lon = strtod(value, nullptr);
  }
}

void config_apply_legacy_wifi_pair(device_config &config,
                                   const char *key, const char *value) {
  if (strcmp(key, "ssid") == 0) {
    snprintf(config.wifi_ssid, sizeof(config.wifi_ssid), "%s", value);
  } else if (strcmp(key, "psk") == 0) {
    snprintf(config.wifi_psk, sizeof(config.wifi_psk), "%s", value);
  }
}

// ---------------------------------------------------------------------------

bool config_parse_kv_file(const char *path,
                          void (*cb)(const char *key, const char *value,
                                     void *user_data),
                          void *user_data) {
  FILE *f = fopen(path, "r");
  if (!f) return false;

  char line[128];
  while (fgets(line, sizeof(line), f)) {
    // Strip newlines.
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    char *cr = strchr(line, '\r');
    if (cr) *cr = '\0';

    // Skip leading whitespace.
    char *trim = line;
    while (*trim == ' ' || *trim == '\t') trim++;

    // Skip blank lines and comments.
    if (*trim == '\0' || *trim == '#') continue;

    char *eq = strchr(trim, '=');
    if (!eq) continue;
    *eq = '\0';

    char *key = trim;
    char *value = eq + 1;

    // Trim trailing whitespace from key.
    char *key_end = key + strlen(key);
    while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) {
      key_end--;
      *key_end = '\0';
    }

    // Trim leading whitespace from value.
    while (*value == ' ' || *value == '\t') value++;

    cb(key, value, user_data);
  }

  fclose(f);
  return true;
}

// ---------------------------------------------------------------------------

struct MigrateLegacyCtx {
  device_config *config;
  bool has_lat;
  bool has_lon;
};

static void weather_kv_cb(const char *key, const char *value, void *ud) {
  auto *ctx = static_cast<MigrateLegacyCtx *>(ud);
  config_apply_legacy_weather_pair(*ctx->config, key, value);
  if (strcmp(key, "lat") == 0) ctx->has_lat = true;
  if (strcmp(key, "lon") == 0) ctx->has_lon = true;
}

static void wifi_kv_cb(const char *key, const char *value, void *ud) {
  config_apply_legacy_wifi_pair(*static_cast<device_config *>(ud), key, value);
}

bool config_migrate_legacy(device_config &config,
                           const char *weather_path, const char *wifi_path) {
  MigrateLegacyCtx ctx{&config, false, false};
  bool weather_ok = config_parse_kv_file(weather_path, weather_kv_cb, &ctx);
  bool wifi_ok    = config_parse_kv_file(wifi_path,    wifi_kv_cb,    &config);
  config.has_weather_location = ctx.has_lat && ctx.has_lon;
  return weather_ok || wifi_ok;
}
