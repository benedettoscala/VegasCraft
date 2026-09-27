#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include "skycraft_protocol.h"

namespace vegas {
namespace proto = skycraft::proto;
inline constexpr wchar_t kMappingName[] = L"Local\\VegasCraft_v1";

// New Vegas end of the SkyCraft v11 shared memory. FalloutNV is a 32-bit process without the
// large-address flag, so the 191 MiB object is never mapped whole: the state, rings and tables
// (first 32 MiB) are one view, and the overlay slots and the render ring are mapped on demand.
class Bridge {
public:
    Bridge() = default;
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    bool open(const std::wstring& name = kMappingName);
    void close();
    void heartbeat();
    void publish(const proto::SkyState& state);
    void publishArms(const proto::ArmPose& arms);
    void writeWaterGrid(const proto::WaterGrid& grid);
    bool readMinecraft(proto::McState& state) const;
    bool minecraftAlive() const;
    std::uint32_t minecraftPid() const;
    DWORD error() const { return error_; }

    // Input ring (New Vegas produces). Drops the event if Minecraft is a full ring behind.
    void pushInput(proto::InputType type, std::uint16_t code, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0);
    // Collision ring (one producer thread). False when the ring is full.
    bool writeCollision(proto::ColType type, const void* payload, std::uint32_t bytes);
    void writeActors(const proto::ActorRecord* records, std::uint32_t count);
    bool popEvent(proto::McEvent& out);
    bool readWorldEntities(proto::WorldEntities& out) const;
    // Render ring (consumer). The payload points into shared memory and is valid during the call.
    void drainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes);
    bool renderRingMapped() const { return render_ != nullptr; }

    // Overlay triple buffer (consumer).
    bool acquireOverlayFrame();
    void resetOverlay();
    const proto::OverlaySlotHdr* frontHeader() const;
    // Pixels of the front slot, mapped for at least `bytes`; null if that view cannot be mapped.
    const std::uint8_t* frontPixels(std::size_t bytes);

private:
    template<class T> T* at(std::uint64_t offset) const { return reinterpret_cast<T*>(base_ + offset); }
    void* mapRange(std::uint64_t offset, std::size_t bytes, void*& view);

    HANDLE mapping_ = nullptr;
    HANDLE owner_ = nullptr;
    unsigned char* base_ = nullptr;
    void* renderView_ = nullptr;
    unsigned char* render_ = nullptr;
    void* slotView_[proto::kOverlaySlots]{};
    unsigned char* slot_[proto::kOverlaySlots]{};
    std::size_t slotBytes_[proto::kOverlaySlots]{};
    std::uint32_t overlayFront_ = 2;
    DWORD error_ = 0;
};
struct Position { double x, y, z; };
Position toMinecraft(Position p, double unitsPerBlock);
Position toNewVegas(Position p, double unitsPerBlock);
// Bethesda heading (radians, 0 north, clockwise) <-> Minecraft yaw (degrees, 0 south).
inline float headingToMcYaw(float heading) { return heading * 57.2957795f + 180.0f; }
inline float mcYawToHeading(float yaw) { return (yaw - 180.0f) * 0.0174532925f; }
} // namespace vegas
