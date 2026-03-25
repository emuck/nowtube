// Host-side tests for game_logic.h / game_logic.cpp.
// No ESP-IDF, FreeRTOS, or LVGL dependencies.

#include "displays/game/game_logic.h"

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal test framework (same pattern as other test files in this suite)
// ---------------------------------------------------------------------------

static int s_pass = 0;
static int s_fail = 0;
static const char *s_suite = "";

#define SUITE(name) do { s_suite = (name); } while (0)

#define CHECK(expr) \
  do { \
    if (expr) { s_pass++; } \
    else { \
      s_fail++; \
      fprintf(stderr, "FAIL [%s] %s:%d — %s\n", s_suite, __FILE__, __LINE__, #expr); \
    } \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GameState fresh() {
    GameState s;
    game_reset(s);
    return s;
}

// ---------------------------------------------------------------------------
// game_reset
// ---------------------------------------------------------------------------

static void test_reset_defaults() {
    SUITE("reset_defaults");
    GameState s;
    s.player_lane = 5;
    s.score       = 99;
    s.phase       = GamePhase::GAME_OVER;
    game_reset(s);
    CHECK_EQ(s.player_lane,       2);
    CHECK_EQ(s.score,             0);
    CHECK_EQ(s.phase,             GamePhase::PLAYING);
    CHECK_EQ(s.projectile.active, false);
    CHECK_EQ(s.step_px,           2);
    CHECK_EQ(s.rand_seed,         1u);   // default seed must be non-zero
}

// ---------------------------------------------------------------------------
// game_input_move — normal movement
// ---------------------------------------------------------------------------

static void test_move_left() {
    SUITE("move_left");
    GameState s = fresh();
    // Default lane is 2; move left once → 1.
    game_input_move(s, -1);
    CHECK_EQ(s.player_lane, 1);
}

static void test_move_right() {
    SUITE("move_right");
    GameState s = fresh();
    game_input_move(s, +1);
    CHECK_EQ(s.player_lane, 3);
}

static void test_move_multi_left() {
    SUITE("move_multi_left");
    GameState s = fresh();
    game_input_move(s, -1);
    game_input_move(s, -1);
    CHECK_EQ(s.player_lane, 0);
}

static void test_move_multi_right() {
    SUITE("move_multi_right");
    GameState s = fresh();
    game_input_move(s, +1);
    game_input_move(s, +1);
    game_input_move(s, +1);
    CHECK_EQ(s.player_lane, 5);
}

// ---------------------------------------------------------------------------
// game_input_move — clamping at boundaries
// ---------------------------------------------------------------------------

static void test_clamp_left_boundary() {
    SUITE("clamp_left");
    GameState s = fresh();
    s.player_lane = 0;
    game_input_move(s, -1);
    CHECK_EQ(s.player_lane, 0);  // must not go below 0
    game_input_move(s, -1);
    CHECK_EQ(s.player_lane, 0);
}

static void test_clamp_right_boundary() {
    SUITE("clamp_right");
    GameState s = fresh();
    s.player_lane = NUM_LANES - 1;
    game_input_move(s, +1);
    CHECK_EQ(s.player_lane, NUM_LANES - 1);  // must not exceed NUM_LANES-1
    game_input_move(s, +1);
    CHECK_EQ(s.player_lane, NUM_LANES - 1);
}

// ---------------------------------------------------------------------------
// game_input_move — blocked in GAME_OVER phase
// ---------------------------------------------------------------------------

static void test_move_blocked_in_game_over() {
    SUITE("move_blocked_game_over");
    GameState s = fresh();
    s.phase = GamePhase::GAME_OVER;
    s.player_lane = 3;
    game_input_move(s, -1);
    CHECK_EQ(s.player_lane, 3);  // must not move during game over
    game_input_move(s, +1);
    CHECK_EQ(s.player_lane, 3);
}

// ---------------------------------------------------------------------------
// game_input_move — full traversal
// ---------------------------------------------------------------------------

static void test_full_traversal_left_to_right() {
    SUITE("full_traversal");
    GameState s = fresh();
    s.player_lane = 0;
    for (int i = 0; i < NUM_LANES - 1; ++i) {
        game_input_move(s, +1);
        CHECK_EQ(s.player_lane, i + 1);
    }
    CHECK_EQ(s.player_lane, NUM_LANES - 1);
}

static void test_full_traversal_right_to_left() {
    SUITE("full_traversal_rtl");
    GameState s = fresh();
    s.player_lane = NUM_LANES - 1;
    for (int i = NUM_LANES - 1; i > 0; --i) {
        game_input_move(s, -1);
        CHECK_EQ(s.player_lane, i - 1);
    }
    CHECK_EQ(s.player_lane, 0);
}

// ---------------------------------------------------------------------------
// game_tick — no spawn before interval
// ---------------------------------------------------------------------------

static void test_tick_no_spawn_before_interval() {
    SUITE("tick_no_spawn_before_interval");
    GameState s = fresh();
    game_tick(s, 0);
    CHECK_EQ(s.enemies[0].active, false);
    game_tick(s, s.spawn_interval_ms - 1);
    CHECK_EQ(s.enemies[0].active, false);
}

// ---------------------------------------------------------------------------
// game_tick — enemy spawns at interval
// ---------------------------------------------------------------------------

static void test_enemy_spawns_at_interval() {
    SUITE("enemy_spawns_at_interval");
    GameState s = fresh();
    game_tick(s, s.spawn_interval_ms);
    CHECK_EQ(s.enemies[0].active, true);
    CHECK(s.enemies[0].lane >= 0);
    CHECK(s.enemies[0].lane < NUM_LANES);
    CHECK_EQ(s.enemies[0].y, -ENEMY_H);   // starts above top edge
    CHECK_EQ(s.last_spawn_ms, s.spawn_interval_ms);
}

// ---------------------------------------------------------------------------
// game_tick — enemy falls by step_px each tick
// ---------------------------------------------------------------------------

static void test_enemy_falls_each_tick() {
    SUITE("enemy_falls");
    GameState s = fresh();
    // Force spawn.
    game_tick(s, s.spawn_interval_ms);
    CHECK_EQ(s.enemies[0].active, true);
    int y_after_spawn = s.enemies[0].y;   // == -ENEMY_H

    game_tick(s, s.spawn_interval_ms + 50);
    CHECK_EQ(s.enemies[0].y, y_after_spawn + s.step_px);

    game_tick(s, s.spawn_interval_ms + 100);
    CHECK_EQ(s.enemies[0].y, y_after_spawn + 2 * s.step_px);
}

// ---------------------------------------------------------------------------
// game_tick — defeat triggers GAME_OVER and deactivates enemy
// ---------------------------------------------------------------------------

static void test_defeat_at_ship_y() {
    SUITE("defeat_at_ship_y");
    GameState s = fresh();
    // Force enemy active just below the defeat threshold.
    s.enemies[0].active = true;
    s.enemies[0].lane   = 0;
    s.enemies[0].y      = SHIP_Y - s.step_px;  // one step before threshold

    game_tick(s, 100);  // now_ms irrelevant here
    CHECK_EQ(s.enemies[0].y,      SHIP_Y);
    CHECK_EQ(s.phase,             GamePhase::GAME_OVER);
    CHECK_EQ(s.enemies[0].active, false);   // consumed on defeat
}

static void test_defeat_past_ship_y() {
    SUITE("defeat_past_ship_y");
    GameState s = fresh();
    s.enemies[0].active = true;
    s.enemies[0].lane   = 0;
    s.enemies[0].y      = SHIP_Y - 1;  // step_px=2 will overshoot

    game_tick(s, 100);
    CHECK_EQ(s.phase, GamePhase::GAME_OVER);
    CHECK_EQ(s.enemies[0].active, false);
}

// ---------------------------------------------------------------------------
// game_tick — no-op in GAME_OVER
// ---------------------------------------------------------------------------

static void test_tick_noop_in_game_over() {
    SUITE("tick_noop_game_over");
    GameState s = fresh();
    s.phase = GamePhase::GAME_OVER;
    s.tick_ms = 0;
    game_tick(s, 9999);
    CHECK_EQ(s.tick_ms, 0u);       // tick_ms must not advance
    CHECK_EQ(s.enemies[0].active, false);
}

// ---------------------------------------------------------------------------
// game_tick — no second spawn while enemy active
// ---------------------------------------------------------------------------

static void test_no_second_spawn_while_active() {
    SUITE("no_second_spawn");
    GameState s = fresh();
    game_tick(s, s.spawn_interval_ms);          // first spawn
    CHECK_EQ(s.enemies[0].active, true);
    int lane0 = s.enemies[0].lane;

    // Tick well past the next interval — but enemy still active, no re-spawn.
    game_tick(s, s.spawn_interval_ms * 3);
    CHECK_EQ(s.enemies[0].active, true);
    CHECK_EQ(s.enemies[0].lane,   lane0);       // same enemy, not replaced
}

// ---------------------------------------------------------------------------
// game_tick — enemy lane is always in [0, NUM_LANES)
// ---------------------------------------------------------------------------

static void test_spawn_lane_in_range() {
    SUITE("spawn_lane_in_range");
    // Run several spawn cycles and verify lane is always valid.
    GameState s = fresh();
    for (int i = 0; i < 12; ++i) {
        // Spawn
        game_tick(s, s.last_spawn_ms + s.spawn_interval_ms);
        CHECK_EQ(s.enemies[0].active, true);
        CHECK(s.enemies[0].lane >= 0);
        CHECK(s.enemies[0].lane < NUM_LANES);
        // Consume by pushing enemy past defeat threshold.
        s.enemies[0].y = SHIP_Y;
        game_tick(s, s.last_spawn_ms + s.spawn_interval_ms + 1);
        // Now in GAME_OVER; reset for next cycle.
        game_reset(s);
    }
}

// ---------------------------------------------------------------------------
// game_input_fire — basic behaviour
// ---------------------------------------------------------------------------

static void test_fire_spawns_projectile() {
    SUITE("fire_spawns");
    GameState s = fresh();
    game_input_fire(s);
    CHECK_EQ(s.projectile.active, true);
    CHECK_EQ(s.projectile.lane,   s.player_lane);
    CHECK_EQ(s.projectile.y,      SHIP_Y - PROJECTILE_H);
}

static void test_fire_blocked_when_active() {
    SUITE("fire_blocked_active");
    GameState s = fresh();
    game_input_fire(s);
    int y_first = s.projectile.y;
    // Manually move projectile, then try to fire again — must not replace.
    s.projectile.y = y_first - 10;
    game_input_fire(s);
    CHECK_EQ(s.projectile.y, y_first - 10);  // still the moved one
}

static void test_fire_blocked_in_game_over() {
    SUITE("fire_blocked_game_over");
    GameState s = fresh();
    s.phase = GamePhase::GAME_OVER;
    game_input_fire(s);
    CHECK_EQ(s.projectile.active, false);
}

// ---------------------------------------------------------------------------
// game_tick — projectile movement
// ---------------------------------------------------------------------------

static void test_projectile_moves_up() {
    SUITE("projectile_moves_up");
    GameState s = fresh();
    game_input_fire(s);
    int y0 = s.projectile.y;
    game_tick(s, 50);
    CHECK_EQ(s.projectile.y, y0 - PROJ_STEP_PX);
    game_tick(s, 100);
    CHECK_EQ(s.projectile.y, y0 - 2 * PROJ_STEP_PX);
}

static void test_projectile_deactivates_at_top() {
    SUITE("projectile_top_oob");
    GameState s = fresh();
    game_input_fire(s);
    // Place so that after one step the bottom edge (y + PROJECTILE_H) reaches 0.
    // After tick: y = -(PROJECTILE_H - PROJ_STEP_PX) - PROJ_STEP_PX = -PROJECTILE_H → bottom = 0.
    s.projectile.y = -(PROJECTILE_H - PROJ_STEP_PX);
    game_tick(s, 50);
    CHECK_EQ(s.projectile.active, false);
}

static void test_projectile_still_active_when_partially_visible() {
    SUITE("projectile_partial_visible");
    GameState s = fresh();
    game_input_fire(s);
    // y = 1 means bottom edge at 1+PROJECTILE_H > 0 — still on screen.
    s.projectile.y = 1;
    game_tick(s, 50);
    // After tick: y = 1 - PROJ_STEP_PX; bottom = 1 - PROJ_STEP_PX + PROJECTILE_H.
    // PROJECTILE_H=14, PROJ_STEP_PX=6: bottom = 9 > 0 → still active.
    CHECK_EQ(s.projectile.active, true);
}

// ---------------------------------------------------------------------------
// game_tick — collision
// ---------------------------------------------------------------------------

static void test_collision_same_lane() {
    SUITE("collision_same_lane");
    GameState s = fresh();
    s.enemies[0].active = true;
    s.enemies[0].lane   = 3;
    s.enemies[0].y      = 50;

    s.projectile.active = true;
    s.projectile.lane   = 3;
    // Place projectile so it overlaps: p.y < e.y + ENEMY_H  AND  p.y+PROJ_H > e.y
    s.projectile.y = 50;   // exact overlap

    game_tick(s, 100);
    CHECK_EQ(s.projectile.active, false);
    CHECK_EQ(s.enemies[0].active, false);
    CHECK_EQ(s.score, 1);
    CHECK_EQ(s.phase, GamePhase::PLAYING);  // not game over
}

static void test_collision_different_lanes_no_hit() {
    SUITE("collision_diff_lane");
    GameState s = fresh();
    s.enemies[0].active = true;
    s.enemies[0].lane   = 1;
    s.enemies[0].y      = 50;

    s.projectile.active = true;
    s.projectile.lane   = 2;  // different lane
    s.projectile.y      = 50;

    game_tick(s, 100);
    // No collision — different lanes.
    CHECK_EQ(s.score, 0);
    CHECK_EQ(s.enemies[0].active, true);
}

static void test_collision_increments_score() {
    SUITE("collision_score");
    GameState s = fresh();
    // Three consecutive kills.
    for (int i = 0; i < 3; ++i) {
        s.enemies[0].active = true;
        s.enemies[0].lane   = 0;
        s.enemies[0].y      = 40;
        s.projectile.active = true;
        s.projectile.lane   = 0;
        s.projectile.y      = 40;
        game_tick(s, (uint32_t)(i * 100 + 50));
        CHECK_EQ(s.score, i + 1);
    }
}

static void test_no_collision_when_projectile_inactive() {
    SUITE("no_collision_proj_inactive");
    GameState s = fresh();
    s.enemies[0].active = true;
    s.enemies[0].lane   = 0;
    s.enemies[0].y      = 40;
    // Projectile inactive — enemy should just fall.
    game_tick(s, 100);
    CHECK_EQ(s.score, 0);
    CHECK_EQ(s.enemies[0].active, true);
}

// ---------------------------------------------------------------------------
// Difficulty progression
// ---------------------------------------------------------------------------

// Helper: force one collision on player lane at arbitrary now_ms.
static void force_kill(GameState &s, uint32_t now_ms) {
    s.enemies[0].active = true;
    s.enemies[0].lane   = s.player_lane;
    s.enemies[0].y      = SHIP_Y / 2;
    s.projectile.active = true;
    s.projectile.lane   = s.player_lane;
    s.projectile.y      = SHIP_Y / 2;
    game_tick(s, now_ms);
}

static void test_difficulty_no_change_before_level1() {
    SUITE("difficulty_none_before_level1");
    GameState s = fresh();
    for (int i = 0; i < KILLS_PER_LEVEL - 1; ++i) force_kill(s, (uint32_t)(i * 100 + 50));
    CHECK_EQ(s.score,       KILLS_PER_LEVEL - 1);
    CHECK_EQ(s.speed_level, 0);
    CHECK_EQ(s.step_px,     BASE_STEP_PX);
    CHECK_EQ((int)s.spawn_interval_ms, (int)BASE_SPAWN_MS);
}

static void test_difficulty_level_up_at_kills_per_level() {
    SUITE("difficulty_level1");
    GameState s = fresh();
    for (int i = 0; i < KILLS_PER_LEVEL; ++i) force_kill(s, (uint32_t)(i * 100 + 50));
    CHECK_EQ(s.score,       KILLS_PER_LEVEL);
    CHECK_EQ(s.speed_level, 1);
    CHECK_EQ(s.step_px,     BASE_STEP_PX + 1);
    CHECK_EQ((int)s.spawn_interval_ms, (int)(BASE_SPAWN_MS - SPAWN_REDUCTION_MS));
}

static void test_difficulty_level_up_at_level2() {
    SUITE("difficulty_level2");
    GameState s = fresh();
    for (int i = 0; i < KILLS_PER_LEVEL * 2; ++i) force_kill(s, (uint32_t)(i * 100 + 50));
    CHECK_EQ(s.speed_level, 2);
    CHECK_EQ(s.step_px,     BASE_STEP_PX + 2);
    CHECK_EQ((int)s.spawn_interval_ms, (int)(BASE_SPAWN_MS - 2u * SPAWN_REDUCTION_MS));
}

static void test_difficulty_capped_at_max_level() {
    SUITE("difficulty_cap");
    GameState s = fresh();
    // Well past the cap at KILLS_PER_LEVEL * MAX_SPEED_LEVEL kills.
    for (int i = 0; i < KILLS_PER_LEVEL * MAX_SPEED_LEVEL + 8; ++i)
        force_kill(s, (uint32_t)(i * 100 + 50));
    CHECK_EQ(s.speed_level, MAX_SPEED_LEVEL);
    CHECK_EQ(s.step_px,     BASE_STEP_PX + MAX_SPEED_LEVEL);
    CHECK_EQ((int)s.spawn_interval_ms,
             (int)(BASE_SPAWN_MS - (uint32_t)MAX_SPEED_LEVEL * SPAWN_REDUCTION_MS));
}

static void test_difficulty_resets_with_game() {
    SUITE("difficulty_reset");
    GameState s = fresh();
    for (int i = 0; i < KILLS_PER_LEVEL * 2; ++i) force_kill(s, (uint32_t)(i * 100 + 50));
    CHECK_EQ(s.speed_level, 2);
    game_reset(s);
    CHECK_EQ(s.speed_level,       0);
    CHECK_EQ(s.step_px,           BASE_STEP_PX);
    CHECK_EQ((int)s.spawn_interval_ms, (int)BASE_SPAWN_MS);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // game_reset
    test_reset_defaults();

    // game_input_move — movement
    test_move_left();
    test_move_right();
    test_move_multi_left();
    test_move_multi_right();

    // game_input_move — clamping
    test_clamp_left_boundary();
    test_clamp_right_boundary();

    // game_input_move — GAME_OVER guard
    test_move_blocked_in_game_over();

    // game_input_move — full traversal
    test_full_traversal_left_to_right();
    test_full_traversal_right_to_left();

    // game_tick — spawning
    test_tick_no_spawn_before_interval();
    test_enemy_spawns_at_interval();
    test_enemy_falls_each_tick();

    // game_tick — defeat
    test_defeat_at_ship_y();
    test_defeat_past_ship_y();

    // game_tick — guards
    test_tick_noop_in_game_over();
    test_no_second_spawn_while_active();
    test_spawn_lane_in_range();

    // game_input_fire
    test_fire_spawns_projectile();
    test_fire_blocked_when_active();
    test_fire_blocked_in_game_over();

    // game_tick — projectile movement
    test_projectile_moves_up();
    test_projectile_deactivates_at_top();
    test_projectile_still_active_when_partially_visible();

    // difficulty progression
    test_difficulty_no_change_before_level1();
    test_difficulty_level_up_at_kills_per_level();
    test_difficulty_level_up_at_level2();
    test_difficulty_capped_at_max_level();
    test_difficulty_resets_with_game();

    // game_tick — collision
    test_collision_same_lane();
    test_collision_different_lanes_no_hit();
    test_collision_increments_score();
    test_no_collision_when_projectile_inactive();

    printf("\n%s: %d passed, %d failed\n",
           s_fail == 0 ? "PASS" : "FAIL", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
