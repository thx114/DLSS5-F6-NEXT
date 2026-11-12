#include "pch.h"
#include <runtime/ModuleLifetime.h>
#include <native/NrConsumerFilter.h>
#include <integration/HoYoShade.h>
#include "dx11_with_dx12_sc.h"
#include <inputs/FG/GenshinBridge_Present_Dx12.h>
#include <inputs/GenshinFsrContract.h>
#include <genshin/GenshinLegacyCapture.h>
#include <genshin/GenshinDx12Transport.h>
#include <wrapped/wrapped_swapchain.h>
#include <framegen/FinalOutputEndpoint.h>
#include <framegen/dlssg/StarRailF5DsPolicy.h>
#include <dlssnr/DlssNrCurrentOutputCapture.h>
#include <resource_tracking/GenericDepth_Dx11.h>

#include <with_dx12/with_dx12.h>
#include <with_dx12/dx11_with_dx12_sync.h>

#include <hooks/FG_Hooks.h>
#include <hooks/Streamline_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <native/NativeDriverDx11.h>
#include <native/NativeLowLatency.h>

#include <Util.h>
#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNrFgTrace.h>
#include <upscalers/CallerCpuTrace.h>
#include <genshin/GenshinHookCpuTiming.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <optional>
#include <cstring>
#include <atomic>

#pragma intrinsic(_ReturnAddress)

static int scCount = 0;

namespace
{
// R39 owner-guard diagnostic mechanism begin
namespace GenshinPostPresentOwnerDiagnostic
{
enum class Failure { None, PrepareRefused, EpochChanged };

struct Observation
{
    Failure failure = Failure::None;
    uint64_t observedEpoch = 0; // Unknown unless the original RHS was evaluated.
    bool EpochWasRead() const noexcept { return failure != Failure::PrepareRefused; }
    const char* Reason() const noexcept
    {
        switch (failure) {
        case Failure::PrepareRefused: return "prepare-refused";
        case Failure::EpochChanged: return "epoch-changed";
        default: return "none";
        }
    }
};

template <typename ChildOwner>
Observation Evaluate(ChildOwner& child, const uint64_t& expectedEpoch)
{
    // Preserve !Prepare() || Epoch()!=expected: one Prepare; no Epoch on
    // false/throw; one Epoch otherwise. Existing exceptions propagate.
    // Read expectedEpoch by reference only AFTER the actual Epoch call.
    // Re-read the same owner for the RHS, exactly as the original guard did.
    const bool prepared = child->Prepare();
    if (!prepared) return { Failure::PrepareRefused, 0 };
    const auto observedEpoch = child->Epoch();
    return { observedEpoch != expectedEpoch ? Failure::EpochChanged : Failure::None, observedEpoch };
}

class ReportBudget final
{
    static_assert(std::atomic<unsigned>::is_always_lock_free, "Diagnostic budget must stay lock-free");
    std::atomic<unsigned> used_{0};
public:
    static constexpr unsigned MaxReports = 8;
    bool TryAcquire(unsigned& ordinal) noexcept
    {
        auto used = used_.load(std::memory_order_relaxed);
        while (used < MaxReports) {
            // No spurious retries; at most MaxReports successful claims can
            // advance this counter before this call reaches the fixed cap.
            if (used_.compare_exchange_strong(used, used + 1, std::memory_order_relaxed)) {
                ordinal = used + 1;
                return true;
            }
        }
        return false;
    }
};

template <typename Emit>
void ReportFailure(ReportBudget& budget, const Observation& observation, Emit&& emit) noexcept
{
    // Process cap only: no owner registry, allocation or dedup bookkeeping.
    if (observation.failure == Failure::None) return;
    unsigned ordinal = 0;
    if (!budget.TryAcquire(ordinal)) return;
    // Contain only the new log attempt; a failed attempt consumes its slot.
    try { emit(ordinal); } catch (...) {}
}
}
// R39 owner-guard diagnostic mechanism end

template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

void SafeCloseHandle(HANDLE& value)
{
    if (value != nullptr)
    {
        CloseHandle(value);
        value = nullptr;
    }
}

UINT PrivateColorSlotCount(bool genshinPrivateRing, UINT bufferCount,
                           const std::vector<UINT64>& copyFenceValues)
{
    // The native game ring and the optional private copy ring have different
    // retirement domains. Keep ordinary interop sized exactly as before. Once
    // initialized, the private color ring must exactly match its fence ring.
    if (!genshinPrivateRing)
        return bufferCount;
    return copyFenceValues.empty() ? std::max<UINT>(bufferCount, 3) :
        static_cast<UINT>(copyFenceValues.size());
}

struct PrivateCopySlotSelection
{
    bool acquired = false;
    bool completedKnown = false;
    UINT slot = 0;
    UINT64 pending = 0;
    UINT64 completed = 0;
    const char* reason = "CopySlotUnavailable";
};

PrivateCopySlotSelection SelectRetiredPrivateCopySlot(UINT preferred, ID3D12Fence* fence,
                                                       const std::vector<UINT64>& fenceValues)
{
    PrivateCopySlotSelection selection;
    if (fenceValues.empty())
        return selection;

    selection.slot = preferred % static_cast<UINT>(fenceValues.size());
    selection.pending = fenceValues[selection.slot];
    if (fence == nullptr)
    {
        selection.reason = "CopyFenceUnavailable";
        return selection;
    }

    selection.completed = fence->GetCompletedValue();
    selection.completedKnown = true;
    if (selection.completed == UINT64_MAX)
    {
        selection.reason = "CopyFenceFailed";
        return selection;
    }

    // GetCompletedValue is monotonic. A completed or never-used slot can be
    // overwritten without waiting; a live slot remains strictly fail-closed.
    for (size_t offset = 0; offset < fenceValues.size(); ++offset)
    {
        const UINT slot = (selection.slot + static_cast<UINT>(offset)) % static_cast<UINT>(fenceValues.size());
        const UINT64 pending = fenceValues[slot];
        if (pending == 0 || selection.completed >= pending)
        {
            selection.acquired = true;
            selection.slot = slot;
            selection.pending = pending;
            selection.reason = nullptr;
            return selection;
        }
    }

    selection.pending = fenceValues[selection.slot];
    selection.reason = "CopySlotBusy";
    return selection;
}

// Private copy wait receipt begin
namespace PrivateCopyOrdering
{
// One CPU submission receipt, scoped to a single private-copy transaction.
// It proves queue order for one exact writer, never GPU completion or reuse.
class SubmittedWait final
{
    const void* queue_ = nullptr;
    const void* fence_ = nullptr;
    uint64_t value_ = 0;
    HRESULT result_ = E_PENDING;
    bool submitting_ = false, reentered_ = false;

    void Clear() noexcept
    {
        queue_ = fence_ = nullptr;
        value_ = 0;
        result_ = E_PENDING;
    }

  public:
    SubmittedWait() = default;
    SubmittedWait(const SubmittedWait&) = delete;
    SubmittedWait& operator=(const SubmittedWait&) = delete;

    template<typename Submit>
    HRESULT Order(const void* queue, const void* fence, uint64_t value, Submit&& submit)
    {
        if (submitting_)
        {
            reentered_ = true;
            Clear();
            return E_UNEXPECTED;
        }
        if (!queue || !fence || !value || value == UINT64_MAX)
        {
            Clear();
            return E_INVALIDARG;
        }
        if (queue_ == queue && fence_ == fence && value_ == value && SUCCEEDED(result_))
            return result_;

        Clear();
        submitting_ = true;
        reentered_ = false;
        struct Finish { bool& submitting; ~Finish() { submitting = false; } } finish { submitting_ };
        const auto result = submit();
        if (reentered_) return E_UNEXPECTED;
        if (SUCCEEDED(result))
        {
            queue_ = queue;
            fence_ = fence;
            value_ = value;
            result_ = result;
        }
        return result;
    }
};
}
// Private copy wait receipt end

struct PrivateCopySlotOverride
{
    const void* owner;
    UINT slot;
    PrivateCopySlotOverride* previous;
    PrivateCopyOrdering::SubmittedWait wait;
    // Keep comparison identities alive until this transaction leaves the stack.
    // Neither reference is a resource-retirement receipt.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> waitQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> waitFence;
};

thread_local PrivateCopySlotOverride* activePrivateCopySlot = nullptr;

struct ScopedPrivateCopySlot
{
    PrivateCopySlotOverride value;
    ScopedPrivateCopySlot(const void* owner, UINT slot) : value { owner, slot, activePrivateCopySlot }
    {
        activePrivateCopySlot = &value;
    }
    ~ScopedPrivateCopySlot() { activePrivateCopySlot = value.previous; }
    ScopedPrivateCopySlot(const ScopedPrivateCopySlot&) = delete;
    ScopedPrivateCopySlot& operator=(const ScopedPrivateCopySlot&) = delete;
};

UINT ResolvePrivateCopySlot(const void* owner, UINT nativeSlot)
{
    return activePrivateCopySlot && activePrivateCopySlot->owner == owner ? activePrivateCopySlot->slot : nativeSlot;
}

HRESULT OrderPrivateCopyWait(const void* owner, ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value)
{
    if (!queue || !fence) return E_INVALIDARG;
    const auto submit = [&] { return queue->Wait(fence, value); };
    auto* scope = activePrivateCopySlot;
    if (!scope || scope->owner != owner) return submit(); // Generic interop and lifecycle keep real calls.
    // Acquire call-local pins only after Order accepts submission. A reentrant
    // call must not replace the outer call's owners before its Wait returns.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> submittedQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> submittedFence;
    const auto result = scope->wait.Order(queue, fence, value, [&] {
        submittedQueue = queue;
        submittedFence = fence;
        return submittedQueue->Wait(submittedFence.Get(), value);
    });
    if (SUCCEEDED(result) && submittedQueue)
    {
        scope->waitQueue = std::move(submittedQueue);
        scope->waitFence = std::move(submittedFence);
    }
    return result;
}

// Armed only around this authenticated Genshin private-copy transaction.
// Capture the first refusal before fallback/teardown changes its fence state.
struct PrivateCopyFailureScope;
thread_local PrivateCopyFailureScope* activePrivateCopyFailure = nullptr;

struct PrivateCopyFailureScope
{
    const void* owner;
    UINT slot;
    ID3D12Fence* fence;
    const std::vector<UINT64>& values;
    const char* stage = "SharedBackBuffer";
    const char* reason = nullptr;
    UINT64 pending = 0;
    UINT64 completed = 0;
    bool completedKnown = false;
    HRESULT result = S_OK; // S_OK denotes a predicate refusal, not an API failure.
    PrivateCopyFailureScope* previous;

    PrivateCopyFailureScope(const void* owner, UINT slot, ID3D12Fence* fence,
                            const std::vector<UINT64>& values, bool enabled)
        : owner(owner), slot(slot), fence(fence), values(values), previous(activePrivateCopyFailure)
    {
        activePrivateCopyFailure = enabled ? this : nullptr;
    }
    ~PrivateCopyFailureScope() { activePrivateCopyFailure = previous; }
};

bool RecordPrivateCopyFailure(const void* owner, const char* reason, HRESULT result = S_OK,
                              std::optional<UINT64> observedCompleted = std::nullopt)
{
    auto* failure = activePrivateCopyFailure;
    if (failure && failure->owner == owner && !failure->reason)
    {
        failure->reason = reason;
        failure->result = result;
        failure->pending = failure->slot < failure->values.size() ? failure->values[failure->slot] : 0;
        failure->completedKnown = observedCompleted.has_value() || failure->fence != nullptr;
        failure->completed = observedCompleted ? *observedCompleted :
            (failure->fence ? failure->fence->GetCompletedValue() : 0);
    }
    return false;
}

bool AllowPrivateCopyFailureLog()
{
    // Process-wide bound: at most one first-failure record per second, without
    // retaining swapchain pointers or adding per-instance header state.
    static std::atomic<ULONGLONG> lastTick { 0 };
    const ULONGLONG now = GetTickCount64();
    ULONGLONG previous = lastTick.load(std::memory_order_relaxed);
    if (previous != 0 && now - previous < 1000)
        return false;
    return lastTick.compare_exchange_strong(previous, now, std::memory_order_relaxed);
}

bool WaitForQueueIdle(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue,
                      InteropQueueDrainReceipt* pendingDrain = nullptr)
{
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::QueueIdle, queue, reinterpret_cast<uint64_t>(fence), State::Instance().frameCount);
    if (!queue || !fence || (!pendingDrain && !fenceEvent))
        return pendingDrain == nullptr;

    auto submit = [&] {
        if (fenceValue >= UINT64_MAX - 1) return false;
        const auto result = queue->Signal(fence, ++fenceValue);
        if (FAILED(result))
            LOG_ERROR("FG/present queue idle Signal failed: {:X}", (UINT) result);
        return SUCCEEDED(result);
    };
    if (pendingDrain ? !pendingDrain->ReuseOrSubmit(queue, fence, fenceValue, submit) : !submit())
        return false;
    const auto waitValue = fenceValue;
    const auto completed=fence->GetCompletedValue();
    if(completed==UINT64_MAX)return false;
    if (completed >= waitValue)
        return true;
    if (pendingDrain)
        return false;

    const auto result = fence->SetEventOnCompletion(waitValue, fenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("FG/present queue idle SetEventOnCompletion failed. fence {}, completed {}, result {:X}", waitValue,
                  fence->GetCompletedValue(), (UINT) result);
        return false;
    }

    CallerCpuTrace::Scope queueIdleCpu(CallerCpuTrace::Op::QueueIdleWait, queue, reinterpret_cast<uint64_t>(fence), State::Instance().frameCount, UINT64_MAX, waitValue);
    const auto waitResult = WaitForSingleObject(fenceEvent, 5000);
    queueIdleCpu.Finish(waitResult, true);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("FG/present queue idle wait failed. fence {}, completed {}, waitResult {:X}", waitValue,
                  fence->GetCompletedValue(), waitResult);
        return false;
    }

    const auto finalCompleted=fence->GetCompletedValue();
    return finalCompleted!=UINT64_MAX && finalCompleted>=waitValue;
}

void TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (commandList == nullptr || resource == nullptr || beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;

    commandList->ResourceBarrier(1, &barrier);
}

UINT ResolveBufferCount(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferCount > 0)
        return desc.BufferCount;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)) && desc1.BufferCount > 0)
        return desc1.BufferCount;

    return 2;
}

DXGI_FORMAT ResolveBufferFormat(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferDesc.Format != DXGI_FORMAT_UNKNOWN)
        return desc.BufferDesc.Format;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)))
        return desc1.Format;

    return DXGI_FORMAT_UNKNOWN;
}

bool IsSame(IDXGISwapChain* swapchain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if (swapchain == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(swapchain->GetDesc(&desc)))
        return false;

    const UINT resolvedBufferCount = bufferCount != 0 ? bufferCount : desc.BufferCount;
    const UINT resolvedWidth = width != 0 ? width : desc.BufferDesc.Width;
    const UINT resolvedHeight = height != 0 ? height : desc.BufferDesc.Height;
    const DXGI_FORMAT resolvedFormat = format != DXGI_FORMAT_UNKNOWN ? format : desc.BufferDesc.Format;

    return resolvedBufferCount == desc.BufferCount && resolvedWidth == desc.BufferDesc.Width &&
           resolvedHeight == desc.BufferDesc.Height && resolvedFormat == desc.BufferDesc.Format && flags == desc.Flags;
}

// Match PrepareDx12InteropDesc/Desc1 for every DX12 presenter, including DLSSG.
// Only XeFG's flag preservation remains a game quirk. The native resize below
// always receives the game's untouched arguments.
Dx11InteropSwapchain::ResizeArgs ResizeArgsForFg(IDXGISwapChain* fgSwapChain, UINT bufferCount,
                                                DXGI_FORMAT format, UINT flags)
{
    auto args = Dx11InteropSwapchain::PresenterResizeArgs(bufferCount, format, flags);

    if (!State::Instance().gameQuirks[GameQuirk::XeFGBridgeResizeAsCreated])
        return args;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (State::Instance().activeFgOutput == FGOutput::XeFG && fgSwapChain != nullptr &&
        SUCCEEDED(fgSwapChain->GetDesc(&desc)))
    {
        args.flags = desc.Flags;
    }

    LOG_INFO("XeFG bridge resize as created: count {} -> {}, format {} -> {}, flags {:X} -> {:X}", bufferCount,
             args.bufferCount, (UINT) format, (UINT) args.format, flags, args.flags);

    return args;
}
} // namespace

