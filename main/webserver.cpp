//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "webserver.h"

#include "app_boot.h"

#include <cstdlib>
#include <cJSON.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <cstdio>
#include <cstring>
#include <sys/param.h>

#include "controllers/display_controller.h"
#include "drivers/lcds.h"
#include "led_manager.h"
#include "mode_manager.h"
#include "services/microphone_service.h"
#include "services/environment_sensor_service.h"
#include "models/device_config.h"
#include "services/backlight_service.h"
#include "services/config_service.h"
#include "services/status_service.h"
#include "version.h"
#include "weather_parser.h"
#include "weather_service.h"
#include <algorithm>

// Embedded web assets — compiled into the firmware binary so OTA updates them.
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");
extern const char app_css_start[]      asm("_binary_app_css_start");
extern const char app_css_end[]        asm("_binary_app_css_end");
extern const char panels_html_start[] asm("_binary_panels_html_start");
extern const char panels_html_end[]   asm("_binary_panels_html_end");
extern const char mic_tune_html_start[] asm("_binary_mic_tune_html_start");
extern const char mic_tune_html_end[]   asm("_binary_mic_tune_html_end");

static const auto TAG = "webserver";
constexpr size_t MAX_BODY_SIZE = 512;

static status_request_callback_t s_status_callback = nullptr;

// ---- Helpers ----------------------------------------------------------------

static void send_json(httpd_req_t *req, const char *json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_cjson(httpd_req_t *req, cJSON *json) {
  char *body = cJSON_PrintUnformatted(json);
  if (body == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    return ESP_FAIL;
  }
  send_json(req, body);
  cJSON_free(body);
  return ESP_OK;
}

struct sized_event_data {
  uint8_t content[MAX_BODY_SIZE];
  size_t length;
};

static esp_err_t read_body(httpd_req_t *req, sized_event_data &data) {
  size_t recv_size = MIN(req->content_len, sizeof(data.content));
  int ret =
      httpd_req_recv(req, reinterpret_cast<char *>(data.content), recv_size);
  if (ret <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_408(req);
    }
    return ESP_FAIL;
  }
  data.length = static_cast<size_t>(ret);
  return ESP_OK;
}

static bool parse_mode_string(const char *value, DisplayMode *mode) {
  if (value == nullptr || mode == nullptr)
    return false;
  if (strcmp(value, "CLOCK") == 0) {
    *mode = DisplayMode::CLOCK;
    return true;
  }
  if (strcmp(value, "TODAY") == 0) {
    *mode = DisplayMode::TODAY;
    return true;
  }
  if (strcmp(value, "FORECAST") == 0) {
    *mode = DisplayMode::FORECAST;
    return true;
  }
  if (strcmp(value, "GAME") == 0) {
    *mode = DisplayMode::GAME;
    return true;
  }
  if (strcmp(value, "SPECTRUM") == 0) {
    *mode = DisplayMode::SPECTRUM;
    return true;
  }
  return false;
}

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *content_type) {
  FILE *file = fopen(path, "rb");
  if (file == nullptr) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, content_type);

  char buffer[512];
  size_t bytes_read = 0;
  while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
      fclose(file);
      httpd_resp_sendstr_chunk(req, nullptr);
      return ESP_FAIL;
    }
  }
  fclose(file);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

// ---- OTA --------------------------------------------------------------------

enum class OtaState { IDLE, DOWNLOADING, VERIFYING, COMPLETE, FAILED };

struct OtaStatus {
  OtaState state        = OtaState::IDLE;
  int      progress_pct = 0;
  char     error[80]    = {};
};

static OtaStatus        s_ota_status;
static portMUX_TYPE     s_ota_lock      = portMUX_INITIALIZER_UNLOCKED;

static constexpr size_t OTA_BUF_SIZE = 4096;

static const char *ota_state_name(OtaState s) {
  switch (s) {
  case OtaState::IDLE:        return "idle";
  case OtaState::DOWNLOADING: return "downloading";
  case OtaState::VERIFYING:   return "verifying";
  case OtaState::COMPLETE:    return "complete";
  case OtaState::FAILED:      return "failed";
  }
  return "unknown";
}

static void ota_set_status(OtaState state, int pct, const char *error = nullptr) {
  portENTER_CRITICAL(&s_ota_lock);
  s_ota_status.state        = state;
  s_ota_status.progress_pct = pct;
  if (error != nullptr) {
    snprintf(s_ota_status.error, sizeof(s_ota_status.error), "%s", error);
  } else {
    s_ota_status.error[0] = '\0';
  }
  portEXIT_CRITICAL(&s_ota_lock);
}

// ---- OTA state accessors (for /api/status) ----------------------------------

const char *webserver_ota_state() {
  portENTER_CRITICAL(&s_ota_lock);
  OtaState state = s_ota_status.state;
  portEXIT_CRITICAL(&s_ota_lock);
  return ota_state_name(state);
}

int webserver_ota_progress() {
  portENTER_CRITICAL(&s_ota_lock);
  int pct = s_ota_status.progress_pct;
  portEXIT_CRITICAL(&s_ota_lock);
  return pct;
}

// ---- Handlers ---------------------------------------------------------------

static auto index_get_handler(httpd_req_t *req) -> esp_err_t {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  // EMBED_TXTFILES appends a null terminator; exclude it from Content-Length.
  return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static auto app_css_get_handler(httpd_req_t *req) -> esp_err_t {
  httpd_resp_set_type(req, "text/css; charset=utf-8");
  return httpd_resp_send(req, app_css_start, app_css_end - app_css_start - 1);
}

static auto app_js_get_handler(httpd_req_t *req) -> esp_err_t {
  httpd_resp_set_type(req, "application/javascript; charset=utf-8");
  return httpd_resp_send(req, app_js_start, app_js_end - app_js_start - 1);
}

static auto panels_html_get_handler(httpd_req_t *req) -> esp_err_t {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, panels_html_start, panels_html_end - panels_html_start - 1);
}

