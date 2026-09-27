#include "Launcher.h"
#include "Log.h"
#include <windows.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <exception>

// Port of SkyCraft's Launcher.cpp (MIT): Minecraft starts with New Vegas, hidden, through the
// player's own launcher (Prism by default) so it signs in with their Microsoft account.
namespace vegas::launcher {
namespace {
std::wstring expand(const std::wstring& s) {
    wchar_t out[MAX_PATH * 2]{};
    ExpandEnvironmentStringsW(s.c_str(), out, MAX_PATH * 2);
    return out;
}
template<class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
// Runs a program the way double-clicking it would: started by the desktop's Explorer, not by
// New Vegas. Under Mod Organizer that keeps Minecraft out of its virtual file system.
bool openFromDesktop(const std::wstring& file, const std::wstring& args, const std::wstring& dir, int show) {
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, reinterpret_cast<void**>(&windows)))) return false;
    VARIANT location{}; location.vt = VT_I4; location.lVal = CSIDL_DESKTOP;
    VARIANT empty{};
    long hwnd = 0;
    IDispatch* desktop = nullptr;
    IServiceProvider* services = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* view = nullptr;
    IDispatch* background = nullptr;
    IShellFolderViewDual* folderView = nullptr;
    IDispatch* application = nullptr;
    IShellDispatch2* shell = nullptr;
    bool ok = SUCCEEDED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop)) && desktop &&
        SUCCEEDED(desktop->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(&services))) &&
        SUCCEEDED(services->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(&browser))) &&
        SUCCEEDED(browser->QueryActiveShellView(&view)) &&
        SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, reinterpret_cast<void**>(&background))) &&
        SUCCEEDED(background->QueryInterface(IID_IShellFolderViewDual, reinterpret_cast<void**>(&folderView))) &&
        SUCCEEDED(folderView->get_Application(&application)) &&
        SUCCEEDED(application->QueryInterface(IID_IShellDispatch2, reinterpret_cast<void**>(&shell)));
    if (ok) {
        BSTR bfile = SysAllocString(file.c_str());
        VARIANT vargs{}, vdir{}, vop{}, vshow{};
        vargs.vt = vdir.vt = vop.vt = VT_BSTR;
        vargs.bstrVal = SysAllocString(args.c_str());
        vdir.bstrVal = SysAllocString(dir.c_str());
        vop.bstrVal = SysAllocString(L"open");
        vshow.vt = VT_I4; vshow.lVal = show;
        ok = SUCCEEDED(shell->ShellExecute(bfile, vargs, vdir, vop, vshow));
        SysFreeString(bfile);
        VariantClear(&vargs); VariantClear(&vdir); VariantClear(&vop);
    }
    release(shell); release(application); release(folderView); release(background);
    release(view); release(browser); release(services); release(desktop); release(windows);
    return ok;
}
std::filesystem::path findPrism() {
    for (const wchar_t* candidate : {L"%LOCALAPPDATA%\\Programs\\PrismLauncher\\prismlauncher.exe", L"%ProgramFiles%\\PrismLauncher\\prismlauncher.exe"}) {
        std::filesystem::path p = expand(candidate);
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}
// Unpacks the bundled portable Prism Launcher (with its ready "VegasCraft" instance) to
// %LOCALAPPDATA%\VegasCraft, again whenever this VegasCraft brings a different bundle. Prism's own
// data there (the sign-in, downloaded Minecraft and Java, the world) is kept.
std::filesystem::path ensureBundle(const std::filesystem::path& bundle) {
    const std::filesystem::path dir = expand(L"%LOCALAPPDATA%\\VegasCraft");
    const auto prism = dir / "Prism" / "prismlauncher.exe";
    std::error_code ec;
    const auto stamp = std::to_string(std::filesystem::file_size(bundle, ec)) + " " +
        std::to_string(std::filesystem::last_write_time(bundle, ec).time_since_epoch().count());
    std::string installed;
    if (std::ifstream in{dir / "bundle.stamp"}; in) std::getline(in, installed);
    if (installed == stamp && std::filesystem::exists(prism)) return prism;
    log::line("Minecraft: unpacking VegasCraft's Minecraft to %ls", dir.c_str());
    std::filesystem::create_directories(dir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(dir / "Prism" / "instances" / "VegasCraft" / ".minecraft" / "mods", ec)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("vegascraft-", 0) == 0 || name.rfind("fabric-api-", 0) == 0 || name.rfind("e4mc-", 0) == 0) std::filesystem::remove(entry.path(), ec);
    }
    const auto copy = dir / "bundle.zip";
    if (!std::filesystem::copy_file(bundle, copy, std::filesystem::copy_options::overwrite_existing, ec)) {
        log::line("Minecraft: couldn't copy %ls (%s)", bundle.c_str(), ec.message().c_str());
        return {};
    }
    std::wstring command = L"\"" + expand(L"%SystemRoot%\\System32\\tar.exe") + L"\" -xf \"" + copy.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    std::filesystem::remove(copy, ec);
    if (code != 0 || !std::filesystem::exists(prism)) {
        log::line("Minecraft: unpacking failed (tar exit code %lu)", code);
        return {};
    }
    const auto cfg = dir / "Prism" / "prismlauncher.cfg";
    if (!std::filesystem::exists(cfg)) std::filesystem::copy_file(dir / "defaults" / "prismlauncher.cfg", cfg, ec);
    std::ofstream(dir / "bundle.stamp") << stamp;
    return prism;
}
bool start(const std::filesystem::path& program, const std::wstring& args) {
    const auto ext = program.extension().wstring();
    const bool script = _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0;
    const std::wstring dir = program.parent_path().wstring();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool viaDesktop = openFromDesktop(program.wstring(), args, dir, script ? SW_HIDE : SW_SHOWNORMAL);
    if (SUCCEEDED(com)) CoUninitialize();
    if (viaDesktop) { log::line("Minecraft: started %ls %ls", program.c_str(), args.c_str()); return true; }
    std::wstring command = script ? L"cmd.exe /c \"\"" + program.wstring() + L"\" " + args + L"\"" : L"\"" + program.wstring() + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, script ? CREATE_NO_WINDOW : 0, nullptr, dir.c_str(), &si, &pi)) {
        log::line("Minecraft: couldn't start %ls (error %lu)", program.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log::line("Minecraft: started %ls (directly)", program.c_str());
    return true;
}
std::wstring setting(const std::wstring& ini, const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[1024]{};
    GetPrivateProfileStringW(L"Minecraft", key, fallback, value, 1024, ini.c_str());
    std::wstring s = value;
    if (auto semi = s.find(L';'); semi != std::wstring::npos) s.erase(semi);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}
}
bool minecraftRunning() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\VegasCraft_v1_minecraft");
    if (!mutex) return false;
    CloseHandle(mutex);
    return true;
}
static void startMinecraftWorker(const std::wstring& gameDir, const std::wstring& ini) {
    log::line("Minecraft: launcher worker started");
    if (GetPrivateProfileIntW(L"Minecraft", L"bStartWithNewVegas", 1, ini.c_str()) == 0) {
        log::line("Minecraft: not started (bStartWithNewVegas = 0)");
        return;
    }
    if (minecraftRunning()) { log::line("Minecraft: already running"); return; }
    const std::filesystem::path chosen = expand(setting(ini, L"sLauncher", L""));
    const std::wstring args = setting(ini, L"sArguments", L"--launch VegasCraft");
    const auto bundle = std::filesystem::path(gameDir) / "Data" / "NVSE" / "Plugins" / "VegasCraft" / "VegasCraft-Minecraft.zip";
    std::filesystem::path program;
    if (!chosen.empty()) {
        if (!std::filesystem::exists(chosen)) { log::line("Minecraft: not started: %ls doesn't exist (sLauncher)", chosen.c_str()); return; }
        program = chosen;
    } else if (std::filesystem::exists(bundle)) {
        program = ensureBundle(bundle);
    } else {
        program = findPrism();
    }
    if (program.empty()) {
        log::line("Minecraft: not started: no VegasCraft-Minecraft.zip next to the plugin and no Prism Launcher installed; set sLauncher in VegasCraft.ini");
        return;
    }
    start(program, args);
}
void startMinecraft(const std::wstring& gameDir, const std::wstring& ini) {
    // NVSE loads plugins from DllMain. COM/Explorer and waiting for tar here can deadlock
    // under the Windows loader lock. Like SkyCraft, let a detached worker do all launch
    // work after plugin loading returns; never join that worker from DllMain.
    try {
        std::thread([gameDir, ini] {
            try {
                startMinecraftWorker(gameDir, ini);
            } catch (const std::exception& e) {
                log::line("Minecraft: launcher failed: %s", e.what());
            } catch (...) {
                log::line("Minecraft: launcher failed with an unknown exception");
            }
        }).detach();
    } catch (const std::exception& e) {
        log::line("Minecraft: couldn't create launcher worker: %s", e.what());
    }
}
} // namespace vegas::launcher
