// SPDX-FileCopyrightText: 2026 Martin Raumann
// SPDX-License-Identifier: MIT

#include "services/microphone_service.h"

#ifdef ESP_PLATFORM

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_adc/adc_continuous.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <soc/soc_caps.h>

#include "mode_manager.h"
#include "services/config_service.h"

namespace microphone_service {
namespace {

constexpr const char *TAG = "mic";

// Harmonic Landscape analysis path. The ADC runs at 32 kHz so the visual can
// cover roughly 40 Hz..15.5 kHz. Amplitude is intentionally relative: the
// Nextube electret/preamp/ADC chain is useful for visual motion, not SPL.
constexpr int SAMPLE_RATE = 32000;
constexpr int FFT_SIZE = RAW_FRAME_SAMPLES;
constexpr int FFT_BINS = FFT_SIZE / 2;
constexpr int RAW_FRAME_BYTES = RAW_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES;
constexpr float FREQ_MIN_HZ = 40.0f;
constexpr float FREQ_MAX_HZ = 15500.0f;
constexpr float PI = 3.14159265358979323846f;
constexpr float DC_ALPHA = 0.995f;
constexpr float NOISE_FAST_ALPHA = 0.08f;
constexpr float NOISE_SLOW_ALPHA = 0.0018f;
constexpr int NOISE_CAL_FRAMES = 96;
constexpr float ATTACK_ALPHA = 0.82f;
constexpr float RELEASE_ALPHA = 0.20f;
constexpr float PEAK_DECAY = 0.965f;
constexpr float REF_ATTACK_ALPHA = 0.18f;
constexpr float REF_RELEASE_ALPHA = 0.004f;
constexpr float REF_FLOOR = 0.18f;
constexpr float EVENT_RAW_PP_MARGIN = 7.0f;
constexpr float EVENT_RMS_MARGIN = 0.9f;
constexpr float SPECTRAL_DEADBAND = 0.028f;
constexpr float SIGNAL_PEAK_GATE = 0.075f;
constexpr int DEBUG_FRAME_COPY_PERIOD = 16;

constexpr adc_channel_t ADC1_CHAN_MAP[8] = {
    ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3,
    ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7,
};
constexpr int ADC1_GPIO_MAP[8] = {36, 37, 38, 39, 32, 33, 34, 35};

struct column_map_entry {
  uint16_t bin = 1;
  float frac = 0.0f;
  float freq_hz = 0.0f;
};

adc_oneshot_unit_handle_t s_adc = nullptr;
adc_continuous_handle_t s_acq = nullptr;
adc_channel_t s_active_chan = ADC_CHANNEL_7;
uint8_t s_active_ch = 7;
volatile bool s_audio_active = false;
SemaphoreHandle_t s_i2s_released = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

float s_hann[FFT_SIZE] = {};
column_map_entry s_col_map[LANDSCAPE_WIDTH] = {};
float s_noise_floor[LANDSCAPE_WIDTH] = {};
float s_landscape[LANDSCAPE_WIDTH] = {};
float s_peak_trace[LANDSCAPE_WIDTH] = {};
float s_bands[BAND_COUNT] = {};
band_debug s_band_debug[BAND_COUNT] = {};
uint16_t s_latest_raw[RAW_FRAME_SAMPLES] = {};
float s_latest_dec[FRAME_SAMPLES] = {};
bool s_latest_frame_valid = false;
diagnostics s_diag = {};
landscape_diagnostics s_land_diag = {};
int s_noise_cal = 0;
volatile int s_last_raw = -1;

void fft_radix2(float *re, float *im, int n) {
  int j = 0;
  for (int i = 1; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }

  for (int len = 2; len <= n; len <<= 1) {
    const float ang = -2.0f * PI / static_cast<float>(len);
    const float wlen_re = cosf(ang);
    const float wlen_im = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float w_re = 1.0f;
      float w_im = 0.0f;
      const int half = len >> 1;
      for (int k = 0; k < half; ++k) {
        const int even = i + k;
        const int odd = even + half;
        const float u_re = re[even];
        const float u_im = im[even];
        const float v_re = re[odd] * w_re - im[odd] * w_im;
        const float v_im = re[odd] * w_im + im[odd] * w_re;
        re[even] = u_re + v_re;
        im[even] = u_im + v_im;
        re[odd] = u_re - v_re;
        im[odd] = u_im - v_im;
        const float next_re = w_re * wlen_re - w_im * wlen_im;
        w_im = w_re * wlen_im + w_im * wlen_re;
        w_re = next_re;
      }
    }
  }
}

bool acq_start() {
  if (s_acq != nullptr) return true;
  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = RAW_FRAME_BYTES * 4;
  hcfg.conv_frame_size = RAW_FRAME_BYTES;
  if (adc_continuous_new_handle(&hcfg, &s_acq) != ESP_OK) {
    s_acq = nullptr;
    return false;
  }
  adc_digi_pattern_config_t pat = {};
  pat.atten = ADC_ATTEN_DB_12;
  pat.channel = static_cast<uint8_t>(s_active_chan);
  pat.unit = ADC_UNIT_1;
  pat.bit_width = ADC_BITWIDTH_12;
  adc_continuous_config_t ccfg = {};
  ccfg.pattern_num = 1;
  ccfg.adc_pattern = &pat;
  ccfg.sample_freq_hz = SAMPLE_RATE;
  ccfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  ccfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
  if (adc_continuous_config(s_acq, &ccfg) != ESP_OK ||
      adc_continuous_start(s_acq) != ESP_OK) {
    adc_continuous_deinit(s_acq);
    s_acq = nullptr;
    return false;
  }
  return true;
}

void acq_stop() {
  if (s_acq == nullptr) return;
  adc_continuous_stop(s_acq);
  adc_continuous_deinit(s_acq);
  s_acq = nullptr;
}

void reconfigure_channel(uint8_t ch) {
  if (ch > 7) ch = 7;
  acq_stop();
  adc_oneshot_chan_cfg_t cfg = {};
  cfg.atten = ADC_ATTEN_DB_12;
  cfg.bitwidth = ADC_BITWIDTH_12;
  adc_channel_t next = ADC1_CHAN_MAP[ch];
  if (adc_oneshot_config_channel(s_adc, next, &cfg) == ESP_OK) {
    s_active_chan = next;
    s_active_ch = ch;
    ESP_LOGI(TAG, "ADC channel -> CH%u (GPIO%d)", ch, ADC1_GPIO_MAP[ch]);
  }
}

float perceptual_weight(float freq_hz) {
  if (freq_hz < 70.0f) return 0.12f;
  if (freq_hz < 160.0f) return 0.25f + 0.55f * ((freq_hz - 70.0f) / 90.0f);
  if (freq_hz < 2000.0f) return 0.85f;
  if (freq_hz < 8000.0f) return 1.25f;
  return 1.05f;
}

void smooth_spatial(float target[LANDSCAPE_WIDTH]) {
  static float tmp[LANDSCAPE_WIDTH];
  for (int pass = 0; pass < 1; ++pass) {
    tmp[0] = target[0] * 0.75f + target[1] * 0.25f;
    for (int x = 1; x < LANDSCAPE_WIDTH - 1; ++x) {
      tmp[x] = target[x - 1] * 0.18f + target[x] * 0.64f + target[x + 1] * 0.18f;
    }
    tmp[LANDSCAPE_WIDTH - 1] = target[LANDSCAPE_WIDTH - 2] * 0.25f + target[LANDSCAPE_WIDTH - 1] * 0.75f;
    memcpy(target, tmp, sizeof(tmp));
  }
}

void update_legacy_bands(const float target[LANDSCAPE_WIDTH], float raw_col[LANDSCAPE_WIDTH]) {
  constexpr int cols_per_band = LANDSCAPE_WIDTH / BAND_COUNT;
  for (int b = 0; b < BAND_COUNT; ++b) {
    const int start = b * cols_per_band;
    const int end = (b == BAND_COUNT - 1) ? LANDSCAPE_WIDTH : start + cols_per_band;
    float sum = 0.0f;
    float raw_sum = 0.0f;
    float floor_sum = 0.0f;
    float max_v = 0.0f;
    for (int x = start; x < end; ++x) {
      sum += target[x];
      raw_sum += raw_col[x];
      floor_sum += s_noise_floor[x];
      max_v = std::max(max_v, target[x]);
    }
    const float denom = static_cast<float>(std::max(1, end - start));
    s_bands[b] = std::max(sum / denom, max_v * 0.72f);
    s_band_debug[b].raw = raw_sum / denom;
    s_band_debug[b].floor = floor_sum / denom;
    s_band_debug[b].power = std::max(0.0f, s_band_debug[b].raw - s_band_debug[b].floor);
    s_band_debug[b].display = s_bands[b];
  }
}

void mic_task(void *) {
  static uint8_t rawbuf[RAW_FRAME_BYTES];
  static float re[FFT_SIZE];
  static float im[FFT_SIZE];
  static float mag[FFT_BINS + 1];
  static uint16_t latest_raw[RAW_FRAME_SAMPLES];
  static float latest_dec[FRAME_SAMPLES];
  static float target[LANDSCAPE_WIDTH];
  static float raw_col[LANDSCAPE_WIDTH];

  float dc = 2048.0f;
  float visual_ref = REF_FLOOR;
  float rms_floor = 0.0f;
  float pp_floor = 0.0f;
  uint64_t last_us = static_cast<uint64_t>(esp_timer_get_time());
  uint32_t frames = 0;
  uint32_t read_timeouts = 0;
  ESP_LOGI(TAG, "mic task running: GPIO%d adc_continuous %d Hz FFT%d", gpio_num(), SAMPLE_RATE, FFT_SIZE);

  for (;;) {
    const device_config &cfg = config_service::get_config();
    bool want_capture = cfg.mic_enabled;
    uint8_t want_ch = cfg.mic_adc_channel < 8 ? cfg.mic_adc_channel : 7;

    if (!want_capture || s_audio_active) {
      acq_stop();
      if (s_audio_active && s_i2s_released) xSemaphoreGive(s_i2s_released);
      taskENTER_CRITICAL(&s_mux);
      memset(s_bands, 0, sizeof(s_bands));
      memset(s_landscape, 0, sizeof(s_landscape));
      memset(s_peak_trace, 0, sizeof(s_peak_trace));
      s_diag.capturing = false;
      s_diag.total_power = 0.0f;
      s_diag.frame_peak = 0.0f;
      s_diag.level_ref = visual_ref;
      s_diag.noise_cal_frames = s_noise_cal;
      s_diag.updated_us = static_cast<uint64_t>(esp_timer_get_time());
      taskEXIT_CRITICAL(&s_mux);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (want_ch != s_active_ch) reconfigure_channel(want_ch);
    if (!acq_start()) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    uint32_t got = 0;
    if (adc_continuous_read(s_acq, rawbuf, RAW_FRAME_BYTES, &got, 100) != ESP_OK || got < RAW_FRAME_BYTES) {
      ++read_timeouts;
      continue;
    }

    auto *d = reinterpret_cast<adc_digi_output_data_t *>(rawbuf);
    int raw_min = 4095;
    int raw_max = 0;
    float sum_sq = 0.0f;
    for (int i = 0; i < FFT_SIZE; ++i) {
      int raw_count = static_cast<int>(d[i].type1.data);
      latest_raw[i] = static_cast<uint16_t>(raw_count);
      raw_min = std::min(raw_min, raw_count);
      raw_max = std::max(raw_max, raw_count);
      s_last_raw = raw_count;
      float raw = static_cast<float>(raw_count);
      dc = DC_ALPHA * dc + (1.0f - DC_ALPHA) * raw;
      float sample = raw - dc;
      latest_dec[i] = sample;
      sum_sq += sample * sample;
      re[i] = sample * s_hann[i];
      im[i] = 0.0f;
    }

    fft_radix2(re, im, FFT_SIZE);

    mag[0] = 0.0f;
    float dominant_value = 0.0f;
    int dominant_bin = 0;
    const int max_bin = std::min(FFT_BINS, static_cast<int>(FREQ_MAX_HZ * FFT_SIZE / SAMPLE_RATE));
    const int dominant_min_bin = std::max(1, static_cast<int>(80.0f * FFT_SIZE / SAMPLE_RATE));
    for (int k = 1; k <= FFT_BINS; ++k) {
      float m = sqrtf(re[k] * re[k] + im[k] * im[k]) / (FFT_SIZE * 0.5f);
      mag[k] = m;
      if (k >= dominant_min_bin && k <= max_bin && m > dominant_value) {
        dominant_value = m;
        dominant_bin = k;
      }
    }

    float signal_peak = 0.0f;
    float total_signal = 0.0f;
    for (int x = 0; x < LANDSCAPE_WIDTH; ++x) {
      const column_map_entry &map = s_col_map[x];
      const int bin = std::clamp<int>(map.bin, 1, FFT_BINS - 1);
      float raw = (mag[bin] * (1.0f - map.frac) + mag[bin + 1] * map.frac) *
                  perceptual_weight(map.freq_hz);
      raw_col[x] = raw;

      if (s_noise_cal < NOISE_CAL_FRAMES) {
        s_noise_floor[x] += NOISE_FAST_ALPHA * (raw - s_noise_floor[x]);
      } else if (raw < s_noise_floor[x] * 1.45f + 0.025f) {
        s_noise_floor[x] += NOISE_SLOW_ALPHA * (raw - s_noise_floor[x]);
      }

      float signal = raw - s_noise_floor[x] * 1.18f - SPECTRAL_DEADBAND;
      if (signal < 0.0f) signal = 0.0f;
      target[x] = signal;
      signal_peak = std::max(signal_peak, signal);
      total_signal += signal;
    }
    if (s_noise_cal < NOISE_CAL_FRAMES) ++s_noise_cal;

    const int raw_pp = raw_max - raw_min;
    const float rms = sqrtf(sum_sq / FFT_SIZE);
    if (s_noise_cal <= NOISE_CAL_FRAMES) {
      const float a = s_noise_cal < 8 ? 0.35f : 0.08f;
      rms_floor += a * (rms - rms_floor);
      pp_floor += a * (static_cast<float>(raw_pp) - pp_floor);
    } else if (signal_peak < SIGNAL_PEAK_GATE && rms < rms_floor + EVENT_RMS_MARGIN) {
      rms_floor += 0.002f * (rms - rms_floor);
      pp_floor += 0.002f * (static_cast<float>(raw_pp) - pp_floor);
    }

    const bool acoustic_event =
        s_noise_cal >= NOISE_CAL_FRAMES &&
        signal_peak >= SIGNAL_PEAK_GATE &&
        ((static_cast<float>(raw_pp) >= pp_floor + EVENT_RAW_PP_MARGIN) ||
         (rms >= rms_floor + EVENT_RMS_MARGIN));
    const bool quiet = !acoustic_event;
    if (signal_peak > visual_ref) visual_ref += REF_ATTACK_ALPHA * (signal_peak - visual_ref);
    else visual_ref += REF_RELEASE_ALPHA * (signal_peak - visual_ref);
    visual_ref = std::max(REF_FLOOR, visual_ref);

    const float denom = std::max(REF_FLOOR, visual_ref * 0.80f);
    for (int x = 0; x < LANDSCAPE_WIDTH; ++x) {
      float compressed = quiet ? 0.0f : log1pf(9.0f * (target[x] / denom)) / log1pf(9.0f);
      if (compressed < 0.055f) compressed = 0.0f;
      target[x] = std::clamp(compressed, 0.0f, 1.0f);
    }
    smooth_spatial(target);

    float frame_peak = 0.0f;
    for (int x = 0; x < LANDSCAPE_WIDTH; ++x) {
      if (quiet) {
        s_landscape[x] *= 0.70f;
        s_peak_trace[x] *= 0.82f;
      } else {
        const float a = target[x] > s_landscape[x] ? ATTACK_ALPHA : RELEASE_ALPHA;
        s_landscape[x] += a * (target[x] - s_landscape[x]);
        if (s_landscape[x] > s_peak_trace[x]) s_peak_trace[x] = s_landscape[x];
        else s_peak_trace[x] *= PEAK_DECAY;
      }
      if (s_peak_trace[x] < 0.002f) s_peak_trace[x] = 0.0f;
      frame_peak = std::max(frame_peak, s_landscape[x]);
    }
    update_legacy_bands(s_landscape, raw_col);

    ++frames;
    uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    float fft_fps = 0.0f;
    if (now_us > last_us) fft_fps = 1000000.0f / static_cast<float>(now_us - last_us);
    last_us = now_us;

    taskENTER_CRITICAL(&s_mux);
    if ((frames % DEBUG_FRAME_COPY_PERIOD) == 0) {
      memcpy(s_latest_raw, latest_raw, sizeof(s_latest_raw));
      memcpy(s_latest_dec, latest_dec, sizeof(s_latest_dec));
      s_latest_frame_valid = true;
    }
    s_diag.capturing = true;
    s_diag.raw = s_last_raw;
    s_diag.raw_min = raw_min;
    s_diag.raw_max = raw_max;
    s_diag.raw_pp = raw_pp;
    s_diag.total_power = total_signal;
    s_diag.frame_peak = frame_peak;
    s_diag.level_ref = visual_ref;
    s_diag.noise_cal_frames = s_noise_cal;
    s_diag.updated_us = now_us;
    s_land_diag.sample_rate_hz = SAMPLE_RATE;
    s_land_diag.fft_size = FFT_SIZE;
    s_land_diag.rms = rms;
    s_land_diag.signal_peak = signal_peak;
    s_land_diag.visual_ref = visual_ref;
    s_land_diag.dominant_freq_hz = dominant_bin * (static_cast<float>(SAMPLE_RATE) / FFT_SIZE);
    s_land_diag.dominant_value = dominant_value;
    s_land_diag.fft_fps = fft_fps;
    s_land_diag.frames = frames;
    s_land_diag.read_timeouts = read_timeouts;
    taskEXIT_CRITICAL(&s_mux);

    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

void build_hann() {
  for (int i = 0; i < FFT_SIZE; ++i) {
    s_hann[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
  }
}

void build_column_map() {
  const float log_range = logf(FREQ_MAX_HZ / FREQ_MIN_HZ);
  for (int x = 0; x < LANDSCAPE_WIDTH; ++x) {
    const float pos = static_cast<float>(x) / static_cast<float>(LANDSCAPE_WIDTH - 1);
    const float f = FREQ_MIN_HZ * expf(log_range * pos);
    const float bin_f = f * FFT_SIZE / SAMPLE_RATE;
    int bin = static_cast<int>(floorf(bin_f));
    bin = std::clamp(bin, 1, FFT_BINS - 1);
    s_col_map[x].bin = static_cast<uint16_t>(bin);
    s_col_map[x].frac = std::clamp(bin_f - bin, 0.0f, 1.0f);
    s_col_map[x].freq_hz = f;
  }
}

}  // namespace

void init() {
  const device_config &cfg = config_service::get_config();
  s_active_ch = cfg.mic_adc_channel < 8 ? cfg.mic_adc_channel : 7;
  s_active_chan = ADC1_CHAN_MAP[s_active_ch];
  build_hann();
  build_column_map();

  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = ADC_UNIT_1;
  unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));
  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten = ADC_ATTEN_DB_12;
  chan_cfg.bitwidth = ADC_BITWIDTH_12;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_active_chan, &chan_cfg));
  s_i2s_released = xSemaphoreCreateBinary();
  configASSERT(s_i2s_released);
  ESP_LOGI(TAG, "initialized on GPIO%d / ADC1_CH%u", gpio_num(), s_active_ch);
}

