// SPDX-FileCopyrightText: 2026 Martin Raumann <mraumann@gmail.com>
// SPDX-License-Identifier: MIT
//
// nowtube I2S microphone pin probe — Phase 2: PDM digital mic sweep
//
// ADC probing (Phase 1) confirmed all ADC1 input pins show 0.0 RMS with and
// without audio — the mic is NOT an analog mic.
//
// This firmware tests PDM digital microphone mode (I2S0 PDM RX).
// A PDM mic needs two signals:
//   CLK  — clock output from ESP32 to the mic  (any output-capable GPIO)
//   DIN  — data input from mic to ESP32         (any GPIO, incl. input-only)
//
// The probe iterates over all plausible CLK×DIN combinations, prints the
// raw RMS for quiet and loud (play audio near the device), and flags the
// combination that shows the most signal variance.
//
// Build + flash:
//   cd tools/i2s_probe
//   source ~/esp/esp-idf/export.sh
//   idf.py -p /dev/ttyUSB0 build flash monitor
//
// Restore main firmware:
//   cd ../..
//   idf.py -p /dev/ttyUSB0 build flash monitor

#include <cstdio>
#include <cmath>

#include <driver/i2s_pdm.h>
#include <driver/i2s_common.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "i2s_probe";

// ---------------------------------------------------------------------------
// Candidate GPIOs
//
// CLK candidates — output-capable GPIOs free in probe firmware (no SPI/I2C/LED init):
//   26 = LCD2 CS / I2S0 BCK default    ← #1
//   27 = LCD Reset                      ← #2
//   25 = DAC pin / I2S0 WS default     ← #3
//   33 = LCD1 CS (free in probe)        ← #4
//   21 = LCD3 CS (free in probe)        ← #5
//   18 = LCD6 CS (free in probe)        ← #6
//    5 = LCD5 CS (free in probe)        ← #7
//    0 = LCD4 CS (free in probe, boot-safe after boot) ← #8
//
// DIN candidates — any GPIO that can be input in probe firmware:
//   34 = ADC1_CH6 / I2S0 DIN default   ← #1
//   35 = ADC1_CH7
//   36 = SENSOR_VP
//   39 = SENSOR_VN
//   32 = WS2812 data pin (free in probe, no RMT init)
//   33 = also usable as input if not chosen as CLK
// ---------------------------------------------------------------------------
static const int CLK_PINS[] = {26, 27, 25, 33, 21, 18, 5};
static const int DIN_PINS[] = {34, 35, 36, 39, 32};
static constexpr int N_CLK = sizeof(CLK_PINS) / sizeof(CLK_PINS[0]);
static constexpr int N_DIN = sizeof(DIN_PINS) / sizeof(DIN_PINS[0]);

static constexpr int    PDM_SAMPLE_RATE = 16000;
static constexpr size_t READ_BUF_WORDS  = 512;  // int16 words per read
static constexpr int    READS_PER_PROBE = 8;    // ~500ms total at 16kHz

// ---------------------------------------------------------------------------
// Probe one CLK×DIN combination. Returns RMS of int16 samples.
// Returns -1.0 on init failure.
// ---------------------------------------------------------------------------
static float probe_pdm(int clk_gpio, int din_gpio) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_chan_handle_t rx_handle = nullptr;

    if (i2s_new_channel(&chan_cfg, nullptr, &rx_handle) != ESP_OK) {
        return -1.0f;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PDM_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = static_cast<gpio_num_t>(clk_gpio),
            .din = static_cast<gpio_num_t>(din_gpio),
            .invert_flags = {.clk_inv = false},
        },
    };

    if (i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_cfg) != ESP_OK) {
        i2s_del_channel(rx_handle);
        return -1.0f;
    }
    if (i2s_channel_enable(rx_handle) != ESP_OK) {
        i2s_del_channel(rx_handle);
        return -1.0f;
    }

    static int16_t buf[READ_BUF_WORDS];
    int64_t  sum_sq    = 0;
    int      total     = 0;
    size_t   bytes_read;

    for (int r = 0; r < READS_PER_PROBE; r++) {
        if (i2s_channel_read(rx_handle, buf, sizeof(buf),
                             &bytes_read, pdMS_TO_TICKS(200)) == ESP_OK) {
            int n = (int)(bytes_read / sizeof(int16_t));
            for (int i = 0; i < n; i++) {
                sum_sq += (int64_t)buf[i] * buf[i];
                total++;
            }
        }
    }

    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);

    return (total > 0) ? sqrtf((float)sum_sq / (float)total) : 0.0f;
}

