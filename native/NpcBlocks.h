#pragma once
#include <cstdint>

namespace vegas::npcblocks {
// Render ring messages: a section's solid Minecraft blocks (kRenSolids); a world change.
void onSolids(const std::uint8_t* data, std::uint32_t bytes);
void clear();
bool solidAt(std::int32_t x, std::int32_t y, std::int32_t z);
// Main thread, every frame: New Vegas actors overlapping solid Minecraft blocks are pushed out.
void pushActorsOut(void* player);
} // namespace vegas::npcblocks
