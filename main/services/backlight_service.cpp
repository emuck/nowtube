//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "services/backlight_service.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "drivers/lcds.h"
#include "drivers/leds.h"
#include "led_manager.h"
#include "services/config_service.h"
#include "services/status_service.h"

namespace backlight_service {

static constexpr const char *TAG = "backlight";
static constexpr uint64_t BREATH_RISE_US = 2ULL * 1'000'000ULL;
static constexpr uint64_t BREATH_FALL_US = 3ULL * 1'000'000ULL;
static constexpr uint64_t BREATH_PERIOD_US = BREATH_RISE_US + BREATH_FALL_US;
static constexpr uint64_t BREATH_TICK_MS = 100u;
static const uint8_t BRIGHTNESS_STEPS[] = {100, 70, 40, 20, 5, 0};
static constexpr size_t BRIGHTNESS_STEP_COUNT =
    sizeof(BRIGHTNESS_STEPS) / sizeof(BRIGHTNESS_STEPS[0]);

enum class Mode { Normal, Breathable, Mixed };
struct led_color {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

static const led_color LED_COLORS[] = {
    {228, 112, 37}, {200, 0, 0},   {0, 200, 0},   {0, 0, 200},
    {0, 200, 200}, {200, 0, 200}, {200, 150, 0},
};
static constexpr size_t LED_COLOR_COUNT =
    sizeof(LED_COLORS) / sizeof(LED_COLORS[0]);
static const char *const LED_COLOR_NAMES[] = {
    "warm orange", "red", "green", "blue", "cyan", "magenta", "amber",
};

static Mode s_mode = Mode::Normal;
static uint8_t s_brightness_pct = 60;
static esp_timer_handle_t s_breath_timer = nullptr;
static int64_t s_breath_start_us = 0;
// Protects s_breath_start_us: int64_t is two 32-bit stores on Xtensa LX6 and
// therefore not atomic.  The timer callback and the mode-change functions run
// on different tasks, so a torn read/write is possible without a lock.
static portMUX_TYPE s_timing_lock = portMUX_INITIALIZER_UNLOCKED;
static size_t s_led_color_idx = 0;
static bool s_leds_on = false;
static bool s_mixed_uniform = true;
static uint64_t s_mixed_random_cycle = 0;
static uint8_t s_mixed_led_indices[NUM_LEDS] = {0};

static float ease_in_out(float t) {
  if (t <= 0.f) return 0.f;
  if (t >= 1.f) return 1.f;
  return 0.5f - 0.5f * cosf(t * 3.14159265f);
}

static float breath_brightness(uint64_t in_cycle_us) {
  if (in_cycle_us < BREATH_RISE_US) {
    float rise_t = static_cast<float>(in_cycle_us) / static_cast<float>(BREATH_RISE_US);
    return ease_in_out(rise_t);
  }

  uint64_t fall_us = in_cycle_us - BREATH_RISE_US;
  float fall_t = static_cast<float>(fall_us) / static_cast<float>(BREATH_FALL_US);
  return 1.f - ease_in_out(fall_t);
}

static void apply_led_color() {
  if (!s_leds_on) {
    return;
  }

  size_t idx = s_led_color_idx;
  if (idx >= LED_COLOR_COUNT) {
    idx = 0;
  }

  const auto &c = LED_COLORS[idx];
  auto &leds = led_manager::get();
  leds.set_all_rgb_silent(c.r, c.g, c.b);
  leds.cancel_pending_flush();
  leds.flush_blocking();
}

static void set_strip_off() {
  auto &leds = led_manager::get();
  leds.off();
  leds.cancel_pending_flush();
  leds.flush_blocking();
}

static void breath_timer_cb(void *) {
  const bool breathable = (s_mode == Mode::Breathable);
  const bool mixed = (s_mode == Mode::Mixed);
  if (!breathable && !mixed) {
    return;
  }

  portENTER_CRITICAL(&s_timing_lock);
  int64_t breath_start = s_breath_start_us;
  portEXIT_CRITICAL(&s_timing_lock);
  int64_t elapsed = esp_timer_get_time() - breath_start;
  if (elapsed < 0) {
    elapsed = 0;
  }
  uint64_t in_cycle = static_cast<uint64_t>(elapsed) % BREATH_PERIOD_US;
  float brightness = breath_brightness(in_cycle);

  leds_state frame = {};
  if (breathable && !s_leds_on) {
    leds_update_if_free(&frame);
    return;
  }

  if (mixed) {
    uint64_t cycle_count = static_cast<uint64_t>(elapsed) / BREATH_PERIOD_US;
    if (s_mixed_uniform) {
      size_t idx = static_cast<size_t>(cycle_count % LED_COLOR_COUNT);
      const auto &c = LED_COLORS[idx];
      for (size_t i = 0; i < NUM_LEDS; i++) {
        frame.set_rgb(
            i,
          static_cast<uint8_t>(brightness * static_cast<float>(c.r) + 0.5f),
          static_cast<uint8_t>(brightness * static_cast<float>(c.g) + 0.5f),
          static_cast<uint8_t>(brightness * static_cast<float>(c.b) + 0.5f));
      }
    } else {
      if (cycle_count != s_mixed_random_cycle) {
        s_mixed_random_cycle = cycle_count;
        for (size_t i = 0; i < NUM_LEDS; i++) {
          s_mixed_led_indices[i] = static_cast<uint8_t>(rand() % LED_COLOR_COUNT);
        }
      }
      for (size_t i = 0; i < NUM_LEDS; i++) {
        const auto &c = LED_COLORS[s_mixed_led_indices[i]];
        frame.set_rgb(
            i,
            static_cast<uint8_t>(brightness * static_cast<float>(c.r) + 0.5f),
            static_cast<uint8_t>(brightness * static_cast<float>(c.g) + 0.5f),
            static_cast<uint8_t>(brightness * static_cast<float>(c.b) + 0.5f));
      }
    }
  } else {
    size_t idx = (s_led_color_idx >= LED_COLOR_COUNT) ? 0 : s_led_color_idx;
    const auto &c = LED_COLORS[idx];
    for (size_t i = 0; i < NUM_LEDS; i++) {
      frame.set_rgb(
          i,
          static_cast<uint8_t>(brightness * static_cast<float>(c.r) + 0.5f),
          static_cast<uint8_t>(brightness * static_cast<float>(c.g) + 0.5f),
          static_cast<uint8_t>(brightness * static_cast<float>(c.b) + 0.5f));
    }
  }
  leds_update_if_free(&frame);
}

bool init(uint8_t initial_brightness_pct) {
  s_brightness_pct = initial_brightness_pct;
  status_service::set_brightness(s_brightness_pct);
  srand(static_cast<unsigned>(esp_random()));

  esp_timer_create_args_t args = {
      .callback = breath_timer_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "backlight_breath",
      .skip_unhandled_events = false,
  };
  esp_err_t err = esp_timer_create(&args, &s_breath_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create breath timer: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

void cycle_brightness() {
  size_t next_idx = BRIGHTNESS_STEP_COUNT - 1;
  for (size_t i = 0; i < BRIGHTNESS_STEP_COUNT; i++) {
    if (BRIGHTNESS_STEPS[i] == s_brightness_pct) {
      next_idx = (i + 1) % BRIGHTNESS_STEP_COUNT;
      break;
    }
  }

  s_brightness_pct = BRIGHTNESS_STEPS[next_idx];
  lcds_set_brightness(s_brightness_pct);
  config_service::set_display_brightness(s_brightness_pct);
  status_service::set_brightness(s_brightness_pct);
  ESP_LOGI(TAG, "Brightness -> %u%%", static_cast<unsigned>(s_brightness_pct));
}

void cycle_led_selection() {
  if (s_mode == Mode::Mixed) {
    s_mixed_uniform = !s_mixed_uniform;
    ESP_LOGI(TAG, "Mixed -> %s", s_mixed_uniform ? "uniform" : "random");
    return;
  }

  size_t next = (s_led_color_idx + 1) % (LED_COLOR_COUNT + 1);
  s_led_color_idx = next;
  if (next == LED_COLOR_COUNT) {
    s_leds_on = false;
    if (s_mode == Mode::Normal) {
      set_strip_off();
    } else {
      auto &leds = led_manager::get();
      leds.set_all_rgb_silent(0, 0, 0);
      leds.cancel_pending_flush();
    }
    ESP_LOGI(TAG, "LEDs -> off");
    return;
  }

  s_leds_on = true;
  if (s_mode == Mode::Normal) {
    apply_led_color();
  }
  ESP_LOGI(TAG, "LEDs -> %s", LED_COLOR_NAMES[next]);
}

void cycle_backlight_mode() {
  if (s_mode == Mode::Normal) {
    auto &leds = led_manager::get();
    leds.cancel_pending_flush();
    s_mode = Mode::Breathable;
    portENTER_CRITICAL(&s_timing_lock);
    s_breath_start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_timing_lock);
    esp_timer_start_periodic(s_breath_timer, BREATH_TICK_MS * 1000ULL);
    ESP_LOGI(TAG, "Backlight -> breathable");
    return;
  }

  if (s_mode == Mode::Breathable) {
    auto &leds = led_manager::get();
    leds.cancel_pending_flush();
    s_mode = Mode::Mixed;
    portENTER_CRITICAL(&s_timing_lock);
    s_breath_start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_timing_lock);
    ESP_LOGI(TAG, "Backlight -> mixed");
    return;
  }

  s_mode = Mode::Normal;
  esp_timer_stop(s_breath_timer);
  if (s_leds_on) {
    apply_led_color();
  } else {
    set_strip_off();
  }
  ESP_LOGI(TAG, "Backlight -> normal");
}

const char *get_mode_name() {
  switch (s_mode) {
  case Mode::Normal:    return "normal";
  case Mode::Breathable: return "breathable";
  case Mode::Mixed:     return "mixed";
  }
  return "normal";
}

const char *get_led_color_name() {
  if (!s_leds_on || s_led_color_idx >= LED_COLOR_COUNT) {
    return "off";
  }
  return LED_COLOR_NAMES[s_led_color_idx];
}

bool set_mode(const char *name) {
  Mode target;
  if (strcmp(name, "normal") == 0)      target = Mode::Normal;
  else if (strcmp(name, "breathable") == 0) target = Mode::Breathable;
  else if (strcmp(name, "mixed") == 0)  target = Mode::Mixed;
  else return false;

  if (target == s_mode) return true;

  if (s_mode == Mode::Normal && target != Mode::Normal) {
    // Normal → animated: start breath timer
    led_manager::get().cancel_pending_flush();
    portENTER_CRITICAL(&s_timing_lock);
    s_breath_start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_timing_lock);
    esp_timer_start_periodic(s_breath_timer, BREATH_TICK_MS * 1000ULL);
  } else if (target == Mode::Normal) {
    // Animated → Normal: stop breath timer, restore static color
    esp_timer_stop(s_breath_timer);
    if (s_leds_on) {
      apply_led_color();
    } else {
      set_strip_off();
    }
  } else {
    // Breathable ↔ Mixed: timer already running, just reset the phase
    portENTER_CRITICAL(&s_timing_lock);
    s_breath_start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_timing_lock);
  }

