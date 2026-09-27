#include "Overlay.h"
#include "Homography.h"
#include "Input.h"
#include "Log.h"
#include "Runtime.h"
#include <algorithm>
#include <cstring>

// D3D9 counterpart of SkyCraft's D3D11 Overlay.cpp (MIT): Minecraft's premultiplied RGBA frame
// is blended over the back buffer; its crosshair goes through Minecraft's invert blend.
namespace vegas::overlay {
namespace {
IDirect3DTexture9* texture = nullptr;
IDirect3DDevice9* owner = nullptr;
UINT texW = 0, texH = 0;
bool flipY = false;
bool haveFrame = false;
bool attached = false;

struct Vertex { float x, y, z, rhw; float u, v; };
struct ColorVertex { float x, y, z, rhw; DWORD color; };

bool ensureTexture(IDirect3DDevice9* device, UINT w, UINT h) {
    if (texture && owner == device && texW == w && texH == h) return true;
    if (texture) { texture->Release(); texture = nullptr; }
    owner = device;
    if (FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
        log::once("overlay-texture", "overlay: %ux%u texture creation failed", w, h);
        texture = nullptr;
        return false;
    }
    texW = w; texH = h;
    log::line("overlay: texture %ux%u", w, h);
    return true;
}
void upload(IDirect3DDevice9* device) {
    auto* link = state().bridge;
    if (!link->acquireOverlayFrame()) return;
    const auto* hdr = link->frontHeader();
    if (!hdr || !hdr->width || !hdr->height || hdr->width > proto::kMaxOverlayW || hdr->height > proto::kMaxOverlayH) return;
    const UINT w = hdr->width, h = hdr->height;
    const auto* src = link->frontPixels(std::size_t(w) * h * 4);
    if (!src) { log::once("overlay-map", "overlay: cannot map the frame (Windows error %lu)", GetLastError()); return; }
    if (!ensureTexture(device, w, h)) return;
    D3DLOCKED_RECT rect{};
    if (FAILED(texture->LockRect(0, &rect, nullptr, 0))) return;
    for (UINT y = 0; y < h; ++y) {
        const auto* in = reinterpret_cast<const std::uint32_t*>(src + std::size_t(y) * w * 4);
        auto* out = reinterpret_cast<std::uint32_t*>(static_cast<unsigned char*>(rect.pBits) + std::size_t(y) * rect.Pitch);
        for (UINT x = 0; x < w; ++x) {
            const std::uint32_t p = in[x];  // RGBA bytes -> D3D's BGRA
            out[x] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        }
    }
    texture->UnlockRect(0);
    flipY = (hdr->flags & 1) != 0;
    haveFrame = true;
}
void quad(IDirect3DDevice9* device, float x0, float y0, float x1, float y1, float w, float h) {
    auto u = [&](float x) { return x / w; };
    auto v = [&](float y) { return flipY ? 1.0f - y / h : y / h; };
    const Vertex q[4] = {
        {x0 - 0.5f, y0 - 0.5f, 0, 1, u(x0), v(y0)}, {x1 - 0.5f, y0 - 0.5f, 0, 1, u(x1), v(y0)},
        {x0 - 0.5f, y1 - 0.5f, 0, 1, u(x0), v(y1)}, {x1 - 0.5f, y1 - 0.5f, 0, 1, u(x1), v(y1)}};
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex));
}
// The whole frame but for a convex quad (clockwise from the top left): the Pip-Boy's screen,
// which New Vegas draws and Minecraft's Pip-Boy surrounds.
void frameWithHole(IDirect3DDevice9* device, const float* hole, float w, float h) {
    auto u = [&](float x) { return x / w; };
    auto v = [&](float y) { return flipY ? 1.0f - y / h : y / h; };
    const float outer[8] = {0, 0, w, 0, w, h, 0, h};
    Vertex tris[24];
    int n = 0;
    auto put = [&](float x, float y) { tris[n++] = {x - 0.5f, y - 0.5f, 0, 1, u(x), v(y)}; };
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        put(outer[i * 2], outer[i * 2 + 1]); put(outer[j * 2], outer[j * 2 + 1]); put(hole[j * 2], hole[j * 2 + 1]);
        put(outer[i * 2], outer[i * 2 + 1]); put(hole[j * 2], hole[j * 2 + 1]); put(hole[i * 2], hole[i * 2 + 1]);
    }
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 8, tris, sizeof(Vertex));
}
// Minecraft's page, laid out flat in the rectangle `page` of the frame, onto the Pip-Boy's screen
// (a projective warp, drawn as a fine grid so the texture stays straight in perspective).
void warpedPage(IDirect3DDevice9* device, const float* hole, const float* page, float w, float h) {
    float q[8];
    homography::inner(hole, homography::kPageInset, q);
    const auto map = homography::squareToQuad(q);
    constexpr int N = 16;
    static Vertex tris[N * N * 6];
    auto vertex = [&](int i, int j) {
        const double u = double(i) / N, v = double(j) / N;
        double x, y;
        homography::apply(map, u, v, x, y);
        const float sx = page[0] + float(u) * (page[2] - page[0]), sy = page[1] + float(v) * (page[3] - page[1]);
        return Vertex{float(x) - 0.5f, float(y) - 0.5f, 0, 1, sx / w, flipY ? 1.0f - sy / h : sy / h};
    };
    int n = 0;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const Vertex a = vertex(i, j), b = vertex(i + 1, j), c = vertex(i + 1, j + 1), d = vertex(i, j + 1);
            tris[n++] = a; tris[n++] = b; tris[n++] = c;
            tris[n++] = a; tris[n++] = c; tris[n++] = d;
        }
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, N * N * 2, tris, sizeof(Vertex));
}
void cursor(IDirect3DDevice9* device, float x, float y) {
    // The arrow SkyCraft's overlay shader draws: black outline, white fill.
    device->SetTexture(0, nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    auto arrow = [&](float grow, DWORD color) {
        const ColorVertex a[3] = {{x - grow, y - grow * 2, 0, 1, color}, {x + 10.8f + grow * 2, y + 18 + grow, 0, 1, color}, {x - grow, y + 18 + grow, 0, 1, color}};
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, a, sizeof(ColorVertex));
    };
    arrow(1.5f, 0xFF000000);
    arrow(0.0f, 0xFFFFFFFF);
}
}
void present(IDirect3DDevice9* device) {
    auto& st = state();
    if (!device || !st.bridge) return;
    IDirect3DSurface9* back = nullptr;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        back->GetDesc(&desc);
        back->Release();
        st.viewportW = static_cast<int>(desc.Width);
        st.viewportH = static_cast<int>(desc.Height);
    }
    if (!attached) {
        D3DDEVICE_CREATION_PARAMETERS params{};
        if (SUCCEEDED(device->GetCreationParameters(&params)) && params.hFocusWindow) input::attachWindow(params.hFocusWindow);
        attached = true;
    }
    if (!st.bridge->minecraftAlive() || !st.mcInWorld) return;
    upload(device);
    if (!haveFrame || !texture || (st.nvMenuOpen && !st.pipBoyUp) || !desc.Width) return;

    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved))) return;
    IDirect3DSurface9* target = nullptr;
    device->GetRenderTarget(0, &target);
    IDirect3DSurface9* backBuffer = nullptr;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (backBuffer) device->SetRenderTarget(0, backBuffer);
    const float w = float(desc.Width), h = float(desc.Height);
    D3DVIEWPORT9 vp{0, 0, desc.Width, desc.Height, 0, 1};
    device->SetViewport(&vp);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetTexture(0, texture);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // Minecraft's crosshair and attack indicator (around the centre): Minecraft's invert blend.
    const int scale = st.mcGuiScale;
    const bool invert = st.mcCrosshair && scale > 0;
    const float g = float(scale) * (texW ? w / float(texW) : 1.0f);
    const float cx = w * 0.5f, cy = h * 0.5f;
    const float ix0 = cx - 12 * g, iy0 = cy - 12 * g, ix1 = cx + 12 * g, iy1 = cy + 28 * g;
    if (st.pipBoyUp && st.pipBoyHoleValid) {
        frameWithHole(device, st.pipBoyHole, w, h);
        if (st.pipBoyMcPage && st.pipBoyPageValid) warpedPage(device, st.pipBoyHole, st.pipBoyPage, w, h);
    } else if (!invert) {
        quad(device, 0, 0, w, h, w, h);
    } else {
        quad(device, 0, 0, w, iy0, w, h);
        quad(device, 0, iy1, w, h, w, h);
        quad(device, 0, iy0, ix0, iy1, w, h);
        quad(device, ix1, iy0, w, iy1, w, h);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCCOLOR);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        quad(device, ix0, iy0, ix1, iy1, w, h);
    }
    if (st.mcScreenOpen) {
        const float sx = texW ? w / float(texW) : 1.0f, sy = texH ? h / float(texH) : 1.0f;
        cursor(device, float(st.cursorX) * sx, float(st.cursorY) * sy);
    }
    if (target) { device->SetRenderTarget(0, target); target->Release(); }
    if (backBuffer) backBuffer->Release();
    saved->Apply();
    saved->Release();
}
void shutdown() {
    if (texture) texture->Release();
    texture = nullptr; owner = nullptr; texW = texH = 0; haveFrame = false;
}
} // namespace vegas::overlay
