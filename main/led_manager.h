//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "drivers/leds.h"
#include "lvgl.h"

class led_manager {
public:
  static auto get() -> led_manager & {
    static led_manager instance;
    return instance;
  }

  void set_rgb(size_t index, uint8_t red, uint8_t green, uint8_t blue);
  void set_rgb_silent(size_t index, uint8_t red, uint8_t green, uint8_t blue);  // no async flush
  void set_all_rgb(uint8_t red, uint8_t green, uint8_t blue);
  void set_all_rgb_silent(uint8_t red, uint8_t green, uint8_t blue);  // no async flush
  void flush();
  void flush_blocking();       // wait for RMT idle then send (button path)
  void cancel_pending_flush(); // cancel async flush so no race with flush_blocking

  led_manager(const led_manager &led_manager) = delete;
  void operator=(const led_manager &) = delete;
  ~led_manager() = default;
  led_manager(led_manager&&) = delete;
  led_manager& operator=(led_manager&&) = delete;

  void off();

private:
  led_manager();
  void reschedule_update();

  leds_state state{};
};
