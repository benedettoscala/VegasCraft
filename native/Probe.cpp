#include "Probe.h"
#include "Game.h"
#include "Guard.h"
#include "Havok.h"
#include "BlockLights.h"
#include "Input.h"
#include "Items.h"
#include "Inventory.h"
#include "PipData.h"
#include "Arms.h"
#include "Actors.h"
#include "DigPhysics.h"
#include "Log.h"
#include "Profile.h"
#include "Rtti.h"
#include "Runtime.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

// Development probes: lines written to <game>/VegasCraft.cmd run on the main thread, results go
// to VegasCraft.log, and the file is deleted. Used to confirm reverse-engineered layouts live.
namespace vegas::probe {
namespace {
// TEMP: hardware write watch on a float, logging the writer when it jumps upward.
volatile float* watched = nullptr;
LONG CALLBACK watchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1) || !watched) return EXCEPTION_CONTINUE_SEARCH;
    static float last = 0;
    static unsigned logs = 0;
    const float now = *watched;
    if (now - last > 100 && logs < 12) {
        ++logs;
        std::uintptr_t frame = e->ContextRecord->Ebp, callers[6]{};
        for (int i = 0; i < 6 && frame > 0x10000 && frame < 0x7FFF0000 && !(frame & 3); ++i) {
            callers[i] = reinterpret_cast<const std::uintptr_t*>(frame)[1];
            const auto next = reinterpret_cast<const std::uintptr_t*>(frame)[0];
            if (next <= frame) break;
            frame = next;
        }
        log::line("watch: z %.1f -> %.1f eip %08X callers %08X %08X %08X %08X %08X %08X", last, now, unsigned(e->ContextRecord->Eip),
            unsigned(callers[0]), unsigned(callers[1]), unsigned(callers[2]), unsigned(callers[3]), unsigned(callers[4]), unsigned(callers[5]));
    }
    last = now;
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
void* watchRegistration = nullptr;
void watch(volatile float* address) {
    if (!watchRegistration) watchRegistration = AddVectoredExceptionHandler(1, watchHandler);
    watched = address;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr0 = reinterpret_cast<DWORD>(address);
    c.Dr7 = 1 | (1u << 16) | (3u << 18); // local enable 0, write, 4 bytes
    log::line("watch: set=%d on %p", SetThreadContext(GetCurrentThread(), &c), address);
}
std::wstring commandFile;
std::uint64_t lastCheck = 0;
bool (*runConsole)(const char*, void*) = nullptr;

std::uintptr_t number(const std::string& s) { return static_cast<std::uintptr_t>(std::strtoul(s.c_str(), nullptr, 0)); }

void dump(const void* address, std::size_t bytes) {
    for (std::size_t off = 0; off < bytes; off += 16) {
        std::uint32_t w[4]{};
        bool ok = true;
        for (int i = 0; i < 4; ++i) ok &= hook::safeRead(static_cast<const unsigned char*>(address) + off + i * 4, w[i]);
        if (!ok) { log::line("  %08X: <unreadable>", static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(address) + off)); return; }
        float f[4];
        std::memcpy(f, w, sizeof(f));
        log::line("  +%03X: %08X %08X %08X %08X | %g %g %g %g", static_cast<unsigned>(off), w[0], w[1], w[2], w[3], f[0], f[1], f[2], f[3]);
    }
}

void describeShape(const void* shape, int depth, int maxChildren) {
    const std::string pad(depth * 2, ' ');
    const char* name = rtti::name(shape);
    log::line("%sshape %p %s", pad.c_str(), shape, name);
    if (depth > 4) return;
    dump(shape, 0x60);
    auto* container = havok::container(shape);
    if (container) {
        log::line("%s  container at +%X", pad.c_str(), static_cast<unsigned>(static_cast<const unsigned char*>(container) - static_cast<const unsigned char*>(shape)));
        int count = 0;
        bool ok = guard::run([&] {
            alignas(16) unsigned char buffer[havok::kShapeBufferBytes];
            for (auto key = havok::firstKey(container); key != havok::kInvalidKey && count < maxChildren; key = havok::nextKey(container, key), ++count) {
                const void* child = havok::childShape(container, key, buffer);
                log::line("%s  key %08X -> %p %s", pad.c_str(), key, child, child ? rtti::name(child) : "");
                if (child && count < 2) describeShape(child, depth + 2, 2);
            }
        });
        if (!ok) log::line("%s  container iteration faulted after %d children", pad.c_str(), count);
    }
    if (auto* single = havok::singleChild(shape)) {
        log::line("%s  single child", pad.c_str());
        describeShape(single, depth + 1, maxChildren);
    }
}

