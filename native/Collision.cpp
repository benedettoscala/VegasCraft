#include "Collision.h"
#include "Game.h"
#include "Dig.h"
#include "Guard.h"
#include "Havok.h"
#include "Log.h"
#include "Rtti.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>
#include <string>

// Ported from SkyCraft's skse/src/Collision.cpp (MIT): the voxelizer, triangulation and message
// layout are SkyCraft's; the body cache and shape walk are New Vegas specific.
namespace vegas {
namespace {
constexpr int kRadius = 5;   // regions around the player horizontally
constexpr int kBelow = 3;
constexpr int kAbove = 2;
constexpr int kGrid = Collision::kRegionSize * 8;  // voxels per region edge
constexpr std::uint64_t kRefreshNearMs = 1000;     // re-send regions next to the player (doors)
constexpr double kExtractBudgetMs = 3.0;
constexpr int kMaxHarvestsPerFrame = 3;
constexpr float kExtractRadius = 128.0f;           // blocks from the player to a body's origin
constexpr float kSteepMin = 0.1f;
constexpr float kSteepMax = 0.643f;
constexpr float kPrimMargin = 0.5f;
float blocksPerHavok = 1.0f / (game::kHavokScale * 70.0f);

bool includedLayer(std::uint32_t layer) {
    switch (layer) {
    case 1:  // static
    case 2:  // anim static (doors, gates)
    case 3:  // transparent
    case 9:  // trees
    case 10: // props
    case 13: // terrain
    case 17: // ground
    case 26: // transparent small
    case 27: // invisible wall
    case 28: // transparent small anim
        return true;
    default:
        return false;
    }
}
bool finite(const float* v, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1.0e7f) return false;
    return true;
}
// Havok transform: rotation columns at [0..2], [4..6], [8..10]; translation at [12..14].
void xfPoint(const float* xf, const float* p, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = xf[i] * p[0] + xf[4 + i] * p[1] + xf[8 + i] * p[2] + xf[12 + i];
}
void xfDir(const float* xf, const float* d, float* out) {
    for (int i = 0; i < 3; ++i) out[i] = xf[i] * d[0] + xf[4 + i] * d[1] + xf[8 + i] * d[2];
}
void xfCompose(const float* parent, const float* child, float* out) {
    for (int c = 0; c < 3; ++c) { xfDir(parent, child + c * 4, out + c * 4); out[c * 4 + 3] = 0; }
    xfPoint(parent, child + 12, out + 12);
    out[15] = 1;
}
bool xfValid(const float* xf) {
    if (!finite(xf, 16)) return false;
    for (int c = 0; c < 3; ++c) {
        const float* col = xf + c * 4;
        if (std::fabs(col[0] * col[0] + col[1] * col[1] + col[2] * col[2] - 1.0f) > 0.05f) return false;
    }
    return true;
}
void hkToMc(const float* h, float* out) { out[0] = h[0] * blocksPerHavok; out[1] = h[2] * blocksPerHavok; out[2] = -h[1] * blocksPerHavok; }
void hkDirToMc(const float* h, float* out) { out[0] = h[0]; out[1] = h[2]; out[2] = -h[1]; }
const float* vec(const void* base, std::size_t offset) {
    return reinterpret_cast<const float*>(static_cast<const unsigned char*>(base) + offset);
}
bool overlaps(const float* alo, const float* ahi, const float* blo, const float* bhi) {
    return alo[0] <= bhi[0] && ahi[0] >= blo[0] && alo[1] <= bhi[1] && ahi[1] >= blo[1] && alo[2] <= bhi[2] && ahi[2] >= blo[2];
}
std::uint64_t regionKey(int x, int y, int z) {
    return (std::uint64_t(std::uint32_t(x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(y) & 0x1FFFFF) << 21) | (std::uint32_t(z) & 0x1FFFFF);
}
int floorDiv(int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
void grow(Collision::Geometry& g, const float* p) {
    for (int i = 0; i < 3; ++i) { g.lo[i] = std::min(g.lo[i], p[i]); g.hi[i] = std::max(g.hi[i], p[i]); }
}

// ---- triangle / box overlap (Akenine-Moller SAT), voxel units ---------------------------------
inline void sub(const float* a, const float* b, float* o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
inline void cross(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
inline float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool axisTest(const float* v0, const float* v1, const float* v2, const float* axis, float h) {
    const float p0 = dot(v0, axis), p1 = dot(v1, axis), p2 = dot(v2, axis);
    const float mn = std::min({p0, p1, p2}), mx = std::max({p0, p1, p2});
    const float r = h * (std::fabs(axis[0]) + std::fabs(axis[1]) + std::fabs(axis[2]));
    return !(mn > r || mx < -r);
}
bool triBoxOverlap(const float* c, float h, const float* ta, const float* tb, const float* tc, const float* n) {
    float v0[3], v1[3], v2[3];
    sub(ta, c, v0); sub(tb, c, v1); sub(tc, c, v2);
    for (int i = 0; i < 3; ++i) {
        const float mn = std::min({v0[i], v1[i], v2[i]}), mx = std::max({v0[i], v1[i], v2[i]});
        if (mn > h || mx < -h) return false;
    }
    const float d = dot(n, v0);
    if (std::fabs(d) > h * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]))) return false;
    float e[3][3];
    sub(v1, v0, e[0]); sub(v2, v1, e[1]); sub(v0, v2, e[2]);
    static constexpr float kAxes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (auto& edge : e) for (auto& unit : kAxes) {
        float axis[3];
        cross(edge, unit, axis);
        if (!axisTest(v0, v1, v2, axis, h)) return false;
    }
    return true;
}

// ---- shape walk ----------------------------------------------------------------------------------
struct Walker {
    Collision::Geometry& out;
    std::uint32_t flags;
    bool terrain;
    std::set<std::string>& unknown;
    int faults = 0;

    void triangle(const void* shape, const float* xf) {
        Collision::Tri tri{};
        for (int v = 0; v < 3; ++v) {
            float w[3];
            xfPoint(xf, vec(shape, 0x20 + v * 0x10), w);
            hkToMc(w, tri.v + v * 3);
        }
        if (!finite(tri.v, 9)) return;
        if (terrain) {
            // The land is a height field: its outside is up, whatever the winding says.
            const float ux = tri.v[3] - tri.v[0], uz = tri.v[5] - tri.v[2];
            const float wx = tri.v[6] - tri.v[0], wz = tri.v[8] - tri.v[2];
            if (uz * wx - ux * wz < 0.0f) std::swap_ranges(tri.v + 3, tri.v + 6, tri.v + 6);
        }
        tri.flags = flags;
        if (terrain) {
            const float x = (tri.v[0] + tri.v[3] + tri.v[6]) * float(proto::kUnitsPerBlock) / 3;
            const float y = -(tri.v[2] + tri.v[5] + tri.v[8]) * float(proto::kUnitsPerBlock) / 3;
            tri.flags |= proto::kTriTerrain | proto::kTriDiggable |
                (std::uint32_t(dig::landMaterial(x, y)) << proto::kTriMaterialShift);
        }
        out.tris.push_back(tri);
        for (int v = 0; v < 3; ++v) grow(out, tri.v + v * 3);
    }
    void box(const void* shape, const float* xf) {
        const float* half = vec(shape, 0x20);
        const float radius = hook::field<float>(shape, 0x10);
        Collision::Obb obb{};
        const float zero[3] = {0, 0, 0};
        float centre[3];
        xfPoint(xf, zero, centre);
        hkToMc(centre, obb.c);
        for (int i = 0; i < 3; ++i) {
            hkDirToMc(xf + i * 4, obb.axis[i]);
            obb.half[i] = (half[i] + radius) * blocksPerHavok;
        }
        if (!finite(obb.c, 3) || !finite(obb.half, 3)) return;
        obb.flags = flags;
        out.boxes.push_back(obb);
        for (int i = 0; i < 3; ++i) {
            const float ext = std::fabs(obb.axis[0][i]) * obb.half[0] + std::fabs(obb.axis[1][i]) * obb.half[1] + std::fabs(obb.axis[2][i]) * obb.half[2];
            float p[3];
            std::memcpy(p, obb.c, sizeof(p)); p[i] -= ext; grow(out, p);
            std::memcpy(p, obb.c, sizeof(p)); p[i] += ext; grow(out, p);
        }
    }
    void capsule(const void* shape, const float* xf, bool sphere) {
        const float radius = hook::field<float>(shape, 0x10);
        const float zero[3] = {0, 0, 0};
        Collision::Capsule cap{};
        float w[3];
        xfPoint(xf, sphere ? zero : vec(shape, 0x20), w);
        hkToMc(w, cap.a);
        xfPoint(xf, sphere ? zero : vec(shape, 0x30), w);
        hkToMc(w, cap.b);
        cap.r = radius * blocksPerHavok;
        if (!finite(cap.a, 3) || !finite(cap.b, 3) || !std::isfinite(cap.r) || cap.r > 100.0f) return;
        cap.flags = flags;
        out.capsules.push_back(cap);
        for (int i = 0; i < 3; ++i) {
            float lo[3] = {cap.a[0], cap.a[1], cap.a[2]}, hi[3] = {cap.b[0], cap.b[1], cap.b[2]};
            lo[i] = std::min(cap.a[i], cap.b[i]) - cap.r;
            hi[i] = std::max(cap.a[i], cap.b[i]) + cap.r;
            grow(out, lo); grow(out, hi);
        }
    }
    void convexVertices(const void* shape, const float* xf) {
        const auto planes = hook::field<havok::ArrayView>(shape, 0x54);
        const float radius = hook::field<float>(shape, 0x10);
        if (planes.size <= 0 || planes.size > 512 || !hook::readable(planes.data, planes.size * 16)) return;
        Collision::Convex cvx{};
        // Local AABB (centre at 0x30, half extents at 0x20) through the transform.
        const float* half = vec(shape, 0x20);
        const float* centre = vec(shape, 0x30);
        for (int i = 0; i < 3; ++i) { cvx.lo[i] = FLT_MAX; cvx.hi[i] = -FLT_MAX; }
        for (int c = 0; c < 8; ++c) {
            const float p[3] = {centre[0] + ((c & 1) ? half[0] : -half[0]) + ((c & 1) ? radius : -radius),
                                centre[1] + ((c & 2) ? half[1] : -half[1]) + ((c & 2) ? radius : -radius),
                                centre[2] + ((c & 4) ? half[2] : -half[2]) + ((c & 4) ? radius : -radius)};
            float w[3], m[3];
            xfPoint(xf, p, w);
            hkToMc(w, m);
            for (int i = 0; i < 3; ++i) { cvx.lo[i] = std::min(cvx.lo[i], m[i]); cvx.hi[i] = std::max(cvx.hi[i], m[i]); }
        }
        const float* data = reinterpret_cast<const float*>(planes.data);
        for (std::int32_t i = 0; i < planes.size; ++i) {
            const float* p = data + i * 4;
            float nw[3];
            xfDir(xf, p, nw);
            const float dw = p[3] - (nw[0] * xf[12] + nw[1] * xf[13] + nw[2] * xf[14]) - radius;
            float nm[3];
            hkDirToMc(nw, nm);
            cvx.planes.push_back({nm[0], nm[1], nm[2], dw * blocksPerHavok});
        }
        if (!finite(cvx.lo, 3) || !finite(cvx.hi, 3)) return;
        cvx.flags = flags;
        grow(out, cvx.lo); grow(out, cvx.hi);
        out.convexes.push_back(std::move(cvx));
    }
    // Child transform of hkpTransformShape / hkpConvexTransformShape / hkpConvexTranslateShape.
    bool childTransform(const void* shape, const char* name, float* local) {
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(local, identity, sizeof(identity));
        if (std::strcmp(name, ".?AVhkpConvexTranslateShape@@") == 0) {
            std::memcpy(local + 12, vec(shape, 0x20), 3 * sizeof(float));
            return finite(local, 16);
        }
        for (std::size_t off : {std::size_t(0x20), std::size_t(0x30), std::size_t(0x40)}) {
            std::memcpy(local, vec(shape, off), 16 * sizeof(float));
            local[3] = local[7] = local[11] = 0; local[15] = 1;
            if (xfValid(local)) return true;
        }
        return false;
    }
    void walk(const void* shape, const float* xf, int depth) {
        if (!shape || depth > 8 || out.tris.size() > 400000) return;
        const char* name = rtti::fastName(shape);
        if (!std::strcmp(name, ".?AVhkNormalTriangleShape@@") || !std::strcmp(name, ".?AVhkpTriangleShape@@")) { triangle(shape, xf); return; }
        if (!std::strcmp(name, ".?AVhkpBoxShape@@")) { box(shape, xf); return; }
        if (!std::strcmp(name, ".?AVhkpSphereShape@@")) { capsule(shape, xf, true); return; }
        if (!std::strcmp(name, ".?AVhkpCapsuleShape@@")) { capsule(shape, xf, false); return; }
        if (!std::strcmp(name, ".?AVhkpConvexVerticesShape@@") || !std::strcmp(name, ".?AVhkCharControllerShape@@")) { convexVertices(shape, xf); return; }
        if (!std::strcmp(name, ".?AVhkpTransformShape@@") || !std::strcmp(name, ".?AVhkpConvexTransformShape@@") || !std::strcmp(name, ".?AVhkpConvexTranslateShape@@")) {
            const void* child = havok::singleChild(shape);
            alignas(16) float local[16], composed[16];
            if (!child || !childTransform(shape, name, local)) {
                if (unknown.insert(std::string(name) + " transform").second) log::line("collision: %s without a usable child transform", name);
                return;
            }
            xfCompose(xf, local, composed);
            walk(child, composed, depth + 1);
            return;
        }
        if (const void* c = havok::container(shape)) {
            int guardCount = 0;
            alignas(16) unsigned char buffer[havok::kShapeBufferBytes];
            for (auto key = havok::firstKey(c); key != havok::kInvalidKey && guardCount < 200000; key = havok::nextKey(c, key), ++guardCount) {
                const void* child = havok::childShape(c, key, buffer);
                if (child) walk(child, xf, depth + 1);
            }
            return;
        }
        if (const void* child = havok::singleChild(shape)) { walk(child, xf, depth + 1); return; }
        if (unknown.insert(name).second) log::line("collision: shape %s is not handled", *name ? name : "(no RTTI)");
    }
};
std::set<std::string> unknownShapes;
}

Collision& Collision::get() { static Collision instance; return instance; }

void Collision::start(Bridge* bridge) {
    bridge_ = bridge;
    if (running_.exchange(true)) return;
    offsets_.clear();
    for (int dx = -kRadius; dx <= kRadius; ++dx)
        for (int dz = -kRadius; dz <= kRadius; ++dz)
            for (int dy = -kBelow; dy <= kAbove; ++dy) offsets_.push_back({dx, dy, dz});
    std::sort(offsets_.begin(), offsets_.end(), [](const auto& a, const auto& b) {
        return a[0] * a[0] + a[2] * a[2] + a[1] * a[1] * 2 < b[0] * b[0] + b[2] * b[2] + b[1] * b[1] * 2;
    });
    worker_ = std::thread([this] { workerLoop(); });
}
void Collision::stop() {
    if (!running_.exchange(false)) return;
    { std::scoped_lock lock(mutex_); queue_.clear(); }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void Collision::reset(std::uint32_t epoch) {
    epoch_ = epoch;
    harvested_.clear();
    urgent_.clear();
    bodies_.clear();
    landSent_.clear();
    landWork_.active = false;
    settled_ = false;
    nearKnown_ = false;
    std::scoped_lock lock(mutex_);
    queue_.clear();
    Job job;
    job.clear = true;
    job.epoch = epoch;
    queue_.push_back(std::move(job));
    cv_.notify_one();
}
void Collision::extract(const void* body, Body& entry) {
    entry.geometry = Geometry{};
    for (int i = 0; i < 3; ++i) { entry.geometry.lo[i] = FLT_MAX; entry.geometry.hi[i] = -FLT_MAX; }
    Walker walker{entry.geometry, entry.flags, havok::bodyLayer(body) == havok::kLayerGround, unknownShapes};
    const void* shape = entry.shape;
    const float* xf = entry.xf;
    if (!guard::run([&] { walker.walk(shape, xf, 0); })) {
        log::once("collision-fault", "collision: faulted reading a Havok shape (%s); it is skipped", rtti::name(shape));
        entry.geometry = Geometry{};
    }
    entry.valid = true;
}
void Collision::markRegions(const float lo[3], const float hi[3]) {
    if (lo[0] > hi[0]) return;
    const int x0 = floorDiv(static_cast<int>(std::floor(lo[0] - 1)), kRegionSize), x1 = floorDiv(static_cast<int>(std::floor(hi[0] + 1)), kRegionSize);
    const int y0 = floorDiv(static_cast<int>(std::floor(lo[1] - 1)), kRegionSize), y1 = floorDiv(static_cast<int>(std::floor(hi[1] + 1)), kRegionSize);
    const int z0 = floorDiv(static_cast<int>(std::floor(lo[2] - 1)), kRegionSize), z1 = floorDiv(static_cast<int>(std::floor(hi[2] + 1)), kRegionSize);
    if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 512) return;
    for (int x = x0; x <= x1; ++x) for (int y = y0; y <= y1; ++y) for (int z = z0; z <= z1; ++z) {
        if (harvested_.erase(regionKey(x, y, z))) urgent_.push_back({x, y, z});
    }
}
void Collision::update(const Position& player, void* world) {
    if (!world || !running_) return;
    std::vector<clip::Cube> changed;
    dig::takeChanged(changed);
    for (const auto& c : changed) {
        const float lo[3] = {float(c[0]), float(c[1]), float(c[2])};
        const float hi[3] = {lo[0] + 1, lo[1] + 1, lo[2] + 1};
        markRegions(lo, hi);
    }
    ++frame_;
    // Walking all of Havok's bodies costs milliseconds: do it every fourth frame (sooner while something near is still unknown).
    const bool walk = (frame_ & 3) == 0 || !nearKnown_ || !changed.empty();
    int pendingNear = 0;
    bool ok = true;
    if (walk) {
    double spent = 0;  // extraction time this frame
        // Keep the body cache in step with Havok: new and moved bodies are (re)extracted under a
        // time budget, bodies gone from the world are dropped, and the regions they touch are resent.
        std::vector<const void*> stale;
        ok = guard::run([&] {
            havok::forEachBody(world, [&](const void* body, bool) {
                if (!includedLayer(havok::bodyLayer(body))) return;
                const float* xf = havok::bodyTransform(body);
                const float mx = xf[12] * blocksPerHavok, mz = -xf[13] * blocksPerHavok;
                const float dx = mx - static_cast<float>(player.x), dz = mz - static_cast<float>(player.z);
                auto it = bodies_.find(body);
                const bool nearby = dx * dx + dz * dz < kExtractRadius * kExtractRadius;
                if (it == bodies_.end()) {
                    if (!nearby) return;
                    it = bodies_.emplace(body, Body{}).first;
                }
                Body& entry = it->second;
                entry.seenFrame = frame_;
                const void* shape = havok::bodyShape(body);
                bool moved = entry.valid && entry.shape != shape;
                for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) moved |= entry.valid && std::fabs(entry.xf[i] - xf[i]) > 1e-4f;
                for (int i : {12, 13, 14}) moved |= entry.valid && std::fabs(entry.xf[i] - xf[i]) > 1e-3f;
                if (entry.valid && !moved) return;
                if (spent > kExtractBudgetMs) { if (nearby) ++pendingNear; return; }
                if (moved) markRegions(entry.geometry.lo, entry.geometry.hi);
                entry.shape = shape;
                std::memcpy(entry.xf, xf, sizeof(entry.xf));
                entry.flags = 0;
                entry.layer = havok::bodyLayer(body);
                const double before = nowMs();
                extract(body, entry);
                spent += nowMs() - before;
                ++stats_.extracted;
                stats_.extractMs += nowMs() - before;
                if (moved) ++stats_.moved;
                if (moved || settled_) markRegions(entry.geometry.lo, entry.geometry.hi);
            });
        });
        if (!ok) log::once("collision-walk", "collision: faulted walking the Havok world");
        for (auto it = bodies_.begin(); it != bodies_.end();) {
            if (it->second.seenFrame != frame_) {
                markRegions(it->second.geometry.lo, it->second.geometry.hi);
                it = bodies_.erase(it);
            } else ++it;
        }
}
    const auto statsTick = GetTickCount64();
    if (statsTick - stats_.since >= 5000) {
        std::size_t valid = 0, tris = 0;
        for (const auto& [b, e] : bodies_) { valid += e.valid; tris += e.geometry.tris.size(); }
        std::size_t queued;
        { std::scoped_lock lock(mutex_); queued = queue_.size(); }
        log::line("collision: %u bodies (%u extracted, %u tris), %d pending near, extracted %u (%u moved) in %.1f ms, %u regions sent, %u queued, faults=%d",
            static_cast<unsigned>(bodies_.size()), static_cast<unsigned>(valid), static_cast<unsigned>(tris), pendingNear, stats_.extracted, stats_.moved,
            stats_.extractMs, static_cast<unsigned>(harvested_.size()), static_cast<unsigned>(queued), ok ? 0 : 1);
        stats_ = {};
        stats_.since = statsTick;
    }
    nearKnown_ = pendingNear == 0;
    if (pendingNear > 0) return;  // the world around the player isn't fully known yet

