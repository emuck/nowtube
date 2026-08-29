// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "weather_service.h"
#include "weather_parser.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "services/config_service.h"

#include <cJSON.h>

static const char *TAG = "weather";

static constexpr size_t RESPONSE_BUF_SIZE = 24576;
static constexpr size_t URL_BUF_SIZE      = 384;

static constexpr uint8_t  REFRESH_MIN_MINUTES = 5;
static constexpr uint8_t  REFRESH_MAX_MINUTES = 30;
static constexpr int64_t  FETCH_TIMEOUT_US    = 60'000'000LL;

// Config snapshot — protected by s_config_mutex.
static bool   s_fetch_aqi        = false;
static char   s_temp_unit[16]    = "fahrenheit";
static char   s_wind_unit[8]     = "mph";
static char   s_unit_label[8]    = "mph";
static char   s_temp_unit_chr[4] = "F";
static double s_lat              = 0.0;
static double s_lon              = 0.0;

static conditions_callback_t s_on_conditions    = nullptr;
static forecast_callback_t   s_on_forecast      = nullptr;
static esp_timer_handle_t    s_timer            = nullptr;
static bool                  s_configured       = false;
static bool                  s_fetch_in_progress = false;
static int64_t               s_fetch_started_us  = 0;
static portMUX_TYPE          s_fetch_lock        = portMUX_INITIALIZER_UNLOCKED;
static char                  s_last_error[80]    = "none";
static int                   s_fail_count        = 0;
static int                   s_success_count     = 0;
static SemaphoreHandle_t     s_config_mutex      = nullptr;


static bool is_nws_supported_location(double lat, double lon) {
    // NWS covers the United States and territories. Keep this broad enough for
    // Alaska/Hawaii while excluding non-US Open-Meteo locations.
    return lat >= 18.0 && lat <= 72.0 && lon >= -180.0 && lon <= -65.0;
}

