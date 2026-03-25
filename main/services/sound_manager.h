#pragma once

namespace sound_manager {

enum class SoundEvent {
    FIRE,
    HIT,
    GAME_OVER,
    RESTART,
    BLOCKED,   // fire attempted while projectile already active
};

// Call once at boot (after FreeRTOS scheduler is running).
void init();

// Post a sound event. Non-blocking; the newest event always wins.
// Safe to call from any task or ISR context.
void play(SoundEvent event);

}  // namespace sound_manager
