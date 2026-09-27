#include "CoreAbi.h"
#include "Game.h"
#include "Guard.h"
#include "Hook.h"
#include "Input.h"
#include "Log.h"
#include "Overlay.h"
#include "Probe.h"
#include "Profile.h"
#include "Runtime.h"
#include "Session.h"
#include "WorldRender.h"
#include <filesystem>

// The reloadable part of the plugin (see CoreAbi.h): everything but the bridge and the launcher.
using namespace vegas;
namespace {
core::Host* host = nullptr;
bool renderHooked = false;

IDirect3DDevice9* device() {
    void* renderer = nullptr;
    IDirect3DDevice9* d = nullptr;
    if (!hook::safeRead(reinterpret_cast<void*>(game::kRendererSlot), renderer) || !renderer) return nullptr;
    hook::safeRead(static_cast<unsigned char*>(renderer) + game::kRendererDevice, d);
    return d;
}
void hookRender() {
    if (renderHooked || !hook::inGameProcess()) return;
    worldrender::install();
    renderHooked = true;
}
}
extern "C" __declspec(dllexport) bool VegasCore_Start(core::Host* h) {
    if (!h || h->abi != core::kAbi || h->bridgeBytes != sizeof(Bridge) || !h->bridge || !h->gameDir || !h->ini) return false;
    host = h;
    const std::filesystem::path dir(h->gameDir);
    log::open((dir / "VegasCraft.log").wstring(), true);
    log::line("VegasCraft core (built %s %s), generation %u", __DATE__, __TIME__, h->generation);
    state().exportLand = GetPrivateProfileIntW(L"World", L"bExportLand", 1, h->ini) != 0;
    if (!state().exportLand) log::line("VegasCraft: land export off (bExportLand=0): no Minecraft overworld features");
    guard::install();
    session::start(h->bridge, h->generation);
    probe::init((dir / "VegasCraft.cmd").wstring(), h->console, &h->reloadRequested);
    if (h->postLoaded) hookRender();
    if (h->gameLoaded) session::onLoaded(true);
    log::line("VegasCraft: core ready");
    return true;
}
extern "C" __declspec(dllexport) void VegasCore_Message(nvse::Message* msg) {
    if (!msg || !host) return;
    using namespace nvse;
    switch (msg->type) {
    case PostLoad:
        hookRender();
        break;
    case NewGame:
        session::onLoaded(true);
        break;
    case PostLoadGame:
        // xNVSE encodes the bool in the pointer value, not in a pointed-to bool.
        session::onLoaded(msg->data != nullptr);
        break;
    case PreLoadGame:
        session::onPreLoad();
        session::frame();
        break;
    case ExitToMainMenu:
        session::onMainMenu();
        session::frame();
        break;
    case MainGameLoop:
        { profile::Scope t(profile::kSession); session::frame(); }
        probe::poll();
        break;
    case OnFramePresent:
        if (msg->data && msg->dataLen == sizeof(int) && *static_cast<const int*>(msg->data) == 0 && hook::inGameProcess()) {
            profile::frame();
            if (auto* d = device()) {
                profile::Scope t(profile::kPresent);
                guard::run([&] { worldrender::presentFallback(d); });
                guard::run([&] { overlay::present(d); });
            }
        }
        break;
    case ExitGame:
    case ExitGameConsole:
        session::onMainMenu();
        session::stop();
        break;
    default: break;
    }
}
extern "C" __declspec(dllexport) bool VegasCore_Stop() {
    if (!host) return true;
    log::line("VegasCraft: core stopping (generation %u)", host->generation);
    session::stop();
    probe::shutdown();
    input::releaseAll();
    bool clean = true;
    if (hook::inGameProcess()) {
        worldrender::shutdown();
        overlay::shutdown();
        clean &= input::uninstall();
        clean &= hook::restoreAll();
    }
    guard::uninstall();
    log::line(clean ? "VegasCraft: core stopped" : "VegasCraft: core stopped, but some hooks could not be taken back: it stays loaded");
    log::close();
    host = nullptr;
    return clean;
}
