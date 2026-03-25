#pragma once

#ifdef ESP_PLATFORM
#include "drivers/touchpads.h"
#endif

namespace game_display {

// Called under gui_lvgl_lock() when entering GAME mode.
void show();

// Called under gui_lvgl_lock() when leaving GAME mode.
void clear();

// Called from input_controller when mode is GAME.
#ifdef ESP_PLATFORM
void handle_input(touchpad_button_t button);
#endif

}  // namespace game_display