void havokReport(int limit) {
    void* player = game::player();
    void* cell = player ? game::parentCell(player) : nullptr;
    void* bhk = nullptr;
    if (cell) guard::run([&] { bhk = game::havokWorld(cell); });
    void* world = bhk ? hook::field<void*>(bhk, 0x08) : nullptr;
    log::line("havok: cell=%p bhkWorld=%p %s hkpWorld=%p %s", cell, bhk, bhk ? rtti::name(bhk) : "", world, world ? rtti::name(world) : "");
    if (!world) return;
    auto pos = game::position(player);
    struct Entry { const void* body; float d2; };
    std::vector<Entry> entries;
    havok::forEachBody(world, [&](const void* body, bool fixed) {
        const float* xf = havok::bodyTransform(body);
        const float x = xf[12] / game::kHavokScale - pos.x, y = xf[13] / game::kHavokScale - pos.y, z = xf[14] / game::kHavokScale - pos.z;
        entries.push_back({body, x * x + y * y + z * z});
        (void)fixed;
    });
    log::line("havok: %u bodies (fixed island %d, active %d, inactive %d)", static_cast<unsigned>(entries.size()),
        havok::islandSize(havok::fixedIsland(world)), havok::islandCount(world, true), havok::islandCount(world, false));
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.d2 < b.d2; });
    for (int i = 0; i < limit && i < static_cast<int>(entries.size()); ++i) {
        const void* body = entries[i].body;
        const float* xf = havok::bodyTransform(body);
        const void* shape = havok::bodyShape(body);
        log::line("  body %p %s layer=%u dist=%.0f at %.0f,%.0f,%.0f shape %p %s", body, rtti::name(body), havok::bodyLayer(body),
            std::sqrt(entries[i].d2), xf[12] / game::kHavokScale, xf[13] / game::kHavokScale, xf[14] / game::kHavokScale, shape, rtti::name(shape));
    }
}
}
struct Release { std::uint64_t at; proto::InputType type; std::uint16_t code; };
std::vector<Release> releases;
volatile bool* reloadRequest = nullptr;
void init(const std::wstring& path, bool (*console)(const char*, void*), volatile bool* reload) { commandFile = path; runConsole = console; reloadRequest = reload; }
void shutdown() {
    if (watchRegistration) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        SetThreadContext(GetCurrentThread(), &c);
        RemoveVectoredExceptionHandler(watchRegistration);
        watchRegistration = nullptr;
        watched = nullptr;
    }
    for (const auto& r : releases) state().bridge->pushInput(r.type, r.code, 0);
    releases.clear();
}
bool console(const char* line) { return runConsole && runConsole(line, nullptr); }
void poll() {
    auto now = GetTickCount64();
    for (auto it = releases.begin(); it != releases.end();) {
        if (now >= it->at) { state().bridge->pushInput(it->type, it->code, 0); it = releases.erase(it); }
        else ++it;
    }
    if (commandFile.empty() || now - lastCheck < 200) return;
    lastCheck = now;
    std::ifstream in{std::filesystem::path(commandFile)};
    if (!in) return;
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) if (!l.empty()) lines.push_back(l);
    in.close();
    DeleteFileW(commandFile.c_str());
    for (std::size_t n = 0; n < lines.size(); ++n) {
        const auto& l = lines[n];
        std::istringstream words(l);
        std::string cmd;
        words >> cmd;
        std::vector<std::string> args;
        for (std::string a; words >> a;) args.push_back(a);
        log::line("probe> %s", l.c_str());
        if (cmd == "reload") {
            // The plugin's core is reloaded before the next frame; the lines after this one are for the new core.
            if (reloadRequest) *reloadRequest = true;
            else log::line("reload: hot reload unavailable");
            if (n + 1 < lines.size()) {
                std::ofstream rest{std::filesystem::path(commandFile)};
                for (std::size_t k = n + 1; k < lines.size(); ++k) rest << lines[k] << '\n';
            }
            break;
        }
        bool ok = guard::run([&] {
            void* player = game::player();
            if (cmd == "pipsec" && !args.empty()) {
                input::selectSection(static_cast<int>(number(args[0])));
            } else if (cmd == "pip") {
                pipdata::report(args.empty() ? 1u : unsigned(number(args[0])));
            } else if (cmd == "av") {
                // Actor values of the player (xNVSE: ActorValueOwner at +0xA4; slot 1 / 3 / 6 / 8 are the getters).
                void* avOwner = player ? static_cast<char*>(player) + 0xA4 : nullptr;
                auto** vt = avOwner ? *reinterpret_cast<void***>(avOwner) : nullptr;
                const unsigned lo = args.empty() ? 0 : unsigned(number(args[0])), hi = args.size() > 1 ? unsigned(number(args[1])) : 76;
                for (unsigned code = lo; vt && code <= hi; ++code) {
                    const float base = reinterpret_cast<float(__thiscall*)(void*, unsigned)>(vt[1])(avOwner, code);
                    const float cur = reinterpret_cast<float(__thiscall*)(void*, unsigned)>(vt[3])(avOwner, code);
                    const float cur2 = reinterpret_cast<float(__thiscall*)(void*, unsigned)>(vt[2])(avOwner, code);
                    const float perm = reinterpret_cast<float(__thiscall*)(void*, unsigned)>(vt[8])(avOwner, code);
                    log::line("av %2u: base %.2f cur %.2f cur2 %.2f perm %.2f", code, base, cur, cur2, perm);
                }
            } else if (cmd == "inv") {
                inventory::report();
            } else if (cmd == "profile") {
                profile::reportUntil = GetTickCount64() + (args.empty() ? 20000 : number(args[0]) * 1000);
            } else if (cmd == "sky") {
                // Sky (0x011DEA20): its root node against the world camera, and the root's children.
                void* sky = *reinterpret_cast<void**>(0x011DEA20);
                void* scene = game::global<void>(game::kSceneGraphSlot);
                void* cam = scene ? hook::field<void*>(scene, 0xAC) : nullptr;
                if (cam) { const auto c = hook::field<game::NiPoint3>(cam, 0x8C); log::line("sky: camera %.1f %.1f %.1f", c.x, c.y, c.z); }
                for (std::size_t off = 4; sky && off <= 0x20; off += 4) {
                    void* node = hook::field<void*>(sky, off);
                    if (!node || !hook::readable(node, 0xA8)) continue;
                    const auto l = hook::field<game::NiPoint3>(node, 0x58), w = hook::field<game::NiPoint3>(node, 0x8C);
                    const char* name = hook::field<const char*>(node, 0x08);
                    log::line("sky +%02X %p %s '%s' local %.1f %.1f %.1f world %.1f %.1f %.1f children %u", unsigned(off), node, rtti::name(node),
                        name && hook::readable(name, 2) ? name : "", l.x, l.y, l.z, w.x, w.y, w.z, unsigned(hook::field<unsigned short>(node, 0xA6)));
                }
            } else if (cmd == "pos" && player) {
                auto p = game::position(player);
                auto r = game::rotation(player);
                void* cell = game::parentCell(player);
                log::line("player %p %s pos %.2f %.2f %.2f rot %.4f %.4f %.4f cell %p ws %p ctrl %p third=%u fov=%.2f/%.2f menu=%d top=%X start=%d",
                    player, rtti::name(player), p.x, p.y, p.z, r.x, r.y, r.z, cell, cell ? game::worldspace(cell) : nullptr,
                    game::charController(player), hook::field<std::uint8_t>(player, game::kPlayerIsThirdPerson),
                    hook::field<float>(player, game::kPlayerWorldFov), hook::field<float>(player, game::kPlayerFirstPersonFov),
                    game::menuMode(), game::topMenu(), game::startMenuOpen());
            } else if (cmd == "actor" && !args.empty()) {
                // Position, havok z and process state of a form (e.g. an NPC), to test holes under it.
                void* target = game::lookupForm(static_cast<std::uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16)));
                if (!target) { log::line("actor: no such form"); return; }
                const auto at = game::position(target);
                log::line("actor %p %s pos %.1f %.1f %.1f (blocks %.2f %.2f %.2f) ctrl %p", target, rtti::name(target), at.x, at.y, at.z,
                    at.x / 70, at.z / 70, -at.y / 70, game::charController(target));
            } else if (cmd == "mobs" && state().bridge) {
                // Minecraft's mobs and players near the camera (their contact shadows), in blocks.
                static proto::WorldEntities we{};
                if (!state().bridge->readWorldEntities(we)) { log::line("mobs: no entity list"); return; }
                unsigned n = 0;
                for (std::uint32_t i = 0; i < std::min(we.count, proto::kMaxWorldEntities); ++i) {
                    const auto& e = we.entities[i];
                    if (e.kind != proto::kWeShadow) continue;
                    log::line("mob %u at %.2f %.2f %.2f width %.2f", e.id, e.x, e.y, e.z, e.scale);
                    ++n;
                }
                log::line("mobs: %u", n);
            } else if (cmd == "putat" && args.size() == 4) {
                // Moves a form to absolute New Vegas coordinates (an NPC over a hole, for example).
                void* target = game::lookupForm(static_cast<std::uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16)));
                if (!target) { log::line("putat: no such form"); return; }
                game::movePlayer(target, {std::strtof(args[1].c_str(), nullptr), std::strtof(args[2].c_str(), nullptr), std::strtof(args[3].c_str(), nullptr)});
                const auto q = game::position(target);
                log::line("putat -> %.1f %.1f %.1f", q.x, q.y, q.z);
            } else if (cmd == "contacts") {
                if (args.size() == 3) digphysics::traceContacts(static_cast<unsigned>(number(args[0])), std::strtof(args[1].c_str(), nullptr), std::strtof(args[2].c_str(), nullptr));
            } else if (cmd == "skydump") {
                // Finds where New Vegas keeps its fog distances: every float of the Sky object around the fog colour
                // (+0xC0). Run it in clear weather, then in a storm or an interior, and see which values change.
                void* sky = *reinterpret_cast<void**>(0x011DEA20);
                if (!sky) { log::line("skydump: no sky"); return; }
                for (std::size_t off = 0x50; off < 0x140; off += 0x10) {
                    float v[4]{};
                    for (int i = 0; i < 4; ++i) v[i] = hook::field<float>(sky, off + 4 * i);
                    log::line("skydump +%03zX: %10.3f %10.3f %10.3f %10.3f", off, v[0], v[1], v[2], v[3]);
                }
            } else if (cmd == "lights" && args.size() == 2) {
                blocklights::setBoost(std::strtof(args[0].c_str(), nullptr), std::strtof(args[1].c_str(), nullptr));
                log::line("lights: radius x%s brightness x%s", args[0].c_str(), args[1].c_str());
            } else if (cmd == "watchz" && player) {
                watch(reinterpret_cast<volatile float*>(static_cast<unsigned char*>(player) + 0x38));
            } else if (cmd == "crosshair") {
                // What New Vegas aims at: the reference, its base form and strings found in the base form.
                void* ref = game::crosshairRef();
                if (!ref) { log::line("crosshair: nothing"); return; }
                void* base = hook::field<void*>(ref, 0x20);
                log::line("crosshair: ref %p %s id %08X base %p %s id %08X type %u", ref, rtti::name(ref), hook::field<std::uint32_t>(ref, 0x0C), base,
                    base ? rtti::name(base) : "-", base ? hook::field<std::uint32_t>(base, 0x0C) : 0, base ? hook::field<std::uint8_t>(base, 0x04) : 0);
                for (std::size_t off = 0; base && off < 0x200; off += 4) {
                    const char* text = hook::field<const char*>(base, off);
                    char buf[64]{};
                    if (!text || !hook::readable(text, 4)) continue;
                    std::size_t n = 0;
                    while (n < 60 && hook::readable(text + n, 1) && text[n] >= 32 && text[n] < 127) { buf[n] = text[n]; ++n; }
                    if (n >= 3 && n < 60 && !text[n]) log::line("  base+%03zX -> \"%s\"", off, buf);
                }
            } else if (cmd == "cellrefs" && player) {
                // The cell's reference list (found by scanning for a tList of objects with RTTI), nearest first.
                void* cell = game::parentCell(player);
                const auto here = game::position(player);
                struct Row { float d; void* ref; };
                for (std::size_t off = 0x30; cell && off < 0xC0; off += 4) {
                    std::vector<Row> rows;
                    unsigned total = 0;
                    void* node = reinterpret_cast<unsigned char*>(cell) + off;
                    for (int i = 0; node && i < 20000 && hook::readable(node, 8); ++i, node = hook::field<void*>(node, 4)) {
                        void* ref = hook::field<void*>(node, 0);
                        if (!ref || !hook::readable(ref, 0x40) || std::strncmp(rtti::fastName(ref), ".?AV", 4) != 0) continue;
                        ++total;
                        const auto at = game::position(ref);
                        rows.push_back({std::hypot(at.x - here.x, at.y - here.y), ref});
                    }
                    if (total < 5) continue;
                    log::line("cellrefs: list at cell+%03zX holds %u references", off, total);
                    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
                    const bool onlyItems = !args.empty() && args[0] == "items";
                    // `cellrefs <type>`: only references whose base form is of that type (28: doors).
                    const unsigned onlyType = !args.empty() && args[0] != "items" ? static_cast<unsigned>(number(args[0])) : 0;
                    unsigned printed = 0;
                    for (std::size_t i = 0; i < rows.size() && printed < 25; ++i) {
                        void* candidate = hook::field<void*>(rows[i].ref, 0x20);
                        if (onlyItems && !(candidate && hook::readable(candidate, 0x10) && items::pickable(hook::field<std::uint8_t>(candidate, 0x04)))) continue;
                        if (onlyType && !(candidate && hook::readable(candidate, 0x10) && hook::field<std::uint8_t>(candidate, 0x04) == onlyType)) continue;
                        ++printed;
                        void* ref = rows[i].ref;
                        void* base = hook::field<void*>(ref, 0x20);
                        const auto at = game::position(ref);
                        log::line("  d=%.0f pos %.0f %.0f %.0f ref %p %s id %08X base %08X %s type %u", rows[i].d, at.x, at.y, at.z, ref, rtti::name(ref), hook::field<std::uint32_t>(ref, 0x0C),
                            base && hook::readable(base, 0x10) ? hook::field<std::uint32_t>(base, 0x0C) : 0, base && hook::readable(base, 0x10) ? rtti::name(base) : "-",
                            base && hook::readable(base, 0x10) ? hook::field<std::uint8_t>(base, 0x04) : 0);
                    }
                    break;
                }
            } else if (cmd == "doors" && player) {
                // Door references (base type 28) in every loaded exterior cell, nearest first, with the
                // door's name: walking tests (entering a building) need one to aim at.
                void* tes = game::tes();
                void* grid = tes ? hook::field<void*>(tes, 8) : nullptr;
                const int n = grid ? hook::field<int>(grid, 0xC) : 0;
                auto** cells = grid ? hook::field<void**>(grid, 0x10) : nullptr;
                const auto here = game::position(player);
                struct Row { float d; void* ref; };
                std::vector<Row> rows;
                for (int c = 0; cells && n > 0 && n <= 21 && hook::readable(cells, n * n * 4) && c < n * n; ++c) {
                    if (!cells[c]) continue;
                    void* node = static_cast<unsigned char*>(cells[c]) + 0xAC;
                    for (int i = 0; node && i < 20000 && hook::readable(node, 8); ++i, node = hook::field<void*>(node, 4)) {
                        void* ref = hook::field<void*>(node, 0);
                        if (!ref || !hook::readable(ref, 0x40)) continue;
                        void* base = hook::field<void*>(ref, 0x20);
                        if (!base || !hook::readable(base, 0x40) || hook::field<std::uint8_t>(base, 0x04) != 28) continue;
                        const auto at = game::position(ref);
                        rows.push_back({std::hypot(at.x - here.x, at.y - here.y), ref});
                    }
                }
                std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
                for (std::size_t i = 0; i < rows.size() && i < 15; ++i) {
                    void* ref = rows[i].ref;
                    void* base = hook::field<void*>(ref, 0x20);
                    const char* name = hook::field<const char*>(base, 0x34);
                    const auto at = game::position(ref);
                    const auto rot = game::rotation(ref);
                    log::line("  door d=%.0f pos %.0f %.0f %.0f heading %.2f id %08X base %08X \"%s\"", rows[i].d, at.x, at.y, at.z, rot.z,
                        hook::field<std::uint32_t>(ref, 0x0C), hook::field<std::uint32_t>(base, 0x0C), name && hook::readable(name, 2) ? name : "");
                }
                log::line("doors: %u in %dx%d cells", unsigned(rows.size()), n, n);
            } else if (cmd == "forms" && !args.empty()) {
                // Lists up to 12 forms of a form type, with the strings their first 0x200 bytes point to.
                const unsigned want = static_cast<unsigned>(number(args[0]));
                void* map = *reinterpret_cast<void**>(game::kFormsMap);
                const auto buckets = map ? hook::field<std::uint32_t>(map, 0x04) : 0;
                auto** table = map ? hook::field<void**>(map, 0x08) : nullptr;
                unsigned shown = 0, total = 0;
                for (std::uint32_t b = 0; table && b < buckets; ++b)
                    for (void* e = table[b]; e; e = hook::field<void*>(e, 0)) {
                        void* form = hook::field<void*>(e, 8);
                        if (!form || !hook::readable(form, 0x40) || hook::field<std::uint8_t>(form, 0x04) != want) continue;
                        const char* shownName = hook::field<const char*>(form, 0x34);
                        if (args.size() > 1 && (!shownName || !hook::readable(shownName, 2) || !std::strstr(shownName, args[1].c_str()))) continue;
                        ++total;
                        if (shown >= 12) continue;
                        ++shown;
                        std::string found;
                        for (std::size_t off = 0; off < 0x200; off += 4) {
                            const char* text = hook::field<const char*>(form, off);
                            if (!text || !hook::readable(text, 4)) continue;
                            std::size_t n = 0;
                            while (n < 40 && hook::readable(text + n, 1) && text[n] >= 32 && text[n] < 127) ++n;
                            if (n >= 3 && n < 40 && !text[n]) found += " +" + std::to_string(off) + "=\"" + std::string(text, n) + "\"";
                        }
                        log::line("form %08X %s%s", hook::field<std::uint32_t>(form, 0x0C), rtti::name(form), found.c_str());
                    }
                log::line("forms: %u of type %u", total, want);
            } else if (cmd == "formdump" && !args.empty()) {
                void* form = game::lookupForm(static_cast<std::uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16)));
                const std::size_t bytes = args.size() > 1 ? static_cast<std::size_t>(number(args[1])) : 0x200;
                if (!form) { log::line("formdump: no such form"); return; }
                const char* name = hook::field<const char*>(form, 0x34);
                log::line("formdump %p %s \"%s\"", form, rtti::name(form), name && hook::readable(name, 2) ? name : "");
                for (std::size_t off = 0; off < bytes; off += 16) {
                    char line[200];
                    std::snprintf(line, sizeof(line), "  +%03zX %08X %08X %08X %08X | %g %g %g %g", off, hook::field<std::uint32_t>(form, off), hook::field<std::uint32_t>(form, off + 4),
                        hook::field<std::uint32_t>(form, off + 8), hook::field<std::uint32_t>(form, off + 12), hook::field<float>(form, off), hook::field<float>(form, off + 4),
                        hook::field<float>(form, off + 8), hook::field<float>(form, off + 12));
                    log::line("%s", line);
                }
            } else if (cmd == "fptree" && player) {
                // The first-person scene graph (arms, weapon): names, classes and the cull flag.
                struct Walk {
                    static void node(void* obj, int depth) {
                        if (!obj || depth > 40 || !hook::readable(obj, 0xA8)) return;
                        const char* name = hook::field<const char*>(obj, 0x08);
                        auto** vt = hook::field<void**>(obj, 0);
                        const bool isNode = reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj) != nullptr;
                        const bool named = name && hook::readable(name, 2) && (std::strstr(name, "ip") || std::strstr(name, "eapon") || std::strstr(name, "Hand"));
                        if (!isNode || depth < 2 || named) // the skeleton is deep: geometry, top levels and Pip-Boy / weapon / hand nodes
                            log::line("fptree %*s%p %s \"%s\" flags %X", (depth < 12 ? depth : 12) * 2, "", obj, rtti::name(obj), name && hook::readable(name, 2) ? name : "",
                                hook::field<std::uint32_t>(obj, 0x30));
                        if (!isNode) return;
                        auto** children = hook::field<void**>(obj, 0xA0);
                        const auto count = hook::field<unsigned short>(obj, 0xA6);
                        if (count > 256 || !hook::readable(children, count * 4)) return;
                        for (unsigned i = 0; i < count; ++i) node(children[i], depth + 1);
                    }
                };
                // `fptree [offset]`: another node of the player (e.g. 0x690, the third-person one).
                const bool third = !args.empty() && args[0] == "3d";  // `fptree 3d`: the reference's Get3D node
                const bool address = !args.empty() && args[0][0] == '@';  // `fptree @<hex>`: any node
                const std::size_t at = args.empty() || third || address ? game::kPlayerFirstPersonNode : static_cast<std::size_t>(number(args[0]));
                void* root = address ? reinterpret_cast<void*>(std::strtoul(args[0].c_str() + 1, nullptr, 16))
                    : third ? actors::vcall<void*>(player, actors::kVfGet3D) : hook::field<void*>(player, at);
                log::line("fptree root player+%03zX %p %s", at, root, root && hook::readable(root, 0x10) ? rtti::name(root) : "-");
                if (root && hook::readable(root, 0xA8)) Walk::node(root, 0);
            } else if (cmd == "find" && !args.empty()) {
                // Every scene-graph node with this name under the world root, with its parent chain.
                struct Find {
                    const std::string& want; int found = 0;
                    void walk(void* obj, int depth) {
                        if (!obj || depth > 60 || found > 20 || !hook::readable(obj, 0xA8)) return;
                        const char* name = hook::field<const char*>(obj, 0x08);
                        if (name && hook::readable(name, want.size() + 1) && want == name) {
                            ++found;
                            std::string chain;
                            void* p = hook::field<void*>(obj, 0x18);
                            for (int i = 0; p && i < 12 && hook::readable(p, 0x10); ++i, p = hook::field<void*>(p, 0x18)) {
                                const char* pn = hook::field<const char*>(p, 0x08);
                                chain += " < "; chain += pn && hook::readable(pn, 2) ? pn : "?";
                            }
                            log::line("find %p %s%s", obj, rtti::name(obj), chain.c_str());
                        }
                        auto** vt = hook::field<void**>(obj, 0);
                        if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) return;
                        auto** children = hook::field<void**>(obj, 0xA0);
                        const auto count = hook::field<unsigned short>(obj, 0xA6);
                        if (count > 4096 || !hook::readable(children, count * 4)) return;
                        for (unsigned i = 0; i < count; ++i) walk(children[i], depth + 1);
                    }
                } f{args[0]};
                void* scene = game::global<void>(game::kSceneGraphSlot);
                f.walk(scene, 0);
                log::line("find: %d \"%s\"", f.found, args[0].c_str());
            } else if (cmd == "fields" && player && args.size() == 2) {
                // The player's pointer fields in [from, to) that point at RTTI objects.
                for (std::size_t off = number(args[0]); off < number(args[1]); off += 4) {
                    void* v = hook::field<void*>(player, off);
                    if (!v || !hook::readable(v, 0x10)) continue;
                    const char* n = rtti::name(v);
                    if (!n || !*n) continue;
                    const char* nn = std::strstr(n, "Ni") ? hook::field<const char*>(v, 0x08) : nullptr;
                    log::line("fields +%03zX %p %s \"%s\"", off, v, n, nn && hook::readable(nn, 2) ? nn : "");
                }
            } else if (cmd == "bones3" && player) {
                arms::reportThird(player);
            } else if (cmd == "anim" && player) {
                arms::reportAnim(player);
            } else if (cmd == "props" && !args.empty()) {
                // The first geometries of an actor's 3D: the pointers after NiAVObject and what they point at.
                void* actor = game::lookupForm(static_cast<std::uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16)));
                void* root = actor ? actors::vcall<void*>(actor, actors::kVfGet3D) : nullptr;
                struct Props {
                    int shown = 0;
                    void walk(void* obj, int depth) {
                        if (!obj || depth > 30 || shown >= 3 || !hook::readable(obj, 0xC0)) return;
                        auto** vt = hook::field<void**>(obj, 0);
                        if (reinterpret_cast<void*(__thiscall*)(void*)>(vt[6])(obj)) {  // geometry
                            ++shown;
                            const char* name = hook::field<const char*>(obj, 0x08);
                            log::line("props %p %s \"%s\"", obj, rtti::name(obj), name && hook::readable(name, 2) ? name : "");
                            for (std::size_t off = 0x20; off < 0xC0; off += 4) {
                                void* p = hook::field<void*>(obj, off);
                                if (!p || !hook::readable(p, 0x10)) continue;
                                const char* n = rtti::name(p);
                                if (!n || !*n) continue;
                                log::line("props   +%02zX %p %s", off, p, n);
                                if (std::strstr(n, "PropertyState"))
                                    for (std::size_t k = 0; k < 0x40; k += 4) {
                                        void* q = hook::field<void*>(p, k);
                                        if (q && hook::readable(q, 0x10) && *rtti::name(q)) log::line("props     state+%02zX %p %s", k, q, rtti::name(q));
                                    }
                            }
                            return;
                        }
                        if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) return;
                        auto** children = hook::field<void**>(obj, 0xA0);
                        const auto count = hook::field<unsigned short>(obj, 0xA6);
                        if (count > 256 || !hook::readable(children, count * 4)) return;
                        for (unsigned i = 0; i < count; ++i) walk(children[i], depth + 1);
                    }
                } pr;
                log::line("props: actor %p root %p", actor, root);
                pr.walk(root, 0);
            } else if (cmd == "bones" && player) {
                arms::report(player);
            } else if (cmd == "wpose" && args.size() == 6) {
                // The weapon's offset in the third-person avatar's hand: rx ry rz (degrees) tx ty tz (blocks).
                float r[3], t[3];
                for (int i = 0; i < 3; ++i) { r[i] = std::strtof(args[i].c_str(), nullptr); t[i] = std::strtof(args[3 + i].c_str(), nullptr); }
                arms::setWeaponOffset(r, t);
                log::line("wpose: %s %s %s / %s %s %s", args[0].c_str(), args[1].c_str(), args[2].c_str(), args[3].c_str(), args[4].c_str(), args[5].c_str());
            } else if (cmd == "heldlog") {
                arms::logHeld(args.empty() ? 1000 : static_cast<unsigned>(number(args[0])));
            } else if (cmd == "msize" && !args.empty()) {
                arms::setMeleeSize(std::strtof(args[0].c_str(), nullptr));
            } else if (cmd == "mpose" && args.size() == 7) {
                // A melee model's offset in Minecraft's item frame: view (1 or 3), rx ry rz (degrees), tx ty tz (blocks).
                float r[3], t[3];
                for (int i = 0; i < 3; ++i) { r[i] = std::strtof(args[1 + i].c_str(), nullptr); t[i] = std::strtof(args[4 + i].c_str(), nullptr); }
                arms::setMeleeOffset(static_cast<int>(number(args[0])), r, t);
                log::line("mpose %s: %s %s %s / %s %s %s", args[0].c_str(), args[1].c_str(), args[2].c_str(), args[3].c_str(), args[4].c_str(), args[5].c_str(), args[6].c_str());
            } else if (cmd == "nvarms" && !args.empty()) {
                state().showNvArms = args[0] != "0";
            } else if (cmd == "nvclick") {
                // Holds New Vegas' left mouse button (fire) for a while.
                state().injectMouseButton = args.size() > 1 ? static_cast<int>(number(args[1])) : 0;  // nvclick <ms> [1: right]
                state().injectMouseUntil = GetTickCount64() + (args.empty() ? 150 : static_cast<std::uint64_t>(number(args[0])));
            } else if (cmd == "nvkey" && !args.empty()) {
                // Holds a New Vegas key (DirectInput scancode, e.g. 0x12 = E) for a while.
                state().injectDik = static_cast<std::uint32_t>(number(args[0]));
                state().injectUntil = GetTickCount64() + (args.size() > 1 ? static_cast<std::uint64_t>(number(args[1])) : 250);
            } else if (cmd == "havok") {
                havokReport(args.empty() ? 20 : static_cast<int>(number(args[0])));
            } else if (cmd == "shape" && !args.empty()) {
                describeShape(reinterpret_cast<const void*>(number(args[0])), 0, args.size() > 1 ? static_cast<int>(number(args[1])) : 6);
            } else if (cmd == "dump" && !args.empty()) {
                dump(reinterpret_cast<const void*>(number(args[0])), args.size() > 1 ? number(args[1]) : 0x80);
            } else if (cmd == "rtti" && !args.empty()) {
                auto* object = reinterpret_cast<const void*>(number(args[0]));
                log::line("%p %s", object, rtti::name(object));
            } else if (cmd == "move" && player && args.size() == 3) {
                auto p = game::position(player);
                game::NiPoint3 to{p.x + std::strtof(args[0].c_str(), nullptr), p.y + std::strtof(args[1].c_str(), nullptr), p.z + std::strtof(args[2].c_str(), nullptr)};
                game::setLocation(player, to);
                auto q = game::position(player);
                log::line("move -> %.2f %.2f %.2f", q.x, q.y, q.z);
            } else if (cmd == "rot" && player && args.size() == 2) {
                auto r = game::rotation(player);
                r.x = std::strtof(args[0].c_str(), nullptr) * 0.0174532925f;
                r.z = std::strtof(args[1].c_str(), nullptr) * 0.0174532925f;
                game::setAngle(player, r);
                auto q = game::rotation(player);
                log::line("rot -> %.4f %.4f %.4f", q.x, q.y, q.z);
            } else if (cmd == "aim" && !args.empty() && player) {
                // Faces the Minecraft look at an actor's chest (testing combat without a mouse).
                void* target = game::lookupForm(static_cast<std::uint32_t>(std::strtoul(args[0].c_str(), nullptr, 16)));
                if (!target) { log::line("aim: no such form"); return; }
                const auto from = game::position(player), to = game::position(target);
                const double dx = (to.x - from.x) / 70.0, dz = -(to.y - from.y) / 70.0, dy = (to.z + 70.0 - from.z - 113.0) / 70.0;
                state().yaw = static_cast<float>(std::atan2(-dx, dz) * 57.2957795);
                state().pitch = static_cast<float>(-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 57.2957795);
                log::line("aim: yaw %.1f pitch %.1f distance %.1f blocks", state().yaw, state().pitch, std::sqrt(dx * dx + dz * dz));
            } else if (cmd == "aimmob" && player && state().bridge) {
                // Faces the Minecraft look at the nearest Minecraft mob's middle (its shadow + 0.9).
                static proto::WorldEntities we{};
                if (!state().bridge->readWorldEntities(we)) { log::line("aimmob: no entity list"); return; }
                const auto from = game::position(player);
                const double px = from.x / 70.0, py = from.z / 70.0 + 1.62, pz = -from.y / 70.0;
                double best = 1e9, bx = 0, by = 0, bz = 0;
                for (std::uint32_t i = 0; i < std::min(we.count, proto::kMaxWorldEntities); ++i) {
                    const auto& e = we.entities[i];
                    const double d = std::hypot(e.x - px, e.z - pz);
                    if (e.kind != proto::kWeShadow || d < 1.5 || d >= best) continue;
                    best = d; bx = e.x; by = e.y + 0.9; bz = e.z;
                }
                if (best > 1e8) { log::line("aimmob: no mob"); return; }
                const double dx = bx - px, dy = by - py, dz = bz - pz;
                state().yaw = static_cast<float>(std::atan2(-dx, dz) * 57.2957795);
                state().pitch = static_cast<float>(-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 57.2957795);
                log::line("aimmob: mob at %.2f %.2f %.2f, %.1f blocks: yaw %.1f pitch %.1f", bx, by, bz, best, state().yaw, state().pitch);
            } else if (cmd == "look" && args.size() == 2) {
                // Minecraft look direction in degrees (pitch 90 = straight down).
                state().yaw = std::strtof(args[0].c_str(), nullptr);
                state().pitch = std::clamp(std::strtof(args[1].c_str(), nullptr), -90.0f, 90.0f);
                log::line("look: yaw %.1f pitch %.1f", state().yaw, state().pitch);
            } else if (cmd == "console" && runConsole) {
                // Console line, e.g. `console load quicksave` from the main menu.
                const auto line = l.substr(l.find("console") + 7);
                log::line("console:%s -> %d", line.c_str(), runConsole(line.c_str(), nullptr));
            } else if (cmd == "cursor" && args.size() >= 2 && state().bridge) {
                // Minecraft's screen cursor, in window pixels (as New Vegas' mouse moves it).
                input::setCursor(static_cast<int>(number(args[0])), static_cast<int>(number(args[1])));
            } else if (cmd == "wheel" && !args.empty() && state().bridge) {
                // Minecraft's mouse wheel, in notches (positive = up).
                state().bridge->pushInput(proto::kInScroll, 0, static_cast<std::int32_t>(number(args[0]) * 120));
            } else if ((cmd == "click" || cmd == "key") && !args.empty() && state().bridge) {
                // Minecraft input with a realistic press length (tools' clicks are shorter than a frame).
                const auto type = cmd == "click" ? proto::kInMouseButton : proto::kInKey;
                const auto code = static_cast<std::uint16_t>(number(args[0]));
                state().bridge->pushInput(type, code, 1);
                releases.push_back({GetTickCount64() + (args.size() > 1 ? number(args[1]) : 120), type, code});
            } else {
                log::line("unknown probe");
            }
        });
        if (!ok) log::line("probe faulted");
    }
}
} // namespace vegas::probe