    const int prx = static_cast<int>(std::floor(player.x / kRegionSize));
    const int pry = static_cast<int>(std::floor(player.y / kRegionSize));
    const int prz = static_cast<int>(std::floor(player.z / kRegionSize));
    const auto tick = GetTickCount64();
    int done = 0;
    while (!urgent_.empty() && done < kMaxHarvestsPerFrame * 2) {
        auto r = urgent_.back();
        urgent_.pop_back();
        if (std::abs(r[0] - prx) > kRadius + 1 || std::abs(r[2] - prz) > kRadius + 1 || r[1] - pry < -kBelow - 1 || r[1] - pry > kAbove + 1) continue;
        harvest(r[0], r[1], r[2], true);
        harvested_[regionKey(r[0], r[1], r[2])] = tick;
        ++done;
    }
    bool allSent = true;
    for (const auto& o : offsets_) {
        const int rx = prx + o[0], ry = pry + o[1], rz = prz + o[2];
        const auto key = regionKey(rx, ry, rz);
        auto it = harvested_.find(key);
        const bool isNear = std::abs(o[0]) <= 1 && std::abs(o[2]) <= 1 && o[1] >= -1 && o[1] <= 0;
        if (it != harvested_.end() && !(isNear && tick - it->second > kRefreshNearMs)) continue;
        if (it == harvested_.end()) allSent = false;
        if (done >= kMaxHarvestsPerFrame) { allSent = false; break; }
        harvest(rx, ry, rz);
        harvested_[key] = tick;
        ++done;
    }
    if (allSent && !settled_) {
        settled_ = true;
        log::line("collision: %u bodies cached; regions around the player sent", static_cast<unsigned>(bodies_.size()));
    }
    if (harvested_.size() > offsets_.size() * 4) harvested_.clear();
}
void Collision::harvest(int rx, int ry, int rz, bool priority) {
    Job job;
    job.rx = rx; job.ry = ry; job.rz = rz;
    job.epoch = epoch_.load();
    const float margin = 0.75f;
    const float lo[3] = {float(rx * kRegionSize) - margin, float(ry * kRegionSize) - margin, float(rz * kRegionSize) - margin};
    const float hi[3] = {float((rx + 1) * kRegionSize) + margin, float((ry + 1) * kRegionSize) + margin, float((rz + 1) * kRegionSize) + margin};
    auto& g = job.geometry;
    for (const auto& [body, entry] : bodies_) {
        const auto& src = entry.geometry;
        if (!entry.valid || src.empty() || !overlaps(src.lo, src.hi, lo, hi)) continue;
        for (const auto& t : src.tris) {
            float tlo[3], thi[3];
            for (int k = 0; k < 3; ++k) {
                tlo[k] = std::min({t.v[k], t.v[3 + k], t.v[6 + k]});
                thi[k] = std::max({t.v[k], t.v[3 + k], t.v[6 + k]});
            }
            if (overlaps(tlo, thi, lo, hi)) g.tris.push_back(t);
        }
        for (const auto& b : src.boxes) g.boxes.push_back(b);
        for (const auto& c : src.capsules) g.capsules.push_back(c);
        for (const auto& c : src.convexes) if (overlaps(c.lo, c.hi, lo, hi)) g.convexes.push_back(c);
    }
    // Take one coherent snapshot on the game thread. A worker must never combine collision
    // triangles from one dig generation with voxels from another.
    for (const auto& t : g.tris) {
        if (!(t.flags & proto::kTriDiggable)) continue;
        float tlo[3], thi[3];
        for (int k = 0; k < 3; ++k) {
            tlo[k] = std::min({t.v[k], t.v[3 + k], t.v[6 + k]});
            thi[k] = std::max({t.v[k], t.v[3 + k], t.v[6 + k]});
        }
        dig::collect(tlo, thi, job.dug);
    }
    dig::collect(lo, hi, job.dug);
    std::sort(job.dug.begin(), job.dug.end());
    job.dug.erase(std::unique(job.dug.begin(), job.dug.end()), job.dug.end());
    if (!job.dug.empty()) {
        const auto boxes = clip::Merge(job.dug);
        std::vector<Tri> cut;
        for (const auto& t : g.tris) {
            if (!(t.flags & proto::kTriDiggable)) { cut.push_back(t); continue; }
            std::vector<clip::Poly> pieces;
            if (!clip::Subtract(clip::FromTriangle(t.v, t.v + 3, t.v + 6), boxes, pieces)) {
                cut.push_back(t);
                continue;
            }
            auto ghost = t;
            ghost.flags |= proto::kTriGhost;
            cut.push_back(ghost);
            for (const auto& p : pieces) for (std::size_t i = 1; i + 1 < p.size(); ++i) {
                Tri fragment{};
                fragment.flags = t.flags;
                std::memcpy(fragment.v, p[0].p, 12);
                std::memcpy(fragment.v + 3, p[i].p, 12);
                std::memcpy(fragment.v + 6, p[i + 1].p, 12);
                cut.push_back(fragment);
            }
        }
        g.tris.swap(cut);
    }
    std::scoped_lock lock(mutex_);
    // Obsolete snapshots must not restore ground after a freshly dug update. Mine edits
    // go ahead of the background region scan, while epoch clears keep their first place.
    std::erase_if(queue_, [&](const Job& queued) { return !queued.clear && queued.rx == rx && queued.ry == ry && queued.rz == rz; });
    if (priority) {
        auto at = queue_.begin();
        if (at != queue_.end() && at->clear) ++at;
        queue_.insert(at, std::move(job));
    } else queue_.push_back(std::move(job));
    cv_.notify_one();
}

