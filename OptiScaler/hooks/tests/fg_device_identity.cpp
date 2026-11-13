// test_fg_device_identity.py inserts real FG_Hooks.cpp definitions here.
// Only D3D12 interfaces are CPU doubles. Windows COM, WRL and events are real.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <runtime/D3D12DeviceIdentity.h>
#include <hooks/FGQueueIdleGate.h>
#include <framegen/FGHookRetirement.h>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
unsigned checks = 0, scenarios = 0;
const char* currentScenario = "startup";
bool referenceUnderflow = false;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Test> void Scenario(const char* name, Test test)
{
    currentScenario = name;
    ++scenarios;
    const auto before = checks;
    test();
    Check(!referenceUnderflow, "COM Release never underflows");
    std::printf("PASS %s (%u checks)\n", name, checks - before);
}
template<class... Args> void DiscardLog(const Args&...) {}
#define LOG_ERROR(...) DiscardLog(__VA_ARGS__)

// Independent golden GUIDs keep an accidental product-contract typo observable.
constexpr GUID KnownReShade =
    {0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
constexpr GUID KnownStreamline =
    {0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};

HRESULT ReturnInterface(IUnknown* value, HRESULT hr, void** output)
{
    if (!output) return E_POINTER;
    *output = value;
    if (value) value->AddRef();
    return hr;
}

// Stack ownership is separate from COM reference accounting. Every QI success
// AddRefs the returned interface; Release balances it and never deletes a stack object.
template<class Interface> struct ComFake : Interface
{
    ULONG refs = 1;
    unsigned queries = 0, addRefs = 0, releases = 0;
    HRESULT unknownHr = S_OK;
    bool unknownOutput = true;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        ++queries;
        if (!output) return E_POINTER;
        *output = nullptr;
        if (IsEqualIID(iid, __uuidof(IUnknown)))
            return ReturnInterface(unknownOutput ? static_cast<Interface*>(this) : nullptr,
                                   unknownHr, output);
        if (IsEqualIID(iid, __uuidof(Interface)))
            return ReturnInterface(static_cast<Interface*>(this), S_OK, output);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++addRefs; return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ++releases;
        if (!refs) { referenceUnderflow = true; return 0; }
        return --refs;
    }
};
using FakeUnknown = ComFake<IUnknown>;

struct FakeDevice final : ComFake<ID3D12Device>
{
    enum class Proxy { Raw, ReShade, Streamline, Unknown };
    Proxy proxy = Proxy::Raw;
    IUnknown* native = nullptr; // Borrowed graph edges; cycle tests must not own themselves.
    ID3D12Fence* createdFence = nullptr;
    HRESULT nativeHr = S_OK, deviceQiHr = S_OK, removed = S_OK, createHr = S_OK;
    bool nativeOutput = true, deviceQiOutput = true;
    unsigned nativeQueries = 0, removedQueries = 0, luidQueries = 0, creates = 0;
    LUID luid {0x12345678, 0}; // Intentionally identical on every distinct actual device.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (IsEqualIID(iid, __uuidof(ID3D12Device)))
        {
            ++queries;
            return ReturnInterface(deviceQiOutput ? this : nullptr, deviceQiHr, output);
        }
        if ((proxy == Proxy::ReShade && IsEqualIID(iid, KnownReShade)) ||
            (proxy == Proxy::Streamline && IsEqualIID(iid, KnownStreamline)))
        {
            ++queries; ++nativeQueries;
            return ReturnInterface(nativeOutput ? native : nullptr, nativeHr, output);
        }
        return ComFake::QueryInterface(iid, output);
    }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial, D3D12_FENCE_FLAGS flags,
                                         REFIID iid, void** output) override
    {
        ++creates;
        if (!output) return E_POINTER;
        *output = nullptr;
        if (initial != 0 || flags != D3D12_FENCE_FLAG_NONE) return E_INVALIDARG;
        if (createHr != S_OK) return createHr;
        return createdFence ? createdFence->QueryInterface(iid, output) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { ++removedQueries; return removed; }
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { ++luidQueries; return luid; }
};

