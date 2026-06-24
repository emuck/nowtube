// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

#include <ctime>

#include "models/current_conditions.h"
#include "models/forecast_data.h"

// Callbacks are invoked from the FreeRTOS fetch task — must not block on gui_lvgl_lock.
using conditions_callback_t = void (*)(const current_conditions &data);
using forecast_callback_t   = void (*)(const forecast_data &data);

// Initialise the weather service with callbacks for both data types.
// Reads initial configuration from config_service.
// Returns false (and stays inactive) when lat/lon is not configured.
bool weather_service_init(conditions_callback_t on_conditions,
                          forecast_callback_t   on_forecast);

// Re-reads configuration from config_service (e.g. after a POST /api/config).
bool weather_service_reload_config();

// Starts an async fetch immediately (noop if not configured).
// Called on WiFi connect; timer fires again per conditions_refresh_minutes.
void weather_service_trigger_fetch();

// Diagnostics — safe to call from any task.
const char *weather_service_last_error();
int weather_service_fail_count();
int weather_service_success_count();
int weather_service_consecutive_fail_count();
time_t weather_service_last_attempt_unix();
time_t weather_service_last_error_unix();