static auto mic_tune_html_get_handler(httpd_req_t *req) -> esp_err_t {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, mic_tune_html_start, mic_tune_html_end - mic_tune_html_start - 1);
}

static auto logo_get_handler(httpd_req_t *req) -> esp_err_t {
  FILE *f = fopen("/spiffs/nowtube.png", "rb");
  if (!f) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "logo not found");
    return ESP_FAIL;
  }
  // Heap-allocate the read buffer — httpd task stack is only 4096 bytes.
  constexpr size_t CHUNK = 2048;
  char *buf = static_cast<char *>(malloc(CHUNK));
  if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
  httpd_resp_set_type(req, "image/png");
  size_t n;
  esp_err_t err = ESP_OK;
  while ((n = fread(buf, 1, CHUNK, f)) > 0) {
    err = httpd_resp_send_chunk(req, buf, static_cast<ssize_t>(n));
    if (err != ESP_OK) break;
  }
  free(buf);
  fclose(f);
  if (err != ESP_OK) return err;
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static auto ping_get_handler(httpd_req_t *req) -> esp_err_t {
  char buf[1536];
  if (s_status_callback != nullptr) {
    s_status_callback(buf, sizeof(buf));
  } else {
    uint32_t uptime_s =
        static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"firmware\":\"%s\",\"uptime_s\":%" PRIu32 "}",
             NOWTUBE_FIRMWARE_REV, uptime_s);
  }
  send_json(req, buf);
  return ESP_OK;
}

static auto config_get_handler(httpd_req_t *req) -> esp_err_t {
  const device_config &config = config_service::get_config();

  cJSON *root = cJSON_CreateObject();
  cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
  cJSON *weather = cJSON_AddObjectToObject(root, "weather");
  cJSON *display = cJSON_AddObjectToObject(root, "display");

  cJSON_AddStringToObject(root, "timezone", config.timezone);
  cJSON_AddStringToObject(wifi, "ssid", config.wifi_ssid);
  cJSON_AddBoolToObject(wifi, "has_psk", config.wifi_psk[0] != '\0');
  cJSON_AddStringToObject(weather, "units", config.weather_units);
  cJSON_AddNumberToObject(weather, "lat", config.weather_lat);
  cJSON_AddNumberToObject(weather, "lon", config.weather_lon);
  cJSON_AddBoolToObject(weather, "has_location", config.has_weather_location);
  cJSON_AddStringToObject(weather, "city", config.weather_city);
  cJSON_AddNumberToObject(weather, "conditions_refresh_minutes", config.conditions_refresh_minutes);
  cJSON_AddNumberToObject(display, "brightness_pct", config.display_brightness_pct);
  cJSON_AddNumberToObject(display, "clock_font", static_cast<int>(config.clock_font));
  cJSON_AddStringToObject(display, "panel_humidity_metric", config.panel_humidity_metric);
  cJSON *cycle = cJSON_AddObjectToObject(display, "cycle");
  cJSON_AddNumberToObject(cycle, "clock_s",    config.cycle_clock_s);
  cJSON_AddNumberToObject(cycle, "today_s",    config.cycle_today_s);
  cJSON_AddNumberToObject(cycle, "forecast_s", config.cycle_forecast_s);
  cJSON_AddNumberToObject(cycle, "spectrum_s", config.cycle_spectrum_s);
  cJSON *mic = cJSON_AddObjectToObject(display, "microphone");
  cJSON_AddBoolToObject(mic, "enabled", config.mic_enabled);
  cJSON_AddNumberToObject(mic, "adc_channel", config.mic_adc_channel);
  cJSON_AddNumberToObject(mic, "gpio", microphone_service::gpio_num());

  esp_err_t err = send_json_cjson(req, root);
  cJSON_Delete(root);
  return err;
}

