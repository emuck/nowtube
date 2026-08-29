// SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
// SPDX-License-Identifier: MIT
//
// Host-side tests for config_validation helpers.
// Build with:  cd tests && cmake -B build && cmake --build build && ./build/test_config_validation

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "config_validation.h"

// ---------------------------------------------------------------------------
// Minimal test framework

static int s_pass = 0, s_fail = 0;
static const char *s_suite  = "";
static int         s_suite_fail = 0;

static void suite_begin(const char *name) {
  s_suite      = name;
  s_suite_fail = 0;
  printf("\n[%s]\n", name);
}

#define CHECK(cond) \
  do { \
    if (cond) { \
      s_pass++; \
    } else { \
      fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      s_fail++; s_suite_fail++; \
    } \
  } while (0)

#define CHECK_STREQ(a, b) \
  do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) == 0) { \
      s_pass++; \
    } else { \
      fprintf(stderr, "  FAIL  %s:%d  \"%s\" != \"%s\"\n", \
              __FILE__, __LINE__, _a, _b); \
      s_fail++; s_suite_fail++; \
    } \
  } while (0)

#define CHECK_NEAR(a, b, eps) \
  do { \
    double _diff = (double)(a) - (double)(b); \
    if (_diff < 0) _diff = -_diff; \
    if (_diff <= (double)(eps)) { \
      s_pass++; \
    } else { \
      fprintf(stderr, "  FAIL  %s:%d  |%g - %g| > %g\n", \
              __FILE__, __LINE__, (double)(a), (double)(b), (double)(eps)); \
      s_fail++; s_suite_fail++; \
    } \
  } while (0)

// ---------------------------------------------------------------------------
// Helper: write a text file, return path (caller must not modify the literal)

static void write_file(const char *path, const char *contents) {
  FILE *f = fopen(path, "w");
  assert(f != nullptr);
  fputs(contents, f);
  fclose(f);
}

// ---------------------------------------------------------------------------
// Tests

static void test_defaults() {
  suite_begin("default config values");

  device_config c{};
  CHECK_STREQ(c.timezone,          "PST8PDT,M3.2.0,M11.1.0");
  CHECK_STREQ(c.weather_units,     "imperial");
  CHECK(c.display_brightness_pct  == 60);
  CHECK(c.has_weather_location    == false);
  CHECK(c.weather_lat             == 0.0);
  CHECK(c.weather_lon             == 0.0);
  CHECK(c.wifi_ssid[0]            == '\0');
  CHECK(c.wifi_psk[0]             == '\0');
  CHECK(c.schema_version          == device_config::SCHEMA_VERSION);

  // validate() on defaults must leave them intact
  config_validate(c);
  CHECK_STREQ(c.timezone,         "PST8PDT,M3.2.0,M11.1.0");
  CHECK_STREQ(c.weather_units,    "imperial");
  CHECK(c.display_brightness_pct == 60);
}

static void test_trim_string() {
  suite_begin("config_trim_string");

  auto t = [](const char *in, const char *expected) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", in);
    config_trim_string(buf);
    CHECK_STREQ(buf, expected);
  };

  t("hello",          "hello");
  t("  hello  ",      "hello");
  t("\thello\t",      "hello");
  t("\r\nhello\r\n",  "hello");
  t("  ",             "");
  t("",               "");
  t("  a",            "a");
  t("a  ",            "a");
  t("  a  b  ",       "a  b");  // only leading/trailing trimmed, not internal
}

static void test_trim_nullptr() {
  suite_begin("config_trim_string with nullptr");
  // Must not crash
  config_trim_string(nullptr);
  CHECK(true);
}

static void test_units_normalisation() {
  suite_begin("weather units normalisation");

  auto check = [](const char *units_in, const char *expected) {
    device_config c{};
    snprintf(c.weather_units, sizeof(c.weather_units), "%s", units_in);
    config_validate(c);
    CHECK_STREQ(c.weather_units, expected);
  };

  check("imperial",   "imperial");   // exact match — kept
  check("metric",     "metric");     // exact match — kept
  check("IMPERIAL",   "imperial");   // case mismatch → fallback
  check("Metric",     "imperial");   // case mismatch → fallback
  check("celsius",    "imperial");   // unknown value → fallback
  check("",           "imperial");   // empty → fallback
  check("  imperial", "imperial");   // leading whitespace trimmed first → kept
  check("  metric  ", "metric");     // surrounding whitespace trimmed first → kept
}