  s_mode = target;
  ESP_LOGI(TAG, "Backlight mode -> %s", name);
  return true;
}

bool set_led_color(const char *name) {
  if (strcmp(name, "off") == 0) {
    s_leds_on = false;
    s_led_color_idx = LED_COLOR_COUNT;  // sentinel
    if (s_mode == Mode::Normal) {
      set_strip_off();
    }
    ESP_LOGI(TAG, "LEDs -> off");
    return true;
  }
  for (size_t i = 0; i < LED_COLOR_COUNT; i++) {
    if (strcmp(name, LED_COLOR_NAMES[i]) == 0) {
      s_led_color_idx = i;
      s_leds_on = true;
      if (s_mode == Mode::Normal) {
        apply_led_color();
      }
      ESP_LOGI(TAG, "LEDs -> %s", name);
      return true;
    }
  }
  return false;
}

void set_recovery_cue() {
  // Stop any breath animation, show a solid dim blue across all LEDs to
  // signal that the device is in Wi-Fi setup (recovery AP) mode.
  if (s_breath_timer != nullptr) {
    esp_timer_stop(s_breath_timer);
  }

  // Apply a temporary 40% LCD brightness so the recovery screen is always
  // readable — regardless of the user's saved normal brightness (which may be
  // 0%).  We do NOT touch s_brightness_pct or NVS; the user's preference is
  // restored by cancel_recovery_cue() when recovery mode is exited.
  lcds_set_brightness(40);
  ESP_LOGI(TAG, "Recovery cue: LCD brightness -> 40%% (temporary, saved=%u%%)",
           static_cast<unsigned>(s_brightness_pct));

  auto &leds = led_manager::get();
  leds.set_all_rgb_silent(0, 0, 100);
  leds.cancel_pending_flush();
  leds.flush_blocking();
  ESP_LOGI(TAG, "Recovery cue: LEDs -> dim blue");
}

