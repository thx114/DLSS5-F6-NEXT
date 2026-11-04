// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

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
    std::array<unsigned char, MaxPayloadBytes> data{};
};

struct Mapping final {
    ComPtr<ID3D11Resource> resource;
    UINT subresource = 0;
    D3D11_MAP type = D3D11_MAP_READ;
    UINT bindFlags = 0;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    void* data = nullptr;
    UINT bytes = 0;
    DWORD thread = 0;
    uintptr_t context = 0;
    bool active = false;
};

struct Binding final {
    ComPtr<ID3D11Buffer> resource;
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
    // CPU diagnostic window publication only, never GPU-consumer authority.
    // Mutated under busy; false is published only after ClearWindow releases
    // every retained arm/payload/mapping/binding. Pending revocation is separate.
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
    unsigned statusLogsRemaining = 8;
};

// One process-wide state object is defined in GenshinCpuWriterProbe.cpp.
// Keeping storage out of this header is required because ExpectDepthInput,
// D3D11 binding trampolines, and Present run from different product TUs.
extern State state;

inline bool HasPublishedWork() noexcept {
    // Never inspect mutable arm/COM/container state outside busy. In particular,
    // keep a LOCATION window live until the existing owner clears it; this
    // predicate does not change collection demand or retire GPU work.
    return state.windowActive.load(std::memory_order_acquire) ||
        state.pendingRevoke.load(std::memory_order_acquire);
}

inline void ClearPayload(Payload& payload) noexcept {
    payload.resource.Reset();
    payload.subresource = payload.bytes = payload.bindFlags = 0;
    payload.usage = D3D11_USAGE_DEFAULT;
    payload.thread = 0;
    payload.context = 0;
    payload.sequence = payload.drawSerial = 0;
    payload.hash = 1469598103934665603ull;
    payload.origin = PayloadOrigin::UpdateSubresource;
}

inline void ClearMapping(Mapping& mapping) noexcept {
    mapping.resource.Reset();
    mapping.subresource = 0;
    mapping.type = D3D11_MAP_READ;
    mapping.bindFlags = 0;
    mapping.usage = D3D11_USAGE_DEFAULT;
    mapping.data = nullptr;
    mapping.bytes = 0;
    mapping.thread = 0;
    mapping.context = 0;
    mapping.active = false;
}

inline void ClearBinding(Binding& binding) noexcept {
    binding.resource.Reset();
    binding.stage = BindingStage::Vertex;
    binding.slot = binding.bytes = binding.bindFlags = 0;
    binding.usage = D3D11_USAGE_DEFAULT;
    binding.thread = 0;
    binding.context = 0;
    binding.drawSerial = 0;
}

inline void ClearWindow() noexcept {
    state.arm.context.Reset();
    state.arm.device.Reset();
    state.arm.target.Reset();
    state.arm = ArmState{};
    for (auto& payload : state.payloads) ClearPayload(payload);
    for (auto& mapping : state.mappings) ClearMapping(mapping);
    for (auto& binding : state.bindings) ClearBinding(binding);
    state.windowActive.store(false, std::memory_order_release);
}

inline void ClearFrameKeepLocation() noexcept {
    state.arm.dsvResource = state.arm.rtvResource = 0;
    state.arm.dsvMip = state.arm.rtvMip = state.arm.rtvSlot = UINT_MAX;
    state.arm.targetBound = false;
    state.arm.boundSerial = 0;
    state.arm.writerDrawSerial = 0;
    state.arm.drawKind = UINT_MAX;
    state.arm.drawCount = state.arm.drawInstances = 0;
    state.arm.lastWriterThread = 0;
    state.arm.lastWriterContext = 0;
    state.arm.generation = state.arm.frame = state.arm.instance = 0;
    state.arm.locationOnly = true;
    state.arm.boundCount = state.arm.drawCalls = state.arm.updateCalls = 0;
    state.arm.mapCalls = state.arm.unmapCalls = 0;
    state.arm.wrongContextCalls = state.arm.deferredMetadataCalls = 0;
    state.arm.nonConstantBufferUpdates = state.arm.rejectedUpdates = 0;
    state.arm.mapWrites = 0;
    state.arm.mappedConstantBuffers = state.arm.mappedPayloads = 0;
    state.arm.constantBufferBinds = state.arm.bindingDrops = 0;
    for (auto& payload : state.payloads) ClearPayload(payload);
    for (auto& mapping : state.mappings) ClearMapping(mapping);
    for (auto& binding : state.bindings) ClearBinding(binding);
}

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

