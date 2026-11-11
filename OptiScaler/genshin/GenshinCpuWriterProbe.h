// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace GenshinCpuWriterProbe {

using Microsoft::WRL::ComPtr;

// Collection-only evidence. These process-lifetime quotas are deliberately
// never replenished by Present, owner changes, or a new source generation.
inline constexpr unsigned MaxPayloads = 4;
inline constexpr unsigned MaxPayloadBytes = 64u * 1024u;
inline constexpr unsigned MaxLoggedBytes = 4096u;
inline constexpr unsigned MaxWindows = 32;
inline constexpr unsigned MaxPayloadCopies = 32;
inline constexpr uint64_t MaxCopiedBytes = 512u * 1024u;
inline constexpr unsigned MaxSummaryLogs = 32;
inline constexpr unsigned MaxPayloadLogs = 32;
inline constexpr unsigned MaxBindingLogs = 256;
inline constexpr unsigned MaxMappings = 8;
inline constexpr unsigned MaxBindings = 128;

enum class PayloadOrigin : unsigned char { UpdateSubresource, MapUnmap };
enum class BindingStage : unsigned char { Vertex, Pixel, Compute };

// One explicit Arm owns one observation interval. Present closes collection;
// a retained LOCATION never subscribes hooks. Outstanding maps only drain.
enum class SessionPhase : unsigned char { Idle, Observing, Draining, LocationOnly };
enum class Demand : unsigned {
    Writer = 1, Payload = 2, Binding = 4, DrainMaps = 8, Output = 16,
    // Closing an interval cannot depend on having logs left to emit.
    CloseSession = 32
};
struct Session final {
    SessionPhase phase = SessionPhase::Idle;
    uint64_t serial = 0;
    unsigned writerEvents = 0, payloadAttempts = 0, bindingAttempts = 0;
    unsigned mappings = 0, reservedCopies = 0;
    unsigned payloadsPending = 0, bindingsUsed = 0;
    uint64_t reservedBytes = 0;
    bool summaryPending = false;
};

struct Payload final {
    ComPtr<ID3D11Resource> resource;
    UINT subresource = 0;
    UINT bytes = 0;
    UINT bindFlags = 0;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    DWORD thread = 0;
    uintptr_t context = 0;
    uint64_t sequence = 0;
    uint64_t drawSerial = 0;
    uint64_t hash = 1469598103934665603ull;
    PayloadOrigin origin = PayloadOrigin::UpdateSubresource;
    D3D11_BUFFER_DESC descriptor{};
    bool pending = false;
    std::array<unsigned char, MaxPayloadBytes> data{};
};

struct Mapping final {
    ComPtr<ID3D11Resource> resource;
    UINT subresource = 0;
    void* data = nullptr;
    UINT bytes = 0;
    DWORD thread = 0;
    uintptr_t context = 0;
    bool active = false;
    uint64_t session = 0;
    D3D11_BUFFER_DESC descriptor{};
};

struct Binding final {
    ComPtr<ID3D11Buffer> resource;
    D3D11_BUFFER_DESC descriptor{};
    BindingStage stage = BindingStage::Vertex;
    UINT slot = 0;
    UINT bytes = 0;
    UINT bindFlags = 0;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    DWORD thread = 0;
    uintptr_t context = 0;
    uint64_t drawSerial = 0;
};

