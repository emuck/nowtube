// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "displays/spectrum/spectrum_display.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <lvgl.h>

#include "drivers/lcds.h"
#include "gui.h"
#include "services/microphone_service.h"

namespace spectrum_display {
namespace {

constexpr int BASELINE_Y = 153;
constexpr int TOP_MARGIN = 8;
constexpr int MAX_HEIGHT = BASELINE_Y - TOP_MARGIN;
constexpr uint32_t BG = 0x020304;

uint16_t *s_buf[NUM_LCDS] = {};
float s_live_snapshot[microphone_service::LANDSCAPE_WIDTH] = {};
float s_peak_snapshot[microphone_service::LANDSCAPE_WIDTH] = {};

uint8_t lerp_u8(uint8_t a, uint8_t b, float t) {
  return static_cast<uint8_t>(std::clamp(a + (b - a) * t, 0.0f, 255.0f));
}

uint16_t color_for_x(int x, float intensity) {
  intensity = std::clamp(intensity, 0.0f, 1.0f);
  const float pos = static_cast<float>(x) / static_cast<float>(microphone_service::LANDSCAPE_WIDTH - 1);
  uint8_t r, g, b;
  if (pos < 0.55f) {
    const float t = pos / 0.55f;
    r = lerp_u8(255, 255, t);
    g = lerp_u8(74, 185, t);
    b = lerp_u8(22, 70, t);
  } else {
    const float t = (pos - 0.55f) / 0.45f;
    r = lerp_u8(255, 82, t);
    g = lerp_u8(185, 214, t);
    b = lerp_u8(70, 255, t);
  }
  const float glow = 0.10f + 0.90f * intensity;
  return color_to_rgb565(static_cast<uint8_t>(r * glow),
                         static_cast<uint8_t>(g * glow),
                         static_cast<uint8_t>(b * glow));
}

void put_px(int panel, int x, int y, uint16_t c) {
  if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
  s_buf[panel][y * LCD_WIDTH + x] = c;
}

void fill_panel(int panel, uint16_t c) {
  const size_t count = static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT;
  for (size_t i = 0; i < count; ++i) s_buf[panel][i] = c;
}

void build_panel(int panel) {
  if (s_buf[panel] == nullptr) {
    s_buf[panel] = static_cast<uint16_t *>(heap_caps_malloc(
        static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_buf[panel] == nullptr) {
      s_buf[panel] = static_cast<uint16_t *>(malloc(
          static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t)));
    }
  }
  configASSERT(s_buf[panel] != nullptr);
  fill_panel(panel, color_to_rgb565(2, 3, 4));
}

void draw_landscape_panel(int panel, const float live[microphone_service::LANDSCAPE_WIDTH],
                          const float peak[microphone_service::LANDSCAPE_WIDTH]) {
  fill_panel(panel, color_to_rgb565(2, 3, 4));

  const int gx0 = panel * LCD_WIDTH;
  for (int lx = 0; lx < LCD_WIDTH; ++lx) {
    const int gx = gx0 + lx;
    float v = std::clamp(live[gx], 0.0f, 1.0f);
    float p = std::clamp(peak[gx], 0.0f, 1.0f);
    const int y = BASELINE_Y - static_cast<int>(v * MAX_HEIGHT);
    const int py = BASELINE_Y - static_cast<int>(p * MAX_HEIGHT);

    for (int yy = y; yy <= BASELINE_Y; ++yy) {
      const float depth = static_cast<float>(BASELINE_Y - yy) / static_cast<float>(MAX_HEIGHT);
      const float edge = 0.28f + 0.72f * depth;
      put_px(panel, lx, yy, color_for_x(gx, std::min(1.0f, v * edge)));
    }

    uint16_t crest = color_for_x(gx, v > 0.01f ? 0.95f : 0.0f);
    put_px(panel, lx, y, crest);
    if (v > 0.01f) put_px(panel, lx, y - 1, color_for_x(gx, 0.55f));
    if (lx > 0) put_px(panel, lx - 1, y, crest);

    if (p > 0.06f) {
      uint16_t ghost = color_for_x(gx, 0.28f);
      put_px(panel, lx, py, ghost);
      put_px(panel, lx, py + 1, ghost);
    }
  }

  for (int x = 0; x < LCD_WIDTH; ++x) {
    put_px(panel, x, BASELINE_Y + 1, color_to_rgb565(55, 66, 74));
  }

  lcd_select(panel);
  lcd_blit_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, s_buf[panel],
                static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t));
}

}  // namespace

void show() {
  for (int panel = 0; panel < static_cast<int>(NUM_LCDS); ++panel) {
    if (s_buf[panel] == nullptr) build_panel(panel);
  }
  refresh();
}

void refresh() {
  if (s_buf[0] == nullptr) return;
  microphone_service::get_landscape(s_live_snapshot, s_peak_snapshot);
  for (int panel = 0; panel < static_cast<int>(NUM_LCDS); ++panel) {
    draw_landscape_panel(panel, s_live_snapshot, s_peak_snapshot);
  }
}

void clear() {
  for (int panel = 0; panel < static_cast<int>(NUM_LCDS); ++panel) {
    if (s_buf[panel] != nullptr) {
      fill_panel(panel, color_to_rgb565(0, 0, 0));
      lcd_select(panel);
      lcd_blit_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, s_buf[panel],
                    static_cast<size_t>(LCD_WIDTH) * LCD_HEIGHT * sizeof(uint16_t));
    }
  }
}

}  // namespace spectrum_display
