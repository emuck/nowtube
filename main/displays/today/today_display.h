// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

#include "models/current_conditions.h"

// Text-first TODAY mode renderer.
//
// TODAY uses all 6 panels:
//   Panel 0 — Month / day number
//   Panel 1 — Weekday / "TODAY" label
//   Panel 2 — WIND / speed + direction (or "--" when weather unavailable)
//   Panel 3 — HUMID / humidity % (or "--")
//   Panel 4 — RISE/SET / next sun event time (or "--")
//   Panel 5 — TIDE / "SOON" placeholder (tide_service not yet implemented)
//
// All functions must be called under gui_lvgl_lock().

namespace today_display {

// Show TODAY across all 6 panels.
// Hides the clock's own LVGL objects (digits, AM/PM, temp label) and creates
// its own label set on top.  If called while already active (e.g. a data
// refresh), updates existing label text without recreating objects.
void show(const current_conditions &data);

// Remove TODAY labels and restore the clock's LVGL objects.
// Idempotent: safe to call when TODAY is not the active display.
void clear();

}  // namespace today_display