struct FakeFence final : ComFake<ID3D12Fence>
{
    ID3D12Device* device;
    UINT64 completed = 0, armedValue = 0;
    HRESULT getHr = S_OK, armHr = S_OK;
    bool getOutput = true, wakeWithoutCompletion = false;
    bool finishOnArm = false, removeAfterRead = false;
    FakeDevice* removeDevice = nullptr;
    unsigned reads = 0, arms = 0;
    explicit FakeFence(ID3D12Device* value) : device(value) {}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (getHr != S_OK || !getOutput) return getHr;
        return device ? device->QueryInterface(iid, output) : E_UNEXPECTED;
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override
    {
        ++reads;
        if (removeAfterRead && removeDevice) removeDevice->removed = DXGI_ERROR_DEVICE_REMOVED;
        return completed;
    }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override
    {
        ++arms;
        armedValue = value;
        if (armHr != S_OK) return armHr;
        if (finishOnArm) completed = value;
        if (wakeWithoutCompletion || completed >= value) SetEvent(event);
        return S_OK;
    }
};

struct FakeQueue final : ComFake<ID3D12CommandQueue>
{
    ID3D12Device* device;
    ID3D12Fence* signaledFence = nullptr;
    UINT64 signaledValue = 0;
    HRESULT getHr = S_OK, signalHr = S_OK;
    bool getOutput = true;
    unsigned signals = 0;
    explicit FakeQueue(ID3D12Device* value) : device(value) {}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (getHr != S_OK || !getOutput) return getHr;
        return device ? device->QueryInterface(iid, output) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override
    {
        ++signals;
        signaledFence = fence;
        signaledValue = value;
        return signalHr; // Deliberately never changes the fence's completed value.
    }
};

class IFGFeature_Dx12;
thread_local bool fgLifecycleCallback = false;
// INSERT_PRODUCT_CALLBACK
// INSERT_PRODUCT_OWNER
// INSERT_PRODUCT_STRICT
// INSERT_PRODUCT_IO
// INSERT_PRODUCT_COMPLETE

struct Rig
{
    FakeDevice raw, wrapper;
    FakeFence fence {&raw};
    FakeQueue queue;
    FakeUnknown pin;
    FGSwapchainOwner owner;
    ID3D12Device* bound;
    explicit Rig(FakeDevice::Proxy kind = FakeDevice::Proxy::Raw)
        : queue(kind == FakeDevice::Proxy::Raw ? &raw : &wrapper), bound(queue.device)
    {
        wrapper.proxy = kind;
        wrapper.native = &raw;
        wrapper.createdFence = raw.createdFence = &fence;
        Check(owner.Bind(&queue) == S_OK, "real FGSwapchainOwner::Bind succeeds");
        pin.AddRef();
        // Pin is an opaque token in these three adapters; never invoke DXGI methods.
        owner.pin = reinterpret_cast<IDXGISwapChain*>(static_cast<IUnknown*>(&pin));
        Retained();
    }
    ~Rig()
    {
        // Fixture-only cleanup after assertions; no retirement/release is fabricated.
        owner.pin = nullptr;
        pin.Release();
    }
    void Retained()
    {
        Check(owner.queue.Get() == &queue && owner.device.Get() == bound && owner.fence.Get() == &fence,
              "original queue/device/fence are retained, never rebound to canonical device");
        Check(owner.pin == reinterpret_cast<IDXGISwapChain*>(static_cast<IUnknown*>(&pin)),
              "original FG pin retained");
        Check(queue.refs == 2 && fence.refs == 2 && static_cast<FakeDevice*>(bound)->refs == 2,
              "owner holds original COM references");
        Check((bound == &raw ? wrapper.refs : raw.refs) == 1, "temporary native identity references balanced");
        Check(pin.refs == 2 && pin.releases == 0, "no pin Release before completion or refusal");
    }
    void Expect(FGHookRetirementState state, HRESULT hr, bool terminal = false)
    {
        const auto result = CompleteFGQueue(&owner, terminal);
        Check(result.state == state && result.hr == hr, "real CompleteFGQueue result and HRESULT");
        Check(owner.quarantined == (state == FGHookRetirementState::Quarantined), "quarantine matches outcome");
        Check(!fgLifecycleCallback, "real callback guard restores thread state");
        Retained();
    }
    void Reject(HRESULT hr = E_INVALIDARG)
    {
        const auto before = queue.signals;
        Expect(FGHookRetirementState::Quarantined, hr);
        Check(queue.signals == before && fence.reads == 0, "identity refusal cannot submit or read completion");
    }
};

