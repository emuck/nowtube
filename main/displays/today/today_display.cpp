// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "displays/today/today_display.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <lvgl.h>

#include "displays/clock/clock.h"
#include "drivers/lcds.h"
#include "gui.h"
#include "services/config_service.h"
#include "weather_parser.h"

LV_FONT_DECLARE(space_mono_44)
LV_FONT_DECLARE(space_mono_54)

namespace today_display {

// ---------------------------------------------------------------------------
// Style

static const lv_color_t TEXT_COLOR = lv_color_hex(0xFCF9D9);

// ---------------------------------------------------------------------------
// Panel layout
//
//  0  weekday   TUE → T/U/E  (stacked, space_mono_44 44 px)
//  1  month     MAR → M/A/R  (stacked)
//  2  day       icon top / day number bottom
//  3  wind      icon top / value+direction bottom
//  4  humidity  39% → 39/%   (value, newline, % sign)
//  5  sun time        7/-/23  (stacked H / - / MM, 24 h)
//
// Font: Space Mono 44 px (space_mono_44), line_height = 40 px.
// Stacked labels: LV_ALIGN_CENTER — 3-char label = 120 px, centres in 162 px.
// Top label (panel 2): LV_ALIGN_TOP_MID, y_offset = 4 px.
// Wind icon (panel 3): LV_ALIGN_TOP_MID, y_offset = 2 px (64×64); label bottom-pinned.

// ---------------------------------------------------------------------------
// LVGL object state

static lv_obj_t *s_lbl[NUM_LCDS]  = {};
static lv_obj_t *s_condition_icon  = nullptr;
static lv_obj_t *s_wind_icon       = nullptr;
static lv_obj_t *s_drop_icon       = nullptr;
static lv_obj_t *s_sunset_icon     = nullptr;

// ---------------------------------------------------------------------------
// Static tables

static const char * const MONTHS[] = {
    "JAN","FEB","MAR","APR","MAY","JUN",
    "JUL","AUG","SEP","OCT","NOV","DEC"
};
static const char * const WEEKDAYS[] = {
    "SUN","MON","TUE","WED","THU","FRI","SAT"
};

// ---------------------------------------------------------------------------
// WMO weather code → condition icon path

// WMO 4677 codes used by Open-Meteo:
//   0–2   clear / mainly clear           → sunny
//   3     overcast                        → cloudy
//   45,48 fog                             → cloudy
//   51–67 drizzle / freezing / rain       → rainy
//   71–77 snow                            → snowy
//   80–82 rain showers                    → rainy
//   85–86 snow showers                    → snowy
//   95–99 thunderstorm                    → rainy
static const char *condition_icon_src(uint16_t code) {
    if (code == 0 || code == 1)                         return "S:/spiffs/sunny.png";
    if (code == 2)                                      return "S:/spiffs/pcloudy.png";
    if (code == 3)                                      return "S:/spiffs/cloudy.png";
    if (code == 45 || code == 48)                       return "S:/spiffs/foggy.png";
    if ((code >= 71 && code <= 77) ||
        (code >= 85 && code <= 86))                     return "S:/spiffs/snowy.png";
    if (code >= 95)                                     return "S:/spiffs/thunder.png";
    return "S:/spiffs/rainy.png";
}

// ---------------------------------------------------------------------------
// Helpers

// Convert each non-space character to its own line.
// "7NE" → "7\nN\nE"   "40%" → "4\n0\n%"
static void stack_chars(char *dst, size_t dst_size, const char *src) {
    size_t out = 0;
    bool first = true;
    for (size_t i = 0; src[i] != '\0'; i++) {
        if (src[i] == ' ') continue;
        if (!first && out + 1 < dst_size) dst[out++] = '\n';
        if (out + 1 < dst_size) dst[out++] = src[i];
        first = false;
    }
    dst[out] = '\0';
}

// Stacked label: characters centred vertically in the panel.
static lv_obj_t *make_stacked(int panel, const char *text) {
    lv_disp_set_default(gui_get_display(panel));
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, &space_mono_54, 0);
    lv_obj_set_style_text_color(label, TEXT_COLOR, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LCD_WIDTH + 2);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    return label;
}

