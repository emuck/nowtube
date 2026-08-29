//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT
//
//

#pragma once

#include <cstdint>

/* returns true if the RTC contained a valid time upon init */
bool rtc_init();

void rtc_persist();
void rtc_discipline_start();
bool rtc_has_valid_time();
bool rtc_battery_ok();
bool rtc_discipline_active();
int32_t rtc_last_error_ms();
int32_t rtc_max_error_ms();
