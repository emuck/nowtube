//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "clock.h"
#include <lvgl.h>
#include <cmath>
#include <ctime>
#include <esp_heap_caps.h>

#include "drivers/lcds.h"
#include "font_theme.h"
#include "gui.h"
#include "services/config_service.h"

static constexpr int   MOON_SIZE     = 70;
static constexpr float MOON_RADIUS   = MOON_SIZE / 2.0f;
// LV_IMG_CF_TRUE_COLOR_ALPHA = RGB565 (2 bytes) + Alpha (1 byte) per pixel
static constexpr size_t MOON_BUF_BYTES = MOON_SIZE * MOON_SIZE * 3;

// Set true to render a phase test across all 6 panels at boot (debug only)
static constexpr bool MOON_TEST_MODE = false;

constexpr auto FLIP_SPACING_MS = 700;
// Set to true to always update clock digits/AM-PM instantly (no flapper). Use until display is solid.
static constexpr bool CLOCK_INSTANT_UPDATE = true;

const std::vector<std::string> digits_loop = {"",  ":", "0", "1", "2", "3",
                                              "4", "5", "6", "7", "8", "9"};
const std::vector<std::string> divider_loop = {"", ":"};

static const lv_color_t TEXT_COLOR = lv_color_hex(0xFCF9D9);
static const lv_color_t BG_COLOR = lv_color_hex(0x141414);

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

// Single 500ms timer: blink colon and check for next minute (keeps colon and time in sync)
constexpr unsigned CLOCK_TICK_MS = 500;
static constexpr size_t COLON_PANEL_INDEX = 2;  // display 2 = third panel (HH : MM)

static void clock_tick_callback(lv_timer_t *timer) {
  auto *instance = static_cast<class clock *>(timer->user_data);
  instance->clock_tick();
}