static auto config_post_handler(httpd_req_t *req) -> esp_err_t {
  sized_event_data event_data{};
  if (read_body(req, event_data) != ESP_OK)
    return ESP_FAIL;

  cJSON *root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(event_data.content), event_data.length);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }

  device_config next = config_service::get_config();

  cJSON *timezone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
  if (cJSON_IsString(timezone)) {
    snprintf(next.timezone, sizeof(next.timezone), "%s", timezone->valuestring);
  }

  cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
  if (cJSON_IsObject(wifi)) {
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(wifi, "ssid");
    cJSON *psk = cJSON_GetObjectItemCaseSensitive(wifi, "psk");
    if (cJSON_IsString(ssid)) {
      snprintf(next.wifi_ssid, sizeof(next.wifi_ssid), "%s", ssid->valuestring);
    }
    if (cJSON_IsString(psk)) {
      snprintf(next.wifi_psk, sizeof(next.wifi_psk), "%s", psk->valuestring);
    }
  }

  cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
  if (cJSON_IsObject(weather)) {
    cJSON *units   = cJSON_GetObjectItemCaseSensitive(weather, "units");
    cJSON *lat     = cJSON_GetObjectItemCaseSensitive(weather, "lat");
    cJSON *lon     = cJSON_GetObjectItemCaseSensitive(weather, "lon");
    cJSON *city    = cJSON_GetObjectItemCaseSensitive(weather, "city");
    cJSON *refresh = cJSON_GetObjectItemCaseSensitive(weather, "conditions_refresh_minutes");
    bool has_lat = cJSON_IsNumber(lat);
    bool has_lon = cJSON_IsNumber(lon);
    if (cJSON_IsString(units)) {
      snprintf(next.weather_units, sizeof(next.weather_units), "%s", units->valuestring);
    }
    if (has_lat) next.weather_lat = lat->valuedouble;
    if (has_lon) next.weather_lon = lon->valuedouble;
    if (has_lat || has_lon) {
      next.has_weather_location = has_lat && has_lon;
    }
    if (cJSON_IsString(city)) {
      snprintf(next.weather_city, sizeof(next.weather_city), "%s", city->valuestring);
    }
    if (cJSON_IsNumber(refresh)) {
      next.conditions_refresh_minutes = static_cast<uint8_t>(refresh->valueint);
    }
  }

  cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
  if (cJSON_IsObject(display)) {
    cJSON *brightness_pct = cJSON_GetObjectItemCaseSensitive(display, "brightness_pct");
    if (cJSON_IsNumber(brightness_pct)) {
      next.display_brightness_pct = static_cast<uint8_t>(std::clamp(brightness_pct->valueint, 0, 100));
    }
    cJSON *clock_font = cJSON_GetObjectItemCaseSensitive(display, "clock_font");
    if (cJSON_IsNumber(clock_font)) {
      next.clock_font = static_cast<ClockFont>(clock_font->valueint);
    }
    cJSON *panel_hum = cJSON_GetObjectItemCaseSensitive(display, "panel_humidity_metric");
    if (cJSON_IsString(panel_hum)) {
      snprintf(next.panel_humidity_metric, sizeof(next.panel_humidity_metric), "%s", panel_hum->valuestring);
    }
    cJSON *cycle = cJSON_GetObjectItemCaseSensitive(display, "cycle");
    if (cJSON_IsObject(cycle)) {
      cJSON *clock_s    = cJSON_GetObjectItemCaseSensitive(cycle, "clock_s");
      cJSON *today_s    = cJSON_GetObjectItemCaseSensitive(cycle, "today_s");
      cJSON *forecast_s = cJSON_GetObjectItemCaseSensitive(cycle, "forecast_s");
      cJSON *spectrum_s = cJSON_GetObjectItemCaseSensitive(cycle, "spectrum_s");
      if (cJSON_IsNumber(clock_s))    next.cycle_clock_s    = static_cast<uint16_t>(clock_s->valueint);
      if (cJSON_IsNumber(today_s))    next.cycle_today_s    = static_cast<uint16_t>(today_s->valueint);
      if (cJSON_IsNumber(forecast_s)) next.cycle_forecast_s = static_cast<uint16_t>(forecast_s->valueint);
      if (cJSON_IsNumber(spectrum_s)) next.cycle_spectrum_s = static_cast<uint16_t>(spectrum_s->valueint);
    }
    cJSON *mic = cJSON_GetObjectItemCaseSensitive(display, "microphone");
    if (cJSON_IsObject(mic)) {
      cJSON *enabled = cJSON_GetObjectItemCaseSensitive(mic, "enabled");
      cJSON *adc_channel = cJSON_GetObjectItemCaseSensitive(mic, "adc_channel");
      if (cJSON_IsBool(enabled)) next.mic_enabled = cJSON_IsTrue(enabled);
      if (cJSON_IsNumber(adc_channel)) next.mic_adc_channel = static_cast<uint8_t>(adc_channel->valueint);
    }
  }

  cJSON_Delete(root);
  if (!config_service::update(next)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config save failed");
    return ESP_FAIL;
  }

  lcds_set_brightness(next.display_brightness_pct);
  status_service::set_brightness(next.display_brightness_pct);
  setenv("TZ", next.timezone, 1);
  tzset();
  weather_service_reload_config();
  // If AQI was just enabled, fetch immediately so the panel populates
  // without waiting up to conditions_refresh_minutes for the next cycle.
  if (strcmp(config_service::get_config().panel_humidity_metric, "aqi") == 0)
      weather_service_trigger_fetch();

  send_json(req, "{\"status\":\"ok\"}");
  return ESP_OK;
}

static auto mode_get_handler(httpd_req_t *req) -> esp_err_t {
  char buf[32];
  snprintf(buf, sizeof(buf), "{\"mode\":\"%s\"}",
           ModeManager::name(ModeManager::get().current()));
  send_json(req, buf);
  return ESP_OK;
}

static auto mode_post_handler(httpd_req_t *req) -> esp_err_t {
  sized_event_data event_data{};
  if (read_body(req, event_data) != ESP_OK) return ESP_FAIL;

  cJSON *root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(event_data.content), event_data.length);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }

  cJSON *j = cJSON_GetObjectItemCaseSensitive(root, "mode");
  if (!cJSON_IsString(j)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mode");
    return ESP_FAIL;
  }

  const char *s = j->valuestring;
  DisplayMode m = DisplayMode::CLOCK;
  bool found = parse_mode_string(s, &m);

  cJSON_Delete(root);
  if (!found) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown mode");
    return ESP_FAIL;
  }

  display_controller::set_mode(m);
  display_controller::apply_mode(m);
  app_boot_reschedule_current_mode_cycle();
  send_json(req, "{\"status\":\"ok\"}");
  return ESP_OK;
}

static auto brightness_get_handler(httpd_req_t *req) -> esp_err_t {
  char buf[32];
  snprintf(buf, sizeof(buf), "{\"brightness\":%d}", lcds_get_brightness());
  send_json(req, buf);
  return ESP_OK;
}

static auto brightness_post_handler(httpd_req_t *req) -> esp_err_t {
  sized_event_data event_data{};
  if (read_body(req, event_data) != ESP_OK) return ESP_FAIL;

  cJSON *root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(event_data.content), event_data.length);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }

  cJSON *j = cJSON_GetObjectItemCaseSensitive(root, "brightness");
  if (!cJSON_IsNumber(j)) {
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing brightness");
    return ESP_FAIL;
  }

  uint8_t pct = static_cast<uint8_t>(std::clamp(j->valueint, 0, 100));
  cJSON_Delete(root);
  lcds_set_brightness(pct);
  if (!config_service::set_display_brightness(pct)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "brightness save failed");
    return ESP_FAIL;
  }
  status_service::set_brightness(pct);
  send_json(req, "{\"status\":\"ok\"}");
  return ESP_OK;
}

