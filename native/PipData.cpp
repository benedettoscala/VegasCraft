#include "PipData.h"
#include "Collision.h"
#include "Game.h"
#include "FormIcons.h"
#include "Guard.h"
#include "Log.h"
#include "Pack.h"
#include "Probe.h"
#include "Runtime.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Layouts and addresses (New Vegas 1.4.0.525) from xNVSE's and JohnnyGuitar's headers, checked live:
//   Actor +0xA4 ActorValueOwner: vtable slot 1 base value, 3 current value, 0xA level
//   ActorValueInfo array at 0x11D61C8 (by actor value code): TESFullName +0x18, TESDescription +0x24, icon string +0x30
//   BGSPerk: TESFullName +0x18, TESDescription +0x24, TESIcon +0x2C, data {isTrait, minLevel, numRanks, ...} +0x38
//   PlayerCharacter +0x87C perks {PerkRank*: BGSPerk* perk, u8 rank}; misc stat Settings 0x11C6D50[43] (value +4)
//   TESDescription::GetDescription is vtable slot 4 (thiscall: form, chunk 'DESC')
namespace vegas::pipdata {
namespace {
constexpr std::size_t kAvOwner = 0xA4, kPerkList = 0x87C;
constexpr std::uintptr_t kAvInfoArray = 0x11D61C8, kMiscStats = 0x11C6D50;
constexpr unsigned kMiscStatCount = 43;
constexpr std::uint32_t kDescChunk = 0x43534544;  // 'DESC' as New Vegas passes it ('CSED')

// VirtualQuery per pointer costs ~0.1 ms here; every read below runs inside guard::run, which catches a bad one.
bool readable(const void* p, std::size_t) { return reinterpret_cast<std::uintptr_t>(p) > 0x10000; }

// ---- actor values ----
float avCurrent(void* player, unsigned code) {
    void* owner = static_cast<char*>(player) + kAvOwner;
    auto** vt = *reinterpret_cast<void***>(owner);
    return reinterpret_cast<float(__thiscall*)(void*, unsigned)>(vt[3])(owner, code);
}
float avBase(void* player, unsigned code) {
    void* owner = static_cast<char*>(player) + kAvOwner;
    auto** vt = *reinterpret_cast<void***>(owner);
    return reinterpret_cast<float(__thiscall*)(void*, unsigned)>(vt[1])(owner, code);
}
unsigned playerLevel(void* player) {
    void* owner = static_cast<char*>(player) + kAvOwner;
    auto** vt = *reinterpret_cast<void***>(owner);
    return reinterpret_cast<std::uint16_t(__thiscall*)(void*)>(vt[0xA])(owner);
}
// The XP a level takes in total (fXPLevelUpBase 200, fXPLevelUpMult 150: New Vegas' defaults).
int xpForLevel(unsigned level) {
    if (level <= 1) return 0;
    const int n = static_cast<int>(level);
    return 200 * (n - 1) + 75 * (n - 1) * (n - 2);
}

// ---- forms ----
const char* nameOf(const void* form, std::size_t fullNameOffset) {
    if (!form || !readable(form, fullNameOffset + 8)) return "";
    const char* s = hook::field<const char*>(form, fullNameOffset + 4);
    return s && readable(s, 2) ? s : "";
}
const char* descriptionOf(const void* form, std::size_t offset) {
    auto* component = static_cast<char*>(const_cast<void*>(form)) + offset;
    if (!readable(component, 8)) return "";
    auto** vt = *reinterpret_cast<void***>(component);
    if (!readable(vt, 20)) return "";
    const char* text = reinterpret_cast<const char*(__thiscall*)(void*, const void*, std::uint32_t)>(vt[4])(component, form, kDescChunk);
    return text && readable(text, 1) ? text : "";
}
// Descriptions are read from the game's files: once per form.
std::unordered_map<const void*, std::string> descriptions;
const std::string& cachedDescription(const void* form, std::size_t offset) {
    auto it = descriptions.find(form);
    if (it == descriptions.end()) it = descriptions.emplace(form, descriptionOf(form, offset)).first;
    return it->second;
}
std::unordered_map<const void*, std::string> icons;
const std::string& cachedIcon(const void* form) {
    auto it = icons.find(form);
    if (it == icons.end()) it = icons.emplace(form, iconPath(form)).first;
    return it->second;
}

// ---- sending ----
std::uint32_t seqs[8]{};
std::vector<std::uint8_t> lastSent[8];
ULONGLONG lastAt[8]{};
ULONGLONG lastRead[8]{};
bool forceAll = true;

void send(proto::PipDataKind kind, std::uint32_t worldId, const Pack& pack, ULONGLONG now) {
    auto& last = lastSent[kind];
    const bool changed = last.size() != pack.size() || std::memcmp(last.data(), pack.bytes().data(), last.size()) != 0;
    if (!changed && !forceAll && now - lastAt[kind] < 5000) return;
    last = pack.bytes();
    lastAt[kind] = now;
    std::vector<std::uint8_t> payload(sizeof(proto::PipDataHeader) + pack.size());
    proto::PipDataHeader header{kind, ++seqs[kind], worldId, static_cast<std::uint32_t>(pack.size())};
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), pack.bytes().data(), pack.size());
    Collision::get().post(proto::kColPipData, std::move(payload));
}