Dx11wDx12SC::Dx11wDx12SC(IDXGISwapChain* real, IDXGISwapChain4* fgSC, ID3D11Device* pDevice, HWND hWnd, UINT flags,
                         std::unique_ptr<GenshinVisibleChild> visibleChild, bool nativeParentRoute,
                         bool sourceFrameLatencyOwner, std::optional<UINT> applicationFlags)
    : SwapchainHookBoundary(OperationsFor<Dx11wDx12SC>()),
      _nativeParentRoute(nativeParentRoute),
      _sourceFrameLatencyOwner(sourceFrameLatencyOwner && !nativeParentRoute && real != nullptr && fgSC != nullptr),
      _starRailF5DsTransport(_sourceFrameLatencyOwner && StarRailF5Ds::Selected()),
      _visibleChild(std::move(visibleChild)), _real(real), _fgSwapChain(fgSC), _refcount(1),
      _dx11Device(pDevice), _handle(hWnd)
{
    _visibleProviderHwnd = _visibleChild ? _visibleChild->Child() : nullptr;
    // Retain an uncertain partial factory owner too. A mismatched/null HWND
    // may not authorize child work or teardown; it must not look like no owner.
    _visibleProvider = _visibleChild ? State::Instance().currentFG : nullptr;
    _id = ++scCount;
    _lastFlags = flags;
    // The factory must supply the application's descriptor BEFORE any SDK or
    // factory overrides. The existing flags parameter alone is effective data.
    _frameLatencyFlags.requestedKnown = applicationFlags.has_value();
    _frameLatencyFlags.requested = applicationFlags.value_or(0);
    if (applicationFlags) {
        StreamlineHooks::RecordFrameLatencyApplicationFlags(_real,*applicationFlags);
        StreamlineHooks::RecordFrameLatencyApplicationFlags(_fgSwapChain,*applicationFlags);
    }

    // Exclusive fullscreen on XeFG's swapchain behind this bridge leaves every following Present failing with
    // DXGI_ERROR_INVALID_CALL (seen in Prey and Blair Witch, both D3D11 through Dx11wDx12). Default to the existing
    // borderless override here, same as the per-game ForceBorderlessWhenUsingXeFG quirk does; an explicit
    // ForceBorderless in the ini still wins. FGHooks::hkSetFullscreenState reads this live, so it applies to the
    // game's first SetFullscreenState right after this swapchain is handed back.
    if (State::Instance().activeFgOutput == FGOutput::XeFG && !Config::Instance()->FGXeFGForceBorderless.has_value())
    {
        LOG_INFO("XeFG through Dx11wDx12: forcing borderless instead of exclusive fullscreen");
        Config::Instance()->FGXeFGForceBorderless.set_volatile_value(true);
    }

    if (_real != nullptr)
    {
        _real->AddRef();
        _real->QueryInterface(IID_PPV_ARGS(&_real1));
        _real->QueryInterface(IID_PPV_ARGS(&_real2));
        _real->QueryInterface(IID_PPV_ARGS(&_real3));
        _real->QueryInterface(IID_PPV_ARGS(&_real4));
    }

    if (_fgSwapChain != nullptr)
        _fgSwapChain->AddRef();

    if (_dx11Device != nullptr)
    {
        _dx11Device->AddRef();
        auto device5Result = _dx11Device->QueryInterface(IID_PPV_ARGS(&_dx11Device5));
        if (FAILED(device5Result))
            LOG_WARN("ID3D11Device5 unavailable: {:X}", (UINT) device5Result);

        _dx11Device->GetImmediateContext(&_dx11Context);
        if (_dx11Context != nullptr)
        {
            auto context4Result = _dx11Context->QueryInterface(IID_PPV_ARGS(&_dx11Context4));
            if (FAILED(context4Result))
                LOG_WARN("ID3D11DeviceContext4 unavailable: {:X}", (UINT) context4Result);
        }
    }

    if (WithDx12::PrepareD3D12ForD3D11(_dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        _dx12Device = WithDx12::GetD3D12Device();
        _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
        if(_nativeParentRoute) {
            if(_dx12Device)_dx12Device->AddRef();
            if(_dx12CommandQueue)_dx12CommandQueue->AddRef();
        }
    }
    else
    {
        LOG_ERROR("failed to resolve D3D12 device/queue");
    }

    State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

    _RefreshCachedSwapchainDesc();
    _RefreshFrameLatencyFlags();
    if (_nativeParentRoute)
    {
        // Both sealed R625 factories pass a newly created, unexposed native
        // chain here. Seed only its standard 8-bit SDR default, never an HDR
        // or arbitrary existing chain. Successful setters replace this fact.
        DXGI_SWAP_CHAIN_DESC fresh{};
        _nativeColorSpaceKnown = _real3 && _real && SUCCEEDED(_real->GetDesc(&fresh)) &&
            (fresh.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
             fresh.BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
             fresh.BufferDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
             fresh.BufferDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        if (_nativeColorSpaceKnown) _nativeColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        // Provider-created child state is not inferred from its pixel format.
        // Its SDR setter and existing lifetime gates must pass before first use.
        _childRevoked = true;
        _childRecoveryAllowed = _visibleChild && _fgSwapChain;
    }

    LOG_INFO("Dx11wDx12SC {} created, real: {:X}, fg: {:X}, dx11: {:X}, dx12: {:X}, queue: {:X}", _id, (UINT64) _real,
             (UINT64) _fgSwapChain, (UINT64) _dx11Device, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
}

Dx11wDx12SC::~Dx11wDx12SC()
{
    if(!_nativeParentRoute) {
        GenshinBridgeFg::Revoke();
        DlssNrFgTrace::EndBridge();
        MenuOverlayDx::CleanupRenderTarget(true,_handle);
    }
    _ReleaseInteropObjects();

    SafeRelease(_real4);
    SafeRelease(_real3);
    SafeRelease(_real2);
    SafeRelease(_real1);
    SafeRelease(_fgSwapChain);
    SafeRelease(_real);
    SafeRelease(_dx11Context4);
    SafeRelease(_dx11Context);
    SafeRelease(_dx11Device5);
    SafeRelease(_dx11Device);
    if(_nativeParentRoute){SafeRelease(_dx12CommandQueue);SafeRelease(_dx12Device);}
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (ppvObject == nullptr)
        return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_nativeParentRoute ? _real4 == nullptr : (_real4 == nullptr && _fgSwapChain == nullptr))
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    if (riid == __uuidof(Dx11wDx12SC))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::AddRef()
{
    auto count = InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", count, Util::WhoIsTheCaller(_ReturnAddress()));
    return count;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);
    if (!ModuleLifetime::CleanupAllowed())
    {
        if (ret == 0)
        {
            InterlockedExchange(&_refcount, 1); // retain the whole owner, including all COM members
            return 1;
        }
        return ret;
    }

    LOG_TRACE("Count: {}, caller: {}", ret, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0 && (_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues)))
    {
        InterlockedExchange(&_refcount, 1); // retain real wrapper/rings without lifecycle teardown
        return 1;
    }
    if (ret == 0 && _nativeParentRoute) return _ReleaseNativeParent();
    if (ret == 0)
    {
        if (_visibleChild && (!_visibleChild->OnOwnerThread() || _visiblePresentEntered))
        {
            InterlockedExchange(&_refcount, 1);
            _visibleChild->RequestHide();
            LOG_ERROR("R564 Release retained: unsupported thread or Present reentry");
            return 1;
        }
        if (_visibleChild) _visibleChild->Invalidate();
        auto fg = State::Instance().currentFG;
        const bool releaseFg = fg != nullptr && (!_visibleChild || _fgSwapChain) &&
            fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr;
        auto lifecycleExecution = fg != nullptr ? fg->GuardExecution(FGExecutionScope::Lifecycle) : FGExecutionGuard {};
        if (releaseFg && !fg->PrepareForSwapchainChange(true))
        {
            InterlockedExchange(&_refcount, 1);
            LOG_ERROR("Dx11wDx12SC Release vetoed: FG owner teardown incomplete");
            return 1;
        }

        // Keep the complete wrapper (including MSAA stage, shared handles,
        // command rings, both swapchains and devices) alive after failed waits.
        // Do not clear global owners or delete this before the actual drain.
        if (!_ReleaseInteropObjects())
        {
            InterlockedExchange(&_refcount, 1);
            LOG_ERROR("Dx11wDx12SC Release retained: interop completion uncertain; retry Release to drain");
            return 1;
        }

        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentWrappedSwapchain == this)
            State::Instance().currentWrappedSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == _real)
            State::Instance().currentRealSwapchain = nullptr;

        FGHooks::ClearDx12InteropPresentSC(_fgSwapChain);

        if (State::Instance().currentFGSwapchain == _fgSwapChain)
            State::Instance().currentFGSwapchain = nullptr;

        if (State::Instance().currentD3D11Device == _dx11Device)
            State::Instance().currentD3D11Device = nullptr;

        State::Instance().swapchainInteropApi = SwapchainInteropApi::None;

        const auto handle = _visibleChild ? _visibleProviderHwnd : _handle;

        // Intel: XeFG's context is destroyed after every reference to its swapchain is gone (else
        // xefgSwapChainDestroy fails with POINTER_STILL_IN_USE). The destructor releases ours.
        delete this;

        if (releaseFg)
            fg->ReleaseSwapchain(handle);
    }

    return ret;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real != nullptr ? _real->SetPrivateData(Name, DataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real != nullptr ? _real->SetPrivateDataInterface(Name, pUnknown) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real != nullptr ? _real->GetPrivateData(Name, pDataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetParent(REFIID riid, void** ppParent)
{
    return _real != nullptr ? _real->GetParent(riid, ppParent) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDevice(REFIID riid, void** ppDevice)
{
    return _real != nullptr ? _real->GetDevice(riid, ppDevice) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(UINT SyncInterval, UINT Flags)
{
    if (_nativeParentRoute) return _PresentNativeVisible(SyncInterval, Flags, nullptr);
    // R897: top-level admitted source only, before LowLatency/interop/provider/native owner scopes.
    if (_sourceFrameLatencyOwner && !(Flags & DXGI_PRESENT_TEST) &&
        GenshinHookCpuTiming::starRailSourceDepth == 0)
        FGHooks::DrainStarRailAttribution();
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Present, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, Flags, 0, 0, true);
    GenshinHookCpuTiming::StarRailInvocationScope attributionParent(
        _sourceFrameLatencyOwner && !(Flags & DXGI_PRESENT_TEST),this,_fgSwapChain,callerCpuScope.BeginQpc());
    if (_real == nullptr || _fgSwapChain == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;
    // Before initialization, copies or allocator resets, even when filtering is off.
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues)) ||
        HoYoShade::ColorConsumers(_fgSwapChain) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;

    if (_fg == nullptr)
        _fg = State::Instance().currentFG;

    if ((Flags & DXGI_PRESENT_TEST) != 0)
    {
        CallerCpuTrace::Scope testPresentCpu(CallerCpuTrace::Op::TestPresent, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, Flags);
        return _real->Present(SyncInterval, Flags);
    }

    // A failed provider resize may have destroyed its fake backbuffers even
    // when a later GetBuffer returns S_OK/a raw buffer. Do not copy, run NR/FG
    // or Present on that unvalidated chain. In this route _real belongs to a
    // hidden HWND, so its S_OK would NOT establish visible native fallback.
    // Recovery requires a real successful resize; no owners are retired here.
    if (const auto failure = _fgResizeStatus.Failure(); FAILED(failure))
        return failure;

    if (_starRailF5DsTransport)
    {
        const auto sourceFailure = _sourceDeviceFailure.load(std::memory_order_acquire);
        if (FAILED(sourceFailure))
        {
            callerCpuScope.Result(static_cast<uint32_t>(sourceFailure));
            return sourceFailure; // no further copies/NR/FG work on a known removed source device
        }
    }

    // Optical F5Low's low latency (native/NativeLowLatency.h): Reflex markers around the game's present, on the game's
    // own D3D11 device. This is the game's only present here; the bridge's D3D12 presents belong to frame generation.
    struct LowLatencyScope
    {
        explicit LowLatencyScope(IUnknown* device) : _device(device)
        {
            if (_device != nullptr)
                native::lowlatency::OnPresentBegin(_device);
        }

        ~LowLatencyScope()
        {
            if (_device != nullptr)
                native::lowlatency::OnPresentEnd(_device);
        }

        IUnknown* _device;
    } lowLatencyScope(State::Instance().currentD3D11Device);

    GenshinBridgeFg::PresentScope genshinFgPresent(_real, _dx11Context, _fgSwapChain, _fg, _dx12Device, _dx12CommandQueue);

    if (!_InitInteropObjects())
        return DXGI_ERROR_DEVICE_REMOVED;

    auto dx11Index = _GetDx11BackBufferIndexForPresent();

    if (!_RequestSharedBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    const bool genshinSender=genshinFgPresent.MatchesNative(_real,_dx11Device);
    if(genshinSender)NativeMotionDx11::OnFGPresent(_real,_dx11Device);

    auto* colourOwner=dynamic_cast<DLSSG_Dx12*>(_fg);
    const auto colourSource=colourOwner && State::Instance().currentFG==colourOwner ?
        colourOwner->CurrentUpscalerColourSource() : DLSSG_Dx12::UpscalerColourSourcePtr{};
    CopySlotAdmission copyAdmission;
    if (!_CopyDx11BackBufferToShared(dx11Index, copyAdmission))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_WaitDx11ThenDx12())
        return DXGI_ERROR_DEVICE_REMOVED;

    ComPtr<ID3D12Resource> copiedColour;
    if (!_CopyDx11SharedToDx12FGBackBuffer(dx11Index, copyAdmission,copiedColour.GetAddressOf()))
        return DXGI_ERROR_DEVICE_REMOVED;
    const auto copiedValue=_lastInteropCopyFenceValue;

    // Overwrites the raw copy just made above with native input's processed picture, when that option is on; a no-op
    // otherwise. Runs here, before FG takes the frame: this is the only present this game gets once Dx11wDx12SC
    // exists, so it is also where the D3D11 depth finder's own frame close has to happen (NativeMotionDx11::
    // OnFGPresent does that), same placement rule Story F used on the D3D12 side.
    if (!genshinSender && !DlssNr::UsesGenshinPresentPipeline())
    {
        native::nrfilter::OpticalInputScope inputSource(_real, _dx11Device);
        _ApplyNativeInputToFGBackBuffer();
    }
    // The optional overwrite must publish its OWN final signal. Never let an
    // earlier successful raw-copy receipt authorize reads of a partial overwrite.
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues)) ||
        !native::nrfilter::FinalCopyReceiptValid(_copyFenceValue, _lastInteropCopyFenceValue)) return E_FAIL;

    if (!_WaitForInteropCopyOnPresentQueue())
        return DXGI_ERROR_DEVICE_REMOVED;
    if (_sourceFrameLatencyOwner)
        GenshinHookCpuTiming::ObserveStarRailCopyReceipt(this,_dx12CommandQueue,_copyFence,_lastInteropCopyFenceValue);

    auto* nrPresentQueue = _fg != nullptr ? _fg->GetCommandQueue() : nullptr;
    if (!nrPresentQueue) nrPresentQueue = _dx12CommandQueue;
    CallerCpuTrace::Scope nrPresentCpu(CallerCpuTrace::Op::NrPresent, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    const auto bridgeOutcome = [&] {
        // Generic interop copies this exact _real buffer/index. Native-parent
        // Present already returned through _PresentNativeVisible above.
        // End the borrowed source scope before any later natural/FG Present.
        native::nrfilter::BridgeScope nrFilterSource(_real, _dx11Device, dx11Index,
            _fgSwapChain, WithDx12::GetD3D12CommandQueue(), nrPresentQueue, State::Instance().frameCount,
            _dx12CommandQueue, _copyFence, _lastInteropCopyFenceValue);
        return DlssNr::ApplyToFinishedPictureDx11Bridge(_fgSwapChain, nrPresentQueue, WithDx12::GetD3D12CommandQueue(),
            genshinFgPresent.NativeReturn(), colourSource ? colourSource->SourceMetadata() : GuideSourceMetadata::Ptr{});
    }();
    if (HoYoShade::ColorConsumers(_fgSwapChain) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
    // The real interop Wait above orders this exact copy on nrPresentQueue.
    // An optional native overwrite has a different receipt/source, so it cannot
    // borrow the selected NGX source's colour identity.
    if (colourOwner && State::Instance().currentFG==colourOwner && copiedValue==_lastInteropCopyFenceValue)
        colourOwner->JoinUpscalerFinalColour(colourSource,_fgSwapChain,nrPresentQueue,bridgeOutcome,
            copiedColour.Get(),_copyFence,copiedValue);
    GenshinBridgeFg::PresentScope::Finished(bridgeOutcome);
    nrPresentCpu.Finish();

    const bool fgHookedPresenter =
        State::Instance().currentFGSwapchain == _fgSwapChain && !FGHooks::IsDx12InteropPresentSC(_fgSwapChain);

    // The game-facing DX11 swapchain is never presented in this wrapper.
    // For a plain external DX12 presenter, draw Opti's overlay here.
    // For a real FG swapchain, FGHooks::FGPresent/LocalPresent owns overlay/present-side work;
    // drawing it here would double-enter the overlay path before the FG present hook.
    if (!fgHookedPresenter)
        MenuOverlayDx::Present(_fgSwapChain, SyncInterval, Flags, nullptr, _dx12CommandQueue, _handle, false);
    else
        LOG_TRACE("real FG presenter detected; skipping wrapper overlay path");

    LOG_DEBUG("frame {}, dx11Index {}, real3Index {}, fakeIndex {}, bufferCount {}", State::Instance().frameCount,
              dx11Index, _real3 != nullptr ? _real3->GetCurrentBackBufferIndex() : 0xFFFFFFFF, _currentFakeIndex,
              _bufferCount);

    if (_real != nullptr)
    {
        UINT realFlags = Flags;
        DXGI_SWAP_CHAIN_DESC hiddenDesc {};
        // R862: the factory-admitted StarRail source owns a distinct hidden
        // flip chain. Only ordinary presents may drop that hidden submission;
        // TEST already returned above and special caller flags keep their path.
        const bool noWaitHidden = _sourceFrameLatencyOwner && !_starRailF5DsTransport && !_visibleChild && _real3 != nullptr &&
            (Flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING)) == 0 &&
            SUCCEEDED(_real->GetDesc(&hiddenDesc)) && hiddenDesc.Windowed &&
            hiddenDesc.OutputWindow != nullptr && hiddenDesc.OutputWindow != _handle &&
            (hiddenDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
             hiddenDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
        if (noWaitHidden) realFlags |= DXGI_PRESENT_DO_NOT_WAIT;

        const bool collectHidden = _sourceFrameLatencyOwner && GenshinHookCpuTiming::StarRailTimingActive();
        auto hiddenStamp = collectHidden ? GenshinHookCpuTiming::MakeStarRailStamp(
            GenshinHookCpuTiming::StarRailOrigin::Hidden,_fgSwapChain) : GenshinHookCpuTiming::StarRailStamp{};
        if (collectHidden) {
            hiddenStamp.chain=reinterpret_cast<uintptr_t>(_real);
            hiddenStamp.sync=0;hiddenStamp.flags=realFlags;hiddenStamp.flagsKnown=true;
        }
        // slot is a copy slot only. Real DXGI flags live in the explicit attribution field.
        CallerCpuTrace::Scope hiddenPresentCpu(CallerCpuTrace::Op::HiddenPresent, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
        auto realPresentResult = _real->Present(0, realFlags);
        hiddenPresentCpu.Finish(static_cast<uint32_t>(realPresentResult),true);
        if (collectHidden) {
            hiddenStamp.result=static_cast<uint32_t>(realPresentResult);hiddenStamp.resultKnown=true;
            GenshinHookCpuTiming::RecordStarRailCall(GenshinHookCpuTiming::StarRailCall::HiddenPresent,hiddenStamp,
                {true,static_cast<int64_t>(hiddenPresentCpu.BeginQpc()),static_cast<int64_t>(hiddenPresentCpu.EndQpc())},true);
        }

        // Busy queued no hidden Present and performed no raw flip. Do not retry
        // or invent a raw index: both public and copy-side index queries use
        // _real3. The independent copy ring still advances only after the visible
        // child succeeds below; all copy/queue/fence ownership stays unchanged.
        if (FAILED(realPresentResult) &&
            !(noWaitHidden && realPresentResult == DXGI_ERROR_WAS_STILL_DRAWING))
            LOG_WARN("hidden real DX11 Present failed: {:X}", (UINT) realPresentResult);
        if (_starRailF5DsTransport &&
            (realPresentResult == DXGI_ERROR_DEVICE_REMOVED || realPresentResult == DXGI_ERROR_DEVICE_RESET ||
             realPresentResult == DXGI_ERROR_DEVICE_HUNG || realPresentResult == DXGI_ERROR_DRIVER_INTERNAL_ERROR))
        {
            // Preserve the real source failure instead of replacing it with a child S_OK.
            // Earlier submitted work retains all of its normal queue/fence/quarantine owners.
            _sourceDeviceFailure.store(realPresentResult, std::memory_order_release);
            callerCpuScope.Result(static_cast<uint32_t>(realPresentResult));
            return realPresentResult;
        }
    }

    CallerCpuTrace::Scope fgPresentCpu(CallerCpuTrace::Op::FgPresent, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, Flags);
    auto result = _fgSwapChain->Present(SyncInterval, Flags);
    genshinFgPresent.Presented(result);
    fgPresentCpu.Finish();

    if (SUCCEEDED(result))
        _AdvanceFakeBackBufferIndex();
    else
        LOG_ERROR("fg Present failed: {:X}", (UINT) result);

    callerCpuScope.Result(static_cast<uint32_t>(result));
    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    return _real != nullptr ? _real->GetBuffer(Buffer, riid, ppSurface) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookSetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    LOG_DEBUG("Dx11wDx12SC SetFullscreenState: {}, target: {:X}, caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    if (_nativeParentRoute)
    {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){_SuspendUnobservedNativeDisplay();return _real ? _real->SetFullscreenState(Fullscreen,pTarget) : DXGI_ERROR_DEVICE_REMOVED;}
        BOOL current = TRUE;
        const bool sameWindowed = !Fullscreen && !pTarget && _real &&
            SUCCEEDED(_real->GetFullscreenState(&current,nullptr)) && !current;
        if (!sameWindowed) { _RevokeNativeChild(true); _TryDrainNativeChild(); }
        const auto hr=_real ? _real->SetFullscreenState(Fullscreen,pTarget) : DXGI_ERROR_DEVICE_REMOVED;
        if (FAILED(hr)) _RevokeNativeChild(true);
        if(SUCCEEDED(hr))State::Instance().realExclusiveFullscreen=Fullscreen;
        return hr;
    }
    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetFullscreenState(Fullscreen, pTarget);

    return _real != nullptr ? _real->SetFullscreenState(Fullscreen, pTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookGetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    if (_nativeParentRoute) return _real->GetFullscreenState(pFullscreen, ppTarget);
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenState(pFullscreen, ppTarget);

    return _real != nullptr ? _real->GetFullscreenState(pFullscreen, ppTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    auto result = _real->GetDesc(pDesc);
    if (SUCCEEDED(result) && pDesc != nullptr)
        pDesc->OutputWindow = _handle;

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                     UINT SwapChainFlags)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (_nativeParentRoute)
    {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){if(_visibleChild)_visibleChild->RequestHide();return DXGI_ERROR_WAS_STILL_DRAWING;}
        _RevokeNativeChild(true);
        _TryDrainNativeChild(); // failure retains private child owners only
        DlssNrCurrentOutput::Capture::ResizeScope capture(DlssNrCurrentOutput::Capture::ResizeScope::Mode::ReserveOnly);
        if(!capture)return DXGI_ERROR_WAS_STILL_DRAWING;
        const auto before=GenshinLegacy::BeforeResize(_real);
        if(FAILED(before))return before;
        if(!DlssNr::WaitForFinishedPicture() || !capture.ReadyForResize())return DXGI_ERROR_WAS_STILL_DRAWING;
        MenuOverlayDx::CleanupRenderTarget(true,_handle);
        const auto hr=_real ? (_real->ResizeBuffers(BufferCount,Width,Height,NewFormat,SwapChainFlags)) : DXGI_ERROR_DEVICE_REMOVED;
        if(SUCCEEDED(hr)){
            if (NewFormat != DXGI_FORMAT_UNKNOWN && NewFormat != _bufferFormat) _nativeColorSpaceKnown = false;
            _RefreshCachedSwapchainDesc();_RefreshFrameLatencyFlags();State::Instance().scChanged=true;
            State::Instance().SCAllowTearing=(SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)!=0;}
        return hr;
    }


    if (const auto failure = _fgResizeStatus.RecoveryFailure(Config::Instance()->FGSkipResizeBuffers.value_or_default());
        FAILED(failure)) return failure;

    // Reserve capture admission BEFORE late Reset, exactly as R610. The
    // reservation outlives both actual native and child resize calls.
    std::optional<DlssNrCurrentOutput::Capture::ResizeScope> outputCaptureResize;
    if (_visibleChild) {
        outputCaptureResize.emplace(DlssNrCurrentOutput::Capture::ResizeScope::Mode::ReserveOnly);
        if (!*outputCaptureResize) return DXGI_ERROR_WAS_STILL_DRAWING;
        const auto captureResize=GenshinLegacy::BeforeResize(_real);
        if (FAILED(captureResize)) return captureResize;
        if (!DlssNr::WaitForFinishedPicture() || !outputCaptureResize->ReadyForResize())
            return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    GenshinBridgeFg::Revoke();
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Resize, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    auto lifecycleOwner = State::Instance().currentFG;
    auto lifecycleExecution = lifecycleOwner != nullptr ? lifecycleOwner->GuardExecution(FGExecutionScope::Lifecycle) : FGExecutionGuard {};
    auto fgArgs = ResizeArgsForFg(_fgSwapChain, BufferCount, NewFormat, SwapChainFlags);
    if (_visibleChild) fgArgs.flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    const bool skipFgResize = _fgResizeStatus.CanSkipResize() &&
        IsSame(_fgSwapChain, fgArgs.bufferCount, Width, Height, fgArgs.format, fgArgs.flags);
    if (!skipFgResize && lifecycleOwner != nullptr && !lifecycleOwner->PrepareForSwapchainChange())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    GenshinBridgeFg::Collect(); // successful provider drain receipts can release both backbuffers before Resize

    const bool synchronizeXeFGPresent = skipFgResize && State::Instance().activeFgOutput == FGOutput::XeFG &&
                                        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;

    std::unique_lock<std::shared_mutex> presentResizeLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (synchronizeXeFGPresent)
    {
        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
        {
            State::Instance().fgChanged = true;
            fg->UpdateTarget();
            fg->Deactivate();
        }

        LOG_DEBUG("Waiting for XeFG native presents before equivalent ResizeBuffers");
        CallerCpuTrace::Scope resizeMutexCpu(CallerCpuTrace::Op::ResizeMutexWait, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
        presentResizeLock.lock();
        resizeMutexCpu.Finish();
        LOG_DEBUG("XeFG native present barrier acquired");
    }

    if (!_WaitForCopyQueueIdle())
    {
        LOG_WARN("ResizeBuffers retained old owners: interop drain incomplete");
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    if (synchronizeXeFGPresent && !WaitForQueueIdle(_dx12CommandQueue, _copyFence, _copyFenceEvent, _copyFenceValue))
    {
        LOG_WARN("ResizeBuffers retained old owners: FG/present wait incomplete");
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropBackBuffers();

    HRESULT realResult = _real != nullptr ? _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags)
                                          : DXGI_ERROR_DEVICE_REMOVED;

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (_visibleChild && SUCCEEDED(realResult)) _visibleChild->Prepare();
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        if (skipFgResize)
        {
            LOG_DEBUG("Skipping FG ResizeBuffers");
            fgResult = S_OK;
        }
        else
        {
            UINT childWidth = Width, childHeight = Height;
            if (_visibleChild)
            {
                DXGI_SWAP_CHAIN_DESC actual{};
                if (SUCCEEDED(_real->GetDesc(&actual)))
                { childWidth = actual.BufferDesc.Width; childHeight = actual.BufferDesc.Height; }
            }
            fgResult = _fgSwapChain->ResizeBuffers(fgArgs.bufferCount, childWidth, childHeight, fgArgs.format, fgArgs.flags);
        }
    }

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    _RefreshFrameLatencyFlags();
    _fgResizeStatus.Complete(realResult, fgResult, !skipFgResize && _fgSwapChain != nullptr);
    if (SUCCEEDED(realResult) && FAILED(fgResult))
        LOG_ERROR("DX12 presenter resize failed: {:X}; interop Present blocked until successful resize. Hidden DX11 source is not a visible fallback",
                  (UINT) fgResult);

    if (SUCCEEDED(realResult))
    {
        _RefreshCachedSwapchainDesc();

        if (SUCCEEDED(fgResult) && Config::Instance()->FGEnabled.value_or_default())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

        if (State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(Width);
            State::Instance().screenHeight = static_cast<float>(Height);
            State::Instance().lastMipBias = 100.0f;
            State::Instance().lastMipBiasMax = -100.0f;
        }
    }

    if (_visibleChild)
    {
        _visibleChild->Hide();
        return realResult; // child HRESULT was logged separately; native chain is authoritative
    }
    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (_nativeParentRoute)
    {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){_SuspendUnobservedNativeDisplay();return _real ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED;}
        _RevokeNativeChild(true);_TryDrainNativeChild();
        return _real ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED;
    }
    return _fgSwapChain != nullptr
               ? _fgSwapChain->ResizeTarget(pNewTargetParameters)
               : (_real != nullptr ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetContainingOutput(IDXGIOutput** ppOutput)
{
    if (_nativeParentRoute) return _real->GetContainingOutput(ppOutput);
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetContainingOutput(ppOutput)
               : (_real != nullptr ? _real->GetContainingOutput(ppOutput) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    if (_nativeParentRoute) return _real->GetFrameStatistics(pStats);
    return _fgSwapChain != nullptr ? _fgSwapChain->GetFrameStatistics(pStats)
                                   : (_real != nullptr ? _real->GetFrameStatistics(pStats) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetLastPresentCount(UINT* pLastPresentCount)
{
    if (_nativeParentRoute) return _real->GetLastPresentCount(pLastPresentCount);
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetLastPresentCount(pLastPresentCount)
               : (_real != nullptr ? _real->GetLastPresentCount(pLastPresentCount) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1 != nullptr ? _real1->GetDesc1(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    if (_nativeParentRoute) return _real1 ? _real1->GetFullscreenDesc(pDesc) : E_NOINTERFACE;
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenDesc(pDesc);

    return _real1 != nullptr ? _real1->GetFullscreenDesc(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetHwnd(HWND* pHwnd)
{
    if (pHwnd == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    *pHwnd = _handle;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1 != nullptr ? _real1->GetCoreWindow(refiid, ppUnk) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present1(UINT SyncInterval, UINT Flags,
                                                const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (_nativeParentRoute) return _PresentNativeVisible(SyncInterval, Flags, pPresentParameters);
    UNREFERENCED_PARAMETER(pPresentParameters);
    return Present(SyncInterval, Flags);
}

BOOL STDMETHODCALLTYPE Dx11wDx12SC::IsTemporaryMonoSupported(void)
{
    return _real1 != nullptr ? _real1->IsTemporaryMonoSupported() : FALSE;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1 != nullptr ? _real1->GetRestrictToOutput(ppRestrictToOutput) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->SetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->GetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (_nativeParentRoute)
    {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){_SuspendUnobservedNativeDisplay();return _real1 ? _real1->SetRotation(Rotation) : E_NOINTERFACE;}
        DXGI_MODE_ROTATION current{};
        const bool sameIdentity = Rotation == DXGI_MODE_ROTATION_IDENTITY && _real1 &&
            SUCCEEDED(_real1->GetRotation(&current)) && current == Rotation;
        if (!sameIdentity) { _RevokeNativeChild(true); _TryDrainNativeChild(); }
        _nativeDefaultRotation=false;
        const auto hr = _real1 ? _real1->SetRotation(Rotation) : E_NOINTERFACE;
        if (FAILED(hr)) _RevokeNativeChild(true);
        return hr;
    }
    return _real1 != nullptr ? _real1->SetRotation(Rotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1 != nullptr ? _real1->GetRotation(pRotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetSourceSize(UINT Width, UINT Height)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (_nativeParentRoute)
    {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){_SuspendUnobservedNativeDisplay();return _real2 ? _real2->SetSourceSize(Width,Height) : E_NOINTERFACE;}
        UINT currentWidth = 0, currentHeight = 0;
        const bool sameSize = _real2 && SUCCEEDED(_real2->GetSourceSize(&currentWidth,&currentHeight)) &&
            Width == currentWidth && Height == currentHeight;
        if (!sameSize) { _RevokeNativeChild(true); _TryDrainNativeChild(); }
        _nativeDefaultSourceSize=false;
        const auto hr = _real2 ? _real2->SetSourceSize(Width,Height) : E_NOINTERFACE;
        if (FAILED(hr)) _RevokeNativeChild(true);
        return hr;
    }
    return _real2 != nullptr ? _real2->SetSourceSize(Width, Height) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2 != nullptr ? _real2->GetSourceSize(pWidth, pHeight) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMaximumFrameLatency(UINT MaxLatency)
{
    const auto target = _SelectFrameLatencyTarget(_ReturnAddress());
    return framegen::latency::Forward<IDXGISwapChain2, HRESULT>(target, _real2, _fgSwapChain,
        [MaxLatency](auto& chain) { return chain.SetMaximumFrameLatency(MaxLatency); },
        DXGI_ERROR_INVALID_CALL, _nativeParentRoute ? E_NOINTERFACE : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    const auto target = _SelectFrameLatencyTarget(_ReturnAddress());
    return framegen::latency::Forward<IDXGISwapChain2, HRESULT>(target, _real2, _fgSwapChain,
        [pMaxLatency](auto& chain) { return chain.GetMaximumFrameLatency(pMaxLatency); },
        DXGI_ERROR_INVALID_CALL, _nativeParentRoute ? E_NOINTERFACE : DXGI_ERROR_DEVICE_REMOVED);
}

void Dx11wDx12SC::_RefreshFrameLatencyFlags()
{
    DXGI_SWAP_CHAIN_DESC native {}, provider {};
    const bool nativeKnown=_real && SUCCEEDED(_real->GetDesc(&native));
    const bool providerKnown=_fgSwapChain && SUCCEEDED(_fgSwapChain->GetDesc(&provider));
    std::lock_guard lock(_frameLatencyMutex);
    _frameLatencyFlags.nativeKnown=nativeKnown;
    _frameLatencyFlags.effectiveNative=nativeKnown ? native.Flags : 0;
    _frameLatencyFlags.providerKnown=providerKnown;
    _frameLatencyFlags.effectiveProvider=providerKnown ? provider.Flags : 0;
}

HANDLE STDMETHODCALLTYPE Dx11wDx12SC::GetFrameLatencyWaitableObject(void)
{
    const auto target = _SelectFrameLatencyTarget(_ReturnAddress());
    return framegen::latency::Forward<IDXGISwapChain2, HANDLE>(target, _real2, _fgSwapChain,
        [](auto& chain) { return chain.GetFrameLatencyWaitableObject(); }, nullptr, nullptr);
}

framegen::latency::Target Dx11wDx12SC::_SelectFrameLatencyTarget(void* returnAddress)
{
    const auto caller = StreamlineHooks::FrameLatencyCaller(returnAddress);
    const bool sdkLoaded = StreamlineHooks::DlssgPluginLoaded();
    framegen::latency::Flags flags;
    { std::lock_guard lock(_frameLatencyMutex);flags=_frameLatencyFlags; }
    const auto target = framegen::latency::Select(flags, caller, sdkLoaded,
        _real2 != nullptr, _fgSwapChain != nullptr, _nativeParentRoute);
    StreamlineHooks::ReportFrameLatencyAccess(this, flags, caller, sdkLoaded, target);
    const auto preserved = !_nativeParentRoute && (!_sourceFrameLatencyOwner || _starRailF5DsTransport) &&
        _fgSwapChain != nullptr ? framegen::latency::Target::Provider : framegen::latency::Target::Native;
    return framegen::latency::Resolve(target, preserved);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (_nativeParentRoute) {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){_SuspendUnobservedNativeDisplay();return _real2 ? _real2->SetMatrixTransform(pMatrix) : E_NOINTERFACE;}
        const bool identity = pMatrix && pMatrix->_11 == 1.0f && pMatrix->_12 == 0.0f &&
            pMatrix->_21 == 0.0f && pMatrix->_22 == 1.0f && pMatrix->_31 == 0.0f && pMatrix->_32 == 0.0f;
        if (!identity || !_nativeMatrixIdentity) { _RevokeNativeChild(true); _TryDrainNativeChild(); }
        const auto callSerial = _childDisplaySerial == UINT64_MAX ? UINT64_MAX : ++_childDisplaySerial;
        if (callSerial == UINT64_MAX) _RevokeNativeChild();
        const auto hr = _real2 ? _real2->SetMatrixTransform(pMatrix) : E_NOINTERFACE;
        if (hr == S_OK && callSerial != UINT64_MAX && callSerial == _childDisplaySerial)
            _nativeMatrixIdentity = identity;
        else
        {
            _nativeMatrixIdentity = false;
            _RevokeNativeChild(true);
            _nativeMatrixIdentity = false; // RequestHide may itself reenter a setter
        }
        return hr;
    }
    return _real2 != nullptr ? _real2->SetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->GetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

UINT STDMETHODCALLTYPE Dx11wDx12SC::GetCurrentBackBufferIndex(void)
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _currentFakeIndex;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookCheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                              UINT* pColorSpaceSupport)
{
    if (_nativeParentRoute) return _real3 ? _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport) : E_NOINTERFACE;
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);

    return _real3 != nullptr ? _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport)
                             : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookSetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    if (_nativeParentRoute) {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){_SuspendUnobservedNativeDisplay();return _real3 ? _real3->SetColorSpace1(ColorSpace) : E_NOINTERFACE;}
        const bool sameSdr = _nativeColorSpaceKnown && _nativeColorSpace == ColorSpace &&
            ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if (!sameSdr) { _RevokeNativeChild(true); _TryDrainNativeChild(); }
        // Even redundant SDR calls need an observation serial: a nested
        // setter can change display state without the outer call revoking it.
        const auto callSerial = _childDisplaySerial == UINT64_MAX ? UINT64_MAX : ++_childDisplaySerial;
        if (callSerial == UINT64_MAX) _RevokeNativeChild();
        const auto hr=_real3 ? _real3->SetColorSpace1(ColorSpace) : E_NOINTERFACE;
        if (hr == S_OK && callSerial != UINT64_MAX && callSerial == _childDisplaySerial)
        {
            GenshinNativeParentFrame::PublishColorSpace(_real,ColorSpace);
            // Publication also calls native SetPrivateData; do not overwrite
            // a nested observation or rearm recovery if it was superseded.
            if (callSerial == _childDisplaySerial)
            {
                _nativeColorSpace = ColorSpace;
                _nativeColorSpaceKnown = true;
                if (_childRevoked && _childRecoveryAllowed) _childRecoveryAttempted = false;
                return hr;
            }
        }
        _nativeColorSpaceKnown = false;
        _RevokeNativeChild(true);
        _nativeColorSpaceKnown = false; // RequestHide may itself reenter a setter
        return hr;
    }
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetColorSpace1(ColorSpace);

    return _real3 != nullptr ? _real3->SetColorSpace1(ColorSpace) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                      UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                      IUnknown* const* ppPresentQueue)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (_nativeParentRoute)
    {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){if(_visibleChild)_visibleChild->RequestHide();return DXGI_ERROR_WAS_STILL_DRAWING;}
        _RevokeNativeChild(true);
        _TryDrainNativeChild(); // failure retains private child owners only
        DlssNrCurrentOutput::Capture::ResizeScope capture(DlssNrCurrentOutput::Capture::ResizeScope::Mode::ReserveOnly);
        if(!capture)return DXGI_ERROR_WAS_STILL_DRAWING;
        const auto before=GenshinLegacy::BeforeResize(_real);
        if(FAILED(before))return before;
        if(!DlssNr::WaitForFinishedPicture() || !capture.ReadyForResize())return DXGI_ERROR_WAS_STILL_DRAWING;
        MenuOverlayDx::CleanupRenderTarget(true,_handle);
        const auto hr=_real ? (_real3 ? _real3->ResizeBuffers1(BufferCount,Width,Height,Format,SwapChainFlags,pCreationNodeMask,ppPresentQueue) :
            _real->ResizeBuffers(BufferCount,Width,Height,Format,SwapChainFlags)) : DXGI_ERROR_DEVICE_REMOVED;
        if(SUCCEEDED(hr)){
            if (Format != DXGI_FORMAT_UNKNOWN && Format != _bufferFormat) _nativeColorSpaceKnown = false;
            _RefreshCachedSwapchainDesc();_RefreshFrameLatencyFlags();State::Instance().scChanged=true;
            State::Instance().SCAllowTearing=(SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)!=0;}
        return hr;
    }

    if (_real3 == nullptr) return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);

    if (const auto failure = _fgResizeStatus.RecoveryFailure(Config::Instance()->FGSkipResizeBuffers.value_or_default());
        FAILED(failure)) return failure;

    // Reserve capture admission BEFORE late Reset, exactly as R610. The
    // reservation outlives both actual native and child resize calls.
    std::optional<DlssNrCurrentOutput::Capture::ResizeScope> outputCaptureResize;
    if (_visibleChild) {
        outputCaptureResize.emplace(DlssNrCurrentOutput::Capture::ResizeScope::Mode::ReserveOnly);
        if (!*outputCaptureResize) return DXGI_ERROR_WAS_STILL_DRAWING;
        const auto captureResize=GenshinLegacy::BeforeResize(_real);
        if (FAILED(captureResize)) return captureResize;
        if (!DlssNr::WaitForFinishedPicture() || !outputCaptureResize->ReadyForResize())
            return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    GenshinBridgeFg::Revoke();
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Resize, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);


    auto lifecycleOwner = State::Instance().currentFG;
    auto lifecycleExecution = lifecycleOwner != nullptr ? lifecycleOwner->GuardExecution(FGExecutionScope::Lifecycle) : FGExecutionGuard {};
    auto fgArgs = ResizeArgsForFg(_fgSwapChain, BufferCount, Format, SwapChainFlags);
    if (_visibleChild) fgArgs.flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    const bool skipFgResize = _fgResizeStatus.CanSkipResize() &&
        IsSame(_fgSwapChain, fgArgs.bufferCount, Width, Height, fgArgs.format, fgArgs.flags);
    if (!skipFgResize && lifecycleOwner != nullptr && !lifecycleOwner->PrepareForSwapchainChange())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    GenshinBridgeFg::Collect(); // successful provider drain receipts can release both backbuffers before Resize

    const bool synchronizeXeFGPresent = skipFgResize && State::Instance().activeFgOutput == FGOutput::XeFG &&
                                        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;

    std::unique_lock<std::shared_mutex> presentResizeLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (synchronizeXeFGPresent)
    {
        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
        {
            State::Instance().fgChanged = true;
            fg->UpdateTarget();
            fg->Deactivate();
        }

        LOG_DEBUG("Waiting for XeFG native presents before equivalent ResizeBuffers1");
        CallerCpuTrace::Scope resizeMutexCpu(CallerCpuTrace::Op::ResizeMutexWait, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
        presentResizeLock.lock();
        resizeMutexCpu.Finish();
        LOG_DEBUG("XeFG native present barrier acquired");
    }

    if (!_WaitForCopyQueueIdle())
    {
        LOG_WARN("ResizeBuffers1 retained old owners: interop drain incomplete");
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    if (synchronizeXeFGPresent && !WaitForQueueIdle(_dx12CommandQueue, _copyFence, _copyFenceEvent, _copyFenceValue))
    {
        LOG_WARN("ResizeBuffers1 retained old owners: FG/present wait incomplete");
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropBackBuffers();

    HRESULT realResult =
        _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (_visibleChild && SUCCEEDED(realResult)) _visibleChild->Prepare();
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        // The game's ppPresentQueue is not valid for the DX12 FG swapchain. Use ResizeBuffers for phase 1.
        if (skipFgResize)
        {
            LOG_DEBUG("Skipping FG ResizeBuffers1");
            fgResult = S_OK;
        }
        else
        {
            UINT childWidth = Width, childHeight = Height;
            if (_visibleChild)
            {
                DXGI_SWAP_CHAIN_DESC actual{};
                if (SUCCEEDED(_real->GetDesc(&actual)))
                { childWidth = actual.BufferDesc.Width; childHeight = actual.BufferDesc.Height; }
            }
            fgResult = _fgSwapChain->ResizeBuffers(fgArgs.bufferCount, childWidth, childHeight, fgArgs.format, fgArgs.flags);
        }
    }

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1 results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    _RefreshFrameLatencyFlags();
    _fgResizeStatus.Complete(realResult, fgResult, !skipFgResize && _fgSwapChain != nullptr);
    if (SUCCEEDED(realResult) && FAILED(fgResult))
        LOG_ERROR("DX12 presenter resize failed: {:X}; interop Present blocked until successful resize. Hidden DX11 source is not a visible fallback",
                  (UINT) fgResult);

    if (SUCCEEDED(realResult))
    {
        _RefreshCachedSwapchainDesc();

        if (SUCCEEDED(fgResult) && Config::Instance()->FGEnabled.value_or_default())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    }

    if (_visibleChild)
    {
        _visibleChild->Hide();
        return realResult; // child HRESULT was logged separately; native chain is authoritative
    }
    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookSetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (_nativeParentRoute) {
        NativeCall call(*this);
        std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
        if(!nativeLock.owns_lock()){_SuspendUnobservedNativeDisplay();return _real4 ? _real4->SetHDRMetaData(Type,Size,pMetaData) : E_NOINTERFACE;}
        const bool absent = Type == DXGI_HDR_METADATA_TYPE_NONE && Size == 0 && !pMetaData;
        if (!absent || !_nativeHdrMetadataAbsent) { _RevokeNativeChild(true); _TryDrainNativeChild(); }
        const auto callSerial = _childDisplaySerial == UINT64_MAX ? UINT64_MAX : ++_childDisplaySerial;
        if (callSerial == UINT64_MAX) _RevokeNativeChild();
        const auto hr = _real4 ? _real4->SetHDRMetaData(Type,Size,pMetaData) : E_NOINTERFACE;
        if (hr == S_OK && callSerial != UINT64_MAX && callSerial == _childDisplaySerial)
            _nativeHdrMetadataAbsent = absent;
        else
        {
            _nativeHdrMetadataAbsent = false;
            _RevokeNativeChild(true);
            _nativeHdrMetadataAbsent = false; // RequestHide may itself reenter a setter
        }
        return hr;
    }
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetHDRMetaData(Type, Size, pMetaData);

    return _real4 != nullptr ? _real4->SetHDRMetaData(Type, Size, pMetaData) : DXGI_ERROR_DEVICE_REMOVED;
}

bool Dx11wDx12SC::_InitInteropObjects()
{
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return false;
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::InteropInit, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (_interopInitialized)
    {
        _RefreshCachedSwapchainDesc();

        if (_bufferCount == 0)
        {
            LOG_ERROR("InitInteropObjects resolved zero backbuffers");
            return false;
        }

        const bool genshinPrivateRing = _nativeParentRoute && GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
        const UINT colorSlotCount = PrivateColorSlotCount(genshinPrivateRing, _bufferCount, _copyAllocatorFenceValues);
        if (_sharedDx11BackBufferCopies.size() != colorSlotCount)
            _sharedDx11BackBufferCopies.resize(colorSlotCount, nullptr);
        _sharedBackBufferIsReturnedSnapshot.resize(colorSlotCount, false);

        if (_openedDx11BackBuffers.size() != colorSlotCount)
            _openedDx11BackBuffers.resize(colorSlotCount, nullptr);

        if (_sharedBackBufferHandles.size() != colorSlotCount)
            _sharedBackBufferHandles.resize(colorSlotCount, nullptr);

        if (_openedDx11BackBufferStates.size() != colorSlotCount)
            _openedDx11BackBufferStates.resize(colorSlotCount, D3D12_RESOURCE_STATE_COMMON);

        return true;
    }

    if (_dx11Device == nullptr || _dx11Device5 == nullptr || _dx11Context4 == nullptr || _dx12Device == nullptr ||
        _dx12CommandQueue == nullptr)
    {
        LOG_ERROR("interop objects missing: dx11 {}, dx11-5 {}, ctx4 {}, dx12 {}, queue {}", (UINT64) _dx11Device,
                  (UINT64) _dx11Device5, (UINT64) _dx11Context4, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
        return false;
    }

    HRESULT result = S_OK;

    const UINT copyAllocatorCount = std::max<UINT>(_bufferCount != 0 ? _bufferCount : 3, 3);

    if (_copyAllocators.size() != copyAllocatorCount)
    {
        for (auto& allocator : _copyAllocators)
            SafeRelease(allocator);

        _copyAllocators.assign(copyAllocatorCount, nullptr);
    }

    if (_copyAllocatorFenceValues.size() != copyAllocatorCount)
        _copyAllocatorFenceValues.assign(copyAllocatorCount, 0);

    if (_copyCommandLists.size() != copyAllocatorCount)
    {
        for (auto& commandList : _copyCommandLists)
            SafeRelease(commandList);

        _copyCommandLists.assign(copyAllocatorCount, nullptr);
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyAllocators[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_copyAllocators[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandAllocator[{}] failed: {:X}", i, (UINT) result);
            return false;
        }
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyCommandLists[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _copyAllocators[i], nullptr,
                                                IID_PPV_ARGS(&_copyCommandLists[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandList failed: {:X}", (UINT) result);
            return false;
        }

        _copyCommandLists[i]->Close();
    }

    if (_copyFence == nullptr)
    {
        result = _dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_copyFence));
        if (FAILED(result))
        {
            LOG_ERROR("Create copy fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_copyFenceEvent == nullptr)
    {
        _copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (_copyFenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent for copy fence failed");
            return false;
        }
    }

    if (_dx11Fence == nullptr)
    {
        result = _dx11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_dx11Fence));
        if (FAILED(result))
        {
            LOG_ERROR("D3D11 CreateFence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_sharedFenceHandle == nullptr)
    {
        result = _dx11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_sharedFenceHandle);
        if (FAILED(result))
        {
            LOG_ERROR("CreateSharedHandle for D3D11 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_dx12SharedFence == nullptr)
    {
        result = _dx12Device->OpenSharedHandle(_sharedFenceHandle, IID_PPV_ARGS(&_dx12SharedFence));
        if (FAILED(result))
        {
            LOG_ERROR("OpenSharedHandle for D3D12 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    _RefreshCachedSwapchainDesc();

    if (_bufferCount == 0)
    {
        LOG_ERROR("InitInteropObjects resolved zero backbuffers");
        return false;
    }

    const bool genshinPrivateRing = _nativeParentRoute && GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    const UINT colorSlotCount = PrivateColorSlotCount(genshinPrivateRing, _bufferCount, _copyAllocatorFenceValues);
    _sharedDx11BackBufferCopies.assign(colorSlotCount, nullptr);
    _sharedBackBufferIsReturnedSnapshot.assign(colorSlotCount, false);
    _openedDx11BackBuffers.assign(colorSlotCount, nullptr);
    _sharedBackBufferHandles.assign(colorSlotCount, nullptr);
    _openedDx11BackBufferStates.assign(colorSlotCount, D3D12_RESOURCE_STATE_COMMON);

    _interopInitialized = true;
    return true;
}

bool Dx11wDx12SC::_EnsureNativeInputCopyRing()
{
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return false;
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::NativeRingInit, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    // Lazy and separate from _InitInteropObjects: every D3D11+FG-interop user pays for that ring, but
    // this one is only worth creating -- and only worth the extra GPU objects and failure surface -- once
    // native input has actually produced a frame to composite.
    if (_dx12Device == nullptr || _bufferCount == 0)
        return false;

    const UINT copyAllocatorCount = std::max<UINT>(_bufferCount, 3);

    if (_nrCopyAllocators.size() != copyAllocatorCount)
    {
        for (auto& allocator : _nrCopyAllocators)
            SafeRelease(allocator);

        _nrCopyAllocators.assign(copyAllocatorCount, nullptr);
    }

    if (_nrCopyAllocatorFenceValues.size() != copyAllocatorCount)
        _nrCopyAllocatorFenceValues.assign(copyAllocatorCount, 0);

    if (_nrCopyCommandLists.size() != copyAllocatorCount)
    {
        for (auto& commandList : _nrCopyCommandLists)
            SafeRelease(commandList);

        _nrCopyCommandLists.assign(copyAllocatorCount, nullptr);
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_nrCopyAllocators[i] != nullptr)
            continue;

        auto result =
            _dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_nrCopyAllocators[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandAllocator (native input copy)[{}] failed: {:X}", i, (UINT) result);
            return false;
        }
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_nrCopyCommandLists[i] != nullptr)
            continue;

        auto result = _dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _nrCopyAllocators[i],
                                                      nullptr, IID_PPV_ARGS(&_nrCopyCommandLists[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandList (native input copy) failed: {:X}", (UINT) result);
            return false;
        }

        _nrCopyCommandLists[i]->Close();
    }

    return true;
}

bool Dx11wDx12SC::_CanReuseReturnedSnapshot() const
{
    auto* scope = GenshinBridgeFg::PresentScope::Current();
    return _nativeParentRoute && GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
        scope && scope->MatchesNative(_real, _dx11Device) && _nativeOriginal &&
        GenshinBridgeFg::Identity(scope->ReturnedSnapshot()) == GenshinBridgeFg::Identity(_nativeOriginal.Get()) &&
        GenshinPresentNr::SharedSnapshotSelected();
}

bool Dx11wDx12SC::_RequestSharedBackBuffer(UINT index, bool reuseReturnedSnapshot)
{
    const UINT copySlot=ResolvePrivateCopySlot(this,_currentFakeIndex);
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::SharedBuffer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (_real == nullptr || _dx11Device == nullptr || _dx12Device == nullptr)
        return RecordPrivateCopyFailure(this, "SharedDevicesUnavailable");

    if (_bufferCount == 0)
        _RefreshCachedSwapchainDesc();

    if (index >= _bufferCount)
    {
        LOG_ERROR("backbuffer index {} out of range {}", index, _bufferCount);
        return RecordPrivateCopyFailure(this, "GameBufferIndexOutOfRange");
    }

    const bool genshinImport = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    const bool genshinPrivateRing = _nativeParentRoute && genshinImport;
    const UINT colorSlotCount = PrivateColorSlotCount(genshinPrivateRing, _bufferCount, _copyAllocatorFenceValues);
    if (copySlot >= colorSlotCount)
        return RecordPrivateCopyFailure(this, "PrivateColorSlotOutOfRange");
    const UINT requiredSlots = genshinPrivateRing ? colorSlotCount : index + 1;

    if (_sharedDx11BackBufferCopies.size() < requiredSlots)
        _sharedDx11BackBufferCopies.resize(colorSlotCount, nullptr);
    _sharedBackBufferIsReturnedSnapshot.resize(colorSlotCount, false);

    if (_openedDx11BackBuffers.size() < requiredSlots)
        _openedDx11BackBuffers.resize(colorSlotCount, nullptr);

    if (_sharedBackBufferHandles.size() < requiredSlots)
        _sharedBackBufferHandles.resize(colorSlotCount, nullptr);

    // Bind on the original successful open even when FG is disabled/paused.
    // Enabling later must not leave every cached ordinary slot unauthenticated.
    // This is already the real DX11->DX12 wrapper open operation. Do not gate
    // its association on mutable global provider eligibility or interop state.
    if (genshinImport && _genshinFgImports.size() < requiredSlots)
    {
        try { _genshinFgImports.resize(colorSlotCount); }
        catch (const std::bad_alloc&) {} // optional FG metadata; ordinary open remains available
    }

    if (_openedDx11BackBufferStates.size() < requiredSlots)
        _openedDx11BackBufferStates.resize(colorSlotCount, D3D12_RESOURCE_STATE_COMMON);

    const auto slotRetired = [&] {
        if (copySlot >= _copyAllocatorFenceValues.size()) return false;
        const auto pending = _copyAllocatorFenceValues[copySlot];
        if (!pending) return true;
        if (!_copyFence || pending == UINT64_MAX) return false;
        const auto completed = _copyFence->GetCompletedValue();
        return completed != UINT64_MAX && completed >= pending;
    };
    const auto scopeLeaseForSnapshot = [&] {
        auto* scope=GenshinBridgeFg::PresentScope::Current();
        return _nativeParentRoute && scope && scope->ReturnedSnapshot()==_nativeOriginal.Get()
            ? scope->ReturnedSnapshotLease() : std::shared_ptr<GenshinPresentNr::SnapshotLease>{};
    };
    const auto recordSnapshotImport = [&](bool reused) {
        static thread_local uint64_t opens=0,reuses=0;
        const auto count=reused?++reuses:++opens;
        if(count==1 || (opens+reuses)%512==0)
            LOG_INFO("R352 NR snapshot association: pid={} frame={} copy_slot={} export_open_reused={} successful_exports_opens={} association_reuses={} performance=UNKNOWN",
                GetCurrentProcessId(),State::Instance().frameCount,copySlot,reused,opens,reuses);
    };
    const auto releaseBinding = [&] {
        // Replacing a slot never releases its in-flight copy owners. The FG
        // Frame separately retains sharedColor11/importedColor until retirement.
        SafeRelease(_openedDx11BackBuffers[copySlot]);
        SafeRelease(_sharedDx11BackBufferCopies[copySlot]);
        SafeCloseHandle(_sharedBackBufferHandles[copySlot]);
        if (copySlot < _genshinFgImports.size()) _genshinFgImports[copySlot].reset();
        _sharedBackBufferIsReturnedSnapshot[copySlot] = false;
        _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;
    };
    if (reuseReturnedSnapshot && _CanReuseReturnedSnapshot() &&
        copySlot < _genshinFgImports.size() && slotRetired())
    {
        // This is the actual immutable snapshot, not a same-sized native B or
        // a previous picture. Opening it removes only snapshot -> shadow copy.
        auto* snapshot = _nativeOriginal.Get();
        auto lease = scopeLeaseForSnapshot();
        if (lease)
        {
            auto imported = lease->CachedImport(_dx12Device);
            if (imported && imported->Matches(snapshot, imported->Resource()))
            {
                releaseBinding();
                snapshot->AddRef();
                _sharedDx11BackBufferCopies[copySlot] = snapshot;
                auto* opened = imported->Resource();opened->AddRef();
                _openedDx11BackBuffers[copySlot] = opened;
                _genshinFgImports[copySlot] = std::move(imported);
                _sharedBackBufferIsReturnedSnapshot[copySlot] = true;
                recordSnapshotImport(true);
                return true;
            }
        }
        D3D11_TEXTURE2D_DESC desc {};
        snapshot->GetDesc(&desc);
        ComPtr<ID3D11Device> sourceDevice;
        snapshot->GetDevice(&sourceDevice);
        const UINT shareFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        ComPtr<IDXGIResource1> exported;
        HANDLE handle = nullptr;
        if ((desc.MiscFlags & shareFlags) == shareFlags &&
            GenshinBridgeFg::Identity(sourceDevice.Get()) == GenshinBridgeFg::Identity(_dx11Device) &&
            SUCCEEDED(snapshot->QueryInterface(IID_PPV_ARGS(&exported))) &&
            SUCCEEDED(exported->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &handle)))
        {
            ComPtr<ID3D12Resource> opened;
            HRESULT openedResult = E_FAIL;
            auto imported = GenshinBridgeFg::ColorImport::Open(_dx12Device, snapshot, handle,
                                                              opened.GetAddressOf(), openedResult);
            // The COM resources own the heap; the OS export is needed only for
            // OpenSharedHandle. No disk handle or CPU-scope lifetime substitutes.
            SafeCloseHandle(handle);
            D3D12_HEAP_PROPERTIES heap {};
            D3D12_HEAP_FLAGS heapFlags {};
            if (SUCCEEDED(openedResult) && opened && imported &&
                GenshinBridgeFg::OnDevice(opened.Get(), _dx12Device) &&
                GenshinBridgeFg::SdrCopy11To12(desc, opened->GetDesc()) &&
                desc.Format == opened->GetDesc().Format &&
                SUCCEEDED(opened->GetHeapProperties(&heap, &heapFlags)) &&
                (heapFlags & D3D12_HEAP_FLAG_SHARED))
            {
                releaseBinding();
                snapshot->AddRef();
                _sharedDx11BackBufferCopies[copySlot] = snapshot;
                _openedDx11BackBuffers[copySlot] = opened.Detach();
                if (lease)lease->CacheImport(_dx12Device, imported);
                _genshinFgImports[copySlot] = std::move(imported);
                _sharedBackBufferIsReturnedSnapshot[copySlot] = true;
                recordSnapshotImport(false);
                return true;
            }
        }
        SafeCloseHandle(handle);
        // Export/import/association failures recorded no GPU work. Continue
        // through the ordinary shared texture plus full-copy path below.
    }
    if (_sharedBackBufferIsReturnedSnapshot[copySlot])
    {
        if (!slotRetired()) return RecordPrivateCopyFailure(this, "SnapshotImportStillInFlight");
        releaseBinding();
    }
    if (_openedDx11BackBuffers[copySlot] != nullptr)
        return true;

    // Read Dx11 sc backbuffer
    ID3D11Texture2D* sourceTexture = nullptr;
    CallerCpuTrace::Scope getBufferCpu(CallerCpuTrace::Op::SharedGetBuffer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, index);
    auto result = _nativeParentRoute
        ? (_nativeOriginal ? _nativeOriginal->QueryInterface(IID_PPV_ARGS(&sourceTexture)) : E_FAIL)
        : _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    getBufferCpu.Finish();
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer({}) failed: {:X}", index, (UINT) result);
        return RecordPrivateCopyFailure(this, "SharedSourceGetBufferFailed", result);
    }

    // Create shared copy
    IDXGIResource1* dxgiResource = nullptr;
    if (_sharedBackBufferHandles[copySlot] == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        sourceTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        // A game with MSAA on has a multisampled swap chain (_real keeps the samples it asked for); the D3D12 side cannot
        // open or present a multisampled texture, so the copy is single-sample and the samples are resolved into it
        // (_CopyDx11BackBufferToShared).
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;

        CallerCpuTrace::Scope createTextureCpu(CallerCpuTrace::Op::SharedCreateTexture, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
        result = _dx11Device->CreateTexture2D(&desc, nullptr, &_sharedDx11BackBufferCopies[copySlot]);
        createTextureCpu.Finish();
        if (FAILED(result) || _sharedDx11BackBufferCopies[copySlot] == nullptr)
        {
            LOG_ERROR("CreateTexture2D shadow buffer {} failed: {:X}", copySlot, (UINT) result);
            sourceTexture->Release();
            return RecordPrivateCopyFailure(this, "SharedTextureCreateFailed", result);
        }

        result = _sharedDx11BackBufferCopies[copySlot]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
        if (FAILED(result) || dxgiResource == nullptr)
        {
            LOG_ERROR("shadow IDXGIResource1 query {} failed: {:X}", copySlot, (UINT) result);
            sourceTexture->Release();
            return RecordPrivateCopyFailure(this, "SharedResourceQueryFailed", result);
        }

        CallerCpuTrace::Scope createHandleCpu(CallerCpuTrace::Op::SharedCreateHandle, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
        result = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                  &_sharedBackBufferHandles[copySlot]);
        createHandleCpu.Finish();
        dxgiResource->Release();

        if (FAILED(result) || _sharedBackBufferHandles[copySlot] == nullptr)
        {
            LOG_ERROR("shadow CreateSharedHandle {} failed: {:X}", copySlot, (UINT) result);
            sourceTexture->Release();
            return RecordPrivateCopyFailure(this, "SharedHandleCreateFailed", result);
        }
    }

    // Dx12 handle to Dx11 shared texture
    CallerCpuTrace::Scope openHandleCpu(CallerCpuTrace::Op::SharedOpenHandle, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
    if (genshinImport && copySlot < _genshinFgImports.size())
    {
        _genshinFgImports[copySlot] = GenshinBridgeFg::ColorImport::Open(
            _dx12Device, _sharedDx11BackBufferCopies[copySlot],
            _sharedBackBufferHandles[copySlot], &_openedDx11BackBuffers[copySlot], result);
        // result describes the ordinary OpenSharedHandle only. A missing
        // association refuses optional FG without failing a valid interop copy.
    }
    else
        result = _dx12Device->OpenSharedHandle(_sharedBackBufferHandles[copySlot],
                                               IID_PPV_ARGS(&_openedDx11BackBuffers[copySlot]));
    openHandleCpu.Finish();
    sourceTexture->Release();

    if (FAILED(result) || _openedDx11BackBuffers[copySlot] == nullptr)
    {
        // A device-removed result here never recovers by itself; log the actual reason once so it's diagnosable
        // without flooding the log on every subsequent frame (this failure repeats identically otherwise).
        if (!_deviceRemovedLogged)
        {
            _deviceRemovedLogged = true;
            LOG_ERROR("OpenSharedHandle for backbuffer {} failed: {:X}", copySlot, (UINT) result);

            if (_dx12Device != nullptr)
                LOG_ERROR("dx12 device removed reason: {:X}", (UINT) _dx12Device->GetDeviceRemovedReason());

            if (_dx11Device != nullptr)
                LOG_ERROR("dx11 device removed reason: {:X}", (UINT) _dx11Device->GetDeviceRemovedReason());
        }

        return RecordPrivateCopyFailure(this, "SharedHandleOpenFailed", result);
    }

    _deviceRemovedLogged = false;

    _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool Dx11wDx12SC::_CopyDx11BackBufferToShared(UINT index, CopySlotAdmission& admission)
{
    admission.Clear();
    const UINT copySlot=ResolvePrivateCopySlot(this,_currentFakeIndex);
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Dx11Copy, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (copySlot >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[copySlot] == nullptr)
    {
        return RecordPrivateCopyFailure(this, "SharedTextureUnavailable");
    }

    if (_dx11Context == nullptr || _real == nullptr)
        return RecordPrivateCopyFailure(this, "Dx11CopyContextUnavailable");

    // Keep StarRail's source timeline boundary separate from common GPU reuse.
    if (_sourceFrameLatencyOwner)
    {
        if (_sourceCopyUncertain.load(std::memory_order_acquire) || _sharedFenceValue >= UINT64_MAX - 1 || _copyFenceValue >= UINT64_MAX - 1)
            return RecordPrivateCopyFailure(this, "SourceCopyTimelineUnavailable");
    }

    // Interop shared writer admission begin
    // Move the existing allocator retirement BEFORE the shared texture write.
    // Both name this exact slot's previous D3D12 reader. The native-parent
    // optional route keeps its nonblocking Busy policy inside the same helper.
    // The local proof is consumed at Reset below; no second completion wait.
    if (_sourceCopyUncertain.load(std::memory_order_acquire) || !_copyFence || !_copyFenceEvent ||
        copySlot >= _copyAllocators.size() || !_copyAllocators[copySlot] ||
        !_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues))
        return RecordPrivateCopyFailure(this, "CopyReaderNotRetired");
    admission.Record(this, &_copyAllocatorFenceValues, _copyFence, _copyAllocators[copySlot],
                     copySlot, _copyAllocatorFenceValues[copySlot]);
    // Interop shared writer admission end

    // Re-read selected consumers at the real copy boundary, including changes
    // since import. Restore a distinct shadow target before the original copy.
    if (_sharedBackBufferIsReturnedSnapshot[copySlot] && !_CanReuseReturnedSnapshot() &&
        !_RequestSharedBackBuffer(index, false))
        return false;

    ID3D11Texture2D* sourceTexture = nullptr;
    CallerCpuTrace::Scope getBufferCpu(CallerCpuTrace::Op::Dx11CopyGetBuffer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, index);
    auto result = _nativeParentRoute
        ? (_nativeOriginal ? _nativeOriginal->QueryInterface(IID_PPV_ARGS(&sourceTexture)) : E_FAIL)
        : _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    getBufferCpu.Finish();
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer for shadow copy {} failed: {:X}", index, (UINT) result);
        return RecordPrivateCopyFailure(this, "Dx11SourceGetBufferFailed", result);
    }

    LOG_DEBUG("Copying DX11 backbuffer {} sourceTexture: {:X} to shadow copy {:X}", index, (size_t) sourceTexture,
              (size_t) _sharedDx11BackBufferCopies[copySlot]);

    D3D11_TEXTURE2D_DESC sourceDesc {};
    sourceTexture->GetDesc(&sourceDesc);

    if(_nativeParentRoute) {
        D3D11_TEXTURE2D_DESC target{};_sharedDx11BackBufferCopies[copySlot]->GetDesc(&target);
        if(!GenshinBridgeFg::SdrCopy11(sourceDesc,target)) {sourceTexture->Release();return RecordPrivateCopyFailure(this, "Dx11SdrCopyContractRefused");}
    }
    const bool reuseSnapshot = _sharedBackBufferIsReturnedSnapshot[copySlot];
    if (reuseSnapshot &&
        GenshinBridgeFg::Identity(sourceTexture) != GenshinBridgeFg::Identity(_sharedDx11BackBufferCopies[copySlot]))
    {
        sourceTexture->Release();
        return RecordPrivateCopyFailure(this, "ReturnedSnapshotIdentityMismatch");
    }
    auto* snapshotScope=GenshinBridgeFg::PresentScope::Current();
    auto snapshotLease=_nativeParentRoute && snapshotScope &&
        snapshotScope->ReturnedSnapshot()==_nativeOriginal.Get()
        ? snapshotScope->ReturnedSnapshotLease() : std::shared_ptr<GenshinPresentNr::SnapshotLease>{};
    // Cover the original shadow fallback as well as direct sharing. If a later
    // Signal fails this armed read cannot recycle, even without a provider tag.
    if(snapshotLease && !snapshotLease->BeginRead11(_sharedDx11BackBufferCopies[copySlot])) {
        sourceTexture->Release();return RecordPrivateCopyFailure(this,"SnapshotRead11AlreadyArmed");
    }
    if (reuseSnapshot)
    {
        // StageReturnedSnapshot already copied precisely this final picture on
        // this D3D11 context. Its shared Open supplies the very same allocation;
        // never issue a self-copy. The unchanged Signal/Flush/Wait below orders
        // that snapshot writer before the D3D12 copy into the FG backbuffer.
    }
    else if (sourceDesc.SampleDesc.Count > 1)
    {
        // The back buffer is multisampled and the shared copy is not: a copy would be dropped by D3D11 without a word.
        ID3D11Texture2D* sharedCopy = _sharedDx11BackBufferCopies[copySlot];

        if (const char* why = _msaaResolve.ResolveIn(_dx11Context, sourceTexture, sharedCopy))
        {
            LOG_ERROR("resolving the multisampled back buffer {} into the shadow copy failed: {}", index, why);
            sourceTexture->Release();
            return RecordPrivateCopyFailure(this, "Dx11MsaaResolveRefused");
        }
    }
    else
    {
        _dx11Context->CopyResource(_sharedDx11BackBufferCopies[copySlot], sourceTexture);
    }

    if (auto* scope = GenshinBridgeFg::PresentScope::Current()) {
        ComPtr<ID3D11Texture2D> nativeWitness;
        if(_nativeParentRoute && SUCCEEDED(_real->GetBuffer(index,IID_PPV_ARGS(&nativeWitness)))) {
            _visibleReceipt.backbuffer=GenshinBridgeFg::Identity(nativeWitness.Get());
            scope->NativeCopied(nativeWitness.Get(),_sharedDx11BackBufferCopies[copySlot]);
        }
        else if(!_nativeParentRoute)scope->NativeCopied(sourceTexture,_sharedDx11BackBufferCopies[copySlot]);
    }
    sourceTexture->Release();
    return true;
}

bool Dx11wDx12SC::_WaitDx11ThenDx12()
{
    const UINT copySlot=ResolvePrivateCopySlot(this,_currentFakeIndex);
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Dx11Sync, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (_dx11Context4 == nullptr || _dx11Fence == nullptr || _dx12CommandQueue == nullptr ||
        _dx12SharedFence == nullptr)
        return RecordPrivateCopyFailure(this, "Dx11SyncObjectsUnavailable");

    if((_nativeParentRoute || _sourceFrameLatencyOwner) && _sharedFenceValue>=UINT64_MAX-1)return RecordPrivateCopyFailure(this, "SharedFenceValueExhausted");
    const auto waitValue = _sharedFenceValue++;

    CallerCpuTrace::Scope dx11SignalCpu(CallerCpuTrace::Op::Dx11Signal, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, waitValue);
    auto result = _dx11Context4->Signal(_dx11Fence, waitValue);
    dx11SignalCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("DX11 Signal failed: {:X}", (UINT) result);
        return RecordPrivateCopyFailure(this, "Dx11FenceSignalFailed", result);
    }

    if(auto* scope=GenshinBridgeFg::PresentScope::Current(); _nativeParentRoute && scope &&
        scope->ReturnedSnapshot()==_nativeOriginal.Get())
        if(auto lease=scope->ReturnedSnapshotLease())lease->SubmittedRead11(_dx12SharedFence,waitValue);

    CallerCpuTrace::Scope flushCpu(CallerCpuTrace::Op::Dx11Flush, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, waitValue);
    _dx11Context4->Flush();
    GenshinTransport::ObserveOutputFlush(_dx11Context4);
    flushCpu.Finish();

    // Keep the producer-queue handoff, including lifecycle retirement. The
    // private transaction shares this exact submitted order with the color
    // writer below; a different presenting queue still needs its own Wait.
    CallerCpuTrace::Scope queueWaitCpu(CallerCpuTrace::Op::CopyQueueWaitSubmit, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, waitValue);
    result = OrderPrivateCopyWait(this, _dx12CommandQueue, _dx12SharedFence, waitValue);
    queueWaitCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("interop copy queue Wait on D3D11 fence failed: {:X}", (UINT) result);
        return RecordPrivateCopyFailure(this, "CopyQueueSharedFenceWaitFailed", result);
    }

    return true;
}

void Dx11wDx12SC::_ReportCopyFenceFault(const char* phase,UINT slot,UINT64 target,UINT64 completed,DWORD waitResult) noexcept
{
    const auto lastError=GetLastError();
    const auto dx12=_dx12Device?_dx12Device->GetDeviceRemovedReason():E_POINTER;
    const auto dx11=_dx11Device?_dx11Device->GetDeviceRemovedReason():E_POINTER;
    auto& reported=FAILED(dx12)||FAILED(dx11)?_copyDeviceFaultReported:_copyFenceFaultReported;
    if(!reported.exchange(true,std::memory_order_relaxed))try {
        LOG_ERROR("F6 copy fence fault: pid={} phase={} frame={} slot={} configured_copy_queue={} fence={} "
            "target={} completed={} wait_result={} dx12_removed={:X} dx11_removed={:X} "
            "max_is_not_complete=true observation_only=true",
            GetCurrentProcessId(),phase,State::Instance().frameCount,slot,reinterpret_cast<uintptr_t>(_dx12CommandQueue),
            reinterpret_cast<uintptr_t>(_copyFence),target,completed,waitResult,static_cast<UINT>(dx12),static_cast<UINT>(dx11));
    }catch(...){}
    SetLastError(lastError);
}
bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues)
{
    if (_sourceFrameLatencyOwner && (_sourceCopyUncertain.load(std::memory_order_acquire) || _copyFence == nullptr || _copyFenceEvent == nullptr))
        return RecordPrivateCopyFailure(this, "SourceCopyFenceUnavailable");
    if (_copyFence == nullptr || _copyFenceEvent == nullptr)
        return true;

    if (slot >= fenceValues.size())
    {
        LOG_ERROR("copy allocator slot {} out of range {}", slot, fenceValues.size());
        return RecordPrivateCopyFailure(this, "CopyAllocatorSlotOutOfRange");
    }

    const auto fenceValue = fenceValues[slot];

    if (_sourceFrameLatencyOwner && fenceValue == UINT64_MAX)
    {
        _sourceCopyUncertain.store(true, std::memory_order_release);
        return RecordPrivateCopyFailure(this, "SourceCopyReceiptUncertain");
    }
    if (fenceValue == 0 && !_sourceFrameLatencyOwner)
        return true;

    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue == UINT64_MAX)
    {
        _ReportCopyFenceFault("copy-fence-invalid",slot,fenceValue,completedValue,WAIT_FAILED);
        if (_sourceFrameLatencyOwner) _sourceCopyUncertain.store(true, std::memory_order_release);
        return RecordPrivateCopyFailure(this, "CopyFenceFailed", S_OK, completedValue);
    }
    if (fenceValue == 0 || completedValue >= fenceValue)
        return true;

    // The native-parent FG ring is optional work. Never stall the render thread
    // to reuse a live slot; the caller distinguishes Busy from terminal failure.
    if (_nativeParentRoute && GenshinBridgeFg::PresentScope::Current())
    {
        _visibleReceipt.reason = "CopySlotBusy"; // Preserve the caller's nonterminal native Busy branch.
        return RecordPrivateCopyFailure(this, "CopySlotBusy", S_OK, completedValue);
    }

    auto result = _copyFence->SetEventOnCompletion(fenceValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator fence SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}",
                  slot, fenceValue, completedValue, (UINT) result);
        return RecordPrivateCopyFailure(this, "CopyAllocatorEventArmFailed", result, completedValue);
    }

    const bool collectCopy = _sourceFrameLatencyOwner && GenshinHookCpuTiming::StarRailTimingActive();
    auto copyStamp = collectCopy ? GenshinHookCpuTiming::MakeStarRailStamp(
        GenshinHookCpuTiming::StarRailOrigin::CopyRetirement,_fgSwapChain) : GenshinHookCpuTiming::StarRailStamp{};
    if (collectCopy) {
        // Compare the actual vector instance, never a shader/NGX handle or a numeric slot alone.
        copyStamp.ring=&fenceValues==&_copyAllocatorFenceValues ? GenshinHookCpuTiming::StarRailCopyRing::SharedColor :
            (&fenceValues==&_nrCopyAllocatorFenceValues ? GenshinHookCpuTiming::StarRailCopyRing::NativeOverwrite :
             GenshinHookCpuTiming::StarRailCopyRing::Unknown);
        copyStamp.queue=reinterpret_cast<uintptr_t>(_dx12CommandQueue);copyStamp.fence=reinterpret_cast<uintptr_t>(_copyFence);
        copyStamp.slot=slot;copyStamp.required=fenceValue;copyStamp.completed=completedValue;copyStamp.completedKnown=true;
    }
    CallerCpuTrace::Scope copyWaitCpu(CallerCpuTrace::Op::CopyAllocatorWait, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, slot, fenceValue, completedValue, false, true);
    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    copyWaitCpu.Finish(waitResult, true);
    if (collectCopy) {
        copyStamp.result=waitResult;copyStamp.resultKnown=true;
        GenshinHookCpuTiming::RecordStarRailCall(GenshinHookCpuTiming::StarRailCall::CopyWait,copyStamp,
            {true,static_cast<int64_t>(copyWaitCpu.BeginQpc()),static_cast<int64_t>(copyWaitCpu.EndQpc())},true);
    }
    if (waitResult != WAIT_OBJECT_0)
    {
        _ReportCopyFenceFault("copy-wait-failed",slot,fenceValue,_copyFence->GetCompletedValue(),waitResult);
        LOG_ERROR("copy allocator fence wait failed. slot {}, fence {}, completed {}, waitResult {:X}", slot,
                  fenceValue, _copyFence->GetCompletedValue(), waitResult);
        return RecordPrivateCopyFailure(this, "CopyAllocatorWaitFailed", S_OK, completedValue);
    }

    const auto actual = _copyFence->GetCompletedValue();
    if(actual==UINT64_MAX||actual<fenceValue)
        _ReportCopyFenceFault("copy-wait-not-complete",slot,fenceValue,actual,waitResult);
    if (_sourceFrameLatencyOwner && actual == UINT64_MAX) _sourceCopyUncertain.store(true, std::memory_order_release);
    return (actual != UINT64_MAX && actual >= fenceValue) ? true :
        RecordPrivateCopyFailure(this, actual == UINT64_MAX ? "CopyFenceFailed" : "CopyAllocatorCompletionNotRetired", S_OK, actual);
}

bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues, CopySlotAdmission& admission)
{
    // Consume only the caller's exact pre-write proof. A changed timeline,
    // ring/allocator/fence or slot cannot be repaired by waiting after writing.
    if (_sourceCopyUncertain.load(std::memory_order_acquire) || slot >= fenceValues.size() ||
        slot >= _copyAllocators.size())
    {
        admission.Clear();
        return RecordPrivateCopyFailure(this, "CopyAdmissionUnavailable");
    }
    return admission.Consume(this, &fenceValues, _copyFence, _copyAllocators[slot], slot, fenceValues[slot]) ? true :
        RecordPrivateCopyFailure(this, "CopyAdmissionChanged");
}

bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(UINT dx11Index, CopySlotAdmission& admission,
                                               ID3D12Resource** copiedColour)
{
    if (copiedColour) *copiedColour=nullptr;
    const UINT copySlot=ResolvePrivateCopySlot(this,_currentFakeIndex);
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::InteropCopy, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (_copyAllocators.empty() || _copyCommandLists.empty() || _dx12CommandQueue == nullptr || _copyFence == nullptr ||
        _fgSwapChain == nullptr || copySlot >= _openedDx11BackBuffers.size() ||
        _openedDx11BackBuffers[copySlot] == nullptr)
    {
        return RecordPrivateCopyFailure(this, "FgCopyObjectsUnavailable");
    }

    if((_nativeParentRoute || _sourceFrameLatencyOwner) && _copyFenceValue>=UINT64_MAX-1)return RecordPrivateCopyFailure(this, "CopyFenceValueExhausted");
    auto* colorProvider = _nativeParentRoute ? dynamic_cast<DLSSG_Dx12*>(_fg) : nullptr;
    if (_nativeParentRoute && (!colorProvider || State::Instance().currentFG != colorProvider))
        return RecordPrivateCopyFailure(this, "ColorPresentProviderUnavailable");
    auto colorGuard = colorProvider ? colorProvider->GuardExecution(FGExecutionScope::Inputs) : FGExecutionGuard{};
    ComPtr<ID3D12CommandQueue> nativeColorQueue;
    auto* colorQueue = _dx12CommandQueue;
    if (_nativeParentRoute)
    {
        nativeColorQueue = colorProvider->GenshinColorWriteQueue(_fgSwapChain, _dx12Device);
        colorQueue = nativeColorQueue.Get();
        if (!colorQueue || !_dx12SharedFence || _sharedFenceValue <= 1 ||
            (_interopPresentQueue && (!GenshinBridgeFg::Identity(_interopPresentQueue.Get()) ||
             GenshinBridgeFg::Identity(_interopPresentQueue.Get()) != GenshinBridgeFg::Identity(colorQueue))))
            return RecordPrivateCopyFailure(this, "ColorPresentQueueOrHandoffRefused");
        if (_openedDx11BackBufferStates[copySlot] != D3D12_RESOURCE_STATE_COMMON)
            return RecordPrivateCopyFailure(this, "ColorSharedCommonStateUnproved");
    }
    auto allocator = _copyAllocators[copySlot];

    if (allocator == nullptr)
        return RecordPrivateCopyFailure(this, "CopyAllocatorUnavailable");

    if (!_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues, admission))
    {
        if (_nativeParentRoute && GenshinBridgeFg::PresentScope::Current() &&
            copySlot < _copyAllocatorFenceValues.size() && _copyAllocatorFenceValues[copySlot] && _copyFence &&
            _copyFence->GetCompletedValue() < _copyAllocatorFenceValues[copySlot])
            _visibleReceipt.reason = "CopySlotBusy";
        return RecordPrivateCopyFailure(this, "CopyAllocatorRetirementRefused");
    }

    CallerCpuTrace::Scope resetCpu(CallerCpuTrace::Op::CopyAllocatorReset, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
    auto result = allocator->Reset();
    resetCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator[{}] reset failed: {:X}", copySlot, (UINT) result);
        return RecordPrivateCopyFailure(this, "CopyAllocatorResetFailed", result);
    }

    CallerCpuTrace::Scope listResetCpu(CallerCpuTrace::Op::CopyListReset, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
    result = _copyCommandLists[copySlot]->Reset(allocator, nullptr);
    listResetCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list reset failed: {:X}", (UINT) result);
        return RecordPrivateCopyFailure(this, "CopyCommandListResetFailed", result);
    }

    UINT fgIndex = _fgSwapChain->GetCurrentBackBufferIndex();
    LOG_DEBUG("dx11Index {}, fgIndex {}", dx11Index, fgIndex);

    Microsoft::WRL::ComPtr<ID3D12Resource> fgBackBuffer;
    CallerCpuTrace::Scope fgBufferCpu(CallerCpuTrace::Op::FgGetBuffer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, fgIndex);
    result = _fgSwapChain->GetBuffer(fgIndex, IID_PPV_ARGS(&fgBackBuffer));
    fgBufferCpu.Finish();
    if (FAILED(result) || fgBackBuffer == nullptr)
    {
        LOG_ERROR("FG GetBuffer({}) failed: {:X}", fgIndex, (UINT) result);
        _copyCommandLists[copySlot]->Close();
        return RecordPrivateCopyFailure(this, "FgBackBufferGetBufferFailed", result);
    }

    if(_nativeParentRoute && !GenshinBridgeFg::SdrCopy12(
        _openedDx11BackBuffers[copySlot]->GetDesc(),fgBackBuffer->GetDesc())) {
        _copyCommandLists[copySlot]->Close();return RecordPrivateCopyFailure(this, "Dx12SdrCopyContractRefused");
    }
    // Prior tags may be modified on the authenticated blocking presenting
    // queue. Other/unknown queues still need exact vendor retirement or teardown.
    if (_nativeParentRoute)
    {
        auto* provider = dynamic_cast<DLSSG_Dx12*>(_fg);
        if (!provider || State::Instance().currentFG != provider ||
            !provider->PrepareGenshinColorWrite(fgBackBuffer.Get(), colorQueue))
        {
            _copyCommandLists[copySlot]->Close();
            return RecordPrivateCopyFailure(this, "ProviderColorWriteRefused");
        }
    }
    auto sourceBefore = _openedDx11BackBufferStates[copySlot];

    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], sourceBefore,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);

    _copyCommandLists[copySlot]->CopyResource(fgBackBuffer.Get(), _openedDx11BackBuffers[copySlot]);

    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], D3D12_RESOURCE_STATE_COPY_SOURCE,
                       sourceBefore);

    _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;

    // Same upload wait, copy list and completion as final PresentColor.
    if(auto* uiScope=GenshinBridgeFg::PresentScope::Current())
        uiScope->BuildUiHudless(_copyCommandLists[copySlot],fgBackBuffer.Get());
    result = _copyCommandLists[copySlot]->Close();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list close failed: {:X}", (UINT) result);
        return RecordPrivateCopyFailure(this, "CopyCommandListCloseFailed", result);
    }

    if (_nativeParentRoute)
    {
        // Reuse only the exact successful Wait from this private transaction.
        // Same-queue order already covers this writer; another queue/fence/value
        // must submit a real Wait. Pin before a potentially partial failure.
        _interopPresentQueue = colorQueue;
        result = OrderPrivateCopyWait(this, colorQueue, _dx12SharedFence, _sharedFenceValue - 1);
        if (FAILED(result)) return RecordPrivateCopyFailure(this, "ColorPresentSharedFenceWaitFailed", result);
    }
    ID3D12CommandList* lists[] = { _copyCommandLists[copySlot] };
    CallerCpuTrace::Scope copyExecuteCpu(CallerCpuTrace::Op::CopyExecute, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
    auto* snapshotScope=GenshinBridgeFg::PresentScope::Current();
    auto snapshotLease=_nativeParentRoute && _sharedBackBufferIsReturnedSnapshot[copySlot] && snapshotScope &&
        snapshotScope->ReturnedSnapshot()==_nativeOriginal.Get()
        ? snapshotScope->ReturnedSnapshotLease() : std::shared_ptr<GenshinPresentNr::SnapshotLease>{};
    // Pin BEFORE Execute. A failed Signal leaves the actual lease/read queue
    // retained by Frame as well as the original quarantined wrapper copy ring.
    if(snapshotLease && !snapshotLease->BeginRead12(_openedDx11BackBuffers[copySlot],fgBackBuffer.Get(),colorQueue))
        return RecordPrivateCopyFailure(this,"SnapshotRead12AlreadyArmed");
    colorQueue->ExecuteCommandLists(1, lists);
    copyExecuteCpu.Finish();

    const auto signalValue = ++_copyFenceValue;

    CallerCpuTrace::Scope copySignalCpu(CallerCpuTrace::Op::CopySignal, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, signalValue);
    result = colorQueue->Signal(_copyFence, signalValue);
    copySignalCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("interop copy fence signal failed: {:X}", (UINT) result);
        // Interop uncertain submission begin
        // Execute accepted a reader on the actual color queue. No old slot
        // receipt, later signal or provider mode change can retire that work.
        // Retain the wrapper's real shared source/list/allocator/fence, and pin
        // the borrowed queue/device plus exact acquired destination explicitly.
        // This is GPU lifetime denial only; it grants no StarRail/native rights.
        _sourceCopyUncertain.store(true, std::memory_order_release);
        _copyAllocatorFenceValues[copySlot] = UINT64_MAX;
        _lastInteropCopyFenceValue = 0;
        AddRef();
        colorQueue->AddRef();
        _dx12Device->AddRef();
        auto* uncertainBackBuffer = fgBackBuffer.Detach();
        HoYoShade::QuarantineColor(_fgSwapChain, colorQueue, _dx12Device,
            uncertainBackBuffer, State::Instance().frameCount ? State::Instance().frameCount : 1);
        // Interop uncertain submission end
        return RecordPrivateCopyFailure(this, "CopyFenceSignalFailed", result);
    }

    if(snapshotLease)snapshotLease->SubmittedRead12(_copyFence,signalValue);
    _copyAllocatorFenceValues[copySlot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;
    // Scope-local reference only, returned after the actual successful Signal.
    // It does not outlive this Present or replace allocator/SDK retirement.
    if (copiedColour) { fgBackBuffer->AddRef(); *copiedColour=fgBackBuffer.Get(); }
    if (auto* scope = GenshinBridgeFg::PresentScope::Current(); scope && copySlot < _genshinFgImports.size())
        scope->Copied(_genshinFgImports[copySlot], _openedDx11BackBuffers[copySlot], fgBackBuffer.Get(),
                      fgIndex, _copyFence, signalValue);

    if (Config::Instance()->DlssNrEnabled.value_or_default() &&
        Config::Instance()->DlssNrFinishedPicture.value_or_default())
        DlssNrFgTrace::BridgeCopy(State::Instance().frameCount, _openedDx11BackBuffers[copySlot],
                                  fgBackBuffer.Get(), _fgSwapChain, fgIndex, colorQueue,
                                  _copyFence, signalValue);
    else if (DlssNrFgTrace::armed.load())
        DlssNrFgTrace::EndBridge();

    return true;
}

void Dx11wDx12SC::_ApplyNativeInputToFGBackBuffer()
{
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::NativeCopy, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    // Runs the D3D11 depth finder's frame close and the native input producer (NativeMotionDx11::OnFGPresent); null
    // when the option is off, the game is calling its own upscaler, or nothing was ready this frame -- all of those
    // are the ordinary case, not a failure.
    CallerCpuTrace::Scope nativeProducerCpu(CallerCpuTrace::Op::NativeProducer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    ID3D12Resource* processed = NativeMotionDx11::OnFGPresent(_real, _dx11Device);
    nativeProducerCpu.Finish();

    if (processed == nullptr)
        return;
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues)) || _copyFenceValue >= UINT64_MAX - 1)
        return; // no optional recording if its timeline cannot publish a new value

    if (_nrCopyAllocators.empty() && !_EnsureNativeInputCopyRing())
    {
        LOG_WARN("native input ready but its copy ring could not be created: dropping this frame's result");
        return;
    }

    if (_nrCopyAllocators.empty() || _nrCopyCommandLists.empty() || _dx12CommandQueue == nullptr ||
        _copyFence == nullptr || _fgSwapChain == nullptr || _currentFakeIndex >= _nrCopyAllocators.size())
    {
        LOG_WARN("native input ready but the FG copy ring is not: dropping this frame's result");
        return;
    }

    const UINT slot = _currentFakeIndex;
    auto allocator = _nrCopyAllocators[slot];

    if (allocator == nullptr || !_WaitForCopyAllocator(slot, _nrCopyAllocatorFenceValues))
        return;

    CallerCpuTrace::Scope nativeResetCpu(CallerCpuTrace::Op::NativeAllocatorReset, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, slot);
    auto result = allocator->Reset();
    nativeResetCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("native input copy allocator[{}] reset failed: {:X}", slot, (UINT) result);
        return;
    }

    auto* list = _nrCopyCommandLists[slot];

    CallerCpuTrace::Scope nativeListResetCpu(CallerCpuTrace::Op::NativeListReset, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, slot);
    result = list->Reset(allocator, nullptr);
    nativeListResetCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("native input copy command list reset failed: {:X}", (UINT) result);
        return;
    }

    UINT fgIndex = _fgSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* fgBackBuffer = nullptr;
    CallerCpuTrace::Scope nativeBufferCpu(CallerCpuTrace::Op::NativeGetBuffer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, fgIndex);
    result = _fgSwapChain->GetBuffer(fgIndex, IID_PPV_ARGS(&fgBackBuffer));
    nativeBufferCpu.Finish();
    if (FAILED(result) || fgBackBuffer == nullptr)
    {
        LOG_ERROR("native input copy FG GetBuffer({}) failed: {:X}", fgIndex, (UINT) result);
        list->Close();
        return;
    }

    if (_nativeParentRoute)
    {
        auto* provider = dynamic_cast<DLSSG_Dx12*>(_fg);
        if (!provider || State::Instance().currentFG != provider ||
            !provider->PrepareGenshinColorWrite(fgBackBuffer))
        {
            fgBackBuffer->Release();
            list->Close();
            return;
        }
    }
    // `processed` rests in COMMON (native::NativeProducer's own contract); fgBackBuffer was already put back in
    // PRESENT by _CopyDx11SharedToDx12FGBackBuffer just before this runs.
    TransitionResource(list, processed, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(list, fgBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

    list->CopyResource(fgBackBuffer, processed);

    TransitionResource(list, fgBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    TransitionResource(list, processed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    // Keep the actual destination reference through Execute/Signal so a failed
    // submission can pin the exact GPU operands rather than requery a new buffer.
    result = list->Close();
    if (FAILED(result))
    {
        LOG_ERROR("native input copy command list close failed: {:X}", (UINT) result);
        fgBackBuffer->Release();
        return;
    }

    ID3D12CommandList* lists[] = { list };
    CallerCpuTrace::Scope nativeExecuteCpu(CallerCpuTrace::Op::NativeExecute, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, slot);
    _dx12CommandQueue->ExecuteCommandLists(1, lists);
    nativeExecuteCpu.Finish();

    const auto signalValue = ++_copyFenceValue;

    CallerCpuTrace::Scope nativeSignalCpu(CallerCpuTrace::Op::NativeSignal, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, slot, signalValue);
    result = _dx12CommandQueue->Signal(_copyFence, signalValue);
    nativeSignalCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("native input copy fence signal failed: {:X}", (UINT) result);
        // Execute already happened: no old raw-copy value can retire this ring.
        // Permanently retain the real wrapper (all rings/handles), exact source,
        // destination, list/allocator and queue/fence. No later menu-off, Present,
        // resize, idle drain or Release may reset or discard these owners.
        for (auto& value : _nrCopyAllocatorFenceValues) value = UINT64_MAX;
        AddRef(); processed->AddRef(); list->AddRef(); allocator->AddRef();
        _dx12CommandQueue->AddRef(); _copyFence->AddRef();
        HoYoShade::QuarantineColor(_fgSwapChain, _dx12CommandQueue, _dx12Device,
            fgBackBuffer, State::Instance().frameCount ? State::Instance().frameCount : 1);
        return; // deliberately retain the existing fgBackBuffer reference
    }
    fgBackBuffer->Release();

    _nrCopyAllocatorFenceValues[slot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;
}

bool Dx11wDx12SC::_WaitForInteropCopyOnPresentQueue()
{
    const UINT copySlot=ResolvePrivateCopySlot(this,_currentFakeIndex);
    if (_copyFence == nullptr)
        return RecordPrivateCopyFailure(this, "PresentCopyFenceUnavailable");

    if (_lastInteropCopyFenceValue == 0)
        return true;

    if (_nativeParentRoute)
    {
        auto* provider = dynamic_cast<DLSSG_Dx12*>(_fg);
        if (!provider || State::Instance().currentFG != provider)
            return RecordPrivateCopyFailure(this, "ColorPresentProviderUnavailable");
        auto guard = provider->GuardExecution(FGExecutionScope::Inputs);
        const auto queue = provider->GenshinColorWriteQueue(_fgSwapChain, _dx12Device);
        const auto actual = GenshinBridgeFg::Identity(_interopPresentQueue.Get());
        if (!queue || !actual || actual != GenshinBridgeFg::Identity(queue.Get()))
            return RecordPrivateCopyFailure(this, "PresentQueueChanged");
        // The actual color Wait/Execute/Signal already ran on this queue. Do
        // not add a self-wait or label the guide producer queue as color writer.
        if (auto* scope = GenshinBridgeFg::PresentScope::Current())
            scope->CopiedOrderedOnPresentQueue(queue.Get(), _copyFence, _lastInteropCopyFenceValue);
        return true;
    }
    // The interop copies are submitted on _dx12CommandQueue. Only an FG backend presenting from a different queue
    // needs a cross-queue wait; with no backend queue (no FG feature, or the backend's own swapchain creation failed
    // and DxgiFactoryHooks fell back to a plain DX12 swapchain on _dx12CommandQueue -- seen with XeFG in Prey, where
    // GetCommandQueue() then stays null for the whole session) the present is already ordered after the copy on
    // the same queue. Failing here instead turned every Present into DXGI_ERROR_DEVICE_REMOVED for the game.
    auto* queue = _fg != nullptr ? _fg->GetCommandQueue() : nullptr;
    if(_interopPresentQueue && _interopPresentQueue.Get()!=queue) {
        if(_nativeParentRoute) {
            // Do not drain either queue from the ordinary Present path. Keep the
            // exact old/new ownership for recoverable teardown and reset instead.
            _visibleReceipt.reason="PresentQueueChanged";
            return RecordPrivateCopyFailure(this, "PresentQueueChanged");
        }
        const auto copiedValue=_lastInteropCopyFenceValue;
        if(!_WaitForCopyQueueIdle())return RecordPrivateCopyFailure(this, "PreviousCopyQueueRetirementRefused");
        _lastInteropCopyFenceValue=copiedValue; // preserve THIS copy's receipt after draining the previous queue
    }
    if(queue)_interopPresentQueue=queue; // arm ownership BEFORE a possibly partially accepted Wait
    if (queue == nullptr || queue == _dx12CommandQueue)
    {
        if (auto* scope = GenshinBridgeFg::PresentScope::Current(); scope && queue)
            scope->Waited(queue, _copyFence, _lastInteropCopyFenceValue);
        return true;
    }

    CallerCpuTrace::Scope presentWaitCpu(CallerCpuTrace::Op::PresentQueueWaitSubmit, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, _lastInteropCopyFenceValue);
    auto result = queue->Wait(_copyFence, _lastInteropCopyFenceValue);
    presentWaitCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("present queue Wait on interop copy fence failed: {:X}", (UINT) result);
        return RecordPrivateCopyFailure(this, "PresentQueueCopyFenceWaitFailed", result);
    }

    if (auto* scope = GenshinBridgeFg::PresentScope::Current())
        scope->Waited(queue, _copyFence, _lastInteropCopyFenceValue);
    return true;
}

bool Dx11wDx12SC::_WaitForCopyQueueIdle()
{
    auto* entered = _nativeParentRoute ? &_copyDrainEntered : nullptr;
    if (entered && *entered) return false;
    if (entered) *entered = true;
    struct DrainScope { bool* entered; ~DrainScope() { if (entered) *entered = false; } } drain{entered};
    // Only the revoked child under the native lifecycle lock can retry the
    // same drain. Normal resize and teardown always establish fresh markers.
    const bool recovery = _nativeParentRoute && _childRecoveryEntered;
    if (_nativeParentRoute && !recovery) { _recoveryCopyDrain = {}; _recoveryPresentDrain = {}; }
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return false;
    const bool haveObjects=!_copyCommandLists.empty() || !_nrCopyCommandLists.empty() ||
        !_sharedDx11BackBufferCopies.empty() || !_openedDx11BackBuffers.empty() || _interopPresentQueue!=nullptr;
    if(!haveObjects)return true;
    if(!_dx12CommandQueue || !_copyFence || !_copyFenceEvent ||
       !_dx11Context4 || !_dx11Fence || !_dx12SharedFence)return false;
    auto submitCopyDrain = [&] {
        // If the copy epoch changed, re-establish both markers in order.
        if (recovery) _recoveryPresentDrain = {};
        // An aborted Present can leave DX11 writes without a handoff. The
        // idle marker must cover those too, not just earlier allocator values.
        if (_copyFenceValue >= UINT64_MAX - 1 || !_WaitDx11ThenDx12()) return false;
        // Do not advance a shared timeline from a different queue until the
        // exact last color submission on the retained queue is ordered first.
        if (_nativeParentRoute && _interopPresentQueue &&
            GenshinBridgeFg::Identity(_interopPresentQueue.Get()) != GenshinBridgeFg::Identity(_dx12CommandQueue) &&
            _lastInteropCopyFenceValue &&
            FAILED(_dx12CommandQueue->Wait(_copyFence, _lastInteropCopyFenceValue))) return false;
        const auto waitValue=++_copyFenceValue;
        if (FAILED(_dx12CommandQueue->Signal(_copyFence,waitValue))) return false;
        _lastInteropCopyFenceValue=waitValue;
        return true;
    };
    if (recovery ? !_recoveryCopyDrain.ReuseOrSubmit(_dx12CommandQueue, _copyFence, _copyFenceValue, submitCopyDrain)
                 : !submitCopyDrain()) return false;
    const auto waitValue = _copyFenceValue;
    auto completed=_copyFence->GetCompletedValue();
    if(completed==UINT64_MAX)return false;
    if(completed<waitValue) {
        if (recovery) return false; // poll this same marker on a later Present
        if(FAILED(_copyFence->SetEventOnCompletion(waitValue,_copyFenceEvent)))return false;
        CallerCpuTrace::Scope copyIdleCpu(CallerCpuTrace::Op::CopyIdleWait,this,reinterpret_cast<uint64_t>(_handle),
            State::Instance().frameCount,UINT64_MAX,waitValue,completed,false,true);
        const auto waitResult=WaitForSingleObject(_copyFenceEvent,5000);copyIdleCpu.Finish(waitResult,true);
        completed=_copyFence->GetCompletedValue();
        if(waitResult!=WAIT_OBJECT_0 || completed==UINT64_MAX || completed<waitValue)return false;
    }
    if(_interopPresentQueue && _interopPresentQueue.Get()!=_dx12CommandQueue) {
        if(!_presentRetirementFence) {
            ComPtr<ID3D12Device> device;
            if(FAILED(_interopPresentQueue->GetDevice(IID_PPV_ARGS(&device))) ||
               FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&_presentRetirementFence))))return false;
        }
        if (!recovery && !_presentRetirementEvent)
            _presentRetirementEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if ((!recovery && !_presentRetirementEvent) ||
            !WaitForQueueIdle(_interopPresentQueue.Get(), _presentRetirementFence,
                _presentRetirementEvent, _presentRetirementValue, recovery ? &_recoveryPresentDrain : nullptr)) return false;
    }
    if (recovery) { _recoveryCopyDrain = {}; _recoveryPresentDrain = {}; }
    for(auto& value:_copyAllocatorFenceValues)value=0;
    for(auto& value:_nrCopyAllocatorFenceValues)value=0;
    _lastInteropCopyFenceValue=0;
    return true;
}

void Dx11wDx12SC::_ReleaseInteropBackBuffers()
{
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return;
    _nativeOriginal.Reset();
    _visibleFgBlocked = false;
    _interopInitialized = false;
    _genshinFgImports.clear();

    for (auto& resource : _openedDx11BackBuffers)
        SafeRelease(resource);

    for (auto& texture : _sharedDx11BackBufferCopies)
        SafeRelease(texture);

    for (auto& handle : _sharedBackBufferHandles)
        SafeCloseHandle(handle);

    _openedDx11BackBufferStates.clear();
    _openedDx11BackBuffers.clear();
    _sharedDx11BackBufferCopies.clear();
    _sharedBackBufferIsReturnedSnapshot.clear();
    _sharedBackBufferHandles.clear();
}

bool Dx11wDx12SC::_ReleaseInteropObjects()
{
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return false;
    if(!_WaitForCopyQueueIdle())return false;
    if (_nativeParentRoute) {GenshinLegacy::ForgetNativeSwapchain(_genshinPresentIdentity); _genshinPresentIdentity=0;}
    _nativeOriginal.Reset();
    _ReleaseInteropBackBuffers();

    _lastInteropCopyFenceValue = 0;

    SafeRelease(_dx12SharedFence);
    SafeRelease(_dx11Fence);
    SafeCloseHandle(_dx11FenceEvent);
    SafeCloseHandle(_sharedFenceHandle);

    for (auto& cmdList : _copyCommandLists)
        SafeRelease(cmdList);

    _copyCommandLists.clear();

    for (auto& allocator : _copyAllocators)
        SafeRelease(allocator);

    _copyAllocators.clear();
    _copyAllocatorFenceValues.clear();

    for (auto& cmdList : _nrCopyCommandLists)
        SafeRelease(cmdList);

    _nrCopyCommandLists.clear();

    for (auto& allocator : _nrCopyAllocators)
        SafeRelease(allocator);

    _nrCopyAllocators.clear();
    _nrCopyAllocatorFenceValues.clear();

    SafeRelease(_copyFence);
    SafeCloseHandle(_copyFenceEvent);
    _copyFenceValue = 1;
    SafeRelease(_presentRetirementFence);
    SafeCloseHandle(_presentRetirementEvent);
    _presentRetirementValue=0;_interopPresentQueue.Reset();

    _interopInitialized = false;
    return true;
}

void Dx11wDx12SC::_RefreshCachedSwapchainDesc()
{
    _bufferCount = ResolveBufferCount(_real, _real1);
    _bufferFormat = ResolveBufferFormat(_real, _real1);
    _currentFakeIndex = _bufferCount > 0 ? _currentFakeIndex : 0;
}

UINT Dx11wDx12SC::_GetDx11BackBufferIndexForPresent() const
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _bufferCount > 0 ? _currentFakeIndex : 0;
}

void Dx11wDx12SC::_AdvanceFakeBackBufferIndex() { _currentFakeIndex = (_currentFakeIndex + 1) % _bufferCount; }


// R564: the native chain remains on the actual game parent. The FG chain owns
// only an initially hidden child. No provider success or hidden Present can
// substitute for the native parent result below.
void Dx11wDx12SC::_SuspendUnobservedNativeDisplay() noexcept
{
    _nativeDisplayUnobserved.store(true);
    if(_visibleChild)_visibleChild->RequestHide();
}
bool Dx11wDx12SC::_ValidateNativeVisible(bool recovering)
{
    if(_nativeDisplayUnobserved.load())return false;
    HWND parentWindow=nullptr,childWindow=nullptr;uint64_t epoch=0;UINT width=0,height=0,dpi=0;
    if(!_visibleChild || !_visibleChild->Prepare() ||
       !_visibleChild->Snapshot(parentWindow,childWindow,epoch,width,height,dpi))return false;
    if ((!recovering && (_childRevoked || _visibleFgBlocked || !_childSdrReady)) ||
        !_nativeColorSpaceKnown || _nativeColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ||
        !_nativeHdrMetadataAbsent || !_nativeMatrixIdentity || !_visibleProvider ||
        State::Instance().currentFG!=_visibleProvider ||
        !dynamic_cast<DLSSG_Dx12*>(_visibleProvider) ||
        dynamic_cast<DLSSG_Dx12*>(_visibleProvider)->Hwnd()!=_visibleProviderHwnd || parentWindow != _handle ||
        !GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) ||
        State::Instance().activeFgInput != FGInput::Upscaler || State::Instance().activeFgOutput != FGOutput::DLSSG ||
        State::Instance().swapchainInteropApi != SwapchainInteropApi::Dx11wDx12 ||
        State::Instance().currentRealSwapchain != _real || State::Instance().currentFGSwapchain != _fgSwapChain ||
        !_real || !_fgSwapChain || !_dx11Context4 || !_dx11Device5 || !_dx12Device || !_dx12CommandQueue)
        return false;
    DXGI_SWAP_CHAIN_DESC native{}, child{};
    BOOL exclusive = TRUE;
    ComPtr<ID3D11Device> actual11, contextDevice;
    ComPtr<ID3D12Device> actual12, queueDevice;
    _dx11Context->GetDevice(&contextDevice);
    if (FAILED(_real->GetDesc(&native)) || FAILED(_fgSwapChain->GetDesc(&child)) ||
        FAILED(_real->GetFullscreenState(&exclusive, nullptr)) || exclusive || !native.Windowed || !child.Windowed ||
        native.OutputWindow != _handle || child.OutputWindow != childWindow ||
        native.BufferDesc.Width != width || native.BufferDesc.Height != height ||
        child.BufferDesc.Width != native.BufferDesc.Width || child.BufferDesc.Height != native.BufferDesc.Height ||
        !GenshinBridgeFg::SdrCopyFormat(native.BufferDesc.Format,child.BufferDesc.Format) ||
        (child.BufferDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && child.BufferDesc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM) ||
        native.SampleDesc.Count!=1 || native.SampleDesc.Quality || child.SampleDesc.Count!=1 || child.SampleDesc.Quality ||
        child.BufferCount<2 || child.BufferCount>16 ||
        (child.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD && child.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) ||
        FAILED(_real->GetDevice(IID_PPV_ARGS(&actual11))) || FAILED(_fgSwapChain->GetDevice(IID_PPV_ARGS(&actual12))) ||
        FAILED(_dx12CommandQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
        GenshinBridgeFg::Identity(actual11.Get()) != GenshinBridgeFg::Identity(_dx11Device) ||
        GenshinBridgeFg::Identity(contextDevice.Get()) != GenshinBridgeFg::Identity(_dx11Device) ||
        GenshinBridgeFg::Identity(actual12.Get()) != GenshinBridgeFg::Identity(_dx12Device) ||
        GenshinBridgeFg::Identity(queueDevice.Get()) != GenshinBridgeFg::Identity(_dx12Device) ||
        _dx11Context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        _dx11Device->GetDeviceRemovedReason() != S_OK || _dx12Device->GetDeviceRemovedReason() != S_OK)
        return false;
    // Native bitblt remains untouched. Only a freshly created, full-client,
    // windowed nonflip chain with no attempted transform setter uses defaults.
    const bool flip=native.SwapEffect==DXGI_SWAP_EFFECT_FLIP_DISCARD || native.SwapEffect==DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    const bool bitblt=(native.SwapEffect==DXGI_SWAP_EFFECT_DISCARD || native.SwapEffect==DXGI_SWAP_EFFECT_SEQUENTIAL) &&
        native.BufferCount==1;
    if(State::Instance().hdrOutputActive || (!flip && !bitblt) || (flip && native.BufferCount<2) ||
       native.BufferDesc.Scaling!=DXGI_MODE_SCALING_UNSPECIFIED ||
       native.BufferDesc.ScanlineOrdering!=DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED)return false;
    DXGI_SWAP_CHAIN_DESC1 child1{},native1{};
    if(FAILED(_fgSwapChain->GetDesc1(&child1)) || child1.Stereo ||
       (child1.AlphaMode!=DXGI_ALPHA_MODE_IGNORE && child1.AlphaMode!=DXGI_ALPHA_MODE_UNSPECIFIED) ||
       (child1.Scaling!=DXGI_SCALING_STRETCH && child1.Scaling!=DXGI_SCALING_NONE))return false;
    if(_real1 && SUCCEEDED(_real1->GetDesc1(&native1))) {
        if(native1.Stereo || (native1.AlphaMode!=DXGI_ALPHA_MODE_IGNORE && native1.AlphaMode!=DXGI_ALPHA_MODE_UNSPECIFIED) ||
           (native1.Scaling!=DXGI_SCALING_STRETCH && native1.Scaling!=DXGI_SCALING_NONE))return false;
    } else if(_real1 || !bitblt)return false;
    DXGI_MODE_ROTATION rotation{};UINT sourceWidth=0,sourceHeight=0;
    const auto rotationHR=_real1?_real1->GetRotation(&rotation):E_NOINTERFACE;
    const auto sourceHR=_real2?_real2->GetSourceSize(&sourceWidth,&sourceHeight):E_NOINTERFACE;
    const auto unavailable=[](HRESULT hr){return hr==E_NOINTERFACE || hr==DXGI_ERROR_INVALID_CALL || hr==DXGI_ERROR_UNSUPPORTED;};
    if(SUCCEEDED(rotationHR) ? rotation!=DXGI_MODE_ROTATION_IDENTITY :
       !(bitblt && _nativeDefaultRotation && unavailable(rotationHR)))return false;
    if(SUCCEEDED(sourceHR) ? (sourceWidth!=width || sourceHeight!=height) :
       !(bitblt && _nativeDefaultSourceSize && unavailable(sourceHR)))return false;
    if(_visibleChild->Epoch()!=epoch)return false;
    return true;
}

HRESULT Dx11wDx12SC::_PresentNativeParent(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters)
{
    if(!_real)return DXGI_ERROR_DEVICE_REMOVED;
    GenshinNativeParentFrame parent(_real,_fgSwapChain,_dx11Device,_handle,_visibleProviderHwnd,_genshinPresentIdentity);
    parent.menuFallback_ = true; // This helper is exclusively native-only.
    if(!(flags & DXGI_PRESENT_TEST))GenericDepthDx11::OnPresent(_real);
    return parent.Present(sync,flags,parameters,false);
}

void Dx11wDx12SC::_RevokeNativeChild(bool recoverableDisplayChange)
{
    if (!ModuleLifetime::CleanupAllowed()) return;
    _recoveryCopyDrain = {};
    _recoveryPresentDrain = {};
    // Terminal failure is sticky. A display setter cannot launder unknown GPU
    // completion, stale provider ownership, or an unsupported Present thread.
    if (_childDisplaySerial == UINT64_MAX) recoverableDisplayChange = false;
    else ++_childDisplaySerial;
    _childRecoveryAllowed = recoverableDisplayChange && (!_childRevoked || _childRecoveryAllowed);
    _childBudgetSuspended = false; // A later terminal failure cannot inherit a budget resume.
    _childRecoveryAttempted = false;
    _childRevoked=true;_visibleFgBlocked=true;
    if(_visibleChild)_visibleChild->RequestHide();
    GenshinBridgeFg::Revoke();
}
bool Dx11wDx12SC::_TryDrainNativeChild()
{
    if (!ModuleLifetime::CleanupAllowed()) return false;
    if(_visiblePresentEntered)return false;
    // A stale/non-current raw provider cannot sign another owner's completion.
    if(_visibleProvider && State::Instance().currentFG!=_visibleProvider)return false;
    auto* provider=dynamic_cast<DLSSG_Dx12*>(_visibleProvider);
    if(_visibleProvider && (!provider || provider->Hwnd()!=_visibleProviderHwnd))return false;
    auto execution=provider ? provider->GuardExecution(FGExecutionScope::Lifecycle) : FGExecutionGuard{};
    if(provider && provider->SwapchainContext() && !provider->PrepareForSwapchainChange())return false;
    GenshinBridgeFg::Collect();
    if(!_WaitForCopyQueueIdle())return false;
    _ReleaseInteropBackBuffers();
    return true;
}
bool Dx11wDx12SC::_TryRecoverNativeChild()
{
    if (!ModuleLifetime::CleanupAllowed()) return false;
    if (!_nativeParentRoute || _parentDetached || !_childRevoked ||
        _childRecoveryEntered || _visiblePresentEntered || !_visibleChild ||
        !Config::Instance()->FGEnabled.value_or_default()) return false;
    if (_childBudgetSuspended)
    {
        if (State::Instance().currentFG != _visibleProvider) return false;
        auto* provider = dynamic_cast<DLSSG_Dx12*>(_visibleProvider);
        if (!provider) return false;
        const auto request = provider->GenshinResumeRequests();
        if (request == UINT64_MAX || request <= _childBudgetResumeRequest) return false;
        // Consume one explicit off/on request, not one credit per frame/free.
        // Full owner/device/window revalidation and real drain still run below.
        _childBudgetSuspended = false;
        _childRecoveryAllowed = true;
        _childRecoveryAttempted = false;
    }
    if (!_childRecoveryAllowed || _childRecoveryAttempted) return false;
    // Ignore only the two admission latches and uninitialized child SDR state.
    // All original executable/route/provider/window/device/descriptor gates run.
    // Changed dimensions do not resize/recreate a child speculatively.
    _childRecoveryEntered = true;
    struct Recovering { bool& value; ~Recovering(){value=false;} } recovering{_childRecoveryEntered};
    if (!_ValidateNativeVisible(true)) return false; // pending owner publication consumes no drain attempt
    const auto displaySerial = _childDisplaySerial;
    const auto stillRevoked = [&]() {
        return !_parentDetached && _childRevoked && _childRecoveryAllowed &&
            _childDisplaySerial == displaySerial;
    };
    _childRecoveryAttempted = true; // at most one probe this Present
    auto* provider = dynamic_cast<DLSSG_Dx12*>(_visibleProvider);
    if (!provider || !provider->SwapchainContext()) return false;
    auto execution = provider->GuardExecution(FGExecutionScope::Lifecycle);
    GenshinBridgeFg::Revoke(); // discard old ready picture and guide predecessor
    if (!_TryDrainNativeChild()) {
        _childRecoveryAttempted = false; // one nonblocking probe per Present until completion
        return false;
    }
    // A window publication can change while the completed drain runs.
    // Refuse this attempt, but let a later Present start again with full
    // validation and a real drain. GPU/provider/SDR failures retain their
    // existing gates; no old receipt or permit gains new authority.
    const auto revalidateForRetry = [&]() {
        if (_ValidateNativeVisible(true)) return true;
        _childRecoveryAttempted = false;
        return false;
    };
    _visibleFgBlocked = true; // drain released old buffers, but not admission
    if (!stillRevoked() || State::Instance().currentFG != provider || !provider->CanSubmitInputs() ||
        !provider->SwapchainContext() || !revalidateForRetry()) return false;
    UINT support = 0;
    _childSdrReady = false;
    if (_fgSwapChain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,&support) != S_OK ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
        _fgSwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) != S_OK) return false;
    // Initial revocation already invalidated old permits; do not cancel the owner publication here.
    if (!stillRevoked() || State::Instance().currentFG != provider ||
        !revalidateForRetry() || !provider->CanSubmitInputs()) return false;
    _RefreshCachedSwapchainDesc();
    if (!revalidateForRetry() || !provider->CanSubmitInputs() || !stillRevoked() ||
        State::Instance().currentFG != provider) return false;
    _fg = nullptr; _visibleReceipt = {};
    _childSdrReady = true;
    _childRevoked = false; _visibleFgBlocked = false;
    _childRecoveryAllowed = false;
    // This grants only admission. Existing same-frame camera/Return/dispatch/
    // parent-S_OK/PresentPermit checks still decide whether anything is shown.
    return true;
}
ULONG Dx11wDx12SC::_ReleaseNativeParent()
{
    // The last external reference is gone. Detach parent COM ownership NOW;
    // retained optional GPU/window owners must not retain the native swapchain.
    {
        std::lock_guard<std::recursive_mutex> nativeLock(_nativeParentMutex);
        _LogNativeVisibleReceipt(true);
        _RevokeNativeChild();_parentDetached=true;
        GenshinLegacy::ForgetNativeSwapchain(_genshinPresentIdentity);_genshinPresentIdentity=0;
        if(State::Instance().currentSwapchain==this)State::Instance().currentSwapchain=nullptr;
        if(State::Instance().currentWrappedSwapchain==this)State::Instance().currentWrappedSwapchain=nullptr;
        if(State::Instance().currentRealSwapchain==_real) {
            State::Instance().currentRealSwapchain=nullptr;
            if(State::Instance().currentD3D11Device==_dx11Device)State::Instance().currentD3D11Device=nullptr;
            State::Instance().swapchainInteropApi=SwapchainInteropApi::None;
        }
        MenuOverlayDx::CleanupRenderTarget(true,_handle);
        SafeRelease(_real4);SafeRelease(_real3);SafeRelease(_real2);SafeRelease(_real1);SafeRelease(_real);
    }
    // A single retained child bounds uncertain teardown. No resurrected COM
    // reference, synchronous SendMessage, thread worker, or game retry needed.
    if(_visibleChild)_visibleChild->RetireOnOwner(_RetireNativeChildOnOwner,this);
    else _TryRetireNativeChildOnOwner();
    return 0;
}
void Dx11wDx12SC::_RetireNativeChildOnOwner(void* value)
{static_cast<Dx11wDx12SC*>(value)->_TryRetireNativeChildOnOwner();}
void Dx11wDx12SC::_TryRetireNativeChildOnOwner()
{
    if (!ModuleLifetime::CleanupAllowed()) return;
    // Owner must never wait behind Present while DXGI may require its message pump.
    std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
    if(!nativeLock.owns_lock() || _childRetirementEntered)return;
    _childRetirementEntered=true;
    struct Retirement {
        bool& entered;bool completed=false;
        ~Retirement(){if(!completed)entered=false;}
    } retirement{_childRetirementEntered};
    if(!_parentDetached || !_TryDrainNativeChild() || !_ReleaseInteropObjects())return;
    auto* provider=_visibleProvider;
    // Release provider references while the HWND and its retained-owner key are
    // still alive. Failure retains the single owner; no speculative destruction.
    if(provider && provider->SwapchainContext() && !provider->PrepareForSwapchainChange(true))return;
    GenshinVisibleChild::ReleasePhysicalOutputs();
    FGHooks::ClearDx12InteropPresentSC(_fgSwapChain);
    if(State::Instance().currentFGSwapchain==_fgSwapChain)State::Instance().currentFGSwapchain=nullptr;
    SafeRelease(_fgSwapChain);
    if(provider && !provider->ReleaseSwapchain(_visibleProviderHwnd))return;
    _visibleProvider=nullptr;
    retirement.completed=true; // reentrant callbacks must retain until destructor ends
    nativeLock.unlock(); // mutex is a member destroyed by delete
    delete this;
}

