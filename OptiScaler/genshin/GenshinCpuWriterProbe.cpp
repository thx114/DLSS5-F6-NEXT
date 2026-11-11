// SPDX-License-Identifier: GPL-3.0-or-later

#include <SysUtils.h>
#include "GenshinCpuWriterProbe.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace GenshinCpuWriterProbe {

State state{};

namespace {
constexpr unsigned Bit(Demand demand) noexcept { return static_cast<unsigned>(demand); }
uintptr_t Identity(IUnknown* object) noexcept { return reinterpret_cast<uintptr_t>(object); }
bool Observing() noexcept { return state.session.phase == SessionPhase::Observing; }

// Capacity is reserved for already registered maps, so updates/new maps cannot
// consume the last copy/byte/log needed by an exact pending Unmap. All counters
// are process lifetime; only the stricter per-session attempt caps restart.
bool PayloadCapacity(UINT bytes) noexcept {
    const auto& s = state.session;
    return Observing() && bytes && state.nextSequence && state.nextSequence != UINT64_MAX &&
        state.payloadCopiesRemaining > s.reservedCopies &&
        state.copiedBytesRemaining >= s.reservedBytes &&
        bytes <= state.copiedBytesRemaining - s.reservedBytes &&
        state.payloadLogsRemaining > s.payloadsPending + s.reservedCopies;
}
bool CanCollectPayload() noexcept { return state.session.payloadAttempts && PayloadCapacity(16); }
bool CanCollectBinding() noexcept {
    return Observing() && state.session.bindingAttempts &&
        state.session.bindingsUsed < MaxBindings && state.session.bindingsUsed < state.bindingLogsRemaining;
}
void PublishDemand() noexcept {
    auto& s = state.session;
    unsigned demand = 0;
    const bool payload = CanCollectPayload();
    const bool binding = CanCollectBinding();
    const bool summary = s.summaryPending && state.summaryLogsRemaining;
    // Writer metadata only serves a pending summary or future payload/binding.
    // Exhausted output consumers cannot keep Draw/OM observation subscribed.
    if (Observing() && s.writerEvents && (summary || payload || binding || s.mappings)) demand |= Bit(Demand::Writer);
    if (payload) demand |= Bit(Demand::Payload);
    if (binding) demand |= Bit(Demand::Binding);
    if (s.mappings) demand |= Bit(Demand::DrainMaps);
    if (Observing()) demand |= Bit(Demand::CloseSession);
    if (summary || (s.payloadsPending && state.payloadLogsRemaining) ||
        (s.bindingsUsed && state.bindingLogsRemaining)) demand |= Bit(Demand::Output);
    if (state.pendingRevoke.load(std::memory_order_acquire)) demand = 0;
    state.demand.store(demand, std::memory_order_release);
}
struct Publication final { ~Publication() { PublishDemand(); } };
void ClearPayload(Payload& p) noexcept {
    p.resource.Reset();p.bytes = 0;p.sequence = 0;p.pending = false;p.descriptor = {};
}
void ClearBinding(Binding& b) noexcept { b.resource.Reset();b.descriptor = {}; }
void ClearMapping(Mapping& mapping) noexcept {
    if (!mapping.active) return;
    --state.session.mappings;--state.session.reservedCopies;
    state.session.reservedBytes -= mapping.bytes;
    mapping.resource.Reset();mapping.data = nullptr;mapping.bytes = 0;
    mapping.active = false;mapping.session = 0;mapping.descriptor = {};
    if (!Observing() && !state.session.mappings)
        state.session.phase = state.arm.target ? SessionPhase::LocationOnly : SessionPhase::Idle;
}
bool ContextMatches(ID3D11DeviceContext* context) noexcept {
    // Arm retained and qualified this exact immediate context/device pair.
    // Object type and device cannot change during that COM lifetime. This only
    // scopes diagnostics; no device-removal result or execution proof is cached.
    if (context && context == state.arm.context.Get() && GetCurrentThreadId() == state.arm.thread) return true;
    ++state.arm.wrongContextCalls;return false;
}
Mapping* FindMapping(ID3D11Resource* resource, UINT sub) noexcept {
    for (auto& m : state.mappings)
        if (m.active && m.resource.Get() == resource && m.subresource == sub) return &m;
    return nullptr;
}
bool DescribeBuffer(ID3D11Resource* resource, D3D11_BUFFER_DESC& desc, ID3D11Buffer* typed = nullptr) noexcept {
    // Reuse only descriptors of actual retained objects in this session's
    // existing pools. No pointer-only cache, extra pool or negative authority.
    for (const auto& p : state.payloads) if (p.resource.Get() == resource) { desc = p.descriptor;return true; }
    for (const auto& m : state.mappings) if (m.active && m.resource.Get() == resource) { desc = m.descriptor;return true; }
    for (unsigned i = 0; i < state.session.bindingsUsed; ++i) {
        const auto& b = state.bindings[i];
        if (b.resource.Get() == resource) { desc = b.descriptor;return true; }
    }
    ComPtr<ID3D11Device> device;resource->GetDevice(&device);
    if (!device || device.Get() != state.arm.device.Get()) return false;
    ComPtr<ID3D11Buffer> buffer;
    if (!typed) {
        if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))) || !buffer) return false;
        typed = buffer.Get();
    }
    typed->GetDesc(&desc);return true;
}
bool ConstantBuffer(const D3D11_BUFFER_DESC& d) noexcept {
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth && d.ByteWidth <= MaxPayloadBytes && !(d.ByteWidth & 15u);
}
uint64_t Hash(const void* source, size_t bytes) noexcept {
    const auto* p = static_cast<const unsigned char*>(source);uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) { hash ^= p[i];hash *= 1099511628211ull; }return hash;
}
bool RecordPayload(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub,
    const void* data, const D3D11_BUFFER_DESC& desc, PayloadOrigin origin) noexcept {
    // Existing reservations are released by BeforeUnmap immediately before
    // this call. Failed/closed sessions still clean their entries without reads.
    if (!data || !PayloadCapacity(desc.ByteWidth)) return false;
    Payload* destination = nullptr;
    for (auto& p : state.payloads) if (p.resource.Get() == resource && p.subresource == sub) { destination = &p;break; }
    if (!destination) {
        destination = &state.payloads[0];
        for (auto& p : state.payloads)
            if (!p.resource) { destination = &p;break; }
            else if (p.sequence < destination->sequence) destination = &p;
    }
    if (!destination->pending) ++state.session.payloadsPending;
    destination->resource = resource;destination->subresource = sub;destination->bytes = desc.ByteWidth;
    destination->bindFlags = desc.BindFlags;destination->usage = desc.Usage;destination->descriptor = desc;
    destination->thread = GetCurrentThreadId();destination->context = Identity(context);
    destination->sequence = state.nextSequence++;destination->drawSerial = state.arm.writerDrawSerial;
    destination->origin = origin;destination->pending = true;
    std::memcpy(destination->data.data(), data, desc.ByteWidth);destination->hash = Hash(destination->data.data(), desc.ByteWidth);
    --state.payloadCopiesRemaining;state.copiedBytesRemaining -= desc.ByteWidth;return true;
}
std::string Hex(const unsigned char* data, unsigned bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    const unsigned count = (std::min)(bytes, MaxLoggedBytes);std::string result;result.reserve(size_t(count) * 2);
    for (unsigned i = 0; i < count; ++i) { result.push_back(digits[data[i] >> 4]);result.push_back(digits[data[i] & 15]); }return result;
}
const char* OriginName(PayloadOrigin origin) noexcept { return origin == PayloadOrigin::MapUnmap ? "MAP_UNMAP" : "UPDATE_SUBRESOURCE"; }
const char* StageName(BindingStage stage) noexcept {
    switch (stage) { case BindingStage::Vertex:return "VS";case BindingStage::Pixel:return "PS";case BindingStage::Compute:return "CS"; }return "UNKNOWN";
}
void EmitPendingPayloadsLocked(const char* association) noexcept;
void ClearFrameKeepLocation() noexcept {
    // Stop collecting, but do not forget outstanding Map registrations at
    // Present. They remain drain-only until the exact/invalidating Unmap; their
    // pointers must never be copied under the retired frame or a new session.
    auto& a = state.arm;
    a.dsvResource = a.rtvResource = 0;a.dsvMip = a.rtvMip = a.rtvSlot = UINT_MAX;
    a.targetBound = false;a.boundSerial = a.writerDrawSerial = 0;
    a.instance = a.generation = a.frame = 0;a.locationOnly = true;
    for (auto& p : state.payloads) ClearPayload(p);
    for (auto& b : state.bindings) ClearBinding(b);
    auto& s = state.session;s.payloadsPending = s.bindingsUsed = 0;s.summaryPending = false;
    s.writerEvents = s.payloadAttempts = s.bindingAttempts = 0;
    s.phase = s.mappings ? SessionPhase::Draining : SessionPhase::LocationOnly;
}
} // namespace