inline uintptr_t Identity(IUnknown* object) noexcept {
    return reinterpret_cast<uintptr_t>(object);
}

inline uint64_t Hash(const void* source, size_t bytes) noexcept {
    const auto* data = static_cast<const unsigned char*>(source);
    uint64_t value = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) {
        value ^= data[i];
        value *= 1099511628211ull;
    }
    return value;
}

inline std::string Hex(const unsigned char* data, unsigned bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    const unsigned count = bytes > MaxLoggedBytes ? MaxLoggedBytes : bytes;
    std::string result;
    result.reserve(static_cast<size_t>(count) * 2u);
    for (unsigned i = 0; i < count; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 0x0F]);
    }
    return result;
}

inline const char* OriginName(PayloadOrigin origin) noexcept {
    return origin == PayloadOrigin::MapUnmap ? "MAP_UNMAP" : "UPDATE_SUBRESOURCE";
}

inline const char* StageName(BindingStage stage) noexcept {
    switch (stage) {
    case BindingStage::Vertex: return "VS";
    case BindingStage::Pixel: return "PS";
    case BindingStage::Compute: return "CS";
    }
    return "UNKNOWN";
}

inline void Revoke(ID3D11DeviceContext* context) noexcept {
    TryLock guard(state);
    if (!guard) {
        state.pendingRevoke.store(true, std::memory_order_release);
        state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!context || state.arm.context.Get() == context) ClearWindow();
}

inline void ObserveConfig(uint64_t epoch) noexcept {
    static std::atomic<uint64_t> lastEpoch{0};
    if (!epoch) return;
    const auto previous = lastEpoch.exchange(epoch, std::memory_order_acq_rel);
    if (previous && previous != epoch) Revoke(nullptr);
}

inline void EmitPendingPayloadsLocked(const char* association) noexcept {
    try {
        for (const auto& payload : state.payloads) {
            if (!payload.resource || !state.payloadLogsRemaining) continue;
            --state.payloadLogsRemaining;
            const auto hex = Hex(payload.data.data(), payload.bytes);
            LOG_INFO("Genshin CPU writer CB: pid={} generation={} sequence={} origin={} resource={} subresource={} bytes={} bind_flags={} usage={} thread={} context={} draw_serial={} hash={:016X} prefix_bytes={} prefix_hex={} association={} semantic=UNPROVED",
                GetCurrentProcessId(), state.arm.generation, payload.sequence,
                OriginName(payload.origin),
                Identity(payload.resource.Get()), payload.subresource, payload.bytes,
                payload.bindFlags, static_cast<unsigned>(payload.usage), payload.thread,
                payload.context, payload.drawSerial, payload.hash,
                payload.bytes > MaxLoggedBytes ? MaxLoggedBytes : payload.bytes, hex, association);
        }
    } catch (...) {
        // Diagnostics must never escape a producer or Present callback.
    }
}

inline void Expectation(ID3D11DeviceContext* context, ID3D11Resource* target,
    UINT mip, uint64_t deviceEpoch, uint64_t producerEpoch, uint64_t configEpoch,
    uint64_t instance, uint64_t generation, uint64_t frame) noexcept {
    const auto count = state.expectationCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 8) {
        LOG_INFO("Genshin CPU writer expectation: pid={} count={} context={} target={} immediate={} mip={} device_epoch={} producer_epoch={} config_epoch={} instance={} generation={} sourceFrame={}",
            GetCurrentProcessId(), count, Identity(context), Identity(target),
            context && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE,
            mip, deviceEpoch, producerEpoch, configEpoch, instance, generation, frame);
    }
}

