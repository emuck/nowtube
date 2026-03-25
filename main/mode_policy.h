// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

#pragma once

// Pure auto-cycle policy helper — no ESP-IDF, FreeRTOS, or LVGL dependencies.
// This module owns the timing constants and the next-state decision function so
// both can be exercised by host-side tests without hardware.

#include <cstdint>

#include "mode_manager.h"

// ---------------------------------------------------------------------------
// Default timing constants (document the factory defaults; cycle_config carries
// the live values at runtime)

static constexpr uint64_t MODE_CYCLE_CLOCK_US    = 50ULL * 1'000'000ULL;
static constexpr uint64_t MODE_CYCLE_TODAY_US    = 10ULL * 1'000'000ULL;
static constexpr uint64_t MODE_CYCLE_FORECAST_US = 10ULL * 1'000'000ULL;
// ---------------------------------------------------------------------------
// Cycle configuration (pure; no ESP-IDF deps; extracted from device_config)

struct cycle_config {
  uint16_t clock_s    = 50;  // CLOCK dwell in seconds
  uint16_t today_s    = 10;  // TODAY dwell; 0 = skip TODAY in cycle
  uint16_t forecast_s = 10;  // FORECAST dwell; 0 = skip FORECAST in cycle
};

// ---------------------------------------------------------------------------
// Policy result

struct mode_transition {
  DisplayMode next;
  uint64_t    delay_us;
};

// Given the current display mode and cycle configuration, return the next mode
// and how long to stay in it (delay_us).
//
// Skip rules:
//   CLOCK    → TODAY (if enabled) → FORECAST (if enabled) → CLOCK
//   Disabled modes are skipped; if both TODAY and FORECAST are disabled,
//   the cycle stays on CLOCK.
//
// |cfg| defaults to the factory defaults so call-sites not yet updated compile.
mode_transition mode_policy_next(DisplayMode cur,
                                 const cycle_config &cfg = cycle_config{});