void ClearWindow() noexcept {
    // This is diagnostic CPU ownership only. Cancel without dereferencing any
    // mapped pointer; no application writeback, GPU lease or completion lives here.
    for (auto& m : state.mappings) ClearMapping(m);
    for (auto& p : state.payloads) ClearPayload(p);
    for (auto& b : state.bindings) ClearBinding(b);
    state.arm = {};state.session = {};
    state.demand.store(0, std::memory_order_release);
    state.windowActive.store(false, std::memory_order_release);
}
void Revoke(ID3D11DeviceContext* context) noexcept {
    TryLock guard(state);
    if (!guard) { state.pendingRevoke.store(true, std::memory_order_release);state.droppedBusy.fetch_add(1, std::memory_order_relaxed);return; }
    if (!context || state.arm.context.Get() == context) ClearWindow();
}
void StopObservation(ID3D11DeviceContext* context) noexcept {
    TryLock guard(state);
    if (!guard) {
        state.pendingRevoke.store(true, std::memory_order_release);
        state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Publication publish;
    if (Observing() && (!context || state.arm.context.Get() == context)) ClearFrameKeepLocation();
}
void ObserveConfig(uint64_t epoch) noexcept {
    static std::atomic<uint64_t> lastEpoch{0};if (!epoch) return;
    const auto previous = lastEpoch.exchange(epoch, std::memory_order_acq_rel);
    if (previous && previous != epoch) Revoke(nullptr);
}
void Expectation(ID3D11DeviceContext* context, ID3D11Resource* target, UINT mip,
    uint64_t deviceEpoch, uint64_t producerEpoch, uint64_t configEpoch, uint64_t instance, uint64_t generation, uint64_t frame) noexcept {
    const auto count = state.expectationCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 8) try {
        LOG_INFO("Genshin CPU writer expectation: pid={} count={} context={} target={} immediate={} mip={} device_epoch={} producer_epoch={} config_epoch={} instance={} generation={} sourceFrame={}",
            GetCurrentProcessId(), count, Identity(context), Identity(target), context && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE,
            mip, deviceEpoch, producerEpoch, configEpoch, instance, generation, frame);
    } catch (...) {}
}
void Arm(ID3D11DeviceContext* context, ID3D11Resource* target, UINT mip,
    uint64_t deviceEpoch, uint64_t producerEpoch, uint64_t configEpoch, uint64_t instance, uint64_t generation, uint64_t frame) noexcept {
    state.armCalls.fetch_add(1, std::memory_order_relaxed);
    if (!context || !target || !deviceEpoch || !producerEpoch || !configEpoch || !instance || !generation || !frame) {
        state.armRejectedPrerequisites.fetch_add(1, std::memory_order_relaxed);return;
    }
    TryLock guard(state);if (!guard) { state.droppedBusy.fetch_add(1, std::memory_order_relaxed);return; }
    Publication publish;
    const auto thread = GetCurrentThreadId();const auto& a = state.arm;
    const bool sameWindow = Observing() && a.context.Get() == context && a.thread == thread && a.target.Get() == target && a.mip == mip &&
        a.deviceEpoch == deviceEpoch && a.producerEpoch == producerEpoch && a.configEpoch == configEpoch &&
        a.instance == instance && a.generation == generation && a.frame == frame;
    if (sameWindow) return;
    if (!state.windowsRemaining || !state.nextSession || state.nextSession == UINT64_MAX ||
        (!state.summaryLogsRemaining && !state.bindingLogsRemaining &&
         (!state.payloadLogsRemaining || !state.payloadCopiesRemaining || state.copiedBytesRemaining < 16))) {
        EmitPendingPayloadsLocked("BUDGET_EXHAUSTED");ClearWindow();return;
    }
    ComPtr<ID3D11Device> device;
    if (a.context.Get() == context && a.device) device = a.device;
    else {
        if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            state.armRejectedContextType.fetch_add(1, std::memory_order_relaxed);return;
        }
        context->GetDevice(&device);
    }
    ComPtr<ID3D11Device> targetDevice;target->GetDevice(&targetDevice);
    if (!device || targetDevice.Get() != device.Get()) return;
    UINT format = DXGI_FORMAT_UNKNOWN;
    if (a.target.Get() == target) format = a.format;
    else {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&texture))) && texture) {
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);format = desc.Format;
        }
    }
    if (a.target) EmitPendingPayloadsLocked("WINDOW_TRANSITION");
    ClearWindow();--state.windowsRemaining;
    auto& next = state.arm;
    next.context = context;next.device = device;next.target = target;next.thread = thread;next.mip = mip;next.format = format;
    next.deviceEpoch = deviceEpoch;next.producerEpoch = producerEpoch;next.configEpoch = configEpoch;
    next.instance = instance;next.generation = generation;next.frame = frame;
    auto& s = state.session;s.phase = SessionPhase::Observing;s.serial = state.nextSession++;
    // Stricter attempt limits also bound rejected/duplicate/non-CB callbacks.
    // They do not replenish any process-lifetime copy/byte/log/window quota.
    s.writerEvents = MaxBindings;s.payloadAttempts = MaxPayloadCopies;s.bindingAttempts = MaxBindings;
    s.summaryPending = state.summaryLogsRemaining != 0;
    state.armAccepted.fetch_add(1, std::memory_order_relaxed);
    state.windowActive.store(true, std::memory_order_release);
}

