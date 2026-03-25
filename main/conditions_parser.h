// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

enum class ConditionsParseStatus {
    OK,
    JSON_PARSE_ERROR,
    MISSING_CURRENT,
    MISSING_FIELDS,
};

struct conditions_parse_result {
    ConditionsParseStatus status   = ConditionsParseStatus::JSON_PARSE_ERROR;
    int16_t  temp          = 0;   // lround(temperature_2m)
    uint8_t  humidity      = 0;   // relative_humidity_2m
    int16_t  wind_speed    = 0;   // lround(wind_speed_10m)
    uint16_t wind_deg      = 0;   // wind_direction_10m % 360
    uint16_t weather_code  = 0;   // WMO 4677 weathercode
    uint16_t sunrise_min   = 0;   // minutes since midnight (daily[0])
    uint16_t sunset_min    = 0;
};

// Build an Open-Meteo URL for current conditions + today's sunrise/sunset.
// temperature_unit: "fahrenheit" or "celsius"
// wind_speed_unit:  "mph" or "kmh"
// Returns false if buf_size is too small.
bool conditions_build_url(char *buf, size_t buf_size,
                          double lat, double lon,
                          const char *temperature_unit,
                          const char *wind_speed_unit);

// Parse a conditions API response.
conditions_parse_result conditions_parse_response(const char *json, size_t json_len);

// Compass abbreviation for wind direction in degrees (0=N, 90=E, …).
const char *wind_direction_abbr(uint16_t deg);
