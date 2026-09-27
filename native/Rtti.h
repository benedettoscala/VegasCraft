#pragma once
#include "Hook.h"
#include <cstdint>
#include <cstring>

// MSVC RTTI of FalloutNV.exe objects (the executable ships it, including Havok's classes).
// Lets the plugin identify engine objects by class name and find base-class subobjects.
namespace vegas::rtti {
struct TypeDescriptor { const void* vftable; void* spare; char name[1]; };
struct BaseClassDescriptor {
    const TypeDescriptor* type;
    std::uint32_t numContained;
    std::int32_t mdisp, pdisp, vdisp;
    std::uint32_t attributes;
};
struct ClassHierarchy {
    std::uint32_t signature, attributes, numBases;
    const BaseClassDescriptor* const* bases;
};
struct CompleteObjectLocator {
    std::uint32_t signature, offset, cdOffset;
    const TypeDescriptor* type;
    const ClassHierarchy* hierarchy;
};
inline const CompleteObjectLocator* locator(const void* object) {
    const void* const* vtable = nullptr;
    if (!hook::safeRead(object, vtable) || !vtable) return nullptr;
    const CompleteObjectLocator* col = nullptr;
    if (!hook::safeRead(vtable - 1, col) || !col || !hook::readable(col, sizeof(*col))) return nullptr;
    if (col->signature != 0 || !hook::readable(col->type, 16) || !hook::readable(col->hierarchy, sizeof(ClassHierarchy))) return nullptr;
    return col;
}
// Mangled class name such as ".?AVhkpBoxShape@@", or "" if the object has no RTTI.
inline const char* name(const void* object) {
    auto* col = locator(object);
    return col ? col->type->name : "";
}
inline bool is(const void* object, const char* mangled) { return std::strcmp(name(object), mangled) == 0; }
// Same as name(), cached per vtable, with plain reads: only call inside guard::run on objects
// that are known to be live (Havok shapes during a walk). Main thread only.
const char* fastName(const void* object);
// Offset of `mangled` base inside the complete object (non-virtual bases), or -1.
inline std::int32_t baseOffset(const void* object, const char* mangled) {
    auto* col = locator(object);
    if (!col) return -1;
    auto* h = col->hierarchy;
    if (h->numBases > 64 || !hook::readable(h->bases, h->numBases * sizeof(void*))) return -1;
    for (std::uint32_t i = 0; i < h->numBases; ++i) {
        auto* base = h->bases[i];
        if (!hook::readable(base, sizeof(*base)) || !hook::readable(base->type, 16)) continue;
        if (std::strcmp(base->type->name, mangled) == 0 && base->pdisp == -1)
            return base->mdisp - static_cast<std::int32_t>(col->offset);
    }
    return -1;
}
template<class T = void> T* cast(void* object, const char* mangled) {
    auto offset = baseOffset(object, mangled);
    return offset < 0 ? nullptr : reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}
} // namespace vegas::rtti