static void test_boot_mode_validation() {
  suite_begin("boot mode validation");

  CHECK(config_validate_mode(DisplayMode::CLOCK)    == DisplayMode::CLOCK);
  // DATE is a legacy NVS value — coerced to CLOCK on next boot
  CHECK(config_validate_mode(DisplayMode::DATE)     == DisplayMode::CLOCK);
  CHECK(config_validate_mode(DisplayMode::TODAY)    == DisplayMode::TODAY);
  CHECK(config_validate_mode(DisplayMode::FORECAST) == DisplayMode::FORECAST);
  CHECK(config_validate_mode(DisplayMode::SPECTRUM) == DisplayMode::SPECTRUM);
  // Old WEATHER value (1) is no longer a valid mode — must clamp to CLOCK
  CHECK(config_validate_mode(static_cast<DisplayMode>(1)) == DisplayMode::CLOCK);
  // Values outside the enum must clamp to CLOCK
  CHECK(config_validate_mode(static_cast<DisplayMode>(99))  == DisplayMode::CLOCK);
  CHECK(config_validate_mode(static_cast<DisplayMode>(255)) == DisplayMode::CLOCK);
  CHECK(config_validate_mode(static_cast<DisplayMode>(-1))  == DisplayMode::CLOCK);

}

static void test_brightness_clamping() {
  suite_begin("brightness clamping");

  device_config c{};
  c.display_brightness_pct = 101;
  config_validate(c);
  CHECK(c.display_brightness_pct == 100);

  c.display_brightness_pct = 200;
  config_validate(c);
  CHECK(c.display_brightness_pct == 100);

  c.display_brightness_pct = 100;
  config_validate(c);
  CHECK(c.display_brightness_pct == 100);

  c.display_brightness_pct = 0;
  config_validate(c);
  CHECK(c.display_brightness_pct == 0);

  c.display_brightness_pct = 60;
  config_validate(c);
  CHECK(c.display_brightness_pct == 60);
}

static void test_latlon_validation() {
  suite_begin("lat/lon range validation");

  auto make = [](double lat, double lon, bool has_loc) {
    device_config c{};
    c.weather_lat         = lat;
    c.weather_lon         = lon;
    c.has_weather_location = has_loc;
    return c;
  };

  // Valid coordinates — must be kept
  {
    device_config c = make(37.73, -122.13, true);
    config_validate(c);
    CHECK(c.has_weather_location == true);
    CHECK_NEAR(c.weather_lat,  37.73,   1e-9);
    CHECK_NEAR(c.weather_lon, -122.13,  1e-9);
  }

  // Boundary values — must be kept
  {
    device_config c = make(90.0, 180.0, true);
    config_validate(c);
    CHECK(c.has_weather_location == true);
  }
  {
    device_config c = make(-90.0, -180.0, true);
    config_validate(c);
    CHECK(c.has_weather_location == true);
  }

  // Lat out of range → location disabled, coordinates zeroed
  {
    device_config c = make(91.0, 0.0, true);
    config_validate(c);
    CHECK(c.has_weather_location == false);
    CHECK(c.weather_lat          == 0.0);
    CHECK(c.weather_lon          == 0.0);
  }

  // Lon out of range → location disabled
  {
    device_config c = make(0.0, 181.0, true);
    config_validate(c);
    CHECK(c.has_weather_location == false);
  }

  // Both out of range → location disabled
  {
    device_config c = make(91.0, 181.0, true);
    config_validate(c);
    CHECK(c.has_weather_location == false);
  }

  // has_location=false: out-of-range values are left untouched (no validation needed)
  {
    device_config c = make(91.0, 181.0, false);
    config_validate(c);
    CHECK(c.has_weather_location == false);
    // Coordinates are NOT zeroed because has_weather_location was already false
    CHECK_NEAR(c.weather_lat, 91.0,  1e-9);
    CHECK_NEAR(c.weather_lon, 181.0, 1e-9);
  }
}

