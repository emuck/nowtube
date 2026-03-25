//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <lvgl.h>
#include <memory>
#include <string>

#include "drivers/lcds.h"
#include "flapper.h"
#include "gui.h"

#include "flap_sequence.h"

class clock {
public:
  static auto get() -> clock & {
    static clock instance;
    return instance;
  }

  void update();
  void shuffle();
  void show_value(const char *str);
  void show_value_instant(const char *str);  // no flapper; use when switching to WEATHER
  void show_date();
  void set_temp(const char *value, const char *unit);        // animated update (after first render)
  void set_temp_static(const char *value);                   // quiet set (safe before first render)
  void clock_tick();   // 500ms: blink colon + run update() at minute boundary (called from LVGL timer)

  // Pause the clock tick timer and hide all clock LVGL objects.
  // Call before rendering an overlay mode (TODAY, FORECAST, etc.).
  // Idempotent: safe to call when already hidden.
  void hide_all();

  // Resume the clock tick timer and show all clock LVGL objects.
  // Call when returning from an overlay mode to CLOCK or WEATHER.
  // Idempotent: safe to call when already visible.
  void restore_all();

  // Recovery AP screen: shows SSID + IP using lv_font_montserrat_14 (full ASCII).
  // Called under gui_lvgl_lock().  clear_recovery_screen() returns to normal rendering.
  void show_recovery_screen();
  void clear_recovery_screen();

  clock(clock const &) = delete;
  void operator=(const clock &) = delete;
  clock(clock&&) = delete;
  clock& operator=(clock&&) = delete;

  ~clock();
private:
  clock();

  std::array<lv_obj_t *, NUM_LCDS> background_images{};
  std::array<lv_obj_t *, NUM_LCDS-1> digit_images{};
  std::array<std::unique_ptr<flapper>, NUM_LCDS> flappers{};
  std::array<std::unique_ptr<flap_sequence>, NUM_LCDS-1> flap_sequences{};
  std::array<lv_timer_t *, NUM_LCDS-1> delayed_start_timers{};
  lv_obj_t *ampm_image{};
  lv_obj_t *temp_label{};
  lv_timer_t *clock_update_timer{};
  time_t next_update_time_{0};  // next minute boundary (for single 500ms timer)
  bool initialized_{false};
  std::array<lv_obj_t *, NUM_LCDS> recovery_labels{};  // nullptr when not in recovery mode
  void delayed_start_flap_sequence(size_t index);
  void animate_panel(size_t i, const std::string &desired, uint32_t &delay);
  void colon_blink_tick();
};
