#pragma once
#include "Bridge.h"
#include <cstdio>

namespace vegas {
// An opt-in camera-space rendering probe. It has no physics or save-game objects.
class MotionPreview {
public:
    explicit MotionPreview(std::uintptr_t rendererSlot = 0x011C73B4, std::uintptr_t sceneSlot = 0x011DEB7C)
        : rendererSlot_(rendererSlot), sceneSlot_(sceneSlot) {}
    void enable(bool value) { enabled_ = value; }
    void reset() { anchored_ = false; }
    void present(const proto::SkyState& host, const Bridge& bridge, double scale, FILE* log);
private:
    std::uintptr_t rendererSlot_, sceneSlot_;
    bool enabled_ = false, anchored_ = false;
    std::uint32_t epoch_ = 0;
    Position anchor_{}, mcOrigin_{};
    std::uint64_t lastLog_ = 0;
};
}