// Upload handler: browser POSTs raw binary; device streams directly to flash.
// Runs synchronously in the httpd worker thread — no separate task needed.
static auto ota_upload_post_handler(httpd_req_t *req) -> esp_err_t {
  portENTER_CRITICAL(&s_ota_lock);
  OtaState snap_state = s_ota_status.state;
  bool busy = (snap_state == OtaState::DOWNLOADING || snap_state == OtaState::VERIFYING);
  if (!busy) {
    s_ota_status.state        = OtaState::DOWNLOADING;
    s_ota_status.progress_pct = 0;
    s_ota_status.error[0]     = '\0';
  }
  portEXIT_CRITICAL(&s_ota_lock);

  if (busy) {
    ESP_LOGW(TAG, "OTA upload: rejected — already %s", ota_state_name(snap_state));
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already in progress");
    return ESP_FAIL;
  }

  int content_length = req->content_len;
  if (content_length <= 0) {
    ota_set_status(OtaState::IDLE, 0);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing Content-Length");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "OTA upload: %d bytes incoming", content_length);

  const esp_partition_t *update_part = esp_ota_get_next_update_partition(nullptr);
  if (!update_part) {
    ota_set_status(OtaState::FAILED, 0, "no OTA partition available");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    return ESP_FAIL;
  }

  esp_ota_handle_t ota_handle;
  esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
  if (err != ESP_OK) {
    char msg[64];
    snprintf(msg, sizeof(msg), "ota_begin: %s", esp_err_to_name(err));
    ota_set_status(OtaState::FAILED, 0, msg);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    return ESP_FAIL;
  }

  // DMA-capable DRAM required for flash writes (PSRAM addresses not reachable by DMA).
  uint8_t *buf = static_cast<uint8_t *>(
      heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!buf) {
    esp_ota_abort(ota_handle);
    ota_set_status(OtaState::FAILED, 0, "out of memory");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  int received = 0;
  bool ok = true;
  char flash_error[80] = {};

  while (received < content_length) {
    int to_read = std::min((int)OTA_BUF_SIZE, content_length - received);
    int n = httpd_req_recv(req, reinterpret_cast<char *>(buf), to_read);
    if (n <= 0) {
      snprintf(flash_error, sizeof(flash_error), "recv error %d", n);
      ok = false;
      break;
    }
    err = esp_ota_write(ota_handle, buf, static_cast<size_t>(n));
    if (err != ESP_OK) {
      snprintf(flash_error, sizeof(flash_error), "ota_write: %s", esp_err_to_name(err));
      ok = false;
      break;
    }
    received += n;
    // Reserve last 5% for the verify step.
    int pct = (received * 95) / content_length;
    portENTER_CRITICAL(&s_ota_lock);
    s_ota_status.progress_pct = pct;
    portEXIT_CRITICAL(&s_ota_lock);
  }

  free(buf);

  if (!ok) {
    esp_ota_abort(ota_handle);
    ota_set_status(OtaState::FAILED, 0, flash_error);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, flash_error);
    return ESP_FAIL;
  }

  ota_set_status(OtaState::VERIFYING, 95);
  ESP_LOGI(TAG, "OTA upload: %d bytes received — verifying", received);

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    char msg[80];
    snprintf(msg, sizeof(msg), "image verify failed: %s", esp_err_to_name(err));
    ota_set_status(OtaState::FAILED, 0, msg);
    ESP_LOGE(TAG, "OTA upload: %s", msg);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update_part);
  if (err != ESP_OK) {
    char msg[80];
    snprintf(msg, sizeof(msg), "set boot partition: %s", esp_err_to_name(err));
    ota_set_status(OtaState::FAILED, 0, msg);
    ESP_LOGE(TAG, "OTA upload: %s", msg);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    return ESP_FAIL;
  }

  ota_set_status(OtaState::COMPLETE, 100);
  ESP_LOGI(TAG, "OTA upload: complete — rebooting");

  send_json(req, "{\"status\":\"ok\"}");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

static auto ota_status_get_handler(httpd_req_t *req) -> esp_err_t {
  OtaStatus snap;
  portENTER_CRITICAL(&s_ota_lock);
  snap = s_ota_status;
  portEXIT_CRITICAL(&s_ota_lock);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "state", ota_state_name(snap.state));
  cJSON_AddNumberToObject(root, "progress_pct", snap.progress_pct);
  if (snap.error[0] != '\0') {
    cJSON_AddStringToObject(root, "error", snap.error);
  }
  esp_err_t err = send_json_cjson(req, root);
  cJSON_Delete(root);
  return err;
}

