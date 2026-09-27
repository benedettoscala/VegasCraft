#include "BlockLights.h"
#include "SceneUtil.h"
#include "Game.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

// SkyCraft's emitter clustering and flame/lava animation, using New Vegas' NiPointLight and
// ShadowSceneNode instead of Skyrim's dynamic-light API. Engine work runs before world culling.
namespace vegas::blocklights {
namespace {
struct Emitter { int x, y, z; unsigned level, rgb, kind; };
std::unordered_map<std::uint64_t, std::vector<Emitter>> sections;
std::uint64_t key(int x, int y, int z) {
    return (std::uint64_t(std::uint32_t(x) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(z) & 0x3FFFFF) << 20) | (std::uint32_t(y) & 0xFFFFF);
}
struct Slot { void* light = nullptr; void* wrapper = nullptr; std::uint64_t id = 0; };
std::array<Slot, 24> slots{};
std::vector<Point> lastPoints;  // the clusters of the last update, for Minecraft's own shader
void* scene = nullptr;
float radiusBoost = 1, brightnessBoost = 1;
// New Vegas' dimmer 1.0 is faint next to Minecraft's torch; at night x1 barely tinted the nearest rocks,
// x2 lit them warm and x4 lit rocks and sandbags clearly. Measured in game, see the README.
constexpr float kBaseBrightness = 3.0f;
void* currentScene() { return game::global<void>(0x11F91C8); }
auto getScene = &currentScene;
auto createLight = reinterpret_cast<void*(__cdecl*)()>(0xA7D6E0);
auto attenuateLight = reinterpret_cast<void(__cdecl*)(void*, unsigned)>(0x50C350);
auto addLight = reinterpret_cast<void*(__thiscall*)(void*, void*, bool)>(0xB5C940);
auto removeLight = reinterpret_cast<void(__thiscall*)(void*, void**)>(0xB5CFF0);
void remove(Slot& s) {
    if (s.light) hook::setField<float>(s.light, 0xC4, 0);
    if (scene && s.wrapper) removeLight(scene, &s.wrapper);
    sceneutil::release(s.wrapper); sceneutil::release(s.light); s = {};
}
void deactivate() {
    lastPoints.clear();
    for (auto& s : slots) remove(s);
    sceneutil::release(scene); scene = nullptr;
}
}
void onLights(const std::uint8_t* data, std::uint32_t bytes) {
    if (bytes < sizeof(proto::RenLights)) return;
    const auto& h = *reinterpret_cast<const proto::RenLights*>(data);
    if (h.count > 4096 || bytes < sizeof(h) + h.count * sizeof(proto::RenLight) ||
        h.sx < -2097152 || h.sx > 2097151 || h.sz < -2097152 || h.sz > 2097151 || h.sy < -524288 || h.sy > 524287) return;
    const auto k = key(h.sx, h.sy, h.sz);
    if (!h.count) { sections.erase(k); return; }
    std::vector<Emitter> emitters;
    const auto* lights = reinterpret_cast<const proto::RenLight*>(data + sizeof(h));
    for (unsigned i = 0; i < h.count; ++i) {
        const auto& l = lights[i];
        if (l.x > 15 || l.y > 15 || l.z > 15 || !l.level || l.level > 15) continue;
        emitters.push_back({h.sx * 16 + l.x, h.sy * 16 + l.y, h.sz * 16 + l.z, l.level, l.color & 0xFFFFFF, (l.color >> 24) & 15});
    }
    sections[k] = std::move(emitters);
}
std::vector<Point> nearby(const Position& player) {
    if (!std::isfinite(player.x) || !std::isfinite(player.y) || !std::isfinite(player.z)) return {};
    struct Cluster { double x = 0, y = 0, z = 0, w = 0; float r = 0, g = 0, b = 0; unsigned level = 0, kind = 0; };
    std::unordered_map<std::uint64_t, Cluster> clusters;
    for (const auto& [k, list] : sections) for (const auto& e : list) {
        const double dx = e.x + 0.5 - player.x, dy = e.y + 0.5 - player.y, dz = e.z + 0.5 - player.z;
        if (dx * dx + dy * dy + dz * dz > 72 * 72) continue;
        auto& c = clusters[key(int(std::floor(e.x / 3.0)), int(std::floor(e.y / 3.0)), int(std::floor(e.z / 3.0)))];
        c.x += (e.x + 0.5) * e.level; c.y += (e.y + 0.5) * e.level; c.z += (e.z + 0.5) * e.level; c.w += e.level;
        c.r += float(e.rgb & 255) * e.level; c.g += float((e.rgb >> 8) & 255) * e.level; c.b += float((e.rgb >> 16) & 255) * e.level;
        if (e.level > c.level) { c.level = e.level; c.kind = e.kind; }
        else if (e.level == c.level) c.kind = std::max(c.kind, e.kind);
    }
    std::vector<Point> out;
    for (const auto& [k, c] : clusters) {
        const float x = float(c.x / c.w), y = float(c.y / c.w), z = float(c.z / c.w);
        const double dx = x - player.x, dy = y - player.y, dz = z - player.z;
        out.push_back({x, y, z, float(c.r / (255 * c.w)), float(c.g / (255 * c.w)), float(c.b / (255 * c.w)),
            float(c.level) * float(proto::kUnitsPerBlock) * 0.8f, float(c.level / (1 + dx * dx + dy * dy + dz * dz)), c.kind, k});
    }
    std::sort(out.begin(), out.end(), [](const Point& a, const Point& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    if (out.size() > slots.size()) out.resize(slots.size());
    return out;
}
const std::vector<Point>& active() { return lastPoints; }
void clear() { sections.clear(); if (scene) deactivate(); }
void setBoost(float radius, float brightness) { radiusBoost = radius; brightnessBoost = brightness; }
void configure(void* light, const Point& p, float time) {
    const game::NiPoint3 pos{p.x * 70, -p.z * 70, p.y * 70};
    hook::setField(light, 0x58, pos); hook::setField(light, 0x8C, pos);
    hook::setField(light, 0xD4, game::NiPoint3{p.r, p.g, p.b});
    hook::setField(light, 0xC8, game::NiPoint3{});
    // New Vegas repurposes NiLight's specular vector as the three-axis light radius.
    // Mirror TESObjectLIGH setup at 50DB92/50DD50, including all three components.
    hook::setField(light, 0xE0, game::NiPoint3{p.radius * radiusBoost, p.radius * radiusBoost, p.radius * radiusBoost});
    attenuateLight(light, unsigned(p.radius * radiusBoost));
    // Stable phase per cluster: reordering by distance must not restart its flicker.
    const float t = time + float(p.id % 997) * 0.037f;
    float brightness = 1;
    if (p.kind == proto::kLightFlame) brightness += 0.11f * std::sin(t * 9.1f) + 0.08f * std::sin(t * 23.7f) + 0.04f * std::sin(t * 41.3f);
    if (p.kind == proto::kLightLava) brightness += 0.08f * std::sin(t * 1.3f);
    hook::setField(light, 0xC4, brightness * brightnessBoost * kBaseBrightness);
    hook::setField<unsigned>(light, 0xA8, hook::field<unsigned>(light, 0xA8) + 1);
}
void update(const Position& player, bool enabled) {
    void* current = getScene();
    if (current != scene || !enabled) deactivate();
    if (!enabled || !current) return;
    if (!scene) { scene = current; sceneutil::retain(scene); }
    const auto points = nearby(player);
    lastPoints = points;
    const float time = float(GetTickCount64() % 1000000) / 1000;
    // Preserve each cluster's scene wrapper as rankings change. Remove vanished/out-of-range
    // clusters rather than keeping dimmed registrations that consume native light slots.
    for (auto& s : slots) if (s.light && std::none_of(points.begin(), points.end(), [&](const Point& p) { return p.id == s.id; })) remove(s);
    for (const auto& p : points) {
        auto it = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) { return s.light && s.id == p.id; });
        if (it == slots.end()) it = std::find_if(slots.begin(), slots.end(), [](const Slot& s) { return !s.light; });
        if (it == slots.end()) break;
        auto& s = *it;
        if (!s.light) {
            s.light = createLight();
            if (!s.light) continue;
            sceneutil::retain(s.light);
            s.id = p.id;
            configure(s.light, p, time);
            s.wrapper = addLight(scene, s.light, true);
            sceneutil::retain(s.wrapper);
            if (!s.wrapper) { remove(s); continue; }
        } else {
            configure(s.light, p, time);
        }
        // AddLight snapshots the source position when dynamic=true (+FC). Refresh it when
        // a weighted cluster centre moves as emitters are added or removed.
        hook::setField(s.wrapper, 0x100, hook::field<game::NiPoint3>(s.light, 0x8C));
    }
}
#ifdef VEGAS_TESTING
void setApiForTests(const Api& api) {
    deactivate();
    getScene = api.scene; createLight = api.create; attenuateLight = api.attenuation;
    addLight = api.add; removeLight = api.remove;
}
#endif
}
