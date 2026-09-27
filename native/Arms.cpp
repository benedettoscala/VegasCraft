#include "Arms.h"
#include "Profile.h"
#include "Bridge.h"
#include "Game.h"
#include "Guard.h"
#include "Log.h"
#include "Rtti.h"
#include "Runtime.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace vegas::arms {
namespace {
// NiAVObject: name +0x08, world rotate (3x3, row major) +0x68, world translate +0x8C. NiNode
// children: array +0xA0, count +0xA6.
constexpr std::size_t kName = 0x08, kWorldRot = 0x68, kWorldPos = 0x8C;
constexpr float kTanHalfFovX = 1.46f;  // at the player's first-person FOV of 55 (see publish)
// New Vegas narrows the first-person FOV while aiming down the sights (55 -> 34.5 for the Cowboy
// Repeater, measured 2026-10-03): the measured projection scales with tan(FOV / 2).
float tanHalfFovX(void* player) {
    const float fov = player ? hook::field<float>(player, game::kPlayerFirstPersonFov) : 55.0f;
    if (!(fov > 5.0f && fov < 120.0f)) return kTanHalfFovX;
    return kTanHalfFovX * std::tan(fov * 0.5f * 0.0174532925f) / std::tan(27.5f * 0.0174532925f);
}
const char* const kBoneNames[2][3] = {
    {"Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand"},
    {"Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand"},
};
void* cachedRoot = nullptr;
void* bones[2][3]{};
void* camera1st = nullptr;
void* pipScreen = nullptr;  // "pipboyscreen:0", the Pip-Boy screen geometry its menus are drawn on

bool named(void* obj, const char* want) {
    const char* name = hook::field<const char*>(obj, kName);
    return hook::plausible(name) && std::strcmp(name, want) == 0;  // callers run inside guard::run
}
void collect(void* obj, int depth) {
    if (!obj || depth > 40 || !hook::readable(obj, 0xA8)) return;
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 3; ++b)
            if (!bones[a][b] && named(obj, kBoneNames[a][b])) bones[a][b] = obj;
    if (!camera1st && named(obj, "Camera1st")) camera1st = obj;
    if (!pipScreen && named(obj, "pipboyscreen:0")) pipScreen = obj;
    auto** vt = hook::field<void**>(obj, 0);
    if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) return;  // IsNode
    auto** children = hook::field<void**>(obj, 0xA0);
    const auto count = hook::field<unsigned short>(obj, 0xA6);
    if (count > 256 || !hook::readable(children, count * 4)) return;
    for (unsigned i = 0; i < count; ++i) collect(children[i], depth + 1);
}
bool find(void* player) {
    void* root = player ? hook::field<void*>(player, game::kPlayerFirstPersonNode) : nullptr;
    if (!root) return false;
    if (root != cachedRoot) {
        std::memset(bones, 0, sizeof(bones));
        camera1st = nullptr;
        pipScreen = nullptr;
        cachedRoot = root;
        collect(root, 0);
    }
    for (auto& arm : bones)
        for (void* b : arm)
            if (!b) return false;
    return true;
}
// The world camera's transform: rotation columns are forward, up, right (Gamebryo NiCamera).
bool cameraTransform(float rot[3][3], float pos[3]) {
    void* scene = game::global<void>(game::kSceneGraphSlot);
    void* cam = scene ? hook::field<void*>(scene, 0xAC) : nullptr;
    if (!cam || !hook::readable(cam, 0x9C)) return false;
    std::memcpy(rot, static_cast<char*>(cam) + kWorldRot, sizeof(float) * 9);
    std::memcpy(pos, static_cast<char*>(cam) + kWorldPos, sizeof(float) * 3);
    return true;
}
// World -> camera space with x right, y up, z backwards.
void toCamera(void* bone, const float camRot[3][3], const float camPos[3], Bone& out) {
    const auto* r = reinterpret_cast<const float*>(static_cast<char*>(bone) + kWorldRot);
    const auto* p = reinterpret_cast<const float*>(static_cast<char*>(bone) + kWorldPos);
    // Camera axes in world space (columns): forward 0, up 1, right 2.
    const float* axes[3] = {nullptr, nullptr, nullptr};
    float right[3], up[3], back[3];
    for (int k = 0; k < 3; ++k) { right[k] = camRot[k][2]; up[k] = camRot[k][1]; back[k] = -camRot[k][0]; }
    axes[0] = right; axes[1] = up; axes[2] = back;
    const float d[3] = {p[0] - camPos[0], p[1] - camPos[1], p[2] - camPos[2]};
    for (int i = 0; i < 3; ++i) {
        out.pos[i] = axes[i][0] * d[0] + axes[i][1] * d[1] + axes[i][2] * d[2];
        // Bone axis j (world column j of r) expressed in camera space.
        for (int j = 0; j < 3; ++j) out.rot[i][j] = axes[i][0] * r[0 * 3 + j] + axes[i][1] * r[1 * 3 + j] + axes[i][2] * r[2 * 3 + j];
    }
}
}

bool sample(void* player, Arm out[2]) {
    bool ok = false;
    guard::run([&] {
        float camRot[3][3], camPos[3];
        if (!find(player) || !cameraTransform(camRot, camPos)) return;
        for (int a = 0; a < 2; ++a) {
            toCamera(bones[a][0], camRot, camPos, out[a].upper);
            toCamera(bones[a][1], camRot, camPos, out[a].fore);
            toCamera(bones[a][2], camRot, camPos, out[a].hand);
        }
        ok = true;
    });
    return ok;
}

