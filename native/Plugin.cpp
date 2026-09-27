#include "Bridge.h"
#include "CoreAbi.h"
#include "Hook.h"
#include "Launcher.h"
#include "Log.h"
#include "NvseAbi.h"
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

// The xNVSE plugin proper: the loader half of the plugin (see CoreAbi.h). It holds the bridge
// to Minecraft, starts Minecraft, and runs a copy of the core, which it swaps for a new build
// while the game runs.
using namespace vegas;
namespace fs = std::filesystem;
namespace {
Bridge bridge;
core::Host host{};
std::wstring gameDir, iniPath;
fs::path coreSource, liveDir;

HMODULE coreModule = nullptr;
core::MessageFn coreMessage = nullptr;
core::StopFn coreStop = nullptr;
bool coreFailed = false;   // the last load failed: wait for a new build
bool loading = false;      // between PreLoadGame and PostLoadGame
bool hotReload = true;

// Stopped cores stay mapped a few seconds, in case a thread is still leaving one of their hooks.
struct Retired { HMODULE module; fs::path file; ULONGLONG at; };
std::vector<Retired> retired;

// The core build being watched: its write time and size, and a newer one waiting to settle.
struct Stamp {
    FILETIME time{}; DWORD size = 0; bool valid = false;
    bool operator==(const Stamp& o) const { return valid == o.valid && size == o.size && CompareFileTime(&time, &o.time) == 0; }
};
Stamp loadedStamp, pendingStamp;
ULONGLONG lastCheck = 0, pendingSince = 0;

Stamp stampOf(const fs::path& file) {
    Stamp s;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) { s.time = data.ftLastWriteTime; s.size = data.nFileSizeLow; s.valid = true; }
    return s;
}
fs::path modulePath(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    return path;
}
// Data/NVSE/Plugins/VegasCraft/VegasCraftCore.dll; beside the loader in a build directory.
fs::path findCore() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&findCore), &self);
    const auto dir = modulePath(self).parent_path();
    for (const auto& candidate : {dir / "VegasCraft" / "VegasCraftCore.dll", dir / "VegasCraftCore.dll"})
        if (fs::exists(candidate)) return candidate;
    return dir / "VegasCraft" / "VegasCraftCore.dll";
}
void clearLiveDir() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(liveDir, ec)) fs::remove(entry.path(), ec);  // fails for a copy still loaded: fine
}
bool loadCore() {
    coreFailed = true;
    loadedStamp = stampOf(coreSource);
    if (!loadedStamp.valid) { log::line("core: %ls is missing", coreSource.c_str()); return false; }
    // A copy is loaded, so the build can overwrite the original while the game runs.
    std::error_code ec;
    fs::create_directories(liveDir, ec);
    const auto copy = liveDir / ("VegasCraftCore-" + std::to_string(host.generation) + "-" + std::to_string(GetTickCount64()) + ".dll");
    if (!fs::copy_file(coreSource, copy, fs::copy_options::overwrite_existing, ec)) { log::line("core: cannot copy the core (%s)", ec.message().c_str()); return false; }
    HMODULE module = LoadLibraryW(copy.c_str());
    if (!module) { log::line("core: LoadLibrary failed: Windows error %lu", GetLastError()); fs::remove(copy, ec); return false; }
    auto start = reinterpret_cast<core::StartFn>(GetProcAddress(module, core::kStart));
    auto message = reinterpret_cast<core::MessageFn>(GetProcAddress(module, core::kMessage));
    auto stop = reinterpret_cast<core::StopFn>(GetProcAddress(module, core::kStop));
    host.reloadRequested = false;
    if (!start || !message || !stop || !start(&host)) {
        log::line("core: the core refused to start (exports %d%d%d; a CoreAbi.h or Bridge.h change needs a New Vegas restart)", !!start, !!message, !!stop);
        FreeLibrary(module);
        fs::remove(copy, ec);
        return false;
    }
    coreModule = module; coreMessage = message; coreStop = stop; coreFailed = false;
    return true;
}
void unloadCore() {
    if (!coreModule) return;
    const bool clean = coreStop();
    if (clean) retired.push_back({coreModule, modulePath(coreModule), GetTickCount64()});
    else log::line("core: the old core keeps some hooks and stays loaded");
    coreModule = nullptr; coreMessage = nullptr; coreStop = nullptr;
}
void freeRetired() {
    const auto now = GetTickCount64();
    for (auto it = retired.begin(); it != retired.end();) {
        if (now - it->at < 3000) { ++it; continue; }
        FreeLibrary(it->module);
        std::error_code ec;
        fs::remove(it->file, ec);
        it = retired.erase(it);
    }
}
void reload(const char* why) {
    const auto began = GetTickCount64();
    log::line("core: reloading (%s)", why);
    unloadCore();
    ++host.generation;
    if (loadCore()) log::line("core: reloaded in %llu ms (generation %u)", GetTickCount64() - began, host.generation);
    else log::line("core: no core running; save a working build to try again");
}
// Main thread, between frames: the probe asked for it, or a new build has settled for 0.5 s.
void maybeReload() {
    if (loading) return;
    if (coreModule && host.reloadRequested) { reload("probe"); return; }
    if (!hotReload) return;
    const auto now = GetTickCount64();
    if (now - lastCheck < 250) return;
    lastCheck = now;
    const auto current = stampOf(coreSource);
    if (!current.valid || current == loadedStamp) { pendingStamp = {}; return; }
    if (!(current == pendingStamp)) { pendingStamp = current; pendingSince = now; return; }
    if (now - pendingSince < 500) return;
    pendingStamp = {};
    reload(coreModule ? "new build" : "new build after a failed one");
}

