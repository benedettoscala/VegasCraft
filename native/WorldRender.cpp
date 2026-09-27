#include "WorldRender.h"
#include "Game.h"
#include "Guard.h"
#include "Log.h"
#include "Profile.h"
#include "NpcBlocks.h"
#include "Dig.h"
#include "DigMesh.h"
#include "BlockLights.h"
#include "Camera.h"
#include "Arms.h"
#include "Runtime.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

// D3D9 counterpart of SkyCraft's WorldRender.cpp (MIT): Minecraft's own block meshes, entity
// models and particles, drawn in New Vegas' world pass with Minecraft's light levels and New
// Vegas' sun, ambient and fog.
namespace vegas::worldrender {
namespace {
constexpr char kShader[] = R"(
row_major float4x4 viewProj : register(c0);  // camera-relative New Vegas units -> clip
float4 offset : register(c4);                // camera-relative New Vegas position of the mesh origin
float4 sunDir : register(c5);                // towards the sun (New Vegas axes); w: daylight
float4 sunColor : register(c6);
float4 ambient : register(c7);
float4 fogParams : register(c8);             // start, 1/(end-start), max, on
float4 fogColor : register(c9);
float4 mode : register(c10);                 // x: camera-space hands, y: MC/NV FOV fit
float4 uvScale : register(c11);              // texture padded to a power of two: used share
float4 lightPos[4] : register(c12);          // nearby emitters, camera-relative New Vegas units; w: radius (0: unused)
float4 lightColor[4] : register(c16);
sampler2D atlas : register(s0);

struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; float4 lf : TEXCOORD1; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; float2 fogCut : TEXCOORD1; };

float Curve(float l) { return l / (4.0 - 3.0 * l); }  // Minecraft's light-level falloff
static const float3 kNormals[8] = {
    float3(0, 0, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0),
    float3(0, -1, 0), float3(-1, 0, 0), float3(1, 0, 0), float3(0, 0, 0) };

VSOut VSMain(VSIn i) {
    VSOut o;
    float3 rel = offset.xyz + float3(i.pos.x, -i.pos.z, i.pos.y) * 70.0;  // Minecraft -> New Vegas axes
    if (mode.x > 0.5) rel = float3(i.pos.x / (mode.y * 0.08), i.pos.y / (mode.y * 0.08), -i.pos.z / 0.08);
    o.pos = mul(float4(rel, 1.0), viewProj);
    o.uv = i.uv * uvScale.xy;
    float block = Curve(i.lf.x / 15.0);
    float sky = Curve(i.lf.y / 15.0);
    int ni = (int)(i.lf.z + 0.5);
    float3 n = kNormals[ni];
    bool hasNormal = ni > 0 && ni < 7;
    float sun = hasNormal ? saturate(dot(n, sunDir.xyz)) : 0.35 + 0.4 * saturate(sunDir.z);
    // Minecraft's fixed face shading keeps block faces readable under a flat sky.
    float face = !hasNormal ? 1.0 : (n.z > 0.5 ? 1.0 : (n.z < -0.5 ? 0.5 : (abs(n.y) > 0.5 ? 0.8 : 0.6)));
    float3 lit = ambient.rgb * lerp(0.35, 1.0, sky) + sunColor.rgb * (sun * sky * sky);
    // New Vegas' sky light is brighter than 1 at noon (ambient + sun about 1.3): scaled so a face
    // in full sun is as bright as in Minecraft, else daylight washes Minecraft's textures out.
    lit /= max(1.0, dot(ambient.rgb + sunColor.rgb, float3(0.3, 0.59, 0.11)));
    // Block light takes the colour of the emitters around the vertex (blue soul fire, red redstone, ...)
    // instead of one warm tone.
    float3 tint = 0.0;
    float tintWeight = 0.0;
    for (int k = 0; k < 4; ++k) {
        float a = saturate(1.0 - length(rel - lightPos[k].xyz) / max(lightPos[k].w, 1.0));
        a *= a;
        tint += lightColor[k].rgb * a;
        tintWeight += a;
    }
    float3 blockTone = lerp(float3(1.0, 0.85, 0.65), tint / max(tintWeight, 1e-4), saturate(tintWeight));
    lit = max(lit * lerp(face, 1.0, 0.35), block * blockTone);
    o.color = float4(i.color.rgb * lit, i.color.a);
    float dist = length(rel);
    o.fogCut.x = fogParams.w > 0.5 ? saturate((dist - fogParams.x) * fogParams.y) * fogParams.z : 0.0;
    o.fogCut.y = fmod(i.lf.w, 2.0);  // bit 0: cutout
    return o;
}
float4 PSMain(VSOut i) : COLOR0 {
    float4 c = tex2D(atlas, i.uv) * i.color;
    clip(c.a - (i.fogCut.y > 0.5 ? 0.1 : 0.004));
    c.rgb = lerp(c.rgb, fogColor.rgb, i.fogCut.x);
    return c;
}
)";

struct GpuVertex { float x, y, z; float u, v; D3DCOLOR color; std::uint8_t lf[4]; };
static_assert(sizeof(GpuVertex) == 28);

struct Section {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT solid = 0, translucent = 0;
    int sx, sy, sz;
};
struct Batch { std::uint32_t texture, first, count, flags; };
struct Mesh {
    std::vector<GpuVertex> vertices;
    std::vector<Batch> batches;
    double origin[3]{};
    bool valid = false;
};

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
IDirect3DDevice9* owner = nullptr;
IDirect3DVertexShader9* vs = nullptr;
IDirect3DPixelShader9* ps = nullptr;
IDirect3DVertexDeclaration9* decl = nullptr;
struct Tex {
    IDirect3DTexture9* t = nullptr;
    float su = 1, sv = 1;  // share of a power-of-two texture the image fills
    bool mips = false;     // the driver keeps its mip chain up to date (the atlas)
    void Release() { if (t) t->Release(); t = nullptr; }
};
Tex atlasTex;
IDirect3DTexture9*& atlas = atlasTex.t;
UINT atlasW = 0, atlasH = 0;
std::unordered_map<std::uint32_t, Tex> textures;
std::unordered_map<std::uint64_t, Section> sections;
Mesh scene, avatar, hands;
ULONGLONG handsAt = 0;
IDirect3DSurface9* handDepth = nullptr;
bool initFailed = false;
bool drawnThisFrame = false;
std::uint32_t passCalls = 0;
using RenderWorldFn = void(__thiscall*)(void*, void*, std::uint32_t, std::uint32_t, std::uint32_t);
RenderWorldFn originalRenderWorld = reinterpret_cast<RenderWorldFn>(game::kRenderWorldSceneGraph);

