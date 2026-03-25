//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <lvgl.h>
#include "models/device_config.h"

struct clock_theme {
  const lv_font_t *digit;  // large digit font (size ~120)
  const lv_font_t *ampm;   // AM/PM font (size ~60)
  const lv_font_t *temp;   // temperature font (size ~48)
};

const clock_theme &font_theme_get(ClockFont font);
