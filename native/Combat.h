#pragma once

namespace vegas::combat {
// Main thread, once per frame: nearby actors out to Minecraft, Minecraft's hits in, damage to
// the New Vegas player bridged to Minecraft (which owns the player's health).
void perFrame(void* player, bool puppeting, float delta);
// Main thread, in the Pip-Boy's menu (perFrame doesn't run there): the page's events.
void pipBoyEvents(void* player);
// Someone nearby is fighting the player.
bool playerEngaged();
} // namespace vegas::combat
