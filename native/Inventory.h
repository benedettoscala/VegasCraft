#pragma once
#include <cstdint>

// The New Vegas player's inventory, which New Vegas owns: Minecraft mirrors it (see
// dev.vegascraft.item.NvInventory). Snapshots go through the collision ring as kColInventory.
namespace vegas::inventory {
// Main thread, every frame in game: reads the inventory a few times a second and sends it when it
// changed (and every few seconds anyway, should a snapshot be lost).
void perFrame(void* player, std::uint32_t worldId);
// The next perFrame sends a snapshot even if nothing changed (Minecraft reconnected, a load).
void resend();
// Logs the last snapshot (probe inv).
void report();
// Minecraft used a linked item (aid): New Vegas uses one, as from its own Pip-Boy.
void use(void* player, std::uint32_t formId);
// The Pip-Boy's New Vegas list: wear or take off an item.
void equip(void* player, std::uint32_t formId, bool on);
// Gives an item one of New Vegas' hotkeys (1-8), as the Pip-Boy's own list does.
void setHotkey(void* player, std::uint32_t formId, int key);
// Minecraft threw `count` of `formId` away: New Vegas drops them in its world.
void drop(void* player, std::uint32_t formId, int count);
} // namespace vegas::inventory