void CompletedWrapper(FakeDevice::Proxy kind)
{
    Rig r(kind);
    Check(!SameComObject(&r.wrapper, &r.raw), "strict IUnknown identities really differ");
    Check(D3D12DeviceIdentity::Same(&r.wrapper, &r.raw), "known QI contract proves native device");
    Check(D3D12DeviceIdentity::Same(&r.raw, &r.wrapper), "native identity is symmetric");
    Check(FGQueueIo {r.owner}.DeviceStatus() == S_OK, "real adapter accepts wrapped queue/raw fence");
    r.fence.completed = 1;
    r.Expect(FGHookRetirementState::Completed, S_OK);
    Check(r.queue.signals == 1 && r.queue.signaledFence == &r.fence && r.queue.signaledValue == 1,
          "completion belongs to original queue/fence/target");
    Check(r.owner.value == 1 && r.owner.queueTarget == 0, "completed target cleared without rebinding");
    Check(r.wrapper.nativeQueries != 0, "known unwrap QI actually executed");
}

void TestIdentityAndCompletion()
{
    using Proxy = FakeDevice::Proxy;
    using Result = FGHookRetirementState;
    Scenario("golden interface IDs and unwrap contracts", [] {
        Check(IsEqualGUID(KnownReShade, D3D12DeviceIdentity::ReShadeNativeObject), "ReShade contract GUID");
        Check(IsEqualGUID(KnownStreamline, D3D12DeviceIdentity::StreamlineNativeObject), "Streamline contract GUID");
        Check(D3D12DeviceIdentity::MaxProxyDepth == 8, "documented bounded depth");
        FakeUnknown object;
        ComPtr<IUnknown> a, b;
        Check(object.QueryInterface(IID_PPV_ARGS(&a)) == S_OK && object.refs == 2, "real IID_PPV_ARGS QI AddRef");
        Check(a.As(&b) == S_OK && a.Get() == b.Get() && object.refs == 3, "real WRL As and COM identity");
        b.Reset(); a.Reset();
        Check(object.refs == 1, "real WRL balances COM references");
    });
    Scenario("raw device completes through real adapters", [] {
        Rig r;
        Check(SameComObject(&r.raw, &r.raw), "raw strict identity equal");
        Check(D3D12DeviceIdentity::Same(&r.raw, &r.raw), "raw native identity equal");
        r.fence.completed = 1;
        r.Expect(Result::Completed, S_OK);
        Check(r.queue.signals == 1 && r.owner.queueTarget == 0, "raw queue target completed");
    });
    Scenario("known ReShade queue wraps device but returns raw fence", [] { CompletedWrapper(Proxy::ReShade); });
    Scenario("known Streamline queue wraps device but returns raw fence", [] { CompletedWrapper(Proxy::Streamline); });
    Scenario("nested Streamline and ReShade wrappers", [] {
        Rig r(Proxy::ReShade);
        FakeDevice inner;
        inner.proxy = Proxy::Streamline; inner.native = &r.raw;
        r.wrapper.native = &inner;
        Check(D3D12DeviceIdentity::Same(&r.raw, &r.wrapper), "nested native identity symmetric");
        r.fence.completed = 1;
        r.Expect(Result::Completed, S_OK);
        Check(inner.refs == 1 && inner.nativeQueries != 0, "inner references balanced after real unwrap");
    });
    Scenario("two distinct known wrappers share native device", [] {
        Rig r(Proxy::ReShade);
        FakeDevice second;
        second.proxy = Proxy::Streamline; second.native = &r.raw;
        r.queue.device = &second;
        Check(!SameComObject(&second, &r.wrapper), "two wrappers have distinct strict identities");
        r.fence.completed = 1;
        r.Expect(Result::Completed, S_OK);
        Check(second.refs == 1, "temporary queue device reference released");
    });
    Scenario("same LUID distinct actual devices rejected", [] {
        Rig r;
        FakeDevice other;
        const auto a = other.GetAdapterLuid(), b = r.raw.GetAdapterLuid();
        Check(a.LowPart == b.LowPart && a.HighPart == b.HighPart, "distinct actual devices intentionally share LUID");
        r.fence.device = &other;
        const auto lookups = r.raw.luidQueries + other.luidQueries;
        Check(!D3D12DeviceIdentity::Same(&r.raw, &other), "same adapter is not same device");
        r.Reject();
        Check(r.raw.luidQueries + other.luidQueries == lookups, "identity never consults adapter LUID");
    });
    Scenario("known unwrap to different same-LUID device rejected", [] {
        Rig r(Proxy::ReShade);
        FakeDevice other;
        r.wrapper.native = &other;
        Check(!D3D12DeviceIdentity::Same(&r.wrapper, &r.raw), "known contract does not bridge actual devices");
        r.Reject();
        Check(other.refs == 1, "different native references balanced");
    });
    Scenario("unknown wrapper rejected", [] {
        Rig r(Proxy::Unknown);
        Check(!D3D12DeviceIdentity::Same(&r.wrapper, &r.raw), "unknown wrapper lacks proof");
        r.Reject();
    });
    Scenario("self cycle rejected with bounded queries", [] {
        Rig r(Proxy::ReShade); r.wrapper.native = &r.wrapper;
        r.Reject();
        Check(r.wrapper.nativeQueries <= 2, "self cycle exits immediately");
    });
    Scenario("two-wrapper cycle rejected with balanced refs", [] {
        Rig r(Proxy::ReShade);
        FakeDevice second; second.proxy = Proxy::Streamline;
        r.wrapper.native = &second; second.native = &r.wrapper;
        r.Reject();
        Check(second.refs == 1 && second.nativeQueries <= 2 && r.wrapper.nativeQueries <= 2,
              "cycle detection is bounded and releases intermediates");
    });
    for (const auto depth : {D3D12DeviceIdentity::MaxProxyDepth, D3D12DeviceIdentity::MaxProxyDepth + 1})
    {
        Scenario(depth == D3D12DeviceIdentity::MaxProxyDepth ? "maximum known proxy depth accepted" :
                 "over-depth chain rejected", [depth] {
            Rig r(Proxy::ReShade);
            std::vector<std::unique_ptr<FakeDevice>> chain;
            IUnknown* next = &r.raw;
            for (size_t i = 1; i < depth; ++i)
            {
                auto node = std::make_unique<FakeDevice>();
                node->proxy = (i % 2) ? Proxy::Streamline : Proxy::ReShade;
                node->native = next; next = node.get(); chain.push_back(std::move(node));
            }
            r.wrapper.native = next;
            if (depth == D3D12DeviceIdentity::MaxProxyDepth)
            { r.fence.completed = 1; r.Expect(Result::Completed, S_OK); }
            else r.Reject();
            for (const auto& node : chain)
                Check(node->refs == 1 && node->nativeQueries <= 2, "depth traversal bounded with balanced refs");
        });
    }
    Scenario("null identity inputs rejected", [] {
        FakeDevice raw;
        Check(!D3D12DeviceIdentity::Same(nullptr, &raw), "null left");
        Check(!D3D12DeviceIdentity::Same(&raw, nullptr), "null right");
        Check(!D3D12DeviceIdentity::Same(nullptr, nullptr), "null is not an identity");
        Check(!D3D12DeviceIdentity::NativeIdentity(nullptr), "null native identity");
    });
    for (const auto hr : {S_OK, S_FALSE, E_FAIL, E_NOINTERFACE})
    {
        for (const bool output : {false, true})
        {
            if (hr == S_OK && output) continue;
            Scenario("malformed known unwrap HRESULT/output rejected", [hr, output] {
                Rig r(Proxy::ReShade);
                r.wrapper.nativeHr = hr; r.wrapper.nativeOutput = output;
                r.Reject();
                Check(r.raw.refs == 1, "failed native QI outputs do not leak references");
            });
        }
    }
    Scenario("known unwrap returns wrong COM interface type", [] {
        Rig r(Proxy::ReShade); FakeUnknown notDevice;
        r.wrapper.native = &notDevice;
        Check(!D3D12DeviceIdentity::NativeIdentity(&r.wrapper), "IUnknown without ID3D12Device cannot terminate chain");
        r.Reject();
        Check(notDevice.refs == 1, "wrong interface output released");
    });
    for (const auto hr : {S_OK, S_FALSE, E_FAIL})
    {
        Scenario("malformed IUnknown QI rejected", [hr] {
            Rig r(Proxy::ReShade);
            r.wrapper.unknownHr = hr; r.wrapper.unknownOutput = hr != S_OK;
            r.Reject();
        });
    }
    Scenario("terminal native device QI wrong HRESULT rejected", [] {
        Rig r(Proxy::ReShade);
        FakeDevice terminal; terminal.deviceQiHr = S_FALSE;
        r.wrapper.native = &terminal;
        Check(!D3D12DeviceIdentity::NativeIdentity(&r.wrapper), "S_FALSE is not terminal ID3D12Device proof");
        r.Reject();
        Check(terminal.refs == 1, "failed terminal device QI output released");
    });
    Scenario("terminal native device QI null output rejected", [] {
        Rig r(Proxy::ReShade);
        FakeDevice terminal; terminal.deviceQiOutput = false;
        r.wrapper.native = &terminal;
        Check(!D3D12DeviceIdentity::NativeIdentity(&r.wrapper), "null is not terminal ID3D12Device proof");
        r.Reject();
        Check(terminal.refs == 1, "null terminal device QI does not retain references");
    });
    Scenario("delayed real fence remains pending without Release", [] {
        Rig r(Proxy::ReShade);
        r.Expect(Result::Pending, DXGI_ERROR_WAS_STILL_DRAWING);
        const auto target = r.owner.queueTarget;
        Check(target == 1 && r.fence.completed == 0 && r.queue.signals == 1, "identity is not GPU completion");
        r.Expect(Result::Pending, DXGI_ERROR_WAS_STILL_DRAWING);
        Check(r.owner.queueTarget == target && r.owner.value == 1 && r.queue.signals == 1,
              "poll preserves original target without resignal");
        FakeFence unrelated(&r.raw); unrelated.completed = 999;
        Check(unrelated.GetCompletedValue() > target, "unrelated fence is already complete");
        r.Expect(Result::Pending, DXGI_ERROR_WAS_STILL_DRAWING);
        Check(r.queue.signaledFence == &r.fence, "only actual bound fence was signaled");
        r.fence.completed = target;
        r.Expect(Result::Completed, S_OK);
        Check(r.queue.signals == 1 && r.owner.queueTarget == 0, "actual completion resumes original target");
        r.Expect(Result::Pending, DXGI_ERROR_WAS_STILL_DRAWING);
        Check(r.owner.value == 2 && r.owner.queueTarget == 2 && r.queue.signals == 2,
              "old completed value cannot prove next queue epoch");
    });
    Scenario("Signal failure quarantines and preserves target", [] {
        Rig r(Proxy::ReShade); r.queue.signalHr = E_FAIL;
        r.Expect(Result::Quarantined, E_FAIL);
        Check(r.owner.queueTarget == 1 && r.owner.value == 1 && r.queue.signals == 1, "failed Signal retains exact epoch");
        r.queue.signalHr = S_OK; r.fence.completed = 1;
        r.Expect(Result::Quarantined, DXGI_ERROR_WAS_STILL_DRAWING);
        Check(r.queue.signals == 1 && r.fence.reads == 0, "quarantine cannot be retried into success");
    });
    Scenario("non-S_OK successful Signal HRESULT still quarantines", [] {
        Rig r(Proxy::ReShade); r.queue.signalHr = S_FALSE;
        r.Expect(Result::Quarantined, E_UNEXPECTED);
        Check(r.owner.queueTarget == 1 && r.queue.signals == 1, "ambiguous Signal does not clear target");
    });
    Scenario("removed owner device rejected before signal", [] {
        Rig r(Proxy::ReShade); r.wrapper.removed = DXGI_ERROR_DEVICE_REMOVED;
        r.Reject(DXGI_ERROR_DEVICE_REMOVED);
    });
    Scenario("removed fence sentinel rejected", [] {
        Rig r(Proxy::ReShade); r.fence.completed = UINT64_MAX;
        r.Expect(Result::Quarantined, DXGI_ERROR_DEVICE_REMOVED);
        Check(r.owner.queueTarget == 1, "removed sentinel is not fence completion");
    });
    Scenario("device removed after completed read rejected", [] {
        Rig r(Proxy::ReShade); r.fence.completed = 1;
        r.fence.removeAfterRead = true; r.fence.removeDevice = &r.wrapper;
        r.Expect(Result::Quarantined, DXGI_ERROR_DEVICE_REMOVED);
        Check(r.owner.queueTarget == 1 && r.queue.signals == 1, "post-completion device check retains epoch");
    });
    Scenario("spurious real event wake cannot complete fence", [] {
        Rig r(Proxy::ReShade); r.fence.wakeWithoutCompletion = true;
        r.Expect(Result::Quarantined, DXGI_ERROR_WAS_STILL_DRAWING, true);
        Check(r.fence.arms == 1 && r.fence.armedValue == 1 && r.fence.completed == 0,
              "actual Windows event wake still requires actual fence value");
        Check(r.owner.queueTarget == 1, "spurious wake retains original target");
    });
    Scenario("terminal real event and actual completed fence accepted", [] {
        Rig r(Proxy::ReShade); r.fence.finishOnArm = true;
        r.Expect(Result::Completed, S_OK, true);
        Check(r.fence.arms == 1 && r.fence.completed == 1 && r.owner.queueTarget == 0,
              "terminal completion rereads exact bound fence after wake");
    });
    Scenario("event arm failure retains owner and target", [] {
        Rig r(Proxy::ReShade); r.fence.armHr = E_FAIL;
        r.Expect(Result::Quarantined, E_FAIL, true);
        Check(r.owner.queueTarget == 1 && r.fence.arms == 1, "failed event arm cannot grant completion");
    });
    for (const bool queueFailure : {false, true})
    {
        for (const auto hr : {S_FALSE, E_FAIL})
        {
            Scenario("GetDevice non-S_OK rejected before signal", [queueFailure, hr] {
                Rig r(Proxy::ReShade);
                if (queueFailure) r.queue.getHr = hr; else r.fence.getHr = hr;
                r.Reject(FAILED(hr) ? hr : E_UNEXPECTED);
            });
        }
        Scenario("GetDevice S_OK null output rejected", [queueFailure] {
            Rig r(Proxy::ReShade);
            if (queueFailure) r.queue.getOutput = false; else r.fence.getOutput = false;
            r.Reject();
        });
    }
    Scenario("queue changes actual device rejected without rebinding", [] {
        Rig r(Proxy::ReShade); FakeDevice other;
        r.queue.device = &other;
        r.Reject();
        Check(other.refs == 1, "different queue device temporary reference balanced");
    });
    Scenario("exhausted counter refuses new target", [] {
        Rig r(Proxy::ReShade); r.owner.value = UINT64_MAX - 1;
        r.Expect(Result::Quarantined, E_UNEXPECTED);
        Check(r.queue.signals == 0 && r.owner.queueTarget == 0, "no wraparound Signal");
    });
    Scenario("null and unbound owner are refused", [] {
        const auto empty = CompleteFGQueue(nullptr, false);
        Check(empty.state == Result::Refused && empty.hr == E_INVALIDARG, "null owner refuses");
        FGSwapchainOwner owner;
        const auto result = CompleteFGQueue(&owner, false);
        Check(result.state == Result::Refused && result.hr == E_UNEXPECTED && !owner.quarantined,
              "unbound owner refuses without inventing identity");
        Check(owner.Bind(nullptr) == E_INVALIDARG, "real Bind rejects null queue");
    });
}

