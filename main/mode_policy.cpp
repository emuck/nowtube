// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

#include "mode_policy.h"

mode_transition mode_policy_next(DisplayMode cur, const cycle_config &cfg) {
  const uint64_t clock_us    = static_cast<uint64_t>(cfg.clock_s)    * 1'000'000ULL;
  const uint64_t today_us    = static_cast<uint64_t>(cfg.today_s)    * 1'000'000ULL;
  const uint64_t forecast_us = static_cast<uint64_t>(cfg.forecast_s) * 1'000'000ULL;
  const uint64_t spectrum_us = static_cast<uint64_t>(cfg.spectrum_s) * 1'000'000ULL;

  switch (cur) {
  case DisplayMode::CLOCK:
    if (cfg.today_s > 0)    return {DisplayMode::TODAY,    today_us};
    if (cfg.forecast_s > 0) return {DisplayMode::FORECAST, forecast_us};
    if (cfg.spectrum_s > 0) return {DisplayMode::SPECTRUM, spectrum_us};
    return {DisplayMode::CLOCK, clock_us};

  case DisplayMode::TODAY:
    if (cfg.forecast_s > 0) return {DisplayMode::FORECAST, forecast_us};
    if (cfg.spectrum_s > 0) return {DisplayMode::SPECTRUM, spectrum_us};
    return {DisplayMode::CLOCK, clock_us};

  case DisplayMode::FORECAST:
    if (cfg.spectrum_s > 0) return {DisplayMode::SPECTRUM, spectrum_us};
    return {DisplayMode::CLOCK, clock_us};

  case DisplayMode::SPECTRUM:
  case DisplayMode::DATE:
    return {DisplayMode::CLOCK, clock_us};

  case DisplayMode::GAME:
    return {DisplayMode::GAME, 3600ULL * 1'000'000ULL};
  }
  return {DisplayMode::CLOCK, clock_us};
}