struct ArmState final {
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Resource> target;
    DWORD thread = 0;
    UINT mip = 0;
    UINT format = DXGI_FORMAT_UNKNOWN;
    uint64_t deviceEpoch = 0;
    uint64_t producerEpoch = 0;
    uint64_t configEpoch = 0;
    uint64_t instance = 0;
    uint64_t generation = 0;
    uint64_t frame = 0;
    bool locationOnly = false;
    uintptr_t dsvResource = 0;
    uintptr_t rtvResource = 0;
    UINT dsvMip = UINT_MAX;
    UINT rtvMip = UINT_MAX;
    UINT rtvSlot = UINT_MAX;
    bool targetBound = false;
    uint64_t boundSerial = 0;
    uint64_t writerDrawSerial = 0;
    UINT drawKind = UINT_MAX;
    UINT drawCount = 0;
    UINT drawInstances = 0;
    DWORD lastWriterThread = 0;
    uintptr_t lastWriterContext = 0;
    uint64_t boundCount = 0;
    uint64_t drawCalls = 0;
    uint64_t updateCalls = 0;
    uint64_t mapCalls = 0;
    uint64_t unmapCalls = 0;
    uint64_t wrongContextCalls = 0;
    uint64_t deferredMetadataCalls = 0;
    uint64_t nonConstantBufferUpdates = 0;
    uint64_t rejectedUpdates = 0;
    uint64_t mapWrites = 0;
    uint64_t mappedConstantBuffers = 0;
    uint64_t mappedPayloads = 0;
    uint64_t constantBufferBinds = 0;
    uint64_t bindingDrops = 0;
};

struct State final {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    ArmState arm;
    Session session;
    uint64_t nextSession = 1;
    std::atomic<unsigned> demand{0};
    std::array<Payload, MaxPayloads> payloads{};
    std::array<Mapping, MaxMappings> mappings{};
    std::array<Binding, MaxBindings> bindings{};
    uint64_t nextSequence = 1;
    unsigned windowsRemaining = MaxWindows;
    unsigned payloadCopiesRemaining = MaxPayloadCopies;
    uint64_t copiedBytesRemaining = MaxCopiedBytes;
    unsigned summaryLogsRemaining = MaxSummaryLogs;
    unsigned payloadLogsRemaining = MaxPayloadLogs;
    unsigned bindingLogsRemaining = MaxBindingLogs;
    std::atomic<uint64_t> droppedBusy{0};
    std::atomic<uint64_t> droppedOversize{0};
    std::atomic<bool> pendingRevoke{false};
    // Retained location only; hot-hook demand is published separately. Legacy
    // owner cleanup may still inspect arm.context/thread while holding busy.
    std::atomic<bool> windowActive{false};
    // Process-lifetime collection counters. They distinguish an absent
    // producer expectation from a hook/context mismatch without changing any
    // depth or FG admission decision.
    std::atomic<uint64_t> expectationCalls{0};
    std::atomic<uint64_t> armCalls{0};
    std::atomic<uint64_t> armRejectedPrerequisites{0};
    std::atomic<uint64_t> armRejectedContextType{0};
    std::atomic<uint64_t> armAccepted{0};
    std::atomic<uint64_t> bindingCalls{0};
    std::atomic<uint64_t> bindingNoWindow{0};
    std::atomic<uint64_t> bindingContextMismatch{0};
    std::atomic<uint64_t> bindingAccepted{0};
};

// One process-wide state object is defined in GenshinCpuWriterProbe.cpp.
// Keeping storage out of this header is required because ExpectDepthInput,
// D3D11 binding trampolines, and Present run from different product TUs.
extern State state;

// Every fast negative reads only atomics. pendingRevoke always reaches a
// try-lock cleanup even when no new collection/output is subscribed.
inline bool HasDemand(Demand wanted) noexcept {
    return (state.demand.load(std::memory_order_acquire) & static_cast<unsigned>(wanted)) != 0 ||
        state.pendingRevoke.load(std::memory_order_acquire);
}
inline bool HasPublishedWork() noexcept {
    return state.demand.load(std::memory_order_acquire) != 0 ||
        state.pendingRevoke.load(std::memory_order_acquire);
}
// New diagnostic collection and outstanding Map cleanup are separate consumers.
// Neither predicate owns application writeback or any SR/NR/FG/GPU lifetime.
inline bool HasCollectionDemand() noexcept {
    constexpr unsigned collection = static_cast<unsigned>(Demand::Writer) |
        static_cast<unsigned>(Demand::Payload) | static_cast<unsigned>(Demand::Binding);
    return (state.demand.load(std::memory_order_acquire) & collection) != 0 &&
        !state.pendingRevoke.load(std::memory_order_acquire);
}
inline bool HasPendingMapCleanup() noexcept { return HasDemand(Demand::DrainMaps); }