void cancel_recovery_cue() {
  // Restore the user's saved LCD brightness (undoes the temporary 40% override).
  lcds_set_brightness(s_brightness_pct);
  ESP_LOGI(TAG, "Recovery cue cancelled: LCD brightness restored to %u%%",
           static_cast<unsigned>(s_brightness_pct));

  if (s_mode == Mode::Normal) {
    if (s_leds_on) {
      apply_led_color();
    } else {
      set_strip_off();
    }
  } else {
    // Breathable or Mixed — restart the breath animation from the top of the cycle.
    portENTER_CRITICAL(&s_timing_lock);
    s_breath_start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_timing_lock);
    esp_timer_start_periodic(s_breath_timer, BREATH_TICK_MS * 1000ULL);
  }
}

void pause() {
  if (s_breath_timer != nullptr) {
    esp_timer_stop(s_breath_timer);
  }
}

void resume() {
  if (s_mode == Mode::Normal) {
    if (s_leds_on) {
      apply_led_color();
    } else {
      set_strip_off();
    }
  } else {
    portENTER_CRITICAL(&s_timing_lock);
    s_breath_start_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_timing_lock);
    esp_timer_start_periodic(s_breath_timer, BREATH_TICK_MS * 1000ULL);
  }
}

void finalize_boot_state() {
  auto &leds = led_manager::get();
  leds.set_all_rgb_silent(0, 0, 0);
  leds.cancel_pending_flush();
  leds.flush_blocking();
  leds.flush_blocking();
  leds_off();
  leds_off();
}

}