namespace detail {
void Bound(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) noexcept {
    TryLock guard(state);if (!guard) { state.droppedBusy.fetch_add(1, std::memory_order_relaxed);return; }
    Publication publish;
    if (!Observing() || !state.session.writerEvents) return;
    --state.session.writerEvents;if (!ContextMatches(context)) return;
    auto& a = state.arm;a.targetBound = false;a.dsvResource = a.rtvResource = 0;
    a.dsvMip = a.rtvMip = a.rtvSlot = UINT_MAX;++a.boundSerial;
    if (dsv) {
        ComPtr<ID3D11Resource> resource;dsv->GetResource(&resource);
        if (resource.Get() == a.target.Get()) {
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};dsv->GetDesc(&desc);
            if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && desc.Texture2D.MipSlice == a.mip) {
                a.dsvResource = Identity(resource.Get());a.dsvMip = a.mip;a.targetBound = true;++a.boundCount;
            }
        }
    }
    if (rtvs && count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) for (UINT slot = 0; slot < (std::min)(count, 8u); ++slot) {
        if (!rtvs[slot]) continue;ComPtr<ID3D11Resource> resource;rtvs[slot]->GetResource(&resource);
        if (resource.Get() != a.target.Get()) continue;
        D3D11_RENDER_TARGET_VIEW_DESC desc{};rtvs[slot]->GetDesc(&desc);
        if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && desc.Texture2D.MipSlice == a.mip) {
            a.rtvResource = Identity(resource.Get());a.rtvMip = a.mip;a.rtvSlot = slot;a.targetBound = true;++a.boundCount;
        }
    }
}
void BeforeDraw(ID3D11DeviceContext* context, UINT kind, UINT count, UINT, INT, UINT instances, UINT) noexcept {
    TryLock guard(state);if (!guard) { state.droppedBusy.fetch_add(1, std::memory_order_relaxed);return; }
    Publication publish;
    if (!Observing() || !state.session.writerEvents) return;
    --state.session.writerEvents;if (!ContextMatches(context) || !state.arm.targetBound) return;
    auto& a = state.arm;++a.drawCalls;a.writerDrawSerial = a.boundSerial;a.drawKind = kind;a.drawCount = count;a.drawInstances = instances;
    a.lastWriterThread = GetCurrentThreadId();a.lastWriterContext = Identity(context);
}
void BoundConstantBuffers(ID3D11DeviceContext* context, BindingStage stage, UINT start, UINT count, ID3D11Buffer* const* buffers) noexcept {
    state.bindingCalls.fetch_add(1, std::memory_order_relaxed);
    TryLock guard(state);if (!guard) { state.droppedBusy.fetch_add(1, std::memory_order_relaxed);return; }
    Publication publish;
    if (!CanCollectBinding()) return;
    if (!ContextMatches(context)) {
        --state.session.bindingAttempts;state.bindingContextMismatch.fetch_add(1, std::memory_order_relaxed);return;
    }
    if (!buffers || !count || start >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
        --state.session.bindingAttempts;return;
    }
    const auto limit = (std::min)(count, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - start);
    for (UINT i = 0; i < limit && CanCollectBinding(); ++i) {
        --state.session.bindingAttempts;auto* buffer = buffers[i];if (!buffer) continue;
        bool duplicate = false;
        for (unsigned n = 0; n < state.session.bindingsUsed; ++n) {
            const auto& b = state.bindings[n];
            if (b.resource.Get() == buffer && b.stage == stage && b.slot == start + i && b.drawSerial == state.arm.boundSerial) { duplicate = true;break; }
        }
        if (duplicate) continue;
        D3D11_BUFFER_DESC desc{};if (!DescribeBuffer(buffer, desc, buffer) || !ConstantBuffer(desc)) continue;
        auto& b = state.bindings[state.session.bindingsUsed++];b.resource = buffer;b.descriptor = desc;
        b.stage = stage;b.slot = start + i;b.bytes = desc.ByteWidth;b.bindFlags = desc.BindFlags;b.usage = desc.Usage;
        b.thread = GetCurrentThreadId();b.context = Identity(context);b.drawSerial = state.arm.boundSerial;
        state.bindingAccepted.fetch_add(1, std::memory_order_relaxed);++state.arm.constantBufferBinds;
    }
}
void Updated(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub, const D3D11_BOX* box, const void* data, UINT flags) noexcept {
    TryLock guard(state);if (!guard) { state.droppedBusy.fetch_add(1, std::memory_order_relaxed);return; }
    Publication publish;
    if (!CanCollectPayload()) return;
    --state.session.payloadAttempts;++state.arm.updateCalls;
    if (!ContextMatches(context) || !resource || sub || box || !data || flags) return;
    D3D11_BUFFER_DESC desc{};
    if (!DescribeBuffer(resource, desc) || !ConstantBuffer(desc) || desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags || desc.MiscFlags) {
        ++state.arm.rejectedUpdates;return;
    }
    if (state.copiedBytesRemaining < state.session.reservedBytes || desc.ByteWidth > state.copiedBytesRemaining - state.session.reservedBytes) return;
    state.arm.lastWriterThread = GetCurrentThreadId();state.arm.lastWriterContext = Identity(context);
    RecordPayload(context, resource, sub, data, desc, PayloadOrigin::UpdateSubresource);
}
void Mapped(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* mapped, HRESULT result) noexcept {
    TryLock guard(state);if (!guard) {
        state.pendingRevoke.store(true, std::memory_order_release);
        state.droppedBusy.fetch_add(1, std::memory_order_relaxed);return;
    }
    Publication publish;
    // Conflicting Map cannot silently replace an outstanding pointer, even
    // when collection is exhausted/closed. No stale pointer survives that key.
    if (state.session.mappings)
        if (auto* old = FindMapping(resource, sub)) { ClearMapping(*old);++state.arm.rejectedUpdates;return; }
    if (!CanCollectPayload()) return;
    --state.session.payloadAttempts;++state.arm.mapCalls;
    if (!ContextMatches(context) || FAILED(result) || !resource || sub || !mapped || !mapped->pData ||
        type < D3D11_MAP_WRITE || type > D3D11_MAP_WRITE_NO_OVERWRITE) return;
    ++state.arm.mapWrites;
    // The fixed journal capacity is checked before any resource COM work.
    Mapping* destination = nullptr;
    for (auto& m : state.mappings) if (!m.active) { destination = &m;break; }
    if (!destination) { ++state.arm.rejectedUpdates;return; }
    D3D11_BUFFER_DESC desc{};
    if (!DescribeBuffer(resource, desc) || !ConstantBuffer(desc) || desc.Usage != D3D11_USAGE_DYNAMIC ||
        !(desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) || state.copiedBytesRemaining < state.session.reservedBytes ||
        desc.ByteWidth > state.copiedBytesRemaining - state.session.reservedBytes) return;
    auto& m = *destination;m.resource = resource;m.descriptor = desc;m.subresource = sub;
    m.data = mapped->pData;m.bytes = desc.ByteWidth;m.thread = GetCurrentThreadId();m.context = Identity(context);m.session = state.session.serial;m.active = true;
    ++state.session.mappings;++state.session.reservedCopies;state.session.reservedBytes += m.bytes;++state.arm.mappedConstantBuffers;
}
void BeforeUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub) noexcept {
    TryLock guard(state);if (!guard) {
        // Missing this return loses the validity of every retained CPU pointer.
        // The next try-lock must clear them before it can inspect another key.
        state.pendingRevoke.store(true, std::memory_order_release);state.droppedBusy.fetch_add(1, std::memory_order_relaxed);return;
    }
    Publication publish;
    auto* m = FindMapping(resource, sub);if (!m) return;
    ++state.arm.unmapCalls;
    const bool exact = Observing() && m->session == state.session.serial && context &&
        m->context == Identity(context) && m->thread == GetCurrentThreadId() && ContextMatches(context);
    // Release reservation before copying, including when attempts are exhausted.
    // Pending work has its own demand; no new descriptor/context COM query occurs.
    auto held = m->resource;const auto desc = m->descriptor;const void* data = m->data;
    ClearMapping(*m);
    if (exact && data && RecordPayload(context, held.Get(), sub, data, desc, PayloadOrigin::MapUnmap)) ++state.arm.mappedPayloads;
}
} // namespace detail

namespace {
void EmitPendingPayloadsLocked(const char* association) noexcept {
    try {
        for (auto& payload : state.payloads) {
            if (!payload.pending || !payload.resource || !state.payloadLogsRemaining) continue;
            payload.pending = false;--state.session.payloadsPending;
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

} // namespace

void OnPresent() noexcept {
    // A drain-only session needs Unmap/revoke, never another output/pool scan.
    if (!HasDemand(Demand::CloseSession) && !HasDemand(Demand::Output)) return;
    TryLock guard(state);
    if (!guard) {
        state.pendingRevoke.store(true, std::memory_order_release);
        state.droppedBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Publication publish;
    if (!state.arm.target) return;
    try {
        if (state.session.summaryPending && state.summaryLogsRemaining) {
            state.session.summaryPending = false;
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
        if (state.session.payloadsPending) EmitPendingPayloadsLocked("PRESENT");
        for (unsigned i = 0; i < state.session.bindingsUsed; ++i) {
            const auto& binding = state.bindings[i];
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
    // Close this explicit observation request. LOCATION keeps no collection
    // demand; pending mappings remain drain-only and can never read old data.
    ClearFrameKeepLocation();
}

} // namespace GenshinCpuWriterProbe
