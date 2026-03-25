// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "displays/forecast/forecast_display.h"

#include <cstdio>
#include <lvgl.h>

#include "displays/clock/clock.h"
#include "drivers/lcds.h"
#include "services/config_service.h"
#include "weather_parser.h"
#include "gui.h"

LV_FONT_DECLARE(space_mono_44)

namespace forecast_display {

// ---------------------------------------------------------------------------
// Colors and row geometry

static const lv_color_t COLOR_DAY  = lv_color_hex(0xFCF9D9);  // cream
static const lv_color_t COLOR_HIGH = lv_color_hex(0xFF8C00);   // orange
static const lv_color_t COLOR_LOW  = lv_color_hex(0x00BFFF);   // cyan

// line_height = 40 px at space_mono_44 44 pt.
// Day row centred in the top half; HI/LO rows aligned to match TODAY's
// bottom-pinned 2-line blocks (BOTTOM_MID -10):
//   label_top = 162-10-2*40 = 72px → line centers at 92px (+11) and 132px (+51).
static constexpr int ROW_DAY_Y  = -52;
static constexpr int ROW_HIGH_Y = +11;
static constexpr int ROW_LOW_Y  = +51;

// Panel 0 key rows — same alignment as the data rows above.
static constexpr int ROW_KEY_HI_Y = +11;
static constexpr int ROW_KEY_LO_Y = +51;

// ---------------------------------------------------------------------------
// WMO 4677 code → condition icon path (shared with today_display)

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
// LVGL object state

static lv_obj_t *s_forecast_icon        = nullptr;
static lv_obj_t *s_day_lbl[NUM_LCDS]   = {};
static lv_obj_t *s_hi_lbl[NUM_LCDS]    = {};
static lv_obj_t *s_lo_lbl[NUM_LCDS]    = {};
static lv_obj_t *s_cond_icon[NUM_LCDS] = {};  // per-day condition icons (panels 1-5)

static lv_timer_t  *s_phase_timer   = nullptr;
static forecast_data s_data_snapshot = {};

// ---------------------------------------------------------------------------
// Helpers

static lv_obj_t *make_row(int panel, const char *text,
                           lv_color_t color, int y_offset) {
    lv_disp_set_default(gui_get_display(panel));
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, &space_mono_44, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LCD_WIDTH + 2);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y_offset);
    return label;
}

static void update_row(lv_obj_t *label, const char *text, int y_offset) {
    if (!label) return;
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y_offset);
}

// ---------------------------------------------------------------------------
// Content

static void build_panel_rows(int panel, const forecast_data &data,
                              char *day_buf, size_t day_size,
                              char *hi_buf,  size_t hi_size,
                              char *lo_buf,  size_t lo_size) {
    if (panel == 0) {
        // Legend: blank day row so HI/LO align with the temp rows.
        day_buf[0] = '\0';
        snprintf(hi_buf, hi_size, "HI");
        snprintf(lo_buf, lo_size, "LO");
        return;
    }

    int day_idx = panel - 1;
    if (day_idx >= FORECAST_DAYS || !data.valid || !data.days[day_idx].valid) {
        day_buf[0] = '\0';
        snprintf(hi_buf, hi_size, "--");
        snprintf(lo_buf, lo_size, "--");
        return;
    }

    const forecast_day &d = data.days[day_idx];
    snprintf(day_buf, day_size, "%s", forecast_day_code(d.wday));
    snprintf(hi_buf,  hi_size,  "%d", d.high);
    snprintf(lo_buf,  lo_size,  "%d", d.low);
}

// ---------------------------------------------------------------------------
// Phase timer — fires once at dwell/2, swapping day codes to condition icons

