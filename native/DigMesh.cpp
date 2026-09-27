#include "DigMesh.h"
#include "Rtti.h"
#include "Dig.h"
#include "Game.h"
#include "Camera.h"
#include "SceneUtil.h"
#include "Log.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

// SkyCraft's Clip polygons and barycentric interpolation, adapted to Gamebryo's CPU-side
// NiTriShapeData and NiTriStripsData (New Vegas' landscape). Original data is retained and restored; replacement data has independent
// engine-allocated vertex/index arrays and renderer buffers. No save-game object is changed.
namespace vegas::digmesh {
namespace {
// NiAdditionalGeometryData (the landscape's per-vertex texture blend weights): streams of floats
// interleaved in data blocks. The replacement mesh shares the object, so its blocks are patched in
// place while a cut is active and restored afterwards.
struct Agd {
    void* object = nullptr;
    unsigned streams = 0, blocks = 0;
    struct Stream { unsigned char* desc; unsigned bytes, offset, block; } stream[4]{};
    struct Block { void* object; void* data; unsigned size, stride; void* fresh; std::size_t freshBytes; } block[4]{};
    unsigned count = 0;
};
struct Cut { void* original = nullptr; void* replacement = nullptr; std::uint64_t signature = 0; Agd agd; bool agdPatched = false; };
std::unordered_map<void*, Cut> cuts;
constexpr std::uintptr_t kNiTriShapeDataVtable = 0x109DAC4, kNiTriStripsDataVtable = 0x109D8AC;
void freeBytes(void* p, std::size_t bytes) { if (p) reinterpret_cast<void(__cdecl*)(void*, std::size_t)>(0xAA1460)(p, bytes); }
bool describe(void* data, unsigned count, Agd& a) {
    a = {};
    void* agd = hook::field<void*>(data, 0x30);
    if (!agd) return true;
    auto* streams = hook::field<unsigned char*>(agd, 0x14);
    auto** blocks = hook::field<void**>(agd, 0x20);
    const unsigned n = hook::field<unsigned>(agd, 0x10), used = hook::field<unsigned short>(agd, 0x26);
    if (hook::field<unsigned>(agd, 0xC) != count || !n || n > 4 || used > 16 || !hook::readable(streams, n * 0x1C) || !hook::readable(blocks, used * 4)) return false;
    a.object = agd; a.count = count; a.streams = n;
    for (unsigned i = 0; i < n; ++i) {
        unsigned char* d = streams + i * 0x1C;
        const auto unit = hook::field<unsigned>(d, 4), bytes = hook::field<unsigned>(d, 8), total = hook::field<unsigned>(d, 0xC);
        const auto stride = hook::field<unsigned>(d, 0x10), index = hook::field<unsigned>(d, 0x14), offset = hook::field<unsigned>(d, 0x18);
        if (unit != 4 || !bytes || bytes % 4 || total != count * bytes || index >= used || !blocks[index] || offset + bytes > stride) return false;
        void* block = blocks[index];
        unsigned slot = 0;
        while (slot < a.blocks && a.block[slot].object != block) ++slot;
        if (slot == a.blocks) {
            if (a.blocks == 4) return false;
            a.block[a.blocks++] = {block, hook::field<void*>(block, 8), hook::field<unsigned>(block, 4), stride, nullptr, 0};
        }
        auto& b = a.block[slot];
        if (b.stride != stride || b.size != count * stride || !hook::readable(b.data, b.size)) return false;
        a.stream[i] = {d, bytes, offset, slot};
    }
    return true;
}
void discard(Agd& a) {
    for (unsigned i = 0; i < a.blocks; ++i) { freeBytes(a.block[i].fresh, a.block[i].freshBytes); a.block[i].fresh = nullptr; }
}
// Floats of every stream blended with the barycentric weights of each output vertex.
template<class Corner> bool blendAgd(Agd& a, const std::vector<Corner>& out) {
    if (!a.object) return true;
    for (unsigned i = 0; i < a.blocks; ++i) {
        auto& b = a.block[i];
        b.freshBytes = out.size() * b.stride;
        b.fresh = sceneutil::allocate(std::max<std::size_t>(1, b.freshBytes));
        if (!b.fresh) { discard(a); return false; }
        std::memset(b.fresh, 0, b.freshBytes);
    }
    for (unsigned i = 0; i < a.streams; ++i) {
        const auto& s = a.stream[i];
        const auto& b = a.block[s.block];
        for (std::size_t v = 0; v < out.size(); ++v) for (unsigned k = 0; k < s.bytes / 4; ++k) {
            float value = 0;
            for (int c = 0; c < 3; ++c)
                value += out[v].v.b[c] * reinterpret_cast<const float*>(static_cast<unsigned char*>(b.data) + out[v].index[c] * b.stride + s.offset)[k];
            reinterpret_cast<float*>(static_cast<unsigned char*>(b.fresh) + v * b.stride + s.offset)[k] = value;
        }
    }
    return true;
}
void patchAgd(Agd& a, std::size_t vertices) {
    if (!a.object) return;
    hook::setField<unsigned>(a.object, 0xC, unsigned(vertices));
    for (unsigned i = 0; i < a.blocks; ++i) {
        hook::setField(a.block[i].object, 8, a.block[i].fresh);
        hook::setField<unsigned>(a.block[i].object, 4, unsigned(a.block[i].freshBytes));
    }
    for (unsigned i = 0; i < a.streams; ++i) hook::setField<unsigned>(a.stream[i].desc, 0xC, unsigned(vertices) * a.stream[i].bytes);
}
void unpatchAgd(Agd& a) {
    if (!a.object) return;
    hook::setField<unsigned>(a.object, 0xC, a.count);
    for (unsigned i = 0; i < a.blocks; ++i) {
        hook::setField(a.block[i].object, 8, a.block[i].data);
        hook::setField<unsigned>(a.block[i].object, 4, a.block[i].size);
    }
    for (unsigned i = 0; i < a.streams; ++i) hook::setField<unsigned>(a.stream[i].desc, 0xC, a.count * a.stream[i].bytes);
    discard(a);
}
std::uint64_t lastGeneration = ~0ull, lastScan = 0;
void replace(void* geom, void* data) {
    auto* old = hook::field<void*>(geom, 0xB8);
    if (old == data) return;
    sceneutil::retain(data);
    hook::setField(geom, 0xB8, data);
    sceneutil::release(old);
}
void restore(void* geom, Cut& c) {
    if (hook::field<void*>(geom, 0xB8) == c.replacement) replace(geom, c.original);
    if (c.agdPatched) unpatchAgd(c.agd);
    sceneutil::release(c.replacement); sceneutil::release(c.original); sceneutil::release(geom);
}
struct Corner { clip::Vert v; unsigned short index[3]; };
using Tri = std::array<unsigned short, 3>;
// Triangles of NiTriShapeData (index list) or NiTriStripsData (strips; degenerate triangles skipped,
// every second one flipped back to the shared winding).
bool triangles(void* data, bool strips, std::vector<Tri>& tris) {
    if (!strips) {
        const auto n = hook::field<unsigned short>(data, 0x40);
        const auto* idx = hook::field<const unsigned short*>(data, 0x48);
        if (!n || !hook::readable(idx, n * 6)) return false;
        for (unsigned i = 0; i < n; ++i) tris.push_back({idx[i * 3], idx[i * 3 + 1], idx[i * 3 + 2]});
        return true;
    }
    const auto strip = hook::field<unsigned short>(data, 0x44);
    const auto* lengths = hook::field<const unsigned short*>(data, 0x48);
    const auto* points = hook::field<const unsigned short*>(data, 0x4C);
    if (!strip || !hook::readable(lengths, strip * 2u)) return false;
    std::size_t total = 0;
    for (unsigned i = 0; i < strip; ++i) total += lengths[i];
    if (!hook::readable(points, total * 2)) return false;
    for (unsigned i = 0; i < strip; ++i) {
        for (unsigned k = 0; k + 2 < lengths[i]; ++k) {
            const unsigned short a = points[k], b = points[k + 1], c = points[k + 2];
            if (a == b || b == c || a == c) continue;
            tris.push_back((k & 1) ? Tri{a, c, b} : Tri{a, b, c});
        }
        points += lengths[i];
    }
    return !tris.empty();
}
void* build(void* data, bool strips, const camera::Transform& xf, const std::vector<clip::Cube>& cubes, Cut& cut) {
    const auto count = hook::field<unsigned short>(data, 8);
    const auto* verts = hook::field<const float*>(data, 0x20);
    std::vector<Tri> tris;
    if (!count || !hook::readable(verts, count * 12) || !triangles(data, strips, tris)) return nullptr;
    const auto flags = hook::field<unsigned short>(data, 0xC);
    const unsigned uvSets = flags & 63, normalSets = (flags & 0x1000) ? 3 : 1;
    if (uvSets > 8 || !describe(data, count, cut.agd)) {
        log::once("digmesh-agd", "digmesh: unsupported additional geometry data (%p); keeping the original mesh", hook::field<void*>(data, 0x30));
        return nullptr;
    }
    const auto boxes = clip::Merge(cubes);
    std::vector<Corner> out;
    bool changed = false;
    for (const auto& idx : tris) {
        float p[3][3];
        for (int v = 0; v < 3; ++v) {
            if (idx[v] >= count) return nullptr;
            float w[3];
            for (int k = 0; k < 3; ++k) {
                w[k] = xf.pos[k];
                for (int a = 0; a < 3; ++a) w[k] += xf.rot[k][a] * verts[idx[v] * 3 + a] * xf.scale;
            }
            p[v][0] = w[0] / 70; p[v][1] = w[2] / 70; p[v][2] = -w[1] / 70;
        }
        std::vector<clip::Poly> pieces;
        changed |= clip::Subtract(clip::FromTriangle(p[0], p[1], p[2]), boxes, pieces);
        for (const auto& poly : pieces) for (std::size_t v = 1; v + 1 < poly.size(); ++v)
            for (auto n : {std::size_t(0), v, v + 1}) {
                Corner c{poly[n], {idx[0], idx[1], idx[2]}};
                out.push_back(c);
            }
        // Both formats index with 16 bits (a strip also needs a length per triangle).
        if (out.size() > 65535) { log::once("digmesh-limit", "digmesh: a cut exceeds Gamebryo's 16-bit vertex limit; keeping its original mesh"); return nullptr; }
    }
    if (!changed) return nullptr;
    // NiTriShapeData::CreateObject / NiTriStripsData::CreateObject
    void* result = strips ? reinterpret_cast<void*(__cdecl*)()>(0xA75EC0)() : reinterpret_cast<void*(__cdecl*)()>(0xA7B790)();
    if (!result) return nullptr;
    sceneutil::retain(result);
    hook::setField<unsigned short>(result, 8, static_cast<unsigned short>(out.size()));
    hook::setField<unsigned short>(result, 0xC, flags);
    // Same keep/compress flags as the mesh it replaces, so the renderer packs it the same way.
    hook::setField<unsigned short>(result, 0xE, hook::field<unsigned short>(data, 0xE));
    // Keep the original local bound; cutting never grows the geometry.
    std::memcpy(static_cast<unsigned char*>(result) + 0x10, static_cast<unsigned char*>(data) + 0x10, 16);
    auto blend = [&](std::size_t offset, unsigned components, unsigned sets) {
        const float* src = hook::field<const float*>(data, offset);
        if (!src || !sets) return true;
        if (!hook::readable(src, std::size_t(count) * components * sets * 4)) return false;
        auto* dst = static_cast<float*>(sceneutil::allocate(std::max<std::size_t>(1, out.size() * components * sets) * 4));
        if (!dst) return false;
        hook::setField(result, offset, dst);
        for (unsigned s = 0; s < sets; ++s) for (std::size_t v = 0; v < out.size(); ++v)
            for (unsigned k = 0; k < components; ++k) {
                float value = 0;
                for (int a = 0; a < 3; ++a) value += out[v].v.b[a] * src[(s * count + out[v].index[a]) * components + k];
                dst[(s * out.size() + v) * components + k] = value;
            }
        return true;
    };
    if (!blend(0x20, 3, 1) || !blend(0x24, 3, normalSets) || !blend(0x28, 4, 1) || !blend(0x2C, 2, uvSets) || !blendAgd(cut.agd, out)) {
        sceneutil::release(result); return nullptr;
    }
    auto* idx = static_cast<unsigned short*>(sceneutil::allocate(std::max<std::size_t>(1, out.size()) * 2));
    unsigned short* lengths = nullptr;
    if (strips) lengths = static_cast<unsigned short*>(sceneutil::allocate(std::max<std::size_t>(1, out.size() / 3) * 2));
    if (!idx || (strips && !lengths)) { discard(cut.agd); sceneutil::release(result); return nullptr; }
    for (std::size_t i = 0; i < out.size(); ++i) idx[i] = static_cast<unsigned short>(i);
    const auto triangleCount = static_cast<unsigned short>(out.size() / 3);
    hook::setField<unsigned short>(result, 0x40, triangleCount);
    if (strips) {
        // One three-point strip per triangle: arbitrary cut triangles with no degenerates.
        for (unsigned i = 0; i < triangleCount; ++i) lengths[i] = 3;
        hook::setField<unsigned short>(result, 0x44, triangleCount);
        hook::setField(result, 0x48, lengths);
        hook::setField(result, 0x4C, idx);
    } else {
        hook::setField(result, 0x48, idx);
        hook::setField<unsigned>(result, 0x44, static_cast<unsigned>(out.size()));
    }
    if (cut.agd.object) { sceneutil::retain(cut.agd.object); hook::setField(result, 0x30, cut.agd.object); }
    return result;
}
void process(void* geom) {
    auto it = cuts.find(geom);
    void* data = it == cuts.end() ? hook::field<void*>(geom, 0xB8) : it->second.original;
    // Only the verified NiTriShapeData / NiTriStripsData layouts are supported, never skinned geometry.
    const auto vtable = data ? hook::field<std::uintptr_t>(data, 0) : 0;
    const bool strips = vtable == kNiTriStripsDataVtable;
    if (!data || (vtable != kNiTriShapeDataVtable && !strips) || hook::field<void*>(geom, 0xBC)) {
        log::once("digmesh-layout", "digmesh: landscape geometry %s data %p (%s) skin %p not cut", rtti::name(geom), data,
            data ? rtti::name(data) : "-", hook::field<void*>(geom, 0xBC));
        return;
    }
    const auto xf = hook::field<camera::Transform>(geom, 0x68);
    if (!std::isfinite(xf.scale) || xf.scale <= 0) return;
    const auto count = hook::field<unsigned short>(data, 8);
    const auto* verts = hook::field<const float*>(data, 0x20);
    if (!hook::readable(verts, count * 12)) return;
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (unsigned v = 0; v < count; ++v) {
        float w[3];
        for (int k = 0; k < 3; ++k) {
            w[k] = xf.pos[k];
            for (int a = 0; a < 3; ++a) w[k] += xf.rot[k][a] * verts[v * 3 + a] * xf.scale;
        }
        const float p[3] = {w[0] / 70, w[2] / 70, -w[1] / 70};
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    }
    std::vector<clip::Cube> cubes;
    dig::collect(lo, hi, cubes);
    std::sort(cubes.begin(), cubes.end());
    std::uint64_t signature = 1469598103934665603ull;
    for (const auto& c : cubes) for (int v : c) signature = (signature ^ std::uint32_t(v)) * 1099511628211ull;
    if (it != cuts.end() && signature == it->second.signature) return;
    if (it != cuts.end()) { restore(geom, it->second); cuts.erase(it); }
    if (cubes.empty()) return;
    Cut cut{data, nullptr, signature};
    if (void* replacement = build(data, strips, xf, cubes, cut)) {
        cut.replacement = replacement;
        sceneutil::retain(geom); sceneutil::retain(data);
        patchAgd(cut.agd, hook::field<unsigned short>(replacement, 8));
        cut.agdPatched = true;
        replace(geom, replacement);
        cuts.emplace(geom, cut);
        log::line("digmesh: cut %u landscape triangles into %u around %u dug cells", hook::field<unsigned short>(data, 0x40),
            hook::field<unsigned short>(replacement, 0x40), unsigned(cubes.size()));
    }
}
void visit(void* obj, std::unordered_set<void*>& seen, int depth = 0) {
    if (!obj || depth > 12 || !seen.insert(obj).second) return;
    auto** vt = hook::field<void**>(obj, 0);
    if (reinterpret_cast<void*(__thiscall*)(void*)>(vt[6])(obj)) { process(obj); return; }
    if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) return;
    auto** children = hook::field<void**>(obj, 0xA0);
    const auto count = hook::field<unsigned short>(obj, 0xA6);
    if (count > 4096 || !hook::readable(children, count * 4)) return;
    for (unsigned i = 0; i < count; ++i) visit(children[i], seen, depth + 1);
}
}
void updateGeometry(void* geometry) { if (geometry) process(geometry); }
void clear() { for (auto& [geom, c] : cuts) restore(geom, c); cuts.clear(); lastGeneration = ~0ull; lastScan = 0; }
void update() {
    if (!dig::any()) { if (!cuts.empty()) clear(); return; }
    const auto now = GetTickCount64(), gen = dig::generation();
    if (gen == lastGeneration && now - lastScan < 1000) return;
    lastGeneration = gen; lastScan = now;
    void* tes = game::tes();
    void* grid = tes ? hook::field<void*>(tes, 8) : nullptr;
    if (!grid) return;
    const auto n = hook::field<int>(grid, 0xC);
    auto** cells = hook::field<void**>(grid, 0x10);
    if (n < 1 || n > 21 || !hook::readable(cells, n * n * 4)) { log::once("digmesh-grid", "digmesh: grid %p size %d unusable", grid, n); return; }
    std::unordered_set<void*> seen;
    for (int i = 0; i < n * n; ++i) {
        void* land = cells[i] ? hook::field<void*>(cells[i], 0x4C) : nullptr;
        if (!land) continue;
        for (unsigned q = 0; q < 4; ++q)
            visit(reinterpret_cast<void*(__thiscall*)(void*, unsigned)>(0x535AF0)(land, q), seen);
    }
    log::once("digmesh-scan", "digmesh: first scan visited %u scene objects in a %dx%d grid", unsigned(seen.size()), n, n);
    for (auto it = cuts.begin(); it != cuts.end();) {
        if (!seen.contains(it->first)) { restore(it->first, it->second); it = cuts.erase(it); }
        else ++it;
    }
}
}
