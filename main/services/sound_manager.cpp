#include "sound_manager.h"

#ifdef ESP_PLATFORM

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "driver/dac_cosine.h"

namespace sound_manager {

// ---------------------------------------------------------------------------
// Tone sequences

struct Segment { uint32_t freq_hz; uint32_t duration_ms; };

// Arcade-style tones: short, stepped, synthetic.
// fire     — descending two-note "pew" (C6→G5, 45 ms)
// hit      — ascending three-note chirp (E5→B5→E6, 70 ms)
// game_over — descending four-note dirge (A4→E4→C4→A3, 320 ms)
// restart  — ascending C-major fanfare (C5→E5→G5→C6, 165 ms)
static constexpr Segment k_fire[]     = {{1047, 20}, {784,  25}};
static constexpr Segment k_hit[]      = {{660,  20}, {990,  20}, {1320, 30}};
static constexpr Segment k_gameover[] = {{440,  70}, {330,  70}, {262,  70}, {220, 110}};
static constexpr Segment k_restart[]  = {{523,  35}, {659,  35}, {784,  35}, {1047, 60}};
// blocked — short descending "nope" cue (E4→A3, 35 ms)
static constexpr Segment k_blocked[]  = {{330,  15}, {220,  20}};

struct SoundDef { const Segment *segs; int n; };

static constexpr SoundDef k_sounds[] = {
    {k_fire,     2},  // FIRE
    {k_hit,      3},  // HIT
    {k_gameover, 4},  // GAME_OVER
    {k_restart,  4},  // RESTART
    {k_blocked,  2},  // BLOCKED
};
static constexpr int k_num_sounds =
    static_cast<int>(sizeof(k_sounds) / sizeof(k_sounds[0]));

// ---------------------------------------------------------------------------

static QueueHandle_t s_queue = nullptr;

// Play a single tone segment. Creates and destroys the DAC handle around the
// delay so the next segment can use a different frequency.
static void play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    dac_cosine_config_t cfg = {
        .chan_id  = DAC_CHAN_0,              // GPIO25 = DAC1, confirmed in Phase A
        .freq_hz  = freq_hz,
        .clk_src  = DAC_COSINE_CLK_SRC_DEFAULT,
        .atten    = DAC_COSINE_ATTEN_DEFAULT,
        .phase    = DAC_COSINE_PHASE_0,
        .offset   = 0,
        .flags    = {.force_set_freq = false},
    };
    dac_cosine_handle_t handle = nullptr;
    if (dac_cosine_new_channel(&cfg, &handle) != ESP_OK) return;
    dac_cosine_start(handle);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    dac_cosine_stop(handle);
    dac_cosine_del_channel(handle);
}

static void sound_task_fn([[maybe_unused]] void *arg) {
    for (;;) {
        SoundEvent event;
        xQueueReceive(s_queue, &event, portMAX_DELAY);

        const int idx = static_cast<int>(event);
        if (idx < 0 || idx >= k_num_sounds) continue;

        const SoundDef &def = k_sounds[idx];
        for (int i = 0; i < def.n; ++i) {
            play_tone(def.segs[i].freq_hz, def.segs[i].duration_ms);
        }
    }
}

// ---------------------------------------------------------------------------

void init() {
    s_queue = xQueueCreate(1, sizeof(SoundEvent));
    configASSERT(s_queue);
    xTaskCreate(sound_task_fn, "sound_task", 3072, nullptr, 2, nullptr);
}

void play(SoundEvent event) {
    if (s_queue == nullptr) return;
    xQueueOverwrite(s_queue, &event);
}

}  // namespace sound_manager

#endif  // ESP_PLATFORM
