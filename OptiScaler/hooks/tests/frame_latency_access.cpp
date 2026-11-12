// The runner inserts the actual wrapper methods and their target resolvers.
// Only DXGI/caller classification/logging are replaced; no SDK or GPU is used.
namespace latency_adapter_tests
{
using framegen::latency::Caller;
using framegen::latency::Flags;
using framegen::latency::Target;
using framegen::latency::WaitableFlag;
using UINT = unsigned;
using HRESULT = int;
using HANDLE = void*;
constexpr HRESULT E_NOINTERFACE = -1, DXGI_ERROR_DEVICE_REMOVED = -2, DXGI_ERROR_INVALID_CALL = -3;
#define STDMETHODCALLTYPE

unsigned checks = 0;
void Check(bool ok, const char* message)
{
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
Caller callerRole = Caller::Application;
bool testSdkLoaded = true;
int callerAddress;
unsigned classifications = 0;
void* _ReturnAddress() { return &callerAddress; }

namespace StreamlineHooks
{
Caller FrameLatencyCaller(void* address)
{
    Check(address == &callerAddress, "classification retains the API caller's return address");
    ++classifications;
    return callerRole;
}
bool DlssgPluginLoaded() { return testSdkLoaded; }
void ReportFrameLatencyAccess(const void*, const Flags&, Caller, bool, Target) {}
}

struct IDXGISwapChain2
{
    unsigned setters = 0, getters = 0, waitables = 0;
    UINT depth = 3, lastRequest = 0;
    HRESULT result = 0;
    HANDLE handle = this;
    std::mutex* bookkeeping = nullptr;
    void Enter()
    {
        if (bookkeeping)
        {
            const bool unlocked = bookkeeping->try_lock();
            Check(unlocked, "no wrapper bookkeeping lock survives into a DXGI call");
            if (unlocked) bookkeeping->unlock();
        }
    }
    HRESULT SetMaximumFrameLatency(UINT request)
    {
        Enter(); ++setters; lastRequest = request;
        if (result >= 0) depth = request;
        return result;
    }
    HRESULT GetMaximumFrameLatency(UINT* output)
    {
        Enter(); ++getters;
        if (!output) return DXGI_ERROR_INVALID_CALL;
        if (result >= 0) *output = depth;
        return result;
    }
    HANDLE GetFrameLatencyWaitableObject() { Enter(); ++waitables; return handle; }
    unsigned Calls() const { return setters + getters + waitables; }
};

struct WrappedIDXGISwapChain4
{
    IDXGISwapChain2* _real2 = nullptr;
    Flags _frameLatencyFlags;
    std::mutex _frameLatencyMutex;
    unsigned observations = 0;
    HRESULT SetMaximumFrameLatency(UINT);
    HRESULT GetMaximumFrameLatency(UINT*);
    HANDLE GetFrameLatencyWaitableObject();
    Target _SelectFrameLatencyTarget(void*);
    void _ObserveFrameLatencySetter(const void* address, UINT, HRESULT)
    {
        Check(address == &callerAddress, "setter telemetry retains the original caller");
        ++observations;
    }
};

struct Dx11wDx12SC
{
    IDXGISwapChain2* _real2 = nullptr;
    IDXGISwapChain2* _fgSwapChain = nullptr;
    Flags _frameLatencyFlags;
    std::mutex _frameLatencyMutex;
    bool _nativeParentRoute = false, _sourceFrameLatencyOwner = false, _starRailF5DsTransport = false;
    HRESULT SetMaximumFrameLatency(UINT);
    HRESULT GetMaximumFrameLatency(UINT*);
    HANDLE GetFrameLatencyWaitableObject();
    Target _SelectFrameLatencyTarget(void*);
};

// INSERT_WRAPPED_LATENCY_APIS
// INSERT_INTEROP_LATENCY_APIS

template<class Wrapper>
void CallsUseOneReceiver(Wrapper& wrapper, IDXGISwapChain2& native, IDXGISwapChain2& provider,
                         IDXGISwapChain2* expected, HRESULT absent = DXGI_ERROR_INVALID_CALL)
{
    native.bookkeeping = provider.bookkeeping = &wrapper._frameLatencyMutex;
    const auto n = native.Calls(), p = provider.Calls();
    const auto classified = classifications;
    UINT output = 99;
    if (!expected)
    {
        Check(wrapper.SetMaximumFrameLatency(7) == absent, "denied/missing setter keeps its actual HRESULT");
        Check(wrapper.GetMaximumFrameLatency(&output) == absent && output == 99,
              "denied/missing getter never fabricates a depth");
        Check(wrapper.GetFrameLatencyWaitableObject() == nullptr, "denied/missing access exposes no wait object");
        Check(native.Calls() == n && provider.Calls() == p, "denied/missing access touches neither swapchain");
    }
    else
    {
        Check(wrapper.GetFrameLatencyWaitableObject() == expected->handle, "waitable belongs to expected receiver");
        Check(wrapper.SetMaximumFrameLatency(7) == 0 && expected->depth == 7,
              "setter updates the same swapchain whose waitable the app received");
        Check(wrapper.GetMaximumFrameLatency(&output) == 0 && output == 7,
              "getter reads that same swapchain's depth");
        Check(native.Calls() == n + (expected == &native ? 3 : 0) &&
              provider.Calls() == p + (expected == &provider ? 3 : 0), "foreign receiver receives no latency calls");
        expected->result = -42;
        output = 99;
        Check(wrapper.SetMaximumFrameLatency(17) == -42 && expected->lastRequest == 17,
              "forward invalid values unchanged so DXGI owns validation and errors");
        Check(wrapper.GetMaximumFrameLatency(&output) == -42 && output == 99,
              "underlying failure returns unchanged without rewriting output");
        Check(wrapper.GetMaximumFrameLatency(nullptr) == DXGI_ERROR_INVALID_CALL,
              "null output validation remains with the chosen DXGI object");
        expected->result = 0;
    }
    Check(classifications > classified, "every adapter resolves caller ownership before invoking DXGI");
}

void Run()
{
    // Rows explicitly cover ownership rather than deriving expected behavior
    // by repeating Select. Native/provider carry distinguishable wait objects.
    struct Case { Caller caller; bool loaded; Flags flags; Target interop, wrapped; };
    const Case cases[] {
        {Caller::Application, true, {0, WaitableFlag, WaitableFlag, true, true, true}, Target::None, Target::None},
        {Caller::Application, true, {WaitableFlag, WaitableFlag, WaitableFlag, true, true, true}, Target::Native, Target::Native},
        {Caller::Application, true, {WaitableFlag, 0, WaitableFlag, true, true, true}, Target::Provider, Target::Preserve},
        {Caller::Application, true, {WaitableFlag, 0, 0, true, true, true}, Target::Preserve, Target::Preserve},
        {Caller::Application, true, {0, WaitableFlag, WaitableFlag, false, true, true}, Target::Preserve, Target::Preserve},
        {Caller::Application, false, {0, WaitableFlag, WaitableFlag, true, true, true}, Target::Preserve, Target::Preserve},
        {Caller::Sdk, true, {0, WaitableFlag, WaitableFlag, true, true, true}, Target::Preserve, Target::Preserve},
        {Caller::Unknown, true, {0, WaitableFlag, WaitableFlag, true, true, true}, Target::Preserve, Target::Preserve}
    };
    for (const auto& item : cases)
    {
#ifdef F6_NEGATIVE_LATENCY_RECEIVER
        if (item.interop != Target::Native) continue;
#endif
        callerRole = item.caller; testSdkLoaded = item.loaded;
        IDXGISwapChain2 native, provider;
        WrappedIDXGISwapChain4 wrapped;
        wrapped._real2 = &native; wrapped._frameLatencyFlags = item.flags;
        CallsUseOneReceiver(wrapped, native, provider, item.wrapped == Target::None ? nullptr : &native);
        Check(wrapped.observations != 0, "physical setter telemetry remains present on success and refusal");
        for (bool parent : {false, true}) for (bool sourceOwner : {false, true}) for (bool f5ds : {false, true})
        {
            Dx11wDx12SC interop;
            interop._real2 = &native; interop._fgSwapChain = &provider; interop._frameLatencyFlags = item.flags;
            interop._nativeParentRoute = parent; interop._sourceFrameLatencyOwner = sourceOwner;
            interop._starRailF5DsTransport = f5ds;
            auto expected = item.interop;
            if (parent && expected == Target::Provider) expected = Target::Preserve;
            if (expected == Target::Preserve) expected = !parent && (!sourceOwner || f5ds) ? Target::Provider : Target::Native;
            CallsUseOneReceiver(interop, native, provider,
                expected == Target::None ? nullptr : expected == Target::Native ? &native : &provider);
        }
    }
    callerRole = Caller::Unknown; testSdkLoaded = true;
    IDXGISwapChain2 native, provider;
    Dx11wDx12SC missing;
    CallsUseOneReceiver(missing, native, provider, nullptr, DXGI_ERROR_DEVICE_REMOVED);
    missing._nativeParentRoute = true;
    CallsUseOneReceiver(missing, native, provider, nullptr, E_NOINTERFACE);
    WrappedIDXGISwapChain4 wrappedMissing;
    CallsUseOneReceiver(wrappedMissing, native, provider, nullptr, E_NOINTERFACE);
    std::printf("PASS %u real frame-latency adapter CPU checks (DXGI/caller mocks; no SDK/game/GPU)\n", checks);
}
#undef STDMETHODCALLTYPE
}
