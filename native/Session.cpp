#include "Session.h"
#include "Collision.h"
#include "Combat.h"
#include "Dig.h"
#include "DigMesh.h"
#include "DigPhysics.h"
#include "Items.h"
#include "Inventory.h"
#include "PipData.h"
#include "Arms.h"
#include "Interaction.h"
#include "Actors.h"
#include "BlockLights.h"
#include "Camera.h"
#include "Hud.h"
#include "NpcBlocks.h"
#include "Game.h"
#include "Guard.h"
#include "Input.h"
#include "Log.h"
#include "Rtti.h"
#include "Profile.h"
#include "Runtime.h"
#include <algorithm>
#include <array>
#include <deque>
#include <vector>
#include <cctype>
#include <cmath>
#include <cstring>

// New Vegas port of the per-frame part of SkyCraft's Game.cpp (MIT): Minecraft's player drives
// New Vegas' player and camera; New Vegas tells Minecraft where it is after loads and doors.
namespace vegas {
Runtime& state() { static Runtime runtime; return runtime; }
namespace session {
namespace {
constexpr float kRadToDeg = 57.2957795f;
constexpr float kDegToRad = 0.0174532925f;
constexpr float kTeleportThreshold = 300.0f;  // units; bigger jumps are New Vegas moving the player
constexpr std::uintptr_t kGameHour = 0x011E0358;

Bridge* link = nullptr;
proto::SkyState sky{};
proto::McState mc{};
bool mcWasAlive = false;
std::uint32_t lastMcPid = 0;
// Starts somewhere new each run, so a Minecraft still acknowledging the previous run's
// teleport can't be taken for having arrived at this one's.
std::uint32_t teleportSeq = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return static_cast<std::uint32_t>(t.QuadPart) | 1u; }();
bool teleportPending = true;
std::uint32_t worldId = 0;
std::uint32_t epoch = 1;
game::NiPoint3 lastSet{};
bool haveLastSet = false;
bool loaded = false;
bool loading = false;
std::uint64_t lastLog = 0;
void* hiddenFirstPerson = nullptr;
// Engine functions are only called inside FalloutNV.exe (the test host only emulates data).
bool engine = false;
interaction::Handoff handoff;

std::int64_t lastFrameQpc = 0;
std::uint32_t waterFrame = 0;

// New Vegas' water (Lake Mead, the Colorado, pools) over the block columns around Minecraft's
// player, so Minecraft swims, floats and drowns in it.
void writeWaterGrid(void* cell, const Position& centre) {
    static proto::WaterGrid grid{};
    constexpr int kSize = static_cast<int>(proto::kWaterGridSize);
    grid.originX = static_cast<int>(std::floor(centre.x)) - kSize / 2;
    grid.originZ = static_cast<int>(std::floor(centre.z)) - kSize / 2;
    grid.worldId = worldId;
    int wet = 0;
    for (int dz = 0; dz < kSize; ++dz)
        for (int dx = 0; dx < kSize; ++dx) {
            game::NiPoint3 p{static_cast<float>((grid.originX + dx + 0.5) * proto::kUnitsPerBlock), static_cast<float>(-(grid.originZ + dz + 0.5) * proto::kUnitsPerBlock),
                static_cast<float>(centre.y * proto::kUnitsPerBlock)};
            const float h = game::waterHeight(p, cell);
            const bool water = std::isfinite(h) && h > -1.0e6f && h < 1.0e6f && h != 0.0f;
            grid.surface[dz * kSize + dx] = water ? static_cast<float>(h / proto::kUnitsPerBlock) : proto::kNoWater;
            wet += water;
        }
    link->writeWaterGrid(grid);
    static int lastWet = -1;
    if ((wet > 0) != (lastWet > 0)) log::line("water: %d of %d columns around the player", wet, kSize * kSize);
    lastWet = wet;
}
game::NiPoint3 mcToNv(double x, double y, double z) {
    return {static_cast<float>(x * proto::kUnitsPerBlock), static_cast<float>(-z * proto::kUnitsPerBlock), static_cast<float>(y * proto::kUnitsPerBlock)};
}
float distance(const game::NiPoint3& a, const game::NiPoint3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// New Vegas' first-person arms and weapon: Minecraft draws the hands instead, except while a
// New Vegas weapon is held (items::wielding), whose real model New Vegas draws.
// New Vegas' own setting, restored as soon as Minecraft no longer drives the player.
int savedAutoVanity = -1;
void suppressAutoVanity(bool suppress) {
    auto* setting = reinterpret_cast<std::uint8_t*>(game::kDisableAutoVanityMode);
    if (suppress && savedAutoVanity < 0) { savedAutoVanity = *setting; *setting = 1; }
    else if (!suppress && savedAutoVanity >= 0) { *setting = std::uint8_t(savedAutoVanity); savedAutoVanity = -1; }
}
void hideFirstPerson(void* player, bool hide) {
    void* node = hook::field<void*>(player, game::kPlayerFirstPersonNode);
    if (hide && node) {
        auto flags = hook::field<std::uint32_t>(node, 0x30);
        if (!(flags & 1)) { hook::setField<std::uint32_t>(node, 0x30, flags | 1); hiddenFirstPerson = node; }
    } else if (!hide && hiddenFirstPerson) {
        if (node == hiddenFirstPerson) hook::setField<std::uint32_t>(node, 0x30, hook::field<std::uint32_t>(node, 0x30) & ~1u);
        hiddenFirstPerson = nullptr;
    }
}
// While a New Vegas weapon is held New Vegas draws only that weapon (it hangs off the "Bip01"
// skeleton) and Minecraft draws the arms: the skin nodes beside the skeleton (UpperBody,
// LeftHand, RightHand) are culled.
void cull(void* obj, bool hide) {
    const auto flags = hook::field<std::uint32_t>(obj, 0x30);
    hook::setField<std::uint32_t>(obj, 0x30, hide ? flags | 1 : flags & ~1u);
}
bool isPipBoy(const char* name) {
    if (!hook::plausible(name)) return false;
    char low[64]{};
    for (std::size_t i = 0; i + 1 < sizeof(low) && name[i]; ++i)
        low[i] = char(std::tolower(static_cast<unsigned char>(name[i])));
    return std::strstr(low, "pipboy") != nullptr;
}
// The Pip-Boy hangs off the skeleton's left forearm, next to the weapon: culled with the arms.
std::vector<void*> weaponPipNodes;
void* weaponPipRoot = nullptr;
std::uint32_t weaponPipForm = 0;
ULONGLONG weaponPipChecked = 0;
void clearWeaponPipCache() {
    weaponPipNodes.clear(); weaponPipRoot = nullptr; weaponPipForm = 0; weaponPipChecked = 0;
}
void collectWeaponPipBoy(void* obj, int depth) {
    // plausible(), not readable(): VirtualQuery per node is slow; callers run inside guard::run.
    if (!obj || depth > 40 || !hook::plausible(obj)) return;
    if (isPipBoy(hook::field<const char*>(obj, 0x08))) { weaponPipNodes.push_back(obj); return; }
    auto** vt = hook::field<void**>(obj, 0);
    if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) return;
    auto** children = hook::field<void**>(obj, 0xA0);
    const auto count = hook::field<unsigned short>(obj, 0xA6);
    if (count > 256 || !hook::plausible(children)) return;
    for (unsigned i = 0; i < count; ++i) collectWeaponPipBoy(children[i], depth + 1);
}
// New Vegas' Pip-Boy is up: its menu stack holds 1 while it is (verified live 2026-10-02; the
// Inventory, Stats and Map menu ids 1002, 1003, 1023 are accepted too).
bool pipBoyOpen() {
    const auto top = game::topMenu();
    return top == 1 || top == 1002 || top == 1003 || top == 1023;
}
// The Pip-Boy, Minecraft style: New Vegas keeps only the Pip-Boy's screen (and its lights), which
// its menus are drawn on; arms, glove, the device and the weapon are culled, and Minecraft draws
// its arms and a Minecraft-style Pip-Boy around that screen. What was culled here comes back after.
std::vector<void*> pipCulled;   // nodes culled for the Pip-Boy view
std::vector<void*> pipShown;    // nodes kept visible that New Vegas may cull again
void* pipStyleRoot = nullptr;
std::uint32_t pipStyleForm = 0;
ULONGLONG pipStyleAt = 0;
bool keepsPipBoyScreen(const char* name) {
    if (!hook::plausible(name)) return false;
    char low[64]{};
    for (std::size_t i = 0; i + 1 < sizeof(low) && name[i]; ++i)
        low[i] = char(std::tolower(static_cast<unsigned char>(name[i])));
    return std::strstr(low, "screen") || std::strstr(low, "light") || std::strstr(low, "glare");
}
void cullForPipBoy(void* obj, int depth, bool inPipBoy) {
    if (!obj || depth > 40 || !hook::plausible(obj)) return;
    const char* name = hook::field<const char*>(obj, 0x08);
    const bool node = reinterpret_cast<void*(__thiscall*)(void*)>(hook::field<void**>(obj, 0)[3])(obj) != nullptr;
    const bool pipBoy = inPipBoy || isPipBoy(name);
    auto hide = [&] {
        if (!(hook::field<std::uint32_t>(obj, 0x30) & 1)) { cull(obj, true); pipCulled.push_back(obj); }
    };
    if (hook::plausible(name) && std::strncmp(name, "Weapon", 6) == 0) { hide(); return; }
    if (inPipBoy && !node) { if (!keepsPipBoyScreen(name)) hide(); return; }
    if (!node) return;
    if (isPipBoy(name)) {  // the device node itself stays
        if (hook::field<std::uint32_t>(obj, 0x30) & 1) cull(obj, false);
        pipShown.push_back(obj);
    }
    auto** children = hook::field<void**>(obj, 0xA0);
    const auto count = hook::field<unsigned short>(obj, 0xA6);
    if (count > 256 || !hook::plausible(children)) return;
    for (unsigned i = 0; i < count; ++i) cullForPipBoy(children[i], depth + 1, pipBoy);
}
void stylePipBoy(void* player, bool on) {
    if (!on) {
        for (void* obj : pipCulled) if (hook::plausible(obj)) cull(obj, false);
        pipCulled.clear(); pipShown.clear();
        pipStyleRoot = nullptr; pipStyleForm = 0; pipStyleAt = 0;
        return;
    }
    void* root = hook::field<void*>(player, game::kPlayerFirstPersonNode);
    if (!root || !hook::plausible(root)) return;
    const auto now = GetTickCount64();
    // Walking the skeleton every frame was the Pip-Boy's frame-rate cost: the scene is walked again only when
    // the model changes (or every 250 ms for late attachments); in between the cached nodes are re-flagged,
    // since New Vegas can reset their cull flags.
    if (root == pipStyleRoot && mc.heldNvForm == pipStyleForm && now - pipStyleAt < 250) {
        for (void* obj : pipCulled) if (hook::plausible(obj)) cull(obj, true);
        for (void* obj : pipShown) if (hook::plausible(obj)) cull(obj, false);
        return;
    }
    for (void* obj : pipCulled) if (hook::plausible(obj)) cull(obj, false);
    pipCulled.clear(); pipShown.clear();
    pipStyleRoot = root; pipStyleForm = mc.heldNvForm; pipStyleAt = now;
    auto** children = hook::field<void**>(root, 0xA0);
    const auto count = hook::field<unsigned short>(root, 0xA6);
    if (count > 64 || !hook::plausible(children)) return;
    for (unsigned i = 0; i < count; ++i) {
        void* child = children[i];
        if (!child || !hook::plausible(child)) continue;
        const char* name = hook::field<const char*>(child, 0x08);
        if (hook::plausible(name) && std::strncmp(name, "Bip01", 5) == 0) cullForPipBoy(child, 0, false);
        else if (!(hook::field<std::uint32_t>(child, 0x30) & 1)) { cull(child, true); pipCulled.push_back(child); }  // arms, gloves
    }
}
bool armsHidden = false;
void hideFirstPersonArms(void* player, bool hide) {
    // Reapply culling each frame, but discover Pip-Boy nodes only on equipment/root changes
    // or periodically for delayed model attachment. Traversing every bone each frame is costly.
    if (!hide && !armsHidden) return;
    armsHidden = hide;
    void* root = hook::field<void*>(player, game::kPlayerFirstPersonNode);
    if (!root || !hook::readable(root, 0xA8)) return;
    auto** children = hook::field<void**>(root, 0xA0);
    const auto count = hook::field<unsigned short>(root, 0xA6);
    if (count > 64 || !hook::readable(children, count * 4)) return;
    const auto now = GetTickCount64();
    if (root != weaponPipRoot || mc.heldNvForm != weaponPipForm || now - weaponPipChecked >= 250) {
        weaponPipNodes.clear();
        weaponPipRoot = root; weaponPipForm = mc.heldNvForm; weaponPipChecked = now;
        for (unsigned i = 0; i < count; ++i) {
            void* child = children[i];
            if (!child || !hook::readable(child, 0x34)) continue;
            const char* name = hook::field<const char*>(child, 0x08);
            if (name && hook::readable(name, 6) && std::strncmp(name, "Bip01", 5) == 0)
                collectWeaponPipBoy(child, 0);
        }
    }
    for (void* node : weaponPipNodes) if (hook::plausible(node)) cull(node, hide);
    for (unsigned i = 0; i < count; ++i) {
        void* child = children[i];
        if (!child || !hook::readable(child, 0x34)) continue;
        const char* name = hook::field<const char*>(child, 0x08);
        if (!(name && hook::readable(name, 6) && std::strncmp(name, "Bip01", 5) == 0)) cull(child, hide);
    }
}
// Minecraft's 20 Hz physics ticks interpolated on New Vegas' own frame clock, as Minecraft's
// renderer does with partial ticks (SkyCraft's scheme): sampling Minecraft's per-frame position
// instead judders, because the two games' frames aren't phase-locked. Ticks are kept on their
// exact rhythm and rendered just far enough in the past that the next one has always arrived.
struct Tick { proto::McState s; std::int64_t at; int slots; };
std::deque<Tick> tickHistory;
std::int64_t lastTickFrameQpc = 0;
double renderDelayMs = 10.0;
std::array<double, 40> tickDue{};
std::size_t tickDueNext = 0;
bool tickDueInit = false;
int stampOutliers = 0;
void interpolatedFeet(double& x, double& y, double& z) {
    x = mc.x; y = mc.y; z = mc.z;
    if (mc.tickQpc == 0 || mc.tickMs <= 0.0f) return;
    static const std::int64_t qpcFreq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    const double qpcPerMs = double(qpcFreq) / 1000.0;
    const std::int64_t period = std::max<std::int64_t>(1, std::llround(double(mc.tickMs) * qpcPerMs));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (tickHistory.empty() || tickHistory.back().s.tickQpc != mc.tickQpc) {
        if (!tickHistory.empty() && mc.tickQpc < tickHistory.back().s.tickQpc) tickHistory.clear();  // Minecraft restarted
        Tick tick{mc, mc.tickQpc, 1};
        if (!tickHistory.empty()) {
            auto& last = tickHistory.back();
            const std::int64_t n = std::llround(double(mc.tickQpc - last.at) / double(period));
            const std::int64_t err = mc.tickQpc - (last.at + n * period);
            if (n == 0 && last.slots >= 2) {
                last.at -= period; last.slots -= 1; tick.at = last.at + period;
            } else if (n >= 1 && n <= 10 && (err < 0 ? -err : err) < period * 3 / 10) {
                tick.at = last.at + n * period + err / 16;  // the rhythm is exact; the stamps are noisy
                tick.slots = static_cast<int>(n);
                stampOutliers = 0;
            } else if (n <= 10 && ++stampOutliers < 3) {
                tick.slots = static_cast<int>(std::max<std::int64_t>(n, 1));
                tick.at = last.at + tick.slots * period;
            } else {
                stampOutliers = 0;
            }
        }
        if (lastTickFrameQpc != 0) {
            if (!tickDueInit) { tickDue.fill(renderDelayMs - 1.0); tickDueInit = true; }
            const double dueMs = double(lastTickFrameQpc - tick.at) / qpcPerMs;
            if (dueMs < 30.0) tickDue[tickDueNext++ % tickDue.size()] = dueMs;
        }
        tickHistory.push_back(tick);
        if (tickHistory.size() > 8) tickHistory.pop_front();
    }
    const double frameMs = lastTickFrameQpc != 0 ? double(now.QuadPart - lastTickFrameQpc) / qpcPerMs : 0.0;
    lastTickFrameQpc = now.QuadPart;
    if (tickDueInit) {
        const double target = std::clamp(*std::max_element(tickDue.begin(), tickDue.end()) + 1.0, 4.0, 30.0);
        const double dt = std::min(frameMs, 100.0) / 1000.0;
        renderDelayMs = target > renderDelayMs ? std::min(target, renderDelayMs + 20.0 * dt) : std::max(target, renderDelayMs - 2.0 * dt);
    }
    const std::int64_t renderQpc = now.QuadPart - std::llround(renderDelayMs * qpcPerMs);
    std::size_t i = 0;
    for (std::size_t k = tickHistory.size(); k-- > 0;) if (tickHistory[k].at <= renderQpc) { i = k; break; }
    const Tick& tick = tickHistory[i];
    const Tick* next = i + 1 < tickHistory.size() ? &tickHistory[i + 1] : nullptr;
    const double ticks = double(renderQpc - tick.at) / double(period);
    const double t = std::clamp(ticks, 0.0, 1.0);
    const auto& st = tick.s;
    x = st.prevX + (st.curX - st.prevX) * t;
    y = st.prevY + (st.curY - st.prevY) * t;
    z = st.prevZ + (st.curZ - st.prevZ) * t;
    if (ticks > 1.0 && next) {
        const auto& n = next->s;
        const double gap = double(next->at - (tick.at + period));
        const double u = gap > 0.0 ? std::clamp(double(renderQpc - (tick.at + period)) / gap, 0.0, 1.0) : 1.0;
        x = st.curX + (n.prevX - st.curX) * u;
        y = st.curY + (n.prevY - st.curY) * u;
        z = st.curZ + (n.prevZ - st.curZ) * u;
    }
}
// After Minecraft attaches New Vegas' camera can stay in its idle/third-person state until the view or the
// player moves: wiggle the view for a moment so it refreshes.
int attachNudge = 0, preNudge = 0;
void drive(void* player) {
    auto& st = state();
    double fx, fy, fz;
    interpolatedFeet(fx, fy, fz);
    const auto pos = mcToNv(fx, fy, fz);
    game::movePlayer(player, pos);
    void* controller = game::charController(player);
    void* proxy = controller ? hook::field<void*>(controller, 8) : nullptr;
    // hkpCharacterProxy velocity (verified live: z grows by about -60 Havok units/s per 0.4 s while
    // falling). Minecraft owns the motion, and without this a hole with no native floor makes the
    // native player fall faster every frame: fall damage, then the engine's "below the world" rescue.
    if (proxy) for (std::size_t offset : {0x10, 0x14, 0x18}) hook::setField<float>(proxy, offset, 0.0f);
    void* phantom = proxy ? hook::field<void*>(proxy, 0x30) : nullptr;
    digphysics::setPuppet(phantom ? static_cast<char*>(phantom) + 0x10 : nullptr, pos.z * game::kHavokScale, pos.x * game::kHavokScale, pos.y * game::kHavokScale);
    lastSet = pos;
    haveLastSet = true;
    auto rot = game::rotation(player);
    rot.x = st.pitch * kDegToRad;
    const float wiggle = attachNudge > 0 ? ((--attachNudge & 1) ? 0.6f : -0.6f) : 0.0f;
    rot.z = mcYawToHeading(st.yaw + wiggle);
    hook::setField(player, game::kRefRotation, rot);
    st.feetX = fx; st.feetY = fy; st.feetZ = fz;
    st.feetValid = true;
    camera::update(mc, fx, fy, fz, st.yaw + wiggle, st.pitch);
    // Ranged, first person: New Vegas' weapon on its first-person skeleton, Minecraft's arms on its bones.
    // Ranged, third person: Minecraft's avatar, holding New Vegas' weapon (on its third-person skeleton).
    // Melee: Minecraft's own hand and swing, with New Vegas' weapon model at Minecraft's held-item pose.
    const bool firstPerson = mc.cameraMode == 0;
    const bool nvWeapon = items::wielding();
    const bool melee = items::wieldingMelee();
    hideFirstPerson(player, !nvWeapon || !firstPerson);
    hideFirstPersonArms(player, nvWeapon && firstPerson && !state().showNvArms);
    { profile::Scope t(profile::kPublish); arms::publish(player, !nvWeapon || melee ? proto::kArmsNone : firstPerson ? proto::kArmsFirstPerson : proto::kArmsThirdPerson); }
    const float feet[3] = {pos.x, pos.y, pos.z};
    arms::holdInFirstPerson(player, nvWeapon && melee && firstPerson && mc.handValid, mc.hand);
    { profile::Scope t(profile::kHoldThird); arms::holdInThirdPerson(player, nvWeapon && !firstPerson && mc.handValid, mc.hand, feet, melee); }
}
}
void start(Bridge* bridge, std::uint32_t generation) {
    link = bridge;
    state().bridge = bridge;
    engine = hook::inGameProcess();
    // A reloaded core starts a new collision epoch, so Minecraft resends blocks, collision and digs.
    epoch = 1 + (generation << 16);
    Collision::get().start(bridge);
    if (engine) { input::install(); digphysics::install(); items::installFireHook(); }
}
void stop() {
    if (engine) guard::run([&] {
        // Nothing culled by this core may stay hidden: a reloaded core doesn't know about it.
        if (void* player = game::player()) { stylePipBoy(player, false); hideFirstPersonArms(player, false); hideFirstPerson(player, false); }
    });
    clearWeaponPipCache();
    if (engine) guard::run([&] { suppressAutoVanity(false); });
    handoff.clear(); digphysics::setPuppet(nullptr, 0); items::clear(); arms::clear();
    camera::clear();
    if (engine) guard::run([&] { hud::restore(); digmesh::clear(); blocklights::clear(); });
    Collision::get().stop();
}
void onPreLoad() {
    clearWeaponPipCache(); armsHidden = false; arms::clear();
    state().nvInGame = false; state().nativeInteraction = false;
    handoff.clear(); digphysics::setPuppet(nullptr, 0);
    camera::clear(); dig::clear();
    if (engine) guard::run([&] { hud::restore(); digmesh::clear(); blocklights::clear(); });
    Collision::get().reset(++epoch);
    loading = true; loaded = false; teleportPending = true; haveLastSet = false; hiddenFirstPerson = nullptr;
}
void onLoaded(bool ok) { inventory::resend(); pipdata::resend(); loading = false; loaded = ok; teleportPending = true; haveLastSet = false; }
void onMainMenu() {
    clearWeaponPipCache(); armsHidden = false; arms::clear();
    state().nvInGame = false; state().nativeInteraction = false;
    handoff.clear(); digphysics::setPuppet(nullptr, 0);
    camera::clear(); dig::clear();
    if (engine) guard::run([&] { hud::restore(); digmesh::clear(); blocklights::clear(); });
    loading = false; loaded = false; hiddenFirstPerson = nullptr;
}

void frame() {
    auto& st = state();
    if (!link) return;
    const double linkStart = profile::qpcMs();
    link->heartbeat();
    const bool mcAlive = link->minecraftAlive();
    const bool haveMc = mcAlive && link->readMinecraft(mc);
    { const double d = profile::qpcMs() - linkStart; profile::total[profile::kLink] += d; profile::peak[profile::kLink] = std::max(profile::peak[profile::kLink], d); ++profile::calls[profile::kLink]; }
    const auto pid = link->minecraftPid();
    const bool newProcess = mcAlive && pid && pid != lastMcPid;
    if (mcAlive) lastMcPid = pid;
    if (mcAlive && (!mcWasAlive || newProcess)) {
        log::line("Minecraft connected (pid %u)", pid);
        link->resetOverlay();
        inventory::resend();
        pipdata::resend();
        dig::clear();
        Collision::get().reset(++epoch);
        teleportPending = true;
        // Before it attaches: leave the idle/third-person state and move the native view a little.
        if (engine) { arms::leaveThirdPerson(game::player()); preNudge = 120; }
    }
    if (!mcAlive && mcWasAlive) log::line("Minecraft disconnected");
    mcWasAlive = mcAlive;
    st.mcInWorld = haveMc;
    const bool screen = haveMc && (mc.flags & proto::kMcScreenOpen);
    if (screen && !st.mcScreenOpen) { st.cursorX = st.viewportW / 2; st.cursorY = st.viewportH / 2; }
    st.mcScreenOpen = screen;
    if (haveMc && mc.sensitivity > 0) st.sensitivity = mc.sensitivity;
    st.mcGuiScale = haveMc ? static_cast<int>(mc.guiScale) : 0;

    void* player = game::player();
    void* cell = player ? game::parentCell(player) : nullptr;
    const bool inGame = loaded && player && cell;
    bool menu = false;
    if (engine) guard::run([&] { menu = inGame && (game::menuMode() || game::startMenuOpen()); });
    const bool load = loading;
    st.nvInGame = inGame && !load;
    unsigned sitSleep = 0;
    if (engine && inGame && !load) guard::run([&] { sitSleep = actors::vcall<unsigned>(player, 0x85); });
    const auto action = handoff.update(sitSleep, engine && inGame && !load && mcAlive);
    st.nativeInteraction = action.active;
    if (action.entered) {
        input::releaseAll();
        haveLastSet = false;
        st.feetValid = false;
        log::line("interaction: New Vegas owns the player during furniture use");
    }
    if (action.left) {
        teleportPending = true;
        haveLastSet = false;
        tickHistory.clear();
        st.lookInitialized = false;
        log::line("interaction: Minecraft resumes from the native player's position");
    }
    if ((menu || load) && !st.nvMenuOpen) input::releaseAll();
    st.nvMenuOpen = menu || load;
    sky.flags = (inGame ? proto::kSkyInGame : 0) | ((menu || action.active) ? proto::kSkyMenuOpen : 0) | (load ? proto::kSkyLoading : 0);

    bool puppet = false;
    if (inGame) {
        void* ws = game::worldspace(cell);
        const auto id = game::formId(ws ? ws : cell);
        if (id != worldId) {
            log::line("world %08X -> %08X", worldId, id);
            worldId = id;
            dig::setWorld(id);
            if (engine) guard::run([&] { digmesh::clear(); blocklights::clear(); });
            Collision::get().reset(++epoch);
            teleportPending = true;
            haveLastSet = false;
        }
        const auto current = game::position(player);
        if (haveLastSet && distance(current, lastSet) > kTeleportThreshold) {
            log::line("New Vegas moved the player (%.0f units, %.1f %.1f %.1f -> %.1f %.1f %.1f); resyncing Minecraft", distance(current, lastSet),
                lastSet.x, lastSet.y, lastSet.z, current.x, current.y, current.z);
            teleportPending = true;
            haveLastSet = false;
        }
        const auto rot = game::rotation(player);
        if (teleportPending) {
            ++teleportSeq;
            teleportPending = false;
            st.yaw = headingToMcYaw(rot.z);
            st.pitch = rot.x * kRadToDeg;
            st.lookInitialized = true;
        }
        float dx = 0, dy = 0;
        input::consumeLook(dx, dy);
        if (!st.lookInitialized) { st.yaw = headingToMcYaw(rot.z); st.pitch = rot.x * kRadToDeg; st.lookInitialized = true; }
        if (!st.mcScreenOpen && !st.nvMenuOpen) {
            const float s = st.sensitivity * 0.6f + 0.2f;
            const float factor = s * s * s * 8.0f * 0.15f;
            st.yaw = std::fmod(st.yaw + dx * factor, 360.0f);
            st.pitch = std::clamp(st.pitch + dy * factor, -90.0f, 90.0f);
        }
        const bool arriving = haveMc && !load && !action.active && mc.teleportAck != teleportSeq;
        puppet = haveMc && !load && !action.active && mc.teleportAck == teleportSeq;
        st.minecraftOwnsPlayer = puppet || arriving;
        if (puppet != st.puppeting) log::line("puppet %s", puppet ? "on (Minecraft drives the player)" : "off");
        if (puppet && !st.puppeting && engine) { preNudge = 0; attachNudge = 30; }
        st.puppeting = puppet;
        if (!puppet && preNudge > 0 && engine) guard::run([&] {
            auto r = game::rotation(player);
            r.z += (preNudge-- & 1) ? 0.01f : -0.01f;
            hook::setField(player, game::kRefRotation, r);
        });
        // New Vegas' idle camera (third person after 120 s) would start while Minecraft is away and be stuck when it returns.
        if (engine) guard::run([&] { suppressAutoVanity(true); });
        st.mcCrosshair = puppet && mc.cameraMode == 0 && !st.mcScreenOpen && !st.nvMenuOpen;
        if (engine) guard::run([&] { profile::Scope t(profile::kItems); items::perFrame(player, puppet && !menu, haveMc ? mc.heldNvForm : 0, haveMc && (mc.heldFlags & proto::kHeldMelee)); });
        if (engine && !load) { { profile::Scope t(profile::kInventory); inventory::perFrame(player, worldId); } { profile::Scope t(profile::kPipData); pipdata::perFrame(player, worldId); } }
        input::pipBoyPage();
        if (st.pipBoyUp && !(engine && puppet && menu && pipBoyOpen())) {
            st.pipBoyUp = false;
            st.pipBoyHoleValid = false;
            if (engine) guard::run([&] { stylePipBoy(player, false); });
        }
        if (engine && puppet && !menu) {
            if (!guard::run([&] { profile::Scope t(profile::kDrive); drive(player); })) log::once("drive-fault", "session: faulted moving the player");
        } else if (engine && puppet && menu && pipBoyOpen()) {
            // The Pip-Boy's menus are drawn on its screen, in New Vegas' first-person view: only
            // that screen is shown, inside Minecraft's Pip-Boy (stylePipBoy).
            guard::run([&] {
                hideFirstPerson(player, false);
                hideFirstPersonArms(player, false);
                stylePipBoy(player, true);
                arms::publish(player, proto::kArmsPipBoy);
            });
            st.pipBoyUp = true;
        } else if (engine && !puppet) {
            digphysics::setPuppet(nullptr, 0);
            camera::clear();
            guard::run([&] { hideFirstPerson(player, false); hideFirstPersonArms(player, false); arms::publish(player, proto::kArmsNone); });
        }
        if (!puppet) {
            const auto nativeFeet = toMinecraft({current.x, current.y, current.z}, proto::kUnitsPerBlock);
            st.feetX = nativeFeet.x; st.feetY = nativeFeet.y; st.feetZ = nativeFeet.z;
            st.feetValid = false;
        }
        // Collision around Minecraft's player (or New Vegas' before Minecraft arrives there).
        const Position centre = haveMc && !arriving && !action.active ? Position{mc.x, mc.y, mc.z} : toMinecraft({current.x, current.y, current.z}, proto::kUnitsPerBlock);
        void* bhk = nullptr;
        if (engine) guard::run([&] { bhk = game::havokWorld(cell); });
        void* world = bhk ? hook::field<void*>(bhk, 0x08) : nullptr;
        if (!menu) { profile::Scope t(profile::kCollision); Collision::get().update(centre, world); }
        // Only exteriors have land for Minecraft's overworld features to grow on.
        if (engine && !menu && !load && game::worldspace(cell) && state().exportLand)
            if (profile::Scope t(profile::kLand); !guard::run([&] { Collision::get().updateLand(centre, worldId, cell); })) log::once("land-fault", "land: faulted reading the ground");
        if (engine && !menu && (++waterFrame & 3) == 0) guard::run([&] { profile::Scope t(profile::kWater); writeWaterGrid(cell, centre); });
        if (engine) {
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            const float delta = lastFrameQpc ? std::min(0.25f, static_cast<float>(double(now.QuadPart - lastFrameQpc) / double(freq.QuadPart))) : 0.0f;
            lastFrameQpc = now.QuadPart;
            if (!menu) { profile::Scope t(profile::kCombat); combat::perFrame(player, puppet, delta); }
            else if (st.pipBoyUp) combat::pipBoyEvents(player);
            if (!menu && puppet) guard::run([&] { profile::Scope t(profile::kPushOut); npcblocks::pushActorsOut(player); });
        }
        const auto here = game::position(player);
        const auto p = toMinecraft({here.x, here.y, here.z}, proto::kUnitsPerBlock);
        sky.posX = p.x; sky.posY = p.y; sky.posZ = p.z;
        sky.yaw = st.yaw;
        sky.pitch = st.pitch;
    } else {
        digphysics::setPuppet(nullptr, 0);
        camera::clear();
        if (engine) guard::run([&] { suppressAutoVanity(false); });
        st.puppeting = false;
        st.minecraftOwnsPlayer = false;
        st.mcCrosshair = false;
    }
    if (engine) guard::run([&] { profile::Scope t(profile::kHud); hud::update(puppet && !st.nvMenuOpen); });
    sky.worldId = worldId;
    sky.collisionEpoch = epoch;
    sky.teleportSeq = teleportSeq;
    sky.viewportW = static_cast<std::uint32_t>(st.viewportW.load());
    sky.viewportH = static_cast<std::uint32_t>(st.viewportH.load());
    sky.gameHour = engine ? *reinterpret_cast<const float*>(kGameHour) : 12.0f;
    link->publish(sky);

    const auto now = GetTickCount64();
    if (now - lastLog >= 5000) {
        lastLog = now;
        log::line("NV inGame=%d menu=%d top=%X world=%08X pos=%.2f,%.2f,%.2f | MC alive=%d inWorld=%d pos=%.2f,%.2f,%.2f ack=%d puppet=%d screen=%d collision=%s",
            inGame, menu, engine ? game::topMenu() : 0u, worldId, sky.posX, sky.posY, sky.posZ, mcAlive, haveMc, mc.x, mc.y, mc.z,
            mc.teleportAck == teleportSeq, puppet, st.mcScreenOpen.load(), Collision::get().settled() ? "sent" : "pending");
    }
}
} // namespace session
} // namespace vegas
