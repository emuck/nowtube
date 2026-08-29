// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "services/environment_sensor_service.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <cstring>

#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace environment_sensor_service {
namespace {

constexpr const char *TAG = "env_sensor";
constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
constexpr uint8_t SHT30_ADDR = 0x44;
constexpr uint8_t CMD_MEAS_HI_NCS[2] = {0x24, 0x00};
constexpr uint32_t I2C_TIMEOUT_MS = 100;
constexpr uint32_t MEAS_WAIT_MS = 20;
constexpr uint32_t SAMPLE_INTERVAL_MS = 30 * 1000;

bool s_present = false;
reading s_last = {};
SemaphoreHandle_t s_mutex = nullptr;

uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

esp_err_t transmit(const uint8_t *data, size_t len) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (cmd == nullptr) return ESP_ERR_NO_MEM;
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (SHT30_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write(cmd, const_cast<uint8_t *>(data), len, true);
  i2c_master_stop(cmd);
  esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
  i2c_cmd_link_delete(cmd);
  return err;
}

esp_err_t receive(uint8_t *data, size_t len) {
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (cmd == nullptr) return ESP_ERR_NO_MEM;
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (SHT30_ADDR << 1) | I2C_MASTER_READ, true);
  if (len > 1) {
    i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
  }
  i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
  i2c_master_stop(cmd);
  esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
  i2c_cmd_link_delete(cmd);
  return err;
}

bool read_once(reading &out) {
  if (!s_present) return false;
  esp_err_t err = transmit(CMD_MEAS_HI_NCS, sizeof(CMD_MEAS_HI_NCS));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "measurement trigger failed: %s", esp_err_to_name(err));
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(MEAS_WAIT_MS));

  uint8_t buf[6] = {};
  err = receive(buf, sizeof(buf));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
    return false;
  }
  if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
    ESP_LOGW(TAG, "CRC mismatch");
    return false;
  }

  uint16_t raw_t = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
  uint16_t raw_h = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];
  out.temp_c = -45.0f + 175.0f * static_cast<float>(raw_t) / 65535.0f;
  out.humidity_pct = std::clamp(100.0f * static_cast<float>(raw_h) / 65535.0f, 0.0f, 100.0f);
  out.valid = true;
  return true;
}

void sensor_task(void *) {
  for (;;) {
    reading r;
    if (read_once(r)) {
      if (s_mutex != nullptr) xSemaphoreTake(s_mutex, portMAX_DELAY);
      s_last = r;
      if (s_mutex != nullptr) xSemaphoreGive(s_mutex);
      ESP_LOGI(TAG, "%.1f C %.1f %%RH", static_cast<double>(r.temp_c), static_cast<double>(r.humidity_pct));
    }
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
  }
}

}  // namespace

bool init() {
  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) {
    ESP_LOGE(TAG, "failed to create mutex");
    return false;
  }

  esp_err_t probe = transmit(CMD_MEAS_HI_NCS, sizeof(CMD_MEAS_HI_NCS));
  if (probe != ESP_OK) {
    ESP_LOGI(TAG, "SHT30 not found at 0x%02X: %s", SHT30_ADDR, esp_err_to_name(probe));
    s_present = false;
    return false;
  }
  s_present = true;
  ESP_LOGI(TAG, "SHT30 found at 0x%02X", SHT30_ADDR);
  return true;
}

void start() {
  if (!s_present) return;
  xTaskCreate(sensor_task, "sht30", 4096, nullptr, 4, nullptr);
}

bool is_present() { return s_present; }

bool get(reading &out) {
  if (s_mutex != nullptr) xSemaphoreTake(s_mutex, portMAX_DELAY);
  out = s_last;
  if (s_mutex != nullptr) xSemaphoreGive(s_mutex);
  return out.valid;
}

}  // namespace environment_sensor_service

#else
namespace environment_sensor_service {
bool init() { return false; }
void start() {}
bool is_present() { return false; }
bool get(reading &out) { out = {}; return false; }
}
#endif