std::uint64_t key(int x, int y, int z) {
    return (std::uint64_t(std::uint32_t(x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(y) & 0x1FFFFF) << 21) | (std::uint32_t(z) & 0x1FFFFF);
}
template<class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
void releaseAll() {
    for (auto& [k, s] : sections) release(s.vb);
    sections.clear();
    for (auto& [id, t] : textures) t.Release();
    textures.clear();
    atlasTex.Release();
    release(vs); release(ps); release(decl);
    scene = {}; avatar = {}; hands = {};
    release(handDepth);
}
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
ResetFn originalReset = nullptr;
// Default-pool resources must go before New Vegas resets its device (resolution change,
// fullscreen Alt-Tab). Minecraft's textures come back when Minecraft resends them.
HRESULT __stdcall resetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    for (auto& [id, t] : textures) t.Release();
    textures.clear();
    atlasTex.Release();
    release(handDepth);
    log::line("worldrender: device reset; Minecraft textures released");
    return originalReset(device, params);
}
bool init(IDirect3DDevice9* device) {
    if (!originalReset) {
        auto** vtable = *reinterpret_cast<void***>(device);
        originalReset = reinterpret_cast<ResetFn>(hook::replaceSlot(&vtable[16], reinterpret_cast<void*>(&resetHook)));
    }
    if (owner == device && vs && ps && decl) return true;
    if (initFailed) return false;
    if (owner && owner != device) releaseAll();
    owner = device;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) { log::line("worldrender: d3dcompiler_47.dll unavailable; Minecraft blocks are not drawn"); initFailed = true; return false; }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    auto build = [&](const char* entry, const char* target) -> ID3DBlob* {
        code = nullptr; errors = nullptr;
        if (FAILED(compile(kShader, sizeof(kShader) - 1, "vegascraft_world", nullptr, nullptr, entry, target, 1 << 15 /* O3 */, 0, &code, &errors))) {
            log::line("worldrender: shader %s failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            release(errors);
            return nullptr;
        }
        release(errors);
        return code;
    };
    if (auto* blob = build("VSMain", "vs_3_0")) {
        device->CreateVertexShader(static_cast<const DWORD*>(blob->GetBufferPointer()), &vs);
        blob->Release();
    }
    if (auto* blob = build("PSMain", "ps_3_0")) {
        device->CreatePixelShader(static_cast<const DWORD*>(blob->GetBufferPointer()), &ps);
        blob->Release();
    }
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 24, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()};
    device->CreateVertexDeclaration(elements, &decl);
    const bool ok = vs && ps && decl;
    log::line("worldrender: %s", ok ? "ready" : "failed to initialize");
    initFailed = !ok;
    return ok;
}
GpuVertex convert(const proto::RenVertex& v) {
    GpuVertex g{};
    g.x = v.x; g.y = v.y; g.z = v.z; g.u = v.u; g.v = v.v;
    const std::uint32_t c = v.color;  // RGBA bytes, r lowest
    g.color = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
    g.lf[0] = static_cast<std::uint8_t>(v.light & 0xFF);
    g.lf[1] = static_cast<std::uint8_t>((v.light >> 8) & 0xFF);
    g.lf[2] = static_cast<std::uint8_t>((v.flags >> 4) & 7);
    g.lf[3] = static_cast<std::uint8_t>(v.flags & 3);
    return g;
}
UINT pow2(UINT v) { UINT p = 1; while (p < v) p <<= 1; return p; }
// Copies RGBA pixels into a default-pool texture through small system-memory strips: FalloutNV's
// 32-bit address space has no room for managed copies of a 20 MB atlas.
bool uploadRegion(IDirect3DDevice9* device, IDirect3DTexture9* dst, UINT x0, UINT y0, UINT w, UINT h, const std::uint8_t* rgba) {
    IDirect3DSurface9* target = nullptr;
    if (FAILED(dst->GetSurfaceLevel(0, &target)) || !target) return false;
    bool ok = true;
    const UINT strip = std::min<UINT>(h, std::max<UINT>(1, (1u << 20) / std::max<UINT>(w * 4, 1)));
    IDirect3DSurface9* staging = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(w, strip, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr)) || !staging) { target->Release(); return false; }
    for (UINT y = 0; y < h && ok; y += strip) {
        const UINT rows = std::min(strip, h - y);
        D3DLOCKED_RECT rect{};
        if (FAILED(staging->LockRect(&rect, nullptr, 0))) { ok = false; break; }
        for (UINT r = 0; r < rows; ++r) {
            const auto* in = reinterpret_cast<const std::uint32_t*>(rgba + (std::size_t(y + r) * w) * 4);
            auto* out = reinterpret_cast<std::uint32_t*>(static_cast<unsigned char*>(rect.pBits) + std::size_t(r) * rect.Pitch);
            for (UINT x = 0; x < w; ++x) { const auto p = in[x]; out[x] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu); }
        }
        staging->UnlockRect();
        RECT src{0, 0, static_cast<LONG>(w), static_cast<LONG>(rows)};
        POINT to{static_cast<LONG>(x0), static_cast<LONG>(y0 + y)};
        ok = SUCCEEDED(device->UpdateSurface(staging, &src, target, &to));
    }
    staging->Release();
    target->Release();
    return ok;
}
Tex createTexture(IDirect3DDevice9* device, UINT w, UINT h, const std::uint8_t* rgba, bool wantMips = false) {
    Tex out;
    IDirect3DTexture9* t = nullptr;
    HRESULT hr = E_FAIL;
    // Distant blocks shimmer without mip levels: let the driver build them from level 0 when it can.
    if (wantMips && SUCCEEDED(device->CreateTexture(w, h, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr)) && t) {
        out.mips = true;
        hr = S_OK;
    } else {
        t = nullptr;
        hr = device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr);
    }
    if (FAILED(hr) || !t) {
        // Some drivers refuse non-power-of-two sizes: pad, and scale the texture coordinates.
        const UINT pw = pow2(w), ph = pow2(h);
        t = nullptr;
        const HRESULT padded = device->CreateTexture(pw, ph, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr);
        log::line("worldrender: %ux%u texture failed (%08lX); padded to %ux%u: %08lX", w, h, static_cast<unsigned long>(hr), pw, ph, static_cast<unsigned long>(padded));
        if (FAILED(padded) || !t) return out;
        out.su = float(w) / float(pw);
        out.sv = float(h) / float(ph);
    }
    if (!uploadRegion(device, t, 0, 0, w, h, rgba)) {
        log::line("worldrender: %ux%u texture upload failed", w, h);
        t->Release();
        return out;
    }
    out.t = t;
    return out;
}
// Minecraft's entity models come without normals (normal index 7): each triangle gets the axis
// its face points along, so the sun and Minecraft's face shading light mobs and the avatar like
// blocks instead of one flat, too bright tone. Minecraft's model quads wind counter-clockwise
// seen from outside.
void addFaceNormals(Mesh& mesh) {
    for (const auto& b : mesh.batches)
        for (std::uint32_t t = b.first; t + 3 <= b.first + b.count; t += 3) {
            auto& a = mesh.vertices[t]; auto& v1 = mesh.vertices[t + 1]; auto& v2 = mesh.vertices[t + 2];
            if (a.lf[2] != 7) continue;
            const float ux = v1.x - a.x, uy = v1.y - a.y, uz = v1.z - a.z, wx = v2.x - a.x, wy = v2.y - a.y, wz = v2.z - a.z;
            // Minecraft axes; New Vegas' are x, -z, y.
            const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
            const float ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
            if (ax + ay + az < 1e-12f) continue;
            // kNormals: 1 down, 2 up, 3 +y (north in New Vegas = Minecraft -z), 4 -y, 5 -x, 6 +x.
            const std::uint8_t index = ay >= ax && ay >= az ? (ny > 0 ? 2 : 1) : az >= ax ? (nz < 0 ? 3 : 4) : (nx < 0 ? 5 : 6);
            a.lf[2] = v1.lf[2] = v2.lf[2] = index;
        }
}
void readMesh(Mesh& mesh, const std::uint8_t* data, std::uint32_t bytes, std::uint32_t batchCount, std::uint32_t vertexCount, std::size_t headerBytes, bool faceNormals) {
    mesh.batches.clear();
    mesh.vertices.clear();
    mesh.valid = false;
    const std::size_t need = headerBytes + std::size_t(batchCount) * sizeof(proto::RenBatch) + std::size_t(vertexCount) * sizeof(proto::RenVertex);
    if (bytes < need || batchCount > 100000 || vertexCount > 4000000) return;
    const auto* batches = reinterpret_cast<const proto::RenBatch*>(data + headerBytes);
    const auto* verts = reinterpret_cast<const proto::RenVertex*>(data + headerBytes + std::size_t(batchCount) * sizeof(proto::RenBatch));
    for (std::uint32_t i = 0; i < batchCount; ++i) {
        if (batches[i].first + batches[i].count > vertexCount) continue;
        mesh.batches.push_back({batches[i].texture, batches[i].first, batches[i].count, batches[i].flags});
    }
    mesh.vertices.reserve(vertexCount);
    for (std::uint32_t i = 0; i < vertexCount; ++i) mesh.vertices.push_back(convert(verts[i]));
    if (faceNormals) addFaceNormals(mesh);
    mesh.valid = true;
}
std::uint32_t messageCounts[16]{};
std::uint32_t drawnSections = 0, drawCalls = 0;
std::uint64_t lastStats = 0;
void onMessage(IDirect3DDevice9* device, std::uint32_t type, const std::uint8_t* data, std::uint32_t bytes) {
    ++messageCounts[type < 16 ? type : 15];
    switch (type) {
    case proto::kRenAtlas: {
        if (bytes < sizeof(proto::RenAtlas)) return;
        const auto* a = reinterpret_cast<const proto::RenAtlas*>(data);
        if (!a->width || !a->height || a->width > 16384 || a->height > 16384 || bytes < sizeof(*a) + std::size_t(a->width) * a->height * 4) return;
        atlasTex.Release();
        atlasTex = createTexture(device, a->width, a->height, data + sizeof(*a), true);
        atlasW = a->width; atlasH = a->height;
        log::line("worldrender: atlas %ux%u %s%s", a->width, a->height, atlas ? "loaded" : "failed", atlasTex.mips ? " (mip chain)" : "");
        break;
    }
    case proto::kRenAtlasRegion: {
        if (!atlas || bytes < sizeof(proto::RenAtlasRegion)) return;
        const auto* r = reinterpret_cast<const proto::RenAtlasRegion*>(data);
        if (r->x + r->width > atlasW || r->y + r->height > atlasH || bytes < sizeof(*r) + std::size_t(r->width) * r->height * 4) return;
        uploadRegion(device, atlas, r->x, r->y, r->width, r->height, data + sizeof(*r));
        break;
    }
    case proto::kRenSection: {
        if (bytes < sizeof(proto::RenSection)) return;
        const auto* s = reinterpret_cast<const proto::RenSection*>(data);
        const auto k = key(s->sx, s->sy, s->sz);
        auto it = sections.find(k);
        if (it != sections.end()) { release(it->second.vb); sections.erase(it); }
        if (!s->vertexCount || bytes < sizeof(*s) + std::size_t(s->vertexCount) * sizeof(proto::RenVertex)) return;
        const auto* verts = reinterpret_cast<const proto::RenVertex*>(data + sizeof(*s));
        Section section{nullptr, 0, 0, s->sx, s->sy, s->sz};
        const UINT total = s->vertexCount;
        if (FAILED(device->CreateVertexBuffer(total * sizeof(GpuVertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &section.vb, nullptr)) || !section.vb) return;
        GpuVertex* out = nullptr;
        if (FAILED(section.vb->Lock(0, 0, reinterpret_cast<void**>(&out), 0))) { release(section.vb); return; }
        // Solid and cutout triangles first, translucent ones after (they're drawn in a later pass).
        for (int pass = 0; pass < 2; ++pass)
            for (UINT t = 0; t + 2 < total; t += 3) {
                const bool translucent = verts[t].flags & 2;
                if (translucent != (pass == 1)) continue;
                for (int v = 0; v < 3; ++v) *out++ = convert(verts[t + v]);
                (pass ? section.translucent : section.solid) += 3;
            }
        section.vb->Unlock();
        sections.emplace(k, section);
        break;
    }
    case proto::kRenClearAll:
        hands = {}; handsAt = 0;
        for (auto& [k, s] : sections) release(s.vb);
        sections.clear();
        npcblocks::clear();
        dig::clear();
        digmesh::clear();
        blocklights::clear();
        break;
    case proto::kRenLights:
        blocklights::onLights(data, bytes);
        break;
    case proto::kRenDug:
        dig::onDug(data, bytes);
        break;
    case proto::kRenSolids:
        npcblocks::onSolids(data, bytes);
        break;
    case proto::kRenTexture: {
        if (bytes < sizeof(proto::RenTexture)) return;
        const auto* t = reinterpret_cast<const proto::RenTexture*>(data);
        if (!t->id || !t->width || !t->height || t->width > 8192 || t->height > 8192 || bytes < sizeof(*t) + std::size_t(t->width) * t->height * 4) return;
        auto& slot = textures[t->id];
        slot.Release();
        slot = createTexture(device, t->width, t->height, data + sizeof(*t));
        break;
    }
    case proto::kRenHands: {
        if (bytes < sizeof(proto::RenAvatar)) return;
        const auto* a = reinterpret_cast<const proto::RenAvatar*>(data);
        readMesh(hands, data, bytes, a->batchCount, a->vertexCount, sizeof(*a), false);
        handsAt = GetTickCount64();
        break;
    }
    case proto::kRenAvatar: {
        if (bytes < sizeof(proto::RenAvatar)) return;
        const auto* a = reinterpret_cast<const proto::RenAvatar*>(data);
        readMesh(avatar, data, bytes, a->batchCount, a->vertexCount, sizeof(*a), true);
        break;
    }
    case proto::kRenScene: {
        if (bytes < sizeof(proto::RenScene)) return;
        const auto* s = reinterpret_cast<const proto::RenScene*>(data);
        readMesh(scene, data, bytes, s->batchCount, s->vertexCount, sizeof(*s), true);
        scene.origin[0] = s->originX; scene.origin[1] = s->originY; scene.origin[2] = s->originZ;
        break;
    }
    default:
        break;  // lights, NPC solids, dug cells and ragdolls: see BlockLights / NpcBlocks
    }
}
void drain(IDirect3DDevice9* device) {
    auto* link = state().bridge;
    if (!link || !init(device)) return;
    link->drainRender([&](std::uint32_t type, const std::uint8_t* data, std::uint32_t bytes) { onMessage(device, type, data, bytes); }, 48ull << 20);
    if (!link->renderRingMapped()) log::once("render-map", "worldrender: cannot map the render ring (Windows error %lu); 32-bit address space is fragmented", GetLastError());
}

struct Camera { float rot[3][3]; float pos[3]; float scale; };
struct Frustum { float l, r, t, b, n, f; std::uint8_t ortho, pad[3]; };
bool readCamera(Camera& cam, Frustum& fr) {
    void* scene = nullptr; void* camera = nullptr;
    return hook::safeRead(reinterpret_cast<void*>(game::kSceneGraphSlot), scene) && scene &&
        hook::safeRead(static_cast<unsigned char*>(scene) + 0xAC, camera) && camera &&
        hook::safeRead(static_cast<unsigned char*>(camera) + 0x68, cam) &&
        hook::safeRead(static_cast<unsigned char*>(camera) + 0xDC, fr) &&
        std::isfinite(cam.pos[0]) && fr.r > fr.l && fr.t > fr.b && fr.n > 0 && fr.f > fr.n;
}
D3DMATRIX viewProjection(const Camera& c, const Frustum& f) {
    // View (camera at the origin): Gamebryo camera columns are forward, up, right.
    float view[4][4]{};
    for (int i = 0; i < 3; ++i) {
        view[i][0] = c.rot[i][2];
        view[i][1] = c.rot[i][1];
        view[i][2] = c.rot[i][0];
    }
    view[3][3] = 1;
    float proj[4][4]{};
    proj[0][0] = 2 / (f.r - f.l);
    proj[1][1] = 2 / (f.t - f.b);
    proj[2][0] = -(f.r + f.l) / (f.r - f.l);
    proj[2][1] = -(f.t + f.b) / (f.t - f.b);
    proj[2][2] = f.f / (f.f - f.n);
    proj[2][3] = 1;
    proj[3][2] = -f.n * proj[2][2];
    D3DMATRIX out{};
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += view[r][k] * proj[k][col];
            out.m[r][col] = s;
        }
    return out;
}
struct Environment { float sunDir[4], sunColor[4], amb[4], fog[4], fogColor[4]; };
Environment readEnvironment() {
    Environment e{{0.3f, 0.3f, 0.9f, 1.0f}, {1.0f, 0.95f, 0.85f, 0}, {0.45f, 0.45f, 0.5f, 0}, {0, 0, 0, 0}, {0.7f, 0.65f, 0.55f, 0}};
    auto& sunDir = e.sunDir; auto& sunColor = e.sunColor; auto& amb = e.amb; auto& fog = e.fog; auto& fogColor = e.fogColor;
    guard::run([&] {
        void* sky = *reinterpret_cast<void**>(0x011DEA20);
        if (!sky) return;
        const auto ambient = hook::field<game::NiPoint3>(sky, 0x60);
        const auto directional = hook::field<game::NiPoint3>(sky, 0x6C);
        const auto fogC = hook::field<game::NiPoint3>(sky, 0xC0);
        amb[0] = ambient.x; amb[1] = ambient.y; amb[2] = ambient.z;
        sunColor[0] = directional.x; sunColor[1] = directional.y; sunColor[2] = directional.z;
        fogColor[0] = fogC.x; fogColor[1] = fogC.y; fogColor[2] = fogC.z;
        void* sun = hook::field<void*>(sky, 0x28);
        void* root = sun ? hook::field<void*>(sun, 0x04) : nullptr;
        if (root) {
            const auto p = hook::field<game::NiPoint3>(root, 0x58);
            const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            if (len > 1e-3f) { sunDir[0] = p.x / len; sunDir[1] = p.y / len; sunDir[2] = p.z / len; }
        }
        fog[0] = 4000.0f; fog[1] = 1.0f / 90000.0f; fog[2] = 0.6f; fog[3] = 1.0f;
    });
    return e;
}
void environment(IDirect3DDevice9* device) {
    const Environment e = readEnvironment();
    device->SetVertexShaderConstantF(5, e.sunDir, 1);
    device->SetVertexShaderConstantF(6, e.sunColor, 1);
    device->SetVertexShaderConstantF(7, e.amb, 1);
    device->SetVertexShaderConstantF(8, e.fog, 1);
    device->SetPixelShaderConstantF(9, e.fogColor, 1);
}
// The first-person hands live in camera space (no world sun direction applies), so they take New Vegas'
// ambient plus part of its sun as one flat light: bright at noon, dim at dusk, dark at night, like the
// native weapon beside them. A small floor keeps them from disappearing in a pitch-black interior.
void setLights(IDirect3DDevice9* device, const Camera* cam);
void handsEnvironment(IDirect3DDevice9* device) {
    const Environment e = readEnvironment();
    constexpr float kSunShare = 0.6f, kFloor = 0.12f;
    float lit[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) lit[i] = std::clamp(e.amb[i] + e.sunColor[i] * kSunShare, kFloor, 4.0f);
    const float zero[4]{};
    device->SetVertexShaderConstantF(5, zero, 1);
    device->SetVertexShaderConstantF(6, zero, 1);
    device->SetVertexShaderConstantF(7, lit, 1);
    device->SetVertexShaderConstantF(8, zero, 1);
    setLights(device, nullptr);
}
void setOffset(IDirect3DDevice9* device, double mx, double my, double mz, const Camera& cam) {
    const float o[4] = {static_cast<float>(mx * 70.0 - cam.pos[0]), static_cast<float>(-mz * 70.0 - cam.pos[1]), static_cast<float>(my * 70.0 - cam.pos[2]), 0};
    device->SetVertexShaderConstantF(4, o, 1);
}
// The four strongest emitters near the player for the block-light tint, camera-relative; all zero
// (no emitter) for the camera-space hands.
void setLights(IDirect3DDevice9* device, const Camera* cam) {
    float pos[4][4]{}, color[4][4]{};
    if (cam) {
        const auto& points = blocklights::active();
        for (std::size_t i = 0; i < 4 && i < points.size(); ++i) {
            const auto& p = points[i];
            pos[i][0] = p.x * 70.0f - cam->pos[0]; pos[i][1] = -p.z * 70.0f - cam->pos[1]; pos[i][2] = p.y * 70.0f - cam->pos[2];
            pos[i][3] = p.radius;
            color[i][0] = p.r; color[i][1] = p.g; color[i][2] = p.b;
        }
    }
    device->SetVertexShaderConstantF(12, &pos[0][0], 4);
    device->SetVertexShaderConstantF(16, &color[0][0], 4);
}
void drawMesh(IDirect3DDevice9* device, const Mesh& mesh, double ox, double oy, double oz, const Camera& cam, bool translucentPass) {
    if (!mesh.valid || mesh.vertices.empty()) return;
    setOffset(device, ox, oy, oz, cam);
    for (const auto& b : mesh.batches) {
        if (((b.flags & 1) != 0) != translucentPass || b.count < 3) continue;
        const Tex* tex = b.texture == 0 ? &atlasTex : nullptr;
        if (b.texture) { auto it = textures.find(b.texture); if (it != textures.end()) tex = &it->second; }
        if (!tex || !tex->t) continue;
        const float scale[4] = {tex->su, tex->sv, 0, 0};
        device->SetVertexShaderConstantF(11, scale, 1);
        device->SetTexture(0, tex->t);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, tex->mips ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, b.count / 3, mesh.vertices.data() + b.first, sizeof(GpuVertex));
    }
}
// Minecraft's arrows, dropped items and blocks, and block-breaking cracks (SkyCraft's
// WorldEntities): built on the CPU each frame around a local origin near the camera.
void drawEntities(IDirect3DDevice9* device, const proto::WorldEntities& we, const Camera& cam) {
    if (!atlas || !we.count) return;
    const double ox = std::floor(cam.pos[0] / 70.0), oy = std::floor(cam.pos[2] / 70.0), oz = std::floor(-cam.pos[1] / 70.0);
    static std::vector<GpuVertex> solid, blended;
    solid.clear(); blended.clear();
    auto vertex = [&](std::vector<GpuVertex>& out, float x, float y, float z, float u, float v, D3DCOLOR color) {
        GpuVertex g{};
        g.x = static_cast<float>(x - ox); g.y = static_cast<float>(y - oy); g.z = static_cast<float>(z - oz);
        g.u = u; g.v = v; g.color = color;
        g.lf[0] = 0; g.lf[1] = 15; g.lf[2] = 0; g.lf[3] = 1;  // sky-lit, no face normal, cutout
        out.push_back(g);
    };
    // A quad a-b-c-d (two triangles) with uv rect {u0, v0, u1, v1}.
    auto quad = [&](std::vector<GpuVertex>& out, const float* a, const float* b, const float* c, const float* d, const float* uv, D3DCOLOR color) {
        vertex(out, a[0], a[1], a[2], uv[0], uv[3], color); vertex(out, b[0], b[1], b[2], uv[2], uv[3], color); vertex(out, c[0], c[1], c[2], uv[2], uv[1], color);
        vertex(out, a[0], a[1], a[2], uv[0], uv[3], color); vertex(out, c[0], c[1], c[2], uv[2], uv[1], color); vertex(out, d[0], d[1], d[2], uv[0], uv[1], color);
    };
    auto rgba = [](std::uint32_t c) -> D3DCOLOR { return c ? (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu) : 0xFFFFFFFFu; };
    for (std::uint32_t i = 0; i < we.count; ++i) {
        const auto& e = we.entities[i];
        const float yaw = e.yaw * 0.0174532925f, pitch = e.pitch * 0.0174532925f;
        const float rx = std::cos(yaw), rz = std::sin(yaw);  // right, about the vertical
        switch (e.kind) {
        case proto::kWeItem: {
            const float h = e.scale * 0.5f;
            const float a[3] = {e.x - rx * h, e.y - h, e.z - rz * h}, b[3] = {e.x + rx * h, e.y - h, e.z + rz * h};
            const float c[3] = {e.x + rx * h, e.y + h, e.z + rz * h}, d[3] = {e.x - rx * h, e.y + h, e.z - rz * h};
            quad(solid, a, b, c, d, e.uv[0], 0xFFFFFFFFu);
            break;
        }
        case proto::kWeBlock: {
            const float h = e.scale * 0.5f;
            float p[8][3];
            for (int k = 0; k < 8; ++k) {
                const float lx = (k & 1) ? h : -h, ly = (k & 2) ? h : -h, lz = (k & 4) ? h : -h;
                p[k][0] = e.x + lx * rx - lz * rz; p[k][1] = e.y + ly; p[k][2] = e.z + lx * rz + lz * rx;
            }
            const D3DCOLOR side = 0xFFC8C8C8u, top = rgba(e.tint);
            quad(solid, p[0], p[1], p[3], p[2], e.uv[0], side);
            quad(solid, p[5], p[4], p[6], p[7], e.uv[0], side);
            quad(solid, p[4], p[0], p[2], p[6], e.uv[0], side);
            quad(solid, p[1], p[5], p[7], p[3], e.uv[0], side);
            quad(solid, p[2], p[3], p[7], p[6], e.uv[1], top);
            quad(solid, p[4], p[5], p[1], p[0], e.uv[2], 0xFF909090u);
            break;
        }
        case proto::kWeArrow:
        case proto::kWeTrident: {
            // Two crossed quads along the flight direction.
            const float len = e.kind == proto::kWeTrident ? 1.2f : 0.5f, w = 0.08f;
            const float fx = -std::sin(yaw) * std::cos(pitch), fy = -std::sin(pitch), fz = std::cos(yaw) * std::cos(pitch);
            const float tail[3] = {e.x - fx * len, e.y - fy * len, e.z - fz * len};
            const float side1[3] = {rx * w, 0, rz * w}, side2[3] = {0, w, 0};
            for (const float* sd : {side1, side2}) {
                const float a[3] = {tail[0] - sd[0], tail[1] - sd[1], tail[2] - sd[2]}, b[3] = {e.x - sd[0], e.y - sd[1], e.z - sd[2]};
                const float c[3] = {e.x + sd[0], e.y + sd[1], e.z + sd[2]}, d[3] = {tail[0] + sd[0], tail[1] + sd[1], tail[2] + sd[2]};
                quad(solid, a, b, c, d, e.uv[0], 0xFFFFFFFFu);
            }
            break;
        }
        case proto::kWeCrack: {
            const float x0 = e.x, y0 = e.y, z0 = e.z, x1 = e.x + e.ext[0], y1 = e.y + e.ext[1], z1 = e.z + e.ext[2];
            const float c[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1}};
            const D3DCOLOR tone = 0x99FFFFFFu;
            quad(blended, c[0], c[1], c[3], c[2], e.uv[0], tone);
            quad(blended, c[5], c[4], c[6], c[7], e.uv[0], tone);
            quad(blended, c[4], c[0], c[2], c[6], e.uv[0], tone);
            quad(blended, c[1], c[5], c[7], c[3], e.uv[0], tone);
            quad(blended, c[2], c[3], c[7], c[6], e.uv[0], tone);
            quad(blended, c[4], c[5], c[1], c[0], e.uv[0], tone);
            break;
        }
        default:
            break;
        }
    }
    setOffset(device, ox, oy, oz, cam);
    const float scale[4] = {atlasTex.su, atlasTex.sv, 0, 0};
    device->SetVertexShaderConstantF(11, scale, 1);
    device->SetTexture(0, atlas);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, atlasTex.mips ? D3DTEXF_LINEAR : D3DTEXF_NONE);
    if (!solid.empty()) {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(solid.size() / 3), solid.data(), sizeof(GpuVertex));
    }
    if (!blended.empty()) {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);  // Minecraft's crumbling blend
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(blended.size() / 3), blended.data(), sizeof(GpuVertex));
    }
}
void drawOutline(IDirect3DDevice9* device, const D3DMATRIX& viewProj, const Camera& cam, const proto::WorldEntities& we) {
    if (!we.hasSelection) return;
    struct V { float x, y, z; D3DCOLOR c; };
    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i) { lo[i] = we.selMin[i] - 0.002f; hi[i] = we.selMax[i] + 0.002f; }
    auto p = [&](int i) {
        const float mx = (i & 1) ? hi[0] : lo[0], my = (i & 2) ? hi[1] : lo[1], mz = (i & 4) ? hi[2] : lo[2];
        return V{mx * 70.0f - cam.pos[0], -mz * 70.0f - cam.pos[1], my * 70.0f - cam.pos[2], 0x66000000};
    };
    static constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    V lines[24];
    for (int e = 0; e < 12; ++e) { lines[e * 2] = p(edges[e][0]); lines[e * 2 + 1] = p(edges[e][1]); }
    D3DMATRIX identity{};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1;
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &viewProj);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->DrawPrimitiveUP(D3DPT_LINELIST, 12, lines, sizeof(V));
}
// Frustum culling of Minecraft's 16-block sections: a bounding sphere against the view-projection's
// side planes (the far plane is left to the fog), so sections behind the player or off to the side
// cost no draw call. Row-vector convention: clip = v * M.
struct Planes { float p[5][4]; };
Planes frustumPlanes(const D3DMATRIX& m) {
    Planes out{};
    auto col = [&](int c, float v[4]) { for (int r = 0; r < 4; ++r) v[r] = m.m[r][c]; };
    float c0[4], c1[4], c2[4], c3[4];
    col(0, c0); col(1, c1); col(2, c2); col(3, c3);
    for (int i = 0; i < 4; ++i) {
        out.p[0][i] = c3[i] + c0[i];  // left
        out.p[1][i] = c3[i] - c0[i];  // right
        out.p[2][i] = c3[i] + c1[i];  // bottom
        out.p[3][i] = c3[i] - c1[i];  // top
        out.p[4][i] = c2[i];          // near
    }
    for (auto& pl : out.p) {
        const float len = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
        if (len > 1e-6f) for (float& v : pl) v /= len;
    }
    return out;
}
// A section's centre is 8 blocks in from its corner on every axis; its half diagonal is about 970
// units, 1100 with a margin.
bool sectionVisible(const Planes& planes, const Section& s, const Camera& cam) {
    const float cx = float(s.sx * 16.0 * 70.0 - cam.pos[0]) + 560.0f;
    const float cy = float(-s.sz * 16.0 * 70.0 - cam.pos[1]) - 560.0f;
    const float cz = float(s.sy * 16.0 * 70.0 - cam.pos[2]) + 560.0f;
    for (const auto& pl : planes.p)
        if (pl[0] * cx + pl[1] * cy + pl[2] * cz + pl[3] < -1100.0f) return false;
    return true;
}
// A soft dark disc under the avatar (a blob shadow): New Vegas draws none for Minecraft's player. It fades
// with the sun, so at night it hardly shows.
void drawAvatarShadow(IDirect3DDevice9* device, const D3DMATRIX& viewProj, const Camera& cam) {
    auto& st = state();
    const Environment e = readEnvironment();
    const float sunLum = e.sunColor[0] * 0.3f + e.sunColor[1] * 0.59f + e.sunColor[2] * 0.11f;
    const float strength = std::clamp(sunLum * 2.0f, 0.25f, 1.0f);
    struct V { float x, y, z; D3DCOLOR c; };
    constexpr int kSegments = 20;
    constexpr float kRadius = 0.55f * 70.0f;
    V fan[kSegments + 2];
    const float cx = float(st.feetX * 70.0 - cam.pos[0]), cy = float(-st.feetZ * 70.0 - cam.pos[1]), cz = float(st.feetY * 70.0 - cam.pos[2]) + 1.5f;
    const D3DCOLOR centre = (D3DCOLOR(0x66 * strength) << 24);
    fan[0] = {cx, cy, cz, centre};
    for (int i = 0; i <= kSegments; ++i) {
        const float a = 6.2831853f * float(i) / float(kSegments);
        fan[i + 1] = {cx + std::cos(a) * kRadius, cy + std::sin(a) * kRadius, cz, 0x00000000};
    }
    D3DMATRIX identity{};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1;
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetTransform(D3DTS_VIEW, &identity);
    device->SetTransform(D3DTS_PROJECTION, &viewProj);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, kSegments, fan, sizeof(V));
    device->SetVertexShader(vs);
    device->SetPixelShader(ps);
    device->SetVertexDeclaration(decl);
}
void draw(IDirect3DDevice9* device) {
    auto& st = state();
    if (!st.bridge || !st.mcInWorld || !vs || !ps || !decl) return;
    Camera cam{};
    Frustum fr{};
    if (!readCamera(cam, fr)) return;
    const D3DMATRIX viewProj = viewProjection(cam, fr);
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) return;
    device->SetVertexShader(vs);
    device->SetPixelShader(ps);
    device->SetVertexDeclaration(decl);
    device->SetVertexShaderConstantF(0, &viewProj.m[0][0], 4);
    const float worldMode[4]{};
    device->SetVertexShaderConstantF(10, worldMode, 1);
    environment(device);
    setLights(device, &cam);
    device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);  // most detailed level allowed: full resolution up close
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, atlasTex.mips ? D3DTEXF_LINEAR : D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    const float atlasScale[4] = {atlasTex.su, atlasTex.sv, 0, 0};
    device->SetVertexShaderConstantF(11, atlasScale, 1);
    const Planes planes = frustumPlanes(viewProj);
    // Solid and cutout geometry.
    if (atlas) {
        device->SetTexture(0, atlas);
        for (const auto& [k, s] : sections) {
            if (!s.solid || !sectionVisible(planes, s, cam)) continue;
            setOffset(device, s.sx * 16.0, s.sy * 16.0, s.sz * 16.0, cam);
            device->SetStreamSource(0, s.vb, 0, sizeof(GpuVertex));
            device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s.solid / 3);
            ++drawnSections;
        }
    }
    drawMesh(device, scene, scene.origin[0], scene.origin[1], scene.origin[2], cam, false);
    if (st.feetValid) drawMesh(device, avatar, st.feetX, st.feetY, st.feetZ, cam, false);
    if (st.feetValid && avatar.valid && !avatar.vertices.empty()) drawAvatarShadow(device, viewProj, cam);
    // Translucent geometry (water, stained glass, particles): blended, no depth writes.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetVertexShaderConstantF(11, atlasScale, 1);
    if (atlas) {
        device->SetTexture(0, atlas);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, atlasTex.mips ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        for (const auto& [k, s] : sections) {
            if (!s.translucent || !sectionVisible(planes, s, cam)) continue;
            setOffset(device, s.sx * 16.0, s.sy * 16.0, s.sz * 16.0, cam);
            device->SetStreamSource(0, s.vb, 0, sizeof(GpuVertex));
            device->DrawPrimitive(D3DPT_TRIANGLELIST, s.solid, s.translucent / 3);
        }
    }
    drawMesh(device, scene, scene.origin[0], scene.origin[1], scene.origin[2], cam, true);
    if (st.feetValid) drawMesh(device, avatar, st.feetX, st.feetY, st.feetZ, cam, true);
    static proto::WorldEntities we{};
    if (st.bridge->readWorldEntities(we)) {
        drawEntities(device, we, cam);
        drawOutline(device, viewProj, cam, we);
    }
    saved->Apply();
    saved->Release();
}
// Render the exported Minecraft hands against the native weapon's actual rigid triangles.
// An independent depth surface keeps this pass separate from world/HUD depth clears.
void drawHands(IDirect3DDevice9* device) {
    auto& st = state();
    void* player = game::player();
    if (!st.puppeting || !st.nvWeapon || st.nvMenuOpen || st.showNvArms || !player
        || hook::field<std::uint8_t>(player, game::kPlayerIsThirdPerson) || !hands.valid
        || hands.vertices.empty() || GetTickCount64() - handsAt > 500 || !vs || !ps || !decl) return;
    static std::vector<float> weapon;
    if (!arms::weaponDepth(weapon))
        log::once("hands-depth-unavailable", "hands: no weapon mesh found for occlusion; keeping hands visible");
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &target)) || !target) return;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc); target->Release();
    if (handDepth) {
        D3DSURFACE_DESC old{}; handDepth->GetDesc(&old);
        if (old.Width != desc.Width || old.Height != desc.Height || old.MultiSampleType != desc.MultiSampleType
            || old.MultiSampleQuality != desc.MultiSampleQuality) release(handDepth);
    }
    if (!handDepth && FAILED(device->CreateDepthStencilSurface(desc.Width, desc.Height, D3DFMT_D24S8,
        desc.MultiSampleType, desc.MultiSampleQuality, TRUE, &handDepth, nullptr))) return;
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) return;
    IDirect3DSurface9* previousDepth = nullptr;
    IDirect3DSurface9* previousTarget = nullptr;
    device->GetDepthStencilSurface(&previousDepth);
    device->GetRenderTarget(0, &previousTarget);
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &target);
    device->SetDepthStencilSurface(nullptr);
    HRESULT targetResult = target ? device->SetRenderTarget(0, target) : E_FAIL;
    release(target);
    if (FAILED(targetResult) || FAILED(device->SetDepthStencilSurface(handDepth))) {
        if (previousTarget) device->SetRenderTarget(0, previousTarget);
        device->SetDepthStencilSurface(previousDepth);
        release(previousTarget); release(previousDepth); saved->Apply(); release(saved); return;
    }
    D3DVIEWPORT9 vp{0, 0, desc.Width, desc.Height, 0, 1}; device->SetViewport(&vp);
    device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    const float aspect = float(desc.Width) / float(desc.Height);
    constexpr float tanX = 1.46f;
    const float fit = std::tan(35.0f * 0.0174532925f) / (tanX / aspect);
    Camera c{}; c.rot[0][2] = c.rot[1][1] = c.rot[2][0] = 1;
    Frustum f{-tanX, tanX, tanX / aspect, -tanX / aspect, 0.1f, 10000.0f, 0, {}};
    const auto projection = viewProjection(c, f);
    device->SetVertexShader(vs); device->SetPixelShader(ps); device->SetVertexDeclaration(decl);
    device->SetVertexShaderConstantF(0, &projection.m[0][0], 4);
    const float mode[4] = {1, fit, 0, 0}, zero[4]{};
    device->SetVertexShaderConstantF(4, zero, 1);
    handsEnvironment(device);
    device->SetVertexShaderConstantF(10, mode, 1);
    device->SetRenderState(D3DRS_ZENABLE, TRUE); device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_DEPTHBIAS, 0); device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE); device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE); device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    // Depth-only weapon prepass, no texture or alpha discard.
    device->SetPixelShader(nullptr); device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    static std::vector<GpuVertex> depthVertices;
    depthVertices.resize(weapon.size() / 3);
    for (std::size_t i = 0; i < depthVertices.size(); ++i) {
        auto& v = depthVertices[i]; v = {};
        v.x = weapon[i * 3] * fit * 0.08f; v.y = weapon[i * 3 + 1] * fit * 0.08f; v.z = -weapon[i * 3 + 2] * 0.08f;
    }
    if (!depthVertices.empty())
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(depthVertices.size() / 3), depthVertices.data(), sizeof(GpuVertex));
    device->SetPixelShader(ps); device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    drawMesh(device, hands, 0, 0, 0, c, false);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    drawMesh(device, hands, 0, 0, 0, c, true);
    device->SetDepthStencilSurface(nullptr);
    if (previousTarget) device->SetRenderTarget(0, previousTarget);
    device->SetDepthStencilSurface(previousDepth); release(previousDepth); release(previousTarget);
    saved->Apply(); release(saved);
}

