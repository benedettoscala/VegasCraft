#include "MotionPreview.h"
#include <d3d9.h>
#include <array>
#include <cmath>
#include <cstring>

namespace vegas {
namespace {
// Executable data addresses and field offsets researched in NVTF / TESReloaded.
// See docs/RENDERING.md. This is independent code, not their renderer implementation.
template<class T> bool read(const void* address, T& value) {
    SIZE_T bytes = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof(T), &bytes) && bytes == sizeof(T);
}
template<class T> bool field(const void* object, std::size_t offset, T& value) {
    return object && read(static_cast<const unsigned char*>(object) + offset, value);
}
struct Transform { float rotation[3][3]; float position[3]; float scale; };
struct Frustum { float left, right, top, bottom, nearZ, farZ; std::uint8_t ortho, pad[3]; };
struct Vertex { float x, y, z; DWORD color; };
bool finite(const Transform& t, const Frustum& f) {
    for (auto& row : t.rotation) for (auto n : row) if (!std::isfinite(n)) return false;
    for (auto n : t.position) if (!std::isfinite(n)) return false;
    return std::isfinite(f.left) && std::isfinite(f.right) && std::isfinite(f.top) && std::isfinite(f.bottom) &&
        std::isfinite(f.nearZ) && std::isfinite(f.farZ) && f.right > f.left && f.top > f.bottom &&
        f.nearZ > 0 && f.farZ > f.nearZ && f.ortho == 0;
}
D3DMATRIX viewMatrix(const Transform& t) {
    D3DMATRIX m{};
    // Gamebryo camera local axes: column 0 forward, column 1 up, column 2 right.
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = t.rotation[i][2];
        m.m[i][1] = t.rotation[i][1];
        m.m[i][2] = t.rotation[i][0];
        for (int j = 0; j < 3; ++j) m.m[3][j] -= t.position[i] * m.m[i][j];
    }
    m.m[3][3] = 1;
    return m;
}
D3DMATRIX projectionMatrix(const Frustum& f) {
    // NiFrustum stores slopes at unit distance, not near-plane extents.
    D3DMATRIX m{};
    m.m[0][0] = 2 / (f.right - f.left);
    m.m[1][1] = 2 / (f.top - f.bottom);
    m.m[2][0] = -(f.right + f.left) / (f.right - f.left);
    m.m[2][1] = -(f.top + f.bottom) / (f.top - f.bottom);
    m.m[2][2] = f.farZ / (f.farZ - f.nearZ);
    m.m[2][3] = 1;
    m.m[3][2] = -f.nearZ * m.m[2][2];
    return m;
}
std::array<Vertex, 36> cube(Position center, float side) {
    std::array<Vertex, 36> vertices{};
    constexpr int faces[6][4] = {{0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3}};
    constexpr DWORD colors[] = {0xFF705030,0xFF72B834,0xFF986E43,0xFF68492D,0xFF805A36,0xFFB18250};
    int out = 0;
    for (int face = 0; face < 6; ++face) for (int corner : {0,1,2,0,2,3}) {
        int p = faces[face][corner];
        vertices[out++] = {static_cast<float>(center.x) + ((p & 1) ? .5f : -.5f) * side,
            static_cast<float>(center.y) + ((p & 2) ? .5f : -.5f) * side,
            static_cast<float>(center.z) + ((p & 4) ? .5f : -.5f) * side, colors[face]};
    }
    return vertices;
}
}
void MotionPreview::present(const proto::SkyState& host, const Bridge& bridge, double scale, FILE* log) {
    if (!enabled_ || !(host.flags & proto::kSkyInGame)) { reset(); return; }
    proto::McState mc{};
    if (!bridge.readMinecraft(mc)) return;
    void* renderer = nullptr; void* scene = nullptr; void* camera = nullptr;
    IDirect3DDevice9* device = nullptr;
    Transform transform{}; Frustum frustum{};
    HRESULT begin = E_FAIL, result = E_FAIL;
    bool ready = read(reinterpret_cast<void*>(rendererSlot_), renderer) && field(renderer, 0x288, device) && device &&
        read(reinterpret_cast<void*>(sceneSlot_), scene) && field(scene, 0xAC, camera) &&
        field(camera, 0x68, transform) && field(camera, 0xDC, frustum) && finite(transform, frustum);
    if (ready && SUCCEEDED(device->TestCooperativeLevel())) {
        if (!anchored_ || epoch_ != host.collisionEpoch) {
            anchor_ = {transform.position[0], transform.position[1], transform.position[2]};
            for (int i = 0; i < 3; ++i) {
                auto offset = transform.rotation[i][0] * scale * 3;
                if (i == 0) anchor_.x += offset;
                if (i == 1) anchor_.y += offset;
                if (i == 2) anchor_.z += offset;
            }
            mcOrigin_ = {mc.x, mc.y, mc.z}; epoch_ = host.collisionEpoch; anchored_ = true;
        }
        auto delta = toNewVegas({mc.x - mcOrigin_.x, mc.y - mcOrigin_.y, mc.z - mcOrigin_.z}, scale);
        Position center{anchor_.x + delta.x, anchor_.y + delta.y, anchor_.z + delta.z};
        auto vertices = cube(center, static_cast<float>(scale));
        IDirect3DStateBlock9* saved = nullptr;
        if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &saved))) {
            // New Vegas may still be inside its own scene at present time; BeginScene then
            // returns D3DERR_INVALIDCALL and the probe draws within the open scene.
            begin = device->BeginScene();
            if (SUCCEEDED(begin) || begin == D3DERR_INVALIDCALL) {
                D3DMATRIX identity{}; for (int i = 0; i < 4; ++i) identity.m[i][i] = 1;
                auto view = viewMatrix(transform); auto projection = projectionMatrix(frustum);
                device->SetVertexShader(nullptr); device->SetPixelShader(nullptr);
                device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
                device->SetTransform(D3DTS_WORLD, &identity);
                device->SetTransform(D3DTS_VIEW, &view); device->SetTransform(D3DTS_PROJECTION, &projection);
                device->SetTexture(0, nullptr);
                device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                device->SetRenderState(D3DRS_LIGHTING, FALSE); device->SetRenderState(D3DRS_FOGENABLE, FALSE);
                device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                device->SetRenderState(D3DRS_STENCILENABLE, FALSE); device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
                // Present-time world depth has already been cleared by the game. This probe is an overlay.
                device->SetRenderState(D3DRS_ZENABLE, FALSE); device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
                result = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 12, vertices.data(), sizeof(Vertex));
                if (SUCCEEDED(begin)) device->EndScene();
            }
            saved->Apply(); saved->Release();
        }
    }
    auto now = GetTickCount64();
    if (log && now - lastLog_ > 2000) {
        lastLog_ = now;
        std::fprintf(log, "Preview ready=%d begin=%08lX hr=%08lX camera=%p pos=%.1f,%.1f,%.1f frustum=%.3f,%.3f,%.3f,%.3f near=%.1f far=%.1f\n",
            ready, static_cast<unsigned long>(begin), static_cast<unsigned long>(result), camera, transform.position[0], transform.position[1], transform.position[2],
            frustum.left, frustum.right, frustum.top, frustum.bottom, frustum.nearZ, frustum.farZ);
        std::fflush(log);
    }
}
}
