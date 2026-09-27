#pragma once
#include "Bridge.h"
#include "NvseAbi.h"
#include <cstdint>

// The plugin is two modules so the native code can be rebuilt while New Vegas runs:
// VegasCraft.dll (the xNVSE plugin, loaded once) owns the bridge, the log file and the Minecraft
// launcher, and loads a copy of VegasCraft/VegasCraftCore.dll, which holds everything else. When
// that file changes, or the probe `reload` asks, the loader stops the core (it takes back all its
// hooks and resources) and loads the new one. A change to this header or to Bridge.h needs a
// full restart of New Vegas: both modules must agree on them.
namespace vegas::core {
inline constexpr std::uint32_t kAbi = 1;
struct Host {
    std::uint32_t abi;
    std::uint32_t bridgeBytes;      // sizeof(Bridge) as the loader was built
    Bridge* bridge;
    const wchar_t* gameDir;         // FalloutNV.exe's directory
    const wchar_t* ini;             // Data/NVSE/Plugins/VegasCraft.ini
    bool (*console)(const char* line, void* callingRef);
    std::uint32_t generation;       // 0 for the core loaded at startup, +1 per reload
    bool postLoaded;                // xNVSE's PostLoad has been sent already
    bool gameLoaded;                // a save or a new game is loaded and running
    volatile bool reloadRequested;  // set by the core (probe `reload`), read by the loader
};
// Exported by the core, extern "C" cdecl.
using StartFn = bool (*)(Host* host);
using MessageFn = void (*)(nvse::Message* msg);
// Undoes everything the core changed. False: something could not be taken back, so the module
// must stay loaded.
using StopFn = bool (*)();
inline constexpr const char* kStart = "VegasCore_Start";
inline constexpr const char* kMessage = "VegasCore_Message";
inline constexpr const char* kStop = "VegasCore_Stop";
} // namespace vegas::core