static bool http_get_to_buffer(const char *url, char *buf, size_t buf_size,
                               const char *user_agent,
                               int *status_out, int *bytes_out) {
    if (!url || !buf || buf_size == 0) return false;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    if (user_agent && user_agent[0] != '\0') {
        esp_http_client_set_header(client, "User-Agent", user_agent);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_fetch_headers(client);

    int total = 0;
    int n = 0;
    while (total < static_cast<int>(buf_size) - 1 &&
           (n = esp_http_client_read(client, buf + total, buf_size - 1 - total)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_out) *status_out = status;
    if (bytes_out) *bytes_out = total;
    return status == 200 && total > 0;
}

static bool try_nws_forecast(double lat, double lon, const char *temp_unit_chr,
                             forecast_data &forecast) {
    if (!is_nws_supported_location(lat, lon)) return false;

    char points_url[URL_BUF_SIZE];
    if (!nws_points_build_url(points_url, sizeof(points_url), lat, lon)) return false;

    char *buf = static_cast<char *>(malloc(RESPONSE_BUF_SIZE));
    if (!buf) return false;

    int status = 0;
    int bytes = 0;
    constexpr const char *USER_AGENT = "nowtube/0.6 local weather display";
    if (!http_get_to_buffer(points_url, buf, RESPONSE_BUF_SIZE, USER_AGENT, &status, &bytes)) {
        ESP_LOGW(TAG, "NWS points fetch failed: HTTP %d, %d bytes", status, bytes);
        free(buf);
        return false;
    }

    char forecast_url[URL_BUF_SIZE];
    if (!nws_parse_points_response(buf, static_cast<size_t>(bytes), forecast_url, sizeof(forecast_url))) {
        ESP_LOGW(TAG, "NWS points response missing forecast URL");
        free(buf);
        return false;
    }

    if (!http_get_to_buffer(forecast_url, buf, RESPONSE_BUF_SIZE, USER_AGENT, &status, &bytes)) {
        ESP_LOGW(TAG, "NWS forecast fetch failed: HTTP %d, %d bytes", status, bytes);
        free(buf);
        return false;
    }

    forecast_data nws_forecast;
    WeatherParseStatus parsed = nws_parse_forecast_response(buf, static_cast<size_t>(bytes),
                                                            temp_unit_chr, nws_forecast);
    free(buf);
    if (parsed != WeatherParseStatus::OK) {
        ESP_LOGW(TAG, "NWS forecast parse failed: %d", static_cast<int>(parsed));
        return false;
    }

    forecast = nws_forecast;
    ESP_LOGI(TAG, "NWS forecast override applied: [0] hi=%d lo=%d", forecast.days[0].high, forecast.days[0].low);
    return true;
}

// ---------------------------------------------------------------------------
// Config

static bool load_config_from_service() {
    const device_config &cfg = config_service::get_config();

    if (s_config_mutex) xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    s_lat = cfg.weather_lat;
    s_lon = cfg.weather_lon;
    s_fetch_aqi = (strcmp(cfg.panel_humidity_metric, "aqi") == 0);
    bool imperial = (strcmp(cfg.weather_units, "imperial") == 0);
    snprintf(s_temp_unit,    sizeof(s_temp_unit),    "%s", imperial ? "fahrenheit" : "celsius");
    snprintf(s_wind_unit,    sizeof(s_wind_unit),    "%s", imperial ? "mph" : "kmh");
    snprintf(s_unit_label,   sizeof(s_unit_label),   "%s", imperial ? "mph" : "km/h");
    snprintf(s_temp_unit_chr, sizeof(s_temp_unit_chr), "%s", imperial ? "F" : "C");
    s_configured = cfg.has_weather_location;
    if (s_config_mutex) xSemaphoreGive(s_config_mutex);

    if (s_configured) {
        ESP_LOGI(TAG, "Weather enabled: lat=%.4f lon=%.4f temp=%s wind=%s",
                 s_lat, s_lon, s_temp_unit, s_wind_unit);
    } else {
        ESP_LOGI(TAG, "Weather disabled (no lat/lon or weather disabled)");
    }
    return s_configured;
}

static uint64_t fetch_interval_us() {
    uint8_t minutes = config_service::get_config().conditions_refresh_minutes;
    if (minutes < REFRESH_MIN_MINUTES || minutes > REFRESH_MAX_MINUTES) minutes = 10;
    return (uint64_t)minutes * 60ULL * 1'000'000ULL;
}

// ---------------------------------------------------------------------------
// Fetch task

static void finish_fetch_task() {
    portENTER_CRITICAL(&s_fetch_lock);
    s_fetch_in_progress = false;
    portEXIT_CRITICAL(&s_fetch_lock);
    vTaskDelete(nullptr);
}

static void fetch_task([[maybe_unused]] void *arg) {
    char   temp_unit[sizeof(s_temp_unit)];
    char   wind_unit[sizeof(s_wind_unit)];
    char   unit_label[sizeof(s_unit_label)];
    char   temp_unit_chr[sizeof(s_temp_unit_chr)];
    double lat = 0.0, lon = 0.0;
    bool   fetch_aqi = false;

    if (s_config_mutex) xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    memcpy(temp_unit,     s_temp_unit,     sizeof(temp_unit));
    memcpy(wind_unit,     s_wind_unit,     sizeof(wind_unit));
    memcpy(unit_label,    s_unit_label,    sizeof(unit_label));
    memcpy(temp_unit_chr, s_temp_unit_chr, sizeof(temp_unit_chr));
    lat = s_lat;
    lon = s_lon;
    fetch_aqi = s_fetch_aqi;
    if (s_config_mutex) xSemaphoreGive(s_config_mutex);

    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Weather fetch starting");
#define FETCH_FAIL(fmt, ...) do { \
        snprintf(s_last_error, sizeof(s_last_error), fmt, ##__VA_ARGS__); \
        s_fail_count++; \
        ESP_LOGE(TAG, "%s", s_last_error); \
    } while(0)

    char *buf = static_cast<char *>(malloc(RESPONSE_BUF_SIZE));
    if (!buf) {
        FETCH_FAIL("OOM allocating response buffer");
        finish_fetch_task();
        return;
    }

    char url[URL_BUF_SIZE];
    weather_build_url(url, sizeof(url), lat, lon, temp_unit, wind_unit);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url               = url;
    http_cfg.timeout_ms        = 10000;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        FETCH_FAIL("HTTP client init failed");
        free(buf);
        finish_fetch_task();
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        FETCH_FAIL("HTTP open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buf);
        finish_fetch_task();
        return;
    }

    esp_http_client_fetch_headers(client);

    int total = 0, n = 0;
    while (total < (int)RESPONSE_BUF_SIZE - 1 &&
           (n = esp_http_client_read(client,
                                     buf + total,
                                     RESPONSE_BUF_SIZE - 1 - total)) > 0) {
        total += n;
    }
    buf[total] = '\0';

    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code != 200 || total == 0) {
        FETCH_FAIL("HTTP %d, %d bytes", status_code, total);
        free(buf);
        finish_fetch_task();
        return;
    }

    weather_parse_result result = weather_parse_response(buf, (size_t)total,
                                                         temp_unit_chr, unit_label);
    free(buf);

    switch (result.status) {
    case WeatherParseStatus::JSON_PARSE_ERROR:
        FETCH_FAIL("JSON parse error");
        finish_fetch_task();
        return;
    case WeatherParseStatus::MISSING_CURRENT:
        FETCH_FAIL("no 'current' in response");
        finish_fetch_task();
        return;
    case WeatherParseStatus::MISSING_DAILY:
        FETCH_FAIL("no 'daily' in response");
        finish_fetch_task();
        return;
    case WeatherParseStatus::MISSING_FIELDS:
        FETCH_FAIL("missing fields in response");
        finish_fetch_task();
        return;
    case WeatherParseStatus::OK:
        break;
    }

    bool nws_ok = try_nws_forecast(lat, lon, temp_unit_chr, result.forecast);
    if (!nws_ok && is_nws_supported_location(lat, lon)) {
        ESP_LOGW(TAG, "Using Open-Meteo forecast fallback for US location");
    }

    // Optional AQI fetch — only when panel_humidity_metric == "aqi".
    if (fetch_aqi) {
        char aqi_url[192];
        aqi_build_url(aqi_url, sizeof(aqi_url), lat, lon);

        esp_http_client_config_t aqi_cfg = {};
        aqi_cfg.url               = aqi_url;
        aqi_cfg.timeout_ms        = 10000;
        aqi_cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t aqi_client = esp_http_client_init(&aqi_cfg);
        if (aqi_client) {
            char aqi_buf[512] = {};
            esp_err_t aqi_err = esp_http_client_open(aqi_client, 0);
            if (aqi_err == ESP_OK) {
                esp_http_client_fetch_headers(aqi_client);
                int aqi_total = 0, aqi_n = 0;
                while (aqi_total < (int)sizeof(aqi_buf) - 1 &&
                       (aqi_n = esp_http_client_read(aqi_client,
                                                     aqi_buf + aqi_total,
                                                     sizeof(aqi_buf) - 1 - aqi_total)) > 0) {
                    aqi_total += aqi_n;
                }
                aqi_buf[aqi_total] = '\0';
                int aqi_status = esp_http_client_get_status_code(aqi_client);
                esp_http_client_close(aqi_client);

                if (aqi_status == 200 && aqi_total > 0) {
                    cJSON *aqi_root = cJSON_ParseWithLength(aqi_buf, (size_t)aqi_total);
                    if (aqi_root) {
                        cJSON *cur = cJSON_GetObjectItemCaseSensitive(aqi_root, "current");
                        cJSON *val = cJSON_GetObjectItemCaseSensitive(cur, "us_aqi");
                        if (cJSON_IsNumber(val)) {
                            result.conditions.us_aqi    = (int16_t)val->valueint;
                            result.conditions.aqi_valid = true;
                            ESP_LOGI(TAG, "AQI fetched: US AQI=%d", result.conditions.us_aqi);
                        }
                        cJSON_Delete(aqi_root);
                    }
                } else {
                    ESP_LOGW(TAG, "AQI fetch HTTP %d, %d bytes", aqi_status, aqi_total);
                }
            } else {
                ESP_LOGW(TAG, "AQI HTTP open failed: %s", esp_err_to_name(aqi_err));
            }
            esp_http_client_cleanup(aqi_client);
        } else {
            ESP_LOGW(TAG, "AQI HTTP client init failed");
        }
    }

    const current_conditions &cond = result.conditions;
    ESP_LOGI(TAG, "Fetched: %d°%s, wind=%d %s @ %u° (%s), humid=%u%%, rise=%u, set=%u; forecast [0] hi=%d lo=%d",
             cond.temp_rounded, cond.temp_unit,
             cond.wind_speed, cond.wind_unit, cond.wind_deg,
             wind_direction_abbr(cond.wind_deg),
             cond.humidity, cond.sunrise_min, cond.sunset_min,
             result.forecast.days[0].high, result.forecast.days[0].low);

    snprintf(s_last_error, sizeof(s_last_error), "ok");
    s_success_count++;
    if (s_on_conditions) s_on_conditions(result.conditions);
    if (s_on_forecast)   s_on_forecast(result.forecast);
    finish_fetch_task();
}

const char *weather_service_last_error()  { return s_last_error; }
int         weather_service_fail_count()  { return s_fail_count; }
int         weather_service_success_count() { return s_success_count; }

// ---------------------------------------------------------------------------
// Timer

static void timer_cb([[maybe_unused]] void *arg) {
    weather_service_trigger_fetch();
}

// ---------------------------------------------------------------------------
// Public API

bool weather_service_init(conditions_callback_t on_conditions,
                          forecast_callback_t   on_forecast) {
    s_on_conditions = on_conditions;
    s_on_forecast   = on_forecast;

    s_config_mutex = xSemaphoreCreateMutex();
    if (!s_config_mutex) {
        ESP_LOGE(TAG, "Failed to create config mutex — config reads will be unprotected");
    }

    esp_timer_create_args_t args = {};
    args.callback        = timer_cb;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name            = "weather_fetch";

    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    return load_config_from_service();
}

bool weather_service_reload_config() {
    bool configured = load_config_from_service();
    if (!configured && s_timer) {
        esp_timer_stop(s_timer);
    }
    return configured;
}

void weather_service_trigger_fetch() {
    if (!s_configured) {
        ESP_LOGW(TAG, "Weather fetch skipped (not configured)");
        return;
    }

    bool should_start = false;

    portENTER_CRITICAL(&s_fetch_lock);

    esp_err_t stop_err = esp_timer_stop(s_timer);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        portEXIT_CRITICAL(&s_fetch_lock);
        ESP_ERROR_CHECK(stop_err);
    }

    uint64_t interval = fetch_interval_us();
    esp_err_t start_err = esp_timer_start_once(s_timer, interval);
    if (start_err == ESP_ERR_INVALID_STATE) {
        stop_err = esp_timer_stop(s_timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            portEXIT_CRITICAL(&s_fetch_lock);
            ESP_ERROR_CHECK(stop_err);
        }
        start_err = esp_timer_start_once(s_timer, interval);
    }
    if (start_err != ESP_OK) {
        portEXIT_CRITICAL(&s_fetch_lock);
        ESP_ERROR_CHECK(start_err);
    }

    int64_t now_us = esp_timer_get_time();
    bool stuck = s_fetch_in_progress &&
                 (now_us - s_fetch_started_us) > FETCH_TIMEOUT_US;
    if (!s_fetch_in_progress || stuck) {
        if (stuck) {
            ESP_LOGW(TAG, "Weather fetch task appears stuck (>60 s); force-resetting guard");
        }
        s_fetch_in_progress = true;
        s_fetch_started_us  = now_us;
        should_start = true;
    }

    portEXIT_CRITICAL(&s_fetch_lock);

    if (!should_start) {
        ESP_LOGW(TAG, "Weather fetch already in progress; skipped duplicate trigger");
        return;
    }

    uint8_t interval_min = config_service::get_config().conditions_refresh_minutes;
    ESP_LOGI(TAG, "Weather fetch triggered (next in %u min)", interval_min);
    BaseType_t ok = xTaskCreate(fetch_task, "weather_fetch", 12288, nullptr, 5, nullptr);
    if (ok != pdPASS) {
        portENTER_CRITICAL(&s_fetch_lock);
        s_fetch_in_progress = false;
        portEXIT_CRITICAL(&s_fetch_lock);
        ESP_LOGE(TAG, "Failed to create weather fetch task");
    }
}
