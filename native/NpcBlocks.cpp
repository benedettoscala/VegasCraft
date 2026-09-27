#include "NpcBlocks.h"
#include "Actors.h"
#include "Bridge.h"
#include "Guard.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

// Port of SkyCraft's NpcBlocks.cpp (MIT): Minecraft blocks are walls for New Vegas' NPCs too.
namespace vegas::npcblocks {
namespace {
std::unordered_map<std::uint64_t, std::array<std::uint64_t, 64>> solids;  // section -> bit x + 16z + 256y
constexpr float kRange = 4000.0f;          // units from the player
constexpr double kMaxRadiusBlocks = 0.45;  // wider actors still fit through a one-block gap
constexpr double kTouchBlocks = 0.1;
std::uint64_t key(std::int32_t x, std::int32_t y, std::int32_t z) {
    return (std::uint64_t(std::uint32_t(x) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(z) & 0x3FFFFF) << 20) | (std::uint64_t(std::uint32_t(y) & 0xFFFFF));
}
}
void onSolids(const std::uint8_t* data, std::uint32_t bytes) {
    if (bytes < sizeof(proto::RenSolids)) return;
    const auto* hdr = reinterpret_cast<const proto::RenSolids*>(data);
    const auto k = key(hdr->sx, hdr->sy, hdr->sz);
    if (hdr->count == 0 || bytes < sizeof(proto::RenSolids) + 512) { solids.erase(k); return; }
    std::memcpy(solids[k].data(), data + sizeof(proto::RenSolids), 512);
}
void clear() { solids.clear(); }
bool solidAt(std::int32_t x, std::int32_t y, std::int32_t z) {
    const auto it = solids.find(key(x >> 4, y >> 4, z >> 4));
    if (it == solids.end()) return false;
    const int bit = (x & 15) + 16 * (z & 15) + 256 * (y & 15);
    return (it->second[bit >> 6] >> (bit & 63)) & 1;
}
void pushActorsOut(void* player) {
    if (solids.empty() || !player) return;
    const auto here = game::position(player);
    actors::forEachHighActor([&](void* actor) {
        if (actor == player || actors::disabled(actor) || actors::dead(actor)) return;
        const auto pos = game::position(actor);
        const float dx = pos.x - here.x, dy = pos.y - here.y;
        if (dx * dx + dy * dy > kRange * kRange) return;
        const float radius = actors::boundRadius(actor);
        if (radius <= 0) return;
        const auto mc = toMinecraft({pos.x, pos.y, pos.z}, proto::kUnitsPerBlock);
        const double r = std::clamp(radius * 0.5 / proto::kUnitsPerBlock, 0.2, kMaxRadiusBlocks) + kTouchBlocks;
        const double h = std::clamp(radius * 1.75 / proto::kUnitsPerBlock, 0.5, 4.0);
        const int y0 = static_cast<int>(std::floor(mc.y + 0.3)), y1 = static_cast<int>(std::floor(mc.y + h - 0.1));
        double cx = mc.x, cz = mc.z;
        bool moved = false;
        for (int pass = 0; pass < 3; ++pass) {
            double pushX = 0, pushZ = 0;
            for (int bx = static_cast<int>(std::floor(cx - r)); bx <= static_cast<int>(std::floor(cx + r)); ++bx)
                for (int bz = static_cast<int>(std::floor(cz - r)); bz <= static_cast<int>(std::floor(cz + r)); ++bz) {
                    bool hit = false;
                    for (int by = y0; by <= y1 && !hit; ++by) hit = solidAt(bx, by, bz);
                    if (!hit) continue;
                    const double qx = std::clamp(cx, double(bx), double(bx + 1)), qz = std::clamp(cz, double(bz), double(bz + 1));
                    double ex = cx - qx, ez = cz - qz;
                    const double d = std::sqrt(ex * ex + ez * ez);
                    if (d >= r) continue;
                    if (d < 1e-6) {
                        const double toW = cx - bx, toE = bx + 1 - cx, toN = cz - bz, toS = bz + 1 - cz;
                        const double m = std::min({toW, toE, toN, toS});
                        ex = m == toW ? -1.0 : m == toE ? 1.0 : 0.0;
                        ez = m == toN ? -1.0 : m == toS ? 1.0 : 0.0;
                        pushX += ex * (m + r);
                        pushZ += ez * (m + r);
                    } else {
                        pushX += ex / d * (r - d);
                        pushZ += ez / d * (r - d);
                    }
                }
            if (std::abs(pushX) < 1e-4 && std::abs(pushZ) < 1e-4) break;
            cx += pushX;
            cz += pushZ;
            moved = true;
        }
        if (moved) game::movePlayer(actor, {static_cast<float>(cx * proto::kUnitsPerBlock), static_cast<float>(-cz * proto::kUnitsPerBlock), pos.z});
    });
}
} // namespace vegas::npcblocks