IDirect3DDevice9* device() {
    void* renderer = nullptr;
    IDirect3DDevice9* d = nullptr;
    if (!hook::safeRead(reinterpret_cast<void*>(game::kRendererSlot), renderer) || !renderer) return nullptr;
    hook::safeRead(static_cast<unsigned char*>(renderer) + game::kRendererDevice, d);
    return d;
}
void __fastcall renderWorld(void* main, void*, void* sun, std::uint32_t firstPerson, std::uint32_t arg2, std::uint32_t arg3) {
    if (!drawnThisFrame) {
        if (auto* d = device()) { profile::Scope t(profile::kDrain); guard::run([&] { drain(d); }); }
        profile::Scope t(profile::kPreRender);
        guard::run([&] {
            camera::apply();
            { profile::Scope t(profile::kApplyHeld); arms::applyHeld(); }
            digmesh::update();
            const auto& st = state();
            blocklights::update({st.feetX, st.feetY, st.feetZ}, st.nvInGame && st.mcInWorld);
        });
    }
    { profile::Scope t(profile::kNvWorld); originalRenderWorld(main, sun, firstPerson, arg2, arg3); }
    const auto site = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - 5;
    if (passCalls < 6) {
        ++passCalls;
        log::line("worldrender: world pass from %08X (%u, %u, %u)", static_cast<unsigned>(site), firstPerson & 0xFF, arg2 & 0xFF, arg3 & 0xFF);
    }
    // Only one call site runs per frame in game (its first argument is 1 there); draw once.
    if (drawnThisFrame) return;
    const auto now = GetTickCount64();
    if (now - lastStats > 5000) {
        lastStats = now;
        log::line("worldrender: %u sections, atlas %s, %u draws (%u section draws); messages atlas=%u region=%u section=%u clear=%u texture=%u avatar=%u scene=%u lights=%u solids=%u dug=%u",
            static_cast<unsigned>(sections.size()), atlas ? "yes" : "no", drawCalls, drawnSections, messageCounts[1], messageCounts[7], messageCounts[2], messageCounts[3],
            messageCounts[4], messageCounts[5], messageCounts[6], messageCounts[8], messageCounts[10], messageCounts[11]);
        drawCalls = drawnSections = 0;
    }
    ++drawCalls;
    if (auto* d = device()) {
        profile::Scope t(profile::kDraw);
        if (!guard::run([&] { draw(d); })) log::once("draw-fault", "worldrender: faulted while drawing");
        drawnThisFrame = true;
    }
}
}
void install() {
    int hooked = 0;
    for (auto site : game::kRenderWorldSceneGraphSites)
        hooked += hook::redirectCall(site, game::kRenderWorldSceneGraph, reinterpret_cast<const void*>(&renderWorld));
    log::line("worldrender: %d of 2 world pass call sites hooked", hooked);
}
void shutdown() {
    releaseAll();
    owner = nullptr;
}
void presentFallback(IDirect3DDevice9* device) {
    if (!drawnThisFrame) drain(device);
    { profile::Scope t(profile::kHands); guard::run([&] { drawHands(device); }); }
    drawnThisFrame = false;
}
} // namespace vegas::worldrender
