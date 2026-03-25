// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT

#include "app_boot.h"

#include <cstring>
#include <ctime>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_psram.h>
#include <esp_spiffs.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "controllers/display_controller.h"
#include "mode_policy.h"
#include "controllers/input_controller.h"
#include "displays/clock/clock.h"
#include "drivers/lcds.h"
#include "drivers/leds.h"
#include "drivers/touchpads.h"
#include "drivers/wifi.h"
#include "gui.h"
#include "models/device_config.h"
#include "rtc.h"
#include "services/backlight_service.h"
#include "services/sound_manager.h"
#include "services/config_service.h"
#include "services/diagnostics_service.h"
#include "services/status_service.h"
#include "version.h"
#include "weather_service.h"
#include "webserver.h"

// ---------------------------------------------------------------------------

#define SPIFFS_MOUNTPOINT_NO_SLASH "/spiffs"
#define SPIFFS_MOUNTPOINT SPIFFS_MOUNTPOINT_NO_SLASH "/"

static const auto TAG = "nowtube";

static const auto SNTP_SERVER = "pool.ntp.org";

ESP_EVENT_DECLARE_BASE(DISPATCH_EVENTS);
enum {
  DISPATCH_EVENT_TIME_CHANGED,
  DISPATCH_EVENT_RTC_TIME_LOADED,
};

static esp_timer_handle_t s_cycle_timer = nullptr;

// ---------------------------------------------------------------------------

static void schedule_cycle(uint64_t delay_us) {
  esp_timer_stop(s_cycle_timer);  // safe if not running (returns ERR_INVALID_STATE)
  ESP_ERROR_CHECK(esp_timer_start_once(s_cycle_timer, delay_us));
}

static void apply_mode_display(DisplayMode m) {
  display_controller::apply_mode(m);
}

// Called from the weather_service FreeRTOS fetch task — must not block on gui_lvgl_lock.
static void on_forecast_fetched(const forecast_data &data) {
  display_controller::on_forecast_updated(data);
}

static void on_conditions_fetched(const current_conditions &data) {
  status_service::set_weather_sync(time(nullptr));
  ESP_LOGI(TAG, "Conditions fetched: %s°%s wind=%d@%u° humid=%u%%",
           data.temp_value, data.temp_unit,
           data.wind_speed, data.wind_deg, data.humidity);
  display_controller::on_conditions_updated(data);
}

// ---------------------------------------------------------------------------
// Auto-cycle

static cycle_config make_cycle_config() {
  const device_config &c = config_service::get_config();
  return cycle_config{c.cycle_clock_s, c.cycle_today_s, c.cycle_forecast_s};
}

static void auto_cycle_cb([[maybe_unused]] void *arg) {
  DisplayMode cur = display_controller::current_mode();
  mode_transition t = mode_policy_next(cur, make_cycle_config());
  schedule_cycle(t.delay_us);

  // Apply the mode transition under the LVGL lock.
  // gui_lvgl_lock() is a recursive mutex — safe to take from any task context.
  // Using lv_async_call() here would race: lv_async.c writes info->cb/user_data
  // *after* inserting the timer into the list, so lvgl_task can fire it before
  // the struct is initialised, producing a garbage user_data pointer and crash.
  gui_lvgl_lock();
  display_controller::set_mode(t.next);
  apply_mode_display(t.next);
  gui_lvgl_unlock();
}

static void schedule_clock_cycle() {
  if (s_cycle_timer != nullptr) {
    schedule_cycle(make_cycle_config().clock_s * 1'000'000ULL);
  }
}

// ---------------------------------------------------------------------------
// Subsystem init helpers

static void nvs_init() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

static void spiffs_init() {
  esp_vfs_spiffs_conf_t config = {
      .base_path              = SPIFFS_MOUNTPOINT_NO_SLASH,
      .partition_label        = nullptr,
      .max_files              = 10,
      .format_if_mount_failed = true,
  };
  esp_err_t err = esp_vfs_spiffs_register(&config);
  if (err != ESP_OK) {
    // Non-fatal: clock still works without SPIFFS assets; log and continue.
    ESP_LOGE(TAG, "SPIFFS mount failed (%s) — display assets unavailable",
             esp_err_to_name(err));
  }
}

static bool s_sntp_initialized = false;

