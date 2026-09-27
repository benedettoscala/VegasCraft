#pragma once
#include "Hook.h"
namespace vegas::sceneutil {
inline void retain(void* p) { if (p) InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<unsigned char*>(p) + 4)); }
inline void release(void* p) {
    if (p && !InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<unsigned char*>(p) + 4))) {
        auto** vt = hook::field<void**>(p, 0);
        reinterpret_cast<void(__thiscall*)(void*)>(vt[1])(p);
    }
}
inline void* allocate(std::size_t size) { return reinterpret_cast<void*(__cdecl*)(std::size_t)>(0xAA1070)(size); }
}
