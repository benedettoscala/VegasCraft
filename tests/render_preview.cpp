#include "MotionPreview.h"
#include <d3d9.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool okay, const char* what) { if (!okay) throw std::runtime_error(what); }
template<class T, std::size_t N> void put(std::array<unsigned char, N>& buffer, std::size_t offset, T value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}
int main() {
    try {
        // Disposable, hidden rendering fixture, no input or access to the game process.
        HWND window = CreateWindowExW(0, L"STATIC", L"VegasCraft render fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "hidden fixture window");
        IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION);
        require(api != nullptr, "D3D9 API");
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
        pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        IDirect3DDevice9* device = nullptr;
        require(SUCCEEDED(api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)), "D3D9 test device");
        std::array<unsigned char, 0x300> renderer{};
        std::array<unsigned char, 0xC0> main{};
        std::array<unsigned char, 0x114> camera{};
        put(renderer, 0x288, device); put(main, 0xAC, camera.data());
        void* rendererSlot = renderer.data(); void* mainSlot = main.data();
        // Camera looks along +Y, with +Z up and +X right.
        float transform[13] = {0,0,1, 1,0,0, 0,1,0, 0,0,100, 1};
        float frustum[6] = {-.75f,.75f,.5625f,-.5625f,5,10000};
        std::memcpy(camera.data() + 0x68, transform, sizeof(transform));
        std::memcpy(camera.data() + 0xDC, frustum, sizeof(frustum));
        // Padding is not the orthographic flag, and need not be zero in the game.
        camera[0xF5] = 0xA5; camera[0xF6] = 0xA5; camera[0xF7] = 0xA5;
        std::wstring name = L"Local\\VegasCraft_render_fixture_" + std::to_wstring(GetCurrentProcessId());
        vegas::Bridge bridge; require(bridge.open(name), "fixture mapping");
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        auto* bytes = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0x1000));
        require(bytes != nullptr, "fixture writer view");
        auto* header = reinterpret_cast<vegas::proto::Header*>(bytes);
        header->mcPid = GetCurrentProcessId(); header->mcHeartbeatMs = GetTickCount64();
        auto* mc = reinterpret_cast<vegas::proto::McState*>(bytes + vegas::proto::kOffMcState);
        mc->flags = vegas::proto::kMcInWorld;
        vegas::proto::SkyState host{}; host.flags = vegas::proto::kSkyInGame; host.collisionEpoch = 1;
        require(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF202028, 1, 0)), "clear fixture");
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
        D3DMATRIX savedProjection{}; for (int i = 0; i < 4; ++i) savedProjection.m[i][i] = 1;
        savedProjection.m[0][0] = 7;
        device->SetTransform(D3DTS_PROJECTION, &savedProjection);
        vegas::MotionPreview preview(reinterpret_cast<std::uintptr_t>(&rendererSlot), reinterpret_cast<std::uintptr_t>(&mainSlot));
        preview.enable(true);
        preview.present(host, bridge, 70, stdout);
        DWORD fillMode = 0; device->GetRenderState(D3DRS_FILLMODE, &fillMode);
        require(fillMode == D3DFILL_WIREFRAME, "render state restored");
        D3DMATRIX after{}; device->GetTransform(D3DTS_PROJECTION, &after);
        require(std::memcmp(&after, &savedProjection, sizeof(after)) == 0, "projection restored");
        IDirect3DSurface9* target = nullptr; IDirect3DSurface9* pixels = nullptr;
        require(SUCCEEDED(device->GetRenderTarget(0, &target)), "fixture render target");
        require(SUCCEEDED(device->CreateOffscreenPlainSurface(640, 480, D3DFMT_X8R8G8B8,
            D3DPOOL_SYSTEMMEM, &pixels, nullptr)), "fixture pixel surface");
        require(SUCCEEDED(device->GetRenderTargetData(target, pixels)), "fixture readback");
        D3DLOCKED_RECT rect{}; require(SUCCEEDED(pixels->LockRect(&rect, nullptr, D3DLOCK_READONLY)), "fixture pixels");
        int changed = 0;
        for (int y = 0; y < 480; ++y) for (int x = 0; x < 640; ++x) {
            auto p = reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(rect.pBits) + y * rect.Pitch)[x];
            if ((p & 0xFFFFFF) != 0x202028) ++changed;
        }
        require(changed > 1000 && changed < 100000, "visible cube occupies a bounded part of the fixture");
        BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
        file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = file.bfOffBits + 640 * 480 * 4;
        info.biSize = sizeof(info); info.biWidth = 640; info.biHeight = -480;
        info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
        std::ofstream output("preview-fixture.bmp", std::ios::binary);
        output.write(reinterpret_cast<char*>(&file), sizeof(file)); output.write(reinterpret_cast<char*>(&info), sizeof(info));
        for (int y = 0; y < 480; ++y) output.write(static_cast<const char*>(rect.pBits) + y * rect.Pitch, 640 * 4);
        output.close(); pixels->UnlockRect();
        auto centerX = [&]() {
            require(SUCCEEDED(device->GetRenderTargetData(target, pixels)), "movement readback");
            D3DLOCKED_RECT moved{};
            require(SUCCEEDED(pixels->LockRect(&moved, nullptr, D3DLOCK_READONLY)), "movement pixels");
            int count = 0; std::int64_t sum = 0;
            for (int y = 0; y < 480; ++y) for (int x = 0; x < 640; ++x) {
                auto p = reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(moved.pBits) + y * moved.Pitch)[x];
                if ((p & 0xFFFFFF) != 0x202028) { ++count; sum += x; }
            }
            pixels->UnlockRect();
            return count ? static_cast<double>(sum) / count : -1.0;
        };
        auto before = centerX();
        auto renderAgain = [&]() {
            require(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF202028, 1, 0)), "movement clear");
            header->mcHeartbeatMs = GetTickCount64();
            preview.present(host, bridge, 70, stdout);
        };
        mc->x = 1; mc->seq += 2;
        renderAgain(); auto moved = centerX();
        require(moved > before + 50, "Minecraft movement changes the actual rendered position");
        ++host.collisionEpoch;
        renderAgain();
        require(std::abs(centerX() - before) < 2, "load epoch resets the Minecraft movement origin");
        // The game can still hold an open scene when xNVSE reports the frame.
        require(SUCCEEDED(device->BeginScene()), "fixture open game scene");
        renderAgain();
        require(SUCCEEDED(device->EndScene()), "game scene left open for the game to close");
        require(std::abs(centerX() - before) < 2, "cube drawn inside an already open scene");
        mc->flags = 0; mc->seq += 2;
        renderAgain(); require(centerX() == -1, "no cube without a loaded Minecraft world");
        // Same runtime entry point must safely ignore missing game pointers.
        mainSlot = nullptr;
        preview.present(host, bridge, 70, stdout);
        pixels->Release(); target->Release(); device->Release(); api->Release(); DestroyWindow(window);
        UnmapViewOfFile(bytes); CloseHandle(mapping); bridge.close();
        std::cout << "D3D9 preview rendered " << changed << " pixels; state restoration and movement passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