static void sntp_init() {
  if (s_sntp_initialized) {
    // Already initialized at first connect.  The internal IP_EVENT_STA_GOT_IP
    // handler registered by esp_netif_sntp_init() will renew the sync
    // automatically on reconnect — no second init needed.
    ESP_LOGI(TAG, "SNTP already running — skipping re-init");
    return;
  }
  esp_sntp_config_t config = {
      .smooth_sync = false,
      .server_from_dhcp = false,
      .wait_for_sync = true,
      .start = true,
      .sync_cb =
          [](struct timeval *tv) IRAM_ATTR {
            ESP_ERROR_CHECK(
                esp_event_post(DISPATCH_EVENTS, DISPATCH_EVENT_TIME_CHANGED, tv,
                               sizeof(struct timeval), portMAX_DELAY));
          },
      .renew_servers_after_new_IP = true,
      .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
      .index_of_first_server = 0,
      .num_of_servers = 1,
      .servers = {SNTP_SERVER},
  };

  ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
  s_sntp_initialized = true;
}

// ---------------------------------------------------------------------------
// Runtime event handlers

static void ntp_changed_time(struct timeval *tv) {
  ESP_LOGI(TAG, "Time changed: %lld", tv->tv_sec);
  rtc_persist();
  display_controller::on_time_changed();
}

static void rtc_loaded_time(struct timeval *tv) {
  ESP_LOGI(TAG, "Time loaded: %lld", tv->tv_sec);
  display_controller::on_time_loaded();
}

static void on_wifi_connected() {
  ESP_LOGI(TAG, "Connected to wifi");
  sntp_init();
  weather_service_trigger_fetch();
}

static void on_wifi_recovery_requested() {
  ESP_LOGW(TAG, "Wi-Fi recovery requested — starting nowtube-setup AP");
  wifi_start_recovery_ap();
  display_controller::show_recovery_screen();
  backlight_service::set_recovery_cue();
}

static void on_wifi_recovery_cancelled() {
  ESP_LOGI(TAG, "Wi-Fi recovery cancelled — returning to normal mode");
  wifi_cancel_recovery_ap();
  display_controller::cancel_recovery_screen();
  backlight_service::cancel_recovery_cue();
  const device_config &config = config_service::get_config();
  if (config.wifi_ssid[0] != '\0') {
    wifi_connect(config.wifi_ssid, config.wifi_psk);
  }
}

static void on_wifi_recovery_toggle() {
  if (display_controller::is_recovery_active()) {
    on_wifi_recovery_cancelled();
  } else {
    on_wifi_recovery_requested();
  }
}

static void status_handler(char *buffer, size_t buffer_size) {
  status_snapshot snap = status_service::get_snapshot();
  snprintf(buffer, buffer_size,
           "{\"status\":\"ok\",\"firmware\":\"%s\",\"uptime_s\":%lu,"
           "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"retry_count\":%lu},"
           "\"display\":{\"mode\":\"%s\",\"brightness_pct\":%u},"
           "\"weather\":{\"available\":%s,\"last_success_unix\":%lld,"
           "\"fetch_ok\":%d,\"fetch_fail\":%d,\"last_error\":\"%s\"},"
           "\"ota\":{\"state\":\"%s\",\"progress_pct\":%d},"
           "\"diagnostics\":{\"last_reset_reason\":\"%s\","
           "\"boot_count\":%lu,\"free_heap\":%lu,\"min_free_heap\":%lu}}",
           snap.firmware_version != nullptr ? snap.firmware_version : NOWTUBE_FIRMWARE_REV,
           static_cast<unsigned long>(snap.uptime_s),
           snap.wifi.connected ? "true" : "false",
           snap.wifi.ip != nullptr ? snap.wifi.ip : "",
           static_cast<unsigned long>(snap.wifi.retry_count),
           snap.mode != nullptr ? snap.mode : "CLOCK",
           static_cast<unsigned>(snap.brightness_pct),
           snap.weather_available ? "true" : "false",
           static_cast<long long>(snap.last_weather_sync),
           weather_service_success_count(),
           weather_service_fail_count(),
           weather_service_last_error(),
           webserver_ota_state(),
           webserver_ota_progress(),
           snap.diagnostics.last_reset_reason != nullptr
               ? snap.diagnostics.last_reset_reason : "unknown",
           static_cast<unsigned long>(snap.diagnostics.boot_count),
           static_cast<unsigned long>(snap.diagnostics.free_heap),
           static_cast<unsigned long>(snap.diagnostics.min_free_heap));
}

