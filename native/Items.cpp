#include "Items.h"
#include "Game.h"
#include "Guard.h"
#include "Log.h"
#include "Probe.h"
#include "Rtti.h"
#include "Runtime.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace vegas::items {
namespace {
// TESForm type ids (xNVSE kFormType_*).
constexpr unsigned kArmor = 0x18, kBook = 0x1A, kMisc = 0x1F, kWeapon = 0x28, kAmmo = 0x29, kKey = 0x2E, kAid = 0x2F;
constexpr std::size_t kRefExtra = 0x44, kFullName = 0x34, kModelPath = 0x40;
constexpr std::size_t kWeaponValue = 0x88, kWeaponWeight = 0x90, kWeaponHealth = 0x98, kWeaponDamage = 0x138;
// TESValueForm / TESWeightForm / TESHealthForm payloads of the other item types (found by dumping live forms).
constexpr std::size_t kArmorValue = 0x5C, kArmorWeight = 0x64, kArmorHealth = 0x6C;
constexpr std::size_t kMiscValue = 0x78, kMiscWeight = 0x80;  // keys are misc objects too
constexpr std::size_t kAmmoValue = 0x7C;
constexpr std::size_t kAidWeight = 0x98, kAidValue = 0xB8;
constexpr unsigned kExtraCount = 0x24;

std::uint32_t wielded = 0;  // the New Vegas weapon equipped and drawn for Minecraft's main hand
bool wieldedMelee = false;

bool readable(const void* p, std::size_t n) { return hook::readable(p, n); }
unsigned kindOf(unsigned type) {
    switch (type) {
    case kWeapon: return proto::kItemWeapon;
    case kArmor: return proto::kItemArmor;
    case kAmmo: return proto::kItemAmmo;
    case kAid: return proto::kItemAid;
    case kBook: return proto::kItemBook;
    case kKey: return proto::kItemKey;
    default: return proto::kItemMisc;
    }
}
}

bool pickable(unsigned type) { return type == kArmor || type == kBook || type == kMisc || type == kWeapon || type == kAmmo || type == kKey || type == kAid; }

unsigned classify(const char* path) {
    if (!path) return proto::kWeaponClassUnknown;
    std::string s;
    for (std::size_t i = 0; i < 160 && readable(path + i, 1) && path[i]; ++i) s += char(std::tolower(static_cast<unsigned char>(path[i])));
    auto has = [&](const char* word) { return s.find(word) != std::string::npos; };
    if (has("1handpistol")) return proto::kWeaponClassPistol;
    if (has("2handautomatic")) return proto::kWeaponClassAutomatic;
    if (has("2handrifle")) return proto::kWeaponClassRifle;
    if (has("2handlauncher")) return proto::kWeaponClassLauncher;
    if (has("grenade") || has("1handmine") || has("lunchbox") || has("thrown")) return proto::kWeaponClassThrown;
    if (has("2handmelee") || has("2handhandle")) return proto::kWeaponClassMelee2H;
    if (has("1handmelee")) return proto::kWeaponClassMelee1H;
    if (has("unarmed") || has("handtohand")) return proto::kWeaponClassUnarmed;
    return proto::kWeaponClassUnknown;
}

bool describe(const void* base, Info& out) {
    if (!base || !readable(base, 0x40)) return false;
    const unsigned type = hook::field<std::uint8_t>(base, 0x04);
    if (!pickable(type)) return false;
    out = {};
    out.formId = hook::field<std::uint32_t>(base, 0x0C);
    out.kind = kindOf(type);
    const char* name = hook::field<const char*>(base, kFullName);
    if (name && readable(name, 2))
        for (std::size_t i = 0; i < sizeof(out.name) - 1 && readable(name + i, 1) && name[i] >= 32 && name[i] < 127; ++i) out.name[i] = name[i];
    if (!out.name[0]) std::snprintf(out.name, sizeof(out.name), "Item %08X", out.formId);
    if (type == kWeapon && readable(base, kWeaponDamage + 4)) {
        const char* model = hook::field<const char*>(base, kModelPath);
        out.weaponClass = classify(model);
        const float damage = hook::field<float>(base, kWeaponDamage);
        out.damage = std::isfinite(damage) && damage >= 0 && damage < 2000 ? damage : 0;
        out.value = hook::field<int>(base, kWeaponValue);
        out.weight = hook::field<float>(base, kWeaponWeight);
        out.health = hook::field<int>(base, kWeaponHealth);
    }
    auto sane = [](float w) { return std::isfinite(w) && w >= 0 && w < 10000 ? w : 0.0f; };
    auto positive = [](int v) { return v >= 0 && v < 10000000 ? v : 0; };
    if (type == kArmor && readable(base, kArmorHealth + 4)) {
        out.value = positive(hook::field<int>(base, kArmorValue));
        out.weight = sane(hook::field<float>(base, kArmorWeight));
        out.health = hook::field<int>(base, kArmorHealth);
    } else if ((type == kMisc || type == kKey) && readable(base, kMiscWeight + 4)) {
        out.value = positive(hook::field<int>(base, kMiscValue));
        out.weight = sane(hook::field<float>(base, kMiscWeight));
    } else if (type == kAmmo && readable(base, kAmmoValue + 4)) {
        out.value = positive(hook::field<int>(base, kAmmoValue));
    } else if (type == kAid && readable(base, kAidValue + 4)) {
        out.value = positive(hook::field<int>(base, kAidValue));
        out.weight = sane(hook::field<float>(base, kAidWeight));
    }
    return true;
}