static void test_default_timezone() {
  suite_begin("default timezone");

  // Empty timezone → default
  {
    device_config c{};
    c.timezone[0] = '\0';
    config_validate(c);
    CHECK_STREQ(c.timezone, "PST8PDT,M3.2.0,M11.1.0");
  }

  // Non-empty timezone → preserved
  {
    device_config c{};
    snprintf(c.timezone, sizeof(c.timezone), "EST5EDT,M3.2.0,M11.1.0");
    config_validate(c);
    CHECK_STREQ(c.timezone, "EST5EDT,M3.2.0,M11.1.0");
  }
}

static void test_legacy_weather_pairs() {
  suite_begin("apply_legacy_weather_pair");

  device_config c{};
  config_apply_legacy_weather_pair(c, "units", "metric");
  CHECK_STREQ(c.weather_units, "metric");

  config_apply_legacy_weather_pair(c, "lat", "37.7300");
  CHECK_NEAR(c.weather_lat, 37.73, 1e-9);

  config_apply_legacy_weather_pair(c, "lon", "-122.1300");
  CHECK_NEAR(c.weather_lon, -122.13, 1e-9);

  // Unknown key must be silently ignored
  config_apply_legacy_weather_pair(c, "unknown_key", "value");
  CHECK_STREQ(c.weather_units, "metric");   // unchanged
}

static void test_legacy_wifi_pairs() {
  suite_begin("apply_legacy_wifi_pair");

  device_config c{};
  config_apply_legacy_wifi_pair(c, "ssid", "MyNetwork");
  CHECK_STREQ(c.wifi_ssid, "MyNetwork");

  config_apply_legacy_wifi_pair(c, "psk", "s3cr3t");
  CHECK_STREQ(c.wifi_psk, "s3cr3t");

  // Unknown key silently ignored
  config_apply_legacy_wifi_pair(c, "unknown", "value");
  CHECK_STREQ(c.wifi_ssid, "MyNetwork");  // unchanged
}

static void test_parse_kv_file() {
  suite_begin("config_parse_kv_file");

  const char *path = "/tmp/nowtube_test_kv.txt";

  // --- Basic parsing ---
  write_file(path,
    "# comment line\n"
    "\n"
    "  # indented comment\n"
    "key1=value1\n"
    "key2 = value2\n"        // space around =
    "  key3=value3\n"        // leading whitespace on key
    "key4=\n"                // empty value
    "no_equals\n"            // no = sign — skipped
  );

  struct Pair { char key[64]; char value[64]; };
  static Pair pairs[16];
  static int  pair_count;
  pair_count = 0;

  config_parse_kv_file(path, [](const char *k, const char *v, void * /*ud*/) {
    if (pair_count < 16) {
      snprintf(pairs[pair_count].key,   64, "%s", k);
      snprintf(pairs[pair_count].value, 64, "%s", v);
      pair_count++;
    }
  }, nullptr);

  CHECK(pair_count == 4);
  CHECK_STREQ(pairs[0].key,   "key1");
  CHECK_STREQ(pairs[0].value, "value1");
  CHECK_STREQ(pairs[1].key,   "key2");
  CHECK_STREQ(pairs[1].value, "value2");    // leading space from value stripped
  CHECK_STREQ(pairs[2].key,   "key3");
  CHECK_STREQ(pairs[2].value, "value3");
  CHECK_STREQ(pairs[3].key,   "key4");
  CHECK_STREQ(pairs[3].value, "");          // empty value is fine

  // --- File not found ---
  bool ok = config_parse_kv_file("/tmp/nowtube_nonexistent.txt",
                                 [](const char *, const char *, void *) {}, nullptr);
  CHECK(ok == false);
}

static void test_migrate_legacy_full() {
  suite_begin("config_migrate_legacy — full files");

  const char *wp = "/tmp/nowtube_test_weather.txt";
  const char *wf = "/tmp/nowtube_test_wifi.txt";

  write_file(wp,
    "# OWM config\n"
    "units=imperial\n"
    "lat=45.5200\n"
    "lon=-122.6800\n"
  );
  write_file(wf,
    "ssid=TestNetwork\n"
    "psk=s3cr3t!\n"
  );

  device_config c{};
  bool ok = config_migrate_legacy(c, wp, wf);

  CHECK(ok == true);
  CHECK_STREQ(c.weather_units,   "imperial");
  CHECK_NEAR (c.weather_lat,  45.52,   1e-9);
  CHECK_NEAR (c.weather_lon, -122.68,  1e-9);
  CHECK(c.has_weather_location == true);
  CHECK_STREQ(c.wifi_ssid, "TestNetwork");
  CHECK_STREQ(c.wifi_psk,  "s3cr3t!");
}

