//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "models/device_config.h"

namespace config_service {

bool init();
const device_config &get_config();
bool update(const device_config &config);
bool set_display_brightness(uint8_t brightness_pct);

}