void report(void* player) {
    Arm arms[2];
    if (!sample(player, arms)) { log::line("bones: unavailable (root %p)", player ? hook::field<void*>(player, game::kPlayerFirstPersonNode) : nullptr); return; }
    const char* side[2] = {"R", "L"};
    for (int a = 0; a < 2; ++a)
        for (const Bone* b : {&arms[a].upper, &arms[a].fore, &arms[a].hand})
            log::line("bones %s %s pos %.1f %.1f %.1f  x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) z(%.2f %.2f %.2f)", side[a],
                b == &arms[a].upper ? "upper" : b == &arms[a].fore ? "fore " : "hand ", b->pos[0], b->pos[1], b->pos[2],
                b->rot[0][0], b->rot[1][0], b->rot[2][0], b->rot[0][1], b->rot[1][1], b->rot[2][1], b->rot[0][2], b->rot[1][2], b->rot[2][2]);
    void* scene = game::global<void>(game::kSceneGraphSlot);
    void* cam = scene ? hook::field<void*>(scene, 0xAC) : nullptr;
    if (cam && hook::readable(cam, 0xF8)) {
        const auto* f = reinterpret_cast<const float*>(static_cast<char*>(cam) + 0xDC);
        log::line("bones camera frustum l %.3f r %.3f t %.3f b %.3f n %.2f f %.0f; player fov world %.2f first %.2f; viewport %d x %d", f[0], f[1], f[2], f[3], f[4], f[5],
            hook::field<float>(player, game::kPlayerWorldFov), hook::field<float>(player, game::kPlayerFirstPersonFov), state().viewportW.load(), state().viewportH.load());
    }
    float camRot[3][3], camPos[3];
    if (camera1st && cameraTransform(camRot, camPos)) {
        Bone c;
        toCamera(camera1st, camRot, camPos, c);
        log::line("bones Camera1st pos %.1f %.1f %.1f", c.pos[0], c.pos[1], c.pos[2]);
    }
}

// A world point or direction in the camera space of toCamera.
void camAxes(const float camRot[3][3], float right[3], float up[3], float back[3]) {
    for (int k = 0; k < 3; ++k) { right[k] = camRot[k][2]; up[k] = camRot[k][1]; back[k] = -camRot[k][0]; }
}
void toCamDir(const float camRot[3][3], const float d[3], float out[3]) {
    float a[3][3];
    camAxes(camRot, a[0], a[1], a[2]);
    for (int i = 0; i < 3; ++i) out[i] = a[i][0] * d[0] + a[i][1] * d[1] + a[i][2] * d[2];
}
// The Pip-Boy screen in camera space: centre, half width and half height vectors (its mesh's
// local bounds; the thinnest axis is its normal, the widest its width), pointing right and up.
bool sampleScreen(float out[9]) {
    bool ok = false;
    guard::run([&] {
        float camRot[3][3], camPos[3];
        if (!pipScreen || !cameraTransform(camRot, camPos) || !hook::plausible(pipScreen)) return;
        void* data = hook::field<void*>(pipScreen, 0xB8);
        if (!data || !hook::plausible(data)) return;
        const auto count = hook::field<unsigned short>(data, 8);
        const auto* v = hook::field<const float*>(data, 0x20);
        if (count < 3 || !hook::plausible(v)) return;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (unsigned i = 0; i < count; ++i)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v[i * 3 + k]); hi[k] = std::max(hi[k], v[i * 3 + k]); }
        int n = 0;
        for (int k = 1; k < 3; ++k) if (hi[k] - lo[k] < hi[n] - lo[n]) n = k;
        int wa = (n + 1) % 3, ha = (n + 2) % 3;
        if (hi[ha] - lo[ha] > hi[wa] - lo[wa]) std::swap(wa, ha);
        const auto* r = reinterpret_cast<const float*>(static_cast<char*>(pipScreen) + kWorldRot);
        const auto* p = reinterpret_cast<const float*>(static_cast<char*>(pipScreen) + kWorldPos);
        const float s = hook::field<float>(pipScreen, 0x98);
        float c[3], local[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2}, w[3], h[3];
        for (int k = 0; k < 3; ++k) {
            c[k] = p[k] + s * (r[k * 3 + 0] * local[0] + r[k * 3 + 1] * local[1] + r[k * 3 + 2] * local[2]) - camPos[k];
            w[k] = s * r[k * 3 + wa] * (hi[wa] - lo[wa]) / 2;
            h[k] = s * r[k * 3 + ha] * (hi[ha] - lo[ha]) / 2;
        }
        toCamDir(camRot, c, out);
        toCamDir(camRot, w, out + 3);
        toCamDir(camRot, h, out + 6);
        if (out[3] < 0) for (int k = 3; k < 6; ++k) out[k] = -out[k];  // width to the right
        if (out[7] < 0) for (int k = 6; k < 9; ++k) out[k] = -out[k];  // height upwards
        ok = std::isfinite(out[0]) && out[2] < 0;
    });
    return ok;
}
// Where the screen's corners land on New Vegas' back buffer, for the overlay to leave them open.
void screenPixels(const float scr[9], float tanX, float tanY) {
    auto& st = state();
    const float w = float(st.viewportW), h = float(st.viewportH);
    if (w <= 0 || h <= 0) { st.pipBoyHoleValid = false; return; }
    const float sx[4] = {-1, 1, 1, -1}, sy[4] = {1, 1, -1, -1};
    for (int i = 0; i < 4; ++i) {
        const float x = scr[0] + sx[i] * scr[3] + sy[i] * scr[6], y = scr[1] + sx[i] * scr[4] + sy[i] * scr[7], z = scr[2] + sx[i] * scr[5] + sy[i] * scr[8];
        if (z > -1.0f) { st.pipBoyHoleValid = false; return; }
        st.pipBoyHole[i * 2] = w * (0.5f + 0.5f * x / (-z * tanX));
        st.pipBoyHole[i * 2 + 1] = h * (0.5f - 0.5f * y / (-z * tanY));
    }
    st.pipBoyHoleValid = true;
}

bool sampleThird(void* player, proto::ArmPose& pose);

