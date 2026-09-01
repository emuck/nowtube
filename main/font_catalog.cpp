// SPDX-License-Identifier: MIT

#include "font_catalog.h"

namespace {

constexpr clock_font_info kClockFonts[] = {
    {ClockFont::NIXIE, "nixie", "Nixie One", "Warm, characterful tube face."},
    {ClockFont::SPACE_MONO, "space-mono", "Space Mono", "Clean technical mono face."},
    {ClockFont::ATKINSON_HYPERLEGIBLE, "atkinson-hyperlegible", "Atkinson Hyperlegible",
     "Maximum at-a-glance readability."},
    {ClockFont::ALDRICH, "aldrich", "Aldrich", "Geometric display face with a calm sci-fi edge."},
};

}  // namespace

const clock_font_info *clock_font_catalog(size_t *count) {
  if (count != nullptr) *count = sizeof(kClockFonts) / sizeof(kClockFonts[0]);
  return kClockFonts;
}

const clock_font_info *clock_font_find(ClockFont font) {
  size_t count = 0;
  const clock_font_info *fonts = clock_font_catalog(&count);
  for (size_t i = 0; i < count; ++i) {
    if (fonts[i].value == font) return &fonts[i];
  }
  return nullptr;
}

bool clock_font_supported(ClockFont font) {
  return clock_font_find(font) != nullptr;
}