inline void Arm(ID3D11DeviceContext* context, ID3D11Resource* target, UINT mip,
    uint64_t deviceEpoch, uint64_t producerEpoch, uint64_t configEpoch,
    uint64_t instance, uint64_t generation, uint64_t frame) noexcept {
    state.armCalls.fetch_add(1, std::memory_order_relaxed);
    if (!context || !target || !deviceEpoch || !producerEpoch || !configEpoch ||
        !instance || !generation || !frame) {
        state.armRejectedPrerequisites.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        state.armRejectedContextType.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TryLock guard(state);
    if (!guard) { state.droppedBusy.fetch_add(1, std::memory_order_relaxed); return; }
    const bool sameWindow = state.arm.target.Get() == target && state.arm.context.Get() == context &&
        state.arm.deviceEpoch == deviceEpoch && state.arm.producerEpoch == producerEpoch &&
        state.arm.configEpoch == configEpoch && state.arm.generation == generation;
    if (!sameWindow) {
        if (!state.windowsRemaining) {
            EmitPendingPayloadsLocked("BUDGET_EXHAUSTED");
            ClearWindow();
            return;
        }
        if (state.arm.target) EmitPendingPayloadsLocked("WINDOW_TRANSITION");
        --state.windowsRemaining;
        ClearWindow();
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) { ClearWindow(); return; }
    state.arm.context = context;
    state.arm.device = device;
    state.arm.target = target;
    state.arm.thread = GetCurrentThreadId();
    state.arm.mip = mip;
    state.arm.deviceEpoch = deviceEpoch;
    state.arm.producerEpoch = producerEpoch;
    state.arm.configEpoch = configEpoch;
    state.arm.instance = instance;
    state.arm.generation = generation;
    state.arm.frame = frame;
    state.arm.locationOnly = false;
    state.armAccepted.fetch_add(1, std::memory_order_relaxed);
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&texture)))) {
        texture->GetDesc(&desc);
        state.arm.format = desc.Format;
    }
    // Publish before Arm returns/unlocks: a new diagnostic window does not
    // wait for Present, a config epoch, or a thread-local cache to wake hooks.
    state.windowActive.store(true, std::memory_order_release);
}

inline bool MatchesTarget(ID3D11Resource* resource) noexcept {
    return resource && state.arm.target && resource == state.arm.target.Get();
}

inline void RecordDsv(ID3D11DepthStencilView* view) noexcept {
    if (!view) return;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!MatchesTarget(resource.Get())) return;
    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
    view->GetDesc(&desc);
    if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && desc.Texture2D.MipSlice == state.arm.mip) {
        state.arm.dsvResource = Identity(resource.Get());
        state.arm.dsvMip = desc.Texture2D.MipSlice;
        state.arm.targetBound = true;
        ++state.arm.boundCount;
    }
}

inline void RecordRtv(ID3D11RenderTargetView* view, UINT slot) noexcept {
    if (!view) return;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!MatchesTarget(resource.Get())) return;
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    view->GetDesc(&desc);
    if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && desc.Texture2D.MipSlice == state.arm.mip) {
        state.arm.rtvResource = Identity(resource.Get());
        state.arm.rtvMip = desc.Texture2D.MipSlice;
        state.arm.rtvSlot = slot;
        state.arm.targetBound = true;
        ++state.arm.boundCount;
    }
}

inline void Bound(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* renderTargets,
    ID3D11DepthStencilView* depthStencil) noexcept {
    if (!HasPublishedWork()) return;
    TryLock guard(state);
    if (!guard || !state.arm.target) {
        if (!guard) state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        ++state.arm.deferredMetadataCalls;
        return;
    }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (!contextDevice || contextDevice.Get() != state.arm.device.Get()) {
        ++state.arm.deferredMetadataCalls;
        return;
    }
    state.arm.dsvResource = state.arm.rtvResource = 0;
    state.arm.dsvMip = state.arm.rtvMip = state.arm.rtvSlot = UINT_MAX;
    state.arm.targetBound = false;
    ++state.arm.boundSerial;
    RecordDsv(depthStencil);
    if (renderTargets && count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        const UINT limit = count < 8u ? count : 8u;
        for (UINT i = 0; i < limit; ++i) RecordRtv(renderTargets[i], i);
    }
}

