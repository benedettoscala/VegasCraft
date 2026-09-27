#include "Inventory.h"
#include "Collision.h"
#include "Game.h"
#include "FormIcons.h"
#include "Guard.h"
#include "Items.h"
#include "Log.h"
#include "Probe.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Layouts from xNVSE (GameForms.h, GameExtraData.h), New Vegas 1.4.0.525:
//   TESActorBase +0x64 TESContainer {vtable, tList<FormCount>}; FormCount {i32 count, TESForm* form, ...}
//   TESObjectREFR +0x44 BaseExtraList {vtable, BSExtraData* head, ...}; BSExtraData {vtable, u8 type, next}
//   ExtraContainerChanges (type 0x15) +0x0C Data* {tList<EntryData>* objList, ...}
//   EntryData {tList<ExtraDataList>* extendData, i32 countDelta, TESForm* type}
//   ExtraWorn 0x16 / ExtraWornLeft 0x17 in an entry's ExtraDataList: the item is equipped;
//   ExtraHotkey 0x4A {.., +0x0C u8 index 0-7}: the hotkey (1-8) the player gave it in the Pip-Boy.
// A tList is its first node inline: {T* item, Node* next}.
namespace vegas::inventory {
namespace {
constexpr std::size_t kRefBase = 0x20, kRefExtra = 0x44, kActorContainer = 0x64;
constexpr std::uint8_t kExtraContainerChanges = 0x15, kExtraWorn = 0x16, kExtraWornLeft = 0x17, kExtraHotkey = 0x4A;
constexpr std::uint32_t kPipBoy = 0x00015038, kPipBoyGlove = 0x00025B83;  // part of the wrist, never moved
constexpr ULONGLONG kReadEveryMs = 250, kResendEveryMs = 3000;

struct Item { void* form = nullptr; int count = 0; bool equipped = false; int hotkey = -1; };
std::vector<std::uint8_t> lastSent;
ULONGLONG lastRead = 0, lastSend = 0;
std::uint32_t seq = 0;
bool forceSend = true;
std::unordered_map<const void*, std::string> icons;  // form -> its inventory image path ("" if none)

// VirtualQuery per pointer costs ~0.1 ms here; the snapshot runs inside guard::run, which catches a bad pointer.
bool readable(const void* p, std::size_t) { return reinterpret_cast<std::uintptr_t>(p) > 0x10000; }
template<class F> void forEachNode(const void* list, F&& fn) {
    const void* node = list;
    for (int i = 0; node && i < 4096 && readable(node, 8); ++i) {
        if (void* item = hook::field<void*>(node, 0)) fn(item);
        node = hook::field<const void*>(node, 4);
    }
}
const void* findExtra(const void* extraList, std::uint8_t type) {
    if (!extraList || !readable(extraList, 8)) return nullptr;
    const void* node = hook::field<const void*>(extraList, 4);
    for (int i = 0; node && i < 128 && readable(node, 0x0C); ++i, node = hook::field<const void*>(node, 8))
        if (hook::field<std::uint8_t>(node, 4) == type) return node;
    return nullptr;
}
// The form's inventory image: the first TESIcon-like component (a String whose text is a .dds
// path under interface\icons) among its components, preferring the Pip-Boy images.
std::string iconOf(const void* form) {
    if (auto it = icons.find(form); it != icons.end()) return it->second;
    auto best = formicons::path(form, 0x30, sizeof(proto::InventoryEntry::icon) - 1);
    icons[form] = best;
    return best;
}
void read(void* player, std::map<std::uint32_t, Item>& out) {
    auto add = [&](void* form, int count, bool equipped, int hotkey) {
        if (!form || !readable(form, 0x10)) return;
        auto& item = out[hook::field<std::uint32_t>(form, 0x0C)];
        item.form = form;
        item.count += count;
        item.equipped |= equipped;
        if (hotkey >= 0) item.hotkey = hotkey;
    };
    // What the player started with...
    void* base = hook::field<void*>(player, kRefBase);
    if (base && readable(static_cast<char*>(base) + kActorContainer, 0x0C)) {
        forEachNode(static_cast<char*>(base) + kActorContainer + 4, [&](void* entry) {
            if (readable(entry, 8)) add(hook::field<void*>(entry, 4), hook::field<int>(entry, 0), false, -1);
        });
    }
    // ...and every change since.
    const void* changes = findExtra(static_cast<char*>(player) + kRefExtra, kExtraContainerChanges);
    void* data = changes ? hook::field<void*>(changes, 0x0C) : nullptr;
    void* objList = data && readable(data, 4) ? hook::field<void*>(data, 0) : nullptr;
    if (!objList) return;
    forEachNode(objList, [&](void* entry) {
        if (!readable(entry, 0x0C)) return;
        bool equipped = false;
        int hotkey = -1;
        forEachNode(hook::field<void*>(entry, 0), [&](void* extras) {
            equipped |= findExtra(extras, kExtraWorn) || findExtra(extras, kExtraWornLeft);
            if (const void* key = findExtra(extras, kExtraHotkey); key && readable(key, 0x10)) {
                const unsigned index = hook::field<std::uint8_t>(key, 0x0C);
                if (index < 8) hotkey = static_cast<int>(index);
            }
        });
        add(hook::field<void*>(entry, 8), hook::field<int>(entry, 4), equipped, hotkey);
    });
}
std::vector<std::uint8_t> snapshot(void* player, std::uint32_t worldId) {
    std::map<std::uint32_t, Item> items;
    read(player, items);
    std::vector<proto::InventoryEntry> entries;
    for (const auto& [formId, item] : items) {
        items::Info info;
        if (item.count <= 0 || formId == kPipBoy || formId == kPipBoyGlove || !items::describe(item.form, info)) continue;
        proto::InventoryEntry e{};
        e.formId = formId;
        e.count = item.count;
        e.kind = static_cast<std::uint16_t>(info.kind);
        e.weaponClass = static_cast<std::uint8_t>(info.weaponClass);
        e.flags = static_cast<std::uint8_t>((item.equipped ? proto::kInvEquipped : 0) | ((item.hotkey + 1) << proto::kInvHotkeyShift));
        e.damage = info.damage;
        e.value = info.value;
        e.weight = info.weight;
        e.health = info.health;
        std::memcpy(e.name, info.name, std::min(sizeof(e.name) - 1, std::strlen(info.name)));
        const auto icon = iconOf(item.form);
        std::memcpy(e.icon, icon.data(), std::min(sizeof(e.icon) - 1, icon.size()));
        entries.push_back(e);
        if (entries.size() >= proto::kMaxInventoryEntries) break;
    }
    std::vector<std::uint8_t> bytes(sizeof(proto::InventoryHeader) + entries.size() * sizeof(proto::InventoryEntry));
    proto::InventoryHeader header{0, static_cast<std::uint32_t>(entries.size()), worldId, 0};
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!entries.empty()) std::memcpy(bytes.data() + sizeof(header), entries.data(), entries.size() * sizeof(proto::InventoryEntry));
    return bytes;
}
}