static void phase_timer_cb(lv_timer_t * /*timer*/) {
    s_phase_timer = nullptr;  // auto-deleted (repeat_count=1)

    for (int i = 1; i < NUM_LCDS; i++) {
        int day_idx = i - 1;
        if (day_idx >= FORECAST_DAYS) continue;
        if (!s_data_snapshot.valid || !s_data_snapshot.days[day_idx].valid) continue;

        if (s_day_lbl[i]) lv_obj_add_flag(s_day_lbl[i], LV_OBJ_FLAG_HIDDEN);
        if (s_cond_icon[i]) {
            lv_img_set_src(s_cond_icon[i],
                           condition_icon_src(s_data_snapshot.days[day_idx].weather_code));
            lv_obj_clear_flag(s_cond_icon[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API

void show(const forecast_data &data) {
    char day[8], hi[8], lo[8];

    if (s_day_lbl[0] != nullptr) {
        // Update path: refresh labels and icon sources.
        s_data_snapshot = data;
        for (int i = 0; i < NUM_LCDS; i++) {
            build_panel_rows(i, data,
                             day, sizeof(day),
                             hi,  sizeof(hi),
                             lo,  sizeof(lo));
            if (i == 0) {
                update_row(s_hi_lbl[0], hi, ROW_KEY_HI_Y);
                update_row(s_lo_lbl[0], lo, ROW_KEY_LO_Y);
            } else {
                update_row(s_day_lbl[i], day, ROW_DAY_Y);
                update_row(s_hi_lbl[i],  hi,  ROW_HIGH_Y);
                update_row(s_lo_lbl[i],  lo,  ROW_LOW_Y);
                int day_idx = i - 1;
                if (s_cond_icon[i] && day_idx < FORECAST_DAYS &&
                    data.valid && data.days[day_idx].valid) {
                    lv_img_set_src(s_cond_icon[i],
                                   condition_icon_src(data.days[day_idx].weather_code));
                }
            }
        }
        return;
    }

    clock::get().hide_all();
    s_data_snapshot = data;

    for (int i = 0; i < NUM_LCDS; i++) {
        build_panel_rows(i, data,
                         day, sizeof(day),
                         hi,  sizeof(hi),
                         lo,  sizeof(lo));
        if (i == 0) {
            // Icon at top; HI/LO key shifted down to match TODAY wind panel rows.
            lv_disp_set_default(gui_get_display(0));
            s_forecast_icon = lv_img_create(lv_scr_act());
            lv_img_set_src(s_forecast_icon, "S:/spiffs/forecast.png");
            lv_obj_align(s_forecast_icon, LV_ALIGN_TOP_MID, 0, 2);
            s_day_lbl[0] = make_row(0, day, COLOR_DAY,  ROW_DAY_Y);  // empty, guards clear()
            s_hi_lbl[0]  = make_row(0, hi,  COLOR_HIGH, ROW_KEY_HI_Y);
            s_lo_lbl[0]  = make_row(0, lo,  COLOR_LOW,  ROW_KEY_LO_Y);
        } else {
            s_day_lbl[i] = make_row(i, day, COLOR_DAY,  ROW_DAY_Y);
            s_hi_lbl[i]  = make_row(i, hi,  COLOR_HIGH, ROW_HIGH_Y);
            s_lo_lbl[i]  = make_row(i, lo,  COLOR_LOW,  ROW_LOW_Y);

            // Condition icon: created hidden; shown by phase timer.
            lv_disp_set_default(gui_get_display(i));
            s_cond_icon[i] = lv_img_create(lv_scr_act());
            lv_obj_align(s_cond_icon[i], LV_ALIGN_TOP_MID, 0, 2);
            lv_obj_add_flag(s_cond_icon[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Schedule phase swap at half the FORECAST dwell (even-normalized by config_validate).
    uint16_t dwell_s = config_service::get_config().cycle_forecast_s;
    if (dwell_s >= 2) {
        s_phase_timer = lv_timer_create(phase_timer_cb,
                                        (uint32_t)(dwell_s / 2) * 1000u,
                                        nullptr);
        lv_timer_set_repeat_count(s_phase_timer, 1);
    }
}

void clear() {
    if (s_day_lbl[0] == nullptr) return;

    if (s_phase_timer) { lv_timer_del(s_phase_timer); s_phase_timer = nullptr; }
    if (s_forecast_icon) { lv_obj_del(s_forecast_icon); s_forecast_icon = nullptr; }
    for (int i = 0; i < NUM_LCDS; i++) {
        if (s_day_lbl[i])  { lv_obj_del(s_day_lbl[i]);  s_day_lbl[i]  = nullptr; }
        if (s_hi_lbl[i])   { lv_obj_del(s_hi_lbl[i]);   s_hi_lbl[i]   = nullptr; }
        if (s_lo_lbl[i])   { lv_obj_del(s_lo_lbl[i]);   s_lo_lbl[i]   = nullptr; }
        if (s_cond_icon[i]){ lv_obj_del(s_cond_icon[i]); s_cond_icon[i] = nullptr; }
    }

    clock::get().restore_all();
    clock::get().update();
}

}  // namespace forecast_display