inline void BeforeDraw(ID3D11DeviceContext* context, UINT kind, UINT count,
    UINT start, INT base, UINT instances, UINT instanceStart) noexcept {
    if (!HasPublishedWork()) return;
    (void)start;
    (void)base;
    (void)instanceStart;
    TryLock guard(state);
    if (!guard || !state.arm.target) {
        if (!guard) state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { ++state.arm.wrongContextCalls; return; }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (!contextDevice || contextDevice.Get() != state.arm.device.Get()) { ++state.arm.wrongContextCalls; return; }
    if (!state.arm.targetBound) return;
    ++state.arm.drawCalls;
    state.arm.writerDrawSerial = state.arm.boundSerial;
    state.arm.drawKind = kind;
    state.arm.drawCount = count;
    state.arm.drawInstances = instances;
    state.arm.lastWriterThread = GetCurrentThreadId();
    state.arm.lastWriterContext = Identity(context);
}

inline void BoundConstantBuffers(ID3D11DeviceContext* context, BindingStage stage,
    UINT startSlot, UINT count, ID3D11Buffer* const* buffers) noexcept {
    if (!HasPublishedWork()) return;
    state.bindingCalls.fetch_add(1, std::memory_order_relaxed);
    TryLock guard(state);
    if (!guard || !state.arm.target) {
        state.bindingNoWindow.fetch_add(1, std::memory_order_relaxed);
        if (!guard) state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        state.bindingContextMismatch.fetch_add(1, std::memory_order_relaxed);
        ++state.arm.deferredMetadataCalls;
        return;
    }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (!contextDevice || contextDevice.Get() != state.arm.device.Get()) {
        state.bindingContextMismatch.fetch_add(1, std::memory_order_relaxed);
        ++state.arm.deferredMetadataCalls;
        return;
    }
    if (!buffers || !count) return;
    const UINT limit = count < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ? count :
        D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    for (UINT i = 0; i < limit; ++i) {
        ID3D11Buffer* buffer = buffers[i];
        if (!buffer) continue;
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || !desc.ByteWidth ||
            desc.ByteWidth > MaxPayloadBytes || (desc.ByteWidth & 15u) != 0) continue;
        state.bindingAccepted.fetch_add(1, std::memory_order_relaxed);
        ++state.arm.constantBufferBinds;
        const UINT slot = startSlot + i;
        bool duplicate = false;
        for (const auto& existing : state.bindings) {
            if (existing.resource.Get() == buffer && existing.stage == stage &&
                existing.slot == slot && existing.drawSerial == state.arm.boundSerial) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        Binding* destination = nullptr;
        for (auto& existing : state.bindings) {
            if (!existing.resource) {
                destination = &existing;
                break;
            }
        }
        if (!destination) {
            ++state.arm.bindingDrops;
            continue;
        }
        destination->resource = buffer;
        destination->stage = stage;
        destination->slot = slot;
        destination->bytes = desc.ByteWidth;
        destination->bindFlags = desc.BindFlags;
        destination->usage = desc.Usage;
        destination->thread = GetCurrentThreadId();
        destination->context = Identity(context);
        destination->drawSerial = state.arm.boundSerial;
    }
}

inline void RecordUpdate(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, const D3D11_BOX* box, const void* data, UINT flags) noexcept {
    if (!HasPublishedWork()) return;
    if (!context || !resource) return;
    TryLock guard(state);
    if (!guard || !state.arm.target) {
        if (!guard) state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++state.arm.updateCalls;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { ++state.arm.wrongContextCalls; return; }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (!contextDevice || contextDevice.Get() != state.arm.device.Get()) { ++state.arm.wrongContextCalls; return; }
    state.arm.lastWriterThread = GetCurrentThreadId();
    state.arm.lastWriterContext = Identity(context);
    if (subresource != 0 || box || !data || flags != 0) { ++state.arm.rejectedUpdates; return; }

    ComPtr<ID3D11Device> device;
    resource->GetDevice(&device);
    if (!device || device.Get() != state.arm.device.Get()) { ++state.arm.wrongContextCalls; return; }
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) {
        ++state.arm.nonConstantBufferUpdates;
        return;
    }
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || desc.Usage != D3D11_USAGE_DEFAULT ||
        desc.CPUAccessFlags != 0 || desc.MiscFlags != 0 || !desc.ByteWidth ||
        desc.ByteWidth > MaxPayloadBytes || (desc.ByteWidth & 15u) != 0) {
        ++state.arm.rejectedUpdates;
        if (desc.ByteWidth > MaxPayloadBytes) state.droppedOversize.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!state.payloadCopiesRemaining || state.copiedBytesRemaining < desc.ByteWidth) {
        ++state.arm.rejectedUpdates;
        return;
    }

    Payload* destination = nullptr;
    for (auto& payload : state.payloads) {
        if (payload.resource.Get() == resource && payload.subresource == subresource) {
            destination = &payload;
            break;
        }
    }
    if (!destination) {
        destination = &state.payloads[0];
        for (auto& payload : state.payloads)
            if (!payload.resource) { destination = &payload; break; }
            else if (payload.sequence < destination->sequence) destination = &payload;
    }
    destination->resource = resource;
    destination->subresource = subresource;
    destination->bytes = desc.ByteWidth;
    destination->bindFlags = desc.BindFlags;
    destination->usage = desc.Usage;
    destination->thread = GetCurrentThreadId();
    destination->context = Identity(context);
    destination->sequence = state.nextSequence++;
    destination->drawSerial = state.arm.writerDrawSerial;
    destination->origin = PayloadOrigin::UpdateSubresource;
    std::memcpy(destination->data.data(), data, desc.ByteWidth);
    destination->hash = Hash(destination->data.data(), desc.ByteWidth);
    --state.payloadCopiesRemaining;
    state.copiedBytesRemaining -= desc.ByteWidth;
}

