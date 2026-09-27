#include "Collision.h"
#include "Guard.h"
#include "Hook.h"
#include "Inventory.h"
#include "Pack.h"
#include "PipData.h"
#include "Runtime.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

// Exercise the production snapshot readers with synthetic engine data. The worker handoff is
// captured here: no game or background collision work is needed to inspect the wire payloads.
struct Message { vegas::proto::ColType type; std::vector<std::uint8_t> payload; };
std::vector<Message> messages;
namespace vegas {
Runtime& state() { static Runtime runtime; return runtime; }
Collision& Collision::get() { static Collision collision; return collision; }
void Collision::post(proto::ColType type, std::vector<std::uint8_t> payload) { messages.push_back({type, std::move(payload)}); }
namespace probe { bool console(const char*) { return false; } }
}
using namespace vegas;
namespace {
void check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
template<std::size_t N, class T> void set(std::array<unsigned char, N>& data, std::size_t offset, T value) { std::memcpy(data.data() + offset, &value, sizeof(value)); }
float __thiscall actorValue(void*, unsigned) { return 100; }
std::uint16_t __thiscall level(void*) { return 5; }
const char* __thiscall description(void*, const void*, std::uint32_t) { return "Fixture description"; }
const char* kIcon = "interface\\icons\\pipboyimages\\fixture.dds";
const void* badPointer = reinterpret_cast<void*>(0xDEADBEEF);

const Message* pipMessage(unsigned kind) {
    for (const auto& message : messages) if (message.type == proto::kColPipData) {
        proto::PipDataHeader header{}; std::memcpy(&header, message.payload.data(), sizeof(header));
        if (header.kind == kind) return &message;
    }
    return nullptr;
}
std::uint16_t count(const Message& message, std::size_t offset) {
    std::uint16_t value; std::memcpy(&value, message.payload.data() + offset, sizeof(value)); return value;
}
}
int main() {
    try {
        auto* globals = VirtualAlloc(reinterpret_cast<void*>(0x011C0000), 0x30000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        check(globals == reinterpret_cast<void*>(0x011C0000), "Cannot reserve synthetic engine globals");
        guard::install();
        // Optional icon data can end at a page boundary or point into an inaccessible page.
        // Neither case should escape the lookup or invalidate the enclosing snapshot.
        auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(pages != nullptr, "Cannot allocate icon boundary fixture");
        DWORD protection;
        check(VirtualProtect(pages + 4096, 4096, PAGE_NOACCESS, &protection), "Cannot protect icon fixture page");
        const auto length = std::strlen(kIcon) + 1;
        char* edgeIcon = reinterpret_cast<char*>(pages + 4096 - length);
        std::memcpy(edgeIcon, kIcon, length);
        std::array<unsigned char, 0x200> iconForm{};
        set(iconForm, 0x18, pages + 4096); set(iconForm, 0x34, edgeIcon);
        check(pipdata::iconPath(iconForm.data()) == kIcon, "Inaccessible candidate or page boundary discarded a valid icon");
        set(iconForm, 0x34, pages + 4096);
        check(pipdata::iconPath(iconForm.data()).empty(), "Unreadable optional icon did not fall back to empty");
        check(pipdata::iconPath(badPointer).empty(), "Unreadable form escaped the bounded icon lookup");
        // A short form at the end of readable memory must not trigger a speculative overread.
        unsigned char* edgeForm = pages + 4096 - 0x40;
        std::memset(edgeForm, 0, 0x40); hook::setField(edgeForm, 0x34, kIcon);
        check(pipdata::iconPath(edgeForm) == kIcon, "Short form at page boundary lost its valid icon");
        std::array<unsigned char, 0xE50> player{};
        std::array<unsigned char, 0x80> base{};
        std::array<unsigned char, 0x200> item{}, info{}, quest{};
        std::array<unsigned char, 8> entry{};
        std::array<unsigned char, 0x24> objective{};
        void* avVtable[11]{}; avVtable[1] = avVtable[3] = reinterpret_cast<void*>(&actorValue); avVtable[10] = reinterpret_cast<void*>(&level);
        void* descVtable[5]{}; descVtable[4] = reinterpret_cast<void*>(&description);
        set(player, 0x20, base.data()); set(player, 0xA4, avVtable);
        set(base, 0x68, entry.data()); set(entry, 0, 3); set(entry, 4, item.data());
        set(item, 4, std::uint8_t(0x1F)); set(item, 0xC, std::uint32_t(0x123));
        set(item, 0x30, badPointer); set(item, 0x34, "Fixture item"); set(item, 0x60, kIcon);
        inventory::resend(); inventory::perFrame(player.data(), 42);
        check(messages.size() == 1 && messages[0].type == proto::kColInventory, "An unrelated non-string pointer suppressed the inventory snapshot");
        proto::InventoryHeader inv{}; std::memcpy(&inv, messages[0].payload.data(), sizeof(inv));
        check(inv.count == 1 && inv.worldId == 42, "Inventory item count/world changed");
        proto::InventoryEntry encoded{}; std::memcpy(&encoded, messages[0].payload.data() + sizeof(inv), sizeof(encoded));
        check(encoded.count == 3 && !std::strcmp(encoded.icon, kIcon), "Inventory count or valid icon was lost");

        set(info, 0x18, badPointer); set(info, 0x1C, "Fixture stat"); set(info, 0x24, descVtable); set(info, 0x34, kIcon);
        *reinterpret_cast<void**>(0x011D61C8 + 4 * 5) = info.data();
        set(quest, 0xC, std::uint32_t(0x456)); set(quest, 0x34, "Fixture quest");
        set(objective, 4, std::uint32_t(10)); set(objective, 8, "Test objective"); set(objective, 0x10, quest.data()); set(objective, 0x20, std::uint32_t(1));
        set(player, 0x6B8, quest.data()); set(player, 0x6BC, objective.data());
        messages.clear(); state().pipBoyUp = false; pipdata::perFrame(player.data(), 42);
        check(messages.empty(), "Closed Pip-Boy performed a data scan");
        state().pipBoyUp = true; pipdata::resend(); pipdata::perFrame(player.data(), 42);
        const auto* stats = pipMessage(proto::kPdStats);
        check(stats != nullptr && count(*stats, sizeof(proto::PipDataHeader)) == 5, "An unrelated non-string pointer suppressed the STATS snapshot");
        const auto* quests = pipMessage(proto::kPdQuests);
        check(quests != nullptr && count(*quests, sizeof(proto::PipDataHeader)) == 1, "Opening the Pip-Boy did not publish quest data");
        proto::PipDataHeader questHeader{}; std::memcpy(&questHeader, quests->payload.data(), sizeof(questHeader));
        Pack expectedQuest;
        expectedQuest.u16(1); expectedQuest.u32(0x456); expectedQuest.str("Fixture quest"); expectedQuest.u8(1);
        expectedQuest.u16(1); expectedQuest.i32(10); expectedQuest.str("Test objective"); expectedQuest.u8(0);
        check(questHeader.worldId == 42 && questHeader.bytes == expectedQuest.size() &&
              quests->payload.size() == sizeof(questHeader) + expectedQuest.size() &&
              !std::memcmp(quests->payload.data() + sizeof(questHeader), expectedQuest.bytes().data(), expectedQuest.size()),
              "Quest name, tracking or objective text was lost from the wire payload");
        check(pipMessage(proto::kPdNotes) && pipMessage(proto::kPdRadio) && pipMessage(proto::kPdMap), "A failed page prevented other pages from publishing");
        guard::uninstall(); VirtualFree(globals, 0, MEM_RELEASE); VirtualFree(pages, 0, MEM_RELEASE);
        std::puts("PASS Pip-Boy snapshots: unrelated invalid pointers, native inventory, STATS, quests and opening/closed page behavior");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
