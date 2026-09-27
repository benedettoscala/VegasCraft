#include "Bridge.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace vegas {
namespace {
LONG64 read64(const std::uint64_t* p) {
    return InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(const_cast<std::uint64_t*>(p)), 0, 0);
}
LONG read32(const std::uint32_t* p) {
    return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(const_cast<std::uint32_t*>(p)), 0, 0);
}
void store64(std::uint64_t* p, std::uint64_t v) {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(p), static_cast<LONG64>(v));
}
// Seqlock writer: seq odd while the body changes.
template<class T> void seqWrite(T* dst, const T& src) {
    auto* seq = reinterpret_cast<volatile LONG*>(dst);
    InterlockedIncrement(seq);
    std::memcpy(reinterpret_cast<unsigned char*>(dst) + 4, reinterpret_cast<const unsigned char*>(&src) + 4, sizeof(T) - 4);
    InterlockedIncrement(seq);
}
constexpr std::uint64_t kLowViewBytes = proto::kOffCollisionRing + proto::kCollisionRingBytes;
constexpr DWORD kGranularity = 0x10000;
}
Bridge::~Bridge() { close(); }
bool Bridge::open(const std::wstring& name) {
    if (base_) return true;
    owner_ = CreateMutexW(nullptr, FALSE, (name + L"_host").c_str());
    const auto mutexError = GetLastError();
    if (!owner_ || mutexError == ERROR_ALREADY_EXISTS) {
        error_ = owner_ ? ERROR_ALREADY_EXISTS : mutexError;
        close();
        return false;
    }
    constexpr auto size = proto::kMappingBytes;
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), name.c_str());
    if (!mapping_) { error_ = GetLastError(); close(); return false; }
    // A Minecraft that outlived the previous New Vegas still holds the object open. As SkyCraft
    // does, keep it and reset everything the host owns; Minecraft sees the new host PID and
    // restarts its side (generation bump).
    base_ = static_cast<unsigned char*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(kLowViewBytes)));
    if (!base_) { error_ = GetLastError(); close(); return false; }
    // The render ring is 64 MB of contiguous address space: New Vegas is a 32-bit process without
    // Large Address Aware, and after a save or two its 2 GB are too fragmented for that much in one
    // piece (Windows error 8). Take it now, while the address space is still clean; drainRender
    // tries again later if this fails.
    if (!render_) render_ = static_cast<unsigned char*>(mapRange(proto::kOffRenderRing, static_cast<std::size_t>(proto::kRenderRingBytes), renderView_));
    std::memset(base_ + proto::kOffSkyState, 0, sizeof(proto::SkyState));
    std::memset(base_ + proto::kOffArmPose, 0, sizeof(proto::ArmPose));
    std::memset(base_ + proto::kOffOverlayCtl, 0, 0x100);
    std::memset(base_ + proto::kOffInputRing, 0, proto::kInputRingDataOff);
    std::memset(base_ + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
    std::memset(base_ + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
    std::memset(base_ + proto::kOffEventRing, 0, proto::kEventRingDataOff);
    std::memset(base_ + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
    auto* water = at<proto::WaterGrid>(proto::kOffWaterGrid);
    std::memset(water, 0, sizeof(*water));
    for (auto& surface : water->surface) surface = proto::kNoWater;
    auto* header = at<proto::Header>(proto::kOffHeader);
    header->version = proto::kVersion;
    header->skyrimPid = GetCurrentProcessId(); // Legacy field name: here this is FalloutNV's PID.
    heartbeat();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&header->magic), proto::kMagic);
    return true;
}
void* Bridge::mapRange(std::uint64_t offset, std::size_t bytes, void*& view) {
    const auto aligned = offset & ~std::uint64_t(kGranularity - 1);
    const auto lead = static_cast<std::size_t>(offset - aligned);
    view = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned), lead + bytes);
    if (!view) { error_ = GetLastError(); return nullptr; }
    return static_cast<unsigned char*>(view) + lead;
}
void Bridge::close() {
    if (base_) {
        auto* header = at<proto::Header>(proto::kOffHeader);
        store64(&header->skyrimHeartbeatMs, 0);
    }
    for (std::uint32_t i = 0; i < proto::kOverlaySlots; ++i) {
        if (slotView_[i]) UnmapViewOfFile(slotView_[i]);
        slotView_[i] = nullptr; slot_[i] = nullptr; slotBytes_[i] = 0;
    }
    if (renderView_) { UnmapViewOfFile(renderView_); renderView_ = nullptr; render_ = nullptr; }
    if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
    if (owner_) { CloseHandle(owner_); owner_ = nullptr; }
}
void Bridge::heartbeat() {
    if (!base_) return;
    store64(&at<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs, GetTickCount64());
}
void Bridge::publishArms(const proto::ArmPose& arms) {
    if (base_) seqWrite(at<proto::ArmPose>(proto::kOffArmPose), arms);
}
void Bridge::publish(const proto::SkyState& state) {
    if (base_) seqWrite(at<proto::SkyState>(proto::kOffSkyState), state);
}
void Bridge::writeWaterGrid(const proto::WaterGrid& grid) {
    if (base_) seqWrite(at<proto::WaterGrid>(proto::kOffWaterGrid), grid);
}
bool Bridge::readMinecraft(proto::McState& state) const {
    if (!base_ || !minecraftAlive()) return false;
    auto* src = at<proto::McState>(proto::kOffMcState);
    for (int attempt = 0; attempt < 64; ++attempt) {
        auto seq = read32(&src->seq);
        if (seq & 1) { YieldProcessor(); continue; }
        std::memcpy(&state, src, sizeof(state));
        MemoryBarrier();
        if (read32(&src->seq) != seq) continue;
        return (state.flags & proto::kMcInWorld) && std::isfinite(state.x) &&
            std::isfinite(state.y) && std::isfinite(state.z) &&
            std::isfinite(state.yaw) && std::isfinite(state.pitch);
    }
    return false;
}
bool Bridge::minecraftAlive() const {
    if (!base_) return false;
    auto* h = at<proto::Header>(proto::kOffHeader);
    auto beat = static_cast<std::uint64_t>(read64(&h->mcHeartbeatMs));
    auto now = GetTickCount64();
    return read32(&h->mcPid) != 0 && beat != 0 && beat <= now && now - beat < 3000;
}
std::uint32_t Bridge::minecraftPid() const {
    return base_ ? static_cast<std::uint32_t>(read32(&at<proto::Header>(proto::kOffHeader)->mcPid)) : 0;
}
void Bridge::pushInput(proto::InputType type, std::uint16_t code, std::int32_t a, std::int32_t b, std::int32_t c) {
    if (!base_) return;
    auto* ring = base_ + proto::kOffInputRing;
    auto* head = reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff);
    auto* tail = reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff);
    const auto h = static_cast<std::uint64_t>(read64(head));
    if (h - static_cast<std::uint64_t>(read64(tail)) >= proto::kInputRingEntries) return;
    auto* entry = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff) + (h & (proto::kInputRingEntries - 1));
    *entry = {static_cast<std::uint16_t>(type), code, a, b, c};
    MemoryBarrier();
    store64(head, h + 1);
}
bool Bridge::writeCollision(proto::ColType type, const void* payload, std::uint32_t bytes) {
    if (!base_) return false;
    auto* ring = base_ + proto::kOffCollisionRing;
    auto* headRef = reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
    auto* tailRef = reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
    auto* data = ring + proto::kColRingDataOff;
    constexpr auto size = proto::kColRingDataBytes;
    const std::uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + bytes + 7) & ~7ull;
    if (msgBytes > size / 2) return false;
    auto head = static_cast<std::uint64_t>(read64(headRef));
    const auto tail = static_cast<std::uint64_t>(read64(tailRef));
    auto pos = head % size;
    const auto pad = (pos + msgBytes > size) ? size - pos : 0;
    if (size - (head - tail) < msgBytes + pad) return false;
    if (pad) {
        *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = {proto::kColPad, 0};
        head += pad; pos = 0;
    }
    *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = {type, bytes};
    if (bytes) std::memcpy(data + pos + sizeof(proto::ColMsgHeader), payload, bytes);
    MemoryBarrier();
    store64(headRef, head + msgBytes);
    return true;
}
void Bridge::writeActors(const proto::ActorRecord* records, std::uint32_t count) {
    if (!base_) return;
    auto* table = at<proto::ActorTable>(proto::kOffActorTable);
    auto* seq = reinterpret_cast<volatile LONG*>(&table->seq);
    InterlockedIncrement(seq);
    count = std::min(count, proto::kMaxActors);
    table->count = count;
    if (count) std::memcpy(table->actors, records, sizeof(proto::ActorRecord) * count);
    InterlockedIncrement(seq);
}
bool Bridge::popEvent(proto::McEvent& out) {
    if (!base_) return false;
    auto* ring = base_ + proto::kOffEventRing;
    auto* headRef = reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingHeadOff);
    auto* tailRef = reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingTailOff);
    const auto head = static_cast<std::uint64_t>(read64(headRef));
    auto tail = static_cast<std::uint64_t>(read64(tailRef));
    if (tail >= head) return false;
    if (head - tail > proto::kEventRingEntries) tail = head - proto::kEventRingEntries;
    out = reinterpret_cast<const proto::McEvent*>(ring + proto::kEventRingDataOff)[tail & (proto::kEventRingEntries - 1)];
    MemoryBarrier();
    store64(tailRef, tail + 1);
    return true;
}
bool Bridge::readWorldEntities(proto::WorldEntities& out) const {
    if (!base_) return false;
    auto* src = at<proto::WorldEntities>(proto::kOffWorldEntities);
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto s1 = read32(&src->seq);
        if (s1 & 1) { YieldProcessor(); continue; }
        const auto count = std::min(src->count, proto::kMaxWorldEntities);
        std::memcpy(&out, src, offsetof(proto::WorldEntities, entities) + sizeof(proto::WorldEntity) * count);
        out.count = count;
        MemoryBarrier();
        if (read32(&src->seq) == s1) return true;
    }
    return false;
}
void Bridge::drainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& fn, std::uint64_t maxBytes) {
    if (!base_) return;
    if (!render_) {
        render_ = static_cast<unsigned char*>(mapRange(proto::kOffRenderRing, static_cast<std::size_t>(proto::kRenderRingBytes), renderView_));
        if (!render_) return;
    }
    auto* headRef = reinterpret_cast<std::uint64_t*>(render_ + proto::kRenRingHeadOff);
    auto* tailRef = reinterpret_cast<std::uint64_t*>(render_ + proto::kRenRingTailOff);
    const auto head = static_cast<std::uint64_t>(read64(headRef));
    auto tail = static_cast<std::uint64_t>(read64(tailRef));
    auto* data = render_ + proto::kRenRingDataOff;
    constexpr auto size = proto::kRenRingDataBytes;
    std::uint64_t done = 0;
    while (tail < head && done < maxBytes) {
        const auto pos = tail % size;
        const auto* hdr = reinterpret_cast<const proto::ColMsgHeader*>(data + pos);
        if (hdr->type == proto::kRenPad) { tail += size - pos; continue; }
        fn(hdr->type, data + pos + sizeof(proto::ColMsgHeader), hdr->payloadBytes);
        const auto msgBytes = (sizeof(proto::ColMsgHeader) + hdr->payloadBytes + 7) & ~7ull;
        tail += msgBytes; done += msgBytes;
    }
    MemoryBarrier();
    store64(tailRef, tail);
}
bool Bridge::acquireOverlayFrame() {
    if (!base_) return false;
    auto* state = reinterpret_cast<volatile LONG*>(&at<proto::OverlayCtl>(proto::kOffOverlayCtl)->state);
    if (!(InterlockedCompareExchange(state, 0, 0) & proto::kOverlayDirty)) return false;
    const auto old = static_cast<std::uint32_t>(InterlockedExchange(state, static_cast<LONG>(overlayFront_)));
    overlayFront_ = old & 3;
    return true;
}
void Bridge::resetOverlay() {
    if (!base_) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&at<proto::OverlayCtl>(proto::kOffOverlayCtl)->state), 0);
    overlayFront_ = 2;
}
const proto::OverlaySlotHdr* Bridge::frontHeader() const {
    if (!base_ || overlayFront_ >= proto::kOverlaySlots) return nullptr;
    return at<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr + sizeof(proto::OverlaySlotHdr) * overlayFront_);
}
const std::uint8_t* Bridge::frontPixels(std::size_t bytes) {
    const auto slot = overlayFront_;
    if (!base_ || slot >= proto::kOverlaySlots || bytes > proto::kOverlaySlotBytes) return nullptr;
    if (slot_[slot] && slotBytes_[slot] >= bytes) return slot_[slot];
    if (slotView_[slot]) { UnmapViewOfFile(slotView_[slot]); slotView_[slot] = nullptr; slot_[slot] = nullptr; }
    slot_[slot] = static_cast<unsigned char*>(mapRange(proto::kOffOverlayPixels + proto::kOverlaySlotBytes * slot, bytes, slotView_[slot]));
    slotBytes_[slot] = slot_[slot] ? bytes : 0;
    return slot_[slot];
}
Position toMinecraft(Position p, double scale) { return {p.x / scale, p.z / scale, -p.y / scale}; }
Position toNewVegas(Position p, double scale) { return {p.x * scale, -p.z * scale, p.y * scale}; }
} // namespace vegas
