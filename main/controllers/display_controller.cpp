//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "controllers/display_controller.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>

#include "displays/clock/clock.h"
#include "displays/forecast/forecast_display.h"
#include "displays/game/game_display.h"
#include "displays/today/today_display.h"
#include "gui.h"
#include "services/status_service.h"

namespace display_controller {

static constexpr const char *TAG = "display_ctrl";

// Conditions state — written from conditions_service, read from LVGL task.
static current_conditions s_conditions      = {};
static SemaphoreHandle_t  s_conditions_mutex = nullptr;

// Forecast state — written from forecast_service, read from LVGL task.
static forecast_data     s_forecast       = {};
static SemaphoreHandle_t s_forecast_mutex = nullptr;

// Set to true when the Wi-Fi recovery AP is active.  Blocks all further
// clock/weather/date rendering so the recovery screen stays visible.
static bool s_recovery_active = false;

// ---------------------------------------------------------------------------

void init() {
  s_conditions_mutex = xSemaphoreCreateMutex();
  if (s_conditions_mutex == nullptr) {
    ESP_LOGE(TAG, "Failed to create conditions mutex");
  }
  s_forecast_mutex = xSemaphoreCreateMutex();
  if (s_forecast_mutex == nullptr) {
    ESP_LOGE(TAG, "Failed to create forecast mutex");
  }
  status_service::set_mode(ModeManager::name(ModeManager::get().current()));
  ESP_LOGI(TAG, "Display controller initialized");
}

// ---------------------------------------------------------------------------
// Internal helpers

static void conditions_lock() {
  if (s_conditions_mutex != nullptr) {
    xSemaphoreTake(s_conditions_mutex, portMAX_DELAY);
  }
}

static void conditions_unlock() {
  if (s_conditions_mutex != nullptr) {
    xSemaphoreGive(s_conditions_mutex);
  }
}

static void forecast_lock() {
  if (s_forecast_mutex != nullptr) {
    xSemaphoreTake(s_forecast_mutex, portMAX_DELAY);
  }
}

static void forecast_unlock() {
  if (s_forecast_mutex != nullptr) {
    xSemaphoreGive(s_forecast_mutex);
  }
}

// ---------------------------------------------------------------------------

void render_boot_screen() {
  gui_lvgl_lock();
  clock::get().update();
  gui_lvgl_unlock();
}

void show_recovery_screen() {
  s_recovery_active = true;
  gui_lvgl_lock();
  clock::get().show_recovery_screen();
  gui_lvgl_unlock();
  ESP_LOGI(TAG, "Recovery screen active");
}

bool is_recovery_active() {
  return s_recovery_active;
}

void cancel_recovery_screen() {
  if (!s_recovery_active) return;
  s_recovery_active = false;
  gui_lvgl_lock();
  clock::get().clear_recovery_screen();
  clock::get().update();
  gui_lvgl_unlock();
  status_service::set_mode(ModeManager::name(ModeManager::get().current()));
  ESP_LOGI(TAG, "Recovery screen cancelled");
}

void apply_mode(DisplayMode mode) {
  if (s_recovery_active) return;
  // Snapshot data under their locks before taking the LVGL lock,
  // so no two locks are ever held simultaneously.
  current_conditions cond_snapshot = {};
  if (mode == DisplayMode::TODAY) {
    conditions_lock();
    cond_snapshot = s_conditions;
    conditions_unlock();
  }

  forecast_data fore_snapshot = {};
  if (mode == DisplayMode::FORECAST) {
    forecast_lock();
    fore_snapshot = s_forecast;
    forecast_unlock();
  }

  gui_lvgl_lock();
  if (mode != DisplayMode::TODAY) {
    today_display::clear();
  }
  if (mode != DisplayMode::FORECAST) {
    forecast_display::clear();
  }
  if (mode != DisplayMode::GAME) {
    game_display::clear();
  }
  switch (mode) {
  case DisplayMode::CLOCK:
    clock::get().update();
    break;
  case DisplayMode::DATE:  // reserved — treat as CLOCK
    clock::get().update();
    break;
  case DisplayMode::TODAY:
    today_display::show(cond_snapshot);
    break;
  case DisplayMode::FORECAST:
    forecast_display::show(fore_snapshot);
    break;
  case DisplayMode::GAME:
    game_display::show();
    break;
  }
  gui_lvgl_unlock();
}

void set_mode(DisplayMode mode) {
  ModeManager::get().set(mode);
  status_service::set_mode(ModeManager::name(mode));
}

void cycle_mode() {
  ModeManager::get().cycle();
  status_service::set_mode(ModeManager::name(ModeManager::get().current()));
}

void reset_mode() {
  ModeManager::get().reset();
  status_service::set_mode(ModeManager::name(DisplayMode::CLOCK));
}

DisplayMode current_mode() {
  return ModeManager::get().current();
}

void on_conditions_updated(const current_conditions &data) {
  if (s_recovery_active) return;

  conditions_lock();
  s_conditions = data;
  conditions_unlock();

  gui_lvgl_lock();
  // Always keep the clock's temperature, weather icon, and sun times current.
  clock::get().set_temp(data.temp_value, data.temp_unit);
  clock::get().set_weather_condition(data.weather_code);
  clock::get().set_sun_times(data.sunrise_min, data.sunset_min);
  // Also refresh TODAY if it is the active mode.
  if (ModeManager::get().current() == DisplayMode::TODAY) {
    today_display::show(data);
  }
  gui_lvgl_unlock();
}

void on_forecast_updated(const forecast_data &data) {
  if (s_recovery_active) return;

  forecast_lock();
  s_forecast = data;
  forecast_unlock();

  DisplayMode cur = ModeManager::get().current();
  if (cur != DisplayMode::FORECAST) return;

  gui_lvgl_lock();
  cur = ModeManager::get().current();
  if (cur == DisplayMode::FORECAST) {
    forecast_display::show(data);
  }
  gui_lvgl_unlock();

  ESP_LOGI(TAG, "Forecast rendered");
}

void on_time_changed() {
  if (s_recovery_active) return;
  if (ModeManager::get().current() == DisplayMode::GAME) return;
  gui_lvgl_lock();
  clock::get().update();
  gui_lvgl_unlock();
}

void on_time_loaded() {
  if (s_recovery_active) return;
  if (ModeManager::get().current() == DisplayMode::GAME) return;
  gui_lvgl_lock();
  clock::get().update();
  gui_lvgl_unlock();
}

}

