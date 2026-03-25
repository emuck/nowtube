// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "weather_parser.h"

#include <cJSON.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

// ---------------------------------------------------------------------------
// URL construction

bool aqi_build_url(char *buf, size_t buf_size, double lat, double lon) {
    int n = snprintf(buf, buf_size,
                     "https://air-quality-api.open-meteo.com/v1/air-quality"
                     "?latitude=%.6f&longitude=%.6f&current=us_aqi",
                     lat, lon);
    return n > 0 && (size_t)n < buf_size;
}

bool weather_build_url(char *buf, size_t buf_size,
                       double lat, double lon,
                       const char *temperature_unit,
                       const char *wind_speed_unit) {
    int n = snprintf(buf, buf_size,
                     "https://api.open-meteo.com/v1/forecast"
                     "?latitude=%.6f&longitude=%.6f"
                     "&current=temperature_2m,relative_humidity_2m"
                     ",wind_speed_10m,wind_direction_10m,weathercode"
                     "&daily=temperature_2m_max,temperature_2m_min"
                     ",cloud_cover_mean,precipitation_probability_max,sunrise,sunset"
                     "&forecast_days=5"
                     "&temperature_unit=%s"
                     "&wind_speed_unit=%s"
                     "&timezone=auto",
                     lat, lon, temperature_unit, wind_speed_unit);
    return n > 0 && (size_t)n < buf_size;
}

// ---------------------------------------------------------------------------
// Utilities

const char *wind_direction_abbr(uint16_t deg) {
    static const char * const DIRS[] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW"
    };
    return DIRS[(deg + 22) / 45 % 8];
}

static const char * const DAY_CODES[7] = {
    "SU", "MO", "TU", "WE", "TH", "FR", "SA"
};

const char *forecast_day_code(int wday) {
    if (wday < 0 || wday > 6) return "--";
    return DAY_CODES[wday];
}

// ---------------------------------------------------------------------------
// Time helpers

// Parse ISO timestamp "2024-03-17T07:23" → 7*60+23 = 443 minutes since midnight.
static uint16_t parse_iso_time_minutes(const char *iso) {
    if (!iso) return 0;
    const char *t = strchr(iso, 'T');
    if (!t || strlen(t) < 6) return 0;
    t++;  // skip 'T' → now at "HH:MM"
    if (t[2] != ':') return 0;
    int h = (t[0] - '0') * 10 + (t[1] - '0');
    int m = (t[3] - '0') * 10 + (t[4] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59) return 0;
    return (uint16_t)(h * 60 + m);
}

// Parse "YYYY-MM-DD" → wday (0=Sun..6=Sat).  Returns -1 on error.
static int parse_wday(const char *date_str) {
    if (!date_str) return -1;
    int year = 0, month = 0, day = 0;
    if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) != 3) return -1;
    struct tm t = {};
    t.tm_year  = year - 1900;
    t.tm_mon   = month - 1;
    t.tm_mday  = day;
    t.tm_isdst = -1;
    time_t tt = mktime(&t);
    if (tt == (time_t)-1) return -1;
    return t.tm_wday;
}

// ---------------------------------------------------------------------------
// Response parsing

