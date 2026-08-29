// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

#include "services/config_service.h"
#include "config_validation.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <nvs.h>

namespace config_service {

static constexpr const char *TAG = "config_service";
static constexpr const char *NAMESPACE = "config";

static device_config s_config{};

// ---------------------------------------------------------------------------
// NVS persistence

static bool save_locked() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return false;
  }

  err  = nvs_set_u32(h, "schema",     s_config.schema_version);
  err |= nvs_set_str(h, "wifi_ssid",  s_config.wifi_ssid);
  err |= nvs_set_str(h, "wifi_psk",   s_config.wifi_psk);
  err |= nvs_set_str(h, "timezone",   s_config.timezone);
  err |= nvs_set_str(h, "weather_un", s_config.weather_units);
  err |= nvs_set_blob(h, "weather_la",&s_config.weather_lat, sizeof(s_config.weather_lat));
  err |= nvs_set_blob(h, "weather_lo",&s_config.weather_lon, sizeof(s_config.weather_lon));
  err |= nvs_set_u8 (h, "weather_hl", s_config.has_weather_location ? 1 : 0);
  err |= nvs_set_str(h, "weather_ci", s_config.weather_city);
  err |= nvs_set_u8 (h, "disp_br",    s_config.display_brightness_pct);
  err |= nvs_set_u8 (h, "cond_ref",   s_config.conditions_refresh_minutes);
  err |= nvs_set_u16(h, "cyc_clk",    s_config.cycle_clock_s);
  err |= nvs_set_u16(h, "cyc_tod",    s_config.cycle_today_s);
  err |= nvs_set_u16(h, "cyc_fcst",   s_config.cycle_forecast_s);
  err |= nvs_set_u16(h, "cyc_spec",   s_config.cycle_spectrum_s);
  err |= nvs_set_u8 (h, "clock_font", static_cast<uint8_t>(s_config.clock_font));
  err |= nvs_set_str(h, "hum_metric", s_config.panel_humidity_metric);
  err |= nvs_set_u8 (h, "mic_en",     s_config.mic_enabled ? 1 : 0);
  err |= nvs_set_u8 (h, "mic_ch",     s_config.mic_adc_channel);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

static void load_string(nvs_handle_t h, const char *key,
                        char *dst, size_t dst_size) {
  size_t len = dst_size;
  esp_err_t err = nvs_get_str(h, key, dst, &len);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "nvs_get_str(%s) failed: %s", key, esp_err_to_name(err));
  }
}

static void load_blob(nvs_handle_t h, const char *key,
                      void *dst, size_t dst_size) {
  size_t len = dst_size;
  esp_err_t err = nvs_get_blob(h, key, dst, &len);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "nvs_get_blob(%s) failed: %s", key, esp_err_to_name(err));
  }
}

static bool load_from_nvs() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
  if (err != ESP_OK) return false;

  uint32_t schema = 0;
  err = nvs_get_u32(h, "schema", &schema);
  if (err != ESP_OK) {
    nvs_close(h);
    return false;
  }

  s_config = device_config{};
  s_config.schema_version = schema;
  load_string(h, "wifi_ssid",   s_config.wifi_ssid,         sizeof(s_config.wifi_ssid));
  load_string(h, "wifi_psk",    s_config.wifi_psk,          sizeof(s_config.wifi_psk));
  load_string(h, "timezone",    s_config.timezone,          sizeof(s_config.timezone));
  load_string(h, "weather_un",  s_config.weather_units,     sizeof(s_config.weather_units));
  load_blob  (h, "weather_la",  &s_config.weather_lat,      sizeof(s_config.weather_lat));
  load_blob  (h, "weather_lo",  &s_config.weather_lon,      sizeof(s_config.weather_lon));
  uint8_t has_weather_location = s_config.has_weather_location ? 1 : 0;
  nvs_get_u8(h, "weather_hl", &has_weather_location);
  s_config.has_weather_location = has_weather_location != 0;
  load_string(h, "weather_ci",  s_config.weather_city,      sizeof(s_config.weather_city));
  nvs_get_u8(h, "disp_br",    &s_config.display_brightness_pct);
  nvs_get_u8(h, "cond_ref",   &s_config.conditions_refresh_minutes);
  nvs_get_u16(h, "cyc_clk",  &s_config.cycle_clock_s);
  nvs_get_u16(h, "cyc_tod",  &s_config.cycle_today_s);
  nvs_get_u16(h, "cyc_fcst", &s_config.cycle_forecast_s);
  nvs_get_u16(h, "cyc_spec", &s_config.cycle_spectrum_s);
  {
    uint8_t clock_font = static_cast<uint8_t>(s_config.clock_font);
    nvs_get_u8(h, "clock_font", &clock_font);
    s_config.clock_font = static_cast<ClockFont>(clock_font);
  }
  load_string(h, "hum_metric", s_config.panel_humidity_metric, sizeof(s_config.panel_humidity_metric));
  {
    uint8_t mic_enabled = s_config.mic_enabled ? 1 : 0;
    nvs_get_u8(h, "mic_en", &mic_enabled);
    s_config.mic_enabled = mic_enabled != 0;
    nvs_get_u8(h, "mic_ch", &s_config.mic_adc_channel);
  }
  nvs_close(h);

  config_validate(s_config);
  return true;
}

// ---------------------------------------------------------------------------
// Public API

bool init() {
  if (!load_from_nvs()) {
    s_config = device_config{};
    config_migrate_legacy(s_config, "/spiffs/weather.txt", "/spiffs/wifi.txt");
    config_validate(s_config);
    if (!save_locked()) {
      return false;
    }
    ESP_LOGI(TAG, "Config initialized from defaults/legacy files");
  } else {
    ESP_LOGI(TAG, "Config loaded from NVS");
  }

  ESP_LOGI(TAG, "Config timezone=%s units=%s brightness=%u",
           s_config.timezone, s_config.weather_units,
           static_cast<unsigned>(s_config.display_brightness_pct));
  return true;
}

const device_config &get_config() {
  return s_config;
}

bool update(const device_config &config) {
  device_config next = config;
  config_validate(next);
  s_config = next;
  if (!save_locked()) {
    return false;
  }
  ESP_LOGI(TAG, "Config updated timezone=%s units=%s brightness=%u",
           s_config.timezone, s_config.weather_units,
           static_cast<unsigned>(s_config.display_brightness_pct));
  return true;
}

bool set_display_brightness(uint8_t brightness_pct) {
  device_config next = s_config;
  next.display_brightness_pct = brightness_pct;
  return update(next);
}

}
