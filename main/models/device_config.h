//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "mode_manager.h"

enum class ClockFont : uint8_t {
  NIXIE      = 0,
  SPACE_MONO = 1,
};

struct device_config {
  static constexpr uint32_t SCHEMA_VERSION = 1;

  uint32_t schema_version = SCHEMA_VERSION;
  char wifi_ssid[64] = "";
  char wifi_psk[64] = "";
  char timezone[64] = "PST8PDT,M3.2.0,M11.1.0";
  char weather_units[16] = "imperial";
  double weather_lat = 0.0;
  double weather_lon = 0.0;
  bool has_weather_location = false;
  char weather_city[64] = "";
  uint8_t display_brightness_pct = 60;
  // Conditions fetch cadence in minutes.  Valid values: 5 / 10 / 15 / 30.
  uint8_t conditions_refresh_minutes = 10;

  // Auto-cycle dwell times in seconds.  0 = skip that mode.  Non-zero clamped to [5, 300].
  uint16_t cycle_clock_s    = 50;
  uint16_t cycle_today_s    = 10;
  uint16_t cycle_forecast_s = 10;

  ClockFont clock_font = ClockFont::NIXIE;

  // Panel 4 (humidity slot in TODAY mode): "humidity" (default) or "aqi".
  char panel_humidity_metric[16] = "humidity";
};
