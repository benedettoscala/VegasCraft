#include "Bridge.h"
#include "NvseAbi.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

using namespace vegas::nvse;
namespace {
void (*listener)(Message*) = nullptr;
std::string runtimeDir;
bool allowListener = true;
bool registerListener(PluginHandle handle, const char* sender, void (*callback)(Message*)) {
    if (handle != 7 || std::strcmp(sender, "NVSE") || !allowListener) return false;
    listener = callback; return true;
}
Messaging messaging{4, registerListener, nullptr};
void* queryInterface(U32 id) { return id == kMessaging ? &messaging : nullptr; }
PluginHandle handle() { return 7; }
const char* runtime() { return runtimeDir.c_str(); }
void check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void message(U32 type, void* data = nullptr) {
    Message m{"NVSE", type, data ? 1u : 0u, data}; listener(&m);
}
template<class T> void set(void* p, std::size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(p) + offset, &value, sizeof(value));
}
vegas::proto::SkyState snapshot(const unsigned char* base) {
    vegas::proto::SkyState result{};
    std::memcpy(&result, base + vegas::proto::kOffSkyState, sizeof(result));
    return result;
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        // Reserve the game's address range before the large mapping can occupy it.
        auto* addressPage = VirtualAlloc(reinterpret_cast<void*>(0x011D0000), 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        check(addressPage == reinterpret_cast<void*>(0x011D0000), "Cannot allocate synthetic player address");
        const auto testRoot = std::filesystem::current_path() / ("test_runtime_" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(testRoot / "Data/NVSE/Plugins");
        runtimeDir = testRoot.string();
        const std::wstring mappingName = L"Local\\VegasCraft_fixture_" + std::to_wstring(GetCurrentProcessId());
        auto ini = (testRoot / "Data/NVSE/Plugins/VegasCraft.ini").wstring();
        check(WritePrivateProfileStringW(L"Bridge", L"MappingName", mappingName.c_str(), ini.c_str()), "Cannot configure isolated test mapping");
        auto dll = LoadLibraryA(argv[1]);
        check(dll != nullptr, "DLL cannot load (wrong architecture or missing dependencies)");
        using Query = bool (*)(const Interface*, PluginInfo*);
        using Load = bool (*)(const Interface*);
        auto query = reinterpret_cast<Query>(GetProcAddress(dll, "NVSEPlugin_Query"));
        auto load = reinterpret_cast<Load>(GetProcAddress(dll, "NVSEPlugin_Load"));
        check(query && load, "Expected xNVSE export names are missing");
        Interface nvse{};
        nvse.nvseVersion = 6; nvse.runtimeVersion = kRuntime;
        nvse.QueryInterface = queryInterface; nvse.GetPluginHandle = handle;
        nvse.GetRuntimeDirectory = runtime;
        PluginInfo info{};
        check(!query(nullptr, &info) && !query(&nvse, nullptr), "Null interfaces accepted");
        check(query(&nvse, &info) && !std::strcmp(info.name, "VegasCraft"), "Supported executable rejected");
        nvse.runtimeVersion = kRuntime + 1; check(!query(&nvse, &info), "Unknown executable accepted");
        nvse.runtimeVersion = kRuntime; nvse.isEditor = 1; check(!query(&nvse, &info), "GECK accepted");
        nvse.isEditor = 0; nvse.isNogore = 1; check(!query(&nvse, &info), "NoGore accepted");
        nvse.isNogore = 0; nvse.nvseVersion = 5; check(!query(&nvse, &info), "Old NVSE accepted");
        nvse.nvseVersion = 6; messaging.version = 3; check(!load(&nvse), "Old messaging accepted");
        messaging.version = 4;
        allowListener = false; check(!load(&nvse), "Listener registration failure accepted");
        allowListener = true;
        check(load(&nvse) && listener, "Plugin failed to load");
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
        check(mapping != nullptr, "Mapping was not created");
        auto* base = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        check(base != nullptr, "Mapping cannot be opened");
        vegas::Bridge duplicate;
        check(!duplicate.open(mappingName) && duplicate.error() == ERROR_ALREADY_EXISTS, "Second host accepted");
        auto* header = reinterpret_cast<vegas::proto::Header*>(base);
        check(header->magic == vegas::proto::kMagic && header->version == vegas::proto::kVersion, "Wrong protocol header");
        auto* water = reinterpret_cast<vegas::proto::WaterGrid*>(base + vegas::proto::kOffWaterGrid);
        for (auto level : water->surface) check(level == vegas::proto::kNoWater, "Uninitialized water creates ghost swimming");
        message(MainGameLoop); check(snapshot(base).flags == 0, "Main menu published as active world");
        alignas(8) unsigned char player[0x80]{}, cell[0xE0]{}, world[0x20]{};
        *reinterpret_cast<void**>(0x011DEA3C) = player;
        set(player, 0x30, 700.0f); set(player, 0x34, 1400.0f); set(player, 0x38, 2100.0f);
        set(player, 0x40, static_cast<void*>(cell));
        set(cell, 0xC0, static_cast<void*>(world)); set(world, 0xC, U32{0xAABB}); set(cell, 0xC, U32{0xCCDD});
        message(NewGame); message(MainGameLoop);
        auto st = snapshot(base);
        check(st.flags == vegas::proto::kSkyInGame && st.worldId == 0xAABB && st.posX == 10 && st.posY == 30 && st.posZ == -20,
            "Player/coordinate/worldspace export mismatch");
        check(st.yaw == 180, "Heading mapping mismatch");
        auto epoch = st.collisionEpoch;
        set(cell, 0xC0, static_cast<void*>(nullptr)); message(MainGameLoop); st = snapshot(base);
        check(st.worldId == 0xCCDD && st.collisionEpoch > epoch, "Interior cell transition lost");
        message(PreLoadGame); check(snapshot(base).flags == vegas::proto::kSkyLoading, "Load flag missing");
        message(PostLoadGame, nullptr); message(MainGameLoop); check(snapshot(base).flags == 0, "Failed load accepted");
        message(PostLoadGame, reinterpret_cast<void*>(1)); message(MainGameLoop);
        check(snapshot(base).flags & vegas::proto::kSkyInGame, "Successful load did not resume");
        // Hot reload: the probe asks, the loader swaps the core between frames, the game goes on.
        epoch = snapshot(base).collisionEpoch;
        { std::FILE* cmd = std::fopen((testRoot / "VegasCraft.cmd").string().c_str(), "w"); check(cmd != nullptr, "Cannot write probe"); std::fputs("reload\n", cmd); std::fclose(cmd); }
        Sleep(250); message(MainGameLoop); message(MainGameLoop);
        st = snapshot(base);
        check((st.flags & vegas::proto::kSkyInGame) && st.collisionEpoch > 0x10000 && st.collisionEpoch > epoch,"Hot reload lost the game or kept the collision epoch");
        *reinterpret_cast<void**>(0x011DEA3C) = nullptr; message(MainGameLoop);
        check(snapshot(base).flags == 0, "Null player dereferenced or exported");
        message(ExitToMainMenu); check(snapshot(base).flags == 0, "Menu did not clear gameplay state");
        message(ExitGameConsole); check(header->skyrimHeartbeatMs == 0, "Shutdown retained live heartbeat");
        UnmapViewOfFile(base); CloseHandle(mapping);
        VirtualFree(addressPage, 0, MEM_RELEASE); FreeLibrary(dll);
        vegas::Bridge first;
        const auto lifecycleName = mappingName + L"_lifecycle";
        check(first.open(lifecycleName), "Cannot create lifecycle mapping");
        auto stale = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, lifecycleName.c_str());
        first.close();
        // A Minecraft that outlived New Vegas keeps the object; the next host reuses and resets it.
        auto* staleView = static_cast<unsigned char*>(MapViewOfFile(stale, FILE_MAP_ALL_ACCESS, 0, 0, 0x1000));
        check(staleView != nullptr, "Cannot map stale reader view");
        reinterpret_cast<vegas::proto::SkyState*>(staleView + vegas::proto::kOffSkyState)->flags = 0xFF;
        vegas::Bridge restart;
        check(restart.open(lifecycleName), "Restart refused a mapping retained by Minecraft");
        auto* staleHeader = reinterpret_cast<vegas::proto::Header*>(staleView);
        check(staleHeader->magic == vegas::proto::kMagic && staleHeader->skyrimPid == GetCurrentProcessId(), "Reused mapping header not refreshed");
        check(reinterpret_cast<vegas::proto::SkyState*>(staleView + vegas::proto::kOffSkyState)->flags == 0, "Reused mapping kept old host state");
        UnmapViewOfFile(staleView);
        CloseHandle(stale);
        auto mc = vegas::toMinecraft({-700, 1400, 2100}, 70);
        auto nv = vegas::toNewVegas(mc, 70);
        check(nv.x == -700 && nv.y == 1400 && nv.z == 2100, "Coordinate roundtrip failed");
        check(!restart.minecraftAlive(), "Absent peer considered alive");
        std::puts("PASS exports, runtime gates, listener failure, mapping ownership, water, player, cells, loads, hot reload, shutdown, restart, coordinates");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
}