static void test_migrate_legacy_lat_only() {
  suite_begin("config_migrate_legacy — lat without lon");

  const char *wp = "/tmp/nowtube_test_weather_latonly.txt";
  write_file(wp, "lat=45.52\n");

  device_config c{};
  config_migrate_legacy(c, wp, "/tmp/nowtube_nonexistent_wifi.txt");

  // Only lat — must NOT set has_weather_location
  CHECK(c.has_weather_location == false);
  CHECK_NEAR(c.weather_lat, 45.52, 1e-9);
}

static void test_migrate_legacy_lon_only() {
  suite_begin("config_migrate_legacy — lon without lat");

  const char *wp = "/tmp/nowtube_test_weather_lononly.txt";
  write_file(wp, "lon=-122.68\n");

  device_config c{};
  config_migrate_legacy(c, wp, "/tmp/nowtube_nonexistent_wifi.txt");

  CHECK(c.has_weather_location == false);
  CHECK_NEAR(c.weather_lon, -122.68, 1e-9);
}

static void test_migrate_legacy_no_files() {
  suite_begin("config_migrate_legacy — neither file exists");

  device_config c{};
  bool ok = config_migrate_legacy(c, "/tmp/no_weather.txt", "/tmp/no_wifi.txt");

  CHECK(ok == false);
  // Config must remain at defaults
  CHECK(c.wifi_ssid[0]         == '\0');
  CHECK(c.has_weather_location == false);
}

static void test_migrate_legacy_whitespace_and_comments() {
  suite_begin("config_migrate_legacy — whitespace and comment handling");

  const char *wp = "/tmp/nowtube_test_weather_ws.txt";
  write_file(wp,
    "# leading comment\n"
    "\n"
    "  units  =  metric  \n"   // spaces around key and value
    "# trailing comment\n"
  );

  device_config c{};
  config_migrate_legacy(c, wp, "/tmp/nowtube_nonexistent.txt");

  // Key is trimmed; value leading whitespace trimmed (trailing is not, per parser design)
  CHECK_STREQ(c.weather_units, "metric  ");  // trailing value whitespace NOT trimmed by parser
}

static void test_cycle_dwell_clamping() {
  suite_begin("cycle dwell clamping");

  // Defaults survive validate unchanged
  {
    device_config c{};
    config_validate(c);
    CHECK(c.cycle_clock_s    == 50);
    CHECK(c.cycle_today_s    == 10);
    CHECK(c.cycle_forecast_s == 10);
  }

  // Below minimum → clamped to 5
  {
    device_config c{};
    c.cycle_clock_s = 0; c.cycle_today_s = 1; c.cycle_forecast_s = 4;
    config_validate(c);
    CHECK(c.cycle_clock_s    == 5);
    CHECK(c.cycle_today_s    == 5);
    CHECK(c.cycle_forecast_s == 6);  // clamped to 5 then rounded up to even
  }

  // Above maximum → clamped to 300
  {
    device_config c{};
    c.cycle_clock_s = 301; c.cycle_today_s = 1000; c.cycle_forecast_s = 65535;
    config_validate(c);
    CHECK(c.cycle_clock_s    == 300);
    CHECK(c.cycle_today_s    == 300);
    CHECK(c.cycle_forecast_s == 300);
  }

  // Boundary values kept as-is
  {
    device_config c{};
    c.cycle_clock_s = 5; c.cycle_today_s = 300; c.cycle_forecast_s = 120;
    config_validate(c);
    CHECK(c.cycle_clock_s    == 5);
    CHECK(c.cycle_today_s    == 300);
    CHECK(c.cycle_forecast_s == 120);
  }

}

