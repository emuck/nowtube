// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

#include "models/device_config.h"

// The catalog is the product-facing list of approved compiled font packs.
// Runtime TTF upload is deliberately not part of this first implementation.
struct clock_font_info {
  ClockFont value;
  const char *id;
  const char *label;
  const char *description;
};

const clock_font_info *clock_font_catalog(size_t *count);
const clock_font_info *clock_font_find(ClockFont font);
bool clock_font_supported(ClockFont font);