weather_parse_result weather_parse_response(const char *json,
                                            size_t json_len,
                                            const char *temp_unit_chr,
                                            const char *wind_unit_label) {
    weather_parse_result result = {};
    result.status = WeatherParseStatus::JSON_PARSE_ERROR;

    if (!json || json_len == 0) return result;

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) return result;

    // --- Current conditions ---
    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!current) {
        cJSON_Delete(root);
        result.status = WeatherParseStatus::MISSING_CURRENT;
        return result;
    }

    cJSON *temp_item     = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    cJSON *humidity_item = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
    cJSON *wind_spd_item = cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m");
    cJSON *wind_dir_item = cJSON_GetObjectItemCaseSensitive(current, "wind_direction_10m");
    cJSON *wcode_item    = cJSON_GetObjectItemCaseSensitive(current, "weathercode");

    if (!cJSON_IsNumber(temp_item)     ||
        !cJSON_IsNumber(humidity_item) ||
        !cJSON_IsNumber(wind_spd_item) ||
        !cJSON_IsNumber(wind_dir_item)) {
        cJSON_Delete(root);
        result.status = WeatherParseStatus::MISSING_FIELDS;
        return result;
    }

    current_conditions &cond = result.conditions;
    cond.temp_rounded = (int16_t)lround(temp_item->valuedouble);
    snprintf(cond.temp_value, sizeof(cond.temp_value), "%d", cond.temp_rounded);
    snprintf(cond.temp_unit,  sizeof(cond.temp_unit),  "%s", temp_unit_chr);
    cond.wind_speed = (int16_t)lround(wind_spd_item->valuedouble);
    uint32_t deg_raw = (uint32_t)lround(wind_dir_item->valuedouble);
    cond.wind_deg = (uint16_t)(deg_raw % 360);
    cond.humidity = (uint8_t)humidity_item->valueint;
    if (cJSON_IsNumber(wcode_item))
        cond.weather_code = (uint16_t)wcode_item->valueint;
    snprintf(cond.wind_unit, sizeof(cond.wind_unit), "%s", wind_unit_label);
    cond.valid      = true;
    cond.fetched_at = time(nullptr);

    // --- Daily: 5-day hi/lo forecast + today's sunrise/sunset ---
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!daily) {
        cJSON_Delete(root);
        result.status = WeatherParseStatus::MISSING_DAILY;
        return result;
    }

    cJSON *times       = cJSON_GetObjectItemCaseSensitive(daily, "time");
    cJSON *highs       = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    cJSON *lows        = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    cJSON *cloud_arr   = cJSON_GetObjectItemCaseSensitive(daily, "cloud_cover_mean");
    cJSON *precip_arr  = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");
    cJSON *sunrises    = cJSON_GetObjectItemCaseSensitive(daily, "sunrise");
    cJSON *sunsets     = cJSON_GetObjectItemCaseSensitive(daily, "sunset");

    if (!cJSON_IsArray(times) || !cJSON_IsArray(highs) || !cJSON_IsArray(lows)) {
        cJSON_Delete(root);
        result.status = WeatherParseStatus::MISSING_FIELDS;
        return result;
    }

    // Today's sunrise/sunset from the first daily entry.
    if (cJSON_IsArray(sunrises) && cJSON_GetArraySize(sunrises) > 0) {
        cJSON *item = cJSON_GetArrayItem(sunrises, 0);
        if (cJSON_IsString(item))
            cond.sunrise_min = parse_iso_time_minutes(item->valuestring);
    }
    if (cJSON_IsArray(sunsets) && cJSON_GetArraySize(sunsets) > 0) {
        cJSON *item = cJSON_GetArrayItem(sunsets, 0);
        if (cJSON_IsString(item))
            cond.sunset_min = parse_iso_time_minutes(item->valuestring);
    }

    // 5-day forecast.
    forecast_data &fc = result.forecast;
    int n = cJSON_GetArraySize(times);
    if (n > FORECAST_DAYS) n = FORECAST_DAYS;
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(times, i);
        cJSON *h = cJSON_GetArrayItem(highs, i);
        cJSON *l = cJSON_GetArrayItem(lows,  i);
        if (!t || !h || !l) continue;
        int wday = parse_wday(cJSON_GetStringValue(t));
        fc.days[i].high  = (int8_t)cJSON_GetNumberValue(h);
        fc.days[i].low   = (int8_t)cJSON_GetNumberValue(l);
        fc.days[i].wday  = (wday >= 0) ? (uint8_t)wday : 0;
        fc.days[i].valid = (wday >= 0);
        // Derive a WMO-like condition code from cloud_cover_mean and
        // precipitation_probability_max.  Open-Meteo's daily weathercode
        // aggregate is unreliable (often returns 3/overcast on clear days).
        {
            uint8_t cloud_pct  = 0;
            uint8_t precip_pct = 0;
            if (cJSON_IsArray(cloud_arr)) {
                cJSON *cv = cJSON_GetArrayItem(cloud_arr, i);
                if (cJSON_IsNumber(cv)) cloud_pct = (uint8_t)cv->valuedouble;
            }
            if (cJSON_IsArray(precip_arr)) {
                cJSON *pv = cJSON_GetArrayItem(precip_arr, i);
                if (cJSON_IsNumber(pv)) precip_pct = (uint8_t)pv->valuedouble;
            }
            if      (precip_pct >= 40) fc.days[i].weather_code = 61;  // → rainy
            else if (cloud_pct  >= 70) fc.days[i].weather_code = 3;   // → cloudy
            else if (cloud_pct  >= 40) fc.days[i].weather_code = 2;   // → partly cloudy
            else                       fc.days[i].weather_code = 0;   // → sunny
        }
    }
    fc.valid      = (n > 0);
    fc.fetched_at = time(nullptr);

    cJSON_Delete(root);
    result.status = WeatherParseStatus::OK;
    return result;
}