void publish(void* player, std::uint32_t space) {
    static bool published = false;
    auto* bridge = state().bridge;
    if (!bridge) return;
    proto::ArmPose pose{};
    Arm arms[2];
    if ((space == proto::kArmsFirstPerson || space == proto::kArmsPipBoy) && sample(player, arms)) {
        pose.valid = space;
        // Measured, not derived (2026-10-02, 1280x720, fDefault1stPersonFOV 55): the bones project
        // where New Vegas draws the weapon for a horizontal tan(FOV / 2) of 1.46; the player's
        // first-person FOV field (55) gives far too narrow a frustum for these bone positions.
        const float aspect = state().viewportH > 0 ? float(state().viewportW) / float(state().viewportH) : 16.0f / 9.0f;
        float tanX = tanHalfFovX(player);
        pose.tanHalfFovY = tanX / aspect;
        if (space == proto::kArmsPipBoy) {
            // Zoomed on the wrist, New Vegas draws the first-person view through the world
            // camera's own frustum (r 0.58, t 0.33 at 16:9; verified live 2026-10-02).
            void* scene = game::global<void>(game::kSceneGraphSlot);
            void* cam = scene ? hook::field<void*>(scene, 0xAC) : nullptr;
            if (cam && hook::readable(cam, 0xF8)) {
                const auto* f = reinterpret_cast<const float*>(static_cast<char*>(cam) + 0xDC);
                if (f[1] > 0.05f && f[1] < 5.0f && f[2] > 0.05f && f[2] < 5.0f) { tanX = f[1]; pose.tanHalfFovY = f[2]; }
            }
        }
        for (int a = 0; a < 2; ++a) {
            std::memcpy(pose.arms[a].start, arms[a].fore.pos, sizeof(float) * 3);
            std::memcpy(pose.arms[a].hand, arms[a].hand.pos, sizeof(float) * 3);
            std::memcpy(pose.arms[a].handRot, arms[a].hand.rot, sizeof(float) * 9);
        }
        if (space == proto::kArmsPipBoy) {
            if (sampleScreen(pose.screen)) screenPixels(pose.screen, tanX, pose.tanHalfFovY);
            else { pose.valid = proto::kArmsFirstPerson; state().pipBoyHoleValid = false; }
            static std::uint64_t lastLog = 0;
            if (GetTickCount64() - lastLog > 2000) {
                lastLog = GetTickCount64();
                const auto& s = pose.screen;
                const auto& hp = state().pipBoyHole;
                log::line("pipboy: screen %p centre %.1f %.1f %.1f w %.1f %.1f %.1f h %.1f %.1f %.1f; hole %s %.0f,%.0f %.0f,%.0f %.0f,%.0f %.0f,%.0f; L hand %.1f %.1f %.1f R hand %.1f %.1f %.1f",
                    pipScreen, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], state().pipBoyHoleValid ? "on" : "off", hp[0], hp[1], hp[2], hp[3], hp[4], hp[5], hp[6], hp[7],
                    pose.arms[1].hand[0], pose.arms[1].hand[1], pose.arms[1].hand[2], pose.arms[0].hand[0], pose.arms[0].hand[1], pose.arms[0].hand[2]);
                void* scene = game::global<void>(game::kSceneGraphSlot);
                void* cam = scene ? hook::field<void*>(scene, 0xAC) : nullptr;
                const float* f = cam && hook::readable(cam, 0xF8) ? reinterpret_cast<const float*>(static_cast<char*>(cam) + 0xDC) : nullptr;
                log::line("pipboy: player fov world %.2f first %.2f; world camera frustum r %.3f t %.3f; settings world %.2f first %.2f",
                    hook::field<float>(player, game::kPlayerWorldFov), hook::field<float>(player, game::kPlayerFirstPersonFov), f ? f[1] : 0.0f, f ? f[2] : 0.0f,
                    hook::field<float>(reinterpret_cast<void*>(game::kWorldFovSetting), 4), hook::field<float>(reinterpret_cast<void*>(game::kFirstPersonFovSetting), 4));
            }
        }
        bridge->publishArms(pose);
        published = true;
    } else if (space == proto::kArmsThirdPerson && sampleThird(player, pose)) {
        pose.valid = space;
        bridge->publishArms(pose);
        published = true;
    } else if (published) {
        bridge->publishArms(pose);
        published = false;
    }
}

