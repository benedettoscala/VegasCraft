#pragma once
namespace vegas::digmesh {
// Render thread, before the world is culled. Cuts only loaded landscape geometry.
void update();
// One loaded landscape geometry; also used by the isolated Gamebryo fixture.
void updateGeometry(void* geometry);
void clear();
}
