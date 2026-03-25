#include "game_display.h"

#ifdef ESP_PLATFORM
#include <cstdio>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <nvs.h>

#include "displays/clock/clock.h"
#include "displays/game/game_logic.h"
#include "drivers/lcds.h"
#include "gui.h"
#include "led_manager.h"
#include "lvgl.h"
#include "services/backlight_service.h"
#include "services/sound_manager.h"

static const char *TAG = "game_display";

// ---------------------------------------------------------------------------
// Rendering constants

static constexpr int SHIP_W  = 30;
static constexpr int ENEMY_W = 30;
static constexpr int PROJ_W  = 6;
static constexpr int TICK_MS = 50;  // 20 fps

// ---------------------------------------------------------------------------
// Module state

static volatile bool     s_active      = false;
static GameState         s_state       = {};
static SemaphoreHandle_t s_state_mutex = nullptr;
static TaskHandle_t      s_game_task   = nullptr;

// LVGL object handles — only accessed under gui_lvgl_lock().
static lv_obj_t *s_ship_obj       = nullptr;
static int       s_ship_disp      = -1;
static lv_obj_t *s_enemy_obj      = nullptr;
static int       s_enemy_disp     = -1;
static lv_obj_t *s_proj_obj       = nullptr;
static int       s_proj_disp      = -1;
static lv_obj_t *s_score_label    = nullptr;
static int       s_last_score     = -1;
static lv_obj_t *s_gameover_label = nullptr;
static lv_obj_t *s_hiscore_label  = nullptr;

// High score — loaded at game entry, saved on game over.
static int s_high_score = 0;

static void load_high_score() {
    nvs_handle_t h;
    if (nvs_open("game", NVS_READONLY, &h) != ESP_OK) { s_high_score = 0; return; }
    uint32_t val = 0;
    nvs_get_u32(h, "hi_score", &val);  // ESP_ERR_NVS_NOT_FOUND on first boot → val stays 0
    nvs_close(h);
    s_high_score = static_cast<int>(val);
}

