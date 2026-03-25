// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "conditions_parser.h"

#include <cJSON.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// URL builder

bool conditions_build_url(char *buf, size_t buf_size,
                          double lat, double lon,
                          const char *temperature_unit,
                          const char *wind_speed_unit) {
    int n = snprintf(buf, buf_size,
                     "https://api.open-meteo.com/v1/forecast"
                     "?latitude=%.6f&longitude=%.6f"
                     "&current=temperature_2m,relative_humidity_2m"
                     ",wind_speed_10m,wind_direction_10m,weathercode"
                     "&daily=sunrise,sunset"
                     "&forecast_days=1"
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

// ---------------------------------------------------------------------------
// Response parsing

conditions_parse_result conditions_parse_response(const char *json, size_t json_len) {
    conditions_parse_result r{};
    r.status = ConditionsParseStatus::JSON_PARSE_ERROR;

    if (!json || json_len == 0) return r;

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) return r;

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!current) {
        cJSON_Delete(root);
        r.status = ConditionsParseStatus::MISSING_CURRENT;
        return r;
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
        r.status = ConditionsParseStatus::MISSING_FIELDS;
        return r;
    }

    r.temp       = (int16_t)lround(temp_item->valuedouble);
    r.humidity   = (uint8_t)humidity_item->valueint;
    r.wind_speed = (int16_t)lround(wind_spd_item->valuedouble);
    uint32_t deg_raw = (uint32_t)lround(wind_dir_item->valuedouble);
    r.wind_deg   = (uint16_t)(deg_raw % 360);
    if (cJSON_IsNumber(wcode_item))
        r.weather_code = (uint16_t)wcode_item->valueint;

    // Optional: today's sunrise/sunset from daily[0].
    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (cJSON_IsObject(daily)) {
        cJSON *sunrises = cJSON_GetObjectItemCaseSensitive(daily, "sunrise");
        cJSON *sunsets  = cJSON_GetObjectItemCaseSensitive(daily, "sunset");
        if (cJSON_IsArray(sunrises) && cJSON_GetArraySize(sunrises) > 0) {
            cJSON *item = cJSON_GetArrayItem(sunrises, 0);
            if (cJSON_IsString(item))
                r.sunrise_min = parse_iso_time_minutes(item->valuestring);
        }
        if (cJSON_IsArray(sunsets) && cJSON_GetArraySize(sunsets) > 0) {
            cJSON *item = cJSON_GetArrayItem(sunsets, 0);
            if (cJSON_IsString(item))
                r.sunset_min = parse_iso_time_minutes(item->valuestring);
        }
    }

    cJSON_Delete(root);
    r.status = ConditionsParseStatus::OK;
    return r;
}
