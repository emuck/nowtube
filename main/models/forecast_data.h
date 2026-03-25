// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <ctime>

static constexpr int FORECAST_DAYS = 5;

struct forecast_day {
    int8_t   high;          // rounded high temperature
    int8_t   low;           // rounded low temperature
    uint8_t  wday;          // 0=Sun, 1=Mon, …, 6=Sat
    uint16_t weather_code;  // WMO 4677 code (0 = unknown)
    bool     valid;
};

struct forecast_data {
    forecast_day days[FORECAST_DAYS];
    bool   valid;
    time_t fetched_at;
};