static void save_high_score(int score) {
    if (score <= s_high_score) return;
    s_high_score = score;
    nvs_handle_t h;
    if (nvs_open("game", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "hi_score", static_cast<uint32_t>(score));
    nvs_commit(h);
    nvs_close(h);
}

// LED flash state — game_display only, no mutex needed (game task only).
static uint32_t   s_hit_flash_until_ms = 0;
static int        s_hit_flash_lane     = -1;
static int        s_prev_score         = 0;
static GamePhase  s_prev_phase         = GamePhase::PLAYING;

namespace game_display {

// ---------------------------------------------------------------------------
// Internal: LVGL helpers — must be called under gui_lvgl_lock()

static void make_rect(lv_obj_t *parent, lv_obj_t **out,
                      int x, int y, int w, int h, uint32_t color_hex) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color_hex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    *out = obj;
}

// Update per-lane LEDs to reflect game state.
// Safe to call from any task context without locks.
static void render_leds(const GameState &snap, uint32_t now_ms) {
    auto &leds = led_manager::get();

    // Detect game reset (score wrapped back to 0 after a restart).
    if (snap.score < s_prev_score) {
        s_prev_score = 0;
        s_prev_phase = GamePhase::PLAYING;
    }

    // Phase transition: PLAYING → GAME_OVER.
    if (snap.phase == GamePhase::GAME_OVER && s_prev_phase == GamePhase::PLAYING) {
        save_high_score(snap.score);
        sound_manager::play(sound_manager::SoundEvent::GAME_OVER);
    }
    s_prev_phase = snap.phase;

    if (snap.phase == GamePhase::GAME_OVER) {
        leds.set_all_rgb(60, 0, 0);
        leds.flush();
        return;
    }

    // Detect score increase → hit flash + sound.
    if (snap.score > s_prev_score) {
        s_hit_flash_until_ms = now_ms + 150;
        s_hit_flash_lane     = snap.enemies[0].lane;  // preserved after deactivation
        s_prev_score         = snap.score;
        sound_manager::play(sound_manager::SoundEvent::HIT);
    }

    for (int i = 0; i < NUM_LANES; ++i) {
        if (now_ms < s_hit_flash_until_ms && i == s_hit_flash_lane) {
            leds.set_rgb_silent(i, 200, 200, 200);  // white flash on kill lane
        } else if (i == snap.player_lane) {
            leds.set_rgb_silent(i, 0, 0, 70);       // blue: player lane
        } else if (snap.enemies[0].active && i == snap.enemies[0].lane) {
            leds.set_rgb_silent(i, 40, 0, 0);       // dim red: incoming enemy
        } else {
            leds.set_rgb_silent(i, 0, 0, 0);        // off
        }
    }
    leds.flush();
}

// Synchronise all LVGL objects to match |snap|.
// Must be called under gui_lvgl_lock().
static void render_frame(const GameState &snap) {

    // ---- Ship ----
    if (snap.phase == GamePhase::PLAYING) {
        if (s_ship_disp != snap.player_lane) {
            if (s_ship_obj != nullptr) {
                lv_obj_del(s_ship_obj);
                s_ship_obj = nullptr;
            }
            lv_disp_set_default(gui_get_display(snap.player_lane));
            make_rect(lv_scr_act(), &s_ship_obj,
                      (LANE_W - SHIP_W) / 2, SHIP_Y, SHIP_W, SHIP_H, 0xFCF9D9);
            s_ship_disp = snap.player_lane;
        }
        lv_obj_clear_flag(s_ship_obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Hide (not delete) so restart can unhide without recreating.
        if (s_ship_obj != nullptr) {
            lv_obj_add_flag(s_ship_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---- Projectile ----
    const Projectile &p = snap.projectile;
    if (p.active) {
        if (s_proj_disp != p.lane) {
            if (s_proj_obj != nullptr) {
                lv_obj_del(s_proj_obj);
                s_proj_obj = nullptr;
            }
            lv_disp_set_default(gui_get_display(p.lane));
            make_rect(lv_scr_act(), &s_proj_obj,
                      (LANE_W - PROJ_W) / 2, p.y, PROJ_W, PROJECTILE_H, 0xFFFF55);
            s_proj_disp = p.lane;
        } else {
            lv_obj_set_pos(s_proj_obj, (LANE_W - PROJ_W) / 2, p.y);
        }
    } else {
        if (s_proj_obj != nullptr) {
            lv_obj_del(s_proj_obj);
            s_proj_obj  = nullptr;
            s_proj_disp = -1;
        }
    }

    // ---- Enemy (slot 0 — single enemy) ----
    const Enemy &e = snap.enemies[0];
    if (e.active) {
        if (s_enemy_disp != e.lane) {
            // Lane changed or first appearance: recreate on correct display.
            if (s_enemy_obj != nullptr) {
                lv_obj_del(s_enemy_obj);
                s_enemy_obj = nullptr;
            }
            lv_disp_set_default(gui_get_display(e.lane));
            make_rect(lv_scr_act(), &s_enemy_obj,
                      (LANE_W - ENEMY_W) / 2, e.y, ENEMY_W, ENEMY_H, 0xFF4444);
            s_enemy_disp = e.lane;
        } else {
            // Same lane: just move it.
            lv_obj_set_pos(s_enemy_obj, (LANE_W - ENEMY_W) / 2, e.y);
        }
    } else {
        if (s_enemy_obj != nullptr) {
            lv_obj_del(s_enemy_obj);
            s_enemy_obj  = nullptr;
            s_enemy_disp = -1;
        }
    }

    // ---- Score ----
    if (snap.phase == GamePhase::PLAYING) {
        if (s_score_label == nullptr) {
            lv_disp_set_default(gui_get_display(5));
            s_score_label = lv_label_create(lv_scr_act());
            lv_obj_set_style_text_font(s_score_label,
                                       &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(s_score_label,
                                        lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_score_label, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_align(s_score_label, LV_ALIGN_TOP_MID, 0, 4);
            s_last_score = -1;  // force first update
        }
        if (snap.score != s_last_score) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", snap.score);
            lv_label_set_text(s_score_label, buf);
            s_last_score = snap.score;
        }
    } else {
        if (s_score_label != nullptr) {
            lv_obj_del(s_score_label);
            s_score_label = nullptr;
            s_last_score  = -1;
        }
    }

    // ---- Game-over overlay (panel 2) + high score (panel 5) ----
    if (snap.phase == GamePhase::GAME_OVER) {
        if (s_gameover_label == nullptr) {
            lv_disp_set_default(gui_get_display(2));
            s_gameover_label = lv_label_create(lv_scr_act());
            lv_label_set_text(s_gameover_label, "GAME\nOVER");
            lv_obj_set_style_text_font(s_gameover_label,
                                       &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(s_gameover_label,
                                        lv_color_hex(0xFF4444), LV_PART_MAIN);
            lv_obj_set_style_text_align(s_gameover_label,
                                        LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_gameover_label, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_align(s_gameover_label, LV_ALIGN_CENTER, 0, 0);
        }
        if (s_hiscore_label == nullptr) {
            lv_disp_set_default(gui_get_display(5));
            s_hiscore_label = lv_label_create(lv_scr_act());
            lv_obj_set_style_text_font(s_hiscore_label,
                                       &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(s_hiscore_label,
                                        lv_color_hex(0xFFCC44), LV_PART_MAIN);
            lv_obj_set_style_text_align(s_hiscore_label,
                                        LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_hiscore_label, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_align(s_hiscore_label, LV_ALIGN_CENTER, 0, 0);
            char buf[16];
            snprintf(buf, sizeof(buf), "BEST\n%d", s_high_score);
            lv_label_set_text(s_hiscore_label, buf);
        }
    } else {
        if (s_gameover_label != nullptr) {
            lv_obj_del(s_gameover_label);
            s_gameover_label = nullptr;
        }
        if (s_hiscore_label != nullptr) {
            lv_obj_del(s_hiscore_label);
            s_hiscore_label = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// Game loop task — 20 fps while s_active

static void game_task_fn([[maybe_unused]] void *arg) {
    while (s_active) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        if (!s_active) break;

        const uint32_t now_ms =
            static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

        // Tick and snapshot under state mutex (brief hold — no blocking inside).
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        game_tick(s_state, now_ms);
        const GameState snap = s_state;
        xSemaphoreGive(s_state_mutex);

        // Update LEDs — no lock needed, safe from any task.
        render_leds(snap, now_ms);

        // Render under LVGL lock; re-check s_active in case clear() ran while
        // we were waiting for the lock (clear() holds it during apply_mode).
        gui_lvgl_lock();
        if (s_active) {
            render_frame(snap);
        }
        gui_lvgl_unlock();
        gui_wake_lvgl_task();
    }
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------

void show() {
    if (s_active) return;
    s_active = true;

    // Fresh mutex each entry — clears any stale lock state from prior session.
    if (s_state_mutex != nullptr) {
        vSemaphoreDelete(s_state_mutex);
    }
    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex);

    load_high_score();
    game_reset(s_state);
    // Seed random lane selection; | 1 ensures seed is never zero.
    s_state.rand_seed =
        (static_cast<uint32_t>(esp_timer_get_time() & 0xFFFFFFFFULL)) | 1u;

    // Take over LEDs for the duration of the game.
    backlight_service::pause();
    s_hit_flash_until_ms = 0;
    s_hit_flash_lane     = -1;
    s_prev_score         = 0;
    s_prev_phase         = GamePhase::PLAYING;
    {
        auto &leds = led_manager::get();
        for (int i = 0; i < NUM_LANES; ++i) {
            leds.set_rgb_silent(i, 0, 0, (i == s_state.player_lane) ? 70u : 0u);
        }
        leds.flush();
    }

    clock::get().hide_all();

    for (int i = 0; i < NUM_LCDS; ++i) {
        lv_disp_set_default(gui_get_display(i));
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x141414), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    }

    render_frame(s_state);  // places ship on starting lane; sets s_ship_disp

    xTaskCreate(game_task_fn, "game_task", 4096, nullptr, 4, &s_game_task);

    ESP_LOGI(TAG, "Game: started, ship lane %d", s_state.player_lane);
}

void clear() {
    if (!s_active) return;
    s_active = false;

    // Delete the game task.  clear() is called under gui_lvgl_lock() from
    // display_controller::apply_mode().  The game task can only hold that lock
    // inside render_frame(), which it can't reach right now (we hold it).
    // So the task is either in vTaskDelay, waiting for the lock, or between
    // them — all safe positions for vTaskDelete().
    if (s_game_task != nullptr) {
        vTaskDelete(s_game_task);
        s_game_task = nullptr;
    }

    if (s_state_mutex != nullptr) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
    }

    // Remove all game LVGL objects (already under gui_lvgl_lock()).
    if (s_ship_obj       != nullptr) { lv_obj_del(s_ship_obj);       s_ship_obj       = nullptr; }
    if (s_enemy_obj      != nullptr) { lv_obj_del(s_enemy_obj);      s_enemy_obj      = nullptr; }
    if (s_proj_obj       != nullptr) { lv_obj_del(s_proj_obj);       s_proj_obj       = nullptr; }
    if (s_score_label    != nullptr) { lv_obj_del(s_score_label);    s_score_label    = nullptr; }
    if (s_gameover_label != nullptr) { lv_obj_del(s_gameover_label); s_gameover_label = nullptr; }
    if (s_hiscore_label  != nullptr) { lv_obj_del(s_hiscore_label);  s_hiscore_label  = nullptr; }
    s_ship_disp  = -1;
    s_enemy_disp = -1;
    s_proj_disp  = -1;
    s_last_score = -1;
    s_hit_flash_until_ms = 0;
    s_hit_flash_lane     = -1;
    s_prev_score         = 0;
    s_prev_phase         = GamePhase::PLAYING;

    // Return LED control to the backlight service.
    backlight_service::resume();

    clock::get().restore_all();
    clock::get().update();

    ESP_LOGI(TAG, "Game: cleared");
}

void handle_input(touchpad_button_t button) {
    if (!s_active || s_state_mutex == nullptr) return;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (s_state.phase == GamePhase::GAME_OVER) {
        // Any button restarts.  Game task picks up the reset on its next tick.
        game_reset(s_state);
        s_state.rand_seed =
            (static_cast<uint32_t>(esp_timer_get_time() & 0xFFFFFFFFULL)) | 1u;
        sound_manager::play(sound_manager::SoundEvent::RESTART);
    } else {
        switch (button) {
        case TOUCHPAD_LEFT_BUTTON:  game_input_move(s_state, -1); break;
        case TOUCHPAD_RIGHT_BUTTON: game_input_move(s_state, +1); break;
        case TOUCHPAD_MIDDLE_BUTTON: {
            const bool was_inactive = !s_state.projectile.active;
            game_input_fire(s_state);
            if (was_inactive && s_state.projectile.active) {
                sound_manager::play(sound_manager::SoundEvent::FIRE);
            } else if (!was_inactive) {
                sound_manager::play(sound_manager::SoundEvent::BLOCKED);
            }
            break;
        }
        default: break;
        }
    }

    xSemaphoreGive(s_state_mutex);
    // Rendering deferred to game task (≤ TICK_MS ms latency — imperceptible).
}

}  // namespace game_display

#endif  // ESP_PLATFORM