void Dx11wDx12SC::_LogNativeVisibleReceipt(bool flush)
{
    const auto emit = [&](const NativeVisibleReceipt& r,const char* event,uint64_t repeated) {
        LOG_INFO("R564 visible: serial={} frame={} epoch={} parent={:X} child={:X} actualParent={:X} actualChild={:X} d11={:X} d12={:X} "
                     "index={} size={}x{} dpi={} B={:X} privateSnapshot={:X} snapshotStaged={} nativeUnchanged={} "
                     "fence={:X} copyQueue={:X} presentQueue={:X} colorCopied={} colorFence={:X} colorValue={} nativeCalled={} nativeHR={:X} childCalled={} childHR={:X} shown={} fallback={} reason={} ownerSnapshotKnown={} srReaderKnown={} srGeneration={} srSourceFrame={} srPictureSerial={} srSelectedRtv={:X} sourceBackbufferPtr={:X} "
                     "sr_to_backbuffer_pixels=UNKNOWN visual=UNKNOWN event={} repeated={} repeatFirstSerial={} repeatLastSerial={}",
                r.serial, r.frame, r.epoch, r.parent, r.child, r.actualParent, r.actualChild, r.device11, r.device12, r.index, r.width, r.height, r.dpi,
                r.backbuffer, r.snapshot, r.snapshotStaged, r.nativeUnchanged, r.fence,
                r.copyQueue, r.presentQueue, r.colorCopied, r.colorFence, r.colorValue,
                r.nativeCalled, static_cast<UINT>(r.nativeResult), r.childCalled, static_cast<UINT>(r.childResult), r.shown,
                r.nativeCalled && r.nativeResult == S_OK && !r.shown, r.reason, r.ownerSnapshotKnown, r.srReaderKnown,
                r.srGeneration,r.srSourceFrame,r.srPictureSerial,r.srSelectedRtv,r.sourceBackbuffer,event,repeated,
                repeated ? _visibleLogFirstRepeat : 0, repeated ? r.serial : 0);
    };
    const auto flushRepeats = [&]() {
        if(_visibleLogRepeats)emit(_visibleLogLast,"repeat-summary",_visibleLogRepeats);
        _visibleLogRepeats=0;_visibleLogFirstRepeat=0;
    };
    if(flush){flushRepeats();return;}
    const auto& a=_visibleLogLast;const auto& b=_visibleReceipt;
    // Serial/frame/epoch, rotating buffers and advancing fences are frame data,
    // not state transitions. Full latest values remain in each summary receipt.
    const bool same=_visibleLogStarted && std::strcmp(a.reason,b.reason)==0 &&
        a.nativeResult==b.nativeResult && a.childResult==b.childResult &&
        a.nativeCalled==b.nativeCalled && a.childCalled==b.childCalled && a.shown==b.shown &&
        a.snapshotStaged==b.snapshotStaged && a.nativeUnchanged==b.nativeUnchanged && a.colorCopied==b.colorCopied &&
        a.ownerSnapshotKnown==b.ownerSnapshotKnown && a.srReaderKnown==b.srReaderKnown &&
        a.parent==b.parent && a.child==b.child && a.actualParent==b.actualParent && a.actualChild==b.actualChild &&
        a.device11==b.device11 && a.device12==b.device12 &&
        a.width==b.width && a.height==b.height && a.dpi==b.dpi &&
        a.copyQueue==b.copyQueue && a.presentQueue==b.presentQueue;
    const auto now=GetTickCount64();
    if(!same) {
        flushRepeats();
        emit(b,"transition",0);_visibleLogTick=now;_visibleLogStarted=true;
        _visibleLogLast=b;return;
    }
    if(!_visibleLogRepeats)_visibleLogFirstRepeat=b.serial;
    ++_visibleLogRepeats;_visibleLogLast=b;
    if(now-_visibleLogTick>=1000){flushRepeats();_visibleLogTick=now;}
}

