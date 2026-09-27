#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

// Minimal, verified code patching for the 32-bit New Vegas executable. Every patch checks the
// bytes it replaces first, so a different executable (or the test host) is left untouched.
namespace vegas::hook {
// The running executable is FalloutNV.exe (the plugin's Query already pinned 1.4.0.525).
inline bool inGameProcess() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return name && _wcsicmp(name + 1, L"FalloutNV.exe") == 0;
}
// A cheap "could be a pointer" check for per-frame walks of the engine's scene graph: VirtualQuery costs ~0.1 ms in this process' fragmented address space.
// Only for code that runs inside guard::run, which catches the access violation a stale pointer would give.
inline bool plausible(const void* address, std::size_t = 0) { return reinterpret_cast<std::uintptr_t>(address) > 0x10000; }
inline bool readable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto end = static_cast<const unsigned char*>(info.BaseAddress) + info.RegionSize;
    return static_cast<const unsigned char*>(address) + bytes <= end;
}
inline bool rawWrite(void* address, const void* bytes, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(address, bytes, size);
    VirtualProtect(address, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}
// Every patch this module made, so it can take them all back before it is unloaded (hot reload).
struct Patch { std::uintptr_t address; std::size_t size; std::uint8_t original[8]; std::uint8_t patched[8]; };
inline std::vector<Patch>& journal() { static std::vector<Patch> patches; return patches; }
inline bool write(void* address, const void* bytes, std::size_t size) {
    Patch patch{reinterpret_cast<std::uintptr_t>(address), size, {}, {}};
    if (size <= sizeof(patch.original)) std::memcpy(patch.original, address, size);
    if (!rawWrite(address, bytes, size)) return false;
    if (size <= sizeof(patch.patched)) { std::memcpy(patch.patched, bytes, size); journal().push_back(patch); }
    return true;
}
// Restores every patch, newest first. Changes nothing (false) if some patch was itself patched
// over since: the code that did it would then jump into an unloaded module.
inline bool restoreAll() {
    auto& patches = journal();
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const auto& p = patches[i];
        bool newest = true;  // only the last patch of an address is still in place
        for (std::size_t j = i + 1; j < patches.size() && newest; ++j) newest = patches[j].address != p.address;
        if (newest && (!readable(reinterpret_cast<void*>(p.address), p.size) || std::memcmp(reinterpret_cast<void*>(p.address), p.patched, p.size) != 0)) return false;
    }
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) rawWrite(reinterpret_cast<void*>(it->address), it->original, it->size);
    patches.clear();
    return true;
}
inline std::uintptr_t callTarget(std::uintptr_t site) {
    if (!readable(reinterpret_cast<void*>(site), 5) || *reinterpret_cast<const std::uint8_t*>(site) != 0xE8) return 0;
    std::int32_t rel;
    std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), 4);
    return site + 5 + rel;
}
// Redirects `call expected` at `site` to `fn`. Returns false (and changes nothing) otherwise.
inline bool redirectCall(std::uintptr_t site, std::uintptr_t expected, const void* fn) {
    if (callTarget(site) != expected) return false;
    const std::int32_t rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(fn) - (site + 5));
    return write(reinterpret_cast<void*>(site + 1), &rel, 4);
}
// Replaces a vtable slot; returns the previous function (null on failure).
inline void* replaceSlot(void** slot, void* fn) {
    if (!readable(slot, sizeof(void*))) return nullptr;
    void* previous = *slot;
    if (previous == fn) return nullptr;
    return write(slot, &fn, sizeof(fn)) ? previous : nullptr;
}
template<class T> T field(const void* object, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(T));
    return value;
}
template<class T> void setField(void* object, std::size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(T));
}
// Bounded read of possibly stale engine memory.
template<class T> bool safeRead(const void* address, T& value) {
    SIZE_T got = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof(T), &got) && got == sizeof(T);
}
} // namespace vegas::hook
