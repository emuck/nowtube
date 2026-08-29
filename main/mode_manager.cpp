#include "mode_manager.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>
static const char *TAG = "mode_manager";
#endif

const char* ModeManager::name(DisplayMode m) {
    switch (m) {
    case DisplayMode::CLOCK:    return "CLOCK";
    case DisplayMode::DATE:      return "DATE";
    case DisplayMode::TODAY:     return "TODAY";
    case DisplayMode::FORECAST:  return "FORECAST";
    case DisplayMode::GAME:      return "GAME";
    case DisplayMode::SPECTRUM:  return "SPECTRUM";
    }
    return "UNKNOWN";
}

void ModeManager::cycle() {
    switch (mode_) {
    case DisplayMode::CLOCK:     mode_ = DisplayMode::TODAY;     break;
    case DisplayMode::TODAY:     mode_ = DisplayMode::FORECAST;  break;
    case DisplayMode::FORECAST:  mode_ = DisplayMode::SPECTRUM;  break;
    case DisplayMode::SPECTRUM:  mode_ = DisplayMode::CLOCK;     break;
    case DisplayMode::DATE:      mode_ = DisplayMode::CLOCK;     break;
    case DisplayMode::GAME:      mode_ = DisplayMode::CLOCK;     break;
    }
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Mode → %s", name(mode_));
#endif
}

void ModeManager::reset() {
    mode_ = DisplayMode::CLOCK;
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Mode reset → CLOCK");
#endif
}

void ModeManager::set(DisplayMode m) {
    mode_ = m;
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Mode set → %s", name(mode_));
#endif
}