namespace {
float offsetRot[3]{}, offsetShift[3]{};
bool thirdShown = false;
void* thirdRootSeen = nullptr;
std::uint32_t thirdRootFlags = 0;
void* movedModel = nullptr;  // the weapon model whose local transform holds the avatar's hand
bool switchedPov = false;
void* pendingRoot = nullptr;  // set by holdInThirdPerson, placed by applyHeld just before drawing
bool pendingMelee = false;      // orientation from Minecraft's hand pose instead of New Vegas' bone
bool pendingFirst = false;      // the first-person model at Minecraft's first-person item pose
// New Vegas melee models (grip at the origin) in Minecraft's held-item frame: [0] first, [1] third person.
// First person: found live 2026-10-02 with the machete, like Minecraft's sword display transform
// (blade flat, rising from the bottom right corner); third person, the blade up and out of the fist.
float meleeRot[2][3]{{0.0f, 0.0f, 25.0f}, {90.0f, 0.0f, 55.0f}}, meleeShift[2][3]{{-0.15f, 0.25f, 0.07f}, {}};
float meleeSize = 1.4f;  // first-person scale on top of the hand space's (probe msize)
ULONGLONG logHeldUntil = 0;  // probe heldlog: log every frame until then
float pendingHand[12]{}, pendingFeet[3]{};     // New Vegas was put in third person for the avatar's weapon
// PlayerCharacter::ToggleFirstPerson(bool toFirst) (0x950110, thiscall): sets +0x64C and swaps the
// first- and third-person models. In first person New Vegas neither updates nor draws the
// third-person body, so the weapon in the avatar's hand needs New Vegas in third person.
void setThirdPerson(void* player, bool third) {
    const bool now = hook::field<std::uint8_t>(player, game::kPlayerIsThirdPerson) != 0;
    if (now != third) reinterpret_cast<void(__thiscall*)(void*, bool)>(0x950110)(player, !third);
}
inline bool fastReadable(const void* p, std::size_t) { return reinterpret_cast<std::uintptr_t>(p) > 0x10000; }
void* findChild(void* obj, const char* want, const char* parentName, int depth) {
    if (!obj || depth > 40 || !fastReadable(obj, 0xA8)) return nullptr;
    if (named(obj, want)) {
        void* parent = hook::field<void*>(obj, 0x18);
        if (!parentName || (parent && fastReadable(parent, 0x10) && named(parent, parentName))) return obj;
    }
    auto** vt = hook::field<void**>(obj, 0);
    if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) return nullptr;
    auto** children = hook::field<void**>(obj, 0xA0);
    const auto count = hook::field<unsigned short>(obj, 0xA6);
    if (count > 256 || !fastReadable(children, count * 4)) return nullptr;
    for (unsigned i = 0; i < count; ++i)
        if (void* found = findChild(children[i], want, parentName, depth + 1)) return found;
    return nullptr;
}
// findChild walks the whole body (hundreds of nodes, each checked with VirtualQuery): about 6 ms
// for the third-person one. The bones found are kept, checked each use and searched again
// every second (the model can be rebuilt: equipment, a new weapon).
constexpr const char* kWeaponBone = "Weapon";
constexpr const char* kRightHandBone = "Bip01 R Hand";
struct FoundChild { void* root; const char* want; const char* parent; void* node; ULONGLONG at; };
std::vector<FoundChild> foundChildren;
void* findChildCached(void* root, const char* want, const char* parentName) {
    const auto now = GetTickCount64();
    for (auto& f : foundChildren) {
        if (f.root != root || f.want != want || f.parent != parentName) continue;
        if (now - f.at < 1000 && (!f.node || (hook::readable(f.node, 0xA8) && named(f.node, want)))) return f.node;
        f.node = findChild(root, want, parentName, 0);
        f.at = now;
        return f.node;
    }
    if (foundChildren.size() > 32) foundChildren.clear();
    foundChildren.push_back({root, want, parentName, findChild(root, want, parentName, 0), now});
    return foundChildren.back().node;
}
// TESObjectREFR::RenderState (+0x64) -> the reference's own node (+0x14): the player's
// third-person body even while New Vegas renders it in first person.
void* thirdPersonRoot(void* player) {
    void* rs = hook::field<void*>(player, 0x64);
    void* root = rs && hook::readable(rs, 0x18) ? hook::field<void*>(rs, 0x14) : nullptr;
    return root && hook::readable(root, 0xA8) ? root : nullptr;
}
void setCull(void* obj, bool cull) {
    const auto flags = hook::field<std::uint32_t>(obj, 0x30);
    hook::setField<std::uint32_t>(obj, 0x30, cull ? flags | 1 : flags & ~1u);
}
// Only the skeleton stays: everything beside "Bip01" (skin, armour, head) is culled.
void showSkeletonOnly(void* root, bool on) {
    auto** children = hook::field<void**>(root, 0xA0);
    const auto count = hook::field<unsigned short>(root, 0xA6);
    if (count > 64 || !hook::readable(children, count * 4)) return;
    for (unsigned i = 0; i < count; ++i) {
        void* child = children[i];
        if (!child || !hook::readable(child, 0x34) || named(child, "Bip01")) continue;
        setCull(child, on);
    }
}
// New Vegas doesn't refresh the third-person body's world transforms while the puppet drives
// the player, so the weapon subtree's are computed here: world = parent world * local.
void propagate(void* node, const float rot[9], const float pos[3], float scale, int depth) {
    if (!node || depth > 20 || !hook::readable(node, 0xA8)) return;
    const auto* lr = reinterpret_cast<const float*>(static_cast<char*>(node) + 0x34);
    const auto* lp = reinterpret_cast<const float*>(static_cast<char*>(node) + 0x58);
    const float ls = hook::field<float>(node, 0x64);
    auto* wr = reinterpret_cast<float*>(static_cast<char*>(node) + kWorldRot);
    auto* wp = reinterpret_cast<float*>(static_cast<char*>(node) + kWorldPos);
    float r[9], p[3];
    for (int i = 0; i < 3; ++i) {
        p[i] = pos[i] + scale * (rot[i * 3 + 0] * lp[0] + rot[i * 3 + 1] * lp[1] + rot[i * 3 + 2] * lp[2]);
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = rot[i * 3 + 0] * lr[0 * 3 + j] + rot[i * 3 + 1] * lr[1 * 3 + j] + rot[i * 3 + 2] * lr[2 * 3 + j];
    }
    // The world bound (NiAVObject +0x20 -> NiSphere {centre, radius}) moves rigidly with the node,
    // or culling would test the old place.
    auto* bound = hook::field<float*>(node, 0x20);
    if (bound && hook::readable(bound, 16)) {
        const float os = hook::field<float>(node, 0x98), ns = scale * ls;
        const float d[3] = {bound[0] - wp[0], bound[1] - wp[1], bound[2] - wp[2]};
        float c[3];
        for (int i = 0; i < 3; ++i) c[i] = (wr[0 * 3 + i] * d[0] + wr[1 * 3 + i] * d[1] + wr[2 * 3 + i] * d[2]) / (os > 1e-4f ? os : 1.0f);
        for (int i = 0; i < 3; ++i) bound[i] = p[i] + ns * (r[i * 3 + 0] * c[0] + r[i * 3 + 1] * c[1] + r[i * 3 + 2] * c[2]);
    }
    std::memcpy(wr, r, sizeof(r));
    std::memcpy(wp, p, sizeof(p));
    hook::setField<float>(node, 0x98, scale * ls);
    auto** vt = hook::field<void**>(node, 0);
    if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(node)) return;
    auto** children = hook::field<void**>(node, 0xA0);
    const auto count = hook::field<unsigned short>(node, 0xA6);
    if (count > 256 || !hook::readable(children, count * 4)) return;
    for (unsigned i = 0; i < count; ++i) propagate(children[i], r, p, scale * ls, depth + 1);
}
// The weapon model (first child of the "Weapon" bone) back on the bone.
void resetModel() {
    if (movedModel && hook::readable(movedModel, 0x68)) {
        auto* r = reinterpret_cast<float*>(static_cast<char*>(movedModel) + 0x34);
        const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(r, identity, sizeof(identity));
        auto* p = reinterpret_cast<float*>(static_cast<char*>(movedModel) + 0x58);
        p[0] = p[1] = p[2] = 0;
        hook::setField<float>(movedModel, 0x64, 1.0f);
    }
    movedModel = nullptr;
}
// m = m * R(axis, deg): turns the frame about its own axis.
void rotate(float m[3][3], int axis, float deg) {
    const float a = deg * 0.0174532925f, c = std::cos(a), s = std::sin(a);
    float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const int i = (axis + 1) % 3, j = (axis + 2) % 3;
    r[i][i] = c; r[i][j] = -s; r[j][i] = s; r[j][j] = c;
    float out[3][3];
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y) out[x][y] = m[x][0] * r[0][y] + m[x][1] * r[1][y] + m[x][2] * r[2][y];
    std::memcpy(m, out, sizeof(out));
}
}

