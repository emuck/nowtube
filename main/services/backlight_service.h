//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace backlight_service {

bool init(uint8_t initial_brightness_pct);
void cycle_brightness();
void cycle_led_selection();
void cycle_backlight_mode();
void finalize_boot_state();

// Direct getters (safe to call from any task — reads non-volatile config state)
const char *get_mode_name();       // "normal" | "breathable" | "mixed"
const char *get_led_color_name();  // "warm orange" | "red" | … | "off"

// Direct setters — same safety as the corresponding cycle_* functions.
// Return false if the name is not recognized.
bool set_mode(const char *name);
bool set_led_color(const char *name);  // color name from LED_COLOR_NAMES, or "off"

// Stops any active breath animation and sets LEDs to solid dim blue.
// Call when entering recovery AP mode.
void set_recovery_cue();

// Restore LED state to what it was before set_recovery_cue().
// Call when cancelling recovery AP mode.
void cancel_recovery_cue();

// Suspend/resume LED animation without changing saved mode or color settings.
// Call pause() on entering a mode that drives LEDs directly (e.g. game);
// call resume() when leaving that mode to hand control back to the backlight.
void pause();
void resume();

}