// POST /api/spiffs/upload?name=filename — writes body to /spiffs/filename.
// Allows pushing individual SPIFFS assets (icons, web UI files) without serial.
// Accepted extensions: .png .html .js .css
static auto spiffs_upload_handler(httpd_req_t *req) -> esp_err_t {
    char query[64] = {};
    char name[33]  = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        httpd_query_key_value(query, "name", name, sizeof(name));

    // Validate: non-empty, ≤32 chars, no path separators, allowed extension.
    size_t nlen = strlen(name);
    auto ends_with = [&](const char *ext) {
        size_t el = strlen(ext);
        return nlen >= el && strcmp(name + nlen - el, ext) == 0;
    };
    if (nlen < 2 || nlen > 32 || strchr(name, '/') || strchr(name, '\\') ||
        (!ends_with(".png") && !ends_with(".html") && !ends_with(".js") && !ends_with(".css"))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid filename (must be *.png/html/js/css, no slashes)");
        return ESP_FAIL;
    }

    int content_length = req->content_len;
    if (content_length <= 0 || content_length > 256 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid content length");
        return ESP_FAIL;
    }

    char path[48];
    snprintf(path, sizeof(path), "/spiffs/%s", name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open file for writing");
        return ESP_FAIL;
    }

    uint8_t buf[512];
    int received = 0;
    bool ok = true;
    while (received < content_length) {
        int to_read = std::min((int)sizeof(buf), content_length - received);
        int n = httpd_req_recv(req, reinterpret_cast<char *>(buf), to_read);
        if (n <= 0) { ok = false; break; }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
        received += n;
    }
    fclose(f);

    if (!ok) {
        remove(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SPIFFS upload: %s — %d bytes", path, received);
    send_json(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}


static const char *condition_icon_name(uint16_t code) {
  if (code == 0 || code == 1) return "sunny";
  if (code == 2) return "partly_cloudy";
  if (code == 3) return "cloudy";
  if (code == 45 || code == 48) return "fog";
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return "snow";
  if (code >= 95) return "thunderstorm";
  return "rain";
}

static void json_add_lines(cJSON *obj, const char *a, const char *b = nullptr, const char *c = nullptr) {
  cJSON *lines = cJSON_AddArrayToObject(obj, "lines");
  if (a) cJSON_AddItemToArray(lines, cJSON_CreateString(a));
  if (b) cJSON_AddItemToArray(lines, cJSON_CreateString(b));
  if (c) cJSON_AddItemToArray(lines, cJSON_CreateString(c));
}

static cJSON *json_panel(int index, const char *kind) {
  cJSON *panel = cJSON_CreateObject();
  cJSON_AddNumberToObject(panel, "index", index);
  cJSON_AddStringToObject(panel, "kind", kind);
  return panel;
}

static void add_text_panel(cJSON *panels, int index, const char *kind, const char *text) {
  cJSON *panel = json_panel(index, kind);
  cJSON_AddStringToObject(panel, "text", text != nullptr ? text : "");
  cJSON_AddItemToArray(panels, panel);
}

static cJSON *add_mode(cJSON *modes, const char *name) {
  cJSON *mode = cJSON_CreateObject();
  cJSON_AddStringToObject(mode, "mode", name);
  cJSON_AddItemToObject(mode, "panels", cJSON_CreateArray());
  cJSON_AddItemToArray(modes, mode);
  return cJSON_GetObjectItem(mode, "panels");
}

static void add_clock_mode(cJSON *modes, const current_conditions &cond) {
  cJSON *panels = add_mode(modes, "CLOCK");
  time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  char buf[16];
  strftime(buf, sizeof(buf), "%I:%M%p", &tm);
  for (int i = 0; i < 5; ++i) {
    char text[2] = {buf[i], '\0'};
    cJSON *panel = json_panel(i, i == 2 ? "separator" : "digit");
    cJSON_AddStringToObject(panel, "text", text);
    if (i == 0 && buf[0] == '0') cJSON_AddBoolToObject(panel, "hidden", true);
    cJSON_AddItemToArray(panels, panel);
  }
  cJSON *panel = json_panel(5, "status");
  char temp[16] = "";
  if (cond.valid) snprintf(temp, sizeof(temp), "%s%s", cond.temp_value, cond.temp_unit);
  json_add_lines(panel, buf[5] == 'A' ? "AM" : "PM", temp[0] ? temp : nullptr);
  cJSON_AddItemToArray(panels, panel);
}

static void add_today_mode(cJSON *modes, const current_conditions &cond) {
  cJSON *panels = add_mode(modes, "TODAY");
  time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  static const char *MONTHS[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
  static const char *WEEKDAYS[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  add_text_panel(panels, 0, "weekday", WEEKDAYS[tm.tm_wday]);
  add_text_panel(panels, 1, "month", MONTHS[tm.tm_mon]);
  char day[8]; snprintf(day, sizeof(day), "%d", tm.tm_mday);
  cJSON *day_panel = json_panel(2, "day_weather");
  cJSON_AddStringToObject(day_panel, "text", day);
  cJSON_AddStringToObject(day_panel, "icon", condition_icon_name(cond.weather_code));
  cJSON_AddItemToArray(panels, day_panel);
  cJSON *wind = json_panel(3, "wind");
  char wind_value[16];
  if (cond.valid) snprintf(wind_value, sizeof(wind_value), "%d", cond.wind_speed); else snprintf(wind_value, sizeof(wind_value), "--");
  json_add_lines(wind, wind_value, cond.valid ? wind_direction_abbr(cond.wind_deg) : nullptr);
  cJSON_AddStringToObject(wind, "icon", "wind");
  cJSON_AddItemToArray(panels, wind);
  cJSON *hum = json_panel(4, "humidity");
  environment_sensor_service::reading env;
  char humidity[16];
  if (environment_sensor_service::get(env)) snprintf(humidity, sizeof(humidity), "%u%%", static_cast<unsigned>(env.humidity_pct + 0.5f));
  else if (cond.valid) snprintf(humidity, sizeof(humidity), "%u%%", cond.humidity);
  else snprintf(humidity, sizeof(humidity), "--");
  cJSON_AddStringToObject(hum, "text", humidity);
  cJSON_AddStringToObject(hum, "source", env.valid ? "indoor" : "weather");
  cJSON_AddStringToObject(hum, "icon", "drop");
  cJSON_AddItemToArray(panels, hum);
  char sun[16] = "--";
  if (cond.valid) {
    uint16_t now_min = static_cast<uint16_t>(tm.tm_hour * 60 + tm.tm_min);
    uint16_t event_min = (cond.sunrise_min > now_min) ? cond.sunrise_min : cond.sunset_min;
    if (event_min == 0 || event_min <= now_min) event_min = cond.sunrise_min;
    snprintf(sun, sizeof(sun), "%u:%02u", event_min / 60, event_min % 60);
  }
  add_text_panel(panels, 5, "sun", sun);
}

static void add_forecast_mode(cJSON *modes, const forecast_data &forecast) {
  cJSON *panels = add_mode(modes, "FORECAST");
  cJSON *key = json_panel(0, "legend");
  json_add_lines(key, "HI", "LO");
  cJSON_AddStringToObject(key, "icon", "forecast");
  cJSON_AddItemToArray(panels, key);
  for (int i = 1; i < static_cast<int>(NUM_LCDS); ++i) {
    cJSON *panel = json_panel(i, "forecast_day");
    int idx = i - 1;
    if (forecast.valid && idx < FORECAST_DAYS && forecast.days[idx].valid) {
      const forecast_day &d = forecast.days[idx];
      char hi[8], lo[8];
      snprintf(hi, sizeof(hi), "%d", d.high);
      snprintf(lo, sizeof(lo), "%d", d.low);
      json_add_lines(panel, forecast_day_code(d.wday), hi, lo);
      cJSON_AddStringToObject(panel, "icon", condition_icon_name(d.weather_code));
    } else {
      json_add_lines(panel, "--", "--");
    }
    cJSON_AddItemToArray(panels, panel);
  }
}

static void add_spectrum_mode(cJSON *modes) {
  cJSON *panels = add_mode(modes, "SPECTRUM");
  float bands[microphone_service::BAND_COUNT];
  microphone_service::get_bands(bands);
  for (int i = 0; i < static_cast<int>(NUM_LCDS); ++i) {
    cJSON *panel = json_panel(i, "spectrum_bands");
    cJSON *bars = cJSON_AddArrayToObject(panel, "bars");
    for (int b = 0; b < 4; ++b) cJSON_AddItemToArray(bars, cJSON_CreateNumber(bands[i * 4 + b]));
    cJSON_AddItemToArray(panels, panel);
  }
}

static void add_game_mode(cJSON *modes) {
  cJSON *panels = add_mode(modes, "GAME");
  add_text_panel(panels, 0, "title", "TUBE");
  add_text_panel(panels, 1, "title", "INV");
  add_text_panel(panels, 2, "state", "live state not exposed");
  add_text_panel(panels, 3, "controls", "LEFT");
  add_text_panel(panels, 4, "controls", "FIRE");
  add_text_panel(panels, 5, "controls", "RIGHT");
}

static auto panels_get_handler(httpd_req_t *req) -> esp_err_t {
  current_conditions cond;
  forecast_data forecast;
  display_controller::get_conditions_snapshot(cond);
  display_controller::get_forecast_snapshot(forecast);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "generated_at", static_cast<double>(time(nullptr)));
  cJSON_AddStringToObject(root, "current_mode", ModeManager::name(ModeManager::get().current()));
  cJSON *modes = cJSON_AddArrayToObject(root, "modes");
  add_clock_mode(modes, cond);
  add_today_mode(modes, cond);
  add_forecast_mode(modes, forecast);
  add_spectrum_mode(modes);
  add_game_mode(modes);
  esp_err_t err = send_json_cjson(req, root);
  cJSON_Delete(root);
  return err;
}

static auto mic_get_handler(httpd_req_t *req) -> esp_err_t {
  float bands[microphone_service::BAND_COUNT];
  microphone_service::get_bands(bands);
  microphone_service::diagnostics diag = microphone_service::get_diagnostics();

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "mode", ModeManager::name(ModeManager::get().current()));
  cJSON_AddBoolToObject(root, "capturing", diag.capturing);
  cJSON_AddNumberToObject(root, "gpio", microphone_service::gpio_num());
  cJSON_AddNumberToObject(root, "raw", diag.raw);
  cJSON_AddNumberToObject(root, "raw_min", diag.raw_min);
  cJSON_AddNumberToObject(root, "raw_max", diag.raw_max);
  cJSON_AddNumberToObject(root, "raw_pp", diag.raw_pp);
  cJSON_AddNumberToObject(root, "total_power", diag.total_power);
  cJSON_AddNumberToObject(root, "frame_peak", diag.frame_peak);
  cJSON_AddNumberToObject(root, "level_ref", diag.level_ref);
  cJSON_AddNumberToObject(root, "noise_cal_frames", diag.noise_cal_frames);
  cJSON_AddNumberToObject(root, "updated_us", static_cast<double>(diag.updated_us));
  microphone_service::landscape_diagnostics ldiag = microphone_service::get_landscape_diagnostics();
  cJSON *land = cJSON_AddObjectToObject(root, "landscape");
  cJSON_AddNumberToObject(land, "sample_rate_hz", ldiag.sample_rate_hz);
  cJSON_AddNumberToObject(land, "fft_size", ldiag.fft_size);
  cJSON_AddNumberToObject(land, "rms", ldiag.rms);
  cJSON_AddNumberToObject(land, "signal_peak", ldiag.signal_peak);
  cJSON_AddNumberToObject(land, "visual_ref", ldiag.visual_ref);
  cJSON_AddNumberToObject(land, "dominant_freq_hz", ldiag.dominant_freq_hz);
  cJSON_AddNumberToObject(land, "dominant_value", ldiag.dominant_value);
  cJSON_AddNumberToObject(land, "fft_fps", ldiag.fft_fps);
  cJSON_AddNumberToObject(land, "frames", static_cast<double>(ldiag.frames));
  cJSON_AddNumberToObject(land, "read_timeouts", static_cast<double>(ldiag.read_timeouts));
  cJSON *arr = cJSON_AddArrayToObject(root, "bands");
  for (float band : bands) cJSON_AddItemToArray(arr, cJSON_CreateNumber(band));

  microphone_service::band_debug debug[microphone_service::BAND_COUNT];
  microphone_service::get_band_debug(debug);
  cJSON *dbg = cJSON_AddArrayToObject(root, "band_debug");
  for (int i = 0; i < microphone_service::BAND_COUNT; ++i) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "i", i);
    cJSON_AddNumberToObject(item, "raw", debug[i].raw);
    cJSON_AddNumberToObject(item, "floor", debug[i].floor);
    cJSON_AddNumberToObject(item, "power", debug[i].power);
    cJSON_AddNumberToObject(item, "display", debug[i].display);
    cJSON_AddItemToArray(dbg, item);
  }
  esp_err_t err = send_json_cjson(req, root);
  cJSON_Delete(root);
  return err;
}


static auto mic_frame_get_handler(httpd_req_t *req) -> esp_err_t {
  uint16_t raw[microphone_service::RAW_FRAME_SAMPLES];
  float dec[microphone_service::FRAME_SAMPLES];
  bool valid = microphone_service::get_latest_frame(raw, dec);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "valid", valid);
  cJSON_AddNumberToObject(root, "rate_hz", 32000);
  cJSON_AddNumberToObject(root, "dec_rate_hz", 32000);
  cJSON_AddNumberToObject(root, "n", microphone_service::RAW_FRAME_SAMPLES);
  cJSON_AddNumberToObject(root, "dec_n", microphone_service::FRAME_SAMPLES);
  if (valid) {
    cJSON *samples = cJSON_AddArrayToObject(root, "samples");
    for (int i = 0; i < microphone_service::RAW_FRAME_SAMPLES; ++i) {
      cJSON_AddItemToArray(samples, cJSON_CreateNumber(raw[i]));
    }
    cJSON *decimated = cJSON_AddArrayToObject(root, "dec");
    for (int i = 0; i < microphone_service::FRAME_SAMPLES; ++i) {
      cJSON_AddItemToArray(decimated, cJSON_CreateNumber(dec[i]));
    }
  } else {
    cJSON_AddStringToObject(root, "error", "no captured frame yet; set SPECTRUM mode first");
  }
  esp_err_t err = send_json_cjson(req, root);
  cJSON_Delete(root);
  return err;
}


