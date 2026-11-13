#include <runtime/ComIdentity.h>
#include "pch.h"
#include <runtime/ModuleLifetime.h>
#include <native/NrConsumerFilter.h>
#include <integration/HoYoShade.h>
#include "dx11_with_dx12_sc.h"
#include <runtime/GameIdentity.h>
#include <wrapped/wrapped_swapchain.h>
#include <framegen/FinalOutputEndpoint.h>
#include <framegen/dlssg/StarRailF5DsPolicy.h>
#include <framegen/dlssg/DLSSG_Dx12.h>
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
#include <runtime/HookCpuTiming.h>

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



bool ReportInteropCopyFailure(const char* reason, HRESULT result = S_OK,
                              std::optional<UINT64> completed = std::nullopt)
{
    static std::atomic<ULONGLONG> lastReport { 0 };
    const auto now = GetTickCount64();
    auto previous = lastReport.load(std::memory_order_relaxed);
    if ((!previous || now - previous >= 1000) &&
        lastReport.compare_exchange_strong(previous, now, std::memory_order_relaxed))
        LOG_WARN("DX11 interop copy refused: reason={} result=0x{:08X} completed_known={} completed={}",
            reason, static_cast<UINT>(result), completed.has_value(), completed.value_or(0));
    return false;
}

bool WaitForQueueIdle(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue)
{
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::QueueIdle, queue, reinterpret_cast<uint64_t>(fence), State::Instance().frameCount);
    if (!queue || !fence || !fenceEvent)
        return false;

    auto submit = [&] {
        if (fenceValue >= UINT64_MAX - 1) return false;
        const auto result = queue->Signal(fence, ++fenceValue);
        if (FAILED(result))
            LOG_ERROR("FG/present queue idle Signal failed: {:X}", (UINT) result);
        return SUCCEEDED(result);
    };
    if (!submit())
        return false;
    const auto waitValue = fenceValue;
    const auto completed=fence->GetCompletedValue();
    if(completed==UINT64_MAX)return false;
    if (completed >= waitValue)
        return true;

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
                         bool sourceFrameLatencyOwner, std::optional<UINT> applicationFlags)
    : SwapchainHookBoundary(OperationsFor<Dx11wDx12SC>()),
      _sourceFrameLatencyOwner(sourceFrameLatencyOwner && real != nullptr && fgSC != nullptr),
      _starRailF5DsTransport(_sourceFrameLatencyOwner && StarRailF5Ds::Selected()),
      _real(real), _fgSwapChain(fgSC), _refcount(1),
      _dx11Device(pDevice), _handle(hWnd)
{
    _id = ++scCount;
    HoYoShade::RegisterBridgeSource(this, _real, _dx11Device);
    _modSourceRegistered=GenericDepthDx11::RegisterModPresentSource(_real, _handle);
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
        }
    else
    {
        LOG_ERROR("failed to resolve D3D12 device/queue");
    }

    State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

    _RefreshCachedSwapchainDesc();
    _RefreshFrameLatencyFlags();
    LOG_INFO("Dx11wDx12SC {} created, real: {:X}, fg: {:X}, dx11: {:X}, dx12: {:X}, queue: {:X}", _id, (UINT64) _real,
             (UINT64) _fgSwapChain, (UINT64) _dx11Device, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
}