static void test_forecast_dwell_even_normalization() {
  suite_begin("forecast dwell even normalization");

  // 0 = skip — must stay 0
  {
    device_config c{};
    c.cycle_forecast_s = 0;
    config_validate(c);
    CHECK(c.cycle_forecast_s == 0);
  }

  // Already even — kept
  {
    device_config c{};
    c.cycle_forecast_s = 10;
    config_validate(c);
    CHECK(c.cycle_forecast_s == 10);
  }

  // Odd → rounds up to next even
  {
    device_config c{};
    c.cycle_forecast_s = 11;
    config_validate(c);
    CHECK(c.cycle_forecast_s == 12);
  }

  // Odd minimum after clamp: 5 → 6
  {
    device_config c{};
    c.cycle_forecast_s = 5;
    config_validate(c);
    CHECK(c.cycle_forecast_s == 6);
  }

  // Odd below minimum: 3 → clamped to 5 → rounds up to 6
  {
    device_config c{};
    c.cycle_forecast_s = 3;
    config_validate(c);
    CHECK(c.cycle_forecast_s == 6);
  }

  // Max boundary: 300 is even — kept
  {
    device_config c{};
    c.cycle_forecast_s = 300;
    config_validate(c);
    CHECK(c.cycle_forecast_s == 300);
  }

  // Odd near max: 299 → 300
  {
    device_config c{};
    c.cycle_forecast_s = 299;
    config_validate(c);
    CHECK(c.cycle_forecast_s == 300);
  }

  // TODAY dwell is NOT even-normalized — odd value kept
  {
    device_config c{};
    c.cycle_today_s = 11;
    config_validate(c);
    CHECK(c.cycle_today_s == 11);
  }
}

static void test_validate_trims_before_normalisation() {
  suite_begin("validate trims whitespace before normalisation");

  // Whitespace around a valid units string must not cause fallback to "imperial"
  device_config c{};
  snprintf(c.weather_units, sizeof(c.weather_units), "  metric  ");
  config_validate(c);
  CHECK_STREQ(c.weather_units, "metric");

  // Same for timezone
  snprintf(c.timezone, sizeof(c.timezone), "  UTC  ");
  config_validate(c);
  CHECK_STREQ(c.timezone, "UTC");

  // Empty after trim → default timezone applied
  snprintf(c.timezone, sizeof(c.timezone), "   ");
  config_validate(c);
  CHECK_STREQ(c.timezone, "PST8PDT,M3.2.0,M11.1.0");
}

// ---------------------------------------------------------------------------

static void test_panel_humidity_metric_normalization() {
  suite_begin("panel_humidity_metric");

  // Default is "humidity".
  device_config c{};
  config_validate(c);
  CHECK_STREQ(c.panel_humidity_metric, "humidity");

  // "aqi" is accepted.
  snprintf(c.panel_humidity_metric, sizeof(c.panel_humidity_metric), "aqi");
  config_validate(c);
  CHECK_STREQ(c.panel_humidity_metric, "aqi");

  // Unknown values fall back to "humidity".
  snprintf(c.panel_humidity_metric, sizeof(c.panel_humidity_metric), "co2");
  config_validate(c);
  CHECK_STREQ(c.panel_humidity_metric, "humidity");

  // Leading/trailing whitespace is trimmed before normalization.
  snprintf(c.panel_humidity_metric, sizeof(c.panel_humidity_metric), " aqi ");
  config_validate(c);
  CHECK_STREQ(c.panel_humidity_metric, "aqi");

  // Empty string falls back to "humidity".
  c.panel_humidity_metric[0] = '\0';
  config_validate(c);
  CHECK_STREQ(c.panel_humidity_metric, "humidity");
}

int main() {
  test_defaults();
  test_trim_string();
  test_trim_nullptr();
  test_units_normalisation();
  test_boot_mode_validation();
  test_brightness_clamping();
  test_latlon_validation();
  test_default_timezone();
  test_legacy_weather_pairs();
  test_legacy_wifi_pairs();
  test_parse_kv_file();
  test_migrate_legacy_full();
  test_migrate_legacy_lat_only();
  test_migrate_legacy_lon_only();
  test_migrate_legacy_no_files();
  test_migrate_legacy_whitespace_and_comments();
  test_cycle_dwell_clamping();
  test_forecast_dwell_even_normalization();
  test_validate_trims_before_normalisation();
  test_panel_humidity_metric_normalization();

  printf("\n=== %d passed, %d failed ===\n", s_pass, s_fail);
  return s_fail > 0 ? 1 : 0;
}