// The third-person arm bones for Minecraft's avatar: shoulder and wrist relative to the feet, in
// Minecraft axes and blocks (the avatar's arms point the same way in the world).
bool sampleThird(void* player, proto::ArmPose& pose) {
    bool ok = false;
    guard::run([&] {
        void* root = thirdPersonRoot(player);
        if (!root) return;
        const auto feet = game::position(player);
        for (int a = 0; a < 2; ++a) {
            void* upper = findChildCached(root, kBoneNames[a][0], nullptr);
            void* hand = findChildCached(root, kBoneNames[a][2], nullptr);
            if (!upper || !hand) return;
            auto toMc = [&](void* bone, float* out) {
                const auto* p = reinterpret_cast<const float*>(static_cast<char*>(bone) + kWorldPos);
                const float k = 1.0f / float(proto::kUnitsPerBlock);
                out[0] = (p[0] - feet.x) * k; out[1] = (p[2] - feet.z) * k; out[2] = -(p[1] - feet.y) * k;
            };
            toMc(upper, pose.arms[a].start);
            toMc(hand, pose.arms[a].hand);
        }
        ok = true;
    });
    return ok;
}

void setWeaponOffset(const float rotDeg[3], const float shift[3]) {
    std::memcpy(offsetRot, rotDeg, sizeof(offsetRot));
    std::memcpy(offsetShift, shift, sizeof(offsetShift));
}

void leaveThirdPerson(void* player) {
    guard::run([&] {
        // ToggleFirstPerson(true) even when the flag already says first: idle/vanity time can leave the third-person body drawn.
        if (player) { reinterpret_cast<void(__thiscall*)(void*, bool)>(0x950110)(player, true); log::line("arms: Minecraft attached: New Vegas back to first person"); }
        switchedPov = false;
    });
}

void holdInThirdPerson(void* player, bool active, const float hand[12], const float feet[3], bool melee) {
    guard::run([&] {
        void* root = player ? thirdPersonRoot(player) : nullptr;
        if (player && active != switchedPov) {
            setThirdPerson(player, active);
            switchedPov = active;
            log::line("arms: New Vegas %s person for the avatar's weapon", active ? "third" : "first");
        } else if (player && !active && hook::field<std::uint8_t>(player, game::kPlayerIsThirdPerson)) {
            // Left in third person (a core reload forgot switchedPov): nothing wants it.
            setThirdPerson(player, false);
        }
        if (!pendingFirst) pendingRoot = nullptr;
        if (!active || !root) {
            if (!pendingFirst) resetModel();
            if (thirdShown && root && root == thirdRootSeen) {
                showSkeletonOnly(root, false);
                hook::setField<std::uint32_t>(root, 0x30, thirdRootFlags);
            }
            thirdShown = false;
            thirdRootSeen = nullptr;
            return;
        }
        if (!thirdShown || root != thirdRootSeen) {
            thirdRootSeen = root;
            thirdRootFlags = hook::field<std::uint32_t>(root, 0x30);
            thirdShown = true;
            log::line("arms: third-person body %p flags %X", root, thirdRootFlags);
        }
        setCull(root, false);
        showSkeletonOnly(root, !state().showNvArms);  // probe nvarms 1: the whole body, for debugging
        pendingRoot = root;
        pendingMelee = melee;
        pendingFirst = false;
        std::memcpy(pendingHand, hand, sizeof(pendingHand));
        std::memcpy(pendingFeet, feet, sizeof(pendingFeet));
    });
}

void holdInFirstPerson(void* player, bool active, const float hand[12]) {
    guard::run([&] {
        if (!active || !player) {
            if (pendingFirst) { pendingRoot = nullptr; pendingFirst = false; resetModel(); }
            return;
        }
        void* root = hook::field<void*>(player, game::kPlayerFirstPersonNode);
        if (!root || !hook::readable(root, 0xA8)) return;
        pendingRoot = root;
        pendingFirst = true;
        pendingMelee = true;
        std::memcpy(pendingHand, hand, sizeof(pendingHand));
    });
}

void setMeleeSize(float size) { meleeSize = size; }
void logHeld(unsigned ms) { logHeldUntil = GetTickCount64() + ms; }
void setMeleeOffset(int view, const float rotDeg[3], const float shift[3]) {
    const int v = view == 1 ? 0 : 1;
    std::memcpy(meleeRot[v], rotDeg, sizeof(meleeRot[v]));
    std::memcpy(meleeShift[v], shift, sizeof(meleeShift[v]));
}

