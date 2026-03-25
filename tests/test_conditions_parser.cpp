// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

// Host-side tests for conditions_parser.h / conditions_parser.cpp.
// No ESP-IDF, no FreeRTOS, no hardware.

#include "conditions_parser.h"

#include <cassert>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int s_pass = 0;
static int s_fail = 0;
static const char *s_suite = "";

#define SUITE(name) do { s_suite = (name); } while (0)

#define CHECK(expr) \
  do { \
    if (expr) { \
      s_pass++; \
    } else { \
      s_fail++; \
      fprintf(stderr, "FAIL [%s] %s:%d — %s\n", s_suite, __FILE__, __LINE__, #expr); \
    } \
  } while (0)

#define CHECK_EQ(a, b)    CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static conditions_parse_result parse(const char *json) {
    return conditions_parse_response(json, strlen(json));
}

static bool url_contains(const char *url, const char *substr) {
    return strstr(url, substr) != nullptr;
}

// Minimal valid response with current + daily.
static const char *VALID_JSON = R"({
  "current": {
    "time": "2024-03-17T14:00",
    "interval": 900,
    "temperature_2m": 72.4,
    "relative_humidity_2m": 55,
    "wind_speed_10m": 11.5,
    "wind_direction_10m": 225
  },
  "daily": {
    "time": ["2024-03-17"],
    "sunrise": ["2024-03-17T07:23"],
    "sunset":  ["2024-03-17T19:30"]
  }
})";

// Valid response without "daily" block.
static const char *NO_DAILY_JSON = R"({
  "current": {
    "temperature_2m": 68.0,
    "relative_humidity_2m": 80,
    "wind_speed_10m": 5.0,
    "wind_direction_10m": 90
  }
})";

// ---------------------------------------------------------------------------
// URL builder tests
// ---------------------------------------------------------------------------

static void test_build_url_imperial() {
    SUITE("conditions_build_url_imperial");
    char url[512];
    bool ok = conditions_build_url(url, sizeof(url), 37.7749, -122.4194,
                                   "fahrenheit", "mph");
    CHECK(ok);
    CHECK(url_contains(url, "api.open-meteo.com"));
    CHECK(url_contains(url, "temperature_unit=fahrenheit"));
    CHECK(url_contains(url, "wind_speed_unit=mph"));
    CHECK(url_contains(url, "temperature_2m"));
    CHECK(url_contains(url, "relative_humidity_2m"));
    CHECK(url_contains(url, "wind_speed_10m"));
    CHECK(url_contains(url, "wind_direction_10m"));
    CHECK(url_contains(url, "sunrise"));
    CHECK(url_contains(url, "sunset"));
    CHECK(url_contains(url, "forecast_days=1"));
    CHECK(url_contains(url, "timezone=auto"));
}

static void test_build_url_metric() {
    SUITE("conditions_build_url_metric");
    char url[512];
    bool ok = conditions_build_url(url, sizeof(url), 48.8566, 2.3522,
                                   "celsius", "kmh");
    CHECK(ok);
    CHECK(url_contains(url, "temperature_unit=celsius"));
    CHECK(url_contains(url, "wind_speed_unit=kmh"));
}

static void test_build_url_truncation() {
    SUITE("conditions_build_url_truncation");
    char tiny[16];
    bool ok = conditions_build_url(tiny, sizeof(tiny), 0.0, 0.0,
                                   "fahrenheit", "mph");
    CHECK(!ok);
}

// ---------------------------------------------------------------------------
// Parse valid response
// ---------------------------------------------------------------------------

static void test_parse_valid() {
    SUITE("conditions_parse_valid");
    auto r = parse(VALID_JSON);
    CHECK_EQ(r.status, ConditionsParseStatus::OK);
    CHECK_EQ(r.temp,       72);    // lround(72.4)
    CHECK_EQ(r.humidity,   55);
    CHECK_EQ(r.wind_speed, 12);    // lround(11.5)
    CHECK_EQ(r.wind_deg,   225);
    CHECK_EQ(r.sunrise_min, (uint16_t)(7*60+23));   // 443
    CHECK_EQ(r.sunset_min,  (uint16_t)(19*60+30));  // 1170
}

static void test_parse_temp_rounding_up() {
    SUITE("conditions_parse_temp_rounding_up");
    const char *json = R"({
      "current":{
        "temperature_2m": 98.5,
        "relative_humidity_2m": 60,
        "wind_speed_10m": 3.0,
        "wind_direction_10m": 0
      }
    })";
    auto r = parse(json);
    CHECK_EQ(r.status, ConditionsParseStatus::OK);
    CHECK_EQ(r.temp, 99);  // lround(98.5) = 99
}