// Bottom-pinned label: used for wind panel (panel 3) when icon occupies top.
static lv_obj_t *make_bottom(int panel, const char *text) {
    lv_disp_set_default(gui_get_display(panel));
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, &space_mono_44, 0);
    lv_obj_set_style_text_color(label, TEXT_COLOR, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LCD_WIDTH + 2);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);
    return label;
}

static void update_label(lv_obj_t *label, const char *text, bool bottom = false) {
    if (!label) return;
    lv_label_set_text(label, text);
    if (bottom) {
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);
    } else {
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// AQI color coding — EPA standard breakpoints (airnow.gov/aqi/aqi-basics/)

static const char *aqi_color_hex(int aqi) {
    if (aqi <=  50) return "00E400";  // Good             — green
    if (aqi <= 100) return "FFFF00";  // Moderate         — yellow
    if (aqi <= 150) return "FF7E00";  // Sensitive groups — orange
    if (aqi <= 200) return "FF0000";  // Unhealthy        — red
    if (aqi <= 300) return "8F3F97";  // Very unhealthy   — purple
    return                 "7E0023";  // Hazardous        — maroon
}

// ---------------------------------------------------------------------------
// Content

static void build_panel_text(int panel, const current_conditions &data, bool use_aqi,
                              char *buf, size_t buf_size) {
    time_t now = time(nullptr);
    struct tm tm {};
    localtime_r(&now, &tm);

    switch (panel) {
    case 0:
        stack_chars(buf, buf_size, WEEKDAYS[tm.tm_wday]);
        break;

    case 1:
        stack_chars(buf, buf_size, MONTHS[tm.tm_mon]);
        break;

    case 2:
        snprintf(buf, buf_size, "%d", tm.tm_mday);
        break;

    case 3:
        if (data.valid) {
            snprintf(buf, buf_size, "%d\n%s",
                     data.wind_speed, wind_direction_abbr(data.wind_deg));
        } else {
            snprintf(buf, buf_size, "--");
        }
        break;

    case 4:
        if (use_aqi) {
            if (data.valid && data.aqi_valid) {
                snprintf(buf, buf_size, "%d\n#%s AQI#",
                         data.us_aqi, aqi_color_hex(data.us_aqi));
            } else {
                snprintf(buf, buf_size, "--");
            }
        } else {
            if (data.valid) {
                snprintf(buf, buf_size, "%u\n%%", data.humidity);
            } else {
                snprintf(buf, buf_size, "--");
            }
        }
        break;

    case 5: {
        // Show the next sun event as stacked H / : / MM groups (24 h).
        // Three lines = 126 px, centred in 162 px panel.
        uint16_t cur_min = static_cast<uint16_t>(tm.tm_hour * 60 + tm.tm_min);
        bool has_sun = data.valid && (data.sunrise_min > 0 || data.sunset_min > 0);
        if (!has_sun) {
            snprintf(buf, buf_size, "--\n--");
        } else {
            uint16_t event_min;
            if (cur_min < data.sunrise_min) {
                event_min = data.sunrise_min;       // before sunrise
            } else if (cur_min < data.sunset_min) {
                event_min = data.sunset_min;        // between sunrise and sunset
            } else {
                event_min = data.sunrise_min;       // after sunset: tomorrow's (today's as proxy)
            }
            uint16_t h12 = event_min / 60 % 12;
            if (h12 == 0) h12 = 12;
            snprintf(buf, buf_size, "%u:\n%02u", h12, event_min % 60);
        }
        break;
    }

    default:
        buf[0] = '\0';
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API

void show(const current_conditions &data) {
    char buf[32];
    bool use_aqi = strcmp(config_service::get_config().panel_humidity_metric, "aqi") == 0;

    if (s_lbl[0] != nullptr) {
        for (int i = 0; i < NUM_LCDS; i++) {
            build_panel_text(i, data, use_aqi, buf, sizeof(buf));
            if (i == 2) {
                lv_label_set_text(s_lbl[i], buf);
                lv_obj_align(s_lbl[i], LV_ALIGN_BOTTOM_MID, 0, -31);
            } else {
                update_label(s_lbl[i], buf, /*bottom=*/i >= 3);
            }
        }
        if (s_condition_icon)
            lv_img_set_src(s_condition_icon, condition_icon_src(data.weather_code));
        if (s_drop_icon)
            lv_img_set_src(s_drop_icon, use_aqi ? "S:/spiffs/air.png" : "S:/spiffs/drop.png");
        return;
    }

    clock::get().hide_all();

    for (int i = 0; i < NUM_LCDS; i++) {
        build_panel_text(i, data, use_aqi, buf, sizeof(buf));
        if (i == 2) {
            // Condition panel: icon at top, day number at bottom.
            lv_disp_set_default(gui_get_display(i));
            s_condition_icon = lv_img_create(lv_scr_act());
            lv_img_set_src(s_condition_icon, condition_icon_src(data.weather_code));
            lv_obj_align(s_condition_icon, LV_ALIGN_TOP_MID, 0, 0);
            s_lbl[i] = make_bottom(i, buf);
            lv_obj_align(s_lbl[i], LV_ALIGN_BOTTOM_MID, 0, -31);
        } else if (i == 3) {
            // Wind panel: icon at top, text at bottom.
            lv_disp_set_default(gui_get_display(i));
            s_wind_icon = lv_img_create(lv_scr_act());
            lv_img_set_src(s_wind_icon, "S:/spiffs/wind.png");
            lv_obj_align(s_wind_icon, LV_ALIGN_TOP_MID, 0, 0);
            s_lbl[i] = make_bottom(i, buf);
        } else if (i == 4) {
            // Humidity / AQI panel: icon at top, text at bottom.
            lv_disp_set_default(gui_get_display(i));
            s_drop_icon = lv_img_create(lv_scr_act());
            lv_img_set_src(s_drop_icon, use_aqi ? "S:/spiffs/air.png" : "S:/spiffs/drop.png");
            lv_obj_align(s_drop_icon, LV_ALIGN_TOP_MID, 0, 0);
            s_lbl[i] = make_bottom(i, buf);
            lv_label_set_recolor(s_lbl[i], true);  // enables #RRGGBB text# coloring
        } else if (i == 5) {
            // Sun event panel: icon at top, time at bottom.
            lv_disp_set_default(gui_get_display(i));
            s_sunset_icon = lv_img_create(lv_scr_act());
            lv_img_set_src(s_sunset_icon, "S:/spiffs/sunset.png");
            lv_obj_align(s_sunset_icon, LV_ALIGN_TOP_MID, 0, 0);
            s_lbl[i] = make_bottom(i, buf);
        } else {
            s_lbl[i] = make_stacked(i, buf);
        }
    }
}

void clear() {
    if (s_lbl[0] == nullptr) return;

    for (int i = 0; i < NUM_LCDS; i++) {
        if (s_lbl[i]) { lv_obj_del(s_lbl[i]); s_lbl[i] = nullptr; }
    }
    if (s_condition_icon) { lv_obj_del(s_condition_icon); s_condition_icon = nullptr; }
    if (s_wind_icon) { lv_obj_del(s_wind_icon); s_wind_icon = nullptr; }
    if (s_drop_icon)   { lv_obj_del(s_drop_icon);   s_drop_icon   = nullptr; }
    if (s_sunset_icon) { lv_obj_del(s_sunset_icon); s_sunset_icon = nullptr; }

    clock::get().restore_all();
    clock::get().update();
}

}  // namespace today_display
