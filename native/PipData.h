#pragma once
#include <cstdint>
#include <string>

// What the Pip-Boy's pages show, read from New Vegas (see PipDataKind in the protocol) and sent to
// Minecraft, which draws them (dev.vegascraft.client.pip.*).
namespace vegas::pipdata {
// Main thread, every frame in game: reads each kind at its own pace and sends what changed.
void perFrame(void* player, std::uint32_t worldId);
// Sends everything again (Minecraft reconnected, a load).
void resend();
// Probe `pip <kind>`: logs one kind's data as it is sent.
void report(unsigned kind);
// The Pip-Boy's menus: quests (set the tracked one), travel and the like come from Minecraft's pages.
void trackQuest(std::uint32_t formId);
void travelTo(std::uint32_t markerRef);
void setRadio(std::uint32_t stationRef, bool on);
// The inventory image of a form: the first of its components' strings that names a .dds under "icons".
std::string iconPath(const void* form);
} // namespace vegas::pipdata
