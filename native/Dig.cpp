#include "Dig.h"
#include "Bridge.h"
#include "Game.h"
#include "Log.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <bit>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace vegas::dig {
namespace {
std::shared_mutex lock;
std::unordered_map<std::uint64_t, std::array<std::uint64_t, 64>> sections;  // bit x + 16z + 256y
std::uint32_t world = 0;
std::atomic<std::uint64_t> gen{0};
std::vector<clip::Cube> changed;
std::unordered_map<std::uint64_t, std::uint8_t> materials;  // 2-block land grid -> material

std::uint64_t key(std::int32_t x, std::int32_t y, std::int32_t z) {
    return (std::uint64_t(std::uint32_t(x) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(z) & 0x3FFFFF) << 20) | (std::uint64_t(std::uint32_t(y) & 0xFFFFF));
}
bool bit(const std::array<std::uint64_t, 64>& b, int i) { return (b[i >> 6] >> (i & 63)) & 1; }
}
void onDug(const std::uint8_t* data, std::uint32_t bytes) {
    if (bytes < sizeof(proto::RenDug)) return;
    const auto* hdr = reinterpret_cast<const proto::RenDug*>(data);
    if (hdr->count > 4096 || (hdr->count && bytes < sizeof(proto::RenDug) + 512)) return;
    // Bound section coordinates before multiplying by 16; malformed packets must not alias
    // another section or overflow a signed block coordinate.
    if (hdr->sx < -2097152 || hdr->sx > 2097151 || hdr->sz < -2097152 || hdr->sz > 2097151 ||
        hdr->sy < -524288 || hdr->sy > 524287) return;
    std::array<std::uint64_t, 64> bits{};
    if (hdr->count && bytes >= sizeof(proto::RenDug) + 512) std::memcpy(bits.data(), data + sizeof(proto::RenDug), 512);
    unsigned count = 0;
    for (auto word : bits) count += std::popcount(word);
    if (count != hdr->count) return;
    std::unique_lock guard(lock);
    if (hdr->worldId != world) return;  // another world's cells (sent before the switch)
    const auto k = key(hdr->sx, hdr->sy, hdr->sz);
    std::array<std::uint64_t, 64> before{};
    if (auto it = sections.find(k); it != sections.end()) before = it->second;
    if (before == bits) return;
    for (int i = 0; i < 4096; ++i)
        if (bit(before, i) != bit(bits, i))
            changed.push_back({hdr->sx * 16 + (i & 15), hdr->sy * 16 + (i >> 8), hdr->sz * 16 + ((i >> 4) & 15)});
    if (hdr->count) sections[k] = bits;
    else sections.erase(k);
    ++gen;
}
void clear() {
    std::unique_lock guard(lock);
    // Removing all holes (atlas resend/disconnect) also restores cached collision regions.
    for (const auto& [k, bits] : sections) {
        const auto sx = std::int32_t(std::uint32_t(k >> 42) << 10) >> 10;
        const auto sz = std::int32_t(std::uint32_t(k >> 20) << 10) >> 10;
        const auto sy = std::int32_t(std::uint32_t(k) << 12) >> 12;
        for (int i = 0; i < 4096; ++i) if (bit(bits, i))
            changed.push_back({sx * 16 + (i & 15), sy * 16 + (i >> 8), sz * 16 + ((i >> 4) & 15)});
    }
    sections.clear();
    materials.clear();
    ++gen;
}
void setWorld(std::uint32_t id) {
    std::unique_lock guard(lock);
    if (id == world) return;
    world = id;
    sections.clear();
    changed.clear();
    materials.clear();
    ++gen;
}
bool any() {
    std::shared_lock guard(lock);
    return !sections.empty();
}
bool isDug(std::int32_t x, std::int32_t y, std::int32_t z) {
    std::shared_lock guard(lock);
    const auto it = sections.find(key(x >> 4, y >> 4, z >> 4));
    return it != sections.end() && bit(it->second, (x & 15) + 16 * (z & 15) + 256 * (y & 15));
}
void collect(const float lo[3], const float hi[3], std::vector<clip::Cube>& out) {
    std::shared_lock guard(lock);
    if (sections.empty()) return;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(lo[i]) || !std::isfinite(hi[i]) || lo[i] > hi[i] || lo[i] <= -33554432.0f || hi[i] >= 33554432.0f) return;
    const int x0 = static_cast<int>(std::floor(lo[0] - clip::kSlop)), y0 = static_cast<int>(std::floor(lo[1] - clip::kSlop)), z0 = static_cast<int>(std::floor(lo[2] - clip::kSlop));
    const int x1 = static_cast<int>(std::floor(hi[0] + clip::kSlop)), y1 = static_cast<int>(std::floor(hi[1] + clip::kSlop)), z1 = static_cast<int>(std::floor(hi[2] + clip::kSlop));
    if (static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 2'000'000) return;
    for (int sy = y0 >> 4; sy <= y1 >> 4; ++sy)
        for (int sz = z0 >> 4; sz <= z1 >> 4; ++sz)
            for (int sx = x0 >> 4; sx <= x1 >> 4; ++sx) {
                const auto it = sections.find(key(sx, sy, sz));
                if (it == sections.end()) continue;
                for (int i = 0; i < 4096; ++i) {
                    if (!bit(it->second, i)) continue;
                    const int x = sx * 16 + (i & 15), y = sy * 16 + (i >> 8), z = sz * 16 + ((i >> 4) & 15);
                    if (x >= x0 && x <= x1 && y >= y0 && y <= y1 && z >= z0 && z <= z1) out.push_back({x, y, z});
                }
            }
}
void takeChanged(std::vector<clip::Cube>& out) {
    std::unique_lock guard(lock);
    out.swap(changed);
    changed.clear();
}
std::uint64_t generation() { return gen.load(); }
std::uint8_t landHavokMaterial(float x, float y) {
    const auto k = (std::uint64_t(std::uint32_t(static_cast<int>(std::floor(x / 140.0f)))) << 32) | std::uint32_t(static_cast<int>(std::floor(y / 140.0f)));
    if (auto it = materials.find(k); it != materials.end()) return it->second;
    std::uint8_t out = kNoHavokMaterial;
    void* t = game::tes();
    if (t) {
        float h = 0;
        game::NiPoint3 p{x, y, 0};
        if (game::landHeight(p, h)) p.z = h;
        // TES::GetLandTexture -> TESLandTexture; its Havok material type is the byte at +0x1C.
        void* texture = reinterpret_cast<void*(__thiscall*)(void*, const game::NiPoint3*)>(0x457720)(t, &p);
        if (texture) out = hook::field<std::uint8_t>(texture, 0x1C) & 0x1F;
    }
    if (materials.size() > 200000) materials.clear();
    materials.emplace(k, out);
    return out;
}
std::uint8_t landMaterial(float x, float y) {
    switch (landHavokMaterial(x, y)) {
    case 0: case 10: case 19: return proto::kDigStone;  // stone, heavy stone, broken concrete
    case 4: return proto::kDigGrass;
    case 14: return proto::kDigSnow;
    case 18: return proto::kDigSand;
    case 2: default: return proto::kDigDirt;
    }
}
} // namespace vegas::dig
