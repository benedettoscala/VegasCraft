#define DIRECTINPUT_VERSION 0x0800
#include "Input.h"
#include "Hook.h"
#include "Homography.h"
#include "Log.h"
#include "Runtime.h"
#include <dinput.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

// Port of SkyCraft's Input.cpp (MIT) to New Vegas' DirectInput. New Vegas polls its keyboard
// and mouse with GetDeviceState (menus and the console also read buffered keyboard data), so
// the hooks sit on dinput8's own device vtable, below xNVSE's DirectInput wrapper.
namespace vegas::input {
namespace {
// DirectInput scan code -> SDL scancode / USB HID usage (what Minecraft uses).
constexpr auto kDikToSdl = [] {
    std::array<std::uint16_t, 256> t{};
    t[0x01] = 41;
    for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);
    t[0x0B] = 39;
    t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;
    t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;
    t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;
    t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;
    t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;
    t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;
    t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;
    t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;
    t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;
    t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;
    for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);
    t[0x45] = 83, t[0x46] = 71;
    t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;
    t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;
    t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;
    t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;
    t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;
    t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;
    t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;
    t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;
    return t;
}();
// Keys New Vegas keeps while Minecraft drives the player; everything else is Minecraft's.
constexpr std::uint8_t kDikEscape = 0x01, kDikTab = 0x0F, kDikGrave = 0x29, kDikF9 = 0x43;
// Remapped for New Vegas: G activates (its E), H waits (its T). O opens Minecraft's menu.
constexpr std::uint8_t kDikG = 0x22, kDikH = 0x23, kDikO = 0x18, kDikE = 0x12, kDikT = 0x14;
// With a New Vegas weapon wielded, R reloads it in New Vegas.
constexpr std::uint8_t kDikR = 0x13;
bool newVegasKey(std::uint32_t dik) { return dik == kDikEscape || dik == kDikTab || dik == kDikGrave || dik == kDikF9; }