HRESULT Dx11wDx12SC::_PresentNativeVisible(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    GenshinTransport::PresentOutputScope outputSubmission(!(flags & DXGI_PRESENT_TEST));
    const auto directNativePresent=[&]() -> HRESULT {
        if(!(flags & DXGI_PRESENT_TEST)) {
            if(_visibleChild)_visibleChild->ParentWillPresent({});
            GenshinTransport::FinishPendingOutput(_dx11Device,GenshinTransport::OutputFlushReason::Consumer);
        }
        return parameters && _real1 ? _real1->Present1(sync,flags,parameters) :
            (_real ? _real->Present(sync,flags) : DXGI_ERROR_DEVICE_REMOVED);
    };
    NativeCall call(*this);
    std::unique_lock<std::recursive_mutex> nativeLock(_nativeParentMutex,std::try_to_lock);
    if(!nativeLock.owns_lock()) {
        if(!(flags & DXGI_PRESENT_TEST) && _visibleChild)_visibleChild->RequestHide();
        // No optional receipt under concurrent lifecycle mutation. The game
        // still receives exactly its original native Present result.
        return directNativePresent();
    }
    if(_visiblePresentEntered || _childRecoveryEntered) {
        // This natural parent Present can overtake the outer child's picture.
        // Revoke its display epoch before forwarding; an outer S_OK must not
        // revive that child after the newer parent has already been submitted.
        if(!(flags & DXGI_PRESENT_TEST) && _visibleChild)_visibleChild->RequestHide();
        return directNativePresent();
    }
    if (!(flags & ~DXGI_PRESENT_ALLOW_TEARING) &&
        (!parameters || (!parameters->DirtyRectsCount && !parameters->pScrollRect && !parameters->pScrollOffset)))
        _TryRecoverNativeChild();
    _visiblePresentEntered=true;
    struct Entered { bool& value; ~Entered(){value=false;} } entered{_visiblePresentEntered};
    if(!_visibleChild)
    {
        _RevokeNativeChild();
        return _PresentNativeParent(sync,flags,parameters);
    }
    // A safe recovery may be waiting for supported topology or an explicit
    // update after failed drain. Ordinary native fallback must not turn that
    // display-only suspension into a permanent failure.
    if (_childRevoked) return _PresentNativeParent(sync,flags,parameters);
    if (!_real) return DXGI_ERROR_DEVICE_REMOVED;
    if (flags & DXGI_PRESENT_TEST)
        return parameters && _real1 ? _real1->Present1(sync, flags, parameters) : _real->Present(sync, flags);
    _visibleReceipt = {};
    _visibleReceipt.serial = ++_visibleSerial; _visibleReceipt.frame = State::Instance().frameCount;
    HWND snapshotParent=nullptr,snapshotChild=nullptr;
    _visibleChild->Prepare();
    // Failed Snapshot leaves the zero-initialized observation UNKNOWN, not a zero-sized real window.
    _visibleReceipt.ownerSnapshotKnown=_visibleChild->Snapshot(snapshotParent,snapshotChild,_visibleReceipt.epoch,
        _visibleReceipt.width,_visibleReceipt.height,_visibleReceipt.dpi);
    _visibleReceipt.parent=reinterpret_cast<uintptr_t>(snapshotParent);
    _visibleReceipt.child=reinterpret_cast<uintptr_t>(snapshotChild);
    _visibleReceipt.device11 = GenshinBridgeFg::Identity(_dx11Device);
    _visibleReceipt.device12 = GenshinBridgeFg::Identity(_dx12Device);
    DXGI_SWAP_CHAIN_DESC actualNative{}, actualChild{};
    if (SUCCEEDED(_real->GetDesc(&actualNative))) _visibleReceipt.actualParent = reinterpret_cast<uintptr_t>(actualNative.OutputWindow);
    if (_fgSwapChain && SUCCEEDED(_fgSwapChain->GetDesc(&actualChild)))
        _visibleReceipt.actualChild = reinterpret_cast<uintptr_t>(actualChild.OutputWindow);
    struct ReceiptLog
    {
        Dx11wDx12SC& owner;
        ~ReceiptLog(){owner._LogNativeVisibleReceipt();}
    } receiptLog{*this};
    struct Markers
    {
        IUnknown* device;
        explicit Markers(IUnknown* d) : device(d) { if (device) native::lowlatency::OnPresentBegin(device); }
        ~Markers() { if (device) native::lowlatency::OnPresentEnd(device); }
    } markers{_dx11Device};
    GenshinNativeParentFrame nativeFrame(_real,_fgSwapChain,_dx11Device,_handle,
        _visibleProviderHwnd,_genshinPresentIdentity);
    bool capturePolled = false, depthClosed = false, exactRepeatedPicture = false;
    GenshinVisibleChild::ContentSource visibleSource{};
    const auto nativePresent = [&]() -> HRESULT
    {
        GenshinTransport::FinishPendingOutput(_dx11Device,GenshinTransport::OutputFlushReason::Consumer);
        // Match any hidden reveal to this ACTUAL returned source before the
        // natural parent can overtake it. No per-parent display-epoch expiry.
        _visibleChild->ParentWillPresent(visibleSource,exactRepeatedPicture);
        // Keep the bounded owner visibility lease across normal frames. An
        // async hide/show per frame would cancel every pending owner request.
        // The actual native HRESULT remains exact, including occlusion.
        if (!depthClosed) GenericDepthDx11::OnPresent(_real);
        _visibleReceipt.nativeResult = nativeFrame.Present(sync,flags,parameters,capturePolled);
        _visibleReceipt.nativeCalled = nativeFrame.Called();
        if (SUCCEEDED(_visibleReceipt.nativeResult) && _bufferCount) _AdvanceFakeBackBufferIndex();
        if(_visibleReceipt.nativeResult!=S_OK) {
            _visibleChild->Hide();
            GenshinBridgeFg::Revoke(); // native failure/occlusion is a real discontinuity
        }
        return _visibleReceipt.nativeResult;
    };
    const auto nativeFallback = [&]() -> HRESULT {
        nativeFrame.menuFallback_ = true;
        _visibleChild->SuspendDisplayForNativeFallback();
        GenshinBridgeFg::Revoke();
        if(auto* provider=dynamic_cast<DLSSG_Dx12*>(State::Instance().currentFG))provider->SuspendGenshinPresent();
        return nativePresent();
    };
    const auto nativeIneligible = [&]() -> HRESULT {
        // Native output still presents exactly once. Visibility/pacing may be
        // suspended, but an absent optional input is not provider teardown and
        // must not erase the last genuinely submitted predecessor.
        nativeFrame.menuFallback_ = true;
        _visibleChild->SuspendDisplayForNativeFallback();
        GenshinBridgeFg::PresentScope::Skip();
        GenshinBridgeFg::Revoke(false);
        return nativePresent();
    };
    // Dirty/scroll/sequence-special Presents remain native. The FG copy path
    // supports exactly a whole frame, not inferred dirty-region equivalence.
    const UINT supportedFlags = DXGI_PRESENT_ALLOW_TEARING;
    const bool partial = parameters && (parameters->DirtyRectsCount || parameters->pScrollRect || parameters->pScrollOffset);
    if ((flags & ~supportedFlags) || partial)
    { _visibleReceipt.reason = "UnsupportedPresentParameters"; return nativeFallback(); }
    if (!Config::Instance()->FGEnabled.value_or_default())
    { _visibleReceipt.reason = "FrameGenerationDisabled"; return nativeFallback(); }
    if (_visibleFgBlocked)
    { _visibleReceipt.reason = "PreviousFgWorkUncertain"; return nativeFallback(); }
    if (!_ValidateNativeVisible())
    { _visibleReceipt.reason = "WindowDeviceOrMappingRefused"; return nativeFallback(); }
    if(!(_visibleReceipt.ownerSnapshotKnown=_visibleChild->Snapshot(snapshotParent,snapshotChild,_visibleReceipt.epoch,
        _visibleReceipt.width,_visibleReceipt.height,_visibleReceipt.dpi))) {
        _visibleReceipt.reason="OwnerSnapshotNotCurrent";return nativeFallback();
    }
    // Service the prior output's real tail/owner ACK at this boundary, then
    // consume THIS source normally. Display handoff must not spend an input
    // frame or create a gap in the SDK/camera predecessor chain.
    _visibleChild->ServiceOutputReveal();
    GenshinLegacy::OnPresent(_dx11Device); capturePolled = true;
    _fg = State::Instance().currentFG;
    GenshinBridgeFg::PresentScope scope(_real, _dx11Context, _fgSwapChain, _fg, _dx12Device, _dx12CommandQueue);
    if (!_fg) {_visibleReceipt.reason="NoFgProvider";return nativeFallback();}
    if (!scope.MatchesNative(_real, _dx11Device)) {
        const auto* refusal=GenshinBridgeFg::RequestRefusal();
        _visibleReceipt.reason=refusal ? refusal : "NativeScopeOrProviderMismatch";
        return nativeFallback();
    }
    // Provider constants are prepared only after NativeCopied authenticates
    // the current returned frame; cold frames suspend without guessed flags.
    NativeMotionDx11::OnFGPresent(_real,_dx11Device);
    depthClosed = true;
    if(scope.RepeatsPresentedPicture() && _visibleChild->Visible(_visibleReceipt.epoch)) {
        // A duplicate parent Present has no new picture to dispatch. Keep only
        // the already committed child under its EXISTING bounded visibility
        // lease: no Show/renewal, replay, provider frame, SetOptions or new commit.
        // Keeping the normal native menu path also avoids backend teardown for
        // this input-free Present. All ordinary ineligible frames still hide.
        exactRepeatedPicture=true;
        const auto result=nativePresent();
        _visibleReceipt.shown=result==S_OK && scope.RepeatsPresentedPicture() &&
            _ValidateNativeVisible() && _visibleChild->Visible(_visibleReceipt.epoch);
        _visibleReceipt.reason=_visibleReceipt.shown ? "RepeatedNativePictureExistingVisibility" :
            "RepeatedNativePictureVisibilityExpired";
        if(!_visibleReceipt.shown) {
            _visibleChild->Hide();
            GenshinBridgeFg::PresentScope::Skip();
            GenshinBridgeFg::Revoke(false);
        }
        return result; // the exact single native Present result
    }
    if (!scope.HasCapturedFrame()) {
        _visibleReceipt.reason = "NoAuthenticatedCameraOrGuides";
        return nativeIneligible();
    }
    if (!_InitInteropObjects()) {_visibleReceipt.reason="InteropInitializationRefused";return nativeFallback();}
    const UINT index=_GetDx11BackBufferIndexForPresent();_visibleReceipt.index=index;
    // Actual Return cloned the private picture BEFORE its existing completion
    // signal. No optional FG command reads/writes/retains native B from here.
    auto* snapshot=scope.ReturnedSnapshot();
    if(!snapshot){_visibleReceipt.reason="ReturnedSnapshotUnauthenticated";return nativeFallback();}
    _nativeOriginal=snapshot;
    _visibleReceipt.snapshotStaged=true;_visibleReceipt.snapshot=GenshinBridgeFg::Identity(snapshot);

    const UINT nativeFakeIndex=_currentFakeIndex;
    const bool genshinPrivateRing=_nativeParentRoute && GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    PrivateCopySlotSelection slotSelection;
    slotSelection.slot=nativeFakeIndex;
    bool copySlotAccepted=true;
    if(genshinPrivateRing) {
        slotSelection=SelectRetiredPrivateCopySlot(nativeFakeIndex,_copyFence,_copyAllocatorFenceValues);
        copySlotAccepted=slotSelection.acquired;
        if(!copySlotAccepted)_visibleReceipt.reason=slotSelection.reason;
    }

    // R846 visibility observation begin
    const auto privateCopyBeginMs=GetTickCount64();
    // R846 visibility observation end
    bool copied=false;
    bool sharedReady=copySlotAccepted;
    const char* copyStage=copySlotAccepted ? "SharedBackBuffer" : "PrivateColorSlotAcquire";
    const char* firstCopyReason=nullptr;
    const auto logFirstCopyFailure=[&](const char* stage,const char* reason,UINT slot,UINT64 pending,UINT64 completed,
                                      bool completedKnown,HRESULT result) {
        if(AllowPrivateCopyFailureLog())
            LOG_INFO("R845 private-copy-first-failure: pid={} owner={:X} serial={} frame={} epoch={} stage={} reason={} slot={} preferred={} pending={} completed={} completedKnown={} hr={:X} gameBuffers={} privateSlots={} fence={:X} copyQueue={:X} presentQueue={:X} providerObservedFrame={}",
                GetCurrentProcessId(),reinterpret_cast<uintptr_t>(this),_visibleReceipt.serial,_visibleReceipt.frame,
                _visibleReceipt.epoch,stage,reason,slot,nativeFakeIndex,pending,completed,completedKnown,static_cast<UINT>(result),
                _bufferCount,_copyAllocatorFenceValues.size(),GenshinBridgeFg::Identity(_copyFence),
                GenshinBridgeFg::Identity(_dx12CommandQueue),_fg ? GenshinBridgeFg::Identity(_fg->GetCommandQueue()) : 0,
                _fg ? _fg->FrameCount() : 0);
    };
    if(copySlotAccepted) {
        PrivateCopyFailureScope failure(this,slotSelection.slot,_copyFence,_copyAllocatorFenceValues,genshinPrivateRing);
        {
            // Only these private copy operations resolve the retired slot.
            // The game's own index remains untouched, including during reentry.
            ScopedPrivateCopySlot privateSlot(this,slotSelection.slot);
            sharedReady=_RequestSharedBackBuffer(index);
            if(sharedReady) {
                CopySlotAdmission copyAdmission;
                copyStage=failure.stage="Dx11BackBufferToShared";
                copied=_CopyDx11BackBufferToShared(index,copyAdmission);
                if(copied){copyStage=failure.stage="Dx11ToDx12Sync";copied=_WaitDx11ThenDx12();}
                if(copied){copyStage=failure.stage="SharedToFgCopy";copied=_CopyDx11SharedToDx12FGBackBuffer(index,copyAdmission);}
                if(copied){copyStage=failure.stage="PresentQueueWait";copied=_WaitForInteropCopyOnPresentQueue();}
            }
        }
        if(!copied) {
            firstCopyReason=failure.reason ? failure.reason : copyStage;
            logFirstCopyFailure(failure.stage,firstCopyReason,failure.slot,failure.pending,
                failure.completed,failure.completedKnown,failure.result);
        }
        if(!sharedReady) {
            _visibleReceipt.reason=firstCopyReason;
            return nativeFallback();
        }
    }
    else {
        firstCopyReason=slotSelection.reason;
        logFirstCopyFailure(copyStage,firstCopyReason,slotSelection.slot,slotSelection.pending,
            slotSelection.completed,slotSelection.completedKnown,S_OK);
    }
    _visibleReceipt.copyQueue=GenshinBridgeFg::Identity(_interopPresentQueue.Get());
    _visibleReceipt.presentQueue=_fg ? GenshinBridgeFg::Identity(_fg->GetCommandQueue()) : 0;
    _visibleReceipt.colorFence=GenshinBridgeFg::Identity(_copyFence);
    _visibleReceipt.colorValue=_lastInteropCopyFenceValue;_visibleReceipt.colorCopied=copied;
    const bool copySlotBusy=!copied && std::strcmp(_visibleReceipt.reason,"CopySlotBusy")==0;
    const bool presentQueueChanged=!copied && std::strcmp(_visibleReceipt.reason,"PresentQueueChanged")==0;
    if(!copied) {
        if(copySlotBusy) {
            // Preserve native output and the last genuinely submitted predecessor.
            // The next valid dispatch observes the picture gap and issues one reset.
            _visibleChild->RequestHide();
            GenshinBridgeFg::PresentScope::Skip();
            GenshinBridgeFg::Revoke(false,true); // one reset on the next valid discontinuous dispatch
        }
        else _RevokeNativeChild(presentQueueChanged);
    }
    if(copied) {
        auto* presentQueue=_fg->GetCommandQueue();if(!presentQueue)presentQueue=_dx12CommandQueue;
        // Reuse the exact returned picture already requested by this consumer.
        // Scalars observe its reader chain, never grant SR pixel/completion authority.
        const auto* returned=scope.NativeReturn();
        if(returned) {
            const auto& ticket=returned->PictureTicket();
            _visibleReceipt.srReaderKnown=true;
            _visibleReceipt.srGeneration=ticket.source.generation;
            _visibleReceipt.srSourceFrame=ticket.source.sourceFrame;
            _visibleReceipt.srPictureSerial=ticket.pictureSerial;
            _visibleReceipt.srSelectedRtv=ticket.source.selectedOutput;
            _visibleReceipt.sourceBackbuffer=reinterpret_cast<uintptr_t>(ticket.backbuffer.Get());
            const auto& s=ticket.source;
            visibleSource={s.device,s.context,GenshinBridgeFg::Identity(_dx12Device),ticket.swapchain,
                GenshinBridgeFg::Identity(ticket.backbuffer.Get()),s.selectedOutput,s.deviceEpoch,s.ownerEpoch,
                s.configEpoch,s.historyEpoch,s.instance,s.generation,s.sourceFrame,ticket.pictureSerial,
                ticket.presentEpoch,s.submitSerial,s.copyOrderedSerial,s.thread,s.selectedSubresource};
        }
        copied=GenshinBridgeFg::PresentScope::Finished(DlssNr::ApplyToFinishedPictureDx11Bridge(
            _fgSwapChain,presentQueue,_dx12CommandQueue,returned));
        if(!copied) {
            firstCopyReason="FinishedPictureOutcomeRefused";
            logFirstCopyFailure("FinishedPictureOutcome",firstCopyReason,slotSelection.slot,
                slotSelection.slot<_copyAllocatorFenceValues.size() ? _copyAllocatorFenceValues[slotSelection.slot] : 0,
                _copyFence ? _copyFence->GetCompletedValue() : 0,_copyFence!=nullptr,S_OK);
        }
    }
    // Native B was never modified by optional FG. Its real NR Return remains
    // the selected output, regardless of private child-copy completion.
    _visibleReceipt.nativeUnchanged=true;
    _visibleReceipt.reason=copied ? "NativeUnchangedAwaitingChild" :
        (firstCopyReason ? firstCopyReason : "PrivateColorCopyOrOutcomeRefused");
    // R846 visibility observation begin
    const auto privateCopyEndMs=GetTickCount64();
    // R846 visibility observation end
    // R39 owner-guard callsite begin
    // Snapshot existing CPU sequences only; do not add child API/epoch reads.
    const auto ownerDisplaySerialBeforeNative = _childDisplaySerial;
    const auto presentSerialBeforeNative = _visibleSerial;
    // The private copy + exact presenting-queue Wait + FinishedPicture have
    // completed their CPU handoff. Observe source BEFORE natural parent effects;
    // never suppress parent: failed copy/FG/owner/child keeps its normal filter.
    framegen::final_output::CopiedSourceScope finalCopy(_fgSwapChain, _real, copied,
        _interopPresentQueue.Get(), _copyFence, _lastInteropCopyFenceValue,
        reinterpret_cast<uint64_t>(this), _visibleReceipt.epoch, _visibleReceipt.serial);
    const HRESULT parentResult = nativePresent();
    finalCopy.ParentPresented(parentResult);
    const auto ownerDisplaySerialAfterNative = _childDisplaySerial;
    const auto presentSerialAfterNative = _visibleSerial;
    if(parentResult!=S_OK){_visibleReceipt.reason="NativePresentNotSOK";return parentResult;}
    if(!copied)return parentResult;
    if(!Config::Instance()->FGEnabled.value_or_default()) {
        _visibleReceipt.reason="FrameGenerationDisabledAfterNativePresent";
        _visibleChild->Hide();GenshinBridgeFg::Revoke();return parentResult;
    }
    const auto ownerCheck = GenshinPostPresentOwnerDiagnostic::Evaluate(_visibleChild, _visibleReceipt.epoch);
    if(ownerCheck.failure != GenshinPostPresentOwnerDiagnostic::Failure::None) {
        _visibleReceipt.reason="OwnerLeaseChangedAfterNativePresent";
        static GenshinPostPresentOwnerDiagnostic::ReportBudget reportBudget;
        GenshinPostPresentOwnerDiagnostic::ReportFailure(reportBudget, ownerCheck, [&](unsigned ordinal) {
            LOG_INFO("R39 FG post-native owner guard: pid={} thread={} report={}/{} failure={} prepare_succeeded={} observed_epoch_read={} expected_epoch={} observed_epoch={} serial={} frame={} present_serial_before={} present_serial_after={} display_serial_before={} display_serial_after={} native_hr={:X} observation_only=true",
                GetCurrentProcessId(), GetCurrentThreadId(), ordinal,
                GenshinPostPresentOwnerDiagnostic::ReportBudget::MaxReports, ownerCheck.Reason(),
                ownerCheck.EpochWasRead(), ownerCheck.EpochWasRead(), _visibleReceipt.epoch, ownerCheck.observedEpoch,
                _visibleReceipt.serial, _visibleReceipt.frame, presentSerialBeforeNative, presentSerialAfterNative,
                ownerDisplaySerialBeforeNative, ownerDisplaySerialAfterNative, static_cast<unsigned>(parentResult));
        });
        _visibleChild->Hide();GenshinBridgeFg::Revoke();return parentResult;
    }
    // R39 owner-guard callsite end
    GenshinVisibleChild::PresentPermit permit(*_visibleChild, _fgSwapChain, _visibleReceipt.epoch,
        parentResult, _visibleReceipt.nativeUnchanged, visibleSource);
    // R846 visibility observation begin
    const auto childCallBeginMs=GetTickCount64();
    // R846 visibility observation end
    // The native chain already paced the game frame. The child does not impose
    // a second SyncInterval; generated-frame cadence still needs real validation.
    const HRESULT childCallResult = _fgSwapChain->Present(0, 0);
    // R846 visibility observation begin
    const auto childCallEndMs=GetTickCount64();
    // R846 visibility observation end
    _visibleReceipt.childCalled = permit.Called();
    _visibleReceipt.childResult = permit.Called() ? permit.ActualResult() : childCallResult;
    const bool submitted = scope.LastResult() == GenshinBridgeFg::Result::Submitted;
    const auto nonterminalChildResult=[](HRESULT hr) noexcept {
        return hr==S_OK || hr==DXGI_ERROR_WAS_STILL_DRAWING || hr==DXGI_STATUS_OCCLUDED;
    };
    const char* postStage="sdk-input";
    uint64_t postEpoch=0;
    bool continuityValid=false;
    if(permit.PhysicalFailure()) {
        _visibleReceipt.reason="PhysicalRawOrTailFailed";
        _RevokeNativeChild();
    } else if(childCallResult==S_OK && _visibleReceipt.childResult==S_OK && submitted &&
        (postStage="configuration",Config::Instance()->FGEnabled.value_or_default()) &&
        (postStage="postvalidation",_ValidateNativeVisible()) &&
        (postStage="epoch",postEpoch=_visibleChild->Epoch(),postEpoch==_visibleReceipt.epoch) &&
        (postStage="stream-accept",permit.AcceptStream())) {
        // This activates only the exact SDK creation stream. An earlier raw may
        // now be evaluated, or a worker can publish one later. Every output still
        // needs its own physical queue tail; no copied parent ticket grants it.
        continuityValid=true;
        _visibleReceipt.shown=_visibleChild->Visible(_visibleReceipt.epoch);
        _visibleReceipt.reason=_visibleReceipt.shown ? "SdkStreamVisible" : "SdkInputAcceptedAwaitingStreamOutput";
        postStage="sdk-stream-accepted";
    } else {
        const bool sdkRetry=permit.Called() &&
            (_visibleReceipt.childResult==DXGI_ERROR_WAS_STILL_DRAWING || _visibleReceipt.childResult==DXGI_STATUS_OCCLUDED);
        const bool savedSdkOk=permit.Called() && _visibleReceipt.childResult==S_OK;
        if(scope.LastResult()==GenshinBridgeFg::Result::ProviderRefused) {
            uint64_t resumeRequest=0;
            auto* provider=State::Instance().currentFG==_visibleProvider
                ? dynamic_cast<DLSSG_Dx12*>(_visibleProvider) : nullptr;
            const bool budgetSuspension=permit.Called() && !_childRevoked &&
                _childDisplaySerial<UINT64_MAX && provider &&
                provider->GenshinBudgetRefusedAt(scope.ObservedPresentEpoch(),resumeRequest);
            _RevokeNativeChild();_childBudgetSuspended=budgetSuspension;_childBudgetResumeRequest=resumeRequest;
            _visibleReceipt.reason="NoAuthenticatedDispatch";
        } else if(savedSdkOk || sdkRetry || permit.HiddenRawCount() || permit.OutputAdmissionPending() ||
            nonterminalChildResult(childCallResult)) {
            // A filter/runtime refusal or retry leaves the naturally filtered
            // parent visible. It cannot certify output and is not host teardown.
            _visibleChild->Hide();
            _visibleReceipt.reason=savedSdkOk ? "SdkStreamPostvalidationRefused" : "PhysicalAdmissionPending";
        } else {
            _RevokeNativeChild();_visibleReceipt.reason="SdkPresentFailed";
        }
    }
    // R846 visibility observation begin
    // Read the exact attempt BEFORE Presented can replace its result with
    // PresentFailed. Child S_OK alone does not authenticate a dispatch or Show.
    // Observe only: no provider query, GPU work, lifecycle change or HWND call.
    static thread_local const char* lastDispatch=nullptr;
    static thread_local const char* lastPost=nullptr;
    static thread_local const char* lastPermit=nullptr;
    static thread_local uint64_t lastBoundaryTick=0, boundarySamples=0;
    static thread_local unsigned boundaryBurst=0;
    const auto boundaryTick=GetTickCount64();
    const auto* dispatch=scope.DispatchStage();
    const auto* permitStage=permit.CommitStage();
    const bool boundaryChanged=!lastDispatch || std::strcmp(lastDispatch,dispatch)!=0 ||
        !lastPost || std::strcmp(lastPost,postStage)!=0 ||
        !lastPermit || std::strcmp(lastPermit,permitStage)!=0;
    // At most four transitions per second and a one-second latest sample.
    // A rate bound preserves late evidence without a first-N warmup cutoff.
    if(!boundarySamples || boundaryTick-lastBoundaryTick>=1000)boundaryBurst=0;
    if(!boundarySamples || boundaryTick-lastBoundaryTick>=1000 || (boundaryChanged && boundaryBurst<4)) {
        if(!boundaryBurst)lastBoundaryTick=boundaryTick;
        ++boundaryBurst;++boundarySamples;
        LOG_INFO("R846 FG visibility-boundary: pid={} owner={:X} serial={} frame={} scope_frame={} current_frame={} providerObservedFrame={} epoch={} observed_epoch={} prepare={} dispatch={} attempt_result={} submitted={} post={} permit={} native_hr={:X} child_call_hr={:X} child_actual_hr={:X} child_called={} shown={} continuity={} reason={} private_copy_ms={} parent_and_owner_ms={} child_call_ms={} post_ms={} samples={} observation_only=true visual=UNKNOWN",
            GetCurrentProcessId(),reinterpret_cast<uintptr_t>(this),_visibleReceipt.serial,_visibleReceipt.frame,
            scope.ObservedPresentEpoch(),State::Instance().frameCount,_fg ? _fg->FrameCount() : 0,
            _visibleReceipt.epoch,postEpoch,scope.PrepareStage(),dispatch,static_cast<unsigned>(scope.LastResult()),
            submitted,postStage,permitStage,static_cast<UINT>(parentResult),static_cast<UINT>(childCallResult),
            static_cast<UINT>(_visibleReceipt.childResult),permit.Called(),_visibleReceipt.shown,continuityValid,
            _visibleReceipt.reason,privateCopyEndMs-privateCopyBeginMs,childCallBeginMs-privateCopyEndMs,
            childCallEndMs-childCallBeginMs,boundaryTick-childCallEndMs,boundarySamples);
    }
    lastDispatch=dispatch;lastPost=postStage;lastPermit=permitStage;
    // R846 visibility observation end
    scope.Presented(permit.Called() ? permit.ActualResult() : E_FAIL,continuityValid,
        permit.Called() && childCallResult==S_OK && !permit.PhysicalFailure());
    // Game-facing HRESULT belongs to its real native Present. The child result
    // is separately recorded, never reclassified as native failure or success.
    return parentResult;
}