// ---- STATS ----
constexpr unsigned kSpecialFirst = 5, kSpecialCount = 7, kSkillFirst = 32, kSkillCount = 14;
constexpr unsigned kLimbs[7] = {25, 26, 27, 28, 29, 30, 31};  // head, torso, arms, legs, brain

void avInfo(Pack& out, void* player, unsigned code) {
    const auto* info = *reinterpret_cast<void* const*>(kAvInfoArray + 4 * code);
    out.f32(avCurrent(player, code));
    out.f32(avBase(player, code));
    out.str(nameOf(info, 0x18));
    out.str(info ? cachedDescription(info, 0x24) : std::string());
    out.str(info ? cachedIcon(info) : std::string());
}
void readStats(void* player, Pack& out) {
    const unsigned level = playerLevel(player);
    const int xp = static_cast<int>(avCurrent(player, 24));
    out.u16(static_cast<std::uint16_t>(level));
    out.i32(xp);
    out.i32(xpForLevel(level));
    out.i32(xpForLevel(level + 1));
    out.f32(avCurrent(player, 16)); out.f32(avBase(player, 16));   // health
    out.f32(avCurrent(player, 12)); out.f32(avBase(player, 12));   // action points
    out.f32(avCurrent(player, 23));                                // karma
    out.f32(avCurrent(player, 54));                                // radiation
    out.f32(avCurrent(player, 76));                                // damage threshold
    out.f32(avCurrent(player, 18));                                // damage resistance
    out.f32(avCurrent(player, 46)); out.f32(avCurrent(player, 13)); // weight carried, carry weight
    out.u8(hook::field<std::uint8_t>(player, 0x7BC));              // hardcore
    out.f32(avCurrent(player, 74)); out.f32(avCurrent(player, 73)); out.f32(avCurrent(player, 75));  // hunger, thirst, sleep
    for (unsigned limb : kLimbs) { out.f32(avCurrent(player, limb)); out.f32(avBase(player, limb)); }
    for (unsigned i = 0; i < kSpecialCount; ++i) avInfo(out, player, kSpecialFirst + i);
    for (unsigned i = 0; i < kSkillCount; ++i) avInfo(out, player, kSkillFirst + i);
    // Perks (0x87C: a list of PerkRank {perk, rank}).
    struct Perk { const void* form; unsigned rank; };
    std::vector<Perk> perks;
    const void* node = static_cast<char*>(player) + kPerkList;
    for (int i = 0; node && i < 256 && readable(node, 8); ++i, node = hook::field<const void*>(node, 4)) {
        const void* rank = hook::field<const void*>(node, 0);
        if (!rank || !readable(rank, 8)) continue;
        const void* perk = hook::field<const void*>(rank, 0);
        if (perk && readable(perk, 0x50)) perks.push_back({perk, hook::field<std::uint8_t>(rank, 4)});
    }
    std::sort(perks.begin(), perks.end(), [](const Perk& a, const Perk& b) { return std::strcmp(nameOf(a.form, 0x18), nameOf(b.form, 0x18)) < 0; });
    out.u16(static_cast<std::uint16_t>(perks.size()));
    for (const auto& perk : perks) {
        out.u32(hook::field<std::uint32_t>(perk.form, 0x0C));
        out.str(nameOf(perk.form, 0x18));
        out.str(cachedDescription(perk.form, 0x24));
        out.u8(static_cast<std::uint8_t>(perk.rank));
        out.u8(hook::field<std::uint8_t>(perk.form, 0x38 + 2));
        out.str(cachedIcon(perk.form));
    }
    // General: the miscellaneous statistics (their names are in the client).
    out.u8(kMiscStatCount);
    for (unsigned i = 0; i < kMiscStatCount; ++i) {
        const void* setting = *reinterpret_cast<void* const*>(kMiscStats + 4 * i);
        out.i32(setting && readable(setting, 8) ? hook::field<std::int32_t>(setting, 4) : 0);
    }
}
// ---- DATA ----
// Player: +0x6B8 the tracked quest, +0x6BC the objectives {id +4, text +8, quest +0x10, status +0x20: 1 shown, 2 done},
// +0x5E4 the notes (BGSNote: name +0x4C, TESDescription* +0x6C, type +0x7C, read +0x7D).
// Pip-Boy radio: enabled +0x11DD434, stations 0x11DD554 {ref +0, in range +0x14}.
constexpr std::size_t kTrackedQuest = 0x6B8, kObjectiveList = 0x6BC, kNoteList = 0x5E4;
constexpr std::uintptr_t kRadioEnabled = 0x11DD434, kRadioStations = 0x11DD554, kRadioCurrent = 0x11DD42C;

