//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "font_theme.h"

LV_FONT_DECLARE(nixie_120)
LV_FONT_DECLARE(nixie_60)
LV_FONT_DECLARE(nixie_temp)

LV_FONT_DECLARE(space_mono_120)
LV_FONT_DECLARE(space_mono_60)
LV_FONT_DECLARE(space_mono_48)

LV_FONT_DECLARE(atkinson_hyperlegible_120)
LV_FONT_DECLARE(atkinson_hyperlegible_60)
LV_FONT_DECLARE(atkinson_hyperlegible_48)

LV_FONT_DECLARE(aldrich_120)
LV_FONT_DECLARE(aldrich_60)
LV_FONT_DECLARE(aldrich_48)

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

static constexpr clock_theme THEME_ATKINSON_HYPERLEGIBLE = {
    .digit = &atkinson_hyperlegible_120,
    .ampm  = &atkinson_hyperlegible_60,
    .temp  = &atkinson_hyperlegible_48,
};

static constexpr clock_theme THEME_ALDRICH = {
    .digit = &aldrich_120,
    .ampm  = &aldrich_60,
    .temp  = &aldrich_48,
};

const clock_theme &font_theme_get(ClockFont font) {
  switch (font) {
  case ClockFont::SPACE_MONO:
    return THEME_SPACE_MONO;
  case ClockFont::ATKINSON_HYPERLEGIBLE:
    return THEME_ATKINSON_HYPERLEGIBLE;
  case ClockFont::ALDRICH:
    return THEME_ALDRICH;
  case ClockFont::NIXIE:
  default:
    return THEME_NIXIE;
  }
}