static auto mic_landscape_get_handler(httpd_req_t *req) -> esp_err_t {
  static float live[microphone_service::LANDSCAPE_WIDTH];
  static float peak[microphone_service::LANDSCAPE_WIDTH];
  microphone_service::get_landscape(live, peak);
  microphone_service::diagnostics diag = microphone_service::get_diagnostics();
  microphone_service::landscape_diagnostics ldiag = microphone_service::get_landscape_diagnostics();

  float max_live = 0.0f;
  int active_cols = 0;
  int max_col = 0;
  for (int i = 0; i < microphone_service::LANDSCAPE_WIDTH; ++i) {
    if (live[i] > 0.03f) ++active_cols;
    if (live[i] > max_live) {
      max_live = live[i];
      max_col = i;
    }
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "name", "Harmonic Landscape");
  cJSON_AddStringToObject(root, "description",
                          "The six tubes form a live map of the sound spectrum. Bass is on the left and treble is on the right. Musical notes create related harmonics, so the shapes show some of the spectral structure that gives music and instruments their character.");
  cJSON_AddStringToObject(root, "mode", ModeManager::name(ModeManager::get().current()));
  cJSON_AddBoolToObject(root, "capturing", diag.capturing);
  cJSON_AddNumberToObject(root, "width", microphone_service::LANDSCAPE_WIDTH);
  cJSON_AddNumberToObject(root, "sample_rate_hz", ldiag.sample_rate_hz);
  cJSON_AddNumberToObject(root, "fft_size", ldiag.fft_size);
  cJSON_AddNumberToObject(root, "rms", ldiag.rms);
  cJSON_AddNumberToObject(root, "raw_pp", diag.raw_pp);
  cJSON_AddNumberToObject(root, "signal_peak", ldiag.signal_peak);
  cJSON_AddNumberToObject(root, "visual_ref", ldiag.visual_ref);
  cJSON_AddNumberToObject(root, "dominant_freq_hz", ldiag.dominant_freq_hz);
  cJSON_AddNumberToObject(root, "fft_fps", ldiag.fft_fps);
  cJSON_AddNumberToObject(root, "max_live", max_live);
  cJSON_AddNumberToObject(root, "max_col", max_col);
  cJSON_AddNumberToObject(root, "active_cols", active_cols);
  esp_err_t err = send_json_cjson(req, root);
  cJSON_Delete(root);
  return err;
}

