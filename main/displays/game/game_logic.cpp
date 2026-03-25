#include "game_logic.h"

// ---------------------------------------------------------------------------
// Internal helpers

static uint32_t xorshift32(uint32_t &state) {
    // state must never be zero; caller is responsible for seeding with non-zero.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// ---------------------------------------------------------------------------

void game_tick(GameState &s, uint32_t now_ms) {
    if (s.phase != GamePhase::PLAYING) return;

    s.tick_ms = now_ms;

    // ---- Projectile ----
    Projectile &p = s.projectile;
    if (p.active) {
        p.y -= PROJ_STEP_PX;
        if (p.y + PROJECTILE_H <= 0) {
            // Left screen top — deactivate.
            p.active = false;
        }
    }

    // ---- Enemy (slot 0 — single enemy) ----
    Enemy &e = s.enemies[0];

    if (e.active) {
        e.y += s.step_px;

        // Collision: projectile and enemy overlap in the same lane.
        if (p.active && p.lane == e.lane &&
            p.y < e.y + ENEMY_H && p.y + PROJECTILE_H > e.y) {
            p.active = false;
            e.active = false;
            s.score++;
            // Difficulty: level up every KILLS_PER_LEVEL kills, capped at MAX_SPEED_LEVEL.
            int new_level = s.score / KILLS_PER_LEVEL;
            if (new_level > MAX_SPEED_LEVEL) new_level = MAX_SPEED_LEVEL;
            if (new_level != s.speed_level) {
                s.speed_level       = new_level;
                s.step_px           = BASE_STEP_PX + new_level;
                s.spawn_interval_ms = BASE_SPAWN_MS - (uint32_t)new_level * SPAWN_REDUCTION_MS;
            }
        } else if (e.y >= SHIP_Y) {
            e.active = false;           // enemy consumed on defeat
            s.phase  = GamePhase::GAME_OVER;
        }
    } else {
        // Spawn when interval has elapsed.
        if (now_ms - s.last_spawn_ms >= s.spawn_interval_ms) {
            e.active        = true;
            e.lane          = (int)(xorshift32(s.rand_seed) % (uint32_t)NUM_LANES);
            e.y             = -ENEMY_H; // start just above the top edge
            s.last_spawn_ms = now_ms;
        }
    }
}

void game_input_move(GameState &s, int delta) {
    if (s.phase != GamePhase::PLAYING) return;
    int next = s.player_lane + delta;
    if (next < 0)          next = 0;
    if (next >= NUM_LANES) next = NUM_LANES - 1;
    s.player_lane = next;
}

void game_input_fire(GameState &s) {
    if (s.phase != GamePhase::PLAYING) return;
    if (s.projectile.active) return;   // one projectile at a time
    s.projectile.active = true;
    s.projectile.lane   = s.player_lane;
    s.projectile.y      = SHIP_Y - PROJECTILE_H;  // just above the ship
}

void game_reset(GameState &s) {
    s = GameState{};
}