clock::clock() {
  const clock_theme &theme = font_theme_get(config_service::get_config().clock_font);

  for (int i = 0; i < NUM_LCDS; i++) {
    lv_disp_set_default(gui_get_display(i));
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, TEXT_COLOR, LV_PART_MAIN);

    background_images[i] = screen;
    flappers[i] = std::make_unique<flapper>(screen);

    if (i != NUM_LCDS - 1) {
      lv_obj_set_style_text_font(screen, theme.digit, LV_PART_MAIN);

      lv_obj_t *label = lv_label_create(screen);
      lv_label_set_text_static(label, "");
      lv_obj_set_width(label, LCD_WIDTH);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      digit_images[i] = label;
      lv_obj_align(label, LV_ALIGN_CENTER, 0, -7);

      if (i == 0) {
        // Weather icon: shown on panel 0 during daytime when first digit is '0'
        weather_icon_ = lv_img_create(screen);
        lv_img_set_src(weather_icon_, condition_icon_src(weather_code_));
        lv_obj_align(weather_icon_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(weather_icon_, LV_OBJ_FLAG_HIDDEN);

        // Moon canvas: shown on panel 0 at night when first digit is '0'
        moon_canvas_buf_ = heap_caps_malloc(MOON_BUF_BYTES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (moon_canvas_buf_) {
          moon_canvas_ = lv_canvas_create(screen);
          lv_canvas_set_buffer(moon_canvas_, moon_canvas_buf_,
                               MOON_SIZE, MOON_SIZE, LV_IMG_CF_TRUE_COLOR_ALPHA);
          lv_obj_align(moon_canvas_, LV_ALIGN_CENTER, 0, 0);
          lv_obj_add_flag(moon_canvas_, LV_OBJ_FLAG_HIDDEN);
        }
      }
    } else {
      ampm_image = lv_label_create(screen);
      lv_obj_set_style_text_font(ampm_image, theme.ampm, 0);
      lv_obj_set_style_text_color(ampm_image, TEXT_COLOR, 0);
      lv_obj_set_style_text_align(ampm_image, LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_long_mode(ampm_image, LV_LABEL_LONG_CLIP);
      lv_obj_set_width(ampm_image, LCD_WIDTH);
      lv_obj_align(ampm_image, LV_ALIGN_TOP_MID, 0, 13);
      lv_label_set_text_static(ampm_image, "");

      temp_label = lv_label_create(screen);
      lv_obj_set_style_text_font(temp_label, theme.temp, 0);
      lv_obj_set_style_text_color(temp_label, TEXT_COLOR, 0);
      lv_obj_set_style_text_align(temp_label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_width(temp_label, LCD_WIDTH + 16);
      lv_obj_set_pos(temp_label, -8, 96);
      lv_label_set_text_static(temp_label, "");

      wifi_status_icon = lv_label_create(screen);
      lv_label_set_text_static(wifi_status_icon, LV_SYMBOL_WIFI);
      lv_obj_set_style_text_font(wifi_status_icon, &lv_font_montserrat_24, 0);
      lv_obj_set_style_text_color(wifi_status_icon, lv_color_hex(0xD69C58), 0);
      lv_obj_align(wifi_status_icon, LV_ALIGN_CENTER, 26, 0);
    }
  }

  clock_update_timer = lv_timer_create(clock_tick_callback, CLOCK_TICK_MS, this);
  lv_timer_set_repeat_count(clock_update_timer, -1);

  if (MOON_TEST_MODE) show_moon_test();
}

void clock::set_wifi_connected(bool connected) {
  wifi_connected_ = connected;
  if (wifi_status_icon == nullptr) return;
  if (wifi_connected_) {
    lv_obj_add_flag(wifi_status_icon, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(wifi_status_icon, LV_OBJ_FLAG_HIDDEN);
  }
}

void clock::clock_tick() {
  colon_blink_tick();
  time_t now = time(nullptr);
  if (now >= next_update_time_) {
    update();
  }
}

void clock::reload_theme() {
  const clock_theme &theme = font_theme_get(config_service::get_config().clock_font);

  for (size_t i = 0; i < digit_images.size(); ++i) {
    lv_obj_set_style_text_font(background_images[i], theme.digit, LV_PART_MAIN);
  }
  if (ampm_image != nullptr) {
    lv_obj_set_style_text_font(ampm_image, theme.ampm, LV_PART_MAIN);
  }
  if (temp_label != nullptr) {
    lv_obj_set_style_text_font(temp_label, theme.temp, LV_PART_MAIN);
  }
}

void clock::colon_blink_tick() {
  lv_obj_t *colon_label = digit_images[COLON_PANEL_INDEX];
  if (lv_obj_has_flag(colon_label, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_clear_flag(colon_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(colon_label, LV_OBJ_FLAG_HIDDEN);
  }
}

void clock::update() {
  time_t now = 0;
  char strftime_buf[64];
  struct tm timeinfo {};

  time(&now);

#ifdef __MINGW32__
  localtime_s(&timeinfo, &now);
#else
  localtime_r(&now, &timeinfo);
#endif

  strftime(strftime_buf, sizeof(strftime_buf), "%I:%M%p", &timeinfo);

  // Instant path: set digits and AM/PM directly (no flapper). Used at first run and when CLOCK_INSTANT_UPDATE.
  if (!initialized_ || CLOCK_INSTANT_UPDATE) {
    char *p = strftime_buf;
    for (size_t j = 0; j < digit_images.size(); j++) {
      char text[2] = {*p++, '\0'};
      lv_label_set_text(digit_images[j], text);
    }
    update_panel0(strftime_buf[0] == '0');
    char ampm_buf[3] = {*p, 'M', '\0'};
    lv_label_set_text(ampm_image, ampm_buf);
    lv_obj_align(ampm_image, LV_ALIGN_TOP_MID, 0, 13);
    if (!initialized_) initialized_ = true;
    next_update_time_ = now + (60 - timeinfo.tm_sec);
    return;
  }

  // Flapper path: animate digit/AM-PM changes (when CLOCK_INSTANT_UPDATE is false).
  char *next_digit = strftime_buf;
  uint32_t delay = 0;
  size_t i = 0;

  for (; i < digit_images.size(); i++) {
    char digit = *next_digit;
    char text[2] = {digit, '\0'};
    if (digit != '\0') next_digit++;
    animate_panel(i, std::string(text), delay);
  }
  update_panel0(strftime_buf[0] == '0');

  char ampm_char = *next_digit;
  char *existing_ampm = lv_label_get_text(ampm_image);

  if (ampm_char != existing_ampm[0]) {
    auto *fp = flappers[i].get();
    fp->before();
    if (ampm_char == 'A') {
      lv_label_set_text_static(ampm_image, "AM");
    } else if (ampm_char == 'P') {
      lv_label_set_text_static(ampm_image, "PM");
    }
    lv_obj_align(ampm_image, LV_ALIGN_TOP_MID, 0, 13);
    fp->after();
    fp->start(true);
  }

  next_update_time_ = now + (60 - timeinfo.tm_sec);
}

void clock::delayed_start_flap_sequence(size_t index) {
  flap_sequences[index]->start();
  delayed_start_timers[index] = nullptr;
}

void clock::animate_panel(size_t i, const std::string &desired, uint32_t &delay) {
  lv_obj_t *digit_label = digit_images[i];
  char *existing_text = lv_label_get_text(digit_label);

  if (strcmp(existing_text, desired.c_str()) == 0) return;

  auto *fp = flappers[i].get();
  std::vector<std::string> values;

  auto existing_string = std::string(existing_text);
  auto &character_loop = (desired == ":") ? divider_loop : digits_loop;

  auto start_iter = std::find(character_loop.cbegin(), character_loop.cend(), existing_string);
  auto end_iter   = std::find(character_loop.cbegin(), character_loop.cend(), desired);

  LV_ASSERT(start_iter != character_loop.cend());
  LV_ASSERT(end_iter   != character_loop.cend());

  auto iter = start_iter;
  while (true) {
    if (++iter == character_loop.cend()) iter = character_loop.cbegin();
    values.push_back(*iter);
    if (iter == end_iter) break;
  }

  if (delayed_start_timers[i] != nullptr) {
    lv_timer_del(delayed_start_timers[i]);
    delayed_start_timers[i] = nullptr;
  }

  flap_sequences[i] = std::make_unique<flap_sequence>(
      fp,
      [digit_label](const std::string &value) {
        lv_label_set_text(digit_label, value.c_str());
      },
      values);

  struct timer_user_data { clock *this_; size_t index; };
  auto *t = lv_timer_create(
      [](lv_timer_t *timer) {
        auto *ud = static_cast<struct timer_user_data *>(timer->user_data);
        ud->this_->delayed_start_flap_sequence(ud->index);
        delete ud;
      },
      delay, new timer_user_data{this, i});
  lv_timer_set_repeat_count(t, 1);
  delayed_start_timers[i] = t;

  delay += FLIP_SPACING_MS;
}

void clock::show_value(const char *str) {
  if (!initialized_) return;
  show_value_instant(str);
}

void clock::show_value_instant(const char *str) {
  if (!initialized_) return;
  const char *p = str;
  for (size_t i = 0; i < digit_images.size(); i++) {
    char c = *p;
    if (c != '\0' && ((c >= '0' && c <= '9') || c == ':')) {
      char text[2] = {c, '\0'};
      lv_label_set_text(digit_images[i], text);
      p++;
    } else {
      lv_label_set_text(digit_images[i], "");
      if (c != '\0') p++;
    }
  }
  lv_label_set_text_static(ampm_image, "");
  lv_obj_align(ampm_image, LV_ALIGN_TOP_MID, 0, 13);
}

void clock::set_temp_static(const char *value) {
  if (!temp_label) return;
  char buf[8];
  snprintf(buf, sizeof(buf), "%s°", value);
  lv_label_set_text(temp_label, buf);
}

void clock::set_temp(const char *value, const char *unit) {
  if (!initialized_ || !temp_label) return;
  (void)unit;

  char buf[8];
  snprintf(buf, sizeof(buf), "%s°", value);

  const char *existing = lv_label_get_text(temp_label);
  if (strcmp(existing, buf) == 0) return;
  lv_label_set_text(temp_label, buf);
}

void clock::show_date() {
  time_t now = 0;
  struct tm timeinfo {};
  time(&now);
  localtime_r(&now, &timeinfo);

  char buf[8];
  strftime(buf, sizeof(buf), "%m:%d", &timeinfo);  // "MM:DD" e.g. "03:12"
  show_value(buf);
}

// ---------------------------------------------------------------------------
// Panel 0 mode management

float clock::current_moon_phase(time_t now) {
  // Reference new moon: Jan 6, 2000 18:14 UTC  (unix timestamp 947182440)
  constexpr double KNOWN_NEW_MOON  = 947182440.0;
  constexpr double SYNODIC_PERIOD  = 29.530588853 * 86400.0; // seconds
  double age = fmod((double)now - KNOWN_NEW_MOON, SYNODIC_PERIOD);
  if (age < 0) age += SYNODIC_PERIOD;
  return (float)(age / SYNODIC_PERIOD); // 0.0 = new moon, 0.5 = full moon
}

bool clock::is_nighttime() const {
  if (sunrise_min_ == 0 && sunset_min_ == 0) return false; // not yet received
  time_t now = time(nullptr);
  struct tm t{};
  localtime_r(&now, &t);
  uint16_t now_min = (uint16_t)(t.tm_hour * 60 + t.tm_min);
  return (now_min < sunrise_min_ || now_min >= sunset_min_);
}

void clock::render_moon_to_canvas(float phase) {
  if (!moon_canvas_ || !moon_canvas_buf_) return;

  // Clear canvas to transparent
  memset(moon_canvas_buf_, 0, MOON_BUF_BYTES);

  // Draw the full-moon PNG onto the canvas
  lv_draw_img_dsc_t img_dsc;
  lv_draw_img_dsc_init(&img_dsc);
  lv_canvas_draw_img(moon_canvas_, 0, 0, "S:/spiffs/moon.png", &img_dsc);

  // Paint shadow pixels using the terminator formula:
  //   b = cos(phase × 2π)
  //   lit  = (phase ≤ 0.5) ? dx ≥ b×√(1−dy²)
  //        :                  dx ≤ −b×√(1−dy²)
  const float b = cosf(phase * 2.0f * (float)M_PI);
  const lv_color_t shadow = lv_color_make(10, 10, 20); // deep-blue shadow

  for (int py = 0; py < MOON_SIZE; py++) {
    const float dy = (py - MOON_RADIUS) / MOON_RADIUS;
    const float dy2 = dy * dy;
    if (dy2 > 1.0f) continue;
    const float limit = sqrtf(1.0f - dy2);

    for (int px = 0; px < MOON_SIZE; px++) {
      const float dx = (px - MOON_RADIUS) / MOON_RADIUS;
      if (dx * dx + dy2 > 1.0f) continue; // outside moon disc

      const bool lit = (phase <= 0.5f) ? (dx >= b * limit) : (dx <= -b * limit);
      if (!lit) lv_canvas_set_px_color(moon_canvas_, px, py, shadow);
    }
  }

  last_moon_phase_ = phase;
}

void clock::set_panel0_mode(Panel0Mode mode) {
  panel0_mode_ = mode;
  // digit_images[0] visible only in DIGIT mode
  if (mode == Panel0Mode::DIGIT) {
    lv_obj_clear_flag(digit_images[0], LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(digit_images[0], LV_OBJ_FLAG_HIDDEN);
  }
  // weather icon visible only in WEATHER_ICON mode
  if (weather_icon_) {
    if (mode == Panel0Mode::WEATHER_ICON) {
      lv_obj_clear_flag(weather_icon_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(weather_icon_, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // moon canvas visible only in MOON mode
  if (moon_canvas_) {
    if (mode == Panel0Mode::MOON) {
      lv_obj_clear_flag(moon_canvas_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(moon_canvas_, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void clock::update_panel0(bool first_is_zero) {
  if (!first_is_zero) {
    set_panel0_mode(Panel0Mode::DIGIT);
    return;
  }
  if (is_nighttime() && moon_canvas_) {
    float phase = current_moon_phase(time(nullptr));
    // Re-render when phase has drifted more than ~3 hours since last render
    if (last_moon_phase_ < 0.0f || fabsf(phase - last_moon_phase_) > 0.005f) {
      render_moon_to_canvas(phase);
    }
    set_panel0_mode(Panel0Mode::MOON);
  } else {
    set_panel0_mode(Panel0Mode::WEATHER_ICON);
  }
}

void clock::set_weather_condition(uint16_t code) {
  weather_code_ = code;
  if (weather_icon_) {
    lv_img_set_src(weather_icon_, condition_icon_src(code));
  }
}

void clock::set_sun_times(uint16_t sunrise_min, uint16_t sunset_min) {
  sunrise_min_ = sunrise_min;
  sunset_min_  = sunset_min;
}

void clock::show_moon_test() {
  static constexpr float phases[NUM_LCDS] = {
      0.0f,    // new moon
      0.125f,  // waxing crescent
      0.25f,   // first quarter
      0.5f,    // full moon
      0.625f,  // waning gibbous
      0.75f,   // last quarter
  };
  lv_timer_pause(clock_update_timer);

  for (int i = 0; i < NUM_LCDS; i++) {
    // Allocate a PSRAM canvas buffer for this panel
    void *buf = heap_caps_malloc(MOON_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) continue;

    lv_disp_set_default(gui_get_display(i));
    lv_obj_t *screen = lv_scr_act();

    // Dark background
    lv_obj_set_style_bg_color(screen, BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, buf, MOON_SIZE, MOON_SIZE, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);

    // Clear to transparent, draw moon PNG, apply shadow
    memset(buf, 0, MOON_BUF_BYTES);
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_canvas_draw_img(canvas, 0, 0, "S:/spiffs/moon.png", &img_dsc);

    const float phase = phases[i];
    const float b     = cosf(phase * 2.0f * (float)M_PI);
    const lv_color_t shadow = lv_color_make(10, 10, 20);

    for (int py = 0; py < MOON_SIZE; py++) {
      const float dy    = (py - MOON_RADIUS) / MOON_RADIUS;
      const float dy2   = dy * dy;
      if (dy2 > 1.0f) continue;
      const float limit = sqrtf(1.0f - dy2);
      for (int px = 0; px < MOON_SIZE; px++) {
        const float dx = (px - MOON_RADIUS) / MOON_RADIUS;
        if (dx * dx + dy2 > 1.0f) continue;
        const bool lit = (phase <= 0.5f) ? (dx >= b * limit) : (dx <= -b * limit);
        if (!lit) lv_canvas_set_px_color(canvas, px, py, shadow);
      }
    }
  }
}

void clock::hide_all() {
  lv_timer_pause(clock_update_timer);
  for (int i = 0; i < NUM_LCDS - 1; i++) {
    lv_obj_add_flag(digit_images[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_add_flag(ampm_image, LV_OBJ_FLAG_HIDDEN);
  if (temp_label) lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
  if (wifi_status_icon) lv_obj_add_flag(wifi_status_icon, LV_OBJ_FLAG_HIDDEN);
  if (weather_icon_) lv_obj_add_flag(weather_icon_, LV_OBJ_FLAG_HIDDEN);
  if (moon_canvas_)  lv_obj_add_flag(moon_canvas_,  LV_OBJ_FLAG_HIDDEN);
}

void clock::restore_all() {
  for (int i = 0; i < NUM_LCDS - 1; i++) {
    lv_obj_clear_flag(digit_images[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_clear_flag(ampm_image, LV_OBJ_FLAG_HIDDEN);
  if (temp_label) lv_obj_clear_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
  if (wifi_status_icon && !wifi_connected_) lv_obj_clear_flag(wifi_status_icon, LV_OBJ_FLAG_HIDDEN);
  // Re-apply panel 0 mode (loop above unconditionally unhid digit_images[0])
  set_panel0_mode(panel0_mode_);
  lv_timer_resume(clock_update_timer);
}

void clock::show_recovery_screen() {
  // Pause the clock tick so digit updates and colon blinks don't overwrite
  // the recovery overlay, then hide all digit/status labels.
  lv_timer_pause(clock_update_timer);
  for (int i = 0; i < NUM_LCDS - 1; i++) {
    lv_obj_add_flag(digit_images[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_add_flag(ampm_image, LV_OBJ_FLAG_HIDDEN);
  if (temp_label) lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
  if (wifi_status_icon) lv_obj_add_flag(wifi_status_icon, LV_OBJ_FLAG_HIDDEN);
  if (weather_icon_) lv_obj_add_flag(weather_icon_, LV_OBJ_FLAG_HIDDEN);
  if (moon_canvas_) lv_obj_add_flag(moon_canvas_, LV_OBJ_FLAG_HIDDEN);

  // A single, high-legibility instruction card: each panel uses the same
  // 24 px face.  The SSID and IP are deliberately stacked to preserve their
  // exact values without shrinking the text to fit a narrow tube.
  struct panel_t { const char *text; const lv_font_t *font; };
  static const panel_t PANELS[NUM_LCDS] = {
      {"SETUP",          &lv_font_montserrat_24},
      {"JOIN",           &lv_font_montserrat_24},
      {"now\ntube\n-setup", &lv_font_montserrat_24},
      {"OPEN",           &lv_font_montserrat_24},
      {"BROWSER",        &lv_font_montserrat_24},
      {"192\n.168\n.4.1", &lv_font_montserrat_24},
  };

  for (int i = 0; i < NUM_LCDS; i++) {
    if (recovery_labels[i] != nullptr) continue;  // idempotent
    lv_disp_set_default(gui_get_display(i));
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, PANELS[i].font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFCF9D9), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LCD_WIDTH + 2);  // +2px: "SETUP" is 81.3px at 24pt, clips cleanly at hardware boundary
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text_static(label, PANELS[i].text);
    recovery_labels[i] = label;
  }
}

void clock::clear_recovery_screen() {
  for (int i = 0; i < NUM_LCDS; i++) {
    if (recovery_labels[i] != nullptr) {
      lv_obj_del(recovery_labels[i]);
      recovery_labels[i] = nullptr;
    }
  }
  for (int i = 0; i < NUM_LCDS - 1; i++) {
    lv_obj_clear_flag(digit_images[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_clear_flag(ampm_image, LV_OBJ_FLAG_HIDDEN);
  if (temp_label) lv_obj_clear_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
  if (wifi_status_icon && !wifi_connected_) lv_obj_clear_flag(wifi_status_icon, LV_OBJ_FLAG_HIDDEN);
  set_panel0_mode(panel0_mode_);
  lv_timer_resume(clock_update_timer);
}

void clock::shuffle() {
  char buf[2] = {static_cast<char>('0' + rand() % 10), '\0'};

  for (size_t j = 0; j < digit_images.size(); j++) {
    lv_label_set_text_static(digit_images[j], j == 2 ? "" : buf);
  }
  lv_label_set_text_static(ampm_image, "");

  update();
}

clock::~clock() {
  // Cancel the periodic tick first so no callbacks fire during teardown.
  lv_timer_del(clock_update_timer);

  // Cancel any in-flight delayed-start timers before deleting the objects
  // they would reference; unique_ptr flappers and flap_sequences clean
  // themselves up afterward via their own destructors.
  for (size_t i = 0; i < delayed_start_timers.size(); i++) {
    if (delayed_start_timers[i] != nullptr) {
      lv_timer_del(delayed_start_timers[i]);
      delayed_start_timers[i] = nullptr;
    }
  }

  // Stop all active flappers before their parent screens are deleted.
  for (auto &fp : flappers) {
    if (fp) fp->stop();
  }

  lv_obj_del(ampm_image);
  if (temp_label)    lv_obj_del(temp_label);
  if (weather_icon_) lv_obj_del(weather_icon_);
  if (moon_canvas_)  lv_obj_del(moon_canvas_);
  if (moon_canvas_buf_) { heap_caps_free(moon_canvas_buf_); moon_canvas_buf_ = nullptr; }

  for (auto *img : digit_images) {
    lv_obj_del(img);
  }

  for (auto *image : background_images) {
    lv_obj_del(image);
  }
}
