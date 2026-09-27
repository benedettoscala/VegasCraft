#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace vegas::formicons {
// Form components contain integers, vtables and other pointers as well as strings. Unlike a
// known engine link, a speculative string candidate must not abort the whole snapshot on a fault.
// These lookups are cached per form by the callers; never query memory once per character.
inline std::size_t readableBytes(const void* address, std::size_t limit) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start < end ? std::min(limit, static_cast<std::size_t>(end - start)) : 0;
}

inline std::string path(const void* form, std::size_t first = 0x18, std::size_t maxPath = 94) {
    std::array<unsigned char, 0x200> fields{};
    const auto bytes = readableBytes(form, fields.size());
    SIZE_T copied = 0;
    if (!bytes || !ReadProcessMemory(GetCurrentProcess(), form, fields.data(), bytes, &copied)) return {};
    std::string best;
    for (std::size_t off = first; off + sizeof(const char*) <= copied; off += 4) {
        const char* candidate = nullptr;
        std::memcpy(&candidate, fields.data() + off, sizeof(candidate));
        if (reinterpret_cast<std::uintptr_t>(candidate) < 0x10000) continue;
        std::array<char, 120> text{};
        const auto span = readableBytes(candidate, text.size());
        SIZE_T read = 0;
        if (!span || !ReadProcessMemory(GetCurrentProcess(), candidate, text.data(), span, &read)) continue;
        std::string value;
        bool terminated = false;
        for (std::size_t i = 0; i < read; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (!c) { terminated = true; break; }
            if (c < 32 || c > 126) break;
            value += static_cast<char>(std::tolower(c));
        }
        if (!terminated || value.size() < 8 || value.size() > maxPath ||
            value.compare(value.size() - 4, 4, ".dds") != 0 || value.find("icons") == std::string::npos) continue;
        if (value.find("pipboyimages") != std::string::npos) return value;
        if (best.empty()) best = std::move(value);
    }
    return best;
}
} // namespace vegas::formicons
