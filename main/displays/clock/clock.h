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
  void set_wifi_connected(bool connected);
  // Reapply the configured font pack to existing clock objects.  The caller
  // must hold gui_lvgl_lock().
  void reload_theme();
  void shuffle();
  void show_value(const char *str);
  void show_value_instant(const char *str);  // no flapper; use when switching to WEATHER
  void show_date();
  void set_temp(const char *value, const char *unit);        // animated update (after first render)
  void set_temp_static(const char *value);                   // quiet set (safe before first render)
  void set_weather_condition(uint16_t code);                 // update weather icon source for panel 0
  void set_sun_times(uint16_t sunrise_min, uint16_t sunset_min); // minutes-since-midnight from conditions
  void show_moon_test();                                     // render 6 phases across all panels (debug)
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
  lv_obj_t *wifi_status_icon{};
  bool wifi_connected_{false};
  lv_obj_t *weather_icon_{};    // weather condition icon, panel 0
  lv_obj_t *moon_canvas_{};    // dynamically rendered moon, panel 0 (shown at night)
  void     *moon_canvas_buf_{}; // PSRAM backing buffer for moon_canvas_
  uint16_t  weather_code_{0};
  uint16_t  sunrise_min_{0};   // minutes since midnight (from conditions)
  uint16_t  sunset_min_{0};
  float     last_moon_phase_{-1.0f}; // phase at last render, -1 = never rendered
  enum class Panel0Mode { DIGIT, WEATHER_ICON, MOON };
  Panel0Mode panel0_mode_{Panel0Mode::DIGIT};
  lv_timer_t *clock_update_timer{};
  time_t next_update_time_{0};  // next minute boundary (for single 500ms timer)
  bool initialized_{false};
  std::array<lv_obj_t *, NUM_LCDS> recovery_labels{};  // nullptr when not in recovery mode
  void delayed_start_flap_sequence(size_t index);
  void animate_panel(size_t i, const std::string &desired, uint32_t &delay);
  void colon_blink_tick();
  void update_panel0(bool first_is_zero);         // pick DIGIT / WEATHER_ICON / MOON for panel 0
  void set_panel0_mode(Panel0Mode mode);          // apply visibility for the chosen mode
  void render_moon_to_canvas(float phase);        // draw moon PNG + shadow into moon_canvas_
  bool is_nighttime() const;                      // true between sunset_min_ and sunrise_min_
  static float current_moon_phase(time_t now);   // 0.0=new … 0.5=full … 1.0=new
};
