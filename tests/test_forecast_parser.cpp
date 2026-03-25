// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

// Host-side tests for forecast_parser.h / forecast_parser.cpp.
// No ESP-IDF, FreeRTOS, or LVGL dependencies.

#include "forecast_parser.h"

#include <cassert>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal test framework

static int s_pass = 0;
static int s_fail = 0;
static const char *s_suite = "";

#define SUITE(name) do { s_suite = (name); } while (0)

#define CHECK(expr) \
  do { \
    if (expr) { s_pass++; } \
    else { \
      s_fail++; \
      fprintf(stderr, "FAIL [%s] %s:%d — %s\n", s_suite, __FILE__, __LINE__, #expr); \
    } \
  } while (0)

#define CHECK_EQ(a, b)    CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)

// ---------------------------------------------------------------------------
// URL builder

static void test_url_contains_lat_lon() {
    SUITE("url_lat_lon");
    char buf[320];
    forecast_build_url(buf, sizeof(buf), 37.7749, -122.4194, "fahrenheit");
    CHECK(strstr(buf, "37.7749") != nullptr);
    CHECK(strstr(buf, "-122.4194") != nullptr);
    CHECK(strstr(buf, "fahrenheit") != nullptr);
}

static void test_url_contains_daily_fields() {
    SUITE("url_daily_fields");
    char buf[320];
    forecast_build_url(buf, sizeof(buf), 0.0, 0.0, "celsius");
    CHECK(strstr(buf, "temperature_2m_max") != nullptr);
    CHECK(strstr(buf, "temperature_2m_min") != nullptr);
    CHECK(strstr(buf, "cloud_cover_mean") != nullptr);
    CHECK(strstr(buf, "precipitation_probability_max") != nullptr);
    CHECK(strstr(buf, "forecast_days=5") != nullptr);
    CHECK(strstr(buf, "celsius") != nullptr);
}

// ---------------------------------------------------------------------------
// day_code helper

static void test_day_codes() {
    SUITE("day_codes");
    CHECK_STREQ(forecast_day_code(0), "SU");
    CHECK_STREQ(forecast_day_code(1), "MO");
    CHECK_STREQ(forecast_day_code(2), "TU");
    CHECK_STREQ(forecast_day_code(3), "WE");
    CHECK_STREQ(forecast_day_code(4), "TH");
    CHECK_STREQ(forecast_day_code(5), "FR");
    CHECK_STREQ(forecast_day_code(6), "SA");
    CHECK_STREQ(forecast_day_code(-1), "--");
    CHECK_STREQ(forecast_day_code(7), "--");
}

// ---------------------------------------------------------------------------
// Valid 5-day response

static const char *VALID_5DAY =
    "{"
    "\"daily\":{"
    "\"time\":[\"2026-03-17\",\"2026-03-18\",\"2026-03-19\",\"2026-03-20\",\"2026-03-21\"],"
    "\"temperature_2m_max\":[18.5,20.1,15.3,22.0,19.8],"
    "\"temperature_2m_min\":[10.2,12.5,8.1,14.0,11.3]"
    "}"
    "}";

static void test_parse_valid_5day() {
    SUITE("parse_5day");
    forecast_parse_result r = forecast_parse_response(VALID_5DAY, (int)strlen(VALID_5DAY));
    CHECK_EQ(r.status, ForecastParseStatus::OK);
    CHECK_EQ(r.count, 5);
    // 2026-03-17 is a Tuesday → wday=2
    CHECK_EQ(r.days[0].wday, (uint8_t)2);
    CHECK_EQ(r.days[0].high, (int8_t)18);
    CHECK_EQ(r.days[0].low,  (int8_t)10);
    CHECK(r.days[0].valid);
    // 2026-03-18 is a Wednesday → wday=3
    CHECK_EQ(r.days[1].wday, (uint8_t)3);
    CHECK_EQ(r.days[1].high, (int8_t)20);
    CHECK_EQ(r.days[1].low,  (int8_t)12);
}

// ---------------------------------------------------------------------------
// Partial response (2 days)

static const char *PARTIAL_2DAY =
    "{"
    "\"daily\":{"
    "\"time\":[\"2026-03-17\",\"2026-03-18\"],"
    "\"temperature_2m_max\":[18.5,20.1],"
    "\"temperature_2m_min\":[10.2,12.5]"
    "}"
    "}";

static void test_parse_partial_2day() {
    SUITE("parse_2day");
    forecast_parse_result r = forecast_parse_response(PARTIAL_2DAY, (int)strlen(PARTIAL_2DAY));
    CHECK_EQ(r.status, ForecastParseStatus::OK);
    CHECK_EQ(r.count, 2);
    CHECK(r.days[0].valid);
    CHECK(r.days[1].valid);
}

// ---------------------------------------------------------------------------
// Negative temperatures

static const char *NEGATIVE_TEMPS =
    "{"
    "\"daily\":{"
    "\"time\":[\"2026-01-15\"],"
    "\"temperature_2m_max\":[-3.2],"
    "\"temperature_2m_min\":[-12.8]"
    "}"
    "}";

static void test_parse_negative_temps() {
    SUITE("parse_negative");
    forecast_parse_result r = forecast_parse_response(NEGATIVE_TEMPS, (int)strlen(NEGATIVE_TEMPS));
    CHECK_EQ(r.status, ForecastParseStatus::OK);
    CHECK_EQ(r.count, 1);
    CHECK_EQ(r.days[0].high, (int8_t)-3);
    CHECK_EQ(r.days[0].low,  (int8_t)-12);
}

// ---------------------------------------------------------------------------
// Missing "daily" key

static const char *NO_DAILY = "{\"current\":{\"temperature\":20}}";

static void test_missing_daily() {
    SUITE("missing_daily");
    forecast_parse_result r = forecast_parse_response(NO_DAILY, (int)strlen(NO_DAILY));
    CHECK_EQ(r.status, ForecastParseStatus::MISSING_DAILY);
}

// ---------------------------------------------------------------------------
// Missing required arrays

static const char *MISSING_MAX =
    "{"
    "\"daily\":{"
    "\"time\":[\"2026-03-17\"],"
    "\"temperature_2m_min\":[10.0]"
    "}"
    "}";

static void test_missing_fields() {
    SUITE("missing_fields");
    forecast_parse_result r = forecast_parse_response(MISSING_MAX, (int)strlen(MISSING_MAX));
    CHECK_EQ(r.status, ForecastParseStatus::MISSING_FIELDS);
}

// ---------------------------------------------------------------------------
// Invalid JSON

static const char *BAD_JSON = "not json at all {";

static void test_invalid_json() {
    SUITE("invalid_json");
    forecast_parse_result r = forecast_parse_response(BAD_JSON, (int)strlen(BAD_JSON));
    CHECK_EQ(r.status, ForecastParseStatus::JSON_PARSE_ERROR);
}

// ---------------------------------------------------------------------------
// Empty time array

static const char *EMPTY_ARRAY =
    "{"
    "\"daily\":{"
    "\"time\":[],"
    "\"temperature_2m_max\":[],"
    "\"temperature_2m_min\":[]"
    "}"
    "}";

static void test_empty_arrays() {
    SUITE("empty_arrays");
    forecast_parse_result r = forecast_parse_response(EMPTY_ARRAY, (int)strlen(EMPTY_ARRAY));
    CHECK_EQ(r.status, ForecastParseStatus::MISSING_FIELDS);
}

// ---------------------------------------------------------------------------
// More than 5 days — clamped to FORECAST_DAYS

static const char *SIX_DAYS =
    "{"
    "\"daily\":{"
    "\"time\":[\"2026-03-17\",\"2026-03-18\",\"2026-03-19\","
               "\"2026-03-20\",\"2026-03-21\",\"2026-03-22\"],"
    "\"temperature_2m_max\":[18,20,15,22,19,16],"
    "\"temperature_2m_min\":[10,12,8,14,11,9]"
    "}"
    "}";

static void test_clamp_to_forecast_days() {
    SUITE("clamp_days");
    forecast_parse_result r = forecast_parse_response(SIX_DAYS, (int)strlen(SIX_DAYS));
    CHECK_EQ(r.status, ForecastParseStatus::OK);
    CHECK_EQ(r.count, FORECAST_DAYS);
}

// ---------------------------------------------------------------------------
// Condition icon derivation from cloud_cover_mean + precipitation_probability_max

static const char *WITH_CLOUD_PRECIP =
    "{"
    "\"daily\":{"
    "\"time\":[\"2026-03-17\",\"2026-03-18\",\"2026-03-19\",\"2026-03-20\",\"2026-03-21\"],"
    "\"temperature_2m_max\":[75.0,68.0,55.0,60.0,62.0],"
    "\"temperature_2m_min\":[55.0,50.0,42.0,44.0,45.0],"
    "\"cloud_cover_mean\":[4.0,83.0,20.0,50.0,30.0],"
    "\"precipitation_probability_max\":[0.0,0.0,60.0,0.0,0.0]"
    "}"
    "}";

static void test_parse_cloud_precip_derivation() {
    SUITE("parse_cloud_precip");
    forecast_parse_result r = forecast_parse_response(WITH_CLOUD_PRECIP,
                                                      (int)strlen(WITH_CLOUD_PRECIP));
    CHECK_EQ(r.status, ForecastParseStatus::OK);
    CHECK_EQ(r.count, 5);
    // cloud=4%,  precip=0%  → sunny (code 0)
    CHECK_EQ(r.days[0].weather_code, (uint16_t)0);
    // cloud=83%, precip=0%  → cloudy (code 3)
    CHECK_EQ(r.days[1].weather_code, (uint16_t)3);
    // cloud=20%, precip=60% → rainy (code 61, precip threshold wins)
    CHECK_EQ(r.days[2].weather_code, (uint16_t)61);
    // cloud=50%, precip=0%  → partly cloudy (code 2)
    CHECK_EQ(r.days[3].weather_code, (uint16_t)2);
    // cloud=30%, precip=0%  → sunny (code 0, below both thresholds)
    CHECK_EQ(r.days[4].weather_code, (uint16_t)0);
}

static void test_parse_missing_cloud_precip() {
    SUITE("parse_missing_cloud_precip");
    // Responses without cloud/precip should still parse OK; codes default to 0 (sunny).
    forecast_parse_result r = forecast_parse_response(VALID_5DAY, (int)strlen(VALID_5DAY));
    CHECK_EQ(r.status, ForecastParseStatus::OK);
    for (int i = 0; i < r.count; i++)
        CHECK_EQ(r.days[i].weather_code, (uint16_t)0);
}

// ---------------------------------------------------------------------------
// main

int main() {
    test_url_contains_lat_lon();
    test_url_contains_daily_fields();
    test_day_codes();
    test_parse_valid_5day();
    test_parse_partial_2day();
    test_parse_negative_temps();
    test_missing_daily();
    test_missing_fields();
    test_invalid_json();
    test_empty_arrays();
    test_clamp_to_forecast_days();
    test_parse_cloud_precip_derivation();
    test_parse_missing_cloud_precip();

    printf("\n%s: %d passed, %d failed\n",
           s_fail == 0 ? "PASS" : "FAIL", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
