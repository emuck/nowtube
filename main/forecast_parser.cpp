// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "forecast_parser.h"

#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <ctime>

// ---------------------------------------------------------------------------
// URL builder

bool forecast_build_url(char *buf, size_t buf_size,
                        double lat, double lon,
                        const char *temperature_unit) {
    int n = snprintf(buf, buf_size,
                     "https://api.open-meteo.com/v1/forecast"
                     "?latitude=%.6f&longitude=%.6f"
                     "&daily=temperature_2m_max,temperature_2m_min"
                     ",cloud_cover_mean,precipitation_probability_max"
                     "&forecast_days=5"
                     "&temperature_unit=%s"
                     "&timezone=auto",
                     lat, lon, temperature_unit);
    return n > 0 && (size_t)n < buf_size;
}

// ---------------------------------------------------------------------------
// Utilities

static const char * const DAY_CODES[7] = {
    "SU", "MO", "TU", "WE", "TH", "FR", "SA"
};

const char *forecast_day_code(int wday) {
    if (wday < 0 || wday > 6) return "--";
    return DAY_CODES[wday];
}

// Parse "YYYY-MM-DD" → wday (0=Sun..6=Sat). Returns -1 on error.
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

forecast_parse_result forecast_parse_response(const char *json, int json_len) {
    forecast_parse_result r{};
    r.status = ForecastParseStatus::JSON_PARSE_ERROR;

    if (!json || json_len <= 0) return r;

    cJSON *root = cJSON_ParseWithLength(json, (size_t)json_len);
    if (!root) return r;

    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!daily) {
        cJSON_Delete(root);
        r.status = ForecastParseStatus::MISSING_DAILY;
        return r;
    }

    cJSON *times      = cJSON_GetObjectItemCaseSensitive(daily, "time");
    cJSON *highs      = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    cJSON *lows       = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    cJSON *cloud_arr  = cJSON_GetObjectItemCaseSensitive(daily, "cloud_cover_mean");
    cJSON *precip_arr = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");

    if (!cJSON_IsArray(times) || !cJSON_IsArray(highs) || !cJSON_IsArray(lows) ||
        cJSON_GetArraySize(times) == 0) {
        cJSON_Delete(root);
        r.status = ForecastParseStatus::MISSING_FIELDS;
        return r;
    }

    int n = cJSON_GetArraySize(times);
    if (n > FORECAST_DAYS) n = FORECAST_DAYS;

    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(times, i);
        cJSON *h = cJSON_GetArrayItem(highs, i);
        cJSON *l = cJSON_GetArrayItem(lows,  i);
        if (!t || !h || !l) continue;
        int wday = parse_wday(cJSON_GetStringValue(t));
        r.days[i].high  = (int8_t)cJSON_GetNumberValue(h);
        r.days[i].low   = (int8_t)cJSON_GetNumberValue(l);
        r.days[i].wday  = (wday >= 0) ? (uint8_t)wday : 0;
        r.days[i].valid = (wday >= 0);
        // Derive condition code from cloud_cover_mean + precipitation_probability_max.
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
            if      (precip_pct >= 40) r.days[i].weather_code = 61;  // → rainy
            else if (cloud_pct  >= 70) r.days[i].weather_code = 3;   // → cloudy
            else if (cloud_pct  >= 40) r.days[i].weather_code = 2;   // → partly cloudy
            else                       r.days[i].weather_code = 0;   // → sunny
        }
    }

    r.count = n;
    cJSON_Delete(root);
    r.status = (n > 0) ? ForecastParseStatus::OK : ForecastParseStatus::MISSING_FIELDS;
    return r;
}
