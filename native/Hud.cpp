#include "Hud.h"
#include "Game.h"
#include <array>
#include <cstring>

// SkyCraft leaves the host compass, activation prompts, quest text and enemy health intact.
// New Vegas uses XML tiles rather than Skyrim's Scaleform movie.
namespace vegas::hud {
namespace {
constexpr unsigned kAlpha = 0xFA9;
constexpr std::array names{"HitPoints", "ActionPoints", "ReticleCenter", "Ammo", "AmmoTypeLabel", "HardcoreMode"};
struct Hidden { void* tile = nullptr; float alpha = 255; };
std::array<Hidden, names.size()> hidden{};
void* root = nullptr;
void* hudRoot() {
    void** menus = *reinterpret_cast<void***>(0x11F350C);
    return menus ? menus[1004 - 1001] : nullptr;
}
void* child(void* tile, const char* name) {
    // Tile::childList is a doubly linked list with {next, previous, child} nodes.
    for (void* n = hook::field<void*>(tile, 4); n; n = hook::field<void*>(n, 0)) {
        void* c = hook::field<void*>(n, 8);
        const char* s = c ? hook::field<const char*>(c, 0x20) : nullptr;
        if (s && !_stricmp(s, name)) return c;
    }
    return nullptr;
}
float alpha(void* tile) {
    auto** values = hook::field<void**>(tile, 0x14);
    const auto count = hook::field<unsigned>(tile, 0x18);
    if (count < 2048) for (unsigned i = 0; values && i < count; ++i)
        if (values[i] && hook::field<unsigned>(values[i], 0) == kAlpha) return hook::field<float>(values[i], 8);
    return 255;
}
void set(void* tile, float value) {
    reinterpret_cast<void(__thiscall*)(void*, unsigned, float, bool)>(0xA012D0)(tile, kAlpha, value, true);
}
}
void restore() {
    if (root && root == hudRoot()) for (auto& h : hidden) if (h.tile) set(h.tile, h.alpha);
    hidden = {}; root = nullptr;
}
void update(bool minecraft) {
    void* current = hudRoot();
    if (current != root) { hidden = {}; root = current; }
    if (!minecraft) { restore(); return; }
    if (!root) return;
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto& h = hidden[i];
        if (!h.tile) {
            h.tile = child(root, names[i]);
            if (h.tile) h.alpha = alpha(h.tile);
        }
        if (h.tile) set(h.tile, 0);
    }
}
}
