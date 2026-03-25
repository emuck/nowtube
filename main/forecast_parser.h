// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

#include "models/forecast_data.h"

enum class ForecastParseStatus {
    OK,
    JSON_PARSE_ERROR,
    MISSING_DAILY,
    MISSING_FIELDS,
};

struct forecast_parse_result {
    ForecastParseStatus status = ForecastParseStatus::JSON_PARSE_ERROR;
    int          count = 0;
    forecast_day days[FORECAST_DAYS] = {};
};

// Build an Open-Meteo URL for the 5-day daily forecast including weathercode.
// temperature_unit: "fahrenheit" or "celsius"
// Returns false if buf_size is too small.
bool forecast_build_url(char *buf, size_t buf_size,
                        double lat, double lon,
                        const char *temperature_unit);

// Parse a forecast API response.
forecast_parse_result forecast_parse_response(const char *json, int json_len);

// 2-character day abbreviation: 0=SU, 1=MO, …, 6=SA. Returns "--" for invalid.
const char *forecast_day_code(int wday);