static void test_parse_negative_temp() {
    SUITE("conditions_parse_negative_temp");
    const char *json = R"({
      "current":{
        "temperature_2m": -5.7,
        "relative_humidity_2m": 90,
        "wind_speed_10m": 20.0,
        "wind_direction_10m": 180
      }
    })";
    auto r = parse(json);
    CHECK_EQ(r.status, ConditionsParseStatus::OK);
    CHECK_EQ(r.temp, -6);  // lround(-5.7) = -6
}

static void test_parse_no_daily() {
    SUITE("conditions_parse_no_daily");
    auto r = parse(NO_DAILY_JSON);
    CHECK_EQ(r.status, ConditionsParseStatus::OK);
    CHECK_EQ(r.temp, 68);
    CHECK_EQ(r.sunrise_min, 0);
    CHECK_EQ(r.sunset_min,  0);
}

static void test_parse_wind_deg_wrap() {
    SUITE("conditions_parse_wind_deg_wrap");
    const char *json = R"({
      "current":{
        "temperature_2m": 50.0,
        "relative_humidity_2m": 40,
        "wind_speed_10m": 5.0,
        "wind_direction_10m": 360
      }
    })";
    auto r = parse(json);
    CHECK_EQ(r.status, ConditionsParseStatus::OK);
    CHECK_EQ(r.wind_deg, 0);  // 360 % 360
}

// ---------------------------------------------------------------------------
// Parse error cases
// ---------------------------------------------------------------------------

static void test_parse_null() {
    SUITE("conditions_parse_null");
    auto r = conditions_parse_response(nullptr, 0);
    CHECK_EQ(r.status, ConditionsParseStatus::JSON_PARSE_ERROR);
}

static void test_parse_empty() {
    SUITE("conditions_parse_empty");
    auto r = parse("");
    CHECK_EQ(r.status, ConditionsParseStatus::JSON_PARSE_ERROR);
}

static void test_parse_malformed() {
    SUITE("conditions_parse_malformed");
    auto r = parse("{not valid json");
    CHECK_EQ(r.status, ConditionsParseStatus::JSON_PARSE_ERROR);
}

static void test_parse_missing_current() {
    SUITE("conditions_parse_missing_current");
    auto r = parse("{\"daily\":{}}");
    CHECK_EQ(r.status, ConditionsParseStatus::MISSING_CURRENT);
}

static void test_parse_missing_temp() {
    SUITE("conditions_parse_missing_temp");
    const char *json = R"({
      "current":{
        "relative_humidity_2m": 50,
        "wind_speed_10m": 5.0,
        "wind_direction_10m": 90
      }
    })";
    auto r = parse(json);
    CHECK_EQ(r.status, ConditionsParseStatus::MISSING_FIELDS);
}

static void test_parse_missing_humidity() {
    SUITE("conditions_parse_missing_humidity");
    const char *json = R"({
      "current":{
        "temperature_2m": 70.0,
        "wind_speed_10m": 5.0,
        "wind_direction_10m": 90
      }
    })";
    auto r = parse(json);
    CHECK_EQ(r.status, ConditionsParseStatus::MISSING_FIELDS);
}

// ---------------------------------------------------------------------------
// Wind direction abbreviation
// ---------------------------------------------------------------------------

static void test_wind_direction_cardinals() {
    SUITE("wind_direction_cardinals");
    CHECK_STREQ(wind_direction_abbr(0),   "N");
    CHECK_STREQ(wind_direction_abbr(90),  "E");
    CHECK_STREQ(wind_direction_abbr(180), "S");
    CHECK_STREQ(wind_direction_abbr(270), "W");
    CHECK_STREQ(wind_direction_abbr(360), "N");  // 360 % 360 → N
}

static void test_wind_direction_intercardinals() {
    SUITE("wind_direction_intercardinals");
    CHECK_STREQ(wind_direction_abbr(45),  "NE");
    CHECK_STREQ(wind_direction_abbr(135), "SE");
    CHECK_STREQ(wind_direction_abbr(225), "SW");
    CHECK_STREQ(wind_direction_abbr(315), "NW");
}

static void test_wind_direction_boundary() {
    SUITE("wind_direction_boundary");
    // 22 → (22+22)/45 = 44/45 = 0 → N
    CHECK_STREQ(wind_direction_abbr(22),  "N");
    // 23 → (23+22)/45 = 45/45 = 1 → NE
    CHECK_STREQ(wind_direction_abbr(23),  "NE");
}

// ---------------------------------------------------------------------------

int main() {
    test_build_url_imperial();
    test_build_url_metric();
    test_build_url_truncation();

    test_parse_valid();
    test_parse_temp_rounding_up();
    test_parse_negative_temp();
    test_parse_no_daily();
    test_parse_wind_deg_wrap();

    test_parse_null();
    test_parse_empty();
    test_parse_malformed();
    test_parse_missing_current();
    test_parse_missing_temp();
    test_parse_missing_humidity();

    test_wind_direction_cardinals();
    test_wind_direction_intercardinals();
    test_wind_direction_boundary();

    printf("conditions_parser: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail > 0 ? 1 : 0;
}
