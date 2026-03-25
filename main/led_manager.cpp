//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "led_manager.h"

#include "drivers/leds.h"
#include "gui.h"

static void flush_callback_wrapper(void *user_data) {
  auto *instance = static_cast<led_manager *>(user_data);
  instance->flush();
}

led_manager::led_manager() {
  // Don't block here; state is zeros. Boot sets off and flush_blocking() for known-good state.
}

void led_manager::set_rgb(size_t index, uint8_t red, uint8_t green,
                          uint8_t blue) {
  state.set_rgb(index, red, green, blue);
  reschedule_update();
}

void led_manager::set_rgb_silent(size_t index, uint8_t red, uint8_t green,
                                  uint8_t blue) {
  state.set_rgb(index, red, green, blue);
}

void led_manager::set_all_rgb(uint8_t red, uint8_t green, uint8_t blue) {
  for (size_t i = 0; i < NUM_LEDS; i++) {
    state.set_rgb(i, red, green, blue);
  }
  reschedule_update();
}

void led_manager::set_all_rgb_silent(uint8_t red, uint8_t green, uint8_t blue) {
  for (size_t i = 0; i < NUM_LEDS; i++) {
    state.set_rgb(i, red, green, blue);
  }
}

void led_manager::cancel_pending_flush() {
  gui_lvgl_lock();
  lv_async_call_cancel(flush_callback_wrapper, this);
  gui_lvgl_unlock();
}

void led_manager::flush() {
  bool updated = leds_update_if_free(&state);
  if (!updated) {
    // RMT is busy — retry via a new async call. Do NOT call reschedule_update()
    // here: if flush() was invoked from within lv_async_timer_cb, calling
    // lv_async_call_cancel on the currently-executing info struct causes a
    // double-free when lv_async_timer_cb frees it afterward.
    gui_lvgl_lock();
    lv_async_call(flush_callback_wrapper, this);
    gui_lvgl_unlock();
  }
}

void led_manager::reschedule_update() {
  gui_lvgl_lock();
  lv_async_call_cancel(flush_callback_wrapper, this);
  lv_async_call(flush_callback_wrapper, this);
  gui_lvgl_unlock();
}

void led_manager::off() {
  state.clear();
  reschedule_update();
}

void led_manager::flush_blocking() {
  // Wait for any in-flight RMT transfer to finish (e.g. from a previous async flush),
  // then send and wait for our transfer. Use long timeout so we don't give up too soon.
  constexpr uint32_t WAIT_IDLE_MS = 2000;
  if (!leds_update_wait(&state, WAIT_IDLE_MS)) {
    // Timeout: channel stayed busy; skip this update (strip keeps previous state).
    return;
  }
}