template <class F> void eachNode(const void* head, F&& fn, int limit = 600) {
    const void* node = head;
    for (int i = 0; node && i < limit && readable(node, 8); ++i, node = hook::field<const void*>(node, 4)) {
        const void* item = hook::field<const void*>(node, 0);
        if (item) fn(item);
    }
}

std::string objectiveText(const void* objective) {
    const char* text = hook::field<const char*>(objective, 8);
    return text && readable(text, 2) ? std::string(text, strnlen(text, 600)) : std::string();
}

void readQuests(void* player, Pack& out) {
    struct Objective { std::uint32_t id; std::string text; bool done; };
    struct Quest { const void* form; std::vector<Objective> objectives; bool open = false; };
    std::vector<Quest> quests;
    eachNode(static_cast<char*>(player) + kObjectiveList, [&](const void* o) {
        if (!readable(o, 0x24)) return;
        const void* quest = hook::field<const void*>(o, 0x10);
        const std::uint32_t status = hook::field<std::uint32_t>(o, 0x20);
        if (!quest || !readable(quest, 0x40) || !(status & 1)) return;
        auto it = std::find_if(quests.begin(), quests.end(), [&](const Quest& q) { return q.form == quest; });
        if (it == quests.end()) { quests.push_back({quest, {}}); it = quests.end() - 1; }
        it->objectives.push_back({hook::field<std::uint32_t>(o, 4), objectiveText(o), (status & 2) != 0});
        if (!(status & 2)) it->open = true;
    });
    const void* tracked = hook::field<const void*>(player, kTrackedQuest);
    quests.erase(std::remove_if(quests.begin(), quests.end(), [](const Quest& q) { return !q.open; }), quests.end());
    std::sort(quests.begin(), quests.end(), [](const Quest& a, const Quest& b) { return std::strcmp(nameOf(a.form, 0x30), nameOf(b.form, 0x30)) < 0; });
    out.u16(static_cast<std::uint16_t>(quests.size()));
    for (auto& q : quests) {
        std::sort(q.objectives.begin(), q.objectives.end(), [](const Objective& a, const Objective& b) { return a.id > b.id; });
        out.u32(hook::field<std::uint32_t>(q.form, 0x0C));
        out.str(nameOf(q.form, 0x30));
        out.u8(q.form == tracked);
        out.u16(static_cast<std::uint16_t>(q.objectives.size()));
        for (const auto& o : q.objectives) { out.i32(static_cast<std::int32_t>(o.id)); out.str(o.text); out.u8(o.done); }
    }
}