inline bool RecordMappedPayload(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, const void* data, UINT bytes, UINT bindFlags, D3D11_USAGE usage) noexcept {
    if (!context || !resource || !data || !bytes || bytes > MaxPayloadBytes || (bytes & 15u) != 0) {
        ++state.arm.rejectedUpdates;
        if (bytes > MaxPayloadBytes) state.droppedOversize.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!state.payloadCopiesRemaining || state.copiedBytesRemaining < bytes) {
        ++state.arm.rejectedUpdates;
        return false;
    }

    Payload* destination = nullptr;
    for (auto& payload : state.payloads) {
        if (payload.resource.Get() == resource && payload.subresource == subresource) {
            destination = &payload;
            break;
        }
    }
    if (!destination) {
        destination = &state.payloads[0];
        for (auto& payload : state.payloads)
            if (!payload.resource) { destination = &payload; break; }
            else if (payload.sequence < destination->sequence) destination = &payload;
    }
    destination->resource = resource;
    destination->subresource = subresource;
    destination->bytes = bytes;
    destination->bindFlags = bindFlags;
    destination->usage = usage;
    destination->thread = GetCurrentThreadId();
    destination->context = Identity(context);
    destination->sequence = state.nextSequence++;
    destination->drawSerial = state.arm.writerDrawSerial;
    destination->origin = PayloadOrigin::MapUnmap;
    std::memcpy(destination->data.data(), data, bytes);
    destination->hash = Hash(destination->data.data(), bytes);
    --state.payloadCopiesRemaining;
    state.copiedBytesRemaining -= bytes;
    return true;
}

inline void Updated(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, const D3D11_BOX* box, const void* data, UINT flags = 0) noexcept {
    RecordUpdate(context, resource, subresource, box, data, flags);
}

