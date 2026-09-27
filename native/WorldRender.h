#pragma once
#include <d3d9.h>
#include <d3dcommon.h>

namespace vegas::worldrender {
// Hooks New Vegas' world scene render so Minecraft's blocks, mobs, items and the block outline
// are drawn into its frame, depth-tested against the Mojave, before image space and the HUD.
void install();
// Before the core is unloaded (main thread): frees every Direct3D resource it made. The hooks
// themselves go back with hook::restoreAll.
void shutdown();
// Present time: drains Minecraft's meshes if the world pass didn't run this frame (menus,
// loading screens), so the render ring keeps moving.
void presentFallback(IDirect3DDevice9* device);
} // namespace vegas::worldrender
