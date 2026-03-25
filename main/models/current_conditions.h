// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

// Unified current-conditions data model shared by TODAY and FORECAST modes.
// Populated by conditions_service from a single Open-Meteo /v1/forecast fetch.
// Date inputs (month, day, weekday) come from local RTC — not stored here.
//
// No ESP-IDF, FreeRTOS, or LVGL dependencies — safe to include from pure
// host-testable code.

#include <cstdint>
#include <ctime>

struct current_conditions {
    // Temperature — from Open-Meteo current.temperature_2m.
    int16_t temp_rounded  = 0;      // lround(temperature_2m)
    char    temp_unit[4]  = "F";    // "F" (imperial) or "C" (metric)
    char    temp_value[8] = "";     // formatted as "%d" for display

    // Wind — from Open-Meteo current.wind_speed_10m / wind_direction_10m.
    int16_t  wind_speed  = 0;       // rounded integer, in configured units
    uint16_t wind_deg    = 0;       // compass bearing 0–359
    char     wind_unit[8] = "mph";  // "mph" (imperial) or "km/h" (metric)

    // Humidity — from Open-Meteo current.relative_humidity_2m.
    uint8_t humidity = 0;           // 0–100 %

    // Air quality — from Open-Meteo air-quality API (fetched only when configured).
    int16_t us_aqi    = 0;          // US AQI 0–500 (0 = not fetched)
    bool    aqi_valid = false;

    // WMO weather interpretation code — from Open-Meteo current.weathercode.
    uint16_t weather_code = 0;

    // Astronomy — from Open-Meteo daily[0], in local clock time.
    uint16_t sunrise_min = 0;       // minutes since midnight (0 = unknown)
    uint16_t sunset_min  = 0;       // minutes since midnight (0 = unknown)

    bool   valid      = false;      // true when live data has been received
    time_t fetched_at = 0;          // unix timestamp of last successful fetch
};