std::unordered_map<const void*, std::string> noteTexts;
const std::string& noteText(const void* note) {
    auto it = noteTexts.find(note);
    if (it != noteTexts.end()) return it->second;
    std::string text;
    const void* component = hook::field<const void*>(note, 0x6C);
    if (component && readable(component, 8)) {
        auto** vt = *reinterpret_cast<void***>(const_cast<void*>(component));
        if (readable(vt, 20)) {
            for (std::uint32_t chunk : {kDescChunk, 0x4D414E54u /* 'TNAM' */}) {
                const char* t = reinterpret_cast<const char*(__thiscall*)(const void*, const void*, std::uint32_t)>(vt[4])(component, note, chunk);
                if (t && readable(t, 1) && *t) { text = t; break; }
            }
        }
    }
    return noteTexts.emplace(note, text).first->second;
}

void readNotes(void* player, Pack& out) {
    std::vector<const void*> notes;
    eachNode(static_cast<char*>(player) + kNoteList, [&](const void* n) { if (readable(n, 0x80)) notes.push_back(n); });
    out.u16(static_cast<std::uint16_t>(notes.size()));
    for (const void* n : notes) {
        out.u32(hook::field<std::uint32_t>(n, 0x0C));
        out.str(nameOf(n, 0x48));
        out.u8(hook::field<std::uint8_t>(n, 0x7C));
        out.u8(hook::field<std::uint8_t>(n, 0x7D));
        out.str(noteText(n), 3000);
    }
}

void readRadio(Pack& out) {
    struct Station { std::uint32_t ref; std::string name; bool inRange; };
    std::vector<Station> stations;
    eachNode(reinterpret_cast<const void*>(kRadioStations), [&](const void* s) {
        if (!readable(s, 0x18)) return;
        const void* ref = hook::field<const void*>(s, 0);
        if (!ref || !readable(ref, 0x30)) return;
        const void* base = hook::field<const void*>(ref, 0x20);
        const std::uint32_t id = hook::field<std::uint32_t>(ref, 0x0C);
        if (!base || std::any_of(stations.begin(), stations.end(), [&](const Station& x) { return x.ref == id; })) return;
        stations.push_back({id, nameOf(base, 0x30), hook::field<std::uint8_t>(s, 0x14) != 0});
    });
    const bool enabled = readable(reinterpret_cast<const void*>(kRadioEnabled), 1) && *reinterpret_cast<const std::uint8_t*>(kRadioEnabled) != 0;
    // The station playing: [0x11DD42C] points at its entry, whose first field is the station reference.
    std::uint32_t current = 0;
    if (enabled && readable(reinterpret_cast<const void*>(kRadioCurrent), 4)) {
        const void* entry = *reinterpret_cast<void* const*>(kRadioCurrent);
        const void* ref = entry && readable(entry, 4) ? hook::field<const void*>(entry, 0) : nullptr;
        if (ref && readable(ref, 0x10)) current = hook::field<std::uint32_t>(ref, 0x0C);
    }
    out.u8(enabled);
    out.u16(static_cast<std::uint16_t>(stations.size()));
    for (const auto& s : stations) { out.u32(s.ref); out.str(s.name); out.u8(s.inRange); out.u8(s.ref == current); }
}
// ---- World map ----
// TESWorldSpace: map texture TESTexture +0x24 (path at +0x28), MapData +0x88 {NW cell x, y, SE cell x, y: int16}, persistent cell +0x34 whose
// object list (+0xAC) holds the map markers: ExtraMapMarker (extra type 44, in the list at ref +0x48) -> data {name +4, flags +0xC:
// 1 visible 2 can travel, type +0xE}. A reference: rotation Z +0x2C, position +0x30/+0x34.
constexpr std::size_t kWsTexture = 0x24, kWsMapData = 0x88, kWsPersistent = 0x34, kCellObjects = 0xAC;
constexpr float kCellSize = 4096.0f;