void start() {
  xTaskCreatePinnedToCore(mic_task, "mic", 8192, nullptr, 2, nullptr, 1);
}

void set_audio_active(bool active) {
  s_audio_active = active;
  if (active && s_i2s_released != nullptr) {
    while (xSemaphoreTake(s_i2s_released, 0) == pdTRUE) {}
    xSemaphoreTake(s_i2s_released, pdMS_TO_TICKS(400));
  }
}

void get_bands(float out[BAND_COUNT]) {
  taskENTER_CRITICAL(&s_mux);
  memcpy(out, s_bands, sizeof(s_bands));
  taskEXIT_CRITICAL(&s_mux);
}

void get_band_debug(band_debug out[BAND_COUNT]) {
  taskENTER_CRITICAL(&s_mux);
  memcpy(out, s_band_debug, sizeof(s_band_debug));
  taskEXIT_CRITICAL(&s_mux);
}

void get_landscape(float live[LANDSCAPE_WIDTH], float peak[LANDSCAPE_WIDTH]) {
  taskENTER_CRITICAL(&s_mux);
  memcpy(live, s_landscape, sizeof(s_landscape));
  memcpy(peak, s_peak_trace, sizeof(s_peak_trace));
  taskEXIT_CRITICAL(&s_mux);
}

landscape_diagnostics get_landscape_diagnostics() {
  taskENTER_CRITICAL(&s_mux);
  landscape_diagnostics d = s_land_diag;
  taskEXIT_CRITICAL(&s_mux);
  return d;
}

