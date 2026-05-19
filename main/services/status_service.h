//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <ctime>

#include "models/status_snapshot.h"

namespace status_service {

void init(const build_info_snapshot &build);
void set_brightness(uint8_t brightness_pct);
void set_mode(const char *mode_name);
void set_wifi(bool connected, const char *ip_address);
void set_wifi_retry_count(uint32_t count);
void set_weather_sync(time_t last_weather_sync);
status_snapshot get_snapshot();

}
