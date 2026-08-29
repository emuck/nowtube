// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

// Host-side tests for mode_policy.h / mode_policy.cpp and ModeManager.
// No ESP-IDF, FreeRTOS, or LVGL dependencies.

#include "mode_manager.h"
#include "mode_policy.h"

#include <cassert>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int s_pass = 0;
static int s_fail = 0;
static const char *s_suite = "";

#define SUITE(name) do { s_suite = (name); } while (0)

#define CHECK(expr) \
  do { \
    if (expr) { s_pass++; } \
    else { \
      s_fail++; \
      fprintf(stderr, "FAIL [%s] %s:%d — %s\n", s_suite, __FILE__, __LINE__, #expr); \
    } \
  } while (0)

#define CHECK_EQ(a, b)    CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)

// ---------------------------------------------------------------------------
// mode_policy_next — auto-cycle transitions
// ---------------------------------------------------------------------------

static void test_clock_to_today() {
  SUITE("clock_to_today");
  auto t = mode_policy_next(DisplayMode::CLOCK);
  CHECK_EQ(t.next, DisplayMode::TODAY);
  CHECK_EQ(t.delay_us, MODE_CYCLE_TODAY_US);
}

static void test_today_to_forecast() {
  SUITE("today_to_forecast");
  auto t = mode_policy_next(DisplayMode::TODAY);
  CHECK_EQ(t.next, DisplayMode::FORECAST);
  CHECK_EQ(t.delay_us, MODE_CYCLE_FORECAST_US);
}

static void test_forecast_to_clock() {
  SUITE("forecast_to_clock");
  auto t = mode_policy_next(DisplayMode::FORECAST);
  CHECK_EQ(t.next, DisplayMode::CLOCK);
  CHECK_EQ(t.delay_us, MODE_CYCLE_CLOCK_US);
}

static void test_spectrum_to_clock() {
  SUITE("spectrum_to_clock");
  auto t = mode_policy_next(DisplayMode::SPECTRUM);
  CHECK_EQ(t.next, DisplayMode::CLOCK);
  CHECK_EQ(t.delay_us, MODE_CYCLE_CLOCK_US);
}

static void test_date_to_clock() {
  SUITE("date_to_clock");
  // Legacy: DATE still falls through to CLOCK
  auto t = mode_policy_next(DisplayMode::DATE);
  CHECK_EQ(t.next, DisplayMode::CLOCK);
  CHECK_EQ(t.delay_us, MODE_CYCLE_CLOCK_US);
}

// ---------------------------------------------------------------------------
// Timing constant sanity checks
// ---------------------------------------------------------------------------

static void test_timing_constants() {
  SUITE("timing_constants");
  CHECK_EQ(MODE_CYCLE_CLOCK_US,    50ULL * 1'000'000ULL);
  CHECK_EQ(MODE_CYCLE_TODAY_US,    10ULL * 1'000'000ULL);
  CHECK_EQ(MODE_CYCLE_FORECAST_US, 10ULL * 1'000'000ULL);
  CHECK_EQ(MODE_CYCLE_SPECTRUM_US, 10ULL * 1'000'000ULL);
}

// ---------------------------------------------------------------------------
// mode_policy_next — configurable dwell times
// ---------------------------------------------------------------------------

static void test_custom_dwell_times() {
  SUITE("custom_dwell_times");
  cycle_config cfg;
  cfg.clock_s = 120; cfg.today_s = 20; cfg.forecast_s = 30; cfg.spectrum_s = 40;

  auto t = mode_policy_next(DisplayMode::CLOCK, cfg);
  CHECK_EQ(t.next, DisplayMode::TODAY);
  CHECK_EQ(t.delay_us, 20ULL * 1'000'000ULL);

  t = mode_policy_next(DisplayMode::TODAY, cfg);
  CHECK_EQ(t.next, DisplayMode::FORECAST);
  CHECK_EQ(t.delay_us, 30ULL * 1'000'000ULL);

  t = mode_policy_next(DisplayMode::FORECAST, cfg);
  CHECK_EQ(t.next, DisplayMode::SPECTRUM);
  CHECK_EQ(t.delay_us, 40ULL * 1'000'000ULL);

  t = mode_policy_next(DisplayMode::SPECTRUM, cfg);
  CHECK_EQ(t.next, DisplayMode::CLOCK);
  CHECK_EQ(t.delay_us, 120ULL * 1'000'000ULL);
}

// ---------------------------------------------------------------------------
// mode_policy_next — skip disabled modes
// ---------------------------------------------------------------------------

static void test_skip_today() {
  SUITE("skip_today");
  cycle_config cfg;
  cfg.today_s = 0;  // TODAY disabled; FORECAST still enabled (default=10)

  // CLOCK should jump directly to FORECAST
  auto t = mode_policy_next(DisplayMode::CLOCK, cfg);
  CHECK_EQ(t.next, DisplayMode::FORECAST);
  CHECK_EQ(t.delay_us, (uint64_t)cfg.forecast_s * 1'000'000ULL);

  // TODAY (if somehow entered) should still go to FORECAST
  t = mode_policy_next(DisplayMode::TODAY, cfg);
  CHECK_EQ(t.next, DisplayMode::FORECAST);
}

static void test_skip_forecast() {
  SUITE("skip_forecast");
  cycle_config cfg;
  cfg.forecast_s = 0;  // FORECAST disabled; TODAY still enabled (default=10)

  // CLOCK → TODAY
  auto t = mode_policy_next(DisplayMode::CLOCK, cfg);
  CHECK_EQ(t.next, DisplayMode::TODAY);

  // TODAY skips FORECAST → CLOCK
  t = mode_policy_next(DisplayMode::TODAY, cfg);
  CHECK_EQ(t.next, DisplayMode::CLOCK);
  CHECK_EQ(t.delay_us, (uint64_t)cfg.clock_s * 1'000'000ULL);
}

static void test_skip_both_ambient() {
  SUITE("skip_both_ambient");
  cycle_config cfg;
  cfg.today_s = 0;
  cfg.forecast_s = 0;
  cfg.spectrum_s = 0;  // CLOCK-only cycle

  auto t = mode_policy_next(DisplayMode::CLOCK, cfg);
  CHECK_EQ(t.next, DisplayMode::CLOCK);
  CHECK_EQ(t.delay_us, (uint64_t)cfg.clock_s * 1'000'000ULL);

  // DATE (legacy) also returns CLOCK
  t = mode_policy_next(DisplayMode::DATE, cfg);
  CHECK_EQ(t.next, DisplayMode::CLOCK);
}

// ---------------------------------------------------------------------------
// ModeManager helpers
// ---------------------------------------------------------------------------

static ModeManager &mm() {
  ModeManager &m = ModeManager::get();
  m.reset();  // restore to CLOCK before each test
  return m;
}

// ---------------------------------------------------------------------------
// ModeManager — button cycle behavior
// ---------------------------------------------------------------------------

static void test_button_cycle_full_round_trip() {
  SUITE("button_cycle_full");
  ModeManager &m = mm();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);

  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::TODAY);

  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::FORECAST);

  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::SPECTRUM);

  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