static auto reboot_post_handler(httpd_req_t *req) -> esp_err_t {
  send_json(req, "{\"status\":\"ok\"}");
  vTaskDelay(pdMS_TO_TICKS(200));  // let response flush before reboot
  esp_restart();
  return ESP_OK;
}

static auto weather_refresh_post_handler(httpd_req_t *req) -> esp_err_t {
  weather_service_trigger_fetch();
  send_json(req, "{\"status\":\"ok\"}");
  return ESP_OK;
}

static constexpr const char *const BACKLIGHT_COLOR_NAMES[] = {
    "warm orange", "red", "green", "blue", "cyan", "magenta", "amber", "off",
};
static constexpr size_t BACKLIGHT_COLOR_COUNT =
    sizeof(BACKLIGHT_COLOR_NAMES) / sizeof(BACKLIGHT_COLOR_NAMES[0]);

static auto backlight_get_handler(httpd_req_t *req) -> esp_err_t {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "mode", backlight_service::get_mode_name());
  cJSON_AddStringToObject(root, "led_color", backlight_service::get_led_color_name());

  cJSON *modes = cJSON_AddArrayToObject(root, "modes");
  for (const char *m : {"normal", "breathable", "mixed"}) {
    cJSON_AddItemToArray(modes, cJSON_CreateString(m));
  }
  cJSON *colors = cJSON_AddArrayToObject(root, "led_colors");
  for (size_t i = 0; i < BACKLIGHT_COLOR_COUNT; i++) {
    cJSON_AddItemToArray(colors, cJSON_CreateString(BACKLIGHT_COLOR_NAMES[i]));
  }

  esp_err_t err = send_json_cjson(req, root);
  cJSON_Delete(root);
  return err;
}