unsigned stackCount(const void* ref) {
    if (!ref || !readable(ref, kRefExtra + 8)) return 1;
    // BaseExtraList: {vtable, first BSExtraData*, presence bitfield}; a node is {vtable, u8 type, next}.
    const void* node = hook::field<const void*>(static_cast<const unsigned char*>(ref) + kRefExtra, 4);
    for (int i = 0; node && i < 64 && readable(node, 0x10); ++i, node = hook::field<const void*>(node, 8))
        if (hook::field<std::uint8_t>(node, 4) == kExtraCount) {
            const unsigned n = hook::field<std::uint16_t>(node, 0x0C);
            return n ? n : 1;
        }
    return 1;
}

void clear() { wielded = 0; state().nvWeapon = false; }

// TESObjectWEAP::Fire(Actor*) (thiscall, ret 4): New Vegas firing a weapon, once per shot (each
// projectile of a shotgun's spread is spawned inside it). Its callers are redirected here; a shot
// of the player's while Minecraft drives is traced by Minecraft too, through its own mobs and
// blocks, which New Vegas' projectiles never meet (kInNvShot).
namespace {
constexpr std::uintptr_t kWeaponFire = 0x523150;
constexpr std::uintptr_t kWeaponFireSites[] = {0x5DA5F8, 0x87BA71, 0x8BADE9};
constexpr std::size_t kWeaponProjectiles = 0x11E;  // DNAM: projectiles per shot (u8)
void playerFired(void* weapon) {
    Info info;
    if (!state().puppeting || !state().bridge || !describe(weapon, info) || info.kind != proto::kItemWeapon) return;
    unsigned projectiles = readable(weapon, kWeaponProjectiles + 1) ? hook::field<std::uint8_t>(weapon, kWeaponProjectiles) : 1;
    if (projectiles < 1 || projectiles > 30) projectiles = 1;
    state().bridge->pushInput(proto::kInNvShot, static_cast<std::uint16_t>(info.weaponClass), static_cast<std::int32_t>(info.damage * 100.0f),
        static_cast<std::int32_t>(info.formId), static_cast<std::int32_t>(projectiles));
    log::once("nvshot-first", "items: first shot of %s (%08X): damage %.1f x%u projectiles", info.name, info.formId, info.damage, projectiles);
}
void __fastcall weaponFire(void* weapon, void*, void* actor) {
    if (actor && actor == game::player()) guard::run([&] { playerFired(weapon); });
    reinterpret_cast<void(__thiscall*)(void*, void*)>(kWeaponFire)(weapon, actor);
}
}
void installFireHook() {
    unsigned hooked = 0;
    for (auto site : kWeaponFireSites) hooked += hook::redirectCall(site, kWeaponFire, reinterpret_cast<const void*>(&weaponFire));
    log::line("items: weapon fire hooked at %u of %u call sites", hooked, unsigned(std::size(kWeaponFireSites)));
}

bool wielding() { return wielded != 0; }
bool wieldingMelee() { return wielded != 0 && wieldedMelee; }

// The player took the weapon in New Vegas, so it is in their inventory: equipping and drawing it
// makes New Vegas render its real model (and first-person arms) in place of Minecraft's item.
void wield(std::uint32_t formId, bool melee) {
    if (formId == wielded) { wieldedMelee = melee; state().nvWeapon = wielded && !melee; return; }
    char line[64];
    if (wielded) {
        std::snprintf(line, sizeof(line), "player.unequipitem %08X 0 1", wielded);
        probe::console(line);
        wielded = 0;
        state().nvWeapon = false;
    }
    void* form = formId ? game::lookupForm(formId) : nullptr;
    if (!form || !readable(form, 0x10) || hook::field<std::uint8_t>(form, 0x04) != kWeapon) return;
    std::snprintf(line, sizeof(line), "player.equipitem %08X 0 1", formId);
    if (!probe::console(line)) return;
    probe::console("player.setalert 1");
    wielded = formId;
    wieldedMelee = melee;
    state().nvWeapon = !melee;  // a melee weapon's buttons stay Minecraft's
    log::line("items: wielding %08X", formId);
}

// What the player takes reaches Minecraft through the inventory snapshots (Inventory.cpp).
void perFrame(void* player, bool active, std::uint32_t heldForm, bool melee) {
    if (!active || !player || !state().bridge) return;
    wield(heldForm, melee);
}
}
