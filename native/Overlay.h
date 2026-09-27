#pragma once
#include <d3d9.h>

namespace vegas::overlay {
// Present time (xNVSE's frame message): composites Minecraft's HUD, hand and screens over the
// finished New Vegas frame. New Vegas still holds its scene open here.
void present(IDirect3DDevice9* device);
// Before the core is unloaded (main thread): frees the overlay texture.
void shutdown();
} // namespace vegas::overlay
