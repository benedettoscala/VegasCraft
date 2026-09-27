#include "BlockLights.h"
#include "Camera.h"
#include "Collision.h"
#include "Dig.h"
#include "DigMesh.h"
#include "DigPhysics.h"
#include "Havok.h"
#include "Interaction.h"
#include "Game.h"
#include "Runtime.h"
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <limits>

namespace vegas {
Runtime& state() { static Runtime st; return st; }
struct CollisionTestAccess {
    static void send(Collision& c, Bridge& bridge, const Collision::Geometry& geometry, int rx = 0, int ry = 0, int rz = 0) {
        c.bridge_ = &bridge; c.running_ = true; c.epoch_ = 7;
        c.bodies_.clear();
        auto& body = c.bodies_[reinterpret_cast<void*>(1)];
        body.valid = true; body.geometry = geometry;
        c.harvest(rx, ry, rz);
        auto job = std::move(c.queue_.back()); c.queue_.clear();
        c.sendTriangles(job); c.voxelize(job);
        c.running_ = false;
    }
};
}
using namespace vegas;
namespace {
void check(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
bool close(float a, float b) { return std::fabs(a - b) < 0.002f; }
void dug(int sx, int sy, int sz, const std::vector<unsigned>& cells, unsigned world = 42, bool truncated = false) {
    std::array<std::uint8_t, sizeof(proto::RenDug) + 512> packet{};
    const proto::RenDug h{sx, sy, sz, unsigned(cells.size()), world, 0};
    std::memcpy(packet.data(), &h, sizeof(h));
    std::array<std::uint64_t, 64> bits{};
    for (auto i : cells) bits[i >> 6] |= 1ull << (i & 63);
    std::memcpy(packet.data() + sizeof(h), bits.data(), 512);
    dig::onDug(packet.data(), truncated ? sizeof(h) : unsigned(packet.size()));
}
struct Result { std::vector<proto::ColTri> tris; std::vector<proto::ColBlock> blocks; };
Result collision(unsigned char* base, Bridge& bridge, Collision::Geometry g) {
    auto* ring = base + proto::kOffCollisionRing;
    auto* head = reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
    auto* tail = reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
    *head = *tail = 0;
    CollisionTestAccess::send(Collision::get(), bridge, g);
    Result r;
    for (std::uint64_t at = 0; at < *head;) {
        const auto* h = reinterpret_cast<const proto::ColMsgHeader*>(ring + proto::kColRingDataOff + at);
        const auto* region = reinterpret_cast<const proto::ColRegion*>(h + 1);
        check(region->epoch == 7 && region->minX == 0 && region->maxX == 7, "collision region header changed");
        const auto* data = reinterpret_cast<const std::uint8_t*>(region + 1);
        if (h->type == proto::kColTris) {
            const auto* tris = reinterpret_cast<const proto::ColTri*>(data); r.tris.assign(tris, tris + region->count);
        } else if (h->type == proto::kColRegion) {
            const auto* blocks = reinterpret_cast<const proto::ColBlock*>(data); r.blocks.assign(blocks, blocks + region->count);
        } else check(false, "unexpected collision message");
        at += (sizeof(*h) + h->payloadBytes + 7) & ~7ull;
    }
    return r;
}
float area(const std::vector<proto::ColTri>& tris, unsigned flags) {
    float out = 0;
    for (const auto& t : tris) if ((t.flags & proto::kTriGhost) == flags) out += clip::Area2(clip::FromTriangle(t.v, t.v + 3, t.v + 6)) / 2;
    return out;
}
bool occupied(const Result& r, int x, int y, int z) {
    for (const auto& b : r.blocks) if (b.x == x && b.y == y && b.z == z)
        for (auto word : b.bits) if (word) return true;
    return false;
}
// Synthetic Gamebryo landscape: verifies allocation, shared-data ownership and restoration
// through the production mesh code without requiring a loaded Fallout save.
unsigned frees = 0;
void __thiscall deleteData(void* p) {
    for (auto off : {0x20, 0x24, 0x28, 0x2C, 0x48, 0x4C}) std::free(hook::field<void*>(p, off));
    if (void* agd = hook::field<void*>(p, 0x30)) --*reinterpret_cast<unsigned*>(static_cast<unsigned char*>(agd) + 4);
    std::free(p); ++frees;
}
void* dataVtable[35]{};
void* __cdecl newData() { auto* p = std::calloc(1, 0x58); hook::setField(p, 0, dataVtable); return p; }
void* __cdecl allocData(std::size_t n) { return std::malloc(n); }
void* __thiscall isGeometry(void* p) { return p; }
void* __thiscall notNode(void*) { return nullptr; }
void trampoline(std::uintptr_t address, void* fn) {
    const unsigned char prefix = 0xB8, jump[2] = {0xFF, 0xE0};
    DWORD old;
    check(VirtualProtect(reinterpret_cast<void*>(address), 7, PAGE_EXECUTE_READWRITE, &old), "fixture trampoline protection failed");
    std::memcpy(reinterpret_cast<void*>(address), &prefix, 1);
    std::memcpy(reinterpret_cast<void*>(address + 1), &fn, 4);
    std::memcpy(reinterpret_cast<void*>(address + 5), jump, 2);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 7);
}
void meshTest() {
    for (auto [address, bytes] : {std::pair{0x011D0000u, 0x20000u}, {0x00A70000u, 0x40000u}, {0x01090000u, 0x10000u}}) {
        if (VirtualAlloc(reinterpret_cast<void*>(address), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != reinterpret_cast<void*>(address))
            throw std::runtime_error("cannot reserve synthetic engine address " + std::to_string(address) + " error " + std::to_string(GetLastError()));
    }
    trampoline(0xA7B790, reinterpret_cast<void*>(&newData));
    trampoline(0xAA1070, reinterpret_cast<void*>(&allocData));
    dataVtable[1] = reinterpret_cast<void*>(&deleteData);
    // The original has the exact production RTTI vtable address. Only DeleteThis is needed.
    hook::setField(reinterpret_cast<void*>(0x109DAC4), 4, reinterpret_cast<void*>(&deleteData));
    void* original = newData(); hook::setField(original, 0, reinterpret_cast<void*>(0x109DAC4));
    hook::setField<unsigned>(original, 4, 1); hook::setField<unsigned short>(original, 8, 3);
    hook::setField<unsigned short>(original, 0xC, 1); hook::setField<unsigned short>(original, 0x40, 1);
    const float positions[9] = {0, 0, 35, 280, 0, 35, 0, -280, 35};
    const float uv[6] = {0, 0, 1, 0, 0, 1}; const unsigned short idx[3] = {0, 1, 2};
    auto copy = [&](unsigned off, const void* src, unsigned n) { auto* p = allocData(n); std::memcpy(p, src, n); hook::setField(original, off, p); };
    copy(0x20, positions, sizeof(positions)); copy(0x2C, uv, sizeof(uv)); copy(0x48, idx, sizeof(idx));
    alignas(8) std::uint8_t geom[0xC4]{}, tes[0x100]{}, grid[0x28]{}, cell[0xE0]{}, land[0x2C]{};
    void* geometryVT[60]{}; geometryVT[6] = reinterpret_cast<void*>(&isGeometry); geometryVT[3] = reinterpret_cast<void*>(&notNode);
    hook::setField(geom, 0, geometryVT); hook::setField<unsigned>(geom, 4, 1); hook::setField(geom, 0xB8, original);
    camera::Transform identity{}; identity.scale = 1; for (int i = 0; i < 3; ++i) identity.rot[i][i] = 1;
    hook::setField(geom, 0x68, identity);
    void* cells[]{cell}; hook::setField(cell, 0x4C, static_cast<void*>(land));
    hook::setField<int>(grid, 0xC, 1); hook::setField(grid, 0x10, cells); hook::setField(tes, 8, static_cast<void*>(grid));
    *reinterpret_cast<void**>(game::kTesSingleton) = tes;
    dig::clear(); dig::setWorld(42); dug(0, 0, 0, {1 + 16});
    digmesh::updateGeometry(geom);
    void* cut = hook::field<void*>(geom, 0xB8);
    check(cut != original && hook::field<unsigned short>(cut, 0x40) > 1, "landscape was not cut");
    check(hook::field<unsigned>(original, 4) == 1 && hook::field<unsigned>(cut, 4) == 2, "mesh data ownership is wrong");
    const auto n = hook::field<unsigned short>(cut, 8);
    const auto* p = hook::field<const float*>(cut, 0x20); const auto* t = hook::field<const float*>(cut, 0x2C);
    float a = 0;
    for (unsigned i = 0; i < n; ++i) check(close(t[i * 2], p[i * 3] / 280) && close(t[i * 2 + 1], -p[i * 3 + 1] / 280), "UV interpolation is detached from cut vertices");
    for (unsigned i = 0; i < n; i += 3) a += clip::Area2(clip::FromTriangle(p + i * 3, p + (i + 1) * 3, p + (i + 2) * 3)) / 2;
    check(close(a / (70 * 70), 7), "cut terrain area is wrong");
    dig::clear(); digmesh::update();
    check(hook::field<void*>(geom, 0xB8) == original && frees == 1, "clearing holes did not restore/free mesh data");
    check(hook::field<unsigned>(original, 4) == 1 && hook::field<unsigned>(geom, 4) == 1, "restored mesh refcounts leaked");
    deleteData(original); *reinterpret_cast<void**>(game::kTesSingleton) = nullptr;
}
// New Vegas' landscape: one triangle strip (with a degenerate pair) plus per-vertex texture blend
// weights in a NiAdditionalGeometryData whose stream sits at an offset inside a larger stride.
unsigned freedBlocks = 0;
void __cdecl freeBlock(void* p, std::size_t) { std::free(p); ++freedBlocks; }
void stripTest() {
    trampoline(0xA75EC0, reinterpret_cast<void*>(&newData));
    trampoline(0xAA1460, reinterpret_cast<void*>(&freeBlock));
    hook::setField(reinterpret_cast<void*>(0x109D8AC), 4, reinterpret_cast<void*>(&deleteData));
    void* original = newData(); hook::setField(original, 0, reinterpret_cast<void*>(0x109D8AC));
    hook::setField<unsigned>(original, 4, 1); hook::setField<unsigned short>(original, 8, 4);
    hook::setField<unsigned short>(original, 0xE, 0x4000);
    const float positions[12] = {0, 0, 35, 280, 0, 35, 0, -280, 35, 280, -280, 35};
    // 0,1,2,3 and a repeated point (degenerate triangles that must be ignored).
    const unsigned short points[6] = {0, 1, 2, 3, 3, 3}, lengths[1] = {6};
    auto copy = [&](unsigned off, const void* src, unsigned n) { auto* p = allocData(n); std::memcpy(p, src, n); hook::setField(original, off, p); };
    copy(0x20, positions, sizeof(positions)); copy(0x48, lengths, sizeof(lengths)); copy(0x4C, points, sizeof(points));
    hook::setField<unsigned short>(original, 0x40, 4); hook::setField<unsigned short>(original, 0x44, 1);
    // Blend weights: two floats per vertex at offset 8 of a 16 byte stride, equal to (f, 10 f) with f = x/4 + z/2.
    float weights[4][4]{};
    for (int v = 0; v < 4; ++v) { weights[v][2] = float(v); weights[v][3] = float(v) * 10; }
    alignas(8) unsigned char agd[0x30]{}, stream[0x1C]{}, block[0x10]{};
    void* blocks[2]{nullptr, block};
    auto* weightData = static_cast<float*>(allocData(sizeof(weights))); std::memcpy(weightData, weights, sizeof(weights));
    hook::setField<unsigned>(agd, 4, 1); hook::setField<unsigned>(agd, 0xC, 4); hook::setField<unsigned>(agd, 0x10, 1);
    hook::setField(agd, 0x14, static_cast<void*>(stream)); hook::setField(agd, 0x20, static_cast<void*>(blocks));
    hook::setField<unsigned short>(agd, 0x26, 2);
    hook::setField<unsigned>(stream, 0, 2); hook::setField<unsigned>(stream, 4, 4); hook::setField<unsigned>(stream, 8, 8);
    hook::setField<unsigned>(stream, 0xC, 32); hook::setField<unsigned>(stream, 0x10, 16); hook::setField<unsigned>(stream, 0x14, 1);
    hook::setField<unsigned>(stream, 0x18, 8);
    hook::setField<unsigned>(block, 4, sizeof(weights)); hook::setField(block, 8, static_cast<void*>(weightData));
    hook::setField(original, 0x30, static_cast<void*>(agd));
    alignas(8) std::uint8_t geom[0xC4]{}, tes[0x100]{}, grid[0x28]{}, cell[0xE0]{}, land[0x2C]{};
    void* geometryVT[60]{}; geometryVT[6] = reinterpret_cast<void*>(&isGeometry); geometryVT[3] = reinterpret_cast<void*>(&notNode);
    hook::setField(geom, 0, geometryVT); hook::setField<unsigned>(geom, 4, 1); hook::setField(geom, 0xB8, original);
    camera::Transform identity{}; identity.scale = 1; for (int i = 0; i < 3; ++i) identity.rot[i][i] = 1;
    hook::setField(geom, 0x68, identity);
    void* cells[]{cell}; hook::setField(cell, 0x4C, static_cast<void*>(land));
    hook::setField<int>(grid, 0xC, 1); hook::setField(grid, 0x10, cells); hook::setField(tes, 8, static_cast<void*>(grid));
    *reinterpret_cast<void**>(game::kTesSingleton) = tes;
    dig::clear(); dig::setWorld(42); dug(0, 0, 0, {1 + 16});
    digmesh::updateGeometry(geom);
    void* cut = hook::field<void*>(geom, 0xB8);
    check(cut != original && hook::field<unsigned short>(cut, 0x44) > 1, "strip landscape was not cut into strip data");
    check(hook::field<unsigned short>(original, 0x44) == 1 && hook::field<unsigned>(agd, 4) == 2, "strip data ownership is wrong");
    const auto n = hook::field<unsigned short>(cut, 8), triangles = hook::field<unsigned short>(cut, 0x40);
    check(n == triangles * 3 && hook::field<unsigned short>(cut, 0x44) == triangles, "cut strips are not one triangle each");
    const auto* lengths2 = hook::field<const unsigned short*>(cut, 0x48);
    for (unsigned i = 0; i < triangles; ++i) check(lengths2[i] == 3, "cut strip length is wrong");
    check(hook::field<unsigned short>(cut, 0xE) == 0x4000, "cut mesh did not keep the original renderer flags");
    const auto* p = hook::field<const float*>(cut, 0x20);
    const auto* newWeights = static_cast<const float*>(hook::field<void*>(block, 8));
    check(hook::field<unsigned>(agd, 0xC) == n && hook::field<unsigned>(block, 4) == n * 16 && hook::field<unsigned>(stream, 0xC) == n * 8,
        "additional geometry data was not resized to the cut mesh");
    float area = 0;
    for (unsigned i = 0; i < n; ++i) {
        const float f = p[i * 3] / 70 / 4 + -p[i * 3 + 1] / 70 / 2;
        check(close(newWeights[i * 4 + 2], f) && close(newWeights[i * 4 + 3], f * 10), "blend weights are detached from the cut vertices");
        check(newWeights[i * 4] == 0 && newWeights[i * 4 + 1] == 0, "stream padding was not preserved as empty");
    }
    for (unsigned i = 0; i < n; i += 3) area += clip::Area2(clip::FromTriangle(p + i * 3, p + (i + 1) * 3, p + (i + 2) * 3)) / 2;
    check(close(area / (70 * 70), 15), "cut strip terrain area is wrong (degenerate triangles or winding)");
    // A second change rebuilds from the untouched original and leaves nothing behind.
    dug(0, 0, 0, {1 + 16, 2 + 16}); digmesh::updateGeometry(geom);
    check(hook::field<void*>(geom, 0xB8) != original && hook::field<unsigned>(agd, 0xC) == hook::field<unsigned short>(hook::field<void*>(geom, 0xB8), 8), "re-cut did not patch the weights again");
    dig::clear(); digmesh::update();
    check(hook::field<void*>(geom, 0xB8) == original, "clearing holes did not restore the strip mesh");
    check(hook::field<unsigned>(agd, 0xC) == 4 && hook::field<void*>(block, 8) == weightData && hook::field<unsigned>(block, 4) == sizeof(weights) &&
        hook::field<unsigned>(stream, 0xC) == 32, "additional geometry data was not restored");
    check(freedBlocks == 2, "cut weight buffers leaked");
    check(hook::field<unsigned>(agd, 4) == 1, "additional geometry data refcount leaked");
    deleteData(original); std::free(weightData); *reinterpret_cast<void**>(game::kTesSingleton) = nullptr;
}
void physicsTest() {
    alignas(16) unsigned char terrain[0x30]{}, actor[0x30]{}, child[0x20]{};
    hook::setField<unsigned>(terrain, 0x1C, havok::kLayerGround);
    hook::setField<unsigned>(actor, 0x1C, 30);
    hook::setField(child, 0xC, static_cast<void*>(terrain));
    const float scale = 70 * game::kHavokScale;
    dig::clear(); dig::setWorld(42); dug(0, 0, 0, {0});
    digphysics::Contact p{{0.5f * scale, -0.5f * scale, scale, 0}, {0, 0, 1, 0}, actor, child};
    check(digphysics::onDugGround(p), "actor support contact on a dug cube's exact top survived");
    std::swap(p.bodyA, p.bodyB); p.normal[2] = -1;
    check(digphysics::onDugGround(p), "reversed terrain/actor contact survived");
    hook::setField<unsigned>(terrain, 0x1C, 1);
    check(!digphysics::onDugGround(p), "native building contacts were removed");
    hook::setField<unsigned>(terrain, 0x1C, havok::kLayerTerrain);
    check(!digphysics::onDugGround(p), "rock/cliff mesh contacts were removed in a dug cell");
    hook::setField<unsigned>(terrain, 0x1C, havok::kLayerGround);
    hook::setField<unsigned>(actor, 0x1C, havok::kLayerCharCast);
    check(digphysics::onDugGround(p), "player controller cast contacts with removed ground survived");
    hook::setField<unsigned>(actor, 0x1C, 37);
    check(!digphysics::onDugGround(p), "line-of-sight queries lost their ground contacts");
    hook::setField<unsigned>(actor, 0x1C, havok::kLayerCharController);
    hook::setField<unsigned>(terrain, 0x1C, havok::kLayerGround);
    hook::setField<unsigned>(actor, 0x1C, 1);
    check(!digphysics::onDugGround(p), "rigid-body terrain contacts were removed");
    hook::setField<unsigned>(actor, 0x1C, 30);
    p.normal[2] = 0;
    check(!digphysics::onDugGround(p), "invalid contact normal changed native collisions");
    p.normal[2] = -1; p.position[0] = 1.5f * scale;
    check(!digphysics::onDugGround(p), "neighboring terrain lost its support");
    dug(-1, -1, -1, {4095});
    p.position[0] = -0.5f * scale; p.position[1] = 0.5f * scale; p.position[2] = 0;
    check(digphysics::onDugGround(p), "negative-coordinate dug contact survived");
    hook::setField(child, 0xC, static_cast<void*>(child));
    check(!digphysics::onDugGround(p), "cyclic collision-body parent chain was accepted");
    hook::setField(child, 0xC, static_cast<void*>(terrain));
    digphysics::setPuppet(actor, 0);
    p.position[2] = 0.4f;
    check(digphysics::abovePuppetFeet(p), "puppet head/wall contact was retained");
    p.position[2] = 0.2f;
    check(!digphysics::abovePuppetFeet(p), "puppet foot support was removed");
    p.position[2] = std::numeric_limits<float>::quiet_NaN();
    check(!digphysics::abovePuppetFeet(p), "invalid puppet contact was accepted");
    // Height field push-out: a cast from another collidable, ground above the feet beside the puppet.
    alignas(16) unsigned char otherBody[0x30]{};
    digphysics::setPuppet(otherBody, 0, -0.5f * scale, 0.5f * scale);
    hook::setField<unsigned>(actor, 0x1C, havok::kLayerCharCast);
    p.position[0] = -0.5f * scale; p.position[1] = 0.5f * scale; p.position[2] = 2.0f;
    check(digphysics::groundAbovePuppet(p), "ground above the puppet's feet lifted it out of a hole");
    p.position[2] = 0.1f;
    check(!digphysics::groundAbovePuppet(p), "ground under the puppet's feet was dropped");
    p.position[2] = 2.0f; p.position[0] = 3.0f * scale;
    check(!digphysics::groundAbovePuppet(p), "ground far from the puppet was dropped");
    p.position[0] = -0.5f * scale;
    hook::setField<unsigned>(terrain, 0x1C, 1);
    check(!digphysics::groundAbovePuppet(p), "building contacts above the puppet were dropped");
    hook::setField<unsigned>(terrain, 0x1C, havok::kLayerGround);
    hook::setField<unsigned>(actor, 0x1C, havok::kLayerCharController);
    digphysics::setPuppet(nullptr, 0);
    check(!digphysics::groundAbovePuppet(p), "ground contacts were dropped without a puppet");
    p.position[2] = scale;
    check(!digphysics::abovePuppetFeet(p), "native control retained the puppet contact filter");
    float height = 70;
    digphysics::adjustLandHeight({35, -35, -35}, height);
    check(close(height, -35), "land rescue raised an actor out of a hole");
    height = 70; digphysics::adjustLandHeight({105, -35, -35}, height);
    check(close(height, 70), "land rescue was disabled on ordinary ground");
    dig::clear(); height = 70; digphysics::adjustLandHeight({35, -35, -35}, height);
    check(close(height, 70) && !digphysics::onDugGround(p), "clearing holes did not restore actor terrain support");
}
// Emulate only the native light API. Exercise production field writes, scene registrations,
// ownership and removal without claiming these stubs validate New Vegas' visual renderer.
void* lightVT[2]{};
unsigned lightFrees = 0, wrapperFrees = 0, lightAdds = 0, lightRemoves = 0;
std::vector<void*> activeLights;
void* lightScene = nullptr;
void* __cdecl getLightScene() { return lightScene; }
void __thiscall deleteLight(void* p) { ++lightFrees; std::free(p); }
void __thiscall deleteWrapper(void* p) {
    auto* light = hook::field<void*>(p, 0xF8);
    const auto refs = hook::field<unsigned>(light, 4) - 1;
    hook::setField(light, 4, refs); if (!refs) deleteLight(light);
    ++wrapperFrees; std::free(p);
}
void* wrapperVT[2]{};
void* __cdecl newLight() { auto* p = std::calloc(1, 0xFC); hook::setField(p, 0, lightVT); return p; }
void __cdecl attenuation(void* p, unsigned radius) {
    check(close(hook::field<float>(p, 0xE0), float(radius)) &&
        close(hook::field<float>(p, 0xE4), float(radius)) && close(hook::field<float>(p, 0xE8), float(radius)),
        "point-light radius was not written on all axes");
    hook::setField<float>(p, 0xF0, 1); hook::setField<float>(p, 0xF4, 0);
    hook::setField<float>(p, 0xF8, 1.0f / (radius * radius));
}
void* __thiscall addLight(void*, void* light, bool dynamic) {
    check(dynamic && close(hook::field<float>(light, 0x58), hook::field<float>(light, 0x8C)) &&
        hook::field<float>(light, 0xE0) > 1 && hook::field<float>(light, 0xF8) > 0,
        "scene received a point light before its fields were configured");
    hook::setField<unsigned>(light, 4, hook::field<unsigned>(light, 4) + 1);
    auto* p = std::calloc(1, 0x120); hook::setField(p, 0, wrapperVT);
    hook::setField<unsigned>(p, 4, 1); hook::setField(p, 0xF8, light);
    activeLights.push_back(p); ++lightAdds; return p;
}
void __thiscall removeLight(void*, void** wrapper) {
    auto it = std::find(activeLights.begin(), activeLights.end(), *wrapper);
    check(it != activeLights.end(), "scene light was removed twice");
    auto* light = hook::field<void*>(*wrapper, 0xF8);
    check(close(hook::field<float>(light, 0xC4), 0), "removed light was not dimmed before scene removal");
    hook::setField<unsigned>(*wrapper, 4, hook::field<unsigned>(*wrapper, 4) - 1);
    activeLights.erase(it); ++lightRemoves;
}
void lightTest() {
    blocklights::setApiForTests({&getLightScene, &newLight, &attenuation, &addLight, &removeLight});
    lightVT[1] = reinterpret_cast<void*>(&deleteLight); wrapperVT[1] = reinterpret_cast<void*>(&deleteWrapper);
    alignas(8) unsigned char sceneA[16]{}, sceneB[16]{};
    hook::setField<unsigned>(sceneA, 4, 1); hook::setField<unsigned>(sceneB, 4, 1);
    auto*& current = lightScene; current = sceneA;
    struct Packet { proto::RenLights h; proto::RenLight l[2]; } packet{{0, 0, 0, 2}, {{0, 0, 0, 15, 0x000000FF}, {6, 0, 0, 15, 0x0000FF00}}};
    blocklights::clear(); blocklights::onLights(reinterpret_cast<const std::uint8_t*>(&packet), sizeof(packet));
    blocklights::update({0, 0, 0}, true);
    check(lightAdds == 2 && activeLights.size() == 2, "light clusters were not registered");
    const auto original = activeLights;
    blocklights::update({7, 0, 0}, true);
    check(activeLights == original && lightAdds == 2, "distance reordering recreated native lights");
    packet.h.count = 1; packet.l[0].x = 2;
    blocklights::onLights(reinterpret_cast<const std::uint8_t*>(&packet), sizeof(packet.h) + sizeof(packet.l[0]));
    blocklights::update({0, 0, 0}, true);
    check(activeLights.size() == 1 && lightRemoves == 1 && lightAdds == 2, "removing an emitter left a scene light registered");
    auto* wrapper = activeLights.front(); auto* light = hook::field<void*>(wrapper, 0xF8);
    check(close(hook::field<float>(wrapper, 0x100), 175) && close(hook::field<float>(light, 0x8C), 175), "weighted light centre did not update the scene snapshot");
    const auto points = blocklights::nearby({0, 0, 0});
    blocklights::configure(light, points.front(), 5);
    check(close(hook::field<float>(light, 0xD4), 1) && close(hook::field<float>(light, 0xD8), 0), "diffuse light color was corrupted by the radius");
    // A truncated update must retain the existing light until a complete replacement arrives.
    packet.h.count = 2; blocklights::onLights(reinterpret_cast<const std::uint8_t*>(&packet), sizeof(packet.h));
    blocklights::update({0, 0, 0}, true); check(activeLights.size() == 1, "truncated packet removed an existing light");
    current = sceneB; blocklights::update({0, 0, 0}, true);
    check(activeLights.size() == 1 && lightAdds == 3 && lightRemoves == 2 && hook::field<unsigned>(sceneA, 4) == 1, "cell transition retained old scene lights");
    blocklights::update({1000, 0, 0}, true);
    check(activeLights.empty() && lightRemoves == 3, "distant light remained registered");
    blocklights::update({0, 0, 0}, true); blocklights::update({0, 0, 0}, false);
    check(activeLights.empty() && lightFrees == 4 && wrapperFrees == 4 && hook::field<unsigned>(sceneB, 4) == 1, "light lifecycle leaked native references");
    blocklights::clear(); current = nullptr;
}
void interactionTest() {
    interaction::Handoff ownership;
    auto t = ownership.update(0, true); check(!t.active && !t.entered && !t.left, "normal gameplay surrendered control");
    t = ownership.update(1, true); check(t.active && t.entered, "sit transition did not surrender control");
    for (unsigned s : {2u, 3u, 4u, 5u}) { t = ownership.update(s, true); check(t.active && !t.entered && !t.left, "chair sequence resumed Minecraft before the animation ended"); }
    t = ownership.update(0, true); check(!t.active && t.left, "standing did not request Minecraft resynchronization");
    for (unsigned s : {6u, 7u, 8u, 9u, 10u}) { t = ownership.update(s, true); check(t.active && !t.left, "sleep sequence lost native ownership"); }
    t = ownership.update(0, true); check(t.left, "waking did not restore Minecraft control");
    ownership.update(4, true); t = ownership.update(4, false); check(t.left && !t.active, "disconnect retained furniture ownership");
    ownership.update(4, true); ownership.clear(); t = ownership.update(0, true);
    check(!t.left && !t.active, "load/main menu retained a stale furniture transition");
    t = ownership.update(11, true); check(!t.active, "unknown furniture state seized control");
}
}
int main() {
    try {
        // Reserve engine addresses before opening shared memory.
        meshTest();
        stripTest();
        physicsTest(); lightTest(); interactionTest();
        dig::clear(); dig::setWorld(42);
        dug(-1, -1, -1, {4095}); check(dig::isDug(-1, -1, -1), "negative section indexing failed");
        auto generation = dig::generation(); dug(-1, -1, -1, {4095}); check(generation == dig::generation(), "identical packet dirtied terrain");
        dug(-1, -1, -1, {1}, 42, true); check(dig::isDug(-1, -1, -1), "truncated packet erased a hole");
        dug(-1, -1, -1, {}, 99); check(dig::isDug(-1, -1, -1), "foreign world packet changed terrain");
        dig::setWorld(43); check(!dig::any(), "world switch retained old holes"); dig::setWorld(42);
        // Triangle on the cube's exact upper face belongs to the dug cube beneath it.
        dug(0, 0, 0, {0}); std::vector<clip::Cube> cells;
        const float lo[3]{0, 1, 0}, hi[3]{1, 1, 1}; dig::collect(lo, hi, cells);
        check(cells.size() == 1, "boundary surface omitted the cube beneath it");
        Bridge bridge; const auto name = L"Local\\VegasCraft_gameplay_" + std::to_wstring(GetCurrentProcessId());
        check(bridge.open(name), "gameplay bridge failed");
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        auto* base = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 32u << 20));
        check(base != nullptr, "gameplay mapping failed");
        Collision::Geometry g{}; for (int i = 0; i < 3; ++i) { g.lo[i] = 0; g.hi[i] = 4; }
        const unsigned flags = proto::kTriTerrain | proto::kTriDiggable | (proto::kDigSand << proto::kTriMaterialShift);
        g.tris.push_back({{0, 0.5f, 0, 0, 0.5f, 4, 4, 0.5f, 0}, flags});
        dig::clear(); auto before = collision(base, bridge, g);
        check(close(area(before.tris, 0), 8) && occupied(before, 1, 0, 1), "baseline terrain collision failed");
        dug(0, 0, 0, {1 + 16}); auto after = collision(base, bridge, g);
        check(close(area(after.tris, 0), 7) && close(area(after.tris, proto::kTriGhost), 8), "cut/ghost triangle coverage failed");
        check(!occupied(after, 1, 0, 1) && occupied(after, 0, 0, 0), "dug voxels or surrounding terrain are wrong");
        for (const auto& tri : after.tris) check((tri.flags >> proto::kTriMaterialShift) == proto::kDigSand, "cut lost land material");
        Collision::Obb box{}; box.c[0] = box.c[2] = 1.5f; box.c[1] = 0.5f;
        for (int i = 0; i < 3; ++i) { box.axis[i][i] = 1; box.half[i] = 0.25f; } g.boxes.push_back(box);
        auto building = collision(base, bridge, g); check(occupied(building, 1, 0, 1), "digging removed a non-terrain building");
        dug(0, 0, 0, {}); g.boxes.clear(); auto filled = collision(base, bridge, g);
        check(close(area(filled.tris, 0), 8) && occupied(filled, 1, 0, 1) && close(area(filled.tris, proto::kTriGhost), 0), "filling did not restore collision");
        UnmapViewOfFile(base); CloseHandle(mapping); bridge.close();
        auto first = camera::orbit(10, 20, 30, 180, 0, 0, 4), behind = camera::orbit(10, 20, 30, 180, 0, 1, 4), front = camera::orbit(10, 20, 30, 180, 0, 2, 4);
        check(close(first.pos[1], -2100) && close(behind.pos[1], -2380) && close(front.pos[1], -1820), "F5 orbit position is wrong");
        check(close(behind.rot[1][0], 1) && close(front.rot[1][0], -1) && close(front.rot[2][1], 1), "front camera is mirrored incorrectly");
        for (unsigned m = 0; m < 3; ++m) {
            auto cam = camera::orbit(0, 0, 0, 25, 70, m, 4);
            for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
                float dot = 0; for (int i = 0; i < 3; ++i) dot += cam.rot[i][a] * cam.rot[i][b];
                check(close(dot, a == b ? 1.0f : 0.0f), "camera basis is not orthonormal");
            }
        }
        struct Packet { proto::RenLights hdr; proto::RenLight light[2]; } lights{{0, 0, 0, 2}, {{0, 0, 0, 15, 0x000000FF}, {1, 0, 0, 15, 0x0000FF00}}};
        blocklights::onLights(reinterpret_cast<const std::uint8_t*>(&lights), sizeof(lights)); auto points = blocklights::nearby({0, 0, 0});
        check(points.size() == 1 && close(points[0].x, 1) && close(points[0].r, 0.5f) && close(points[0].g, 0.5f), "light clustering/color failed");
        check(blocklights::nearby({1000, 0, 0}).empty(), "far lights were kept active");
        auto removed = lights.hdr; removed.count = 0; blocklights::onLights(reinterpret_cast<const std::uint8_t*>(&removed), sizeof(removed));
        check(blocklights::nearby({0, 0, 0}).empty(), "removed torch remained lit");
        std::puts("Gameplay fixtures passed: terrain contact filtering and land rescue, chair/sleep handoff, native light fields and lifetime, dig packets, reversible mesh/UVs, cut/ghost collision, buildings, F5");
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