void applyHeld() {
    if (!pendingRoot) return;
    guard::run([&] {
        void* root = pendingRoot;
        const float* hand = pendingHand;
        const float* feet = pendingFeet;
        // The "Weapon" bone is animated (New Vegas rewrites its local transform every frame); the
        // weapon model under it isn't, so the model's local transform carries the offset to the
        // avatar's hand. It follows the bone through New Vegas' own update (one frame late).
        void* parent = findChildCached(root, kWeaponBone, kRightHandBone);
        auto** children = parent ? hook::field<void**>(parent, 0xA0) : nullptr;
        const auto count = parent ? hook::field<unsigned short>(parent, 0xA6) : 0;
        void* weapon = count > 0 && count < 16 && hook::readable(children, count * 4) ? children[0] : nullptr;
        if (!weapon || !hook::readable(weapon, 0x9C)) return;
        if (weapon != movedModel) { resetModel(); movedModel = weapon; }
        // Minecraft (x east, y up, z south) -> New Vegas (x east, y north, z up), 70 units per block.
        auto toNv = [](const float* v, float* out) { out[0] = v[0]; out[1] = -v[2]; out[2] = v[1]; };
        float axes[3][3], want[3], modelScale = 1.0f;
        if (pendingFirst) {
            // Minecraft's first-person hand space (x right, y up, z backwards, blocks, 70 degree FOV)
            // -> New Vegas' first-person camera space (NvArmPose's mapping, inverted) -> world.
            float camRot[3][3], camPos[3];
            if (!cameraTransform(camRot, camPos)) return;
            const float aspect = state().viewportH > 0 ? float(state().viewportW) / float(state().viewportH) : 16.0f / 9.0f;
            const float fit = std::tan(35.0f * 0.0174532925f) / (tanHalfFovX(game::player()) / aspect);
            constexpr float kArmScale = 0.08f;  // NvArmPose: blocks per New Vegas unit
            auto toWorldDir = [&](const float* v, float* out) {  // camera (x right, y up, z back) -> world
                for (int i = 0; i < 3; ++i) out[i] = camRot[i][2] * v[0] + camRot[i][1] * v[1] - camRot[i][0] * v[2];
            };
            float cols[3][3];
            for (int j = 0; j < 3; ++j) {
                const float d[3] = {hand[j * 3] / fit, hand[j * 3 + 1] / fit, hand[j * 3 + 2]};
                toWorldDir(d, cols[j]);
            }
            // Gram-Schmidt (the FOV fit isn't uniform): x, then y, z = x cross y.
            auto norm = [](float* v) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) for (int i = 0; i < 3; ++i) v[i] /= l; };
            norm(cols[0]);
            const float dot = cols[1][0] * cols[0][0] + cols[1][1] * cols[0][1] + cols[1][2] * cols[0][2];
            for (int i = 0; i < 3; ++i) cols[1][i] -= dot * cols[0][i];
            norm(cols[1]);
            cols[2][0] = cols[0][1] * cols[1][2] - cols[0][2] * cols[1][1];
            cols[2][1] = cols[0][2] * cols[1][0] - cols[0][0] * cols[1][2];
            cols[2][2] = cols[0][0] * cols[1][1] - cols[0][1] * cols[1][0];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) axes[i][j] = cols[j][i];
            for (int k = 0; k < 3; ++k) rotate(axes, k, meleeRot[0][k]);
            // The offset is in the item's frame (like Minecraft's display transform), so it turns with the swing.
            const float* sh = meleeShift[0];
            float at[3];
            for (int i = 0; i < 3; ++i) at[i] = hand[9 + i] + hand[i] * sh[0] + hand[3 + i] * sh[1] + hand[6 + i] * sh[2];
            const float o[3] = {at[0] / (fit * kArmScale), at[1] / (fit * kArmScale), at[2] / kArmScale};
            float ow[3];
            toWorldDir(o, ow);
            for (int i = 0; i < 3; ++i) want[i] = camPos[i] + ow[i];
            // The hand space is NvArmPose's (1 block = 1 / kArmScale units), not the world's (70).
            modelScale = 1.0f / (kArmScale * float(proto::kUnitsPerBlock)) * meleeSize;
        } else {
            // Ranged: orientation from New Vegas' animated Weapon bone (aiming, firing and reloading
            // poses of that weapon; the avatar's arms follow the same skeleton). Melee: from the
            // avatar's hand (Minecraft's swing). Position: the avatar's hand.
            const auto* pr0 = reinterpret_cast<const float*>(static_cast<char*>(parent) + kWorldRot);
            for (int j = 0; j < 3; ++j) {
                float v[3];
                toNv(hand + j * 3, v);
                const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                for (int i = 0; i < 3; ++i)
                    axes[i][j] = pendingMelee ? (len > 1e-6f ? v[i] / len : (i == j ? 1.0f : 0.0f)) : pr0[i * 3 + j];
            }
            const float* rot = pendingMelee ? meleeRot[1] : offsetRot;
            const float* sh = pendingMelee ? meleeShift[1] : offsetShift;
            for (int k = 0; k < 3; ++k) rotate(axes, k, rot[k]);
            float origin[3];
            toNv(hand + 9, origin);
            for (int i = 0; i < 3; ++i)
                want[i] = feet[i] + float(proto::kUnitsPerBlock) * (origin[i] + axes[i][0] * sh[0] + axes[i][1] * sh[1] + axes[i][2] * sh[2]);
        }
        // world = parent * local  ->  local = parent^-1 * world (the parent's rotation is orthonormal)
        const auto* pr = reinterpret_cast<const float*>(static_cast<char*>(parent) + kWorldRot);
        const auto* pp = reinterpret_cast<const float*>(static_cast<char*>(parent) + kWorldPos);
        const float ps = hook::field<float>(parent, 0x98);
        const float inv = ps > 1e-4f ? 1.0f / ps : 1.0f;
        auto* lr = reinterpret_cast<float*>(static_cast<char*>(weapon) + 0x34);
        auto* lp = reinterpret_cast<float*>(static_cast<char*>(weapon) + 0x58);
        const float d[3] = {want[0] - pp[0], want[1] - pp[1], want[2] - pp[2]};
        for (int i = 0; i < 3; ++i) {
            lp[i] = (pr[0 * 3 + i] * d[0] + pr[1 * 3 + i] * d[1] + pr[2 * 3 + i] * d[2]) * inv;
            for (int j = 0; j < 3; ++j) lr[i * 3 + j] = pr[0 * 3 + i] * axes[0][j] + pr[1 * 3 + i] * axes[1][j] + pr[2 * 3 + i] * axes[2][j];
        }
        hook::setField<float>(weapon, 0x64, inv * modelScale);
        propagate(weapon, pr, pp, ps > 1e-4f ? ps : 1.0f, 0);
        static ULONGLONG loggedAt = 0;
        if (GetTickCount64() - loggedAt > 5000 || GetTickCount64() < logHeldUntil) {
            loggedAt = GetTickCount64();
            const auto* wp = reinterpret_cast<const float*>(static_cast<char*>(weapon) + kWorldPos);
            log::line("arms: weapon %p want %.0f %.0f %.0f world %.0f %.0f %.0f hand bone %.0f %.0f %.0f feet %.0f %.0f %.0f", weapon, want[0], want[1], want[2],
                wp[0], wp[1], wp[2], pp[0], pp[1], pp[2], feet[0], feet[1], feet[2]);
            log::line("arms: hand x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) z(%.2f %.2f %.2f) at %.3f %.3f %.3f", hand[0], hand[1], hand[2], hand[3], hand[4], hand[5], hand[6], hand[7], hand[8],
                hand[9], hand[10], hand[11]);
        }
    });
}

