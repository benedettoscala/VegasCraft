#include "Rtti.h"
#include <unordered_map>

namespace vegas::rtti {
const char* fastName(const void* object) {
    static std::unordered_map<const void*, const char*> cache;
    const void* vtable = *static_cast<const void* const*>(object);
    auto it = cache.find(vtable);
    if (it != cache.end()) return it->second;
    const char* n = name(object);
    cache.emplace(vtable, n);
    return n;
}
} // namespace vegas::rtti