static void test_button_cycle_from_today() {
  SUITE("button_cycle_from_today");
  ModeManager &m = mm();
  m.set(DisplayMode::TODAY);
  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::FORECAST);
}

static void test_button_cycle_from_forecast() {
  SUITE("button_cycle_from_forecast");
  ModeManager &m = mm();
  m.set(DisplayMode::FORECAST);
  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::SPECTRUM);
}

static void test_button_cycle_from_spectrum() {
  SUITE("button_cycle_from_spectrum");
  ModeManager &m = mm();
  m.set(DisplayMode::SPECTRUM);
  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

static void test_button_cycle_from_date() {
  SUITE("button_cycle_from_date");
  // Legacy DATE still cycles to CLOCK
  ModeManager &m = mm();
  m.set(DisplayMode::DATE);
  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

// ---------------------------------------------------------------------------
// ModeManager — reset-to-clock (long-press) behavior
// ---------------------------------------------------------------------------

static void test_reset_from_today() {
  SUITE("reset_from_today");
  ModeManager &m = mm();
  m.set(DisplayMode::TODAY);
  m.reset();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

static void test_reset_from_forecast() {
  SUITE("reset_from_forecast");
  ModeManager &m = mm();
  m.set(DisplayMode::FORECAST);
  m.reset();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

static void test_reset_from_date() {
  SUITE("reset_from_date");
  ModeManager &m = mm();
  m.set(DisplayMode::DATE);
  m.reset();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

static void test_reset_from_clock_is_noop() {
  SUITE("reset_from_clock");
  ModeManager &m = mm();
  m.reset();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

// ---------------------------------------------------------------------------
// ModeManager — set() override
// ---------------------------------------------------------------------------

static void test_set_override() {
  SUITE("set_override");
  ModeManager &m = mm();
  m.set(DisplayMode::TODAY);
  CHECK_EQ(m.current(), DisplayMode::TODAY);
  m.set(DisplayMode::FORECAST);
  CHECK_EQ(m.current(), DisplayMode::FORECAST);
  m.set(DisplayMode::CLOCK);
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

// ---------------------------------------------------------------------------
// ModeManager — name()
// ---------------------------------------------------------------------------

static void test_mode_names() {
  SUITE("mode_names");
  CHECK_STREQ(ModeManager::name(DisplayMode::CLOCK),    "CLOCK");
  CHECK_STREQ(ModeManager::name(DisplayMode::DATE),     "DATE");
  CHECK_STREQ(ModeManager::name(DisplayMode::TODAY),    "TODAY");
  CHECK_STREQ(ModeManager::name(DisplayMode::FORECAST), "FORECAST");
  CHECK_STREQ(ModeManager::name(DisplayMode::GAME),     "GAME");
  CHECK_STREQ(ModeManager::name(DisplayMode::SPECTRUM), "SPECTRUM");
}

// ---------------------------------------------------------------------------
// GAME mode — policy, cycle, reset
// ---------------------------------------------------------------------------

static void test_game_policy_holds() {
  SUITE("game_policy_holds");
  // GAME mode must not auto-transition to another mode.
  auto t = mode_policy_next(DisplayMode::GAME);
  CHECK_EQ(t.next, DisplayMode::GAME);
  // Suppression delay must be at least 30 minutes.
  CHECK(t.delay_us >= 30ULL * 60ULL * 1'000'000ULL);
}

static void test_button_cycle_from_game() {
  SUITE("button_cycle_from_game");
  // cycle() from GAME exits to CLOCK (defensive — game normally exits via long-press).
  ModeManager &m = mm();
  m.set(DisplayMode::GAME);
  m.cycle();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

static void test_reset_from_game() {
  SUITE("reset_from_game");
  ModeManager &m = mm();
  m.set(DisplayMode::GAME);
  m.reset();
  CHECK_EQ(m.current(), DisplayMode::CLOCK);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  // mode_policy_next — auto-cycle transitions (default cycle_config)
  test_clock_to_today();
  test_today_to_forecast();
  test_forecast_to_clock();
  test_spectrum_to_clock();
  test_date_to_clock();

  // mode_policy_next — configurable dwell times
  test_custom_dwell_times();

  // mode_policy_next — skip disabled modes
  test_skip_today();
  test_skip_forecast();
  test_skip_both_ambient();

  // Timing constants
  test_timing_constants();

  // ModeManager — button cycle
  test_button_cycle_full_round_trip();
  test_button_cycle_from_today();
  test_button_cycle_from_forecast();
  test_button_cycle_from_spectrum();
  test_button_cycle_from_date();

  // ModeManager — reset (long-press)
  test_reset_from_today();
  test_reset_from_forecast();
  test_reset_from_date();
  test_reset_from_clock_is_noop();

  // ModeManager — set override
  test_set_override();

  // ModeManager — name()
  test_mode_names();

  // GAME mode — policy, cycle, reset
  test_game_policy_holds();
  test_button_cycle_from_game();
  test_reset_from_game();

  printf("\n%s: %d passed, %d failed\n",
         s_fail == 0 ? "PASS" : "FAIL", s_pass, s_fail);
  return s_fail == 0 ? 0 : 1;
}