struct MapMarker { std::uint32_t ref; std::string name; std::uint8_t type; float x, y; bool travel; };
std::vector<MapMarker> markerCache;
const void* markerWorld = nullptr;
ULONGLONG markersAt = 0;

const void* extraOfType(const void* ref, std::uint8_t type) {
    const void* node = hook::field<const void*>(ref, 0x48);
    for (int i = 0; node && i < 40 && readable(node, 12); ++i, node = hook::field<const void*>(node, 8))
        if (hook::field<std::uint8_t>(node, 4) == type) return node;
    return nullptr;
}

void scanMarkers(const void* ws) {
    markerCache.clear();
    const void* persistent = hook::field<const void*>(ws, kWsPersistent);
    if (!persistent || !readable(persistent, kCellObjects + 8)) return;
    eachNode(static_cast<const char*>(persistent) + kCellObjects, [&](const void* ref) {
        if (!readable(ref, 0x50)) return;
        const void* extra = extraOfType(ref, 44);
        const void* data = extra && readable(extra, 0x10) ? hook::field<const void*>(extra, 0xC) : nullptr;
        if (!data || !readable(data, 0x10)) return;
        const std::uint16_t flags = hook::field<std::uint16_t>(data, 0xC);
        if (!(flags & 1)) return;
        markerCache.push_back({hook::field<std::uint32_t>(ref, 0x0C), nameOf(data, 0), static_cast<std::uint8_t>(hook::field<std::uint16_t>(data, 0xE)),
                               hook::field<float>(ref, 0x30), hook::field<float>(ref, 0x34), (flags & 2) != 0});
    }, 20000);
}

void readMap(void* player, Pack& out, ULONGLONG now) {
    const void* cell = hook::field<const void*>(player, 0x40);
    const void* ws = cell && readable(cell, 0xC8) ? hook::field<const void*>(cell, 0xC0) : nullptr;
    if (!ws || !readable(ws, 0xA0)) {
        out.str("");
        return;
    }
    const char* tex = hook::field<const char*>(ws, kWsTexture + 4);
    out.str(tex && readable(tex, 2) ? tex : "");
    const auto cellNwX = hook::field<std::int16_t>(ws, kWsMapData), cellNwY = hook::field<std::int16_t>(ws, kWsMapData + 2);
    const auto cellSeX = hook::field<std::int16_t>(ws, kWsMapData + 4), cellSeY = hook::field<std::int16_t>(ws, kWsMapData + 6);
    out.f32(cellNwX * kCellSize); out.f32((cellNwY + 1) * kCellSize); out.f32((cellSeX + 1) * kCellSize); out.f32(cellSeY * kCellSize);
    out.f32(hook::field<float>(player, 0x30)); out.f32(hook::field<float>(player, 0x34)); out.f32(hook::field<float>(player, 0x2C));
    out.u8(1);
    if (markerWorld != ws || now - markersAt > 4000) {
        markerWorld = ws;
        markersAt = now;
        scanMarkers(ws);
    }
    out.u16(static_cast<std::uint16_t>(markerCache.size()));
    for (const auto& m : markerCache) { out.u32(m.ref); out.str(m.name); out.u8(m.type); out.f32(m.x); out.f32(m.y); out.u8(m.travel); }
}
} // namespace

