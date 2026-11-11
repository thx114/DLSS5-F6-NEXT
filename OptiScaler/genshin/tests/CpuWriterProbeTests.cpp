// SPDX-License-Identifier: GPL-3.0-or-later
// Compile with cpu-writer-probe/stubs first on the include path and link the
// real GenshinCpuWriterProbe.cpp as a separate TU. No D3D runtime is linked.
#include "../GenshinCpuWriterProbe.h"
#include <SysUtils.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>

namespace Probe = GenshinCpuWriterProbe;
namespace {
unsigned checks = 0, groups = 0;
void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", why); std::abort(); }
}
unsigned Bits(Probe::Demand d) { return static_cast<unsigned>(d); }
unsigned Published() { return Probe::state.demand.load(std::memory_order_acquire); }

template<class Interface> struct Object : Interface {
    ULONG refs = 1;
    unsigned queries = 0, retains = 0, releases = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        ++queries;
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(Interface)) return E_NOINTERFACE;
        *out = static_cast<Interface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++retains; return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { ++releases; return --refs; }
};
struct Device final : Object<ID3D11Device> {};
struct Context final : Object<ID3D11DeviceContext> {
    ID3D11Device* device;
    D3D11_DEVICE_CONTEXT_TYPE type;
    unsigned typeReads = 0, deviceReads = 0;
    explicit Context(ID3D11Device* d, D3D11_DEVICE_CONTEXT_TYPE t = D3D11_DEVICE_CONTEXT_IMMEDIATE) : device(d), type(t) {}
    D3D11_DEVICE_CONTEXT_TYPE GetType() override { ++typeReads; return type; }
    void GetDevice(ID3D11Device** out) override { ++deviceReads; *out = device; if (device) device->AddRef(); }
};
struct Buffer final : Object<ID3D11Buffer> {
    ID3D11Device* device;
    D3D11_BUFFER_DESC desc{};
    unsigned deviceReads = 0, descReads = 0;
    explicit Buffer(ID3D11Device* d) : device(d) { desc.ByteWidth = 16; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; }
    void GetDevice(ID3D11Device** out) override { ++deviceReads; *out = device; if (device) device->AddRef(); }
    void GetDesc(D3D11_BUFFER_DESC* out) override { ++descReads; *out = desc; }
    void Dynamic() { desc.Usage = D3D11_USAGE_DYNAMIC; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; }
};
struct Texture final : Object<ID3D11Texture2D> {
    ID3D11Device* device;
    unsigned deviceReads = 0, descReads = 0;
    explicit Texture(ID3D11Device* d) : device(d) {}
    void GetDevice(ID3D11Device** out) override { ++deviceReads; *out = device; if (device) device->AddRef(); }
    void GetDesc(D3D11_TEXTURE2D_DESC* out) override { ++descReads; out->Format = DXGI_FORMAT_R32_FLOAT; }
};
template<class Interface, class Desc> struct View final : Object<Interface> {
    ID3D11Resource* resource;
    Desc desc{};
    unsigned resourceReads = 0, descReads = 0;
    explicit View(ID3D11Resource* r) : resource(r) {}
    void GetResource(ID3D11Resource** out) override { ++resourceReads; *out = resource; if (resource) resource->AddRef(); }
    void GetDesc(Desc* out) override { ++descReads; *out = desc; }
};
using Dsv = View<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>;
using Rtv = View<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>;

