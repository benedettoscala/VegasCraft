#include "Camera.h"
#include "Game.h"
#include "Runtime.h"
#include <algorithm>
#include <cmath>

// SkyCraft keeps the host game in first person and moves only its world camera for F5.
// New Vegas' NiCamera world transform and UpdateWorldToCamera replace Skyrim's camera hooks.
namespace vegas::camera {
namespace { constexpr float kShoulderBlocks = 0.75f; Transform target{}; bool valid = false; float zoom = 0; unsigned lastMode = 0; ULONGLONG lastTick = 0; }
Transform orbit(double x, double y, double z, float yaw, float pitch, unsigned mode, float distance) {
    constexpr float rad = 0.0174532925f;
    if (mode == 2) { yaw += 180; pitch = -pitch; }
    const float a = yaw * rad, p = pitch * rad;
    const float f[3] = {-std::sin(a) * std::cos(p), -std::cos(a) * std::cos(p), -std::sin(p)};
    const float r[3] = {-std::cos(a), std::sin(a), 0};
    const float u[3] = {r[1] * f[2], -r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    Transform t{};
    t.scale = 1;
    const float eye[3] = {float(x * proto::kUnitsPerBlock), float(-z * proto::kUnitsPerBlock), float(y * proto::kUnitsPerBlock)};
    for (int k = 0; k < 3; ++k) {
        t.rot[k][0] = f[k]; t.rot[k][1] = u[k]; t.rot[k][2] = r[k];
        t.pos[k] = eye[k] - (mode ? std::clamp(distance, 0.0f, 16.0f) * float(proto::kUnitsPerBlock) * f[k] : 0);
    }
    return t;
}
void update(const proto::McState& mc, double x, double y, double z, float yaw, float pitch) {
    valid = false;
    if (mc.cameraMode > 2 || !std::isfinite(mc.cameraDistance) || !std::isfinite(mc.eyeX) ||
        !std::isfinite(mc.eyeY) || !std::isfinite(mc.eyeZ) || (mc.flags & proto::kMcDead)) { clear(); return; }
    // Minecraft supplies its interpolated sneak eye and zoom collision against both worlds.
    const double ex = mc.eyeX - mc.x, ey = mc.eyeY - mc.y, ez = mc.eyeZ - mc.z;
    if (std::fabs(ex) > 2 || std::fabs(ey) > 4 || std::fabs(ez) > 2) { clear(); return; }
    const auto now = GetTickCount64();
    const float dt = lastTick ? std::min(0.25f, float(now - lastTick) / 1000) : 0;
    lastTick = now;
    const float desired = mc.cameraMode ? std::clamp(mc.cameraDistance, 0.0f, 16.0f) : 0;
    if (mc.cameraMode != lastMode || desired < zoom) zoom = desired;
    else zoom += (desired - zoom) * (1 - std::exp(-dt / 0.2f));
    lastMode = mc.cameraMode;
    target = orbit(x + ex, y + ey, z + ez, yaw, pitch, mc.cameraMode, zoom);
    // Third person from behind looks over the right shoulder, as in New Vegas. The shift shrinks with
    // the zoom, which Minecraft pulls in against walls, so it doesn't push the camera into them.
    if (mc.cameraMode == 1) {
        const float side = kShoulderBlocks * std::clamp(zoom / 4.0f, 0.0f, 1.0f) * float(proto::kUnitsPerBlock);
        for (int k = 0; k < 3; ++k) target.pos[k] += target.rot[k][2] * side;
    }
    valid = true;
}
void clear() { valid = false; zoom = 0; lastMode = 0; lastTick = 0; }
namespace {
// Moves a node and all its descendants by d (world space). Bounds stay: the sky is far bigger.
void shift(void* obj, const float* d, int depth) {
    if (!obj || depth > 12 || !hook::plausible(obj, 0xA8)) return;
    auto p = hook::field<game::NiPoint3>(obj, 0x8C);
    p.x += d[0]; p.y += d[1]; p.z += d[2];
    hook::setField(obj, 0x8C, p);
    auto** vt = hook::field<void**>(obj, 0);
    if (!vt || !hook::plausible(vt, 16) || !reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) return;  // not a node
    auto** children = hook::field<void**>(obj, 0xA0);
    const auto count = hook::field<unsigned short>(obj, 0xA6);
    if (count > 512 || !hook::plausible(children, count * 4)) return;
    for (unsigned i = 0; i < count; ++i) shift(children[i], d, depth + 1);
}
// New Vegas centres its sky dome on its own camera; with Minecraft's third-person camera a few
// blocks away the sky slid around as the view turned. It follows Minecraft's camera instead.
void centreSky(const float* eye) {
    void* sky = game::global<void>(0x011DEA20);
    void* root = sky ? hook::field<void*>(sky, 0x04) : nullptr;
    if (!root || !hook::plausible(root, 0xA8)) return;
    const auto at = hook::field<game::NiPoint3>(root, 0x8C);
    const float d[3] = {eye[0] - at.x, eye[1] - at.y, eye[2] - at.z};
    if (std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]) < 0.01f || std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]) > 5000) return;
    shift(root, d, 0);
}
}
void apply() {
    if (!valid || !state().puppeting || state().nvMenuOpen) return;
    void* scene = game::global<void>(game::kSceneGraphSlot);
    void* cam = scene ? hook::field<void*>(scene, 0xAC) : nullptr;
    if (!cam) return;
    hook::setField(cam, 0x68, target);
    centreSky(target.pos);
    // Verified against NiCamera's world-data override and the executable: this refreshes
    // the culling/projection matrix from the world transform without moving its parent.
    reinterpret_cast<void(__thiscall*)(void*)>(0xA70BA0)(cam);
}
}