// ---- the ground under Minecraft chunks ----------------------------------------------------------
namespace {
constexpr int kLandChunkRadius = 6;          // chunks around the player's (all within kExtractRadius)
constexpr std::uint64_t kLandRetryMs = 3000;  // a chunk whose land wasn't all loaded is tried again
constexpr std::uint32_t kLayerInvisibleWall = 27;
constexpr int kLandColumnsPerFrame = 32;      // a chunk takes 8 frames
constexpr double kLandWaterRadius = 40.0;     // blocks around the player where water is asked
std::uint64_t chunkKey(int cx, int cz) { return (std::uint64_t(std::uint32_t(cx)) << 32) | std::uint32_t(cz); }
}
void Collision::updateLand(const Position& player, std::uint32_t worldId, void* cell) {
    if (!running_ || !settled_ || !nearKnown_) return;
    const int pcx = floorDiv(static_cast<int>(std::floor(player.x)), 16), pcz = floorDiv(static_cast<int>(std::floor(player.z)), 16);
    const auto tick = GetTickCount64();
    auto& work = landWork_;
    if (work.active && (work.land.epoch != epoch_.load() || std::abs(work.cx - pcx) > kLandChunkRadius + 1 || std::abs(work.cz - pcz) > kLandChunkRadius + 1)) work.active = false;
    if (!work.active) {
        // The nearest chunk not sent yet (or incomplete and due for another try).
        int best = INT32_MAX, bx = 0, bz = 0;
        for (int dz = -kLandChunkRadius; dz <= kLandChunkRadius; ++dz)
            for (int dx = -kLandChunkRadius; dx <= kLandChunkRadius; ++dx) {
                const int d = dx * dx + dz * dz;
                if (d > kLandChunkRadius * kLandChunkRadius || d >= best) continue;
                const auto it = landSent_.find(chunkKey(pcx + dx, pcz + dz));
                if (it != landSent_.end() && (it->second == 0 || tick - it->second < kLandRetryMs)) continue;
                best = d; bx = pcx + dx; bz = pcz + dz;
            }
        if (best == INT32_MAX) return;
        if (landSent_.size() > 4096) landSent_.clear();
        work = {};
        work.active = true;
        work.cx = bx; work.cz = bz;
        work.land.epoch = epoch_.load();
        work.land.worldId = worldId;
        work.land.chunkX = bx; work.land.chunkZ = bz;
    }
    auto& land = work.land;
    const int x0 = work.cx * 16, z0 = work.cz * 16;
    // A few columns a frame. Water only near the player: TES::GetWaterHeight is given the
    // player's cell, which says nothing about cells farther out.
    for (int n = 0; n < kLandColumnsPerFrame && work.next < 256; ++n, ++work.next) {
        const int i = work.next, x = i & 15, z = i >> 4;
        const float px = static_cast<float>((x0 + x + 0.5) * proto::kUnitsPerBlock), py = static_cast<float>(-(z0 + z + 0.5) * proto::kUnitsPerBlock);
        float h = 0;
        land.objectTop[i] = proto::kLandUnknown;
        if (!game::landHeight({px, py, 0}, h) || !std::isfinite(h)) {
            land.height[i] = proto::kLandUnknown;
            work.complete = false;
            continue;
        }
        land.height[i] = static_cast<float>(h / proto::kUnitsPerBlock);
        land.material[i] = dig::landMaterial(px, py);
        if (dig::landHavokMaterial(px, py) == 19) land.flags[i] |= proto::kLandRoad;  // broken concrete: roads
        const double ddx = x0 + x + 0.5 - player.x, ddz = z0 + z + 0.5 - player.z;
        if (ddx * ddx + ddz * ddz < kLandWaterRadius * kLandWaterRadius) {
            const float w = game::waterHeight({px, py, h}, cell);
            if (std::isfinite(w) && w > -1.0e6f && w < 1.0e6f && w != 0.0f && w > h + 5.0f) land.flags[i] |= proto::kLandWater;
        }
    }
    if (work.next < 256) return;
    work.active = false;
    const int bx = work.cx, bz = work.cz;
    const bool complete = work.complete;
    // New Vegas objects standing on these columns: anything of theirs reaching from below the
    // surface to a few blocks above it (a bridge or a sign high overhead doesn't count).
    auto mark = [&](const float* lo, const float* hi) {
        if (hi[0] < x0 || lo[0] > x0 + 16 || hi[2] < z0 || lo[2] > z0 + 16) return;
        const int ix0 = std::clamp(static_cast<int>(std::floor(lo[0])) - x0, 0, 15), ix1 = std::clamp(static_cast<int>(std::floor(hi[0])) - x0, 0, 15);
        const int iz0 = std::clamp(static_cast<int>(std::floor(lo[2])) - z0, 0, 15), iz1 = std::clamp(static_cast<int>(std::floor(hi[2])) - z0, 0, 15);
        for (int z = iz0; z <= iz1; ++z)
            for (int x = ix0; x <= ix1; ++x) {
                const int i = z * 16 + x;
                const float ground = land.height[i];
                if (ground == proto::kLandUnknown || hi[1] < ground - 0.5f || lo[1] > ground + 4.0f) continue;
                if (land.objectTop[i] == proto::kLandUnknown || hi[1] > land.objectTop[i]) land.objectTop[i] = hi[1];
            }
    };
    const float clo[3] = {float(x0), -FLT_MAX, float(z0)}, chi[3] = {float(x0 + 16), FLT_MAX, float(z0 + 16)};
    for (const auto& [body, entry] : bodies_) {
        const auto& g = entry.geometry;
        if (!entry.valid || g.empty() || entry.layer == havok::kLayerGround || entry.layer == kLayerInvisibleWall || !overlaps(g.lo, g.hi, clo, chi)) continue;
        for (const auto& t : g.tris) {
            float lo[3], hi[3];
            for (int k = 0; k < 3; ++k) { lo[k] = std::min({t.v[k], t.v[3 + k], t.v[6 + k]}); hi[k] = std::max({t.v[k], t.v[3 + k], t.v[6 + k]}); }
            mark(lo, hi);
        }
        for (const auto& b : g.boxes) {
            float lo[3], hi[3];
            for (int i = 0; i < 3; ++i) {
                const float ext = std::fabs(b.axis[0][i]) * b.half[0] + std::fabs(b.axis[1][i]) * b.half[1] + std::fabs(b.axis[2][i]) * b.half[2];
                lo[i] = b.c[i] - ext; hi[i] = b.c[i] + ext;
            }
            mark(lo, hi);
        }
        for (const auto& c : g.capsules) {
            float lo[3], hi[3];
            for (int i = 0; i < 3; ++i) { lo[i] = std::min(c.a[i], c.b[i]) - c.r; hi[i] = std::max(c.a[i], c.b[i]) + c.r; }
            mark(lo, hi);
        }
        for (const auto& c : g.convexes) mark(c.lo, c.hi);
    }
    unsigned blocked = 0;
    for (int i = 0; i < 256; ++i)
        if (land.objectTop[i] != proto::kLandUnknown && land.objectTop[i] > land.height[i] + 0.2f) { land.flags[i] |= proto::kLandBlocked; ++blocked; }
    landSent_[chunkKey(bx, bz)] = complete ? 0 : tick;
    log::once("land-first", "land: first chunk %d %d sent (%s, %u blocked columns)", bx, bz, complete ? "complete" : "partly loaded", blocked);
    Job job;
    job.epoch = land.epoch;
    job.land.resize(sizeof(land));
    std::memcpy(job.land.data(), &land, sizeof(land));
    std::scoped_lock lock(mutex_);
    queue_.push_back(std::move(job));
    cv_.notify_one();
}

