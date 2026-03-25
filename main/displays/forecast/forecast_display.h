// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

#include "models/forecast_data.h"

namespace forecast_display {

// Show (or refresh) the forecast mode across all 6 panels.
// Panel 0: legend (DY / HI / LO).
// Panels 1–5: one day each (day code / high / low).
void show(const forecast_data &data);

// Remove all forecast labels and restore the clock layer.
void clear();

}  // namespace forecast_display