static void dispatch_event_handler([[maybe_unused]] void *handler_args,
                                   [[maybe_unused]] esp_event_base_t base,
                                   int32_t id, void *event_data) {
  switch (id) {
  case DISPATCH_EVENT_TIME_CHANGED:
    ntp_changed_time(static_cast<struct timeval *>(event_data));
    break;
  case DISPATCH_EVENT_RTC_TIME_LOADED:
    rtc_loaded_time(static_cast<struct timeval *>(event_data));
    break;
  }
}

// ---------------------------------------------------------------------------

ESP_EVENT_DEFINE_BASE(DISPATCH_EVENTS);

void app_boot_run() {
  size_t psram_size = esp_psram_get_size();
  ESP_LOGI(TAG, "nowtube firmware rev %s", NOWTUBE_FIRMWARE_REV);
  ESP_LOGI(TAG, "Starting... PSRAM size: %d bytes", psram_size);

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_log_level_set("gpio", ESP_LOG_WARN);
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      DISPATCH_EVENTS, ESP_EVENT_ANY_ID, dispatch_event_handler, nullptr,
      nullptr));

  ESP_LOGI(TAG, "boot: storage init");
  spiffs_init();
  nvs_init();

  ESP_LOGI(TAG, "boot: core services");
  diagnostics_service::init();
  config_service::init();
  status_service::init(NOWTUBE_FIRMWARE_REV);

  ESP_LOGI(TAG, "boot: weather service init");
  weather_service_init(on_conditions_fetched, on_forecast_fetched);

  ESP_LOGI(TAG, "boot: drivers");
  leds_init();
  leds_off();
  leds_off();  // second zero frame so strip latches cleanly (no random pixels)
  lcds_init();
  const device_config &config = config_service::get_config();
  // Apply the user's saved brightness directly.  0% is a valid user choice;
  // if the device boots into recovery mode, set_recovery_cue() will
  // temporarily override to 40% without touching NVS.
  uint8_t boot_brightness = config.display_brightness_pct;
  lcds_set_brightness(boot_brightness);
  backlight_service::init(boot_brightness);
  sound_manager::init();
  bool got_time = rtc_init();

  ESP_LOGI(TAG, "boot: wifi init");
  wifi_init(on_wifi_connected);
  ESP_LOGI(TAG, "boot: gui_init");
  gui_init();

  ESP_LOGI(TAG, "boot: touchpads_init");
  input_controller::init(false, apply_mode_display,
                         schedule_clock_cycle, on_wifi_recovery_toggle);
  touchpads_init(input_controller::on_button_tapped,
                 input_controller::on_button_touched);

  setenv("TZ", config.timezone, 1);
  tzset();
  ESP_LOGI(TAG, "boot: timezone=%s", config.timezone);

  ESP_LOGI(TAG, "boot: display_controller init");
  display_controller::init();

  clock::get();
  ESP_LOGI(TAG, "boot: gui_start");
  gui_start();
  display_controller::render_boot_screen();

  ESP_LOGI(TAG, "boot: webserver_init");
  webserver_init(status_handler);

  {
    esp_timer_create_args_t args = {
        .callback = auto_cycle_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "auto_cycle",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_cycle_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(s_cycle_timer, MODE_CYCLE_CLOCK_US));
    ESP_LOGI(TAG, "boot: auto-cycle started (first transition in %llus)",
             MODE_CYCLE_CLOCK_US / 1'000'000ULL);
  }

  if (config.wifi_ssid[0] != '\0') {
    ESP_LOGI(TAG, "Starting WiFi connection from config...");
    wifi_connect(config.wifi_ssid, config.wifi_psk);
  } else {
    // No SSID configured: start recovery AP immediately so the user can
    // reach the config UI without a USB cable.
    ESP_LOGW(TAG, "No Wi-Fi SSID configured — starting recovery AP (nowtube-setup / 192.168.4.1)");
    wifi_start_recovery_ap();
    display_controller::show_recovery_screen();
    backlight_service::set_recovery_cue();
  }

  if (got_time) {
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    ESP_ERROR_CHECK(esp_event_post(DISPATCH_EVENTS, DISPATCH_EVENT_RTC_TIME_LOADED,
                                   &tv, sizeof(struct timeval), portMAX_DELAY));
  }

  // Ensure LEDs are dark after boot (clears any residual pixel state).
  backlight_service::finalize_boot_state();

  ESP_LOGI(TAG, "boot: complete (free heap=%lu)", esp_get_free_heap_size());
}
