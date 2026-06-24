//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "services/diagnostics_service.h"

#include <inttypes.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs.h>

namespace diagnostics_service {

static constexpr const char *TAG = "diagnostics";
static diagnostics_snapshot s_snapshot{};

static const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_UNKNOWN:   return "ESP_RST_UNKNOWN";
  case ESP_RST_POWERON:   return "ESP_RST_POWERON";
  case ESP_RST_EXT:       return "ESP_RST_EXT";
  case ESP_RST_SW:        return "ESP_RST_SW";
  case ESP_RST_PANIC:     return "ESP_RST_PANIC";
  case ESP_RST_INT_WDT:   return "ESP_RST_INT_WDT";
  case ESP_RST_TASK_WDT:  return "ESP_RST_TASK_WDT";
  case ESP_RST_WDT:       return "ESP_RST_WDT";
  case ESP_RST_DEEPSLEEP: return "ESP_RST_DEEPSLEEP";
  case ESP_RST_BROWNOUT:  return "ESP_RST_BROWNOUT";
  case ESP_RST_SDIO:      return "ESP_RST_SDIO";
  default:                return "ESP_RST_OTHER";
  }
}

void init() {
  esp_reset_reason_t reason = esp_reset_reason();
  s_snapshot.last_reset_reason = reset_reason_name(reason);
  s_snapshot.boot_count = 0;

  nvs_handle_t h;
  if (nvs_open("nowtube", NVS_READWRITE, &h) == ESP_OK) {
    uint32_t boot_count = 0;
    if (nvs_get_u32(h, "boot_count", &boot_count) != ESP_OK) {
      boot_count = 0;
    }
    boot_count++;
    s_snapshot.boot_count = boot_count;
    nvs_set_u32(h, "boot_count", boot_count);
    nvs_set_u32(h, "last_reset", static_cast<uint32_t>(reason));
    nvs_commit(h);
    nvs_close(h);
  }

  ESP_LOGI(TAG, "Diagnostics initialized: reset=%s boot_count=%" PRIu32,
           s_snapshot.last_reset_reason, s_snapshot.boot_count);
}

diagnostics_snapshot get_snapshot() {
  s_snapshot.free_heap     = static_cast<uint32_t>(esp_get_free_heap_size());
  s_snapshot.min_free_heap = static_cast<uint32_t>(esp_get_minimum_free_heap_size());
  s_snapshot.free_internal_heap = static_cast<uint32_t>(
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  s_snapshot.min_free_internal_heap = static_cast<uint32_t>(
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return s_snapshot;
}

}
