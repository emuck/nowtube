// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

#include "models/current_conditions.h"
#include "models/forecast_data.h"

// Builds a single Open-Meteo /v1/forecast URL that requests both current
// conditions and the 5-day daily forecast in one round-trip.
bool weather_build_url(char *buf, size_t buf_size,
                       double lat, double lon,
                       const char *temperature_unit,  // "fahrenheit" or "celsius"
                       const char *wind_speed_unit);  // "mph" or "kmh"

enum class WeatherParseStatus {
    OK,
    JSON_PARSE_ERROR,
    MISSING_CURRENT,
    MISSING_DAILY,
    MISSING_FIELDS,
};

struct weather_parse_result {
    WeatherParseStatus status;
    current_conditions conditions;
    forecast_data      forecast;
};

// Parse the combined JSON response.  On OK, both conditions and forecast
// are fully populated.
weather_parse_result weather_parse_response(const char *json,
                                            size_t json_len,
                                            const char *temp_unit_chr,    // "F" or "C"
                                            const char *wind_unit_label); // "mph" or "km/h"


// Builds and parses NWS forecast URLs/responses. Used as a US forecast override
// while Open-Meteo remains the current-conditions and non-US fallback source.
bool nws_points_build_url(char *buf, size_t buf_size, double lat, double lon);
bool nws_parse_points_response(const char *json, size_t json_len, char *forecast_url, size_t forecast_url_size);
WeatherParseStatus nws_parse_forecast_response(const char *json, size_t json_len,
                                               const char *temp_unit_chr,
                                               forecast_data &forecast);

// Utilities used by display code.
const char *wind_direction_abbr(uint16_t deg);
const char *forecast_day_code(int wday);

// Builds an Open-Meteo air-quality URL that requests the current US AQI.
bool aqi_build_url(char *buf, size_t buf_size, double lat, double lon);
