#pragma once
#include "Game.h"
#include "Rtti.h"
#include <cmath>
#include <cstring>

// New Vegas actors (main thread, inside guard::run). Sources as in Game.h.
namespace vegas::actors {
inline constexpr std::uintptr_t kProcessLists = 0x011E0E80;
inline constexpr std::uintptr_t kGetLifeState = 0x4F8960;  // Actor::GetLifeState: 0 alive
inline constexpr int kVfGet3D = 0x1D0 / 4;                  // TESObjectREFR (called by SetPos)

inline void* const* vtable(const void* object) { return *static_cast<void* const* const*>(object); }
template<class R, class... A> R vcall(void* object, int index, A... args) {
    return reinterpret_cast<R(__thiscall*)(void*, A...)>(vtable(object)[index])(object, args...);
}
template<class R, class... A> R call(std::uintptr_t address, void* object, A... args) {
    return reinterpret_cast<R(__thiscall*)(void*, A...)>(address)(object, args...);
}
inline bool isActor(const void* object) {
    const char* n = rtti::fastName(object);
    return !std::strcmp(n, ".?AVCharacter@@") || !std::strcmp(n, ".?AVCreature@@");
}
inline bool dead(void* actor) { return call<int>(kGetLifeState, actor) != 0; }
inline bool disabled(const void* ref) { return hook::field<std::uint32_t>(ref, 0x08) & 0x800; }
// Bounding sphere radius of the actor's 3D (NiAVObject+0x20 points to its world NiBound:
// centre, radius), 0 without 3D.
inline float boundRadius(void* actor) {
    void* node = vcall<void*>(actor, kVfGet3D);
    const void* bound = node ? hook::field<const void*>(node, 0x20) : nullptr;
    if (!bound) return 0;
    const float r = hook::field<float>(bound, 0x0C);
    return std::isfinite(r) && r > 1.0f && r < 5000.0f ? r : 0.0f;
}
// High-process actors: ProcessLists' ProcessArray (at +0x04) holds MobileObject pointers, with
// [head, tail) of each process level (high = 0).
template<class F> void forEachHighActor(F&& fn) {
    auto* lists = reinterpret_cast<unsigned char*>(kProcessLists);
    auto** items = hook::field<void**>(lists, 0x04 + 0x04);
    const auto head = hook::field<std::uint32_t>(lists, 0x04 + 0x10);
    const auto tail = hook::field<std::uint32_t>(lists, 0x04 + 0x20);
    if (!items || tail <= head || tail - head > 2000) return;
    for (std::uint32_t i = head; i < tail; ++i)
        if (void* object = items[i]; object && isActor(object)) fn(object);
}
} // namespace vegas::actors