Dx11wDx12SC::~Dx11wDx12SC()
{
    HoYoShade::ForgetBridgeSource(this);

        DlssNrFgTrace::EndBridge();
        MenuOverlayDx::CleanupRenderTarget(true,_handle);
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
        if (_real4 == nullptr && _fgSwapChain == nullptr)
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
    if (ret == 0)
    {


        auto fg = State::Instance().currentFG;
        const bool releaseFg = fg != nullptr &&
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

        GenericDepthDx11::ForgetModPresentSource(_real);
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

        const auto handle = _handle;

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
    if (_sourceFrameLatencyOwner && !(Flags & DXGI_PRESENT_TEST) &&
        HookCpuTiming::starRailSourceDepth == 0)
        FGHooks::DrainStarRailAttribution();
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Present, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, Flags, 0, 0, true);
    HookCpuTiming::StarRailInvocationScope attributionParent(
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


    if (!_modSourceRegistered)
        _modSourceRegistered=GenericDepthDx11::RegisterModPresentSource(_real,_handle);

    if (!_InitInteropObjects())
        return DXGI_ERROR_DEVICE_REMOVED;

    auto dx11Index = _GetDx11BackBufferIndexForPresent();

    if (!_RequestSharedBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

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
    if (!DlssNr::UsesGenshinPresentPipeline())
    {
        native::nrfilter::OpticalInputScope inputSource(_real, _dx11Device);
        _ApplyNativeInputToFGBackBuffer();
    }
    else
    {
        // Present NR bypasses the optical adapter. Its native DX11 owner
        // still closes observations and prepares late-enabled MOD hooks once.
        GenericDepthDx11::OnPresent(_real);
    }
    // The optional overwrite must publish its OWN final signal. Never let an
    // earlier successful raw-copy receipt authorize reads of a partial overwrite.
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues)) ||
        !native::nrfilter::FinalCopyReceiptValid(_copyFenceValue, _lastInteropCopyFenceValue)) return E_FAIL;

    if (!_WaitForInteropCopyOnPresentQueue())
        return DXGI_ERROR_DEVICE_REMOVED;
    if (_sourceFrameLatencyOwner)
        HookCpuTiming::ObserveStarRailCopyReceipt(this,_dx12CommandQueue,_copyFence,_lastInteropCopyFenceValue);

    auto* nrPresentQueue = _fg != nullptr ? _fg->GetCommandQueue() : nullptr;
    if (!nrPresentQueue) nrPresentQueue = _dx12CommandQueue;
    CallerCpuTrace::Scope nrPresentCpu(CallerCpuTrace::Op::NrPresent, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    const auto bridgeOutcome = [&] {
        // Generic interop copies this exact _real buffer/index.
        // End the borrowed source scope before any later natural/FG Present.
        native::nrfilter::BridgeScope nrFilterSource(_real, _dx11Device, dx11Index,
            _fgSwapChain, WithDx12::GetD3D12CommandQueue(), nrPresentQueue, State::Instance().frameCount,
            _dx12CommandQueue, _copyFence, _lastInteropCopyFenceValue);
        return DlssNr::ApplyToFinishedPictureDx11Bridge(_fgSwapChain, nrPresentQueue, WithDx12::GetD3D12CommandQueue(),
            colourSource ? colourSource->SourceMetadata() : GuideSourceMetadata::Ptr{});
    }();
    if (HoYoShade::ColorConsumers(_fgSwapChain) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
    // The real interop Wait above orders this exact copy on nrPresentQueue.
    // An optional native overwrite has a different receipt/source, so it cannot
    // borrow the selected NGX source's colour identity.
    if (colourOwner && State::Instance().currentFG==colourOwner && copiedValue==_lastInteropCopyFenceValue)
        colourOwner->JoinUpscalerFinalColour(colourSource,_fgSwapChain,nrPresentQueue,bridgeOutcome,
            copiedColour.Get(),_copyFence,copiedValue);

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
        const bool noWaitHidden = _sourceFrameLatencyOwner && !_starRailF5DsTransport && _real3 != nullptr &&
            (Flags & ~UINT(DXGI_PRESENT_ALLOW_TEARING)) == 0 &&
            SUCCEEDED(_real->GetDesc(&hiddenDesc)) && hiddenDesc.Windowed &&
            hiddenDesc.OutputWindow != nullptr && hiddenDesc.OutputWindow != _handle &&
            (hiddenDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
             hiddenDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
        if (noWaitHidden) realFlags |= DXGI_PRESENT_DO_NOT_WAIT;

        const bool collectHidden = _sourceFrameLatencyOwner && HookCpuTiming::StarRailTimingActive();
        auto hiddenStamp = collectHidden ? HookCpuTiming::MakeStarRailStamp(
            HookCpuTiming::StarRailOrigin::Hidden,_fgSwapChain) : HookCpuTiming::StarRailStamp{};
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
            HookCpuTiming::RecordStarRailCall(HookCpuTiming::StarRailCall::HiddenPresent,hiddenStamp,
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

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetFullscreenState(Fullscreen, pTarget);

    return _real != nullptr ? _real->SetFullscreenState(Fullscreen, pTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookGetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
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
    HoYoShade::BridgeSourceResize sourceResize(this);
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (const auto failure = _fgResizeStatus.RecoveryFailure(Config::Instance()->FGSkipResizeBuffers.value_or_default());
        FAILED(failure)) return failure;



    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Resize, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    auto lifecycleOwner = State::Instance().currentFG;
    auto lifecycleExecution = lifecycleOwner != nullptr ? lifecycleOwner->GuardExecution(FGExecutionScope::Lifecycle) : FGExecutionGuard {};
    auto fgArgs = ResizeArgsForFg(_fgSwapChain, BufferCount, NewFormat, SwapChainFlags);

    const bool skipFgResize = _fgResizeStatus.CanSkipResize() &&
        IsSame(_fgSwapChain, fgArgs.bufferCount, Width, Height, fgArgs.format, fgArgs.flags);
    if (!skipFgResize && lifecycleOwner != nullptr && !lifecycleOwner->PrepareForSwapchainChange())
        return DXGI_ERROR_WAS_STILL_DRAWING;

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


    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    return _fgSwapChain != nullptr
               ? _fgSwapChain->ResizeTarget(pNewTargetParameters)
               : (_real != nullptr ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetContainingOutput(ppOutput)
               : (_real != nullptr ? _real->GetContainingOutput(ppOutput) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _fgSwapChain != nullptr ? _fgSwapChain->GetFrameStatistics(pStats)
                                   : (_real != nullptr ? _real->GetFrameStatistics(pStats) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetLastPresentCount(UINT* pLastPresentCount)
{
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
    return _real1 != nullptr ? _real1->SetRotation(Rotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1 != nullptr ? _real1->GetRotation(pRotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetSourceSize(UINT Width, UINT Height)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
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
        DXGI_ERROR_INVALID_CALL, DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    const auto target = _SelectFrameLatencyTarget(_ReturnAddress());
    return framegen::latency::Forward<IDXGISwapChain2, HRESULT>(target, _real2, _fgSwapChain,
        [pMaxLatency](auto& chain) { return chain.GetMaximumFrameLatency(pMaxLatency); },
        DXGI_ERROR_INVALID_CALL, DXGI_ERROR_DEVICE_REMOVED);
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
        _real2 != nullptr, _fgSwapChain != nullptr, false);
    StreamlineHooks::ReportFrameLatencyAccess(this, flags, caller, sdkLoaded, target);
    const auto preserved = (!_sourceFrameLatencyOwner || _starRailF5DsTransport) &&
        _fgSwapChain != nullptr ? framegen::latency::Target::Provider : framegen::latency::Target::Native;
    return framegen::latency::Resolve(target, preserved);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
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
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);

    return _real3 != nullptr ? _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport)
                             : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookSetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetColorSpace1(ColorSpace);

    return _real3 != nullptr ? _real3->SetColorSpace1(ColorSpace) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                      UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                      IUnknown* const* ppPresentQueue)
{
    HoYoShade::BridgeSourceResize sourceResize(this);
    if (!ModuleLifetime::CleanupAllowed()) return DXGI_ERROR_WAS_STILL_DRAWING;
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (_real3 == nullptr) return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);

    if (const auto failure = _fgResizeStatus.RecoveryFailure(Config::Instance()->FGSkipResizeBuffers.value_or_default());
        FAILED(failure)) return failure;



    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Resize, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);


    auto lifecycleOwner = State::Instance().currentFG;
    auto lifecycleExecution = lifecycleOwner != nullptr ? lifecycleOwner->GuardExecution(FGExecutionScope::Lifecycle) : FGExecutionGuard {};
    auto fgArgs = ResizeArgsForFg(_fgSwapChain, BufferCount, Format, SwapChainFlags);

    const bool skipFgResize = _fgResizeStatus.CanSkipResize() &&
        IsSame(_fgSwapChain, fgArgs.bufferCount, Width, Height, fgArgs.format, fgArgs.flags);
    if (!skipFgResize && lifecycleOwner != nullptr && !lifecycleOwner->PrepareForSwapchainChange())
        return DXGI_ERROR_WAS_STILL_DRAWING;

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


    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookSetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
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

            const UINT colorSlotCount = _bufferCount;
        if (_sharedDx11BackBufferCopies.size() != colorSlotCount)
            _sharedDx11BackBufferCopies.resize(colorSlotCount, nullptr);

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

    const UINT colorSlotCount = _bufferCount;
    _sharedDx11BackBufferCopies.assign(colorSlotCount, nullptr);
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



bool Dx11wDx12SC::_RequestSharedBackBuffer(UINT index)
{
    const UINT copySlot=_currentFakeIndex;
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::SharedBuffer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (_real == nullptr || _dx11Device == nullptr || _dx12Device == nullptr)
        return ReportInteropCopyFailure("SharedDevicesUnavailable");

    if (_bufferCount == 0)
        _RefreshCachedSwapchainDesc();

    if (index >= _bufferCount)
    {
        LOG_ERROR("backbuffer index {} out of range {}", index, _bufferCount);
        return ReportInteropCopyFailure("GameBufferIndexOutOfRange");
    }

    const UINT colorSlotCount = _bufferCount;
    if (copySlot >= colorSlotCount)
        return ReportInteropCopyFailure("PrivateColorSlotOutOfRange");
    const UINT requiredSlots = index + 1;

    if (_sharedDx11BackBufferCopies.size() < requiredSlots)
        _sharedDx11BackBufferCopies.resize(colorSlotCount, nullptr);

    if (_openedDx11BackBuffers.size() < requiredSlots)
        _openedDx11BackBuffers.resize(colorSlotCount, nullptr);

    if (_sharedBackBufferHandles.size() < requiredSlots)
        _sharedBackBufferHandles.resize(colorSlotCount, nullptr);

    // Bind on the original successful open even when FG is disabled/paused.
    // Enabling later must not leave every cached ordinary slot unauthenticated.
    // This is already the real DX11->DX12 wrapper open operation. Do not gate
    // its association on mutable global provider eligibility or interop state.
    if (_openedDx11BackBufferStates.size() < requiredSlots)
        _openedDx11BackBufferStates.resize(colorSlotCount, D3D12_RESOURCE_STATE_COMMON);

    if (_openedDx11BackBuffers[copySlot] != nullptr)
        return true;

    // Read Dx11 sc backbuffer
    ID3D11Texture2D* sourceTexture = nullptr;
    CallerCpuTrace::Scope getBufferCpu(CallerCpuTrace::Op::SharedGetBuffer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, index);
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    getBufferCpu.Finish();
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer({}) failed: {:X}", index, (UINT) result);
        return ReportInteropCopyFailure("SharedSourceGetBufferFailed", result);
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
            return ReportInteropCopyFailure("SharedTextureCreateFailed", result);
        }

        result = _sharedDx11BackBufferCopies[copySlot]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
        if (FAILED(result) || dxgiResource == nullptr)
        {
            LOG_ERROR("shadow IDXGIResource1 query {} failed: {:X}", copySlot, (UINT) result);
            sourceTexture->Release();
            return ReportInteropCopyFailure("SharedResourceQueryFailed", result);
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
            return ReportInteropCopyFailure("SharedHandleCreateFailed", result);
        }
    }

    // Dx12 handle to Dx11 shared texture
    CallerCpuTrace::Scope openHandleCpu(CallerCpuTrace::Op::SharedOpenHandle, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
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

        return ReportInteropCopyFailure("SharedHandleOpenFailed", result);
    }

    _deviceRemovedLogged = false;

    _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool Dx11wDx12SC::_CopyDx11BackBufferToShared(UINT index, CopySlotAdmission& admission)
{
    admission.Clear();
    const UINT copySlot=_currentFakeIndex;
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Dx11Copy, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (copySlot >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[copySlot] == nullptr)
    {
        return ReportInteropCopyFailure("SharedTextureUnavailable");
    }

    if (_dx11Context == nullptr || _real == nullptr)
        return ReportInteropCopyFailure("Dx11CopyContextUnavailable");

    // Keep StarRail's source timeline boundary separate from common GPU reuse.
    if (_sourceFrameLatencyOwner)
    {
        if (_sourceCopyUncertain.load(std::memory_order_acquire) || _sharedFenceValue >= UINT64_MAX - 1 || _copyFenceValue >= UINT64_MAX - 1)
            return ReportInteropCopyFailure("SourceCopyTimelineUnavailable");
    }

    // Interop shared writer admission begin
    // Move the existing allocator retirement BEFORE the shared texture write.
    // Both name this exact slot's previous D3D12 reader. The native-parent
    // optional route keeps its nonblocking Busy policy inside the same helper.
    // The local proof is consumed at Reset below; no second completion wait.
    if (_sourceCopyUncertain.load(std::memory_order_acquire) || !_copyFence || !_copyFenceEvent ||
        copySlot >= _copyAllocators.size() || !_copyAllocators[copySlot] ||
        !_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues))
        return ReportInteropCopyFailure("CopyReaderNotRetired");
    admission.Record(this, &_copyAllocatorFenceValues, _copyFence, _copyAllocators[copySlot],
                     copySlot, _copyAllocatorFenceValues[copySlot]);
    // Interop shared writer admission end

    // Re-read selected consumers at the real copy boundary, including changes
    // since import. Restore a distinct shadow target before the original copy.
    ID3D11Texture2D* sourceTexture = nullptr;
    CallerCpuTrace::Scope getBufferCpu(CallerCpuTrace::Op::Dx11CopyGetBuffer, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, index);
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    getBufferCpu.Finish();
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer for shadow copy {} failed: {:X}", index, (UINT) result);
        return ReportInteropCopyFailure("Dx11SourceGetBufferFailed", result);
    }

    LOG_DEBUG("Copying DX11 backbuffer {} sourceTexture: {:X} to shadow copy {:X}", index, (size_t) sourceTexture,
              (size_t) _sharedDx11BackBufferCopies[copySlot]);

    D3D11_TEXTURE2D_DESC sourceDesc {};
    sourceTexture->GetDesc(&sourceDesc);


    if (sourceDesc.SampleDesc.Count > 1)
    {
        // The back buffer is multisampled and the shared copy is not: a copy would be dropped by D3D11 without a word.
        ID3D11Texture2D* sharedCopy = _sharedDx11BackBufferCopies[copySlot];

        if (const char* why = _msaaResolve.ResolveIn(_dx11Context, sourceTexture, sharedCopy))
        {
            LOG_ERROR("resolving the multisampled back buffer {} into the shadow copy failed: {}", index, why);
            sourceTexture->Release();
            return ReportInteropCopyFailure("Dx11MsaaResolveRefused");
        }
    }
    else
    {
        _dx11Context->CopyResource(_sharedDx11BackBufferCopies[copySlot], sourceTexture);
    }

    sourceTexture->Release();
    return true;
}

bool Dx11wDx12SC::_WaitDx11ThenDx12()
{
    const UINT copySlot=_currentFakeIndex;
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Dx11Sync, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (_dx11Context4 == nullptr || _dx11Fence == nullptr || _dx12CommandQueue == nullptr ||
        _dx12SharedFence == nullptr)
        return ReportInteropCopyFailure("Dx11SyncObjectsUnavailable");

    if(_sourceFrameLatencyOwner && _sharedFenceValue>=UINT64_MAX-1)return ReportInteropCopyFailure("SharedFenceValueExhausted");
    const auto waitValue = _sharedFenceValue++;

    CallerCpuTrace::Scope dx11SignalCpu(CallerCpuTrace::Op::Dx11Signal, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, waitValue);
    auto result = _dx11Context4->Signal(_dx11Fence, waitValue);
    dx11SignalCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("DX11 Signal failed: {:X}", (UINT) result);
        return ReportInteropCopyFailure("Dx11FenceSignalFailed", result);
    }



    CallerCpuTrace::Scope flushCpu(CallerCpuTrace::Op::Dx11Flush, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, waitValue);
    _dx11Context4->Flush();
    flushCpu.Finish();

    // Keep the producer-queue handoff, including lifecycle retirement. The
    // private transaction shares this exact submitted order with the color
    // writer below; a different presenting queue still needs its own Wait.
    CallerCpuTrace::Scope queueWaitCpu(CallerCpuTrace::Op::CopyQueueWaitSubmit, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, waitValue);
    result = _dx12CommandQueue->Wait(_dx12SharedFence, waitValue);
    queueWaitCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("interop copy queue Wait on D3D11 fence failed: {:X}", (UINT) result);
        return ReportInteropCopyFailure("CopyQueueSharedFenceWaitFailed", result);
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
        return ReportInteropCopyFailure("SourceCopyFenceUnavailable");
    if (_copyFence == nullptr || _copyFenceEvent == nullptr)
        return true;

    if (slot >= fenceValues.size())
    {
        LOG_ERROR("copy allocator slot {} out of range {}", slot, fenceValues.size());
        return ReportInteropCopyFailure("CopyAllocatorSlotOutOfRange");
    }

    const auto fenceValue = fenceValues[slot];

    if (_sourceFrameLatencyOwner && fenceValue == UINT64_MAX)
    {
        _sourceCopyUncertain.store(true, std::memory_order_release);
        return ReportInteropCopyFailure("SourceCopyReceiptUncertain");
    }
    if (fenceValue == 0 && !_sourceFrameLatencyOwner)
        return true;

    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue == UINT64_MAX)
    {
        _ReportCopyFenceFault("copy-fence-invalid",slot,fenceValue,completedValue,WAIT_FAILED);
        if (_sourceFrameLatencyOwner) _sourceCopyUncertain.store(true, std::memory_order_release);
        return ReportInteropCopyFailure("CopyFenceFailed", S_OK, completedValue);
    }
    if (fenceValue == 0 || completedValue >= fenceValue)
        return true;

    // The native-parent FG ring is optional work. Never stall the render thread
    // to reuse a live slot; the caller distinguishes Busy from terminal failure.
    auto result = _copyFence->SetEventOnCompletion(fenceValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator fence SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}",
                  slot, fenceValue, completedValue, (UINT) result);
        return ReportInteropCopyFailure("CopyAllocatorEventArmFailed", result, completedValue);
    }

    const bool collectCopy = _sourceFrameLatencyOwner && HookCpuTiming::StarRailTimingActive();
    auto copyStamp = collectCopy ? HookCpuTiming::MakeStarRailStamp(
        HookCpuTiming::StarRailOrigin::CopyRetirement,_fgSwapChain) : HookCpuTiming::StarRailStamp{};
    if (collectCopy) {
        // Compare the actual vector instance, never a shader/NGX handle or a numeric slot alone.
        copyStamp.ring=&fenceValues==&_copyAllocatorFenceValues ? HookCpuTiming::StarRailCopyRing::SharedColor :
            (&fenceValues==&_nrCopyAllocatorFenceValues ? HookCpuTiming::StarRailCopyRing::NativeOverwrite :
             HookCpuTiming::StarRailCopyRing::Unknown);
        copyStamp.queue=reinterpret_cast<uintptr_t>(_dx12CommandQueue);copyStamp.fence=reinterpret_cast<uintptr_t>(_copyFence);
        copyStamp.slot=slot;copyStamp.required=fenceValue;copyStamp.completed=completedValue;copyStamp.completedKnown=true;
    }
    CallerCpuTrace::Scope copyWaitCpu(CallerCpuTrace::Op::CopyAllocatorWait, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, slot, fenceValue, completedValue, false, true);
    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    copyWaitCpu.Finish(waitResult, true);
    if (collectCopy) {
        copyStamp.result=waitResult;copyStamp.resultKnown=true;
        HookCpuTiming::RecordStarRailCall(HookCpuTiming::StarRailCall::CopyWait,copyStamp,
            {true,static_cast<int64_t>(copyWaitCpu.BeginQpc()),static_cast<int64_t>(copyWaitCpu.EndQpc())},true);
    }
    if (waitResult != WAIT_OBJECT_0)
    {
        _ReportCopyFenceFault("copy-wait-failed",slot,fenceValue,_copyFence->GetCompletedValue(),waitResult);
        LOG_ERROR("copy allocator fence wait failed. slot {}, fence {}, completed {}, waitResult {:X}", slot,
                  fenceValue, _copyFence->GetCompletedValue(), waitResult);
        return ReportInteropCopyFailure("CopyAllocatorWaitFailed", S_OK, completedValue);
    }

    const auto actual = _copyFence->GetCompletedValue();
    if(actual==UINT64_MAX||actual<fenceValue)
        _ReportCopyFenceFault("copy-wait-not-complete",slot,fenceValue,actual,waitResult);
    if (_sourceFrameLatencyOwner && actual == UINT64_MAX) _sourceCopyUncertain.store(true, std::memory_order_release);
    return (actual != UINT64_MAX && actual >= fenceValue) ? true :
        ReportInteropCopyFailure(actual == UINT64_MAX ? "CopyFenceFailed" : "CopyAllocatorCompletionNotRetired", S_OK, actual);
}

bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot, std::vector<UINT64>& fenceValues, CopySlotAdmission& admission)
{
    // Consume only the caller's exact pre-write proof. A changed timeline,
    // ring/allocator/fence or slot cannot be repaired by waiting after writing.
    if (_sourceCopyUncertain.load(std::memory_order_acquire) || slot >= fenceValues.size() ||
        slot >= _copyAllocators.size())
    {
        admission.Clear();
        return ReportInteropCopyFailure("CopyAdmissionUnavailable");
    }
    return admission.Consume(this, &fenceValues, _copyFence, _copyAllocators[slot], slot, fenceValues[slot]) ? true :
        ReportInteropCopyFailure("CopyAdmissionChanged");
}

bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(UINT dx11Index, CopySlotAdmission& admission,
                                               ID3D12Resource** copiedColour)
{
    if (copiedColour) *copiedColour=nullptr;
    const UINT copySlot=_currentFakeIndex;
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::InteropCopy, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount);
    if (_copyAllocators.empty() || _copyCommandLists.empty() || _dx12CommandQueue == nullptr || _copyFence == nullptr ||
        _fgSwapChain == nullptr || copySlot >= _openedDx11BackBuffers.size() ||
        _openedDx11BackBuffers[copySlot] == nullptr)
    {
        return ReportInteropCopyFailure("FgCopyObjectsUnavailable");
    }

    if(_sourceFrameLatencyOwner && _copyFenceValue>=UINT64_MAX-1)return ReportInteropCopyFailure("CopyFenceValueExhausted");
    DLSSG_Dx12* colorProvider = nullptr;
    auto colorGuard = colorProvider ? colorProvider->GuardExecution(FGExecutionScope::Inputs) : FGExecutionGuard{};
    ComPtr<ID3D12CommandQueue> nativeColorQueue;
    auto* colorQueue = _dx12CommandQueue;
    auto allocator = _copyAllocators[copySlot];

    if (allocator == nullptr)
        return ReportInteropCopyFailure("CopyAllocatorUnavailable");

    if (!_WaitForCopyAllocator(copySlot, _copyAllocatorFenceValues, admission))
    {
        return ReportInteropCopyFailure("CopyAllocatorRetirementRefused");
    }

    CallerCpuTrace::Scope resetCpu(CallerCpuTrace::Op::CopyAllocatorReset, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
    auto result = allocator->Reset();
    resetCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator[{}] reset failed: {:X}", copySlot, (UINT) result);
        return ReportInteropCopyFailure("CopyAllocatorResetFailed", result);
    }

    CallerCpuTrace::Scope listResetCpu(CallerCpuTrace::Op::CopyListReset, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);
    result = _copyCommandLists[copySlot]->Reset(allocator, nullptr);
    listResetCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list reset failed: {:X}", (UINT) result);
        return ReportInteropCopyFailure("CopyCommandListResetFailed", result);
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
        return ReportInteropCopyFailure("FgBackBufferGetBufferFailed", result);
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
    result = _copyCommandLists[copySlot]->Close();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list close failed: {:X}", (UINT) result);
        return ReportInteropCopyFailure("CopyCommandListCloseFailed", result);
    }

    ID3D12CommandList* lists[] = { _copyCommandLists[copySlot] };
    CallerCpuTrace::Scope copyExecuteCpu(CallerCpuTrace::Op::CopyExecute, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot);

    // Pin BEFORE Execute. A failed Signal leaves the actual lease/read queue
    // retained by Frame as well as the original quarantined wrapper copy ring.
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
        return ReportInteropCopyFailure("CopyFenceSignalFailed", result);
    }

    _copyAllocatorFenceValues[copySlot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;
    // Scope-local reference only, returned after the actual successful Signal.
    // It does not outlive this Present or replace allocator/SDK retirement.
    if (copiedColour) { fgBackBuffer->AddRef(); *copiedColour=fgBackBuffer.Get(); }


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
    const UINT copySlot=_currentFakeIndex;
    if (_copyFence == nullptr)
        return ReportInteropCopyFailure("PresentCopyFenceUnavailable");

    if (_lastInteropCopyFenceValue == 0)
        return true;

    auto* queue = _fg != nullptr ? _fg->GetCommandQueue() : nullptr;
    if(_interopPresentQueue && _interopPresentQueue.Get()!=queue) {
        const auto copiedValue=_lastInteropCopyFenceValue;
        if(!_WaitForCopyQueueIdle())return ReportInteropCopyFailure("PreviousCopyQueueRetirementRefused");
        _lastInteropCopyFenceValue=copiedValue; // preserve THIS copy's receipt after draining the previous queue
    }
    // Adopt even a null owner after successful retirement; otherwise the old
    // queue remains pinned and every subsequent null-queue Present drains it.
    _interopPresentQueue = queue; // arm ownership BEFORE a possibly partially accepted Wait
    if (queue == nullptr || queue == _dx12CommandQueue)
    {

        return true;
    }

    CallerCpuTrace::Scope presentWaitCpu(CallerCpuTrace::Op::PresentQueueWaitSubmit, this, reinterpret_cast<uint64_t>(_handle), State::Instance().frameCount, copySlot, _lastInteropCopyFenceValue);
    auto result = queue->Wait(_copyFence, _lastInteropCopyFenceValue);
    presentWaitCpu.Finish();
    if (FAILED(result))
    {
        LOG_ERROR("present queue Wait on interop copy fence failed: {:X}", (UINT) result);
        return ReportInteropCopyFailure("PresentQueueCopyFenceWaitFailed", result);
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyQueueIdle()
{
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return false;
    const bool haveObjects=!_copyCommandLists.empty() || !_nrCopyCommandLists.empty() ||
        !_sharedDx11BackBufferCopies.empty() || !_openedDx11BackBuffers.empty() || _interopPresentQueue!=nullptr;
    if(!haveObjects)return true;
    if(!_dx12CommandQueue || !_copyFence || !_copyFenceEvent ||
       !_dx11Context4 || !_dx11Fence || !_dx12SharedFence)return false;
    auto submitCopyDrain = [&] {
        // An aborted Present can leave DX11 writes without a handoff. The
        // idle marker must cover those too, not just earlier allocator values.
        if (_copyFenceValue >= UINT64_MAX - 1 || !_WaitDx11ThenDx12()) return false;
        // Do not advance a shared timeline from a different queue until the
        // exact last color submission on the retained queue is ordered first.
        const auto waitValue=++_copyFenceValue;
        if (FAILED(_dx12CommandQueue->Signal(_copyFence,waitValue))) return false;
        _lastInteropCopyFenceValue=waitValue;
        return true;
    };
    if (!submitCopyDrain()) return false;
    const auto waitValue = _copyFenceValue;
    auto completed=_copyFence->GetCompletedValue();
    if(completed==UINT64_MAX)return false;
    if(completed<waitValue) {
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
        if (!_presentRetirementEvent)
            _presentRetirementEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!_presentRetirementEvent ||
            !WaitForQueueIdle(_interopPresentQueue.Get(), _presentRetirementFence,
                _presentRetirementEvent, _presentRetirementValue)) return false;
    }
    for(auto& value:_copyAllocatorFenceValues)value=0;
    for(auto& value:_nrCopyAllocatorFenceValues)value=0;
    _lastInteropCopyFenceValue=0;
    return true;
}

void Dx11wDx12SC::_ReleaseInteropBackBuffers()
{
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return;
    _interopInitialized = false;

    for (auto& resource : _openedDx11BackBuffers)
        SafeRelease(resource);

    for (auto& texture : _sharedDx11BackBufferCopies)
        SafeRelease(texture);

    for (auto& handle : _sharedBackBufferHandles)
        SafeCloseHandle(handle);

    _openedDx11BackBufferStates.clear();
    _openedDx11BackBuffers.clear();
    _sharedDx11BackBufferCopies.clear();
    _sharedBackBufferHandles.clear();
}

bool Dx11wDx12SC::_ReleaseInteropObjects()
{
    if ((_sourceCopyUncertain.load(std::memory_order_acquire) || native::nrfilter::HasUncertainCopy(_nrCopyAllocatorFenceValues))) return false;
    if(!_WaitForCopyQueueIdle())return false;
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
