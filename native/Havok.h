#pragma once
#include "Hook.h"
#include "Rtti.h"
#include <cstdint>

// New Vegas' 32-bit Havok (hkpWorld and shapes). Offsets from JohnnyGuitarNVSE's havok.h and
// TESReloaded's NewVegas/GameHavok.h, confirmed in game with the `havok` / `shape` probes.
// Shapes are identified by their MSVC RTTI names rather than by Havok's type enum.
namespace vegas::havok {
using ShapeKey = std::uint32_t;
inline constexpr ShapeKey kInvalidKey = 0xFFFFFFFF;
inline constexpr std::size_t kShapeBufferBytes = 512;

struct ArrayView { void** data; std::int32_t size; std::uint32_t capacity; };

// hkpWorld
inline void* fixedIsland(const void* world) { return hook::field<void*>(world, 0x20); }
inline ArrayView islands(const void* world, bool active) { return hook::field<ArrayView>(world, active ? 0x28 : 0x34); }
// hkpSimulationIsland: rigid bodies
inline ArrayView islandBodies(const void* island) { return hook::field<ArrayView>(island, 0x48); }
inline int islandSize(const void* island) { return island ? islandBodies(island).size : 0; }
inline int islandCount(const void* world, bool active) { return islands(world, active).size; }
// hkpRigidBody (hkpWorldObject: collidable at 0x10 = shape, shapeKey, motion state, parent)
inline const void* bodyShape(const void* body) { return hook::field<const void*>(body, 0x10); }
inline const float* bodyTransform(const void* body) { return hook::field<const float*>(body, 0x18); }
inline std::uint32_t bodyFilterInfo(const void* body) { return hook::field<std::uint32_t>(body, 0x2C); }
inline std::uint32_t bodyLayer(const void* body) { return bodyFilterInfo(body) & 0x7F; }
// New Vegas collision layers (not Skyrim's): the landscape height field is OL_GROUND (17);
// OL_TERRAIN (13) holds rock and cliff meshes, which stay intact like other statics.
inline constexpr std::uint32_t kLayerGround = 17;
inline constexpr std::uint32_t kLayerTerrain = 13;
inline constexpr std::uint32_t kLayerCharController = 30;
// The player's controller runs its support/step shape casts on this layer (OL_CUSTOMPICK2).
inline constexpr std::uint32_t kLayerCharCast = 40;
inline const void* bodyOwner(const void* body) { return hook::field<const void*>(body, 0x0C); } // bhkWorldObject

template<class F> void forEachBody(const void* world, F&& fn) {
    auto visit = [&](const void* island, bool fixed) {
        if (!island) return;
        auto bodies = islandBodies(island);
        if (bodies.size < 0 || bodies.size > 200000 || !hook::plausible(bodies.data, bodies.size * sizeof(void*))) return;
        for (std::int32_t i = 0; i < bodies.size; ++i) {
            const void* body = bodies.data[i];
            if (body && bodyShape(body) && bodyTransform(body)) fn(body, fixed);
        }
    };
    visit(fixedIsland(world), true);
    for (int active = 1; active >= 0; --active) {
        auto list = islands(world, active != 0);
        if (list.size < 0 || list.size > 10000 || !hook::plausible(list.data, list.size * sizeof(void*))) continue;
        for (std::int32_t i = 0; i < list.size; ++i) visit(list.data[i], false);
    }
}

// hkpShapeContainer interface (a base subobject of hkpShapeCollection, hkpConvexListShape, ...):
// [2] getFirstKey, [3] getNextKey, [5] getChildShape(key, buffer).
inline const void* container(const void* shape) {
    return rtti::cast<const void>(const_cast<void*>(shape), ".?AVhkpShapeContainer@@");
}
inline void* const* vtable(const void* object) { return *static_cast<void* const* const*>(object); }
inline ShapeKey firstKey(const void* c) {
    return reinterpret_cast<ShapeKey(__thiscall*)(const void*)>(vtable(c)[2])(c);
}
inline ShapeKey nextKey(const void* c, ShapeKey key) {
    return reinterpret_cast<ShapeKey(__thiscall*)(const void*, ShapeKey)>(vtable(c)[3])(c, key);
}
inline const void* childShape(const void* c, ShapeKey key, void* buffer) {
    return reinterpret_cast<const void*(__thiscall*)(const void*, ShapeKey, void*)>(vtable(c)[5])(c, key, buffer);
}
// Shapes that wrap exactly one child (MOPP/BV trees, transforms) embed an
// hkpSingleShapeContainer {vptr, child}; find it by its RTTI.
inline const void* singleChild(const void* shape) {
    for (std::size_t off = 0x10; off < 0x40; off += 4) {
        auto* member = static_cast<const unsigned char*>(shape) + off;
        if (rtti::is(member, ".?AVhkpSingleShapeContainer@@")) return hook::field<const void*>(member, 4);
    }
    return nullptr;
}
} // namespace vegas::havok