void reportThird(void* player) {
    guard::run([&] {
        void* root = player ? thirdPersonRoot(player) : nullptr;
        if (!root) { log::line("bones3: no third-person body"); return; }
        const auto here = game::position(player);
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 3; ++b) {
                void* bone = findChild(root, kBoneNames[a][b], nullptr, 0);
                if (!bone) continue;
                const auto* p = reinterpret_cast<const float*>(static_cast<char*>(bone) + kWorldPos);
                log::line("bones3 %s %.1f %.1f %.1f", kBoneNames[a][b], p[0] - here.x, p[1] - here.y, p[2] - here.z);
            }
    });
}

void reportAnim(void* player) {
    guard::run([&] {
        void* root = thirdPersonRoot(player);
        void* first = hook::field<void*>(player, 0x690);
        log::line("anim: third root %p, first AnimData %p", root, first);
        // The third-person AnimData: a pointer, in the actor's process, to a block whose +8 is the root.
        void* process = hook::field<void*>(player, 0x68);
        void* third = nullptr;
        for (std::size_t off = 0; process && off < 0x400 && hook::readable(static_cast<char*>(process) + off, 4); off += 4) {
            void* p = hook::field<void*>(process, off);
            if (p && hook::readable(p, 0x10) && hook::field<void*>(p, 8) == root) {
                log::line("anim: process %p +%03zX -> %p", process, off, p);
                third = p;
            }
        }
        for (void* d : {first, third}) {
            if (!d || !hook::readable(d, 0x140)) continue;
            for (std::size_t off = 0; off < 0x140; off += 16)
                log::line("anim %p +%03zX %08X %08X %08X %08X | %g %g %g %g", d, off, hook::field<std::uint32_t>(d, off), hook::field<std::uint32_t>(d, off + 4),
                    hook::field<std::uint32_t>(d, off + 8), hook::field<std::uint32_t>(d, off + 12), hook::field<float>(d, off), hook::field<float>(d, off + 4),
                    hook::field<float>(d, off + 8), hook::field<float>(d, off + 12));
        }
    });
}

