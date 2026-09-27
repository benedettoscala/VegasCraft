#include "DigPhysics.h"
#include "Dig.h"
#include "Guard.h"
#include "Havok.h"
#include "Log.h"
#include <atomic>
#include <cmath>

// SkyCraft's character contact filtering, adapted to New Vegas' older 32-bit Havok ABI.
// Drop contacts inside holes instead of changing shared collision meshes behind Havok.
namespace vegas::digphysics {
namespace {
constexpr float kBlocksPerHavok = 1.0f / (game::kHavokScale * 70.0f);
using AddPoint = void(__thiscall*)(void*, const Contact*);
AddPoint originalCharacter = nullptr, originalAll = nullptr;
std::atomic<const void*> puppet{nullptr};
std::atomic<float> puppetFeet{0}, puppetX{0}, puppetY{0};
std::atomic<unsigned> dropped{0};
const void* root(const void* body) {
    for (unsigned i = 0; body && i < 16; ++i) {
        const void* parent = nullptr;
        if (!hook::safeRead(static_cast<const char*>(body) + 0xC, parent)) return nullptr;
        if (!parent) return body;
        body = parent;
    }
    return nullptr;
}
unsigned layer(const void* collidable) {
    unsigned filter = 0;
    return collidable && hook::safeRead(static_cast<const char*>(collidable) + 0x1C, filter) ? filter & 0x7F : 0;
}
bool valid(const Contact& p) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p.position[i]) || std::fabs(p.position[i]) > 1e7f || !std::isfinite(p.normal[i])) return false;
    const float n = p.normal[0] * p.normal[0] + p.normal[1] * p.normal[1] + p.normal[2] * p.normal[2];
    return n > 0.5f && n < 1.5f;
}
bool filter(const Contact& p) { return onDugGround(p) || abovePuppetFeet(p) || groundAbovePuppet(p); }
std::atomic<unsigned> traced{0};
std::atomic<float> traceX{0}, traceZ{0};
void trace(const Contact& p, bool dropped) {
    if (!traced.load() || std::fabs(p.position[0] * kBlocksPerHavok - traceX.load()) > 1.5f || std::fabs(-p.position[1] * kBlocksPerHavok - traceZ.load()) > 1.5f || traced.fetch_sub(1) == 0) return;
    log::line("contact %s at blocks %.2f %.2f %.2f normal %.2f %.2f %.2f layers %u/%u", dropped ? "DROPPED" : "kept",
        p.position[0] * kBlocksPerHavok, p.position[2] * kBlocksPerHavok, -p.position[1] * kBlocksPerHavok,
        p.normal[0], p.normal[2], -p.normal[1], layer(root(p.bodyA)), layer(root(p.bodyB)));
}
void __fastcall characterPoint(void* collector, void*, const Contact* point) {
    if (point) trace(*point, filter(*point));
    if (point && filter(*point)) {
        if (dropped.fetch_add(1) < 3) log::line("digphysics: actor contact with removed ground dropped");
        return;
    }
    originalCharacter(collector, point);
}
void __fastcall allPoint(void* collector, void*, const Contact* point) {
    if (point) trace(*point, filter(*point));
    if (point && filter(*point)) return;
    originalAll(collector, point);
}
// PlayerCharacter's own "below the ground" check (0x9EA250): under the land by more than 32 units it
// moves the player to land + 32 with a second height query. Skipped while the player is in a hole.
bool inHole(const game::NiPoint3& p) {
    if (!dig::any() || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::fabs(p.x) > 1e7f || std::fabs(p.y) > 1e7f || std::fabs(p.z) > 1e7f) return false;
    const int x = int(std::floor(p.x / 70)), z = int(std::floor(-p.y / 70));
    const float y = p.z / 70;
    return dig::isDug(x, int(std::floor(y + 0.1f)), z) || dig::isDug(x, int(std::floor(y + 1)), z);
}
bool __fastcall playerRescue(void* query, void*, const game::NiPoint3* position, float* height) {
    const bool found = reinterpret_cast<bool(__thiscall*)(void*, const game::NiPoint3*, float*)>(0x53F180)(query, position, height);
    return found && !(position && inHole(*position));
}
bool __fastcall landRescue(void* tes, void*, const game::NiPoint3* position, float* height) {
    const bool found = reinterpret_cast<bool(__thiscall*)(void*, const game::NiPoint3*, float*)>(game::kTesGetLandHeight)(tes, position, height);
    if (found && position && height) adjustLandHeight(*position, *height);
    return found;
}
bool patch(std::uintptr_t slot, std::uintptr_t expected, void* fn, AddPoint& original) {
    auto** at = reinterpret_cast<void**>(slot);
    if (!hook::readable(at, 4) || reinterpret_cast<std::uintptr_t>(*at) != expected) return false;
    original = reinterpret_cast<AddPoint>(hook::replaceSlot(at, fn));
    return original != nullptr;
}
}
bool onDugGround(const Contact& p) {
    if (!dig::any() || !valid(p)) return false;
    const auto* a = root(p.bodyA); const auto* b = root(p.bodyB);
    const unsigned la = layer(a), lb = layer(b);
    // Landscape ground against a character controller or its casts. Other queries (rays, rigid bodies, and
    // buildings) retain their original contacts even if they share a dug cube.
    using havok::kLayerGround;
    auto character = [](unsigned layer) { return layer == havok::kLayerCharController || layer == havok::kLayerCharCast; };
    const bool worldB = character(la) && lb == kLayerGround;
    if (!worldB && !(character(lb) && la == kLayerGround)) return false;
    const float s = worldB ? 0.05f : -0.05f;
    const float x = (p.position[0] - p.normal[0] * s) * kBlocksPerHavok;
    const float y = (p.position[2] - p.normal[2] * s) * kBlocksPerHavok;
    const float z = -(p.position[1] - p.normal[1] * s) * kBlocksPerHavok;
    return dig::isDug(int(std::floor(x)), int(std::floor(y)), int(std::floor(z)));
}
bool abovePuppetFeet(const Contact& p) {
    const void* body = puppet.load();
    return body && valid(p) && (root(p.bodyA) == body || root(p.bodyB) == body) && p.position[2] > puppetFeet.load() + 0.3f;
}
bool groundAbovePuppet(const Contact& p) {
    if (!puppet.load() || !valid(p) || p.position[2] <= puppetFeet.load() + 0.3f) return false;
    const unsigned la = layer(root(p.bodyA)), lb = layer(root(p.bodyB));
    auto character = [](unsigned l) { return l == havok::kLayerCharController || l == havok::kLayerCharCast; };
    if (!((la == havok::kLayerGround && character(lb)) || (lb == havok::kLayerGround && character(la)))) return false;
    const float dx = (p.position[0] - puppetX.load()) * kBlocksPerHavok, dy = (p.position[1] - puppetY.load()) * kBlocksPerHavok;
    return dx * dx + dy * dy < 1.5f * 1.5f;
}
void setPuppet(const void* collidable, float feetHavokZ, float havokX, float havokY) {
    puppetFeet.store(feetHavokZ); puppetX.store(havokX); puppetY.store(havokY); puppet.store(collidable);
}
void adjustLandHeight(const game::NiPoint3& p, float& height) {
    if (!dig::any() || !std::isfinite(height) || height <= p.z || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
    if (std::fabs(p.x) > 1e7f || std::fabs(p.y) > 1e7f || std::fabs(p.z) > 1e7f || std::fabs(height) > 1e7f) return;
    const int x = int(std::floor(p.x / 70)), z = int(std::floor(-p.y / 70));
    const float y = p.z / 70;
    if (dig::isDug(x, int(std::floor(height / 70 - 0.01f)), z) || inHole(p)) height = p.z;
}
void traceContacts(unsigned count, float x, float z) { traceX.store(x); traceZ.store(z); traced.store(count); }
void install() {
    if (originalCharacter || originalAll) return;
    const bool character = patch(0x10CB304, 0xCD36A0, reinterpret_cast<void*>(&characterPoint), originalCharacter);
    const bool support = patch(0x104DA2C, 0xCAB970, reinterpret_cast<void*>(&allPoint), originalAll);
    const bool rescue = hook::redirectCall(0x930150, game::kTesGetLandHeight, reinterpret_cast<void*>(&landRescue));
    const bool playerCheck = hook::redirectCall(0x9EA29D, 0x53F180, reinterpret_cast<void*>(&playerRescue));
    log::line("digphysics: character=%d support=%d land-rescue=%d player-rescue=%d", character, support, rescue, playerCheck);
}
}
