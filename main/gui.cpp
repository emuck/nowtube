//  SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//  SPDX-License-Identifier: MIT

#include "gui.h"
#include "drivers/lcds.h"

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <lvgl.h>

constexpr auto BUFFER_ROWS =
    LCD_SPI_MAX_TRANSFER_SIZE / LCD_WIDTH / sizeof(uint16_t);
constexpr auto PIXEL_BUFFER_SIZE_PX = LCD_WIDTH * BUFFER_ROWS;

static DMA_ATTR uint16_t lcd_buffers[NUM_LCDS][PIXEL_BUFFER_SIZE_PX];
static lv_disp_draw_buf_t draw_buffers[NUM_LCDS];
static lv_disp_drv_t display_drivers[NUM_LCDS];
static lv_disp_t *displays[NUM_LCDS];

struct driver_user_data {
  size_t display_index;
};

static struct driver_user_data driver_user_datas[NUM_LCDS];
static SemaphoreHandle_t s_lvgl_mutex = nullptr;
static TaskHandle_t s_lvgl_task_handle = nullptr;

static void flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                     lv_color_t *color_p) {
  auto *user_data = static_cast<driver_user_data *>(disp_drv->user_data);
  lcd_select(user_data->display_index);
  lcd_blit_rect(area->x1, area->y1, area->x2 - area->x1 + 1,
                area->y2 - area->y1 + 1, (const uint16_t *)color_p,
                (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1) *
                    sizeof(lv_color_t));
  lv_disp_flush_ready(disp_drv);
}

static void lvgl_task([[maybe_unused]] void *arg) {
  s_lvgl_task_handle = xTaskGetCurrentTaskHandle();
  while (true) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));  // run soon when notified, or every 5 ms
    gui_lvgl_lock();
    lv_timer_handler();
    gui_lvgl_unlock();
  }
}

#if LV_USE_LOG
static void log_cb(const char *buf) {
  ESP_LOGI("lvgl", "%s", buf);
}
#endif

void gui_lvgl_lock() {
  xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
}

void gui_lvgl_unlock() {
  xSemaphoreGiveRecursive(s_lvgl_mutex);
}

void gui_wake_lvgl_task() {
  if (s_lvgl_task_handle != nullptr)
    xTaskNotifyGive(s_lvgl_task_handle);
}

void gui_init() {
#if LV_USE_LOG
  lv_log_register_print_cb(log_cb);
#endif

  lv_init();

  for (size_t i = 0; i < NUM_LCDS; i++) {
    lv_disp_draw_buf_init(&draw_buffers[i], lcd_buffers[i], nullptr,
                          PIXEL_BUFFER_SIZE_PX);
    lv_disp_drv_t *driver = &display_drivers[i];
    lv_disp_drv_init(driver);
    driver->draw_buf = &draw_buffers[i];
    driver->hor_res = LCD_WIDTH;
    driver->ver_res = LCD_HEIGHT;
    driver->flush_cb = flush_cb;

    struct driver_user_data *user_data = &driver_user_datas[i];
    user_data->display_index = i;

    driver->user_data = user_data;

    displays[i] = lv_disp_drv_register(driver);
  }

  s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
  configASSERT(s_lvgl_mutex);
}

void gui_start() {
  // Do NOT run lv_timer_handler() here: the first frame decodes 12 PNGs from SPIFFS
  // (6× split_flap + 6× divider) and blits 6 displays, which can block for minutes
  // and prevents WiFi/weather from starting. Let the lvgl task do the first frame.
  xTaskCreate(lvgl_task, "lvgl", 8192, nullptr, 5, nullptr);
}

lv_disp_t *gui_get_display(size_t index) {
  assert(lv_is_initialized());
  assert(index < NUM_LCDS);
  return displays[index];
}

void gui_invalidate_all_screens() {
  for (auto *display : displays) {
    lv_obj_t *screen = lv_disp_get_scr_act(display);
    lv_obj_invalidate(screen);
  }
}