bool get_latest_frame(uint16_t raw[RAW_FRAME_SAMPLES], float dec[FRAME_SAMPLES]) {
  taskENTER_CRITICAL(&s_mux);
  bool valid = s_latest_frame_valid;
  if (valid) {
    memcpy(raw, s_latest_raw, sizeof(s_latest_raw));
    memcpy(dec, s_latest_dec, sizeof(s_latest_dec));
  }
  taskEXIT_CRITICAL(&s_mux);
  return valid;
}

diagnostics get_diagnostics() {
  taskENTER_CRITICAL(&s_mux);
  diagnostics d = s_diag;
  taskEXIT_CRITICAL(&s_mux);
  return d;
}

int read_raw() {
  if (s_acq == nullptr && s_adc != nullptr) {
    int raw = -1;
    if (adc_oneshot_read(s_adc, s_active_chan, &raw) == ESP_OK) s_last_raw = raw;
  }
  return s_last_raw;
}

int gpio_num() {
  return ADC1_GPIO_MAP[s_active_ch < 8 ? s_active_ch : 7];
}

}  // namespace microphone_service

#else
namespace microphone_service {
void init() {}
void start() {}
void set_audio_active(bool) {}
void get_bands(float out[BAND_COUNT]) { for (int i = 0; i < BAND_COUNT; ++i) out[i] = 0.0f; }
void get_band_debug(band_debug out[BAND_COUNT]) { for (int i = 0; i < BAND_COUNT; ++i) out[i] = {}; }
void get_landscape(float live[LANDSCAPE_WIDTH], float peak[LANDSCAPE_WIDTH]) {
  for (int i = 0; i < LANDSCAPE_WIDTH; ++i) { live[i] = 0.0f; peak[i] = 0.0f; }
}
landscape_diagnostics get_landscape_diagnostics() { return {}; }
bool get_latest_frame(uint16_t raw[RAW_FRAME_SAMPLES], float dec[FRAME_SAMPLES]) {
  for (int i = 0; i < RAW_FRAME_SAMPLES; ++i) raw[i] = 0;
  for (int i = 0; i < FRAME_SAMPLES; ++i) dec[i] = 0.0f;
  return false;
}
diagnostics get_diagnostics() { return {}; }
int read_raw() { return -1; }
int gpio_num() { return 35; }
}
#endif
