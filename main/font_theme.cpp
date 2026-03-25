//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "font_theme.h"

LV_FONT_DECLARE(nixie_120)
LV_FONT_DECLARE(nixie_60)
LV_FONT_DECLARE(nixie_temp)

LV_FONT_DECLARE(space_mono_120)
LV_FONT_DECLARE(space_mono_60)
LV_FONT_DECLARE(space_mono_48)

static constexpr clock_theme THEME_NIXIE = {
    .digit = &nixie_120,
    .ampm  = &nixie_60,
    .temp  = &nixie_temp,
};

static constexpr clock_theme THEME_SPACE_MONO = {
    .digit = &space_mono_120,
    .ampm  = &space_mono_60,
    .temp  = &space_mono_48,
};

const clock_theme &font_theme_get(ClockFont font) {
  switch (font) {
  case ClockFont::SPACE_MONO:
    return THEME_SPACE_MONO;
  case ClockFont::NIXIE:
  default:
    return THEME_NIXIE;
  }
}
