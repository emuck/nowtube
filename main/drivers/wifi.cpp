//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT
//
//

#include "wifi.h"

#include <cstring>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "services/status_service.h"

#define RETRY_LIMIT UINT_MAX

static const char *const TAG = "wifi";
static wifi_connected_callback_t s_wifi_connected_callback = nullptr;
static bool s_sta_started    = false;
static bool s_ap_netif_created = false;

static constexpr const char *RECOVERY_AP_SSID = "nowtube-setup";
static constexpr const char *RECOVERY_AP_IP   = "192.168.4.1";
static bool s_recovery_mode = false;  // suppresses STA reconnect during AP transition

static void event_handler([[maybe_unused]] void *arg,
                          esp_event_base_t event_base, int32_t event_id,
                          void *event_data) {
  static unsigned int s_retry_num = 0;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    auto *disc = static_cast<wifi_event_sta_disconnected_t *>(event_data);
    status_service::set_wifi(false, "");
    if (s_recovery_mode) {
      ESP_LOGI(TAG, "WiFi disconnected (reason=%d) — recovery mode, not reconnecting", disc->reason);
    } else if (s_retry_num < RETRY_LIMIT) {
      s_retry_num++;
      status_service::set_wifi_retry_count(s_retry_num);
      esp_wifi_connect();
      ESP_LOGW(TAG, "WiFi disconnected (reason=%d) — retrying (attempt %u)",
               disc->reason, s_retry_num);
    } else {
      ESP_LOGE(TAG, "WiFi disconnected (reason=%d) — retry limit reached", disc->reason);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    status_service::set_wifi(true, ip);
    s_retry_num = 0;
    status_service::set_wifi_retry_count(0);

    if (s_wifi_connected_callback != nullptr) {
      s_wifi_connected_callback();
    }
  }
}

void wifi_init(wifi_connected_callback_t callback) {
  ESP_LOGI(TAG, "Starting wifi & TCP/IP...");

  s_wifi_connected_callback = callback;

  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  esp_event_handler_instance_t instance_any_id = nullptr;
  esp_event_handler_instance_t instance_got_ip = nullptr;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr,
      &instance_got_ip));
}

/* TODO: implement QR code provisioning from
 https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/provisioning/wifi_provisioning.html
 */
bool wifi_connect(const char *ssid, const char *psk) {
  if (ssid == nullptr || ssid[0] == '\0') {
    ESP_LOGW(TAG, "WiFi connect skipped: SSID is empty");
    return false;
  }

  wifi_config_t wifi_config = {};
  strlcpy(reinterpret_cast<char *>(wifi_config.sta.ssid), ssid,
          sizeof(wifi_config.sta.ssid));
  strlcpy(reinterpret_cast<char *>(wifi_config.sta.password), psk != nullptr ? psk : "",
          sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  ESP_LOGI(TAG, "Attempting to connect to SSID '%s'",
           reinterpret_cast<char *>(wifi_config.sta.ssid));

  ESP_ERROR_CHECK(esp_wifi_start());
  s_sta_started = true;
  return true;
}

void wifi_start_recovery_ap() {
  s_recovery_mode = true;  // prevent event handler from reconnecting STA
  if (s_sta_started) {
    ESP_LOGW(TAG, "Recovery AP: stopping STA");
    esp_wifi_stop();
    s_sta_started = false;
  }

  if (!s_ap_netif_created) {
    esp_netif_create_default_wifi_ap();
    s_ap_netif_created = true;
  }

  wifi_config_t ap_cfg = {};
  strlcpy(reinterpret_cast<char *>(ap_cfg.ap.ssid), RECOVERY_AP_SSID,
          sizeof(ap_cfg.ap.ssid));
  ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
  ap_cfg.ap.max_connection = 4;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGW(TAG, "Recovery AP active — SSID: '%s'  IP: %s",
           RECOVERY_AP_SSID, RECOVERY_AP_IP);
}

void wifi_cancel_recovery_ap() {
  s_recovery_mode = false;
  esp_wifi_stop();
  s_sta_started = false;
  ESP_LOGI(TAG, "Recovery AP stopped");
}
