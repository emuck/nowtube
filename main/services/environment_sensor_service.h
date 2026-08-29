// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

namespace environment_sensor_service {

struct reading {
  float temp_c = 0.0f;
  float humidity_pct = 0.0f;
  bool valid = false;
};

bool init();
void start();
bool is_present();
bool get(reading &out);

}
