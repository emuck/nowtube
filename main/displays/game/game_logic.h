#pragma once

#include <cstdint>

// Pure C++17 game logic — no ESP-IDF, no FreeRTOS, no LVGL.
// All structs and functions here are host-testable.

constexpr int NUM_LANES     = 6;
constexpr int LANE_W        = 80;
constexpr int LANE_H        = 162;
constexpr int SHIP_H        = 12;
constexpr int SHIP_Y        = LANE_H - SHIP_H - 4;
constexpr int ENEMY_H       = 18;
constexpr int PROJECTILE_H  = 14;
constexpr int      PROJ_STEP_PX      = 6;     // projectile moves up this many px per tick
constexpr int      MAX_ENEMIES       = NUM_LANES;
constexpr int      BASE_STEP_PX      = 2;     // starting enemy descent speed (px/tick)
constexpr uint32_t BASE_SPAWN_MS     = 2000;  // starting spawn interval (ms)
constexpr int      MAX_SPEED_LEVEL   = 4;     // difficulty caps at this level
constexpr int      KILLS_PER_LEVEL   = 8;     // kills required to advance one difficulty level
constexpr uint32_t SPAWN_REDUCTION_MS = 200;  // spawn interval shrinks by this many ms per level

struct Enemy {
    bool active = false;
    int  lane   = 0;
    int  y      = 0;   // top edge, px from top of display
};

struct Projectile {
    bool active = false;
    int  lane   = 0;
    int  y      = 0;
};

enum class GamePhase { PLAYING, GAME_OVER };

struct GameState {
    GamePhase  phase             = GamePhase::PLAYING;
    int        player_lane       = 2;
    Projectile projectile        = {};
    Enemy      enemies[MAX_ENEMIES] = {};
    int        score             = 0;
    int        speed_level       = 0;
    int        step_px           = 2;
    uint32_t   spawn_interval_ms = 2000;
    uint32_t   last_spawn_ms     = 0;
    uint32_t   tick_ms           = 0;
    uint32_t   rand_seed         = 1;   // xorshift32 state; must never be zero
};

// Pure functions — no side effects except mutating state.
void game_tick(GameState &s, uint32_t now_ms);
void game_input_move(GameState &s, int delta);   // delta = -1 (left) or +1 (right)
void game_input_fire(GameState &s);
void game_reset(GameState &s);