// Called only under busy, including the existing Legacy idle-owner cleanup.
void ClearWindow() noexcept;
class TryLock final {
    State& owner_;
    bool held_ = false;
public:
    explicit TryLock(State& owner) noexcept : owner_(owner), held_(!owner_.busy.test_and_set(std::memory_order_acquire)) {
        if (held_ && owner_.pendingRevoke.exchange(false, std::memory_order_acq_rel)) ClearWindow();
    }
    ~TryLock() { if (held_) owner_.busy.clear(std::memory_order_release); }
    TryLock(const TryLock&) = delete;
    TryLock& operator=(const TryLock&) = delete;
    explicit operator bool() const noexcept { return held_; }
};

void Revoke(ID3D11DeviceContext*) noexcept;
// Stop new collection and discard diagnostic outputs. Registered CPU pointers
// remain cleanup-only until Unmap/revoke; they are never copied after stopping.
// Semantic/Portable mandatory driver writeback must run independently of this.
void StopObservation(ID3D11DeviceContext*) noexcept;
void ObserveConfig(uint64_t epoch) noexcept;
void Expectation(ID3D11DeviceContext*, ID3D11Resource*, UINT mip, uint64_t deviceEpoch,
    uint64_t producerEpoch, uint64_t configEpoch, uint64_t instance, uint64_t generation, uint64_t frame) noexcept;
// Diagnostic request only; never issued automatically by a source/guide proof.
void Arm(ID3D11DeviceContext*, ID3D11Resource*, UINT mip, uint64_t deviceEpoch,
    uint64_t producerEpoch, uint64_t configEpoch, uint64_t instance, uint64_t generation, uint64_t frame) noexcept;
void OnPresent() noexcept;

namespace detail {
void Bound(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*) noexcept;
void BeforeDraw(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT, UINT) noexcept;
void BoundConstantBuffers(ID3D11DeviceContext*, BindingStage, UINT, UINT, ID3D11Buffer* const*) noexcept;
void Updated(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT) noexcept;
void Mapped(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, D3D11_MAPPED_SUBRESOURCE*, HRESULT) noexcept;
void BeforeUnmap(ID3D11DeviceContext*, ID3D11Resource*, UINT) noexcept;
}
inline void Bound(ID3D11DeviceContext* c, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) noexcept {
    if (HasDemand(Demand::Writer)) detail::Bound(c, count, rtvs, dsv);
}
inline void BeforeDraw(ID3D11DeviceContext* c, UINT kind, UINT count, UINT start, INT base, UINT instances, UINT firstInstance) noexcept {
    if (HasDemand(Demand::Writer)) detail::BeforeDraw(c, kind, count, start, base, instances, firstInstance);
}
inline void BoundConstantBuffers(ID3D11DeviceContext* c, BindingStage stage, UINT first, UINT count, ID3D11Buffer* const* buffers) noexcept {
    if (HasDemand(Demand::Binding)) detail::BoundConstantBuffers(c, stage, first, count, buffers);
}
inline void Updated(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT flags = 0) noexcept {
    if (HasDemand(Demand::Payload)) detail::Updated(c, r, sub, box, data, flags);
}
inline void Mapped(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* data, HRESULT result) noexcept {
    if (HasDemand(Demand::Payload) || HasDemand(Demand::DrainMaps)) detail::Mapped(c, r, sub, type, data, result);
}
inline void BeforeUnmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) noexcept {
    if (HasDemand(Demand::DrainMaps)) detail::BeforeUnmap(c, r, sub);
}
} // namespace GenshinCpuWriterProbe