namespace {
// VirtualQuery costs ~0.1 ms in New Vegas' fragmented address space: the per-frame weapon walk is inside guard::run, which catches a bad pointer.
// NiSkinInstance {+0x08 NiSkinData*, +0x14 NiAVObject** bones, +0x1C bone count}; NiSkinData
// {+0x40 BoneData*, +0x44 count}; BoneData (0x4C bytes) {+0x00 NiTransform skinToBone (rot 3x3,
// pos, scale), +0x44 {u16 vertex, f32 weight}*, +0x48 u16 count} (TESReloaded's GameNi.h).
bool skinnedPositions(void* skin, const float* verts, unsigned count, std::vector<float>& world) {
    if (!fastReadable(skin, 0x20)) return false;
    void* data = hook::field<void*>(skin, 0x08);
    auto** bones = hook::field<void**>(skin, 0x14);
    const unsigned boneCount = hook::field<std::uint32_t>(skin, 0x1C);
    if (!data || !fastReadable(data, 0x48) || boneCount == 0 || boneCount > 256 || !fastReadable(bones, boneCount * 4)) return false;
    const auto* boneData = hook::field<const unsigned char*>(data, 0x40);
    if (hook::field<std::uint32_t>(data, 0x44) != boneCount || !fastReadable(boneData, boneCount * 0x4C)) return false;
    for (unsigned b = 0; b < boneCount; ++b) {
        void* bone = bones[b];
        const unsigned char* bd = boneData + b * 0x4C;
        const auto* weights = *reinterpret_cast<const unsigned char* const*>(bd + 0x44);
        const unsigned n = *reinterpret_cast<const unsigned short*>(bd + 0x48);
        if (!bone || !fastReadable(bone, 0x9C) || !n || !fastReadable(weights, n * 8)) continue;
        const auto* sr = reinterpret_cast<const float*>(bd);
        const auto* sp = reinterpret_cast<const float*>(bd + 0x24);
        const float ss = *reinterpret_cast<const float*>(bd + 0x30);
        const auto* br = reinterpret_cast<const float*>(static_cast<char*>(bone) + kWorldRot);
        const auto* bp = reinterpret_cast<const float*>(static_cast<char*>(bone) + kWorldPos);
        const float bs = hook::field<float>(bone, 0x98);
        for (unsigned k = 0; k < n; ++k) {
            const unsigned v = *reinterpret_cast<const unsigned short*>(weights + k * 8);
            const float w = *reinterpret_cast<const float*>(weights + k * 8 + 4);
            if (v >= count || !(w > 0.0f)) continue;
            float s[3];  // skin space -> bone space
            for (int i = 0; i < 3; ++i) s[i] = sp[i] + ss * (sr[i * 3] * verts[v * 3] + sr[i * 3 + 1] * verts[v * 3 + 1] + sr[i * 3 + 2] * verts[v * 3 + 2]);
            for (int i = 0; i < 3; ++i) world[v * 3 + i] += w * (bp[i] + bs * (br[i * 3] * s[0] + br[i * 3 + 1] * s[1] + br[i * 3 + 2] * s[2]));
        }
    }
    return true;
}
void depthTriangles(void* obj, const float cr[3][3], const float cp[3], std::vector<float>& out, int depth) {
    if (!obj || depth > 24 || out.size() >= 360000 || !fastReadable(obj, 0xA8)
            || (hook::field<std::uint32_t>(obj, 0x30) & 1)) return;
    auto** vt = hook::field<void**>(obj, 0);
    if (reinterpret_cast<void*(__thiscall*)(void*)>(vt[3])(obj)) {
        auto** children = hook::field<void**>(obj, 0xA0);
        unsigned count = hook::field<unsigned short>(obj, 0xA6);
        if (count > 256 || !fastReadable(children, count * 4)) return;
        for (unsigned i = 0; i < count; ++i) depthTriangles(children[i], cr, cp, out, depth + 1);
        return;
    }
    if (!reinterpret_cast<void*(__thiscall*)(void*)>(vt[6])(obj) || !fastReadable(obj, 0xC0)) return;
    void* data = hook::field<void*>(obj, 0xB8);
    if (!data || !fastReadable(data, 0x50)) return;
    const auto table = hook::field<std::uintptr_t>(data, 0);
    if (table != 0x109DAC4 && table != 0x109D8AC) return;
    unsigned count = hook::field<unsigned short>(data, 8);
    auto* verts = hook::field<const float*>(data, 0x20);
    if (!count || !fastReadable(verts, count * 12)) return;
    // World positions: rigid geometry through its own world transform; skinned geometry (most New
    // Vegas first-person weapons) as the weighted sum over its bones of bone world * skin-to-bone.
    static std::vector<float> world;
    world.assign(std::size_t(count) * 3, 0.0f);
    if (void* skin = hook::field<void*>(obj, 0xBC)) {
        if (!skinnedPositions(skin, verts, count, world)) return;
    } else {
        const auto* r = reinterpret_cast<const float*>(static_cast<char*>(obj) + kWorldRot);
        const auto* p = reinterpret_cast<const float*>(static_cast<char*>(obj) + kWorldPos);
        const float scale = hook::field<float>(obj, 0x98);
        for (unsigned v = 0; v < count; ++v)
            for (int i = 0; i < 3; ++i)
                world[v * 3 + i] = p[i] + scale * (r[i * 3] * verts[v * 3] + r[i * 3 + 1] * verts[v * 3 + 1] + r[i * 3 + 2] * verts[v * 3 + 2]);
    }
    auto triangle = [&](unsigned a, unsigned b, unsigned c) {
        if (a >= count || b >= count || c >= count || a == b || b == c || a == c || out.size() >= 360000) return;
        for (unsigned idx : {a, b, c}) {
            float d[3];
            for (int i = 0; i < 3; ++i) d[i] = world[idx * 3 + i] - cp[i];
            for (int axis : {2, 1, 0}) out.push_back(cr[0][axis] * d[0] + cr[1][axis] * d[1] + cr[2][axis] * d[2]);
        }
    };
    if (table == 0x109DAC4) {
        unsigned n = hook::field<unsigned short>(data, 0x40);
        auto* idx = hook::field<const unsigned short*>(data, 0x48);
        if (!fastReadable(idx, n * 6)) return;
        for (unsigned i = 0; i < n; ++i) triangle(idx[i * 3], idx[i * 3 + 1], idx[i * 3 + 2]);
    } else {
        unsigned n = hook::field<unsigned short>(data, 0x44);
        auto* lengths = hook::field<const unsigned short*>(data, 0x48);
        auto* idx = hook::field<const unsigned short*>(data, 0x4C);
        if (n > 1024 || !fastReadable(lengths, n * 2)) return;
        unsigned total = 0;
        for (unsigned i = 0; i < n; ++i) total += lengths[i];
        if (!fastReadable(idx, total * 2)) return;
        for (unsigned i = 0; i < n; ++i) {
            for (unsigned k = 0; k + 2 < lengths[i]; ++k) triangle(idx[k], idx[k + 1], idx[k + 2]);
            idx += lengths[i];
        }
    }
}
}
bool weaponDepth(std::vector<float>& triangles) {
    triangles.clear();
    bool ok = false;
    guard::run([&] {
        float cr[3][3], cp[3];
        if (!find(game::player()) || !cameraTransform(cr, cp)) return;
        void* weapon = findChild(bones[0][2], "Weapon", "Bip01 R Hand", 0);
        if (!weapon) return;
        depthTriangles(weapon, cr, cp, triangles, 0);
        ok = !triangles.empty();
    });
    if (!ok) triangles.clear();
    return ok;
}

void clear() {
    movedModel = nullptr;
    pendingFirst = false;
    pendingRoot = nullptr;
    switchedPov = false;
    thirdShown = false;
    thirdRootSeen = nullptr;
    cachedRoot = nullptr;
    foundChildren.clear();
    std::memset(bones, 0, sizeof(bones));
    camera1st = nullptr;
}
}
