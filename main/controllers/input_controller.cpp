//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "controllers/input_controller.h"

#include <esp_timer.h>

#include "controllers/display_controller.h"
#include "displays/game/game_display.h"
#include "services/backlight_service.h"

namespace input_controller {

static bool s_clock_only = true;
static void (*s_apply_mode)(DisplayMode) = nullptr;
static void (*s_schedule_clock_cycle)() = nullptr;
static void (*s_on_wifi_recovery_requested)() = nullptr;
static int64_t s_left_push_us   = 0;
static int64_t s_middle_push_us = 0;
static int64_t s_right_push_us  = 0;
static constexpr int64_t LONG_PRESS_US = 1'000'000;

void init(bool clock_only, void (*apply_mode)(DisplayMode),
          void (*schedule_clock_cycle)(),
          void (*on_wifi_recovery_requested)()) {
  s_clock_only = clock_only;
  s_apply_mode = apply_mode;
  s_schedule_clock_cycle = schedule_clock_cycle;
  s_on_wifi_recovery_requested = on_wifi_recovery_requested;
}

void on_button_tapped(touchpad_button_t button) {
  // The iot_touchpad library fires TAP on every release, including long presses.
  // Suppress the tap action if the button was held long enough to be a long press,
  // so that a long-press does not also trigger the short-tap action.

  // In GAME mode, short taps are consumed by game input; long-press left is
  // handled in on_button_touched and suppresses the tap here.
  if (display_controller::current_mode() == DisplayMode::GAME) {
    if (button == TOUCHPAD_LEFT_BUTTON &&
        (esp_timer_get_time() - s_left_push_us) > LONG_PRESS_US) return;
    game_display::handle_input(button);
    return;
  }

  switch (button) {
  case TOUCHPAD_LEFT_BUTTON:
    if ((esp_timer_get_time() - s_left_push_us) > LONG_PRESS_US) break;
    if (s_clock_only) {
      display_controller::set_mode(DisplayMode::CLOCK);
      if (s_apply_mode != nullptr) {
        s_apply_mode(DisplayMode::CLOCK);
      }
    } else {
      display_controller::cycle_mode();
      if (s_apply_mode != nullptr) {
        s_apply_mode(display_controller::current_mode());
      }
      if (s_schedule_clock_cycle != nullptr) {
        s_schedule_clock_cycle();
      }
    }
    break;
  case TOUCHPAD_MIDDLE_BUTTON:
    if ((esp_timer_get_time() - s_middle_push_us) > LONG_PRESS_US) break;
    backlight_service::cycle_brightness();
    break;
  case TOUCHPAD_RIGHT_BUTTON:
    if ((esp_timer_get_time() - s_right_push_us) > LONG_PRESS_US) break;
    backlight_service::cycle_led_selection();
    break;
  }
}

void on_button_touched(touchpad_button_t button, bool is_pressed) {
  if (button == TOUCHPAD_LEFT_BUTTON) {
    if (is_pressed) {
      s_left_push_us = esp_timer_get_time();
    } else if ((esp_timer_get_time() - s_left_push_us) > LONG_PRESS_US) {
      // Long-press left toggles GAME mode.
      if (display_controller::current_mode() == DisplayMode::GAME) {
        display_controller::reset_mode();
        if (s_apply_mode != nullptr) {
          s_apply_mode(DisplayMode::CLOCK);
        }
        if (s_schedule_clock_cycle != nullptr) {
          s_schedule_clock_cycle();
        }
      } else {
        display_controller::set_mode(DisplayMode::GAME);
        if (s_apply_mode != nullptr) {
          s_apply_mode(DisplayMode::GAME);
        }
      }
    }
    return;
  }

  if (button == TOUCHPAD_MIDDLE_BUTTON) {
    if (is_pressed) {
      s_middle_push_us = esp_timer_get_time();
    } else if ((esp_timer_get_time() - s_middle_push_us) > LONG_PRESS_US) {
      if (s_on_wifi_recovery_requested != nullptr) {
        s_on_wifi_recovery_requested();
      }
    }
    return;
  }

  if (button == TOUCHPAD_RIGHT_BUTTON) {
    if (is_pressed) {
      s_right_push_us = esp_timer_get_time();
    } else if ((esp_timer_get_time() - s_right_push_us) > LONG_PRESS_US) {
      backlight_service::cycle_backlight_mode();
    }
  }
}

}
