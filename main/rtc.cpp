//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT
//
//

#include "rtc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>

#include "i2c_helper.h"
#include "pcf8563.h"
#include "timegm.h"

static i2c_port_t i2c_port = I2C_NUM_0;
static const pcf8563_t rtc = {
    .read = i2c_read,
    .write = i2c_write,
    .handle = &i2c_port,
};

static const auto TAG = "external_rtc";
static constexpr uint8_t PCF8563_CLKOUT_CONTROL = 0x0d;
static constexpr uint32_t DISCIPLINE_INTERVAL_MS = 60'000;
static constexpr int64_t DISCIPLINE_EDGE_TIMEOUT_MS = 1100;
static constexpr int64_t DISCIPLINE_MAX_ADJ_US = 2'000'000;
static constexpr time_t RTC_MIN_VALID_EPOCH = 1735689600;  // 2025-01-01 UTC

static bool s_initialized = false;
static bool s_time_valid = false;
static bool s_battery_ok = false;
static bool s_discipline_active = false;
static bool s_task_started = false;
static int32_t s_last_error_ms = 0;
static int32_t s_max_error_ms = 0;

static bool valid_tm(const tm &t) {
  return t.tm_year >= 125 && t.tm_year <= 199 &&
         t.tm_mon >= 0 && t.tm_mon <= 11 &&
         t.tm_mday >= 1 && t.tm_mday <= 31 &&
         t.tm_hour >= 0 && t.tm_hour <= 23 &&
         t.tm_min >= 0 && t.tm_min <= 59 &&
         t.tm_sec >= 0 && t.tm_sec <= 60;
}

static void timeval_from_us(struct timeval *tv, int64_t us) {
  tv->tv_sec = static_cast<time_t>(us / 1'000'000LL);
  tv->tv_usec = static_cast<suseconds_t>(us % 1'000'000LL);
  if (tv->tv_usec < 0) {
    tv->tv_sec--;
    tv->tv_usec += 1'000'000;
  }
}

static bool read_rtc_utc(tm &out) {
  memset(&out, 0, sizeof(out));
  pcf8563_err_t err = pcf8563_read(&rtc, &out);
  if (err == PCF8563_ERR_LOW_VOLTAGE) {
    ESP_LOGE(TAG, "pcf8563_read failed: low voltage");
    s_battery_ok = false;
    return false;
  }
  if (err != PCF8563_OK) {
    ESP_LOGE(TAG, "pcf8563_read failed: %ld", err);
    return false;
  }
  if (!valid_tm(out)) {
    ESP_LOGW(TAG, "RTC decoded out-of-range: %04d-%02d-%02d %02d:%02d:%02d",
             out.tm_year + 1900, out.tm_mon + 1, out.tm_mday,
             out.tm_hour, out.tm_min, out.tm_sec);
    return false;
  }
  time_t epoch = timegm(&out);
  if (epoch < RTC_MIN_VALID_EPOCH) {
    ESP_LOGW(TAG, "RTC time too old: %04d-%02d-%02d — waiting for NTP",
             out.tm_year + 1900, out.tm_mon + 1, out.tm_mday);
    return false;
  }
  s_battery_ok = true;
  return true;
}

static void disable_clkout() {
  uint8_t zero = 0;
  int32_t err = i2c_write(&i2c_port, PCF8563_ADDRESS, PCF8563_CLKOUT_CONTROL, &zero, 1);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "PCF8563 CLKOUT disabled");
  } else {
    ESP_LOGW(TAG, "PCF8563 CLKOUT disable failed: %ld", err);
  }
}

bool rtc_init() {
  ESP_ERROR_CHECK(i2c_init(i2c_port));
  ESP_ERROR_CHECK(pcf8563_init(&rtc));
  s_initialized = true;
  disable_clkout();

  tm rtc_time = {};
  if (!read_rtc_utc(rtc_time)) {
    s_time_valid = false;
    return false;
  }

  time_t now = timegm(&rtc_time);
  timeval tv = {.tv_sec = now, .tv_usec = 0};
  settimeofday(&tv, nullptr);
  s_time_valid = true;

  char formatted_time[26];
  asctime_r(&rtc_time, formatted_time);
  ESP_LOGI(TAG, "Got time from RTC: %s", formatted_time);
  return true;
}

void rtc_persist() {
  timeval tv_now = {};
  gettimeofday(&tv_now, nullptr);
  time_t rounded = tv_now.tv_sec + (tv_now.tv_usec >= 500000 ? 1 : 0);
  tm utc_time = {};
  gmtime_r(&rounded, &utc_time);

  pcf8563_err_t err = pcf8563_write(&rtc, &utc_time);
  if (err != PCF8563_OK) {
    ESP_LOGE(TAG, "pcf8563_write failed: %ld", err);
    return;
  }

  s_time_valid = true;
  s_battery_ok = true;
  char formatted_time[26];
  asctime_r(&utc_time, formatted_time);
  ESP_LOGI(TAG, "Saved rounded time to RTC: %s", formatted_time);
}

static void discipline_tick() {
  if (!s_initialized || !s_time_valid) return;

  tm a = {};
  tm b = {};
  if (!read_rtc_utc(a)) return;

  TickType_t t0 = xTaskGetTickCount();
  timeval sysnow = {};
  bool ticked = false;
  while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(DISCIPLINE_EDGE_TIMEOUT_MS)) {
    if (!read_rtc_utc(b)) return;
    if (b.tm_sec != a.tm_sec) {
      gettimeofday(&sysnow, nullptr);
      ticked = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
  if (!ticked) {
    ESP_LOGW(TAG, "PCF discipline: no tick edge");
    return;
  }

  time_t rtc_epoch = timegm(&b);
  double sys = static_cast<double>(sysnow.tv_sec) + sysnow.tv_usec / 1e6;
  double err_s = sys - static_cast<double>(rtc_epoch);  // +ve = ESP ahead
  int64_t adj_us = llround(-err_s * 1e6);
  adj_us = std::clamp<int64_t>(adj_us, -DISCIPLINE_MAX_ADJ_US, DISCIPLINE_MAX_ADJ_US);
  timeval delta = {};
  timeval_from_us(&delta, adj_us);
  adjtime(&delta, nullptr);

  int32_t err_ms = static_cast<int32_t>(llround(err_s * 1000.0));
  s_last_error_ms = err_ms;
  int32_t abs_ms = err_ms < 0 ? -err_ms : err_ms;
  if (abs_ms > s_max_error_ms) s_max_error_ms = abs_ms;
  s_discipline_active = true;
  ESP_LOGD(TAG, "PCF discipline: clock was %+ld ms vs RTC -> slew %+lld us",
           static_cast<long>(err_ms), static_cast<long long>(adj_us));
}

static void discipline_task(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(DISCIPLINE_INTERVAL_MS));
    discipline_tick();
  }
}

void rtc_discipline_start() {
  if (s_task_started) return;
  BaseType_t ok = xTaskCreate(discipline_task, "rtc_disc", 4096, nullptr, 4, nullptr);
  if (ok == pdPASS) {
    s_task_started = true;
    ESP_LOGI(TAG, "PCF8563 discipline task started");
  } else {
    ESP_LOGE(TAG, "PCF8563 discipline task start failed");
  }
}

bool rtc_has_valid_time() { return s_time_valid; }
bool rtc_battery_ok() { return s_battery_ok; }
bool rtc_discipline_active() { return s_discipline_active; }
int32_t rtc_last_error_ms() { return s_last_error_ms; }
int32_t rtc_max_error_ms() { return s_max_error_ms; }