void Collision::post(proto::ColType type, std::vector<std::uint8_t> payload) {
    if (!running_) return;
    Job job;
    job.type = type;
    job.land = std::move(payload);
    std::scoped_lock lock(mutex_);
    queue_.push_back(std::move(job));
    cv_.notify_one();
}

// ---- worker -----------------------------------------------------------------------------------
void Collision::workerLoop() {
    while (running_) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
            if (!running_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (job.clear) {
            std::vector<std::uint8_t> payload(4);
            std::memcpy(payload.data(), &job.epoch, 4);
            send(payload, proto::kColClear);
        } else if (!job.land.empty()) {
            if (job.type != proto::kColLand || job.epoch == epoch_.load()) send(job.land, job.type);
        } else if (job.epoch == epoch_.load()) {
            sendTriangles(job);
            voxelize(job);
        }
    }
}
void Collision::send(const std::vector<std::uint8_t>& payload, proto::ColType type) {
    for (int attempt = 0; attempt < 2000 && running_; ++attempt) {
        if (bridge_->writeCollision(type, payload.data(), static_cast<std::uint32_t>(payload.size()))) return;
        Sleep(1);  // ring full: Minecraft is behind (or not running)
    }
    log::once("collision-full", "collision ring stayed full; dropped a message");
}
void Collision::triangulate(const Geometry& src, std::vector<Tri>& out) {
    out.insert(out.end(), src.tris.begin(), src.tris.end());
    std::uint32_t flags = 0;
    const float* centre = nullptr;
    const float* outward = nullptr;
    auto emit = [&](const float* a, const float* b, const float* c) {
        Tri t{{a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]}, flags};
        float e1[3], e2[3], n[3];
        sub(b, a, e1); sub(c, a, e2); cross(e1, e2, n);
        float dir[3] = {0, 0, 0};
        if (outward) std::memcpy(dir, outward, sizeof(dir));
        else if (centre) for (int i = 0; i < 3; ++i) dir[i] = (a[i] + b[i] + c[i]) / 3.0f - centre[i];
        if (dot(n, dir) < 0.0f) std::swap_ranges(t.v + 3, t.v + 6, t.v + 6);
        out.push_back(t);
    };
    auto quad = [&](const float* a, const float* b, const float* c, const float* d) { emit(a, b, c); emit(a, c, d); };
    auto box = [&](const float* c, const float (*axis)[3], const float* half) {
        float corner[8][3];
        for (int i = 0; i < 8; ++i) {
            const float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f, sz = (i & 4) ? 1.0f : -1.0f;
            for (int k = 0; k < 3; ++k) corner[i][k] = c[k] + axis[0][k] * half[0] * sx + axis[1][k] * half[1] * sy + axis[2][k] * half[2] * sz;
        }
        quad(corner[0], corner[1], corner[3], corner[2]);
        quad(corner[4], corner[5], corner[7], corner[6]);
        quad(corner[0], corner[1], corner[5], corner[4]);
        quad(corner[2], corner[3], corner[7], corner[6]);
        quad(corner[0], corner[2], corner[6], corner[4]);
        quad(corner[1], corner[3], corner[7], corner[5]);
    };
    for (const auto& b : src.boxes) { flags = b.flags; centre = b.c; box(b.c, b.axis, b.half); }
    for (const auto& cap : src.capsules) {
        float ab[3];
        sub(cap.b, cap.a, ab);
        const float len = std::sqrt(dot(ab, ab));
        float axis[3][3]{};
        if (len > 1e-4f) for (int k = 0; k < 3; ++k) axis[2][k] = ab[k] / len;
        else axis[2][1] = 1.0f;
        const float ref[3] = {std::fabs(axis[2][1]) < 0.9f ? 0.0f : 1.0f, std::fabs(axis[2][1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
        cross(ref, axis[2], axis[0]);
        const float l0 = std::sqrt(dot(axis[0], axis[0]));
        for (int k = 0; k < 3; ++k) axis[0][k] /= l0;
        cross(axis[2], axis[0], axis[1]);
        const float c[3] = {(cap.a[0] + cap.b[0]) * 0.5f, (cap.a[1] + cap.b[1]) * 0.5f, (cap.a[2] + cap.b[2]) * 0.5f};
        const float half[3] = {cap.r, cap.r, len * 0.5f + cap.r};
        flags = cap.flags; centre = c;
        box(c, axis, half);
    }
    centre = nullptr;
    for (const auto& cvx : src.convexes) {
        flags = cvx.flags;
        const float ex = cvx.hi[0] - cvx.lo[0], ey = cvx.hi[1] - cvx.lo[1], ez = cvx.hi[2] - cvx.lo[2];
        const float diag = std::sqrt(ex * ex + ey * ey + ez * ez) + 1.0f;
        const float mid[3] = {(cvx.lo[0] + cvx.hi[0]) * 0.5f, (cvx.lo[1] + cvx.hi[1]) * 0.5f, (cvx.lo[2] + cvx.hi[2]) * 0.5f};
        for (std::size_t i = 0; i < cvx.planes.size(); ++i) {
            const auto& pl = cvx.planes[i];
            const float n[3] = {pl[0], pl[1], pl[2]};
            const float nl = std::sqrt(dot(n, n));
            if (nl < 1e-6f) continue;
            const float dist = (dot(n, mid) + pl[3]) / (nl * nl);
            const float o[3] = {mid[0] - n[0] * dist, mid[1] - n[1] * dist, mid[2] - n[2] * dist};
            const float ref[3] = {std::fabs(n[1]) < 0.9f * nl ? 0.0f : 1.0f, std::fabs(n[1]) < 0.9f * nl ? 1.0f : 0.0f, 0.0f};
            float t1[3], t2[3];
            cross(ref, n, t1);
            const float lt = std::sqrt(dot(t1, t1));
            for (int k = 0; k < 3; ++k) t1[k] /= lt;
            cross(n, t1, t2);
            const float l2 = std::sqrt(dot(t2, t2));
            for (int k = 0; k < 3; ++k) t2[k] /= l2;
            std::vector<std::array<float, 3>> poly;
            const float sgn1[4] = {-1, 1, 1, -1}, sgn2[4] = {-1, -1, 1, 1};
            for (int q = 0; q < 4; ++q) {
                const float s1 = sgn1[q] * diag, s2 = sgn2[q] * diag;
                poly.push_back({o[0] + t1[0] * s1 + t2[0] * s2, o[1] + t1[1] * s1 + t2[1] * s2, o[2] + t1[2] * s1 + t2[2] * s2});
            }
            for (std::size_t j = 0; j < cvx.planes.size() && poly.size() >= 3; ++j) {
                if (j == i) continue;
                const auto& cp = cvx.planes[j];
                std::vector<std::array<float, 3>> clipped;
                for (std::size_t v = 0; v < poly.size(); ++v) {
                    const auto& A = poly[v];
                    const auto& B = poly[(v + 1) % poly.size()];
                    const float da = cp[0] * A[0] + cp[1] * A[1] + cp[2] * A[2] + cp[3];
                    const float db = cp[0] * B[0] + cp[1] * B[1] + cp[2] * B[2] + cp[3];
                    if (da <= 0.0f) clipped.push_back(A);
                    if ((da <= 0.0f) != (db <= 0.0f)) {
                        const float t = da / (da - db);
                        clipped.push_back({A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, A[2] + (B[2] - A[2]) * t});
                    }
                }
                poly.swap(clipped);
            }
            outward = n;
            for (std::size_t v = 1; v + 1 < poly.size(); ++v) emit(poly[0].data(), poly[v].data(), poly[v + 1].data());
            outward = nullptr;
        }
    }
}
void Collision::sendTriangles(const Job& job) {
    std::vector<Tri> solid;
    triangulate(job.geometry, solid);
    const float lo[3] = {float(job.rx * kRegionSize) - 0.5f, float(job.ry * kRegionSize) - 0.5f, float(job.rz * kRegionSize) - 0.5f};
    const float hi[3] = {lo[0] + kRegionSize + 1.0f, lo[1] + kRegionSize + 1.0f, lo[2] + kRegionSize + 1.0f};
    std::vector<proto::ColTri> out;
    out.reserve(solid.size());
    for (const auto& t : solid) {
        float tlo[3], thi[3];
        for (int k = 0; k < 3; ++k) {
            tlo[k] = std::min({t.v[k], t.v[3 + k], t.v[6 + k]});
            thi[k] = std::max({t.v[k], t.v[3 + k], t.v[6 + k]});
        }
        if (overlaps(tlo, thi, lo, hi) && finite(t.v, 9)) {
            proto::ColTri c{};
            std::memcpy(c.v, t.v, sizeof(c.v));
            c.flags = t.flags;
            out.push_back(c);
        }
    }
    proto::ColRegion header{};
    header.minX = job.rx * kRegionSize; header.minY = job.ry * kRegionSize; header.minZ = job.rz * kRegionSize;
    header.maxX = header.minX + kRegionSize - 1; header.maxY = header.minY + kRegionSize - 1; header.maxZ = header.minZ + kRegionSize - 1;
    header.epoch = job.epoch;
    header.count = static_cast<std::uint32_t>(out.size());
    std::vector<std::uint8_t> payload(sizeof(header) + out.size() * sizeof(proto::ColTri));
    std::memcpy(payload.data(), &header, sizeof(header));
    if (!out.empty()) std::memcpy(payload.data() + sizeof(header), out.data(), out.size() * sizeof(proto::ColTri));
    send(payload, proto::kColTris);
}
void Collision::voxelize(const Job& job) {
    constexpr int G = kGrid;
    std::vector<std::uint64_t> solid(G * G, 0), steep(G * G, 0), land(G * G, 0), landSteep(G * G, 0);
    auto set = [&](std::vector<std::uint64_t>& grid, int x, int y, int z) { grid[y * G + z] |= 1ull << x; };
    const float ox = float(job.rx * kRegionSize), oy = float(job.ry * kRegionSize), oz = float(job.rz * kRegionSize);
    auto toVoxel = [&](const float* mc, float* out) { out[0] = (mc[0] - ox) * 8.0f; out[1] = (mc[1] - oy) * 8.0f; out[2] = (mc[2] - oz) * 8.0f; };
    auto clampLo = [](float v) { return std::clamp(static_cast<int>(std::floor(v)), 0, G - 1); };
    auto clampHi = [](float v) { return std::clamp(static_cast<int>(std::ceil(v)) - 1, 0, G - 1); };
    for (const auto& tri : job.geometry.tris) {
        if (tri.flags & proto::kTriGhost) continue;
        float a[3], b[3], c[3];
        toVoxel(tri.v, a); toVoxel(tri.v + 3, b); toVoxel(tri.v + 6, c);
        float lo[3], hi[3];
        for (int i = 0; i < 3; ++i) { lo[i] = std::min({a[i], b[i], c[i]}); hi[i] = std::max({a[i], b[i], c[i]}); }
        if (hi[0] < 0 || hi[1] < 0 || hi[2] < 0 || lo[0] > G || lo[1] > G || lo[2] > G) continue;
        float e1[3], e2[3], n[3];
        sub(b, a, e1); sub(c, a, e2); cross(e1, e2, n);
        const float len = std::sqrt(dot(n, n));
        if (len < 1e-9f) continue;
        n[0] /= len, n[1] /= len, n[2] /= len;
        const float ny = std::fabs(n[1]);
        const bool terrain = tri.flags & proto::kTriTerrain;
        auto& grid = (ny >= kSteepMax || ny < kSteepMin) ? (terrain ? land : solid) : (terrain ? landSteep : steep);
        int dom = 0;
        if (std::fabs(n[1]) > std::fabs(n[dom])) dom = 1;
        if (std::fabs(n[2]) > std::fabs(n[dom])) dom = 2;
        const int u = (dom + 1) % 3, v = (dom + 2) % 3;
        const float d = dot(n, a);
        const float r = 0.5f * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
        // A surface has zero thickness. Include the voxels touching either side of a grid
        // plane; floor(min)..ceil(max)-1 otherwise drops perfectly flat ground entirely.
        const int iu0 = clampLo(lo[u] - 0.5f), iu1 = clampHi(hi[u] + 0.5f), iv0 = clampLo(lo[v] - 0.5f), iv1 = clampHi(hi[v] + 0.5f);
        const int id0 = clampLo(lo[dom] - 0.5f), id1 = clampHi(hi[dom] + 0.5f);
        for (int iu = iu0; iu <= iu1; ++iu) for (int iv = iv0; iv <= iv1; ++iv) {
            const float cu = iu + 0.5f, cv = iv + 0.5f;
            const float s0 = (d - r - n[u] * cu - n[v] * cv) / n[dom];
            const float s1 = (d + r - n[u] * cu - n[v] * cv) / n[dom];
            const int a0 = std::max(id0, static_cast<int>(std::floor(std::min(s0, s1) - 0.5f)));
            const int a1 = std::min(id1, static_cast<int>(std::ceil(std::max(s0, s1) - 0.5f)));
            for (int id = a0; id <= a1; ++id) {
                float cen[3];
                cen[dom] = id + 0.5f; cen[u] = cu; cen[v] = cv;
                if (triBoxOverlap(cen, 0.5f, a, b, c, n)) {
                    int p[3];
                    p[dom] = id, p[u] = iu, p[v] = iv;
                    set(grid, p[0], p[1], p[2]);
                }
            }
        }
    }
    auto fillPrimitive = [&](const float* plo, const float* phi, auto&& inside) {
        float lo[3], hi[3];
        toVoxel(plo, lo); toVoxel(phi, hi);
        if (hi[0] < 0 || hi[1] < 0 || hi[2] < 0 || lo[0] > G || lo[1] > G || lo[2] > G) return;
        for (int y = clampLo(lo[1] - 1); y <= clampHi(hi[1] + 1); ++y)
            for (int z = clampLo(lo[2] - 1); z <= clampHi(hi[2] + 1); ++z)
                for (int x = clampLo(lo[0] - 1); x <= clampHi(hi[0] + 1); ++x) {
                    const float p[3] = {ox + (x + 0.5f) / 8.0f, oy + (y + 0.5f) / 8.0f, oz + (z + 0.5f) / 8.0f};
                    if (inside(p)) set(solid, x, y, z);
                }
    };
    constexpr float m = kPrimMargin / 8.0f;
    for (const auto& box : job.geometry.boxes) {
        float lo[3], hi[3];
        for (int i = 0; i < 3; ++i) {
            const float ext = std::fabs(box.axis[0][i]) * box.half[0] + std::fabs(box.axis[1][i]) * box.half[1] + std::fabs(box.axis[2][i]) * box.half[2];
            lo[i] = box.c[i] - ext; hi[i] = box.c[i] + ext;
        }
        fillPrimitive(lo, hi, [&](const float* p) {
            const float d[3] = {p[0] - box.c[0], p[1] - box.c[1], p[2] - box.c[2]};
            for (int i = 0; i < 3; ++i) if (std::fabs(dot(d, box.axis[i])) > box.half[i] + m) return false;
            return true;
        });
    }
    for (const auto& cap : job.geometry.capsules) {
        float lo[3], hi[3];
        for (int i = 0; i < 3; ++i) { lo[i] = std::min(cap.a[i], cap.b[i]) - cap.r; hi[i] = std::max(cap.a[i], cap.b[i]) + cap.r; }
        fillPrimitive(lo, hi, [&](const float* p) {
            float ab[3], ap[3];
            sub(cap.b, cap.a, ab); sub(p, cap.a, ap);
            const float len2 = dot(ab, ab);
            const float t = len2 > 0 ? std::clamp(dot(ap, ab) / len2, 0.0f, 1.0f) : 0.0f;
            const float q[3] = {cap.a[0] + ab[0] * t - p[0], cap.a[1] + ab[1] * t - p[1], cap.a[2] + ab[2] * t - p[2]};
            return dot(q, q) <= (cap.r + m) * (cap.r + m);
        });
    }
    for (const auto& cvx : job.geometry.convexes) {
        fillPrimitive(cvx.lo, cvx.hi, [&](const float* p) {
            for (const auto& pl : cvx.planes) if (pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] + pl[3] > m) return false;
            return true;
        });
    }
    // Steep (50-84 degree) surfaces snap to whole-block footprints, so Minecraft's step rules
    // treat cliffs like stacked blocks (SkyCraft's rule).
    for (int pass = 0; pass < 2; ++pass) {
    auto& slope = pass ? landSteep : steep;
    auto& target = pass ? land : solid;
    for (int by = 0; by < kRegionSize; ++by) for (int bz = 0; bz < kRegionSize; ++bz) for (int bx = 0; bx < kRegionSize; ++bx) {
        const std::uint64_t xmask = 0xFFull << (bx * 8);
        int minY = 99, maxY = -1;
        for (int y = by * 8; y < by * 8 + 8; ++y) for (int z = bz * 8; z < bz * 8 + 8; ++z)
            if (slope[y * G + z] & xmask) { minY = std::min(minY, y); maxY = std::max(maxY, y); }
        if (maxY < 0) continue;
        for (int y = minY; y <= maxY; ++y) for (int z = bz * 8; z < bz * 8 + 8; ++z) target[y * G + z] |= xmask;
    }
    }
    for (const auto& c : job.dug) {
        const int bx = c[0] - job.rx * kRegionSize, by = c[1] - job.ry * kRegionSize, bz = c[2] - job.rz * kRegionSize;
        if (bx < 0 || bx >= kRegionSize || by < 0 || by >= kRegionSize || bz < 0 || bz >= kRegionSize) continue;
        const auto mask = ~(0xFFull << (bx * 8));
        for (int y = by * 8; y < by * 8 + 8; ++y) for (int z = bz * 8; z < bz * 8 + 8; ++z) land[y * G + z] &= mask;
    }
    for (std::size_t i = 0; i < solid.size(); ++i) solid[i] |= land[i];
    std::vector<proto::ColBlock> blocks;
    for (int by = 0; by < kRegionSize; ++by) for (int bz = 0; bz < kRegionSize; ++bz) for (int bx = 0; bx < kRegionSize; ++bx) {
        proto::ColBlock blk{};
        bool any = false;
        for (int sy = 0; sy < 8; ++sy) {
            std::uint64_t layer = 0;
            for (int sz = 0; sz < 8; ++sz) layer |= ((solid[(by * 8 + sy) * G + (bz * 8 + sz)] >> (bx * 8)) & 0xFF) << (sz * 8);
            blk.bits[sy] = layer;
            any |= layer != 0;
        }
        if (!any) continue;
        blk.x = job.rx * kRegionSize + bx; blk.y = job.ry * kRegionSize + by; blk.z = job.rz * kRegionSize + bz;
        blocks.push_back(blk);
    }
    proto::ColRegion header{};
    header.minX = job.rx * kRegionSize; header.minY = job.ry * kRegionSize; header.minZ = job.rz * kRegionSize;
    header.maxX = header.minX + kRegionSize - 1; header.maxY = header.minY + kRegionSize - 1; header.maxZ = header.minZ + kRegionSize - 1;
    header.epoch = job.epoch;
    header.count = static_cast<std::uint32_t>(blocks.size());
    std::vector<std::uint8_t> payload(sizeof(header) + blocks.size() * sizeof(proto::ColBlock));
    std::memcpy(payload.data(), &header, sizeof(header));
    if (!blocks.empty()) std::memcpy(payload.data() + sizeof(header), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
    send(payload, proto::kColRegion);
}
} // namespace vegas