struct Quotas {
    unsigned windows = Probe::state.windowsRemaining, copies = Probe::state.payloadCopiesRemaining;
    uint64_t bytes = Probe::state.copiedBytesRemaining;
    unsigned summaries = Probe::state.summaryLogsRemaining, payloads = Probe::state.payloadLogsRemaining, bindings = Probe::state.bindingLogsRemaining;
    bool Same() const {
        return windows == Probe::state.windowsRemaining && copies == Probe::state.payloadCopiesRemaining &&
            bytes == Probe::state.copiedBytesRemaining && summaries == Probe::state.summaryLogsRemaining &&
            payloads == Probe::state.payloadLogsRemaining && bindings == Probe::state.bindingLogsRemaining;
    }
};
struct Fixture {
    Device device, otherDevice;
    Context context{&device}, otherContext{&device}, deferred{&device, D3D11_DEVICE_CONTEXT_DEFERRED};
    Texture target{&device}, otherTarget{&otherDevice};
    Buffer cb{&device}, dynamic{&device}, otherCb{&otherDevice};
    std::array<unsigned char, Probe::MaxPayloadBytes> data{};
    Fixture() {
        // Test isolation only: production never reconstructs process storage.
        Probe::Revoke(nullptr); Probe::state.~State(); new (&Probe::state) Probe::State{};
        ProbeTestLog::summaries = ProbeTestLog::payloads = ProbeTestLog::bindings = ProbeTestLog::expectations = 0;
        ProbeTestLog::fail = false; dynamic.Dynamic(); data.fill(0x5a);
    }
    ~Fixture() {
        ProbeTestLog::fail = false; Probe::Revoke(nullptr);
        Check(context.refs == 1 && otherContext.refs == 1 && target.refs == 1 && cb.refs == 1 && dynamic.refs == 1 &&
            device.refs == 1 && otherDevice.refs == 1 && otherCb.refs == 1, "all retained COM identities released");
    }
    void Arm(uint64_t frame = 1) { Probe::Arm(&context, &target, 0, 1, 2, 3, 4, 5, frame); }
    void Map(Buffer& buffer, void* pointer = nullptr) {
        D3D11_MAPPED_SUBRESOURCE mapped{pointer ? pointer : data.data(), 0, 0};
        Probe::Mapped(&context, &buffer, 0, D3D11_MAP_WRITE_DISCARD, &mapped, S_OK);
    }
    void Update() { Probe::Updated(&context, &cb, 0, nullptr, data.data()); }
};
void CheckClosed(bool maps = false) {
    Check(!Probe::HasCollectionDemand(), "closed interval has no collection demand");
    Check(Published() == (maps ? Bits(Probe::Demand::DrainMaps) : 0), "only exact map cleanup remains subscribed");
    Check(Probe::state.session.phase == (maps ? Probe::SessionPhase::Draining : Probe::SessionPhase::LocationOnly), "closed phase matches actual pending maps");
}
void IdleLocationAndPresent() {
    ++groups; Fixture f;
    Probe::Expectation(&f.context, &f.target, 0, 1, 2, 3, 4, 5, 1);
    Check(!Probe::HasPublishedWork() && !Probe::state.arm.target, "expectation cannot arm diagnostics");
    f.Arm();
    Check(Probe::HasCollectionDemand() && Probe::HasDemand(Probe::Demand::CloseSession), "explicit arm subscribes a bounded interval and its close");
    const auto windows = Probe::state.windowsRemaining; const auto serial = Probe::state.session.serial;
    f.Arm(); Check(Probe::state.windowsRemaining == windows && Probe::state.session.serial == serial, "duplicate live arm does not refill attempts or spend a window");
    Probe::OnPresent(); CheckClosed();
    Check(ProbeTestLog::summaries == 1 && Probe::state.windowActive && Probe::state.arm.target.Get() == &f.target, "one summary leaves a retained location");
    Check(!Probe::state.arm.instance && !Probe::state.arm.generation && !Probe::state.arm.frame, "closed location retires source frame identity");
    const Quotas quotas;
    const auto dropped = Probe::state.droppedBusy.load(), bindingCalls = Probe::state.bindingCalls.load();
    const auto reads = f.context.typeReads + f.context.deviceReads + f.cb.deviceReads + f.cb.descReads + f.cb.queries;
    Check(!Probe::state.busy.test_and_set(), "test holds busy for negative fast paths");
    ID3D11Buffer* buffers[] = {&f.cb};
    for (unsigned i = 0; i < 10000; ++i) {
        Probe::Bound(&f.context, 0, nullptr, nullptr);
        Probe::BeforeDraw(&f.context, 0, 3, 0, 0, 1, 0);
        Probe::BoundConstantBuffers(&f.context, Probe::BindingStage::Vertex, 0, 1, buffers);
        f.Update(); f.Map(f.dynamic); Probe::BeforeUnmap(&f.context, &f.dynamic, 0); Probe::OnPresent();
    }
    Probe::state.busy.clear();
    Check(quotas.Same() && ProbeTestLog::summaries == 1, "permanent location emits nothing and replenishes nothing");
    Check(Probe::state.droppedBusy == dropped && Probe::state.bindingCalls == bindingCalls && !Probe::state.pendingRevoke, "idle wrappers never attempt the busy lock or callback counters");
    Check(reads == f.context.typeReads + f.context.deviceReads + f.cb.deviceReads + f.cb.descReads + f.cb.queries, "idle hooks perform no COM work");
}
struct Inaccessible {
    void* data = VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    Inaccessible() { Check(data != nullptr, "allocate unreadable mapped-pointer sentinel"); }
    ~Inaccessible() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
};
void PresentWithoutOutput() {
    ++groups; Fixture f; Inaccessible unreadable;
    Probe::state.summaryLogsRemaining = 0; f.Arm();
    Check(!Probe::HasDemand(Probe::Demand::Output) && Probe::HasDemand(Probe::Demand::CloseSession), "empty no-summary interval still needs Present close");
    f.Map(f.dynamic, unreadable.data);
    const Quotas quotas; Probe::OnPresent(); CheckClosed(true);
    Check(Probe::state.session.mappings == 1 && Probe::state.session.reservedCopies == 1, "Present preserves only outstanding metadata reservations");
    for (unsigned i = 0; i < 1000; ++i) Probe::OnPresent();
    Probe::BeforeUnmap(&f.context, &f.dynamic, 0); CheckClosed();
    Check(quotas.Same() && !Probe::state.session.reservedBytes && !Probe::state.session.mappings, "draining never reads inaccessible data or spends/refunds copies");
    f.Arm(2); Probe::OnPresent(); CheckClosed();
}
void StopIsCleanupOnly() {
    ++groups; Fixture f; Inaccessible unreadable; f.Arm(); f.Update(); f.Map(f.dynamic, unreadable.data);
    const Quotas quotas;
    Probe::StopObservation(&f.otherContext); Check(Probe::HasCollectionDemand(), "another context cannot stop this interval");
    Probe::StopObservation(&f.context); CheckClosed(true);
    Check(!Probe::state.session.payloadsPending && ProbeTestLog::payloads == 0, "stop discards outputs without emitting or writing back");
    f.Update(); Probe::OnPresent();
    Probe::BeforeUnmap(&f.context, &f.dynamic, 0); CheckClosed();
    Check(quotas.Same(), "stop and exact cleanup cannot dereference the application pointer or refill quotas");
}
void ReservedUnmapCapacity() {
    ++groups; Fixture f;
    Probe::state.payloadCopiesRemaining = Probe::state.payloadLogsRemaining = 2; Probe::state.copiedBytesRemaining = 32;
    f.Arm(); f.Map(f.dynamic); f.Update();
    Check(!Probe::HasDemand(Probe::Demand::Payload) && Probe::HasPendingMapCleanup(), "last copy byte and log remain reserved for pending Unmap");
    f.Update();
    Probe::BeforeUnmap(&f.context, &f.dynamic, 0);
    Check(Probe::state.arm.mappedPayloads == 1 && Probe::state.session.payloadsPending == 2, "exact Unmap consumes its reserved capacity after new-input demand ends");
    Check(!Probe::state.payloadCopiesRemaining && !Probe::state.copiedBytesRemaining && !Probe::state.session.reservedCopies, "copy and byte quotas spent exactly once");
    Check(f.dynamic.descReads == 1 && f.dynamic.deviceReads == 1, "Unmap does not query descriptors or devices again");
    Probe::OnPresent(); CheckClosed();
    Check(ProbeTestLog::payloads == 2 && !Probe::state.payloadLogsRemaining, "pending payload logs are bounded independently");
}
void AttemptsAndReservations() {
    ++groups; Fixture f; f.Arm(); f.Map(f.dynamic);
    for (unsigned i = 0; i < 1000; ++i) Probe::Updated(&f.context, &f.cb, 1, nullptr, f.data.data());
    Check(!Probe::state.session.payloadAttempts && !Probe::HasDemand(Probe::Demand::Payload), "invalid updates exhaust attempts and unsubscribe payload hooks");
    Check(f.cb.descReads == 0 && f.cb.deviceReads == 0, "structural rejection precedes descriptor COM work");
    const auto windows = Probe::state.windowsRemaining; f.Arm();
    Check(!Probe::state.session.payloadAttempts && Probe::state.windowsRemaining == windows, "same live request cannot refill attempts");
    Probe::BeforeUnmap(&f.context, &f.dynamic, 0);
    Check(Probe::state.arm.mappedPayloads == 1, "attempt exhaustion cannot starve already registered exact Unmap");
    for (unsigned i = 0; i < 1000; ++i) {
        Probe::Bound(&f.otherContext, 0, nullptr, nullptr);
        Probe::BoundConstantBuffers(&f.context, Probe::BindingStage::Vertex, 0, 0, nullptr);
    }
    Check(!Probe::HasCollectionDemand() && !Probe::state.session.writerEvents && !Probe::state.session.bindingAttempts, "writer and rejected binding attempts also saturate");
    Probe::OnPresent(); CheckClosed();
}
void ExactOwnerAndInvalidation() {
    ++groups; Fixture f; Inaccessible unreadable; f.Arm();
    std::thread wrong([&] { f.Update(); f.Map(f.dynamic, unreadable.data); }); wrong.join();
    Check(Probe::state.payloadCopiesRemaining == Probe::MaxPayloadCopies && !Probe::state.session.mappings, "foreign thread cannot collect payload or register a pointer");
    f.Map(f.dynamic, unreadable.data);
    Probe::BeforeUnmap(&f.context, &f.dynamic, 1);
    Check(Probe::state.session.mappings == 1, "different subresource cannot consume the exact registration");
    Probe::BeforeUnmap(&f.otherContext, &f.dynamic, 0);
    Check(!Probe::state.session.mappings && Probe::state.payloadCopiesRemaining == Probe::MaxPayloadCopies, "wrong context invalidates without reading");
    f.Map(f.dynamic, unreadable.data);
    std::thread wrongUnmap([&] { Probe::BeforeUnmap(&f.context, &f.dynamic, 0); }); wrongUnmap.join();
    Check(!Probe::state.session.mappings && Probe::state.payloadCopiesRemaining == Probe::MaxPayloadCopies, "wrong-thread Unmap clears without reading");
    f.Map(f.dynamic, unreadable.data); f.Map(f.dynamic, unreadable.data);
    Check(!Probe::state.session.mappings && !Probe::state.session.reservedCopies, "conflicting Map discards its old pointer rather than replacing it");
    f.Map(f.dynamic, unreadable.data); Probe::OnPresent();
    f.Map(f.dynamic, unreadable.data); CheckClosed();
    Check(!Probe::state.session.mappings, "drain-only Map can invalidate but cannot register a new pointer");
}
void RevokeConfigAndNewSession() {
    ++groups; Fixture f; Inaccessible unreadable; f.Arm(); f.Map(f.dynamic, unreadable.data);
    const auto serial = Probe::state.session.serial; const auto copies = Probe::state.payloadCopiesRemaining;
    f.Arm(2);
    Check(Probe::state.session.serial != serial && !Probe::state.session.mappings, "new request cannot inherit old session pointers");
    Probe::BeforeUnmap(&f.context, &f.dynamic, 0);
    Check(Probe::state.payloadCopiesRemaining == copies, "stale session Unmap cannot copy");
    f.Map(f.dynamic, unreadable.data); const Quotas quotas;
    Probe::Revoke(&f.otherContext); Check(Probe::state.session.mappings == 1, "unrelated revoke does not take this owner");
    Probe::ObserveConfig(901); Probe::ObserveConfig(901);
    Check(Probe::state.session.mappings == 1, "unchanged config epoch preserves the current interval");
    Probe::ObserveConfig(902);
    Check(!Probe::HasPublishedWork() && !Probe::state.windowActive && !Probe::state.arm.target && quotas.Same(), "config change revokes pointers and location without reading or quota reset");
}
void TryLockFailClosed() {
    ++groups; Fixture f; Inaccessible unreadable;
    for (unsigned action = 0; action < 5; ++action) {
        f.Arm(action + 1); f.Map(f.dynamic, unreadable.data); const Quotas quotas;
        Check(!Probe::state.busy.test_and_set(), "hold busy across a nonblocking callback");
        switch (action) {
        case 0: Probe::BeforeUnmap(&f.context, &f.dynamic, 0); break;
        case 1: f.Map(f.dynamic, unreadable.data); break;
        case 2: Probe::Revoke(&f.context); break;
        case 3: Probe::StopObservation(&f.context); break;
        default: Probe::OnPresent(); break;
        }
        Check(Probe::state.pendingRevoke && !Probe::HasCollectionDemand(), "missed pointer boundary requests fail-closed cleanup immediately");
        Probe::state.busy.clear();
        Probe::BeforeDraw(&f.context, 0, 3, 0, 0, 1, 0);
        Check(!Probe::HasPublishedWork() && !Probe::state.session.mappings && !Probe::state.arm.target, "next successful try-lock revokes before any callback work");
        Check(quotas.Same(), "contention cleanup neither copies data nor refunds capacity");
    }
}
void QualificationAndDescriptors() {
    ++groups; Fixture f; const Quotas quotas;
    Probe::Arm(&f.context, &f.target, 0, 1, 2, 3, 4, 5, 0);
    Probe::Arm(&f.deferred, &f.target, 0, 1, 2, 3, 4, 5, 1);
    Probe::Arm(&f.context, &f.otherTarget, 0, 1, 2, 3, 4, 5, 1);
    Check(!Probe::HasPublishedWork() && quotas.Same(), "missing identity deferred context and foreign device cannot arm");
    f.Arm();
    Probe::Updated(&f.otherContext, &f.cb, 0, nullptr, f.data.data());
    Probe::Updated(&f.context, &f.otherCb, 0, nullptr, f.data.data());
    f.cb.desc.BindFlags = 0; f.Update(); f.cb.desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    f.cb.desc.ByteWidth = Probe::MaxPayloadBytes + 16; f.Update(); f.cb.desc.ByteWidth = 17; f.Update(); f.cb.desc.ByteWidth = 16;
    f.cb.desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; f.Update(); f.cb.desc.CPUAccessFlags = 0;
    f.cb.desc.MiscFlags = 1; f.Update(); f.cb.desc.MiscFlags = 0;
    Check(Probe::state.payloadCopiesRemaining == Probe::MaxPayloadCopies, "all rejected descriptors and identities leave copy quota intact");
    f.Update();
    Check(Probe::state.payloads[0].bytes == 16 && std::memcmp(Probe::state.payloads[0].data.data(), f.data.data(), 16) == 0, "qualified update records exact CPU bytes");
    const auto reads = f.cb.descReads; f.Update();
    Check(f.cb.descReads == reads, "only a retained immutable resource descriptor is reused");
}
void WriterAndBindingBounds() {
    ++groups; Fixture f; f.Arm(); Dsv dsv{&f.target}; Rtv rtv{&f.target};
    dsv.desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D; dsv.desc.Texture2D.MipSlice = 1;
    Probe::Bound(&f.context, 0, nullptr, &dsv);
    Check(!Probe::state.arm.targetBound, "writer requires exact target mip");
    dsv.desc.Texture2D.MipSlice = 0; Probe::Bound(&f.context, 0, nullptr, &dsv);
    Probe::BeforeDraw(&f.context, 2, 11, 0, 0, 3, 0);
    Check(Probe::state.arm.drawCalls == 1 && Probe::state.arm.drawCount == 11 && Probe::state.arm.drawInstances == 3, "matching bound writer records diagnostic metadata");
    rtv.desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; ID3D11RenderTargetView* rtvs[] = {&rtv};
    Probe::Bound(&f.context, 1, rtvs, nullptr);
    Check(Probe::state.arm.targetBound && Probe::state.arm.rtvSlot == 0, "RTV path uses same exact location contract");
    ID3D11Buffer* buffers[] = {&f.cb, &f.cb};
    Probe::BoundConstantBuffers(&f.context, Probe::BindingStage::Pixel, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1, 2, buffers);
    Check(Probe::state.session.bindingsUsed == 1, "CB range is clipped to remaining API slots");
    for (unsigned i = 0; i < 1000; ++i) Probe::BoundConstantBuffers(&f.context, Probe::BindingStage::Pixel, 13, 1, buffers);
    Check(Probe::state.session.bindingsUsed == 1 && !Probe::HasDemand(Probe::Demand::Binding) && f.cb.descReads == 1, "duplicate bindings spend attempts and stop without repeated COM work");
    Probe::OnPresent(); CheckClosed(); Check(ProbeTestLog::bindings == 1, "unique binding emits once");
}
void LifetimeQuotasAndLogFailure() {
    ++groups; Fixture f;
    for (unsigned i = 0; i < Probe::MaxWindows; ++i) {
        const auto before = Probe::state.windowsRemaining; f.Arm(i + 1); f.Update(); Probe::OnPresent();
        Check(Probe::state.windowsRemaining + 1 == before, "each explicit interval spends a lifetime window");
    }
    const Quotas spent; f.Arm(1000); Probe::Revoke(nullptr); Probe::StopObservation(nullptr);
    Check(spent.Same() && !Probe::state.windowsRemaining && !Probe::HasPublishedWork(), "exhausted lifetime windows never refill through arm stop or revoke");
    Check(ProbeTestLog::payloads == Probe::MaxPayloadCopies && ProbeTestLog::summaries == Probe::MaxSummaryLogs, "actual process log and copy ceilings reached exactly");
}
void ByteLogAndSerialLimits() {
    ++groups; Fixture f; f.cb.desc.ByteWidth = Probe::MaxPayloadBytes; f.Arm();
    for (unsigned i = 0; i < 16; ++i) f.Update();
    Check(!Probe::state.copiedBytesRemaining && Probe::state.payloadCopiesRemaining == Probe::MaxPayloadCopies - 8 && !Probe::HasDemand(Probe::Demand::Payload), "byte quota closes payload demand before copy quota");
    ProbeTestLog::fail = true; Probe::OnPresent(); ProbeTestLog::fail = false; CheckClosed();
    const Quotas quotas; Probe::OnPresent(); Check(quotas.Same(), "throwing logger cannot retain collection or retry spent quotas");
    Probe::state.nextSession = UINT64_MAX; f.Arm(2); Check(!Probe::HasPublishedWork(), "session serial cannot wrap into an old pointer identity");
}
} // namespace
int main() {
    IdleLocationAndPresent(); PresentWithoutOutput(); StopIsCleanupOnly(); ReservedUnmapCapacity();
    AttemptsAndReservations(); ExactOwnerAndInvalidation(); RevokeConfigAndNewSession(); TryLockFailClosed();
    QualificationAndDescriptors(); WriterAndBindingBounds(); LifetimeQuotasAndLogFailure(); ByteLogAndSerialLimits();
    std::printf("CPU writer probe: %u groups, %u checks passed (CPU fakes; no GPU/game proof)\n", groups, checks);
}
