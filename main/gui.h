//  SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//  SPDX-License-Identifier: MIT

#pragma once

#include <lvgl.h>

void gui_init();
void gui_start();
lv_disp_t *gui_get_display(size_t index);
void gui_invalidate_all_screens();
void gui_lvgl_lock();
void gui_lvgl_unlock();
/** Wake the LVGL task so it runs lv_timer_handler() soon (e.g. after queuing weather UI update). */
void gui_wake_lvgl_task();

