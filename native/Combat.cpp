#include "Input.h"
#include "Inventory.h"
#include "PipData.h"
#include "Combat.h"
#include "Actors.h"
#include "Game.h"
#include "Guard.h"
#include "Log.h"
#include "Rtti.h"
#include "Runtime.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// New Vegas port of SkyCraft's Combat.cpp (MIT). Minecraft computes every hit with its own rules
// (weapons, enchantments, crits, armour); New Vegas applies the result to its actors, and
// damage New Vegas deals to its player becomes Minecraft damage.
namespace vegas::combat {
namespace {
using namespace actors;
constexpr float kActorRange = 80.0f * 70.0f;      // stand-ins exist this far out (units)
constexpr float kHitMemorySeconds = 0.35f;
constexpr float kDotFlushSeconds = 0.5f;
constexpr std::uint32_t kHealth = 16;              // ActorValue::Health

// Engine (FalloutNV 1.4.0.525; see docs/RENDERING.md and THIRD-PARTY-NOTICES.md for sources).
constexpr std::uintptr_t kGetLevel = 0x87F9F0;         // Actor::GetLevel
constexpr std::uintptr_t kIsInCombat = 0x493BB0;       // Actor::IsInCombat
constexpr std::uintptr_t kGetActorBase = 0x4181E0;     // Actor::GetActorBase
constexpr std::uintptr_t kIsPlayerTeammate = 0x566950;
// Actor virtual functions (JohnnyGuitarNVSE's Actor.hpp order, anchored to xNVSE's indices).
constexpr int kVfDoDamage = 0xCE;            // (float health, float fatigue, Actor* source)
constexpr int kVfDamageModActorValue = 0xEB; // (index, float modifier, Actor* attacker)
constexpr int kVfStartCombat = 0x109;
constexpr int kVfGetCombatTarget = 0x10B;
constexpr int kVfHandleHealthDamage = 0x12E; // (Actor* attacker, float damage)
// ActorValueOwner: [3] GetActorValueF, [8] GetPermanentActorValueF.


struct Health { float current = 0, max = 0; bool ok = false; };
Health health(void* actor) {
    Health h;
    auto* avo = rtti::cast<void>(actor, ".?AVActorValueOwner@@");
    if (!avo) return h;
    h.current = vcall<float>(avo, 3, kHealth);
    h.max = vcall<float>(avo, 8, kHealth);
    h.ok = std::isfinite(h.current) && std::isfinite(h.max) && h.max > 0;
    return h;
}
void* lookup(std::uint32_t formId) { return game::lookupForm(formId); }
float distance(const game::NiPoint3& a, const game::NiPoint3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
void displayName(void* actor, char* out, std::size_t size) {
    out[0] = 0;
    void* base = call<void*>(kGetActorBase, actor);
    auto* full = base ? rtti::cast<unsigned char>(base, ".?AVTESFullName@@") : nullptr;
    const char* text = full ? hook::field<const char*>(full, 4) : nullptr;
    if (text && hook::readable(text, 1)) {
        std::strncpy(out, text, size - 1);
        out[size - 1] = 0;
    }
}


// ---- what hit the player ---------------------------------------------------------------------
std::uint32_t lastDamager = 0;
float lastDamagerAge = 99.0f;
using HandleDamageFn = void(__thiscall*)(void*, void*, float);
HandleDamageFn originalHandleDamage = nullptr;
void __fastcall handleHealthDamage(void* self, void*, void* attacker, float damage) {
    originalHandleDamage(self, attacker, damage);
    if (attacker && attacker != self) { lastDamager = game::formId(attacker); lastDamagerAge = 0.0f; }
}
struct PendingHurt { std::uint32_t attacker = 0; proto::HurtKind kind = proto::kHurtOther; float damage = 0, age = 0; } dot;
bool healthPrimed = false;
bool engaged = false;
float engagedTimer = 0;

void sendHurt(proto::HurtKind kind, float damage, std::uint32_t attacker) {
    if (damage <= 0.01f) return;
    state().bridge->pushInput(proto::kInHurt, static_cast<std::uint16_t>(kind), static_cast<std::int32_t>(damage * 100.0f), static_cast<std::int32_t>(attacker), 0);
    log::line("combat: player hit by %08X for %.1f New Vegas damage (%s)", attacker, damage,
        kind == proto::kHurtMelee ? "melee" : kind == proto::kHurtProjectile ? "projectile" : "other");
}
// Minecraft owns the player's health: New Vegas damage is refunded here and sent to Minecraft.
void bridgePlayerDamage(void* player, float delta) {
    const auto h = health(player);
    if (!h.ok) return;
    const float deficit = h.max - h.current;
    lastDamagerAge += delta;
    if (!healthPrimed) {
        // Damage the save had before Minecraft took over isn't a new hit.
        healthPrimed = true;
        if (deficit > 0) vcall<void>(player, kVfDamageModActorValue, kHealth, deficit, static_cast<void*>(nullptr));
        return;
    }
    if (deficit > 0.01f) {
        vcall<void>(player, kVfDamageModActorValue, kHealth, deficit, static_cast<void*>(nullptr));
        if (lastDamagerAge < kHitMemorySeconds && lastDamager) {
            proto::HurtKind kind = proto::kHurtMelee;
            if (void* attacker = lookup(lastDamager)) {
                if (distance(game::position(attacker), game::position(player)) > 4.0f * 70.0f) kind = proto::kHurtProjectile;
            }
            sendHurt(kind, deficit, lastDamager);
            lastDamagerAge = 99.0f;
        } else {
            dot.damage += deficit;  // radiation, falls, fire: batched under Minecraft's i-frames
        }
    }
    dot.age += delta;
    if (dot.age >= kDotFlushSeconds) {
        sendHurt(proto::kHurtOther, dot.damage, 0);
        dot = {};
    }
}

void writeActorTable(void* player) {
    static std::vector<proto::ActorRecord> records;
    records.clear();
    const auto here = game::position(player);
    forEachHighActor([&](void* actor) {
        if (actor == player || records.size() >= proto::kMaxActors || disabled(actor)) return;
        const float radius = boundRadius(actor);
        if (radius <= 0) return;  // no 3D
        const auto pos = game::position(actor);
        if (distance(pos, here) > kActorRange) return;
        proto::ActorRecord r{};
        r.formId = game::formId(actor);
        const bool isDead = dead(actor);
        void* target = isDead ? nullptr : vcall<void*>(actor, kVfGetCombatTarget);
        r.flags = (target == player ? proto::kActorHostile : 0u) | (isDead ? proto::kActorDead : 0u) |
            (call<bool>(kIsInCombat, actor) ? proto::kActorInCombat : 0u);
        const auto mc = toMinecraft({pos.x, pos.y, pos.z}, proto::kUnitsPerBlock);
        r.x = static_cast<float>(mc.x); r.y = static_cast<float>(mc.y); r.z = static_cast<float>(mc.z);
        r.yaw = headingToMcYaw(game::rotation(actor).z);
        r.height = std::clamp(radius * 1.75f / 70.0f, 0.3f, 12.0f);
        r.width = std::clamp(radius * 0.8f / 70.0f, 0.3f, 6.0f);
        const auto h = health(actor);
        r.healthFrac = h.ok ? std::clamp(h.current / h.max, 0.0f, 1.0f) : 0.0f;
        r.level = call<std::uint16_t>(kGetLevel, actor);
        displayName(actor, r.name, sizeof(r.name));
        records.push_back(r);
        if (!isDead && target == player) engaged = true;
    });
    state().bridge->writeActors(records.data(), static_cast<std::uint32_t>(records.size()));
}

// Minecraft's knockback (LivingEntity.knockback / travel): a push of `strength` blocks per tick away
// from the attacker plus a hop, slowed by ground friction (0.6 a tick) and gravity (0.08 a tick).
struct Knock { std::uint32_t formId; float vx, vy, vz; float z0, left; };
std::vector<Knock> knocks;
constexpr float kTick = 0.05f;
void startKnockback(std::uint32_t formId, void* actor, float dirX, float dirZ, float strength) {
    if (strength <= 0) return;
    const float len = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (len < 1e-4f) return;
    // Minecraft (x, z) -> New Vegas (x, -y); blocks per tick -> units per second.
    const float k = float(proto::kUnitsPerBlock) / kTick * strength / len;
    knocks.erase(std::remove_if(knocks.begin(), knocks.end(), [&](const Knock& n) { return n.formId == formId; }), knocks.end());
    knocks.push_back({formId, dirX * k, -dirZ * k, std::min(0.4f, strength) * float(proto::kUnitsPerBlock) / kTick, game::position(actor).z, 0.5f});
}
void stepKnockbacks(float delta) {
    if (knocks.empty() || delta <= 0) return;
    const float dt = std::min(delta, 0.1f);
    const float friction = std::pow(0.6f, dt / kTick);
    const float gravity = 0.08f * float(proto::kUnitsPerBlock) / (kTick * kTick);
    for (auto it = knocks.begin(); it != knocks.end();) {
        void* actor = lookup(it->formId);
        it->left -= dt;
        if (!actor || !isActor(actor) || it->left <= 0) { it = knocks.erase(it); continue; }
        auto p = game::position(actor);
        p.x += it->vx * dt;
        p.y += it->vy * dt;
        p.z = std::max(it->z0, p.z + it->vz * dt);
        it->vz -= gravity * dt;
        it->vx *= friction;
        it->vy *= friction;
        game::movePlayer(actor, p);
        ++it;
    }
}
// Minecraft's hurt flash: the struck actor glows red for hurtTime (10 ticks). NiMaterialProperty
// (geometry +0xA4) emissive colour at +0x28 and its multiplier at +0x40 (verified live 2026-10-02:
// black and 1 on NPC bodies); the originals come back when the flash ends.
struct Flash { std::uint32_t formId; float left; };
std::vector<Flash> flashes;
struct SavedMaterial { void* material; float emissive[3]; float mult; };
std::vector<SavedMaterial> savedMaterials;
constexpr float kFlashSeconds = 0.5f;
constexpr std::size_t kGeomMaterial = 0xA4, kMatEmissive = 0x28, kMatEmitMult = 0x40;
void tintGeometry(void* obj, float strength, int depth) {
    if (!obj || depth > 40 || !hook::plausible(obj, 0xC0)) return;
    auto** vt = hook::field<void**>(obj, 0);
    if (reinterpret_cast<void*(__thiscall*)(void*)>(vt[6])(obj)) {  // geometry
        void* m = hook::field<void*>(obj, kGeomMaterial);
        if (!m || !hook::plausible(m, 0x48) || std::strcmp(rtti::name(m), ".?AVNiMaterialProperty@@") != 0) return;
        auto it = std::find_if(savedMaterials.begin(), savedMaterials.end(), [&](const SavedMaterial& s) { return s.material == m; });
        if (it == savedMaterials.end()) {
            SavedMaterial s{m, {}, hook::field<float>(m, kMatEmitMult)};
            std::memcpy(s.emissive, static_cast<char*>(m) + kMatEmissive, sizeof(s.emissive));
            savedMaterials.push_back(s);
            it = savedMaterials.end() - 1;
        }
        auto* e = reinterpret_cast<float*>(static_cast<char*>(m) + kMatEmissive);
        e[0] = it->emissive[0] + (1.0f - it->emissive[0]) * 0.6f * strength;
        e[1] = it->emissive[1] * (1.0f - strength);
        e[2] = it->emissive[2] * (1.0f - strength);
        hook::setField<float>(m, kMatEmitMult, std::max(it->mult, 1.0f));
        return;
    }
    if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) return;
    auto** children = hook::field<void**>(obj, 0xA0);
    const auto count = hook::field<unsigned short>(obj, 0xA6);
    if (count > 256 || !hook::plausible(children, count * 4)) return;
    for (unsigned i = 0; i < count; ++i) tintGeometry(children[i], strength, depth + 1);
}
void restoreMaterials() {
    for (const auto& s : savedMaterials)
        if (hook::plausible(s.material, 0x48)) {
            std::memcpy(static_cast<char*>(s.material) + kMatEmissive, s.emissive, sizeof(s.emissive));
            hook::setField<float>(s.material, kMatEmitMult, s.mult);
        }
    savedMaterials.clear();
}
void startFlash(std::uint32_t formId) {
    for (auto& f : flashes)
        if (f.formId == formId) { f.left = kFlashSeconds; return; }
    flashes.push_back({formId, kFlashSeconds});
}
void stepFlashes(float delta) {
    if (flashes.empty() && savedMaterials.empty()) return;
    restoreMaterials();  // every frame from the originals, so overlapping flashes never stack
    for (auto it = flashes.begin(); it != flashes.end();) {
        it->left -= delta;
        void* actor = lookup(it->formId);
        if (it->left <= 0 || !actor || !isActor(actor)) { it = flashes.erase(it); continue; }
        if (void* root = vcall<void*>(actor, kVfGet3D)) tintGeometry(root, 1.0f, 0);
        ++it;
    }
}
void applyHit(void* player, const proto::McEvent& ev) {
    void* actor = lookup(ev.formId);
    if (!actor || !isActor(actor) || dead(actor)) return;
    const float level = static_cast<float>(call<std::uint16_t>(kGetLevel, actor));
    // A New Vegas melee weapon already deals New Vegas damage; Minecraft weapons use SkyCraft's scale:
    // a diamond-sword crit drops a level-10 raider.
    const bool nv = ev.flags & proto::kHitNvDamage;
    const float damage = nv ? ev.a : ev.a * (5.0f + 0.25f * level);
    if (damage <= 0) return;
    startKnockback(ev.formId, actor, ev.b, ev.c, ev.d);
    startFlash(ev.formId);
    vcall<bool>(actor, kVfDoDamage, damage, 0.0f, player);
    if (!dead(actor) && !call<bool>(kIsInCombat, actor) && !call<bool>(kIsPlayerTeammate, actor))
        vcall<void>(actor, kVfStartCombat, player, static_cast<void*>(nullptr), 1, 0, 0, 0, 0, static_cast<void*>(nullptr));
    char name[24];
    displayName(actor, name, sizeof(name));
    log::line("combat: hit %s (%08X, level %.0f) for %.1f %s -> %.0f New Vegas damage, knockback %.2f%s%s", name, ev.formId, level, ev.a,
        nv ? "New Vegas" : "Minecraft", damage, ev.d,
        ev.flags & proto::kHitCritical ? ", critical" : "", ev.flags & proto::kHitProjectile ? ", projectile" : "");
}
void killPlayer(void* player, const proto::McEvent& ev) {
    if (dead(player)) return;
    const auto h = health(player);
    log::line("combat: Minecraft player died (killer %08X); killing the New Vegas player", ev.formId);
    void* killer = ev.formId ? lookup(ev.formId) : nullptr;
    vcall<void>(player, kVfDamageModActorValue, kHealth, -(h.current + 1000.0f), killer);
}
void installHook(void* player) {
    if (originalHandleDamage) return;
    auto** table = *reinterpret_cast<void***>(player);
    originalHandleDamage = reinterpret_cast<HandleDamageFn>(hook::replaceSlot(&table[kVfHandleHealthDamage], reinterpret_cast<void*>(&handleHealthDamage)));
    log::line("combat: player damage hook %s", originalHandleDamage ? "installed" : "failed");
}
}
// While the Pip-Boy is up New Vegas is in a menu and perFrame doesn't run: the page's own events
// (equip, use, drop, quest tracking, travel, the native screens) are handled here instead.
void pipBoyEvents(void* player) {
    if (!player || !state().bridge) return;
    auto& link = *state().bridge;
    proto::McEvent ev;
    while (link.popEvent(ev)) {
        bool ok = true;
        switch (ev.type) {
        case proto::kEvNvDrop: ok = guard::run([&] { inventory::drop(player, ev.formId, static_cast<int>(ev.a)); }); break;
        case proto::kEvNvUse: ok = guard::run([&] { inventory::use(player, ev.formId); }); break;
        case proto::kEvNvEquip: ok = guard::run([&] { inventory::equip(player, ev.formId, ev.a > 0.5f); }); break;
        case proto::kEvPipTrack: ok = guard::run([&] { pipdata::trackQuest(ev.formId); }); break;
        case proto::kEvPipTravel: ok = guard::run([&] { pipdata::travelTo(ev.formId); }); break;
        case proto::kEvPipRadio: ok = guard::run([&] { pipdata::setRadio(ev.formId, ev.a > 0.5f); }); break;
        case proto::kEvNvHotkey: ok = guard::run([&] { inventory::setHotkey(player, ev.formId, static_cast<int>(ev.a)); }); break;
        case proto::kEvPipBoyNative: ok = guard::run([&] { input::nativeView(static_cast<int>(ev.a)); }); break;
        case proto::kEvPlayerDied: ok = guard::run([&] { killPlayer(player, ev); }); break;
        default: break;
        }
        if (!ok) log::once("pip-event-fault", "combat: faulted applying a Pip-Boy event");
    }
}
void perFrame(void* player, bool puppeting, float delta) {
    auto& link = *state().bridge;
    if (!player) return;
    guard::run([&] { installHook(player); });
    if (!puppeting) {
        guard::run([&] { flashes.clear(); restoreMaterials(); });
        healthPrimed = false;
        engaged = false;
        proto::McEvent ev;
        while (link.popEvent(ev))
            if (ev.type == proto::kEvPlayerDied && link.minecraftAlive()) guard::run([&] { killPlayer(player, ev); });
            else if (ev.type == proto::kEvNvDrop) inventory::drop(player, ev.formId, static_cast<int>(ev.a));
            else if (ev.type == proto::kEvNvUse) inventory::use(player, ev.formId);
            else if (ev.type == proto::kEvNvEquip) inventory::equip(player, ev.formId, ev.a > 0.5f);
        link.writeActors(nullptr, 0);
        return;
    }
    guard::run([&] { stepKnockbacks(delta); });
    guard::run([&] { stepFlashes(delta); });
    engagedTimer -= delta;
    if (engagedTimer <= 0) { engagedTimer = 0.25f; engaged = false; }
    if (!guard::run([&] { writeActorTable(player); })) log::once("actors-fault", "combat: faulted reading nearby actors");
    proto::McEvent ev;
    while (link.popEvent(ev)) {
        bool ok = true;
        switch (ev.type) {
        case proto::kEvHitActor: ok = guard::run([&] { applyHit(player, ev); }); break;
        case proto::kEvPlayerDied: ok = guard::run([&] { killPlayer(player, ev); }); break;
        case proto::kEvNvDrop: inventory::drop(player, ev.formId, static_cast<int>(ev.a)); break;
        case proto::kEvNvUse: inventory::use(player, ev.formId); break;
        case proto::kEvNvEquip: inventory::equip(player, ev.formId, ev.a > 0.5f); break;
        case proto::kEvPipTrack: pipdata::trackQuest(ev.formId); break;
        case proto::kEvPipTravel: pipdata::travelTo(ev.formId); break;
        case proto::kEvPipRadio: pipdata::setRadio(ev.formId, ev.a > 0.5f); break;
        case proto::kEvNvHotkey: inventory::setHotkey(player, ev.formId, static_cast<int>(ev.a)); break;
        case proto::kEvPipBoyNative: input::nativeView(static_cast<int>(ev.a)); break;
        case proto::kEvExplosion: log::line("combat: Minecraft explosion at %.1f %.1f %.1f, radius %.1f", ev.a, ev.b, ev.c, ev.d); break;
        default: break;
        }
        if (!ok) log::once("event-fault", "combat: faulted applying a Minecraft event");
    }
    if (!dead(player)) guard::run([&] { bridgePlayerDamage(player, delta); });
}
bool playerEngaged() { return engaged; }
} // namespace vegas::combat