// INSERT_PRODUCT_NR_IDENTITY

struct FakeChild final : ComFake<ID3D12DeviceChild>
{
    ID3D12Device* device;
    HRESULT getHr = S_OK;
    bool getOutput = true;
    explicit FakeChild(ID3D12Device* value) : device(value) {}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override {
        if (getHr != S_OK || !getOutput) return ReturnInterface(nullptr, getHr, output);
        return device ? device->QueryInterface(iid, output) : E_NOINTERFACE;
    }
};
void TestNrDeviceIdentity()
{
    using Proxy = FakeDevice::Proxy;
    Scenario("NR child resolver handles actual device proxy contracts", [] {
        FakeDevice native, foreign, sl, reshade, nested, otherWrapper, unknown;
        sl.proxy = Proxy::Streamline; sl.native = &native;
        reshade.proxy = Proxy::ReShade; reshade.native = &native;
        nested.proxy = Proxy::ReShade; nested.native = &sl;
        otherWrapper.proxy = Proxy::Streamline; otherWrapper.native = &native;
        unknown.proxy = Proxy::Unknown; unknown.native = &native;
        FakeChild raw(&native);
        for (auto* device : {&native, &sl, &reshade, &nested, &otherWrapper}) {
            FakeChild child(device);
            Check(DetailReuse::SameDevice(&child, &raw), "NR resolves known proxy to native device");
            Check(DetailReuse::SameDevice(&raw, &child), "NR device identity is symmetric");
        }
        FakeChild left(&reshade), right(&otherWrapper), wrong(&foreign), opaque(&unknown);
        Check(DetailReuse::SameDevice(&left, &right), "NR accepts distinct wrappers of one native device");
        Check(&native != &foreign && native.luid.LowPart == foreign.luid.LowPart, "foreign device deliberately shares adapter LUID");
        Check(!DetailReuse::SameDevice(&left, &wrong), "NR rejects different actual devices despite equal LUID");
        Check(!DetailReuse::SameDevice(&raw, &opaque), "NR cannot unwrap unknown proxy");
        Check(!DetailReuse::SameDevice(nullptr, &raw), "NR rejects missing child");
        for (auto hr : {S_FALSE, E_FAIL}) {
            wrong.getHr = hr;
            Check(!DetailReuse::SameDevice(&raw, &wrong), "NR rejects non-S_OK GetDevice");
        }
        wrong.getHr = S_OK; wrong.getOutput = false;
        Check(!DetailReuse::SameDevice(&raw, &wrong), "NR rejects null successful device return");
        for (auto* device : {&native, &foreign, &sl, &reshade, &nested, &otherWrapper, &unknown})
            Check(device->refs == 1, "NR device query references balanced");
        Check(raw.refs == 1 && left.refs == 1 && right.refs == 1, "NR child references unchanged");
    });
}

int main()
{
    try
    {
        TestIdentityAndCompletion();
        TestNrDeviceIdentity();
        std::printf("PASS FG device identity: %u checks / %u scenarios; real COM/WRL; CPU only\n", checks, scenarios);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL FG device identity: scenario=%s; checks=%u; scenarios=%u; %s\n",
                     currentScenario, checks, scenarios, error.what());
        return 1;
    }
}
