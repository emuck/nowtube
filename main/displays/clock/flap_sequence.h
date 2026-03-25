//   SPDX-FileCopyrightText: 2023 Ian Levesque <ian@ianlevesque.org>
//   SPDX-License-Identifier: MIT
//
//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <functional>
#include <lvgl.h>

#include "flapper.h"

using flap_sequence_update_callback =
    std::function<void(const std::string &value)>;

class flap_sequence {
public:
  explicit flap_sequence(flapper *flapper_,
                         flap_sequence_update_callback update_cb,
                         std::vector<std::string> &values)
      : values(values) {
    this->flapper_ = flapper_;
    this->update_cb = std::move(update_cb);
  }

  ~flap_sequence() {
    // Clear the callback we registered so animation_deleted() doesn't fire
    // into freed memory if the old animation outlives this flap_sequence.
    flapper_->set_finished_callback(nullptr, nullptr);
    // Cancel any pending lv_async_call queued by start() to prevent
    // next_step() running on a destroyed flap_sequence.
    lv_async_call_cancel(next_step_wrapper, this);
  }

  void start() { lv_async_call(next_step_wrapper, this); }

  static void next_step_wrapper(void *user_data) {
    static_cast<flap_sequence *>(user_data)->next_step();
  }

private:
  void next_step() {
    if (next_value_index >= values.size()) {
      return;
    }

    auto &value = values[next_value_index++];

    flapper_->before();
    update_cb(value);
    flapper_->after();

    flapper_->set_finished_callback(
        [](void *user_data) {
          auto *this_ = static_cast<flap_sequence *>(user_data);
          this_->next_step();
        },
        this);

    bool last = next_value_index == values.size();
    flapper_->start(last);
  }

  flapper *flapper_;
  flap_sequence_update_callback update_cb;
  std::vector<std::string> values;
  size_t next_value_index{0};
};