using GetStateFn = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, LPVOID);
using GetDataFn = HRESULT(__stdcall*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
GetStateFn originalState = nullptr;
GetDataFn originalData = nullptr;
IDirectInputDevice8A* keyboardDevice = nullptr;
std::uint8_t previousKeys[256]{};
std::uint8_t previousButtons[8]{};
float lookDx = 0, lookDy = 0;
bool wasRouting = false;
WNDPROC previousProc = nullptr;
HWND window = nullptr;

bool routing() {
    auto& st = state();
    return (st.puppeting && !st.nvMenuOpen) || st.pipBoyMcPage;
}
constexpr std::uint8_t kDikF1 = 0x3B, kDikF2 = 0x3C, kDikF3 = 0x3D, kDikF4 = 0x3E;
bool pageSawScreen = false;  // Minecraft's screen opened since the page did

// Pip-Boy menus, as its three buttons: 0 STATS, 1 ITEMS, 2 DATA. ITEMS (and, as they are made, the others)
// are Minecraft's page on the screen; the rest are New Vegas' own screens.
int pipSection = 1;
int knobTicks = 0;
std::uint32_t pressedMask = 0;
ULONGLONG pressedUntil[3]{};
bool stateSent = false;
std::int32_t sentSection = -1, sentMask = -1, sentKnob = 0;
void sendPipBoyState(bool force) {
    auto& st = state();
    if (!st.bridge) return;
    const auto now = GetTickCount64();
    std::uint32_t mask = pressedMask;
    for (int i = 0; i < 3; ++i) if (now < pressedUntil[i]) mask |= 1u << i;  // a tap shows for a moment
    if (!force && stateSent && sentSection == pipSection && sentMask == static_cast<std::int32_t>(mask) && sentKnob == knobTicks) return;
    stateSent = true; sentSection = pipSection; sentMask = static_cast<std::int32_t>(mask); sentKnob = knobTicks;
    st.bridge->pushInput(proto::kInPipBoyState, static_cast<std::uint16_t>(pipSection), static_cast<std::int32_t>(mask), knobTicks);
}
// Where Minecraft's frame is drawn on the Pip-Boy's screen: the screen quad shrunk a little.
void pageQuad(float* out) { homography::inner(state().pipBoyHole, homography::kPageInset, out); }
// A point of the window on the Pip-Boy's page, in the rectangle it is laid out in (false: no page).
bool pagePoint(double wx, double wy, int& px, int& py) {
    auto& st = state();
    if (!st.pipBoyPageValid || !st.pipBoyHoleValid) return false;
    float q[8];
    pageQuad(q);
    const auto inv = homography::inverse(homography::squareToQuad(q));
    double u, v;
    homography::apply(inv, wx, wy, u, v);
    u = std::clamp(u, 0.0, 1.0); v = std::clamp(v, 0.0, 1.0);
    px = static_cast<int>(st.pipBoyPage[0] + u * (st.pipBoyPage[2] - st.pipBoyPage[0]));
    py = static_cast<int>(st.pipBoyPage[1] + v * (st.pipBoyPage[3] - st.pipBoyPage[1]));
    return true;
}
// The Pip-Boy's Minecraft page on or off; Minecraft opens or closes its screen over the Pip-Boy's.
void setPage(bool on) {
    auto& st = state();
    if (st.pipBoyMcPage == on || !st.bridge) return;
    if (on && !st.pipBoyHoleValid) return;  // the screen's corners aren't known yet
    st.pipBoyMcPage = on;
    pageSawScreen = false;
    std::int32_t a = 0, b = 0;
    if (on) {
        const float w = float(std::max(1, st.viewportW.load())), h = float(std::max(1, st.viewportH.load()));
        float q[8], r[4];
        pageQuad(q);
        homography::insideRect(q, r);
        std::memcpy(st.pipBoyPage, r, sizeof(r));
        st.pipBoyPageValid = r[2] > r[0] + 8 && r[3] > r[1] + 8;
        auto norm = [](float v, float size) { return static_cast<std::int32_t>(std::clamp(v / size, 0.0f, 1.0f) * 65535.0f); };
        a = (norm(r[0], w) << 16) | norm(r[1], h);
        b = (norm(r[2], w) << 16) | norm(r[3], h);
        // The cursor starts in the middle of the screen.
        st.cursorX = static_cast<int>((st.pipBoyHole[0] + st.pipBoyHole[2] + st.pipBoyHole[4] + st.pipBoyHole[6]) * 0.25f);
        st.cursorY = static_cast<int>((st.pipBoyHole[1] + st.pipBoyHole[3] + st.pipBoyHole[5] + st.pipBoyHole[7]) * 0.25f);
    } else {
        st.pipBoyPageValid = false;
    }
    releaseAll();
    st.bridge->pushInput(proto::kInPipBoyPage, on ? 1 : 0, a, b);
    log::line("input: Pip-Boy Minecraft page %s (rect %.0f %.0f %.0f %.0f, valid %d, view %dx%d)", on ? "open" : "closed", st.pipBoyPage[0], st.pipBoyPage[1], st.pipBoyPage[2], st.pipBoyPage[3], st.pipBoyPageValid, st.viewportW.load(), st.viewportH.load());
}
bool pipBoyPagePending = false;   // the Pip-Boy is up and its page waits for the screen to settle
float pipBoyLastHole[8]{};
int pipBoyStill = 0;              // frames the screen's corners have held still on screen
bool pipBoyScreenSettled() { return state().pipBoyHoleValid && pipBoyStill >= 10; }
// A menu button (key or tap): the page shows that menu (Minecraft draws it).
void pressSection(int section) {
    pipSection = section;
    // Before the Pip-Boy has risen and settled its screen's corners are not usable: pipBoyPage() opens the page then.
    if (!pipBoyScreenSettled()) pipBoyPagePending = true;
    else setPage(true);
    sendPipBoyState(true);
}
void transition(bool now) {
    if (wasRouting && !now) releaseAll();
    wasRouting = now;
}
// The character a key press types, from DirectInput's state and the keyboard layout in use
// (Shift, AltGr, Caps Lock), for Minecraft's chat and text fields. New Vegas reads its
// keyboard through DirectInput, so its window never gets the WM_CHAR messages that would carry it.
void typeCharacter(std::uint32_t dik, const std::uint8_t* raw) {
    auto* link = state().bridge;
    const bool shift = (raw[0x2A] | raw[0x36]) & 0x80, ctrl = (raw[0x1D] | raw[0x9D]) & 0x80, altGr = raw[0xB8] & 0x80;
    if (ctrl && !altGr) return;  // Minecraft's shortcuts (Ctrl+V, ...), not text
    const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    const UINT scan = (dik & 0x7F) | ((dik & 0x80) ? 0xE000 : 0);
    const UINT vk = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, layout);
    if (!vk) return;
    BYTE keyState[256]{};
    if (shift) keyState[VK_SHIFT] = keyState[VK_LSHIFT] = 0x80;
    if (altGr) keyState[VK_CONTROL] = keyState[VK_LCONTROL] = keyState[VK_MENU] = keyState[VK_RMENU] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) keyState[VK_CAPITAL] = 1;
    keyState[vk] = 0x80;
    wchar_t out[4]{};
    const int n = ToUnicodeEx(vk, scan, keyState, out, 4, 4 /* keep the dead-key state */, layout);
    for (int i = 0; i < n; ++i)
        if (out[i] >= 0x20 && out[i] != 0x7F) link->pushInput(proto::kInText, 0, static_cast<std::int32_t>(out[i]));
}
void keyboard(std::uint8_t* keys) {
    auto& st = state();
    // In the Pip-Boy: F1, F2, F3 are its STATS, ITEMS, DATA buttons (as in New Vegas); F4 on the
    // ITEMS page switches between New Vegas' items and Minecraft's inventory and crafting.
    if (st.pipBoyUp) {
        static bool was[4]{};
        static constexpr std::uint8_t kKeys[4] = {kDikF1, kDikF2, kDikF3, kDikF4};
        const bool injecting = GetTickCount64() < st.injectUntil.load();
        for (int i = 0; i < 4; ++i) {
            const bool down = (keys[kKeys[i]] & 0x80) || (injecting && st.injectDik.load() == kKeys[i]);
            if (down == was[i]) continue;
            was[i] = down;
            if (i < 3) {
                if (down) { pressedMask |= 1u << i; pressedUntil[i] = GetTickCount64() + 160; pressSection(i); }
                else { pressedMask &= ~(1u << i); sendPipBoyState(false); }
            } else if (down && st.pipBoyMcPage && st.bridge) {
                st.bridge->pushInput(proto::kInPipBoyPage, 2, 0, 0);  // the page's other tab
            }
        }
        sendPipBoyState(false);
    }
    const bool route = routing();
    transition(route);
    std::uint8_t raw[256];
    std::memcpy(raw, keys, sizeof(raw));
    if (route) {
        auto* link = st.bridge;
        const bool screen = st.mcScreenOpen;
        for (std::uint32_t k = 0; k < 256; ++k) {
            const bool down = raw[k] & 0x80, was = previousKeys[k] & 0x80;
            if (down == was) continue;
            if (!screen) {
                if (newVegasKey(k) || k == kDikG || k == kDikH || (k == kDikR && st.nvWeapon)) continue;
                if (k == kDikO) {
                    if (down) { releaseAll(); link->pushInput(proto::kInOpenMenu, 0); }
                    continue;
                }
            }
            if (st.pipBoyUp && (k == kDikF1 || k == kDikF2 || k == kDikF3 || k == kDikF4)) continue;
            if (auto sdl = kDikToSdl[k]) link->pushInput(proto::kInKey, sdl, down ? 1 : 0);
            if (screen && down) typeCharacter(k, raw);
        }
        // What New Vegas sees: only its own keys (none while a Minecraft screen is open; Tab on the
        // Pip-Boy's Minecraft page, which closes the Pip-Boy).
        std::memset(keys, 0, 256);
        if (st.pipBoyMcPage) keys[kDikTab] = raw[kDikTab];
        if (!screen) {
            for (std::uint32_t k : {kDikEscape, kDikTab, kDikGrave, kDikF9}) keys[k] = raw[k];
            keys[kDikE] = raw[kDikG];
            keys[kDikT] = raw[kDikH];
            if (st.nvWeapon) keys[kDikR] = raw[kDikR];
        }
    } else if (st.nativeInteraction && !st.nvMenuOpen) {
        // Sitting/sleeping leaves movement to New Vegas. Keep G usable to stand up;
        // preserve its ordinary E/T keys as well during this native interaction.
        keys[kDikE] |= raw[kDikG]; keys[kDikT] |= raw[kDikH];
        keys[kDikG] = keys[kDikH] = 0;
    }
    if (const auto dik = st.injectDik.load(); dik && dik < 256 && GetTickCount64() < st.injectUntil.load()) keys[dik] = 0x80;
    std::memcpy(previousKeys, raw, sizeof(raw));
}
// Fire (left) and aim (right) belong to New Vegas while it wields its own weapon.
bool nvButton(int b) { return (b == 0 || b == 1) && state().nvWeapon && !state().mcScreenOpen; }
void mouse(void* data, DWORD bytes) {
    auto* m = static_cast<DIMOUSESTATE2*>(data);
    const int buttons = bytes >= sizeof(DIMOUSESTATE2) ? 8 : 4;
    auto& st = state();
    if (st.pipBoyUp && m->lZ) { knobTicks += m->lZ > 0 ? 1 : -1; sendPipBoyState(false); }  // the knob turns with the wheel
    const bool route = routing();
    transition(route);
    if (route) {
        auto* link = st.bridge;
        if (st.mcScreenOpen) {
            const int x = std::clamp(st.cursorX.load() + static_cast<int>(m->lX), 0, st.viewportW.load() - 1);
            const int y = std::clamp(st.cursorY.load() + static_cast<int>(m->lY), 0, st.viewportH.load() - 1);
            if (x != st.cursorX || y != st.cursorY) {
                st.cursorX = x; st.cursorY = y;
                int px = x, py = y;
                if (st.pipBoyMcPage) pagePoint(x, y, px, py);  // on the Pip-Boy's page: where that spot of the screen is in Minecraft's frame
                link->pushInput(proto::kInCursor, 0, px, py);
            }
        } else {
            lookDx += static_cast<float>(m->lX);
            lookDy += static_cast<float>(m->lY);
        }
        if (m->lZ) link->pushInput(proto::kInScroll, 0, m->lZ > 0 ? 120 : -120);
        static constexpr std::uint16_t kSdlButton[8] = {1, 3, 2, 4, 5, 0, 0, 0};
        for (int b = 0; b < buttons; ++b) {
            const bool down = m->rgbButtons[b] & 0x80, was = previousButtons[b] & 0x80;
            if (down != was && kSdlButton[b] && !nvButton(b)) link->pushInput(proto::kInMouseButton, kSdlButton[b], down ? 1 : 0);
        }
    }
    for (int b = 0; b < buttons; ++b) previousButtons[b] = m->rgbButtons[b];
    if (route) {
        m->lX = m->lY = m->lZ = 0;
        for (int b = 0; b < buttons; ++b)
            if (!nvButton(b)) m->rgbButtons[b] = 0;
    }
    if (GetTickCount64() < st.injectMouseUntil.load()) m->rgbButtons[std::clamp(st.injectMouseButton.load(), 0, 1)] = 0x80;
}
HRESULT afterState(IDirectInputDevice8A* self, DWORD bytes, LPVOID data, HRESULT hr) {
    if (hr == DI_OK && data) {
        if (bytes == 256) { keyboardDevice = self; keyboard(static_cast<std::uint8_t*>(data)); }
        else if (bytes == sizeof(DIMOUSESTATE2) || bytes == sizeof(DIMOUSESTATE)) mouse(data, bytes);
    }
    return hr;
}
HRESULT __stdcall getState(IDirectInputDevice8A* self, DWORD bytes, LPVOID data) { return afterState(self, bytes, data, originalState(self, bytes, data)); }
// dinput8 doesn't always give the mouse the keyboard's vtable (seen after a core reload).
GetStateFn originalMouseState = nullptr;
HRESULT __stdcall getMouseState(IDirectInputDevice8A* self, DWORD bytes, LPVOID data) { return afterState(self, bytes, data, originalMouseState(self, bytes, data)); }
// Buffered keyboard data feeds New Vegas' menus and console: while Minecraft drives the
// player, only New Vegas' own keys (and the remapped G/H) get through.
HRESULT __stdcall getDataFiltered(IDirectInputDevice8A* self, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    const HRESULT hr = originalData(self, size, data, count, flags);
    const bool route = routing();
    const bool native = state().nativeInteraction && !state().nvMenuOpen;
    if (FAILED(hr) || !data || !count || self != keyboardDevice || size < sizeof(DIDEVICEOBJECTDATA) || (!route && !native)) return hr;
    const bool screen = state().mcScreenOpen;
    DWORD kept = 0;
    auto* bytes = reinterpret_cast<unsigned char*>(data);
    for (DWORD i = 0; i < *count; ++i) {
        auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(bytes + i * size);
        DWORD key = e->dwOfs;
        if (route && screen && !(state().pipBoyMcPage && key == kDikTab)) continue;
        if (key == kDikG) key = kDikE;
        else if (key == kDikH) key = kDikT;
        else if (route && !newVegasKey(key)) continue;
        auto* out = reinterpret_cast<DIDEVICEOBJECTDATA*>(bytes + kept * size);
        if (out != e) std::memmove(out, e, size);
        out->dwOfs = key;
        ++kept;
    }
    *count = kept;
    return hr;
}
// Development key injection (the probe's nvkey): the press and release as buffered events too.
HRESULT __stdcall getData(IDirectInputDevice8A* self, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) {
    const DWORD capacity = count ? *count : 0;
    const HRESULT hr = getDataFiltered(self, size, data, count, flags);
    auto& st = state();
    static bool down = false;
    const auto dik = st.injectDik.load();
    const bool active = dik && GetTickCount64() < st.injectUntil.load();
    if (FAILED(hr) || !data || !count || self != keyboardDevice || size < sizeof(DIDEVICEOBJECTDATA) || *count >= capacity || active == down) return hr;
    auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<unsigned char*>(data) + *count * size);
    std::memset(e, 0, size);
    e->dwOfs = dik;
    e->dwData = active ? 0x80 : 0;
    e->dwTimeStamp = GetTickCount();
    ++*count;
    down = active;
    return hr;
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Text for Minecraft comes from the key presses (typeCharacter), not from WM_CHAR.
    return CallWindowProcW(previousProc, hwnd, msg, wp, lp);
}
}
bool install() {
    if (originalState) return true;
    IDirectInput8A* di = nullptr;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&di), nullptr)) || !di) {
        log::line("input: DirectInput8Create failed");
        return false;
    }
    IDirectInputDevice8A* keyboardProbe = nullptr;
    IDirectInputDevice8A* mouseProbe = nullptr;
    di->CreateDevice(GUID_SysKeyboard, &keyboardProbe, nullptr);
    di->CreateDevice(GUID_SysMouse, &mouseProbe, nullptr);
    bool ok = false;
    if (keyboardProbe) {
        auto** vtable = *reinterpret_cast<void***>(keyboardProbe);
        originalState = reinterpret_cast<GetStateFn>(hook::replaceSlot(&vtable[9], reinterpret_cast<void*>(&getState)));
        originalData = reinterpret_cast<GetDataFn>(hook::replaceSlot(&vtable[10], reinterpret_cast<void*>(&getData)));
        ok = originalState && originalData;
        if (auto** mouseTable = mouseProbe ? *reinterpret_cast<void***>(mouseProbe) : nullptr; mouseTable && mouseTable != vtable) {
            originalMouseState = reinterpret_cast<GetStateFn>(hook::replaceSlot(&mouseTable[9], reinterpret_cast<void*>(&getMouseState)));
            log::line("input: the mouse has its own DirectInput vtable: %s", originalMouseState ? "hooked too" : "not hooked, mouse input is not bridged");
        }
    }
    if (mouseProbe) mouseProbe->Release();
    if (keyboardProbe) keyboardProbe->Release();
    di->Release();
    log::line("input: DirectInput hooks %s", ok ? "installed" : "failed");
    return ok;
}
void selectSection(int section) { pressSection(std::clamp(section, 0, 2)); }
void nativeView(int section) {
    // New Vegas' own screen for a menu the page doesn't have (yet): the page goes, the tab's key goes in.
    auto& st = state();
    pipSection = std::clamp(section, 0, 2);
    setPage(false);
    st.injectDik = pipSection == 0 ? kDikF1 : pipSection == 1 ? kDikF2 : kDikF3;
    st.injectUntil = GetTickCount64() + 150;
    sendPipBoyState(true);
}
void setCursor(int x, int y) {
    auto& st = state();
    if (!st.bridge) return;
    x = std::clamp(x, 0, std::max(0, st.viewportW.load() - 1));
    y = std::clamp(y, 0, std::max(0, st.viewportH.load() - 1));
    st.cursorX = x; st.cursorY = y;
    int px = x, py = y;
    if (st.pipBoyMcPage) pagePoint(x, y, px, py);
    st.bridge->pushInput(proto::kInCursor, 0, px, py);
}
void pipBoyPage() {
    auto& st = state();
    // The Pip-Boy came up: on its last menu. New Vegas opens on its own last tab, so the page, or the
    // New Vegas tab, is set once the screen's corners are known.
    static bool wasUp = false;
    bool& pending = pipBoyPagePending;
    if (st.pipBoyUp && !wasUp) pending = true;
    if (!st.pipBoyUp) pending = false;
    wasUp = st.pipBoyUp;
    // The Pip-Boy rises into view first: its screen counts once it is on the screen and has stopped.
    float (&lastHole)[8] = pipBoyLastHole;
    int& still = pipBoyStill;
    if (st.pipBoyHoleValid) {
        const float w = float(st.viewportW.load()), h = float(st.viewportH.load());
        float moved = 0;
        bool inside = true;
        for (int i = 0; i < 8; ++i) {
            moved = std::max(moved, std::fabs(st.pipBoyHole[i] - lastHole[i]));
            lastHole[i] = st.pipBoyHole[i];
            inside &= st.pipBoyHole[i] > 0 && st.pipBoyHole[i] < ((i & 1) ? h : w);
        }
        still = (inside && moved < 2.5f) ? still + 1 : 0;
    } else {
        still = 0;
    }
    if (pending && st.pipBoyHoleValid && still >= 10) {
        pending = false;
        setPage(true);
        sendPipBoyState(true);
    }
    if (!st.pipBoyMcPage) return;
    if (st.mcScreenOpen) pageSawScreen = true;
    // The Pip-Boy closed, or Minecraft's screen did (Esc, E): the page goes too.
    if (!st.pipBoyUp || (pageSawScreen && !st.mcScreenOpen)) setPage(false);
}
void consumeLook(float& dx, float& dy) { dx = lookDx; dy = lookDy; lookDx = lookDy = 0; }
void releaseAll() { if (state().bridge) state().bridge->pushInput(proto::kInReleaseAll, 0); }
bool uninstall() {
    if (!window) return true;
    if (GetWindowLongPtrW(window, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&windowProc)) {
        log::line("input: another window procedure was installed over ours; it stays");
        return false;
    }
    SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousProc));
    window = nullptr;
    return true;
}
void attachWindow(HWND hwnd) {
    if (window || !hwnd) return;
    window = hwnd;
    previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc)));
    log::line("input: text input from window %p", static_cast<void*>(hwnd));
}
} // namespace vegas::input