static auto backlight_post_handler(httpd_req_t *req) -> esp_err_t {
  sized_event_data event_data{};
  if (read_body(req, event_data) != ESP_OK) return ESP_FAIL;

  cJSON *root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(event_data.content), event_data.length);
  if (!root) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    return ESP_FAIL;
  }

  cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
  if (cJSON_IsString(mode)) {
    if (!backlight_service::set_mode(mode->valuestring)) {
      cJSON_Delete(root);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown mode");
      return ESP_FAIL;
    }
  }

  cJSON *color = cJSON_GetObjectItemCaseSensitive(root, "led_color");
  if (cJSON_IsString(color)) {
    if (!backlight_service::set_led_color(color->valuestring)) {
      cJSON_Delete(root);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown led_color");
      return ESP_FAIL;
    }
  }

  cJSON_Delete(root);
  send_json(req, "{\"status\":\"ok\"}");
  return ESP_OK;
}

// ---- URI registrations ------------------------------------------------------

static const httpd_uri_t uri_health = {
    .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_app_css = {
    .uri = "/app.css", .method = HTTP_GET, .handler = app_css_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_app_js = {
    .uri = "/app.js", .method = HTTP_GET, .handler = app_js_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_logo = {
    .uri = "/logo.png", .method = HTTP_GET, .handler = logo_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_panels_html = {
    .uri = "/panels", .method = HTTP_GET, .handler = panels_html_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_mic_tune_html = {
    .uri = "/mic-tune", .method = HTTP_GET, .handler = mic_tune_html_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_status = {
    .uri = "/api/status", .method = HTTP_GET, .handler = ping_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_panels_api = {
    .uri = "/api/panels", .method = HTTP_GET, .handler = panels_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_mic_api = {
    .uri = "/api/mic", .method = HTTP_GET, .handler = mic_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_mic_frame_api = {
    .uri = "/api/mic/frame", .method = HTTP_GET, .handler = mic_frame_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_mic_landscape_api = {
    .uri = "/api/mic/landscape", .method = HTTP_GET, .handler = mic_landscape_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_config_get = {
    .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_config_post = {
    .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_mode_get = {
    .uri = "/api/mode", .method = HTTP_GET, .handler = mode_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_mode_post = {
    .uri = "/api/mode", .method = HTTP_POST, .handler = mode_post_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_brightness_get = {
    .uri = "/api/brightness", .method = HTTP_GET, .handler = brightness_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_brightness_post = {
    .uri = "/api/brightness", .method = HTTP_POST, .handler = brightness_post_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_ota_upload = {
    .uri = "/api/ota/upload", .method = HTTP_POST, .handler = ota_upload_post_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_ota_status = {
    .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_spiffs_upload = {
    .uri = "/api/spiffs/upload", .method = HTTP_POST, .handler = spiffs_upload_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_reboot = {
    .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_weather_refresh = {
    .uri = "/api/weather/refresh", .method = HTTP_POST, .handler = weather_refresh_post_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_backlight_get = {
    .uri = "/api/backlight", .method = HTTP_GET, .handler = backlight_get_handler, .user_ctx = nullptr};

static const httpd_uri_t uri_backlight_post = {
    .uri = "/api/backlight", .method = HTTP_POST, .handler = backlight_post_handler, .user_ctx = nullptr};

// ---- Init -------------------------------------------------------------------

void webserver_init(status_request_callback_t status_callback) {
  s_status_callback = status_callback;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.max_uri_handlers  = 24;
  config.max_open_sockets  = 8;
  config.lru_purge_enable = true;

  httpd_handle_t server = nullptr;
  ESP_ERROR_CHECK(httpd_start(&server, &config));

  httpd_register_uri_handler(server, &uri_health);
  httpd_register_uri_handler(server, &uri_app_css);
  httpd_register_uri_handler(server, &uri_app_js);
  httpd_register_uri_handler(server, &uri_logo);
  httpd_register_uri_handler(server, &uri_panels_html);
  httpd_register_uri_handler(server, &uri_mic_tune_html);
  httpd_register_uri_handler(server, &uri_status);
  httpd_register_uri_handler(server, &uri_panels_api);
  httpd_register_uri_handler(server, &uri_mic_api);
  httpd_register_uri_handler(server, &uri_mic_frame_api);
  httpd_register_uri_handler(server, &uri_mic_landscape_api);
  httpd_register_uri_handler(server, &uri_config_get);
  httpd_register_uri_handler(server, &uri_config_post);
  httpd_register_uri_handler(server, &uri_mode_get);
  httpd_register_uri_handler(server, &uri_mode_post);
  httpd_register_uri_handler(server, &uri_brightness_get);
  httpd_register_uri_handler(server, &uri_brightness_post);
  httpd_register_uri_handler(server, &uri_ota_upload);
  httpd_register_uri_handler(server, &uri_ota_status);
  httpd_register_uri_handler(server, &uri_reboot);
  httpd_register_uri_handler(server, &uri_weather_refresh);
  httpd_register_uri_handler(server, &uri_backlight_get);
  httpd_register_uri_handler(server, &uri_backlight_post);
  httpd_register_uri_handler(server, &uri_spiffs_upload);

  ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