inline void Mapped(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* mapped, HRESULT result) noexcept {
    if (!HasPublishedWork()) return;
    TryLock guard(state);
    if (!guard || !state.arm.target) {
        if (!guard) state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++state.arm.mapCalls;
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { ++state.arm.wrongContextCalls; return; }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (!contextDevice || contextDevice.Get() != state.arm.device.Get()) { ++state.arm.wrongContextCalls; return; }
    if (FAILED(result) || !resource || !mapped || !mapped->pData ||
        type < D3D11_MAP_WRITE || type > D3D11_MAP_WRITE_NO_OVERWRITE) return;
    ++state.arm.mapWrites;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) return;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || desc.Usage != D3D11_USAGE_DYNAMIC ||
        !(desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) || !desc.ByteWidth ||
        desc.ByteWidth > MaxPayloadBytes || (desc.ByteWidth & 15u) != 0) return;
    Mapping* slot = nullptr;
    for (auto& mapping : state.mappings)
        if (mapping.active && mapping.resource.Get() == resource && mapping.subresource == subresource) { slot = &mapping; break; }
    if (!slot) for (auto& mapping : state.mappings) if (!mapping.active) { slot = &mapping; break; }
    if (!slot) { ++state.arm.rejectedUpdates; return; }
    slot->resource = resource;
    slot->subresource = subresource;
    slot->type = type;
    slot->bindFlags = desc.BindFlags;
    slot->usage = desc.Usage;
    slot->data = mapped->pData;
    slot->bytes = desc.ByteWidth;
    slot->thread = GetCurrentThreadId();
    slot->context = Identity(context);
    slot->active = true;
    ++state.arm.mappedConstantBuffers;
}

