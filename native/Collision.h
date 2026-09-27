#pragma once
#include "Bridge.h"
#include "Clip.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vegas {
// Streams New Vegas' Havok collision around the Minecraft player as SkyCraft's ColTris and
// 1/8-block ColRegion voxels.
//
// Main thread: keeps a cache of each nearby static/keyframed body's geometry (triangles and
// convex primitives, in Minecraft coordinates), extracted under a per-frame time budget, and
// picks 8x8x8-block regions to (re)send. Worker thread: voxelizes and writes the ring.
class Collision {
public:
    static Collision& get();
    void start(Bridge* bridge);
    void stop();
    // Drops everything; Minecraft clears its store on the new epoch.
    void reset(std::uint32_t epoch);
    // Once per frame with the Minecraft player's feet (MC coordinates) and the Havok world.
    void update(const Position& player, void* hkpWorld);
    // True once the geometry around the player has been sent at least once since the reset.
    bool settled() const { return settled_; }
    // Once per frame after update, inside the game: the ground under the next Minecraft chunk
    // around the player (proto::LandChunk). Uses the land, its textures and water in `cell`'s
    // world and the cached bodies standing on it.
    void updateLand(const Position& player, std::uint32_t worldId, void* cell);
    // Any thread: a message for the same ring (the worker is its only writer), outside the epochs.
    void post(proto::ColType type, std::vector<std::uint8_t> payload);

    static constexpr int kRegionSize = 8;

    struct Tri { float v[9]; std::uint32_t flags = 0; };
    struct Obb { float c[3]; float axis[3][3]; float half[3]; std::uint32_t flags = 0; };
    struct Capsule { float a[3], b[3]; float r; std::uint32_t flags = 0; };
    struct Convex { std::vector<std::array<float, 4>> planes; float lo[3], hi[3]; std::uint32_t flags = 0; };
    struct Geometry {
        std::vector<Tri> tris;
        std::vector<Obb> boxes;
        std::vector<Capsule> capsules;
        std::vector<Convex> convexes;
        float lo[3], hi[3];
        bool empty() const { return tris.empty() && boxes.empty() && capsules.empty() && convexes.empty(); }
    };

private:
    struct Body {
        const void* shape = nullptr;
        float xf[16]{};
        std::uint32_t flags = 0;
        Geometry geometry;
        std::uint32_t seenFrame = 0;
        std::uint32_t layer = 0;
        bool valid = false;
    };
    struct Job {
        int rx = 0, ry = 0, rz = 0;
        std::uint32_t epoch = 0;
        bool clear = false;
        Geometry geometry;
        std::vector<clip::Cube> dug;
        std::vector<std::uint8_t> land;  // a proto::LandChunk to send as is (or a posted message)
        proto::ColType type = proto::kColLand;
    };

    void extract(const void* body, Body& entry);
    void harvest(int rx, int ry, int rz, bool priority = false);
    void markRegions(const float lo[3], const float hi[3]);
    void workerLoop();
    void send(const std::vector<std::uint8_t>& payload, proto::ColType type);
    void sendTriangles(const Job& job);
    void voxelize(const Job& job);
    friend struct CollisionTestAccess;
    static void triangulate(const Geometry& src, std::vector<Tri>& out);

    Bridge* bridge_ = nullptr;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> epoch_{0};
    std::unordered_map<const void*, Body> bodies_;
    std::unordered_map<std::uint64_t, std::uint64_t> harvested_;  // region -> tick sent
    std::vector<std::array<int, 3>> urgent_;
    std::vector<std::array<int, 3>> offsets_;
    std::uint32_t frame_ = 0;
    bool settled_ = false;
    bool nearKnown_ = false;  // no body near the player waits to be extracted
    std::unordered_map<std::uint64_t, std::uint64_t> landSent_;  // chunk -> tick (0: complete)
    // The chunk being read, a few columns a frame (engine land queries are not free).
    struct LandWork { bool active = false; int cx = 0, cz = 0, next = 0; bool complete = true; proto::LandChunk land{}; } landWork_;
    struct Stats { std::uint64_t since = 0; unsigned extracted = 0, moved = 0; double extractMs = 0; } stats_;
};
} // namespace vegas
