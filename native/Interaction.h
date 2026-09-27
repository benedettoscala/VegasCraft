#pragma once

namespace vegas::interaction {
// New Vegas SIT_SLEEP_STATE (BSEnums.hpp). Includes animation transitions so Minecraft
// never pulls the player away while the engine walks them into or out of furniture.
inline bool furniture(unsigned state) { return state >= 1 && state <= 10; }
struct Transition { bool active, entered, left; };
class Handoff {
    bool active_ = false;
public:
    Transition update(unsigned sitSleepState, bool enabled) {
        const bool active = enabled && furniture(sitSleepState);
        Transition t{active, active && !active_, !active && active_};
        active_ = active;
        return t;
    }
    void clear() { active_ = false; }
};
}