void perFrame(void* player, std::uint32_t worldId) {
    const auto now = GetTickCount64();
    if (!player || now - lastRead < kReadEveryMs) return;
    lastRead = now;
    std::vector<std::uint8_t> bytes;
    if (!guard::run([&] { bytes = snapshot(player, worldId); })) { log::once("inventory-fault", "inventory: faulted reading the player's inventory"); return; }
    const bool changed = bytes.size() != lastSent.size() || std::memcmp(bytes.data() + 4, lastSent.data() + (lastSent.empty() ? 0 : 4), bytes.size() - 4) != 0;
    if (!changed && !forceSend && now - lastSend < kResendEveryMs) return;
    if (changed) log::line("inventory: %u kinds of items", static_cast<unsigned>((bytes.size() - sizeof(proto::InventoryHeader)) / sizeof(proto::InventoryEntry)));
    const std::uint32_t next = ++seq;
    std::memcpy(bytes.data(), &next, 4);
    lastSent = bytes;
    lastSend = now;
    forceSend = false;
    Collision::get().post(proto::kColInventory, std::move(bytes));
}
void resend() { forceSend = true; icons.clear(); }
void report() {
    const auto count = lastSent.size() >= sizeof(proto::InventoryHeader) ? (lastSent.size() - sizeof(proto::InventoryHeader)) / sizeof(proto::InventoryEntry) : 0;
    for (std::size_t i = 0; i < count; ++i) {
        proto::InventoryEntry e;
        std::memcpy(&e, lastSent.data() + sizeof(proto::InventoryHeader) + i * sizeof(e), sizeof(e));
        const unsigned key = e.flags >> proto::kInvHotkeyShift;
        log::line("inv: %08X x%d kind %u%s%s%c %s | %s", e.formId, e.count, e.kind, (e.flags & proto::kInvEquipped) ? " equipped" : "", key ? " hotkey " : "", key ? char('0' + key) : ' ', e.name, e.icon);
    }
}
void use(void* player, std::uint32_t formId) {
    if (!player || !formId) return;
    char line[64];
    std::snprintf(line, sizeof(line), "player.equipitem %08X 0 1", formId);  // equipping an aid item consumes it
    const bool ok = probe::console(line);
    log::line("inventory: Minecraft used %08X: %s", formId, ok ? "used in New Vegas" : "console unavailable");
    lastRead = 0;
}
void equip(void* player, std::uint32_t formId, bool on) {
    if (!player || !formId) return;
    char line[64];
    std::snprintf(line, sizeof(line), "player.%s %08X 0 1", on ? "equipitem" : "unequipitem", formId);
    const bool ok = probe::console(line);
    log::line("inventory: %s %08X from the Pip-Boy's list: %s", on ? "equip" : "unequip", formId, ok ? "done" : "console unavailable");
    lastRead = 0;
}
void setHotkey(void* player, std::uint32_t formId, int key) {
    if (!player || !formId || key < 1 || key > 8) return;
    char line[64];
    std::snprintf(line, sizeof(line), "SetHotkeyItem %d %08X", key, formId);  // xNVSE
    const bool ok = probe::console(line);
    log::line("inventory: hotkey %d for %08X: %s", key, formId, ok ? "set" : "console unavailable");
    lastRead = 0;
}
void drop(void* player, std::uint32_t formId, int count) {
    if (!player || !formId || count <= 0) return;
    char line[64];
    std::snprintf(line, sizeof(line), "player.drop %08X %d", formId, count);
    const bool ok = probe::console(line);
    log::line("inventory: Minecraft threw %d of %08X away: %s", count, formId, ok ? "dropped in New Vegas" : "console unavailable");
    lastRead = 0;  // the next frame reads the result
}
} // namespace vegas::inventory
