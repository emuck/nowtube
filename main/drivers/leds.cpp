//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT

#include "leds.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip_encoder.h"

#define RMT_LED_STRIP_RESOLUTION_HZ                                            \
  10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high
           // resolution)
constexpr gpio_num_t RMT_LED_STRIP_GPIO_NUM = GPIO_NUM_32;

constexpr auto TAG = "leds";

// For each physical strip position 0..5, which logical (display) index to show there.
// Default {0,1,2,3,4,5}. If LEDs look dislocated vs displays, try reordering (e.g. {1,0,3,2,5,4}).
static const uint8_t PHYSICAL_POS_TO_LOGICAL[NUM_LEDS] = {0, 1, 2, 3, 4, 5};

static rmt_channel_handle_t led_chan = nullptr;
static rmt_encoder_handle_t led_encoder = nullptr;
static uint8_t led_tx_buf[NUM_LEDS * 3] = {0};
static SemaphoreHandle_t led_tx_mutex = nullptr;

static rmt_transmit_config_t tx_config = {
    .loop_count = 0, // no transfer loop
};

static void state_to_wire_order(const leds_state *state, uint8_t *out_grb) {
  for (size_t p = 0; p < NUM_LEDS; p++) {
    size_t log = PHYSICAL_POS_TO_LOGICAL[p];
    out_grb[p * 3 + 0] = state->pixel_grb[log * 3 + 0];
    out_grb[p * 3 + 1] = state->pixel_grb[log * 3 + 1];
    out_grb[p * 3 + 2] = state->pixel_grb[log * 3 + 2];
  }
}

void leds_init() {
  ESP_LOGI(TAG, "Create RMT TX channel");
  rmt_tx_channel_config_t tx_chan_config = {
      .gpio_num = RMT_LED_STRIP_GPIO_NUM,
      .clk_src = RMT_CLK_SRC_DEFAULT, // select source clock
      .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
      .mem_block_symbols =
          64, // increase the block size can make the LEDs flicker less
      .trans_queue_depth = 4, // set the number of transactions that can be
                              // pending in the background
  };
  ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

  ESP_LOGI(TAG, "Install led strip encoder");
  led_strip_encoder_config_t encoder_config = {
      .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
  };
  ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

  ESP_LOGI(TAG, "Enable RMT TX channel");
  ESP_ERROR_CHECK(rmt_enable(led_chan));

  led_tx_mutex = xSemaphoreCreateMutex();
  if (led_tx_mutex == nullptr) {
    ESP_LOGE(TAG, "Failed to create LED mutex — LED updates will be skipped");
  }
}

void leds_off() {
  leds_state state = {};
  leds_update(&state);
}

bool leds_update_if_free(const leds_state *state) {
  if (led_tx_mutex == nullptr) return false;
  if (xSemaphoreTake(led_tx_mutex, 0) != pdTRUE) {
    return false;
  }
  esp_err_t err = rmt_tx_wait_all_done(led_chan, 0);
  if (err == ESP_ERR_TIMEOUT) {
    xSemaphoreGive(led_tx_mutex);
    return false;
  }
  ESP_ERROR_CHECK(err);

  state_to_wire_order(state, led_tx_buf);
  ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_tx_buf,
                               sizeof(led_tx_buf), &tx_config));
  xSemaphoreGive(led_tx_mutex);
  return true;
}

void leds_update(const leds_state *state) {
  ESP_LOGV(TAG, "Updating LEDS with RMT");
  if (led_tx_mutex == nullptr) return;
  xSemaphoreTake(led_tx_mutex, portMAX_DELAY);
  uint8_t wire_grb[NUM_LEDS * 3];
  state_to_wire_order(state, wire_grb);
  ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, wire_grb,
                               sizeof(wire_grb), &tx_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
  xSemaphoreGive(led_tx_mutex);
}

bool leds_update_wait(const leds_state *state, uint32_t timeout_ms) {
  if (led_tx_mutex == nullptr) return false;
  TickType_t lock_timeout_ticks = pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(led_tx_mutex, lock_timeout_ticks) != pdTRUE) {
    return false;
  }
  uint32_t tx_timeout_ticks = pdMS_TO_TICKS(timeout_ms);
  esp_err_t err = rmt_tx_wait_all_done(led_chan, tx_timeout_ticks);
  if (err == ESP_ERR_TIMEOUT) {
    xSemaphoreGive(led_tx_mutex);
    return false;
  }
  ESP_ERROR_CHECK(err);
  uint8_t wire_grb[NUM_LEDS * 3];
  state_to_wire_order(state, wire_grb);
  ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, wire_grb,
                               sizeof(wire_grb), &tx_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
  xSemaphoreGive(led_tx_mutex);
  return true;
}