std::string iconPath(const void* form) {
    return formicons::path(form);
}

void resend() { forceAll = true; }

void perFrame(void* player, std::uint32_t worldId) {
    if (!player) return;
    // The pages only exist while the Pip-Boy is up: reading them costs milliseconds (the map's marker scan walks thousands of references).
    static bool wasUp = false;
    const bool up = state().pipBoyUp;
    if (!up) { wasUp = false; return; }
    if (!wasUp) { wasUp = true; for (auto& at : lastRead) at = 0; markersAt = 0; }
    const ULONGLONG now = GetTickCount64();
    auto due = [&](proto::PipDataKind kind, ULONGLONG every) {
        if (now - lastRead[kind] < every) return false;
        lastRead[kind] = now;
        return true;
    };
    if (due(proto::kPdStats, 1000)) {
        Pack pack;
        if (guard::run([&] { readStats(player, pack); })) send(proto::kPdStats, worldId, pack, now);
        else log::once("pipdata-stats-fault", "pipdata: faulted reading the statistics");
    }
    if (due(proto::kPdQuests, 1500)) {
        Pack pack;
        if (guard::run([&] { readQuests(player, pack); })) send(proto::kPdQuests, worldId, pack, now);
        else log::once("pipdata-quests-fault", "pipdata: faulted reading the quests");
    }
    if (due(proto::kPdNotes, 2000)) {
        Pack pack;
        if (guard::run([&] { readNotes(player, pack); })) send(proto::kPdNotes, worldId, pack, now);
        else log::once("pipdata-notes-fault", "pipdata: faulted reading the notes");
    }
    if (due(proto::kPdRadio, 1500)) {
        Pack pack;
        if (guard::run([&] { readRadio(pack); })) send(proto::kPdRadio, worldId, pack, now);
        else log::once("pipdata-radio-fault", "pipdata: faulted reading the radio");
    }
    if (due(proto::kPdMap, 500)) {
        Pack pack;
        if (guard::run([&] { readMap(player, pack, now); })) send(proto::kPdMap, worldId, pack, now);
        else log::once("pipdata-map-fault", "pipdata: faulted reading the map");
    }
    forceAll = false;
}

void report(unsigned kind) {
    log::line("pipdata: kind %u: seq %u, %u bytes last sent", kind, kind < 8 ? seqs[kind] : 0u, kind < 8 ? static_cast<unsigned>(lastSent[kind].size()) : 0u);
}
void trackQuest(std::uint32_t formId) {
    char line[64];
    std::snprintf(line, sizeof(line), "SetCurrentQuest %08X", formId);
    const bool ok = probe::console(line);
    log::line("pipdata: tracking quest %08X: %s", formId, ok ? "done" : "console unavailable");
}
void travelTo(std::uint32_t markerRef) {
    char line[64];
    std::snprintf(line, sizeof(line), "player.moveto %08X", markerRef);
    const bool ok = probe::console(line);
    // New Vegas' own fast travel closes the Pip-Boy as it moves the player; Minecraft takes the player back once no menu is up.
    if (ok) probe::console("CloseAllMenus");
    log::line("pipdata: travel to marker %08X: %s", markerRef, ok ? "done" : "console unavailable");
}
// The Pip-Boy radio through its script command (PipboyRadio enable|disable|tune <station ref>).
void setRadio(std::uint32_t stationRef, bool on) {
    char line[64];
    if (on) std::snprintf(line, sizeof(line), "PipboyRadio enable %08X", stationRef);
    else std::snprintf(line, sizeof(line), "PipboyRadio disable");
    const bool ok = probe::console(line);
    log::line("pipdata: radio %s %08X: %s", on ? "tuned to" : "off", stationRef, ok ? "done" : "console unavailable");
}
} // namespace vegas::pipdata
