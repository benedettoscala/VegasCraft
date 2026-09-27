#pragma once
#include "Hook.h"
#include <cstdint>

// Fallout: New Vegas 1.4.0.525 engine access. Addresses and field offsets were taken from the
// pinned xNVSE, JohnnyGuitarNVSE and TESReloaded sources (see THIRD-PARTY-NOTICES.md); the code
// here is new. Everything is main-thread only unless noted.
namespace vegas::game {
struct NiPoint3 { float x, y, z; };

inline constexpr std::uintptr_t kPlayerSingleton = 0x011DEA3C;
inline constexpr std::uintptr_t kTesSingleton = 0x011DEA10;
inline constexpr std::uintptr_t kInterfaceManager = 0x011D8A80;
inline constexpr std::uintptr_t kStartMenu = 0x011DAAC0;      // main/pause menu while open
inline constexpr std::uintptr_t kRendererSlot = 0x011C73B4;   // NiDX9Renderer*
inline constexpr std::uintptr_t kSceneGraphSlot = 0x011DEB7C; // world SceneGraph*
inline constexpr std::uintptr_t kMainSlot = 0x011DEA0C;       // Main (TESMain)*
inline constexpr std::uintptr_t kWorldFovSetting = 0x01203160;
inline constexpr std::uintptr_t kFirstPersonFovSetting = 0x0120316C;
inline constexpr float kHavokScale = 0.1428767293691635f;      // game units -> Havok units

// TESForm / TESObjectREFR / Actor / PlayerCharacter fields.
inline constexpr std::size_t kFormId = 0x0C;
inline constexpr std::size_t kRefRotation = 0x24;  // NiPoint3: x pitch, z heading (radians)
inline constexpr std::size_t kRefPosition = 0x30;
inline constexpr std::size_t kRefParentCell = 0x40;
inline constexpr std::size_t kRefLoaded3D = 0x64;  // TESObjectREFR::RenderState* -> NiNode at +0x14 is engine-private; use Get3D
inline constexpr std::size_t kCellWorldspace = 0xC0;
inline constexpr std::size_t kPlayerIsThirdPerson = 0x64C;
inline constexpr std::size_t kPlayerWorldFov = 0x670;
inline constexpr std::size_t kPlayerFirstPersonFov = 0x674;
inline constexpr std::size_t kPlayerFirstPersonNode = 0x694;
inline constexpr std::size_t kInterfaceCrosshairRef = 0x0FC;
inline constexpr std::size_t kInterfaceMenuStack = 0x114;
inline constexpr std::size_t kRendererDevice = 0x288;
// INI Setting bDisableAutoVanityMode:General (vtable 0x1017720; the bool is at +4). While
// Minecraft drives, New Vegas sees no input and would orbit a third-person camera after
// fVanityModeAutoDelay (120 s).
inline constexpr std::uintptr_t kDisableAutoVanityMode = 0x11E09E4;

// Engine functions (thiscall unless noted).
inline constexpr std::uintptr_t kRefSetLocation = 0x575830;  // TESObjectREFR::SetLocationOnReference(const NiPoint3&)
inline constexpr std::uintptr_t kRefSetAngle = 0x575700;     // TESObjectREFR::SetAngleOnReference(NiPoint3)
inline constexpr std::uintptr_t kGetCharController = 0x9306D0; // MobileObject::GetCharController()
inline constexpr std::uintptr_t kControllerSetPosition = 0x5620E0; // bhkCharacterController::SetPosition(const NiPoint3&) (from SetPos)
inline constexpr std::uintptr_t kControllerIsState4 = 0x5C0860;   // bhkCharacterController: current state == 4 (SetPos skips those)
inline constexpr std::uintptr_t kCellGetHavokWorld = 0x4543C0; // TESObjectCELL::GetbhkWorld()
inline constexpr std::uintptr_t kTesGetLandHeight = 0x4572E0;  // TES::GetLandHeight(const NiPoint3&, float&)
inline constexpr std::uintptr_t kTesGetWaterHeight = 0x45CBC0; // TES::GetWaterHeight(const NiPoint3&, const TESObjectCELL*)
inline constexpr std::uintptr_t kIsMenuMode = 0x702360;        // cdecl bool
inline constexpr std::uintptr_t kRenderWorldSceneGraph = 0x873200; // Main::RenderWorldSceneGraph(Sun*, u8, u8, u8)
inline constexpr std::uintptr_t kRenderWorldSceneGraphSites[] = {0x870AE8, 0x870E18};

template<class T> T* global(std::uintptr_t slot) { return *reinterpret_cast<T**>(slot); }
inline void* player() { return global<void>(kPlayerSingleton); }
inline void* tes() { return global<void>(kTesSingleton); }
inline void* interfaceManager() { return global<void>(kInterfaceManager); }

inline NiPoint3 position(const void* ref) { return hook::field<NiPoint3>(ref, kRefPosition); }
inline NiPoint3 rotation(const void* ref) { return hook::field<NiPoint3>(ref, kRefRotation); }
inline std::uint32_t formId(const void* form) { return hook::field<std::uint32_t>(form, kFormId); }
inline void* parentCell(const void* ref) { return hook::field<void*>(ref, kRefParentCell); }
inline void* worldspace(const void* cell) { return hook::field<void*>(cell, kCellWorldspace); }

inline void setLocation(void* ref, const NiPoint3& p) {
    reinterpret_cast<void(__thiscall*)(void*, const NiPoint3*)>(kRefSetLocation)(ref, &p);
}
inline void setAngle(void* ref, NiPoint3 r) {
    reinterpret_cast<void(__thiscall*)(void*, NiPoint3)>(kRefSetAngle)(ref, r);
}
inline void* charController(void* actor) {
    return reinterpret_cast<void*(__thiscall*)(void*)>(kGetCharController)(actor);
}
// What the console's SetPos does: the reference, then its Havok character controller.
inline void movePlayer(void* actor, const NiPoint3& p) {
    setLocation(actor, p);
    void* controller = charController(actor);
    if (controller && !reinterpret_cast<bool(__thiscall*)(void*)>(kControllerIsState4)(controller))
        reinterpret_cast<void(__thiscall*)(void*, const NiPoint3*)>(kControllerSetPosition)(controller, &p);
}
inline void* havokWorld(void* cell) {
    return reinterpret_cast<void*(__thiscall*)(void*)>(kCellGetHavokWorld)(cell);
}
inline bool landHeight(const NiPoint3& p, float& height) {
    void* t = tes();
    return t && reinterpret_cast<bool(__thiscall*)(void*, const NiPoint3*, float*)>(kTesGetLandHeight)(t, &p, &height);
}
inline float waterHeight(const NiPoint3& p, void* cell) {
    void* t = tes();
    return t ? reinterpret_cast<float(__thiscall*)(void*, const NiPoint3*, void*)>(kTesGetWaterHeight)(t, &p, cell) : -1.0e30f;
}
inline bool menuMode() { return reinterpret_cast<bool(__cdecl*)()>(kIsMenuMode)(); }
inline bool startMenuOpen() { return *reinterpret_cast<void**>(kStartMenu) != nullptr; }
// Top of InterfaceManager's menu stack (kMenuType_* ids; 0 when no menu).
inline std::uint32_t topMenu() {
    void* im = interfaceManager();
    if (!im) return 0;
    std::uint32_t top = 0;
    for (int i = 0; i < 10; ++i) {
        auto id = hook::field<std::uint32_t>(im, kInterfaceMenuStack + 4 * i);
        if (!id) break;
        top = id;
    }
    return top;
}
// TESForm lookup: the engine's NiTPointerMap of all forms (xNVSE's LookupFormByID):
// {vtable, bucket count, buckets, item count}; entries {next, key, form}.
inline constexpr std::uintptr_t kFormsMap = 0x011C54C0;
inline void* lookupForm(std::uint32_t id) {
    void* map = *reinterpret_cast<void**>(kFormsMap);
    if (!map) return nullptr;
    const auto buckets = hook::field<std::uint32_t>(map, 0x04);
    auto** table = hook::field<void**>(map, 0x08);
    if (!buckets || !table) return nullptr;
    for (void* e = table[id % buckets]; e; e = hook::field<void*>(e, 0x00))
        if (hook::field<std::uint32_t>(e, 0x04) == id) return hook::field<void*>(e, 0x08);
    return nullptr;
}
inline void* crosshairRef() {
    void* im = interfaceManager();
    return im ? hook::field<void*>(im, kInterfaceCrosshairRef) : nullptr;
}
} // namespace vegas::game