inline void BeforeUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource) noexcept {
    if (!HasPublishedWork()) return;
    TryLock guard(state);
    if (!guard || !state.arm.target) {
        if (!guard) state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++state.arm.unmapCalls;
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { ++state.arm.wrongContextCalls; return; }
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    if (!contextDevice || contextDevice.Get() != state.arm.device.Get()) { ++state.arm.wrongContextCalls; return; }
    for (auto& mapping : state.mappings) {
        if (!mapping.active || mapping.resource.Get() != resource || mapping.subresource != subresource) continue;
        if (context && contextDevice && contextDevice.Get() == state.arm.device.Get() &&
            mapping.context == Identity(context) &&
            mapping.thread == GetCurrentThreadId() && mapping.data) {
            state.arm.lastWriterThread = mapping.thread;
            state.arm.lastWriterContext = mapping.context;
            if (RecordMappedPayload(context, resource, subresource, mapping.data, mapping.bytes,
                mapping.bindFlags, mapping.usage)) ++state.arm.mappedPayloads;
        }
        ClearMapping(mapping);
        break;
    }
}

inline void OnPresent() noexcept {
    if (!HasPublishedWork()) return;
    TryLock guard(state);
    if (!guard) {
        if (!guard) state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!state.arm.target) {
        if (state.statusLogsRemaining) {
            --state.statusLogsRemaining;
            LOG_INFO("Genshin CPU writer probe status: pid={} expectation_calls={} arm_calls={} arm_rejected_prerequisites={} arm_rejected_context={} arm_accepted={} binding_calls={} binding_no_window={} binding_context_mismatch={} binding_accepted={} installed_window=false semantic=UNPROVED association=UNPROVED",
                GetCurrentProcessId(), state.expectationCalls.load(std::memory_order_relaxed),
                state.armCalls.load(std::memory_order_relaxed),
                state.armRejectedPrerequisites.load(std::memory_order_relaxed),
                state.armRejectedContextType.load(std::memory_order_relaxed),
                state.armAccepted.load(std::memory_order_relaxed),
                state.bindingCalls.load(std::memory_order_relaxed),
                state.bindingNoWindow.load(std::memory_order_relaxed),
                state.bindingContextMismatch.load(std::memory_order_relaxed),
                state.bindingAccepted.load(std::memory_order_relaxed));
        }
        return;
    }
    try {
        if (state.summaryLogsRemaining) {
            --state.summaryLogsRemaining;
            const auto& arm = state.arm;
            LOG_INFO("Genshin CPU writer probe: pid={} generation={} frame={} instance={} location_only={} target={} mip={} format={} arm_thread={} last_writer_thread={} last_writer_context={} dsv={} dsv_mip={} rtv={} rtv_mip={} rtv_slot={} target_bound={} bound={} deferred_meta={} draws={} writer_draw_serial={} draw_kind={} draw_count={} instances={} updates={} map={} map_writes={} unmap={} mapped_cb={} mapped_payloads={} cb_binds={} binding_drops={} wrong_context={} non_cb_updates={} rejected_updates={} windows_left={} copies_left={} bytes_left={} dropped_busy={} dropped_oversize={} expectation_calls={} arm_calls={} arm_rejected_prerequisites={} arm_rejected_context={} arm_accepted={} binding_calls={} binding_no_window={} binding_context_mismatch={} binding_accepted={} semantic=UNPROVED association=UNPROVED fg_eligible=false",
                GetCurrentProcessId(), arm.generation, arm.frame, arm.instance, arm.locationOnly,
                Identity(arm.target.Get()), arm.mip, arm.format, arm.thread,
                arm.lastWriterThread, arm.lastWriterContext,
                arm.dsvResource, arm.dsvMip, arm.rtvResource, arm.rtvMip, arm.rtvSlot,
                arm.targetBound, arm.boundCount, arm.deferredMetadataCalls, arm.drawCalls,
                arm.writerDrawSerial, arm.drawKind, arm.drawCount, arm.drawInstances,
                arm.updateCalls, arm.mapCalls, arm.mapWrites, arm.unmapCalls, arm.mappedConstantBuffers, arm.mappedPayloads,
                arm.constantBufferBinds, arm.bindingDrops, arm.wrongContextCalls, arm.nonConstantBufferUpdates, arm.rejectedUpdates,
                state.windowsRemaining, state.payloadCopiesRemaining, state.copiedBytesRemaining,
                state.droppedBusy.load(std::memory_order_relaxed), state.droppedOversize.load(std::memory_order_relaxed),
                state.expectationCalls.load(std::memory_order_relaxed), state.armCalls.load(std::memory_order_relaxed),
                state.armRejectedPrerequisites.load(std::memory_order_relaxed), state.armRejectedContextType.load(std::memory_order_relaxed),
                state.armAccepted.load(std::memory_order_relaxed), state.bindingCalls.load(std::memory_order_relaxed),
                state.bindingNoWindow.load(std::memory_order_relaxed), state.bindingContextMismatch.load(std::memory_order_relaxed),
                state.bindingAccepted.load(std::memory_order_relaxed));
        }
        EmitPendingPayloadsLocked(state.arm.locationOnly ? "LOCATION_ONLY" : "PRESENT");
        for (const auto& binding : state.bindings) {
            if (!binding.resource || !state.bindingLogsRemaining) continue;
            --state.bindingLogsRemaining;
            bool payloadMatch = false;
            for (const auto& payload : state.payloads) {
                if (payload.resource.Get() == binding.resource.Get()) {
                    payloadMatch = true;
                    break;
                }
            }
            LOG_INFO("Genshin CPU writer CB binding: pid={} generation={} frame={} stage={} resource={} slot={} bytes={} bind_flags={} usage={} thread={} context={} draw_serial={} payload_match={} semantic=UNPROVED association=UNPROVED",
                GetCurrentProcessId(), state.arm.generation, state.arm.frame,
                StageName(binding.stage), Identity(binding.resource.Get()), binding.slot,
                binding.bytes, binding.bindFlags, static_cast<unsigned>(binding.usage),
                binding.thread, binding.context, binding.drawSerial, payloadMatch);
        }
    } catch (...) {
        // Diagnostics must never escape the Present callback.
    }
    // Keep the authenticated resource location for the next bounded writer
    // interval. Its previous frame owner is explicitly retired above.
    ClearFrameKeepLocation();
}

} // namespace GenshinCpuWriterProbe
