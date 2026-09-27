#pragma once
#include "Clip.h"
#include <cstdint>
#include <vector>

// Digging into New Vegas' world (SkyCraft's Dig.h, MIT). Minecraft decides what's dug and sends
// each section's dug cells (proto::kRenDug); in a dug cell New Vegas' own ground is gone: not in
// the collision Minecraft sees (Collision.cpp) and not drawn (DigMesh.cpp).
namespace vegas::dig {
// Render ring message (main thread): a section's dug cells.
void onDug(const std::uint8_t* data, std::uint32_t bytes);
void clear();
// New Vegas' current world (SkyState::worldId); cells dug in another world are dropped.
void setWorld(std::uint32_t worldId);
bool any();
bool isDug(std::int32_t x, std::int32_t y, std::int32_t z);
// Dug cells whose cube overlaps [lo, hi] (Minecraft coords). Any thread.
void collect(const float lo[3], const float hi[3], std::vector<clip::Cube>& out);
// Cells dug (or filled again) since the last call. Main thread.
void takeChanged(std::vector<clip::Cube>& out);
// Changes whenever the dug cells do. Any thread.
std::uint64_t generation();
// The Minecraft block (proto::DigMaterial) the land at a New Vegas point digs into, from the
// texture painted there. Main thread.
std::uint8_t landMaterial(float x, float y);
// The Havok material type (0-31) of that texture: 0 stone, 2 dirt, 4 grass, 14 snow, 18 sand,
// 19 broken concrete...; kNoHavokMaterial where no land texture is loaded. Main thread.
inline constexpr std::uint8_t kNoHavokMaterial = 0xFF;
std::uint8_t landHavokMaterial(float x, float y);
} // namespace vegas::dig
