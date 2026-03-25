#pragma once

// Explicit integer values preserve NVS compatibility across firmware upgrades.
// Do not renumber existing entries; add new entries only at the end.
enum class DisplayMode {
    CLOCK    = 0,
    DATE     = 2,  // legacy — still accepted but not in the auto-cycle
    TODAY    = 3,
    FORECAST = 4,
    GAME     = 5,  // entered via explicit input only; not in the auto-cycle
};

class ModeManager {
public:
    static ModeManager& get() {
        static ModeManager instance;
        return instance;
    }

    void cycle();
    void reset();
    void set(DisplayMode m);
    DisplayMode current() const { return mode_; }
    static const char* name(DisplayMode m);

    ModeManager(const ModeManager&) = delete;
    void operator=(const ModeManager&) = delete;

private:
    ModeManager() = default;
    DisplayMode mode_{DisplayMode::CLOCK};
};