void onMessage(nvse::Message* msg) {
    if (!msg) return;
    using namespace nvse;
    switch (msg->type) {
    case PostLoad: host.postLoaded = true; break;
    case NewGame: host.gameLoaded = true; loading = false; break;
    case PostLoadGame: host.gameLoaded = msg->data != nullptr; loading = false; break;
    case PreLoadGame: host.gameLoaded = false; loading = true; break;
    case ExitToMainMenu: host.gameLoaded = false; loading = false; break;
    case MainGameLoop: freeRetired(); maybeReload(); break;
    // Present runs while the game is paused too: keep the heartbeat alive here as well.
    case OnFramePresent: bridge.heartbeat(); break;
    default: break;
    }
    // The core loads with the first message, outside DllMain (xNVSE loads plugins from there).
    if (!coreModule && !coreFailed) loadCore();
    if (coreMessage) coreMessage(msg);
    if (msg->type == ExitGame || msg->type == ExitGameConsole) {
        bridge.close();
        log::close();
    }
}
}
extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const nvse::Interface* nvse, nvse::PluginInfo* info) {
    if (!nvse || !info) return false;
    *info = {1, "VegasCraft", 2};
    return !nvse->isEditor && !nvse->isNogore && nvse->nvseVersion >= 6 && nvse->runtimeVersion == nvse::kRuntime;
}
extern "C" __declspec(dllexport) bool NVSEPlugin_Load(const nvse::Interface* nvse) {
    nvse::PluginInfo info{};
    if (!NVSEPlugin_Query(nvse, &info) || !nvse->QueryInterface || !nvse->GetPluginHandle || !nvse->GetRuntimeDirectory) return false;
    auto* messages = static_cast<nvse::Messaging*>(nvse->QueryInterface(nvse::kMessaging));
    if (!messages || messages->version < 4 || !messages->RegisterListener) return false;
    const char* runtime = nvse->GetRuntimeDirectory();
    if (!runtime) return false;
    gameDir = fs::path(runtime).wstring();
    iniPath = (fs::path(runtime) / "Data" / "NVSE" / "Plugins" / "VegasCraft.ini").wstring();
    wchar_t mappingName[256]{};
    GetPrivateProfileStringW(L"Bridge", L"MappingName", kMappingName, mappingName, 256, iniPath.c_str());
    if (std::wcsncmp(mappingName, L"Local\\", 6) != 0) return false;
    log::open((fs::path(runtime) / "VegasCraft.log").wstring());
    if (!bridge.open(mappingName)) {
        log::line("Failed to create the bridge: Windows error %lu. Another New Vegas already hosts it.", bridge.error());
        log::close();
        return false;
    }
    log::line("VegasCraft 0.2.1: protocol=%u, %.0f units per block", proto::kVersion, proto::kUnitsPerBlock);
    if (!messages->RegisterListener(nvse->GetPluginHandle(), "NVSE", onMessage)) {
        bridge.close();
        log::close();
        return false;
    }
    hotReload = GetPrivateProfileIntW(L"Development", L"bHotReload", 1, iniPath.c_str()) != 0;
    coreSource = findCore();
    liveDir = coreSource.parent_path() / "live";
    clearLiveDir();
    auto* console = static_cast<nvse::Console*>(nvse->QueryInterface(nvse::kConsole));
    host.abi = core::kAbi;
    host.bridgeBytes = sizeof(Bridge);
    host.bridge = &bridge;
    host.gameDir = gameDir.c_str();
    host.ini = iniPath.c_str();
    host.console = console ? console->RunScriptLine : nullptr;
    log::line("VegasCraft: core %ls, hot reload %s", coreSource.c_str(), hotReload ? "on" : "off");
    // As early as possible: Minecraft takes about as long to start as New Vegas to reach its menu.
    if (hook::inGameProcess()) launcher::startMinecraft(gameDir, iniPath);
    log::line("VegasCraft: plugin initialization complete");
    return true;
}
