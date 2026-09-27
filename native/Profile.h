#pragma once
#include "Log.h"
#include <windows.h>
#include <cstdio>

// Main-thread timing of the plugin's parts (probe `profile <seconds>` turns the report on).
namespace vegas::profile {
enum Part { kFrame, kSession, kDrain, kPreRender, kNvWorld, kDraw, kPresent, kItems, kPublish, kHoldThird, kApplyHeld, kCollision, kPipData, kHands, kInventory, kLand, kWater, kCombat, kPushOut, kDrive, kHud, kLink, kCount };
inline const char* const kNames[kCount] = {"frame", "session", "drain", "prerender", "nv-world", "draw", "present", "items", "publish", "hold3rd", "applyheld", "collision", "pipdata", "hands", "inventory", "land", "water", "combat", "pushout", "drive", "hud", "link"};
inline double total[kCount]{}, peak[kCount]{};
inline unsigned calls[kCount]{};
inline ULONGLONG reportUntil = 0;
inline double qpcMs() {
    static const double perMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart) / 1000.0; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / perMs;
}
struct Scope {
    Part part; double start;
    explicit Scope(Part p) : part(p), start(qpcMs()) {}
    ~Scope() { const double d = qpcMs() - start; total[part] += d; if (d > peak[part]) peak[part] = d; ++calls[part]; }
};
// Once per presented frame: the frame's own length, and every 2 s the averages.
inline void frame() {
    static double last = 0, lastReport = 0;
    const double now = qpcMs();
    if (last) { total[kFrame] += now - last; ++calls[kFrame]; if (now - last > peak[kFrame]) peak[kFrame] = now - last; }
    last = now;
    if (now - lastReport < 2000) return;
    lastReport = now;
    if (GetTickCount64() < reportUntil && calls[kFrame]) {
        char line[512]; int n = 0;
        for (int i = 0; i < kCount; ++i)
            n += std::snprintf(line + n, sizeof(line) - n, " %s %.2f(max %.1f)", kNames[i], calls[i] ? total[i] / calls[kFrame] : 0.0, peak[i]);
        log::line("profile:%s", line);
    }
    for (int i = 0; i < kCount; ++i) { total[i] = 0; calls[i] = 0; peak[i] = 0; }
}
} // namespace vegas::profile
