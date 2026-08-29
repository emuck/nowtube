//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include "mode_manager.h"
#include "models/current_conditions.h"
#include "models/forecast_data.h"

namespace display_controller {

// Must be called once before any other function.
void init();

void render_boot_screen();
void apply_mode(DisplayMode mode);
void set_mode(DisplayMode mode);
void cycle_mode();
void reset_mode();
DisplayMode current_mode();
bool get_conditions_snapshot(current_conditions &out);
bool get_forecast_snapshot(forecast_data &out);

// Called from conditions_service fetch task when fresh data arrives.
// Refreshes TODAY display if it is currently active.
void on_conditions_updated(const current_conditions &data);

// Called from forecast_service fetch task when fresh forecast data arrives.
void on_forecast_updated(const forecast_data &data);

void on_time_changed();
void on_time_loaded();

// Show recovery AP instructions on all displays.  Blocks further clock/weather
// rendering until cancelled or the device is rebooted.
void show_recovery_screen();

// Returns true while the recovery screen is active.
bool is_recovery_active();

// Cancel recovery mode: clears the overlay, resumes normal rendering.
// Caller is responsible for stopping the AP and reconnecting Wi-Fi.
void cancel_recovery_screen();

}
