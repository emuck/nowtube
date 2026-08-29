// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace microphone_service {

static constexpr int BAND_COUNT = 24;
static constexpr int LANDSCAPE_WIDTH = 480;
static constexpr int RAW_FRAME_SAMPLES = 1024;
static constexpr int FRAME_SAMPLES = 1024;

struct diagnostics {
  bool capturing = false;
  int raw = -1;
  int raw_min = -1;
  int raw_max = -1;
  int raw_pp = 0;
  float total_power = 0.0f;
  float frame_peak = 0.0f;
  float level_ref = 0.0f;
  int noise_cal_frames = 0;
  uint64_t updated_us = 0;
};

struct band_debug {
  float raw = 0.0f;
  float floor = 0.0f;
  float power = 0.0f;
  float display = 0.0f;
};

struct landscape_diagnostics {
  int sample_rate_hz = 0;
  int fft_size = 0;
  float rms = 0.0f;
  float signal_peak = 0.0f;
  float visual_ref = 0.0f;
  float dominant_freq_hz = 0.0f;
  float dominant_value = 0.0f;
  float fft_fps = 0.0f;
  uint32_t frames = 0;
  uint32_t read_timeouts = 0;
};

void init();
void start();
void set_audio_active(bool active);
void get_bands(float out[BAND_COUNT]);
void get_band_debug(band_debug out[BAND_COUNT]);
void get_landscape(float live[LANDSCAPE_WIDTH], float peak[LANDSCAPE_WIDTH]);
landscape_diagnostics get_landscape_diagnostics();
bool get_latest_frame(uint16_t raw[RAW_FRAME_SAMPLES], float dec[FRAME_SAMPLES]);
diagnostics get_diagnostics();
int read_raw();
int gpio_num();

}