// ---------------------------------------------------------------------------
// Measure all combinations and print a table row per combo.
// Returns the combo index with the highest reading.
// ---------------------------------------------------------------------------
static int probe_all(float results[N_CLK][N_DIN]) {
    int best_clk = 0, best_din = 0;
    float best_val = -1.0f;

    for (int c = 0; c < N_CLK; c++) {
        for (int d = 0; d < N_DIN; d++) {
            results[c][d] = probe_pdm(CLK_PINS[c], DIN_PINS[d]);
            if (results[c][d] > best_val) {
                best_val = results[c][d];
                best_clk = c;
                best_din = d;
            }
            // Brief pause so we can see the readings as they come in
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    return best_clk * N_DIN + best_din;
}

static void print_table(float results[N_CLK][N_DIN],
                        float baseline[N_CLK][N_DIN],
                        bool is_baseline) {
    printf("  %-8s", "CLK\\DIN");
    for (int d = 0; d < N_DIN; d++) printf("  GPIO%-3d", DIN_PINS[d]);
    printf("\n");

    for (int c = 0; c < N_CLK; c++) {
        printf("  GPIO%-3d ", CLK_PINS[c]);
        for (int d = 0; d < N_DIN; d++) {
            float v = results[c][d];
            if (v < 0.0f) {
                printf("  %-7s", "ERR");
            } else if (is_baseline) {
                printf("  %-7.0f", v);
            } else {
                float delta = v - baseline[c][d];
                bool  sig   = (delta > 1000.0f && v > baseline[c][d] * 2.0f);
                printf("  %-5.0f%s", v, sig ? "◄" : "  ");
            }
        }
        printf("\n");
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
extern "C" void app_main() {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║      nowtube — PDM Microphone Pin Probe (Phase 2)             ║\n");
    printf("╠════════════════════════════════════════════════════════════════╣\n");
    printf("║  ADC probe confirmed: mic is digital (PDM), not analog.       ║\n");
    printf("║  This probe sweeps CLK x DIN combinations via I2S0 PDM RX.   ║\n");
    printf("║  Step 1: stay quiet for baseline                               ║\n");
    printf("║  Step 2: play a tone or whistle near the device               ║\n");
    printf("║  The working combo will show ◄ in the signal table.           ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n\n");

    printf("CLK candidates (output): GPIO%d (I2S0 BCK default), GPIO%d (LCD reset), GPIO%d (DAC pin)\n",
           CLK_PINS[0], CLK_PINS[1], CLK_PINS[2]);
    printf("DIN candidates (input):  GPIO%d (I2S0 DIN default), GPIO%d, GPIO%d, GPIO%d\n\n",
           DIN_PINS[0], DIN_PINS[1], DIN_PINS[2], DIN_PINS[3]);

    // ---- Baseline (silence) ------------------------------------------------
    printf("Step 1: Measuring baseline — keep quiet for ~10 seconds...\n\n");
    vTaskDelay(pdMS_TO_TICKS(2000));

    float baseline[N_CLK][N_DIN] = {};
    probe_all(baseline);

    printf("Baseline table (RMS in quiet):\n");
    print_table(baseline, baseline, true);

    // ---- Signal measurement ------------------------------------------------
    printf("Step 2: Now play audio or whistle near the device.\n");
    printf("        Checking all %d combinations — takes ~%d seconds...\n\n",
           N_CLK * N_DIN, N_CLK * N_DIN * READS_PER_PROBE / 10);
    vTaskDelay(pdMS_TO_TICKS(2000));

    printf("(Press Ctrl+] to exit)\n\n");

    int pass = 0;
    while (true) {
        pass++;
        float live[N_CLK][N_DIN] = {};
        probe_all(live);

        printf("Pass %d — Signal table (◄ = delta>1000 AND 2x baseline):\n", pass);
        print_table(live, baseline, false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
