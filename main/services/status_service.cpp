//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "services/status_service.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "services/diagnostics_service.h"

namespace status_service {

static constexpr const char *TAG = "status_service";
static status_snapshot s_snapshot{};
static char s_wifi_ip[16] = "";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void init(const char *firmware_version) {
  s_snapshot.firmware_version = firmware_version;
  s_snapshot.mode = "CLOCK";
  s_snapshot.brightness_pct = 0;
  s_snapshot.wifi.connected = false;
  s_snapshot.wifi.ip = s_wifi_ip;
  s_snapshot.weather_available = false;
  s_snapshot.last_weather_sync = 0;
  s_snapshot.diagnostics = diagnostics_service::get_snapshot();
  ESP_LOGI(TAG, "Status service initialized");
}

void set_brightness(uint8_t brightness_pct) {
  portENTER_CRITICAL(&s_lock);
  s_snapshot.brightness_pct = brightness_pct;
  portEXIT_CRITICAL(&s_lock);
}

void set_mode(const char *mode_name) {
  portENTER_CRITICAL(&s_lock);
  s_snapshot.mode = mode_name;
  portEXIT_CRITICAL(&s_lock);
}

void set_wifi(bool connected, const char *ip_address) {
  portENTER_CRITICAL(&s_lock);
  s_snapshot.wifi.connected = connected;
  snprintf(s_wifi_ip, sizeof(s_wifi_ip), "%s",
           ip_address != nullptr ? ip_address : "");
  s_snapshot.wifi.ip = s_wifi_ip;
  portEXIT_CRITICAL(&s_lock);
}

void set_wifi_retry_count(uint32_t count) {
  portENTER_CRITICAL(&s_lock);
  s_snapshot.wifi.retry_count = count;
  portEXIT_CRITICAL(&s_lock);
}

void set_weather_sync(time_t last_weather_sync) {
  portENTER_CRITICAL(&s_lock);
  s_snapshot.weather_available = true;
  s_snapshot.last_weather_sync = last_weather_sync;
  portEXIT_CRITICAL(&s_lock);
}

status_snapshot get_snapshot() {
  portENTER_CRITICAL(&s_lock);
  s_snapshot.uptime_s =
      static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
  status_snapshot copy = s_snapshot;
  portEXIT_CRITICAL(&s_lock);
  copy.diagnostics = diagnostics_service::get_snapshot();
  return copy;
}

}
