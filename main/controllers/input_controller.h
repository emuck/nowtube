//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include "drivers/touchpads.h"
#include "mode_manager.h"

namespace input_controller {

void init(bool clock_only, void (*apply_mode)(DisplayMode),
          void (*schedule_clock_cycle)(),
          void (*on_wifi_recovery_requested)() = nullptr);
void on_button_tapped(touchpad_button_t button);
void on_button_touched(touchpad_button_t button, bool is_pressed);

}
