#pragma once
#include "Bridge.h"
#include <cstdint>

// New Vegas items (MISC, WEAP, ARMO, AMMO, ALCH, BOOK, KEYM) as Minecraft sees them; the player's
// inventory itself is mirrored by Inventory.cpp (see dev.vegascraft.item.NvInventory).
namespace vegas::items {
struct Info {
    std::uint32_t formId = 0;
    unsigned kind = 0;           // proto::ItemKind
    unsigned weaponClass = 0;    // proto::WeaponClass
    float damage = 0;
    int value = 0, health = 0;
    float weight = 0;
    char name[37]{};
};
// The form types that can be taken.
bool pickable(unsigned formType);
// Reads a base form (TESObjectWEAP, TESObjectMISC, ...); false if it isn't an item.
bool describe(const void* base, Info& out);
// Weapon class from the model folder ("weapons\\1handpistol\\9mm.NIF" is a pistol).
unsigned classify(const char* modelPath);
// ExtraCount of a world reference (1 without one).
unsigned stackCount(const void* ref);
// Per frame while Minecraft drives the player: wields `heldForm` (the New Vegas weapon in Minecraft's main hand, 0 for none) on the player.
void perFrame(void* player, bool active, std::uint32_t heldForm, bool melee);
// True while the player holds a New Vegas weapon: New Vegas draws its own first-person arms and
// that weapon's model instead of Minecraft's hand.
bool wielding();
// The wielded weapon is a melee one: Minecraft swings it, New Vegas only shows its model.
bool wieldingMelee();
void clear();
// Hooks New Vegas' weapon firing (main thread, inside the game): the player's shots go to
// Minecraft as kInNvShot.
void installFireHook();
}
