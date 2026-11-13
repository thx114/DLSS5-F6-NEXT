#include "pch.h"
#include <runtime/ModuleLifetime.h>
#include <runtime/D3D12DeviceIdentity.h>
#include <with_dx12/dx11_with_dx12.h>
#include <runtime/GuideCapture.h>
#include <runtime/GuideSourceMetadata.h>
#include <dlssnr/DlssNrFeature_Dx12.h>

#include <native/OpticalFgNrPolicy.h>

#include "DLSSG_Dx12.h"
#include "DLSSG_Dx12_Internal.h"
#include <runtime/ComIdentity.h>
#include <framegen/GuideOrientation.h>
#include <hooks/SwapchainCreateRule.h>
#include "StarRailF5DsPolicy.h"
#include <runtime/StarRailResidencyPolicy.h>
#include <upscalers/CallerCpuTrace.h>
#include "FgDepthContract.h"
#include <resource_tracking/ResTrack_dx12.h>
#include <cmath>
#include <bit>
#include <string_view>
#include <utility>
#include <iterator>
#include <type_traits>
#include <runtime/GameIdentity.h>
#include <runtime/HookCpuTiming.h>
#include "Kcd2Hdr.h"

#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <hooks/Reflex_Hooks.h>
#include <hooks/FG_Hooks.h>
#include <hooks/Streamline_Hooks.h>
#include <native/NativeLowLatency.h>
#include "DlssgTelemetry.h"
#include <hooks/DxgiFactory_Hooks.h>

#include <magic_enum.hpp>

#include <DirectXMath.h>

using namespace DirectX;

using dlssg::detail::g_dlssgTeardownQuarantined;

feature_version DLSSG_Dx12::Version()
{
    if (StreamlineProxy::LoadStreamline())
    {
        auto ver = StreamlineProxy::Version();
        return ver;
    }

    return { 0, 0, 0 };
}

HWND DLSSG_Dx12::Hwnd() { return _hwnd; }

bool DLSSG_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                 IDXGISwapChain** swapChain, bool readyToRelease)
{
    {
        std::lock_guard continuityLock(_inputContinuityMutex);
        if (_hookRetiring && _swapChain) return false;
        _hookRetiring = _hookHostFreed = _hookRetired = false;
        _hookRetirementBinding = {};
        _hookRetiredEpoch = 0;
    }
    Kcd2Hdr::ApplyQuirk();
    if (!PrepareForSwapchainChange()) return false;
    {
        std::lock_guard continuityLock(_inputContinuityMutex);
        _ownerChanging = _continuityOwner;
    }
    // Keep activation blocked during factory callbacks without holding continuity
    // across legacy outer FG-mutex acquisition in an initial swapchain creation.
    auto finishOwnerChange = [this](DLSSG_Dx12*) {
        std::lock_guard continuityLock(_inputContinuityMutex); _ownerChanging = false;
    };
    auto changingOwner = std::unique_ptr<DLSSG_Dx12, decltype(finishOwnerChange)>(this, finishOwnerChange);
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height,
                              desc->BufferDesc.Format, desc->Flags) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            if (!ReleaseSwapchain(_hwnd)) return false;

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (StreamlineProxy::Module() == nullptr)
    {
        LOG_ERROR("Streamline proxy can't find sl.interposer.dll!");
        return false;
    }

    if (!StreamlineProxy::IsD3D12Inited())
    {
        if (State::Instance().currentD3D12Device != nullptr &&
            !StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device))
        {
            return false;
        }
    }

    {
        std::lock_guard continuityLock(_inputContinuityMutex);
        if (_inputContinuity.Held() && (_width != desc->BufferDesc.Width || _height != desc->BufferDesc.Height)) Deactivate();
        _width = desc->BufferDesc.Width;
        _height = desc->BufferDesc.Height;
    }

    IDXGIFactory* slFactory = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &slFactory))
    {
        StreamlineProxy::UpgradeInterface()((void**) &factory);
        DxgiFactoryHooks::HookToDLSSGFactory(factory);
    }
    // CheckForRealObject returns a borrowed identity; it already released QI.

    StreamlineProxy::SetFeatureLoaded()(sl::kFeatureDLSS_G, true);

    desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (State::Instance().gameName == "KCD2")
    {
        // KCD2 waits on this handle. Declare application ownership so Streamline
        // does not also consume it and stall its flip queue.
        desc->Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        LOG_INFO("KCD2: requesting application-owned frame-latency waitable");
    }

    auto result = S_FALSE;

    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        framegen::final_output::RetireProvider(_finalOutputProvider);
        _finalOutputProvider = framegen::final_output::CreateProvider();
        framegen::final_output::CreationScope outputCreation(_finalOutputProvider);
        result = factory->CreateSwapChain(cmdQueue, desc, swapChain);
    }

    if (!SwapchainCreate::Created(result, swapChain))
    {
        LOG_ERROR("CreateSwapChain error: {:X}", (UINT) result);
        return false;
    }

    SetCommandQueue(FG_ResourceType::Depth, cmdQueue);
    _swapChain = *swapChain;
    framegen::final_output::BindInput(_finalOutputProvider, _swapChain);
    RememberPresentQueue(_swapChain, cmdQueue);
    RefreshProviderCapabilities(); // Read-only query after binding the actual owner.
    _hwnd = desc->OutputWindow;
    DxgiFactoryHooks::NoteCreateStatus(result, _swapChain, State::Instance().currentFGSwapchain != nullptr
        ? State::Instance().currentFGSwapchain : _swapChain, desc);

    return true;
}

bool DLSSG_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                  IDXGISwapChain1** swapChain, bool readyToRelease)
{
    {
        std::lock_guard continuityLock(_inputContinuityMutex);
        if (_hookRetiring && _swapChain) return false;
        _hookRetiring = _hookHostFreed = _hookRetired = false;
        _hookRetirementBinding = {};
        _hookRetiredEpoch = 0;
    }
    Kcd2Hdr::ApplyQuirk();
    if (!PrepareForSwapchainChange()) return false;
    {
        std::lock_guard continuityLock(_inputContinuityMutex);
        _ownerChanging = _continuityOwner;
    }
    // Keep activation blocked during factory callbacks without holding continuity
    // across legacy outer FG-mutex acquisition in an initial swapchain creation.
    auto finishOwnerChange = [this](DLSSG_Dx12*) {
        std::lock_guard continuityLock(_inputContinuityMutex); _ownerChanging = false;
    };
    auto changingOwner = std::unique_ptr<DLSSG_Dx12, decltype(finishOwnerChange)>(this, finishOwnerChange);
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->Width, desc->Height, desc->Format, desc->Flags) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            if (!ReleaseSwapchain(_hwnd)) return false;

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (StreamlineProxy::Module() == nullptr)
    {
        LOG_ERROR("Streamline proxy can't find sl.interposer.dll!");
        return false;
    }

    if (!StreamlineProxy::IsD3D12Inited())
    {
        if (State::Instance().currentD3D12Device != nullptr &&
            !StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device))
        {
            return false;
        }
    }

    {
        std::lock_guard continuityLock(_inputContinuityMutex);
        if (_inputContinuity.Held() && (_width != desc->Width || _height != desc->Height)) Deactivate();
        _width = desc->Width;
        _height = desc->Height;
    }

    HRESULT result = E_FAIL;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

        IDXGIFactory* slFactory = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &slFactory))
        {
            StreamlineProxy::UpgradeInterface()((void**) &factory);
            DxgiFactoryHooks::HookToDLSSGFactory(factory);
        }
        // CheckForRealObject returns a borrowed identity; it already released QI.

        IDXGIFactory2* factory2 = nullptr;
        if (factory->QueryInterface(IID_PPV_ARGS(&factory2)) != S_OK)
            return false;

        StreamlineProxy::SetFeatureLoaded()(sl::kFeatureDLSS_G, true);

        desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        if (State::Instance().gameName == "KCD2")
        {
            desc->Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            LOG_INFO("KCD2: requesting application-owned frame-latency waitable");
        }
        framegen::final_output::RetireProvider(_finalOutputProvider);
        _finalOutputProvider = framegen::final_output::CreateProvider();
        framegen::final_output::CreationScope outputCreation(_finalOutputProvider);
        result = factory2->CreateSwapChainForHwnd(cmdQueue, hwnd, desc, pFullscreenDesc, nullptr, swapChain);

        factory2->Release();
        factory2 = nullptr;

        if (!SwapchainCreate::Created(result, swapChain))
        {
            LOG_ERROR("CreateSwapChain error: {:X}", (UINT) result);
            return false;
        }
    }

    SetCommandQueue(FG_ResourceType::Depth, cmdQueue);
    _swapChain = *swapChain;
    framegen::final_output::BindInput(_finalOutputProvider, _swapChain);
    RememberPresentQueue(_swapChain, cmdQueue);
    RefreshProviderCapabilities();
    _hwnd = hwnd;
    DxgiFactoryHooks::NoteCreateStatus(result, _swapChain, State::Instance().currentFGSwapchain != nullptr
        ? State::Instance().currentFGSwapchain : _swapChain);

    return true;
}

void DLSSG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    LOG_DEBUG("");

    if (_hookRetiring || _teardownCompleting || _teardownPending || _teardownQuarantined ||
        g_dlssgTeardownQuarantined.load() || _device != nullptr)
        return;

    _device = device;
    CreateObjects(device);
    RefreshProviderCapabilities(); // Creation can precede swapchain/device binding.

    if (_isActive)
    {
        LOG_INFO("FG context recreated while active, pausing");
        State::Instance().fgChanged = true;
        UpdateTarget();
        Deactivate();
    }
}

// Resources and fences require the same exact COM device-identity proof.
static bool SameDeviceOwner(ID3D12Device* expected, ID3D12DeviceChild* resource)
{
    if (!expected || !resource) return false;
    ID3D12Device* actual = nullptr;
    IUnknown* expectedIdentity = nullptr;
    IUnknown* actualIdentity = nullptr;
    bool same = false;
    if (SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&actual))) && actual &&
        SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&expectedIdentity))) &&
        SUCCEEDED(actual->QueryInterface(IID_PPV_ARGS(&actualIdentity))))
        same = expectedIdentity == actualIdentity;
    if (actualIdentity) actualIdentity->Release();
    if (expectedIdentity) expectedIdentity->Release();
    if (actual) actual->Release();
    return same;
}

bool DLSSG_Dx12::CanRetainForInputGap() const
{
    static const bool starRail = [] {
        wchar_t path[32768] {};
        const auto size = GetModuleFileNameW(nullptr, path, (DWORD) std::size(path));
        if (!size || size >= std::size(path)) return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        return _wcsicmp(name ? name + 1 : path, L"StarRail.exe") == 0;
    }();
    const auto& state = State::Instance();
    return !g_dlssgTeardownQuarantined.load() && !_teardownPending && starRail && state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
           !native::OpticalFgNrEnabled() &&
           state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG &&
           !state.isShuttingDown && !state.fgChanged && Config::Instance()->FGEnabled.value_or_default();
}

dlssg::InputOwner DLSSG_Dx12::CurrentInputOwner() const
{
    const auto orientation = framegen::ResolveGuideOrientation(*Config::Instance());
    return {(uintptr_t) _device, (uintptr_t) _gameCommandQueue, _inputOwnerEpoch, _width, _height,
            (uint32_t) Config::Instance()->FGDLSSGInterpolationCount.value_or_default(),
            (uint32_t) Config::Instance()->FGDLSSGForceDMFG.value_or_default(),
            (uint32_t) Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default(),
            _constants.displayWidth, _constants.displayHeight,
            (uint32_t) _constants.flags[FG_Flags::Hdr] |
                ((uint32_t) _constants.flags[FG_Flags::InvertedDepth] << 1) |
                ((uint32_t) _constants.flags[FG_Flags::JitteredMVs] << 2) |
                ((uint32_t) _constants.flags[FG_Flags::DisplayResolutionMVs] << 3) |
                ((uint32_t) _constants.flags[FG_Flags::Async] << 4),
            orientation.Key(),
            (uint32_t) Config::Instance()->FGEnableDepthScale.value_or_default(),
            (uint32_t)orientation.UsesOffset(),
            Config::Instance()->FGEnableDepthScale.value_or_default() ?
                std::bit_cast<uint32_t>(Config::Instance()->FGDepthScaleMax.value_or_default()) : 0u};
}

bool DLSSG_Dx12::SuspendForInputGap()
{
    if (!CanRetainForInputGap() || !_haveRunningOptions || !_device || !_gameCommandQueue ||
        _lastRunningOptions.mode == sl::DLSSGMode::eOff || !(CurrentInputOwner() == _runningInputOwner) ||
        !(static_cast<uint32_t>(_lastRunningOptions.flags) & static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff)))
        return false;
    if (IsStarRailContinuousOwner())
    {
        // Suppress the first missing source, or an explicit discontinuity.
        // Off/null tags are not GPU completion.
        _inputContinuity.Hold(_frameCount, CurrentInputOwner());
        _starRailDispatchPending = false;
        return SuppressStarRailGapForPresent();
    }
    InvalidateDispatchOptions();
    auto options = _lastRunningOptions;
    options.mode = sl::DLSSGMode::eOff; // Other selected SR routes retain their policy.
    auto result = StreamlineHooks::SubmitDlssgOptions(StreamlineProxy::DLSSGSetOptions(),viewport,options);
    if (result != sl::Result::eOk) return false;

    // FrameToken is a finite SDK ring entry: reacquire for this observed real frame,
    // use immediately, and never dereference a cached token across a gap.
    uint32_t id = static_cast<uint32_t>(_frameCount);
    sl::FrameToken* token = nullptr;
    result = StreamlineProxy::GetNewFrameToken()(token, &id);
    if (result != sl::Result::eOk || !token) return false;
    sl::ResourceTag tags[4] {};
    tags[0].type = sl::kBufferTypeDepth;
    tags[1].type = sl::kBufferTypeMotionVectors;
    tags[2].type = sl::kBufferTypeHUDLessColor;
    tags[3].type = sl::kBufferTypeUIColorAndAlpha;
    // A null command buffer is valid here because every resource tag is null.
    result = StreamlineProxy::SetTagForFrame()(*token, viewport, tags, 4, nullptr);
    if (result != sl::Result::eOk) return false;
    _inputContinuity.Hold(_frameCount, CurrentInputOwner());
    if (!_starRailUiBusy) // Busy admission can recur every source frame; no per-gap IO.
        LOG_INFO("F5 FG continuity: logical active, input waiting, provider Off+retain, frame {}; generated UNKNOWN", _frameCount);
    return true;
}

UINT64 DLSSG_Dx12::StartNewFrame()
{
    // The upscaler enters this before waiting for the present-side FG mutex.
    // Serialize frame increment, ready bits and ring recycling with Dispatch/SetResource.
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_hookRetiring) return _frameCount;
    PollGuideObservations();
    // The exact Genshin DX11 bridge has one Present-owned clock. Ordinary
    // upscaler callbacks cannot advance/recycle that ring a second time.

    if (_teardownPending || _teardownQuarantined ||
        g_dlssgTeardownQuarantined.load()) return _frameCount;
    // ZZZ native source ordinals belong to selected SR, never Reflex. Refuse
    // nested source publication while this same provider is consuming a Present.
    if (OwnsNativeZzzSource() && (_presentExecutionDepth || _inputRecordingDepth>1)) return _frameCount;
    // Source ordinals never adopt the SDK/Reflex marker clock or wrap.
    if (OwnsNativeZzzSource() && _frameCount>=UINT64_MAX-1)
    {
        _nativeGuideFailed=true; _managedRetained=true; Deactivate();
        return _frameCount;
    }
    const auto generation = _inputContinuity.SourceGeneration();
    const auto frame = IFGFeature::StartNewFrame();
    if (IsStarRailContinuousOwner())
    {
        // NewFrame may call vendor code recursively. A clock change during it
        // cannot certify this old recording as a source of the new generation.
        _starRailRecordingGeneration = _inputRecordingDepth && _inputContinuity.SourceClockValid() &&
            generation == _inputContinuity.SourceGeneration() && frame == _frameCount ? generation : 0;
        _starRailRecordingFrame = frame;
    }
    return frame;
}

void DLSSG_Dx12::SetFrameCountFromReflex(UINT64 frameId)
{
    // Use the same owner predicate and lock as BeginGenshinPresent/StartNewFrame.
    // A marker may arrive between Prepare and Dispatch; it cannot choose a new
    // ring index for this exact Present-owned route. Do not acquire FG Mutex here.
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_hookRetiring) return;

    if (_teardownPending || _teardownQuarantined ||
        g_dlssgTeardownQuarantined.load()) return;

    // Markers keep their original SDK/PCL IDs. Only actual selected SR may
    // advance the native ZZZ source ring, including while it is warming up.
    if (OwnsNativeZzzSource() || TracksNativeGuides()) return;
    // Other DLSSG owners retain the game's Reflex clock and existing diagnostics.
    const auto frameCount = FrameCount();
    if (frameId - frameCount > 1)
        LOG_WARN("FrameId Moved too much??? {} -> {}", frameCount, frameId);
    if (frameId != frameCount)
    {
        if ((IsStarRailContinuousOwner() || OwnsUpscalerDepthContract()) && frameId < frameCount)
        {
            // A real source marker, including an SDK observation adopted at the
            // guarded input boundary, invalidates CPU guide authority on rewind.
            // Tags/resources still read by the GPU keep their real owners.
            _starRailDispatchPending = false;
            _starRailRecordingGeneration = 0;
            if (!_inputContinuity.RestartSource(frameId, CurrentInputOwner()))
            {
                FailStarRailUiFrame(); // Exhaustion is terminal, never wrap/reuse.
                return;
            }
            // The native ZZZ producer has its own successful-source/submit
            // receipt. A Reflex rewind invalidates CPU tags, not a StarRail
            // input-gap hold on a route that cannot retain that hold.
            if (!IsStarRailContinuousOwner()) _inputContinuity.Resume();
        }
        IFGFeature::SetFrameCount(frameId);
    }
}

void DLSSG_Dx12::Activate()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    LOG_DEBUG("");

    if (_hookRetiring || _ownerChanging || _teardownPending || _teardownQuarantined || g_dlssgTeardownQuarantined.load()) return;

    if (!_isActive)
    {

        UpdateTarget();
        _isActive = true;
    }
}

bool DLSSG_Dx12::IsStarRailUiOwner() const
{
    const auto& state = State::Instance();
    return CanRetainForInputGap() && state.currentFG == this &&
        !state.externalFrameGeneration && !state.gameCalledSlInit;
}

bool DLSSG_Dx12::IsStarRailContinuousOwner() const
{
    // Exact StarRail DLSS-on-DX12 source, not native DX12, Genshin, ZZZ,
    // the optical producer or a game-owned Streamline provider.
    return IsStarRailF5DsOwner();
}

bool DLSSG_Dx12::IsStarRailF5DsOwner() const
{
    return IsStarRailUiOwner() && StarRailF5Ds::Selected();
}

bool DLSSG_Dx12::ForceStarRailFgResident() const
{
    // Independent of NR. An option cannot authenticate a producer, override a
    // real owner/configuration change, or authorize a provider without depth.
    return Config::Instance()->StarRailForceFGResident.ObservedValue() && IsStarRailF5DsOwner();
}

int DLSSG_Dx12::SelectStarRailF5DsFrame(UINT64& frame)
{
    // F5-DS GetDispatchIndex/GetIndexWillBeDispatched selection, without its
    // premature "dispatched" publication before readiness/SDK calls succeed.
    if (!_frameCount || _lastDispatchedFrame == _frameCount ||
        _lastDispatchedFrame == UINT64_MAX) return -1;
    frame = _lastDispatchedFrame + 1;
    const auto diff = _frameCount - _lastDispatchedFrame;
    if (!_lastDispatchedFrame || diff > Config::Instance()->FGAllowedFrameAhead.value_or_default())
    {
        const auto index = GetIndex();
        if (HasResource(FG_ResourceType::Depth, index) || HasResource(FG_ResourceType::Velocity, index) ||
            HasResource(FG_ResourceType::UIColor, index) || HasResource(FG_ResourceType::HudlessColor, index))
            frame = _frameCount;
    }
    if (frame > _frameCount) return -1;
    const auto slot = static_cast<int>(frame % BUFFER_COUNT);
    // Extra safety versus the snapshot: require both successful tags for this
    // actual slot/source generation/owner, never merely an old map entry.
    // This bridge borrows the prepared cache; it has no immutable lease for
    // an older picture. Do not treat its CPU stamp as proof of old content.
    return frame == _frameCount && _inputContinuity.Current(slot, frame, CurrentInputOwner()) ? slot : -1;
}

bool DLSSG_Dx12::PresentStarRailInputGap()
{
    if (!IsStarRailF5DsOwner() || !_haveRunningOptions ||
        !(CurrentInputOwner() == _runningInputOwner)) return false;
    // A retained provider has no new source token to pace. Leaving SDK On for
    // several original Presents would require markers for an absent source.
    // Keep its allocations, but suppress generation at the first such boundary.
    return _inputContinuity.Held() ? SuppressStarRailGapForPresent() : SuspendForInputGap();
}

bool DLSSG_Dx12::SuppressStarRailGapForPresent()
{
    if (_starRailGapSuppressing) return false;
    if (!IsStarRailContinuousOwner() || !_inputContinuity.Held() || !_haveRunningOptions ||
        !_inputContinuity.OwnerMatches(CurrentInputOwner()) ||
        !(CurrentInputOwner() == _runningInputOwner)) return false;
    const auto owner = CurrentInputOwner();
    const auto frame = _frameCount;
    const auto generation = _inputContinuity.SourceGeneration();
    const auto lifecycle = _providerOptionsLifecycle;
    const auto current = [&]() {
        return !_hookRetiring && !_ownerChanging && !_teardownPending && !_teardownCompleting &&
            !_teardownQuarantined && !g_dlssgTeardownQuarantined.load() && !_starRailUiFailed &&
            IsStarRailContinuousOwner() && _haveRunningOptions && _inputContinuity.Held() &&
            _inputContinuity.SourceClockValid() && _inputContinuity.OwnerMatches(owner) &&
            owner == CurrentInputOwner() && owner == _runningInputOwner && frame == _frameCount &&
            generation == _inputContinuity.SourceGeneration() && lifecycle == _providerOptionsLifecycle;
    };
    _starRailGapSuppressing = true;
    struct FinishSuppression
    {
        DLSSG_Dx12& provider;
        bool completed=false;
        ~FinishSuppression()
        {
            provider._starRailGapSuppressing=false;
            // Includes C++ exceptions from Off/token/tag callbacks, and cached
            // Off paths which only attempted this Present's null tags.
            if (!completed) provider.InvalidateDispatchOptions();
        }
    } finish {*this};
    auto options = _lastRunningOptions;
    options.mode = sl::DLSSGMode::eOff;
    options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (!StarRailResidency::SuppressFgGap(_starRailGapOffAccepted, current,
        [&]() {
            // FG is Off, but Reflex stays running on this exact owner. Revoke
            // only FG's acknowledgement; a fresh source must really send On.
            InvalidateDlssgOptions();
            CallerCpuTrace::Scope optionsCpu(CallerCpuTrace::Op::FgDlssgOptions, this,
                lifecycle, frame);
            const auto setOptions = StreamlineProxy::DLSSGSetOptions();
            const auto result = setOptions ? StreamlineHooks::SubmitDlssgOptions(setOptions,viewport,options) :
                sl::Result::eErrorMissingOrInvalidAPI;
            optionsCpu.Finish(static_cast<uint32_t>(result), true);
            return result == sl::Result::eOk;
        }, [&]() {
            // Reacquire a token only for the actual observed source ordinal.
            // Never truncate/wrap an unknown source into another SDK frame.
            if (!frame || frame > UINT32_MAX) return false;
            uint32_t id = static_cast<uint32_t>(frame);
            sl::FrameToken* token = nullptr;
            const auto getToken = StreamlineProxy::GetNewFrameToken();
            const auto setTags = StreamlineProxy::SetTagForFrame();
            if (!getToken || !setTags || getToken(token, &id) != sl::Result::eOk || !token ||
                static_cast<uint32_t>(*token) != id || !current()) return false;
            sl::ResourceTag tags[4] {};
            tags[0].type = sl::kBufferTypeDepth;
            tags[1].type = sl::kBufferTypeMotionVectors;
            tags[2].type = sl::kBufferTypeHUDLessColor;
            tags[3].type = sl::kBufferTypeUIColorAndAlpha;
            return setTags(*token, viewport, tags, 4, nullptr) == sl::Result::eOk;
        })) return false;
    finish.completed=true;
    if (_starRailGapOffAccepted) return true; // Only Off was cached; THIS frame's null tags just succeeded.
    // F5-DS leaves Reflex running during input gaps. User Off and real
    // resize/device teardown still use the existing complete Off recipe.
    _starRailGapOffAccepted = true;
    _starRailDispatchPending = false;
    State::Instance().dlssgDetectedInterpolationCount = 0;
    ReflexHooks::setDlssgFrameCount(0);
    if (!_starRailUiGap)
        LOG_INFO("StarRail FG input gap: forced_resident={} SDK Off+retain, Reflex retained; owner/slots retained, frame {}; no synthetic guide dispatch, displayed output UNKNOWN",
            ForceStarRailFgResident(), _frameCount);
    return true;
}

dlssg::options::ProviderOwner DLSSG_Dx12::CurrentProviderOwner() const
{
    const auto& state = State::Instance();
    const auto setOptions = StreamlineProxy::DLSSGSetOptions();
    const auto getState = StreamlineProxy::DLSSGGetState();
    // The game's wrapper can rewrite both the request and reported maximum.
    // It is not this self-owned provider's capability authority.
    const bool direct = !StreamlineHooks::IsWrappedDlssgInterface(setOptions, getState) &&
        dlssg::options::Schema(viewport, sl::kStructVersion1);
    return { _providerOwnerGeneration, _providerOptionsLifecycle,
        reinterpret_cast<uintptr_t>(_device), reinterpret_cast<uintptr_t>(_gameCommandQueue),
        reinterpret_cast<uintptr_t>(_swapChain), reinterpret_cast<uintptr_t>(StreamlineProxy::Module()),
        reinterpret_cast<uintptr_t>(state.optiSlDLSSG),
        direct ? reinterpret_cast<uintptr_t>(setOptions) : 0,
        direct ? reinterpret_cast<uintptr_t>(getState) : 0,
        static_cast<uint32_t>(viewport), _width, _height };
}

void DLSSG_Dx12::InvalidateDispatchOptions()
{
    _dispatchOptions.Invalidate();
    _dlssgOptionsAck.valid = _reflexOptionsAck.valid = false;
    _generationState.InvalidateApplied();
    _framesToInterpolate = 0;
}

void DLSSG_Dx12::InvalidateDlssgOptions()
{
    _dispatchOptions.InvalidateDlssg();
    _dlssgOptionsAck.valid = false;
    _generationState.InvalidateApplied();
    _framesToInterpolate = 0;
}

void DLSSG_Dx12::InvalidateProviderCapabilities()
{
    InvalidateDispatchOptions();
    if (_providerOptionsLifecycle != UINT64_MAX) ++_providerOptionsLifecycle;
    _generationState.InvalidateCapability();
    _maxInterpolationCount = 0;
    _supportsDMFG = false;
}

sl::Result DLSSG_Dx12::QueryProviderState(sl::DLSSGState& actualState, framegen::DlssgStateQueryReason reason)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_providerStateQueryActive || _dispatchOptions.InFlight())
    {
        InvalidateDispatchOptions();
        return sl::Result::eErrorInvalidState;
    }
    const auto owner = CurrentProviderOwner();
    if (!owner.Known() || !StreamlineProxy::IsD3D12Inited())
    {
        _generationState.InvalidateCapability();
        _maxInterpolationCount = 0;
        _supportsDMFG = false;
        InvalidateDispatchOptions();
        return sl::Result::eErrorMissingOrInvalidAPI;
    }
    _providerStateQueryActive = true;
    struct Finish
    {
        DLSSG_Dx12& provider;
        bool completed=false;
        ~Finish()
        {
            provider._providerStateQueryActive=false;
            if (!completed)
            {
                provider._generationState.InvalidateCapability();
                provider._maxInterpolationCount=0;
                provider._supportsDMFG=false;
                provider.InvalidateDispatchOptions();
            }
        }
    } finish { *this };
    const auto serial = _dispatchOptions.Serial();
    const auto previousMax = _generationState.Maximum(owner);
    const auto previousRawMax = _generationState.rawMaxGenerated;
    const auto previousDynamic = _generationState.dynamicSupported;
    // nullptr makes this a query, never an implicit Off/SetOptions transaction.
    framegen::DlssgStateSample sample;
    const auto result = StreamlineHooks::QueryDlssgState(StreamlineProxy::DLSSGGetState(),
        viewport,actualState,nullptr,reason,&sample);
    // Observe this existing consumptive query; never issue a diagnostics query.
    try
    {
        auto& observation=_guideObservations[GetIndex()];
        if (observation.frame==_frameCount && observation.sourceTick &&
            serial==_dispatchOptions.Serial() && owner==CurrentProviderOwner())
        {
            auto& snapshot=observation.snapshot;
            snapshot.sdkQueryObserved=true;snapshot.sdkQueryResult=static_cast<int>(result);
            snapshot.sdkQueryTick=GuideDiagnostics::Now();snapshot.sdkStatusKnown=result==sl::Result::eOk;
            snapshot.sdkQuerySequence=sample.querySequence;
            snapshot.sdkPreviousQueryTick=sample.previousQueryEndNs/1000000;
            snapshot.sdkQueryBeginNs=sample.queryBeginNs;snapshot.sdkQueryEndNs=sample.queryEndNs;
            snapshot.sdkPreviousQueryEndNs=sample.previousQueryEndNs;
            snapshot.sdkIntervalKnown=sample.counterIntervalKnown;
            if(snapshot.sdkStatusKnown){snapshot.sdkStatus=static_cast<uint32_t>(actualState.status);
                snapshot.sdkPresentedInterval=actualState.numFramesActuallyPresented;}
            PublishGuideObservation(GetIndex(),snapshot.stage,"SDK GetState observed; presented count is query-interval data, generated identity unknown");
        }
    }catch(...){}
    _generationState.Observe(owner, actualState, result,
        serial == _dispatchOptions.Serial() && owner == CurrentProviderOwner());
    _maxInterpolationCount = static_cast<int>(_generationState.Maximum(CurrentProviderOwner()));
    _supportsDMFG = _generationState.Current(CurrentProviderOwner()) && _generationState.dynamicSupported;
    if (!_generationState.Current(owner) || previousMax != _generationState.Maximum(owner) ||
        previousRawMax != _generationState.rawMaxGenerated ||
        previousDynamic != _generationState.dynamicSupported || actualState.status != sl::DLSSGStatus::eOk)
        InvalidateDispatchOptions();
    finish.completed=true;
    // Keep the raw SDK result in GenerationState. Callers must not consume
    // returned fences/capability after an unknown schema or recursive owner loss.
    return result == sl::Result::eOk && !_generationState.Current(owner) ? sl::Result::eErrorInvalidState : result;
}

void DLSSG_Dx12::RefreshProviderCapabilities()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_generationState.Current(CurrentProviderOwner()) || !CurrentProviderOwner().Known()) return;
    sl::DLSSGState state {};
    const auto result = QueryProviderState(state,framegen::DlssgStateQueryReason::CapabilityCold);
    LOG_INFO("F6 FG capability: owner={} lifecycle={} result={} known={} raw_max_generated={} selectable_max_generated={} dynamic_supported={}; generated=UNKNOWN",
        _providerOwnerGeneration, _providerOptionsLifecycle, static_cast<int>(result),
        _generationState.capabilityKnown, _generationState.rawMaxGenerated, _maxInterpolationCount, _supportsDMFG);
}

dlssg::options::Scope DLSSG_Dx12::CurrentDispatchScope(int index) const
{
    const auto& state = State::Instance();
    dlssg::options::Scope scope {};
    scope.provider = CurrentProviderOwner();
    scope.input = CurrentInputOwner();
    scope.sourceGeneration = _inputContinuity.SourceGeneration();
    scope.reflexPlugin = reinterpret_cast<uintptr_t>(state.optiSlReflex);
    scope.reflexSetOptions = reinterpret_cast<uintptr_t>(StreamlineProxy::ReflexSetOptions());
    scope.inputPath = static_cast<uint32_t>(state.activeFgInput);
    scope.outputPath = static_cast<uint32_t>(state.activeFgOutput);
    scope.interop = static_cast<uint32_t>(state.swapchainInteropApi);
    scope.guideFlags = (uint32_t)_constants.flags[FG_Flags::Hdr] |
        ((uint32_t)_constants.flags[FG_Flags::InvertedDepth] << 1) |
        ((uint32_t)_constants.flags[FG_Flags::JitteredMVs] << 2) |
        ((uint32_t)_constants.flags[FG_Flags::DisplayResolutionMVs] << 3) |
        ((uint32_t)_constants.flags[FG_Flags::Async] << 4) |
        ((uint32_t)_constants.flags[FG_Flags::InfiniteDepth] << 5);
    scope.consumers = (uint32_t)!_noHudless[index] | ((uint32_t)!_noUi[index] << 1) |
        ((uint32_t)!_noDistortionField[index] << 2);
    scope.providerCap = _generationState.rawMaxGenerated;
    scope.forceReflex = (uint32_t)Config::Instance()->FN_ForceReflex.value_or_default();
    scope.colorSpace = static_cast<uint32_t>(state.outputColorSpace.dxgiColorSpace);
    scope.transfer = (uint32_t)state.outputColorSpace.transfer;
    scope.primaries = (uint32_t)state.outputColorSpace.primaries;
    scope.range = (uint32_t)state.outputColorSpace.range;
    scope.colorModel = (uint32_t)state.outputColorSpace.model;
    scope.colorSpaceValid = state.outputColorSpace.valid;
    scope.enabled = Config::Instance()->FGEnabled.value_or_default();
    scope.markers = ReflexHooks::gameIsSendingMarkers();
    scope.boost = native::lowlatency::WantsBoostForFrameGeneration();
    scope.sharedSdk = state.gameCalledSlInit;
    scope.external = state.externalFrameGeneration;
    scope.secondDevice = state.gameQuirks[GameQuirk::CreateSLOnThe2ndDevice];
    return scope;
}

template<typename Options, typename Submit>
dlssg::options::Submission DLSSG_Dx12::SetDispatchOptions(dlssg::options::Acknowledgement<Options>& cache,
    const dlssg::options::Scope& scope, int index, const Options& options,
    UINT64 sourceFrame, bool force, Submit&& submit)
{
    const auto frame = _frameCount;
    const auto current = [&] {
        const auto& state = State::Instance();
        return CanSubmitInputs() && _isActive && frame == _frameCount && !state.isShuttingDown &&
            !state.fgChanged && state.currentFG == this && scope == CurrentDispatchScope(index);
    };
    // Eligibility depends on SDK/input ownership, not a game name. Shared game
    // SDKs, second-device quirks and wrapped Reflex retain real calls every time.
    const bool cacheable = scope.provider.Known() && scope.reflexPlugin && scope.reflexSetOptions &&
        scope.input.epoch && scope.input.epoch != UINT64_MAX && scope.sourceGeneration != UINT64_MAX &&
        StreamlineProxy::IsD3D12Inited() && sourceFrame == frame &&
        !scope.external && !scope.sharedSdk && !scope.secondDevice &&
        !StreamlineHooks::IsWrappedReflexInterface(StreamlineProxy::ReflexSetOptions());
    try
    {
        constexpr bool reflex = std::is_same_v<Options, sl::ReflexOptions>;
        const auto outcome = _dispatchOptions.Apply(cache, scope, options, sourceFrame, cacheable, force,
            current, [&]() {
                CallerCpuTrace::Scope optionsCpu(reflex ? CallerCpuTrace::Op::FgReflexOptions :
                    CallerCpuTrace::Op::FgDlssgOptions, this, scope.provider.lifecycle, sourceFrame, index);
                const auto result = std::forward<Submit>(submit)();
                optionsCpu.Finish(static_cast<uint32_t>(result), true);
                return result;
            });
        auto& counts = _dispatchOptionsCounts[reflex ? 1 : 0];
        ++counts.attempts;
        counts.submitted += outcome.submitted;
        counts.reused += !outcome.submitted && outcome.confirmed;
        counts.failed += outcome.Result() != sl::Result::eOk;
        counts.forced += force;
        counts.keepalive += outcome.reason == dlssg::options::Reason::Keepalive;
        counts.scopeChanged += outcome.reason == dlssg::options::Reason::ScopeChanged;
        counts.payloadChanged += outcome.reason == dlssg::options::Reason::PayloadChanged;
        counts.invalidated += outcome.reason == dlssg::options::Reason::Invalidated;
        counts.uncacheable += outcome.reason == dlssg::options::Reason::NotCacheable;
        if (counts.attempts == 1 || counts.attempts % 600 == 0 ||
            (outcome.Result() != sl::Result::eOk && counts.failed <= 3))
            LOG_INFO("F6 FG configuration totals: kind={} source={} owner={} lifecycle={} completed_attempts={} submit_attempts={} ack_hits={} failed={} forced={} keepalive={} scope_changed={} payload_changed={} invalidated={} uncacheable={} last_reason={} result={}; counters=dispatch_only excludes_throwing_calls=true no_retirement_authority=true",
                reflex ? "Reflex" : "DLSSG", sourceFrame, scope.provider.instance, scope.provider.lifecycle,
                counts.attempts, counts.submitted, counts.reused, counts.failed, counts.forced,
                counts.keepalive, counts.scopeChanged, counts.payloadChanged, counts.invalidated, counts.uncacheable,
                dlssg::options::ReasonName(outcome.reason), static_cast<int>(outcome.Result()));
        return outcome;
    }
    catch (...)
    {
        InvalidateDispatchOptions();
        throw;
    }
}

void DLSSG_Dx12::FailStarRailUiFrame()
{
    // Preserve the real owners, including Execute followed by failed Signal.
    // Only the existing safe Present/retirement boundary drains or quarantines.
    _starRailUiFailed = true;
    _managedRetained = true;
    Deactivate();
    UpdateTarget();
}

bool DLSSG_Dx12::WaitForStarRailF5DsUIAllocator(UINT index)
{
    if (index >= BUFFER_COUNT) return false;
    const auto fenceValue = _uiAllocatorFenceValues[index];
    if (fenceValue == 0) return true;
    if (!_uiFence || !_uiFenceEvent) return false;

    CallerCpuTrace::Scope allocatorCpu(CallerCpuTrace::Op::FgUiAllocatorWait, this,
        _providerOptionsLifecycle, _frameCount, index, fenceValue, _uiFence->GetCompletedValue(), false, true);

    // A timed-out SetEventOnCompletion registration can signal this shared
    // auto-reset event during a later slot's wait. That early wake is not a
    // failure and does not authorize Reset until this slot's target completes.
    const auto deadline = GetTickCount64() + 5000;
    DWORD waitResult = WAIT_OBJECT_0;
    UINT earlyWakeups = 0;
    for (;;)
    {
        const auto completed = _uiFence->GetCompletedValue();
        if (completed == UINT64_MAX) return false;
        if (completed >= fenceValue) return true;

        const auto now = GetTickCount64();
        if (now >= deadline)
        {
            waitResult = WAIT_TIMEOUT;
            LOG_ERROR("StarRail F5-DS UI allocator fence wait incomplete. slot {}, fence {}, completed {}, waitResult {:X}, earlyWakeups {}",
                      index, fenceValue, completed, waitResult, earlyWakeups);
            return false;
        }

        const auto result = _uiFence->SetEventOnCompletion(fenceValue, _uiFenceEvent);
        if (FAILED(result))
        {
            LOG_ERROR("StarRail F5-DS UI allocator SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}",
                      index, fenceValue, completed, (UINT) result);
            return false;
        }

        const auto remaining = static_cast<DWORD>(deadline - now);
        waitResult = WaitForSingleObject(_uiFenceEvent, remaining);
        const auto completedAfterWait = _uiFence->GetCompletedValue();
        if (completedAfterWait != UINT64_MAX && completedAfterWait >= fenceValue) return true;
        if (waitResult != WAIT_OBJECT_0 || completedAfterWait == UINT64_MAX)
        {
            LOG_ERROR("StarRail F5-DS UI allocator fence wait incomplete. slot {}, fence {}, completed {}, waitResult {:X}, earlyWakeups {}",
                      index, fenceValue, completedAfterWait, waitResult, earlyWakeups);
            return false;
        }
        ++earlyWakeups;
    }
}

void DLSSG_Dx12::AdmitStarRailUiFrame()
{
    if (!IsStarRailUiOwner())
    {
        if (_starRailUiBusy) FailStarRailUiFrame();
        return;
    }
    _starRailUiBusy = false;
    if (!CanSubmitInputs()) return;
    const UINT next = static_cast<UINT>((_frameCount + 1) % BUFFER_COUNT);
    if (_frameCount == UINT64_MAX || !_device || !_gameCommandQueue || !_uiFence || !_uiFenceEvent ||
        !_uiCommandAllocator[next] || !_uiCommandList[next] || _uiCloseFailed[next] ||
        _uiFenceValue >= UINT64_MAX - 1)
    {
        FailStarRailUiFrame();
        return;
    }
    // The generic GetUICommandList/NewFrame would submit these recordings.
    // Do it before polling, never close/reset an executing allocator on refusal.
    for (UINT j = 0; j < 2; ++j)
    {
        const UINT slot = (next + j) % BUFFER_COUNT;
        if (_uiCommandListResetted[slot] && !SubmitUICommandList(slot))
        {
            return; // Submit keeps the failed real owner for safe teardown.
        }
    }
    const auto status = PollUIAllocator(next);
    if (status == UIAllocatorStatus::Invalid)
    {
        FailStarRailUiFrame();
        return;
    }
    if (status == UIAllocatorStatus::Busy)
    {
        // Old F5-DS waits rather than counting a callback it did not record.
        // The current waiter rechecks the real target and rejects device loss.
        if (IsStarRailF5DsOwner())
        {
            if (!WaitForStarRailF5DsUIAllocator(next)) FailStarRailUiFrame();
            return;
        }
        // Count this real source callback exactly once, but do not call
        // StartNewFrame/NewFrame: every ready bit/resource/allocator stays owned
        // by the old submission. The producer's existing CanSubmitInputs check
        // returns before camera setters, cache guides, or depth-scale recording.
        ++_frameCount;
        _starRailUiBusy = true;
        return;
    }
}

bool DLSSG_Dx12::WaitForUIAllocator(UINT index)
{
    if (_starRailUiFailed || !CanSubmitInputs()) return false;
    if (!IsStarRailUiOwner()) return IFGFeature_Dx12::WaitForUIAllocator(index);
    if (_starRailUiBusy || !CanSubmitInputs()) return false;
    return IsStarRailF5DsOwner() ? WaitForStarRailF5DsUIAllocator(index) :
        PollUIAllocator(index) == UIAllocatorStatus::Ready;
}

void DLSSG_Dx12::RecordDx11GuideSubmission(UINT slot) noexcept
{
    if (!_dx11GuideBorrowed[slot]) return;
    const auto& depth=_guideTags[slot][0];const auto& motion=_guideTags[slot][1];
    if (!depth.accepted || !motion.accepted || depth.frame!=motion.frame ||
        depth.generation!=motion.generation || !(depth.owner==motion.owner) ||
        depth.input.cmdList!=_uiCommandList[slot] || motion.input.cmdList!=_uiCommandList[slot]) return;
    _dx11GuideSubmissions[slot]={depth.frame,depth.generation,_uiAllocatorFenceValues[slot],_uiFence,_gameCommandQueue};
}

bool DLSSG_Dx12::SubmitUICommandList(UINT index)
{
    if (_starRailUiFailed || !CanSubmitInputs()) return false;
    if (!IsStarRailUiOwner())
    {
        if (index>=std::size(_uiCommandListResetted) || !_uiCommandListResetted[index]) return true;
        if (_uiCloseFailed[index] || !_gameCommandQueue || !_uiFence || _uiFenceValue>=UINT64_MAX-1) return false;
        _uiAllocatorFenceValues[index]=++_uiFenceValue;
        const auto closeResult=_uiCommandList[index]->Close();
        if (FAILED(closeResult)) { _uiCloseFailed[index]=true; return false; }
        const auto callbacks=DlssNr::FinishedPictureSubmissionCallbacks();
        const auto previewReceipt=DlssNr::FinishedPictureSubmitting(_gameCommandQueue,1,
            (ID3D12CommandList**)&_uiCommandList[index]);
        _gameCommandQueue->ExecuteCommandLists(1,(ID3D12CommandList**)&_uiCommandList[index]);
        if (callbacks==DlssNr::FinishedPictureSubmissionCallbacks())
            DlssNr::FinishedPictureSubmitted(_gameCommandQueue,1,
                (ID3D12CommandList**)&_uiCommandList[index],previewReceipt);
        _uiCommandListResetted[index]=false; // Execute cannot be replayed after a failed Signal.
        if (FAILED(_gameCommandQueue->Signal(_uiFence,_uiAllocatorFenceValues[index]))) return false;
        RecordDx11GuideSubmission(index);
        ObserveGuideSubmission(index,_uiFence,_uiAllocatorFenceValues[index]);
        return true;
    }
    if (index >= BUFFER_COUNT || !_uiCommandListResetted[index]) return true;
    if (_uiCloseFailed[index] || _uiFenceValue >= UINT64_MAX - 1)
    {
        FailStarRailUiFrame();
        return false;
    }
    // GetUICommandList reserves during Reset, but recordings can be submitted in
    // another order. Reserve this real Execute/Signal target at submission so a
    // later numeric Signal cannot falsely retire a not-yet-submitted busy slot.
    _uiAllocatorFenceValues[index] = ++_uiFenceValue;
    if (!IFGFeature_Dx12::SubmitUICommandList(index))
    {
        _uiCloseFailed[index] = true; // Never re-Close/replay after partial failure.
        FailStarRailUiFrame();
        return false;
    }
    RecordDx11GuideSubmission(index);
    ObserveGuideSubmission(index,_uiFence,_uiAllocatorFenceValues[index]);
    return true;
}

DLSSG_Dx12::StarRailPresent DLSSG_Dx12::PrepareStarRailPresent(bool beforeSdkPresent, UINT64* dispatchedFrame)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (dispatchedFrame) *dispatchedFrame = 0;
    if (_starRailUiFailed) return StarRailPresent::Failed;
    if (beforeSdkPresent && OwnsUpscalerDepthContract() && _starRailDispatchPending &&
        !GuideTagsCurrent(_starRailDispatchFrame%BUFFER_COUNT,_starRailDispatchFrame))
        RefuseDepthContract("starrail-pre-sdk-present-guide-pair");
    if (_starRailRuntimeOwner && !IsStarRailContinuousOwner())
    {
        // A changed backend/config/owner is a real teardown, not an input pause.
        Deactivate();
        return StarRailPresent::Failed;
    }
    if (IsStarRailContinuousOwner() && _haveRunningOptions)
    {
        const auto owner = CurrentInputOwner();
        if (!(owner == _runningInputOwner) ||
            (_inputContinuity.Held() && (!_inputContinuity.OwnerMatches(owner) ||
                                        _frameCount < _inputContinuity.HoldFrame())))
        {
            FailStarRailUiFrame();
            return StarRailPresent::Failed;
        }
        if (beforeSdkPresent)
        {
            // A failed/occluded SDK return must not replay this CPU admission.
            if (!_starRailUiBusy && !_inputContinuity.Held() &&
                _starRailDispatchPending && _starRailDispatchFrame == _frameCount &&
                _starRailDispatchGeneration == _inputContinuity.SourceGeneration() &&
                _starRailDispatchOwner == owner &&
                _inputContinuity.Current(_starRailDispatchFrame % BUFFER_COUNT, _starRailDispatchFrame, owner))
            {
                if (dispatchedFrame) *dispatchedFrame = _starRailDispatchFrame;
                _starRailDispatchPending = false;
                return StarRailPresent::Proceed;
            }
        }
        else if (!_starRailUiBusy)
        {
            UINT64 selected = 0;
            if ((!_inputContinuity.Held() && SelectStarRailF5DsFrame(selected) >= 0) ||
                (_inputContinuity.Held() && _inputContinuity.Fresh(GetIndex(), _frameCount,
                    _lastDispatchedFrame, owner,
                    _starRailDispatchGeneration == _inputContinuity.SourceGeneration())))
                return StarRailPresent::Proceed;
        }

        // Only final SDK admission changes generation state. Early NR selection
        // cannot authorize an On Present without a successful fresh Dispatch.
        if (beforeSdkPresent && !PresentStarRailInputGap())
        {
            FailStarRailUiFrame();
            return StarRailPresent::Failed;
        }
        _starRailUiGap = _starRailUiBusy && _inputContinuity.Held();
        return StarRailPresent::NativeFallback;
    }
    // Other selected StarRail upscalers retain their existing UI-busy policy.
    if (!_starRailUiBusy || !IsStarRailUiOwner()) return StarRailPresent::Proceed;
    if (_haveRunningOptions &&
        ((!_inputContinuity.Held() && !SuspendForInputGap()) ||
         (_inputContinuity.Held() && (!_inputContinuity.OwnerMatches(CurrentInputOwner()) ||
                                     _frameCount < _inputContinuity.HoldFrame()))))
    {
        FailStarRailUiFrame();
        return StarRailPresent::Failed;
    }
    _starRailUiGap = _inputContinuity.Held();
    return StarRailPresent::NativeFallback;
}

FGExecutionGuard DLSSG_Dx12::GuardExecution(FGExecutionScope scope)
{
    auto guard = FGExecutionGuard(_inputContinuityMutex,
        scope == FGExecutionScope::Inputs ? &_inputRecordingDepth :
        scope == FGExecutionScope::Present ? &_presentExecutionDepth : nullptr);
    if (scope == FGExecutionScope::Inputs && _inputRecordingDepth == 1)
    {
        if (IsStarRailF5DsOwner())
        {
            const auto observed = _starRailF5DsMarker.exchange(UINT64_MAX, std::memory_order_acq_rel);
            if (observed != UINT64_MAX) SetFrameCountFromReflex(observed);
        }
        AdmitStarRailUiFrame();
    }
    return guard;
}

bool DLSSG_Dx12::CanSubmitInputs()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    // An SDK provider may exist before source creation finishes. Preserve that
    // warmup, but a failed source hook must never accept more game inputs.
    // This observation grants no resource retirement or teardown authority.
    const auto& state = State::Instance();
    const auto hookStatus = FGHooks::SwapchainHookStatus();
    if (state.currentFG == this && !state.externalFrameGeneration && state.activeFgOutput == FGOutput::DLSSG &&
        hookStatus != S_OK && hookStatus != HRESULT_FROM_WIN32(ERROR_NOT_READY))
        return false;
    return !_hookRetiring && !_ownerChanging && !_teardownCompleting && !_teardownPending && !_teardownQuarantined &&
        !g_dlssgTeardownQuarantined.load() && !_unretainedOffPending &&
        _reflexOwnership != ReflexOwnership::ReleasePending && !_starRailUiBusy && !_starRailUiFailed &&
        _inputContinuity.SourceClockValid();
}

std::shared_ptr<framegen::DlssgPacingLifetime> DLSSG_Dx12::PacingLifetime()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    const auto& state=State::Instance();
    if (state.currentFG!=this || state.isShuttingDown || !_isActive || IsPaused() ||
        !CanSubmitInputs() || !StreamlineProxy::IsD3D12Inited() ||
        !StreamlineProxy::RawGetNewFrameToken() || !StreamlineProxy::ReflexSleep() ||
        !StreamlineProxy::PCLSetMarker())
    {
        _pacingLifetime->CloseAdmission();
        return {};
    }
    return _pacingLifetime->OpenAdmission() ? _pacingLifetime : nullptr;
}

bool DLSSG_Dx12::Dispatch()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    PollGuideObservations();
    LOG_FUNC();

    if (_dispatchActive || _providerStateQueryActive || _dispatchOptions.InFlight() || _starRailGapSuppressing)
    {
        InvalidateDispatchOptions();
        return false;
    }
    _dispatchActive = true;
    struct FinishDispatch { bool& active; ~FinishDispatch() { active = false; } } finishDispatch { _dispatchActive };
    if (!CanSubmitInputs()) return false;
    const bool depthContractFrame=OwnsDepthContract();
    if (depthContractFrame && _depthSourceRejected) return false;
    const bool resumingInputGap = _inputContinuity.Held();
    const bool starRailFrame = IsStarRailContinuousOwner();
    const bool restartStarRailRuntime = starRailFrame && _starRailGapOffAccepted;
    const auto sourceGeneration = _inputContinuity.SourceGeneration();
    const bool resetStarRailHistory = starRailFrame && (resumingInputGap ||
        restartStarRailRuntime || _starRailDispatchGeneration != sourceGeneration);
    const auto sourceFrameCount = _frameCount;
    const auto sourceInputOwner = CurrentInputOwner();
    UINT64 willDispatchFrame = 0;
    int fIndex = -1;
    if (resumingInputGap)
    {
        if (!CanRetainForInputGap() || !_inputContinuity.OwnerMatches(CurrentInputOwner()) ||
            _frameCount < _inputContinuity.HoldFrame())
        {
            Deactivate();
            UpdateTarget();
            return false;
        }
        fIndex = GetIndex();
        if (!_inputContinuity.Fresh(fIndex, _frameCount, _lastDispatchedFrame, CurrentInputOwner(),
                !starRailFrame || _starRailDispatchGeneration == sourceGeneration))
            return false;
        willDispatchFrame = _frameCount;
    }
    else if (starRailFrame)
        fIndex = SelectStarRailF5DsFrame(willDispatchFrame);
    else if (IsStarRailUiOwner() || OwnsDepthContract())
    {
        // Both exact source owners use this frame, never an older ring entry.
        // StarRail may have deliberately skipped busy source frames without
        // clearing their slots; frame-ahead selection must not resurrect them.
        willDispatchFrame = _frameCount;
        fIndex = _lastDispatchedFrame == _frameCount ? -1 : GetIndex();
    }
    else
        fIndex = GetDispatchIndex(willDispatchFrame);
    if (fIndex < 0)
        return false;

    const auto nativeRecording=OwnsNativeZzzSource() ? FindNativeGuideSource(NativeGuideFrameSource(fIndex)) : nullptr;
    const auto nativeSource=nativeRecording ? nativeRecording->source : dlssg::NativeSource{};
    if (OwnsNativeZzzSource() && !NativeGuideSourceCurrent(nativeSource)) return false;
    if (!IsActive() || IsPaused())
        return false;
    if (depthContractFrame && !GuideTagsCurrent(fIndex,willDispatchFrame))
        return RefuseDepthContract("current-guide-pair");
    const auto sourceStillCurrent = [&]() {
        return sourceGeneration == _inputContinuity.SourceGeneration() && willDispatchFrame == _frameCount &&
            _inputContinuity.Current(fIndex, willDispatchFrame, CurrentInputOwner());
    };
    if (starRailFrame &&
        (!sourceStillCurrent() || (_starRailDispatchGeneration == sourceGeneration &&
                                  willDispatchFrame <= _starRailDispatchFrame)))
        return false; // Complete current tags and a high-water mark in this source generation only.

    LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    auto& state = State::Instance();

    // Reuse the existing non-Genshin observation before selecting options, so
    // a newly lowered raw maximum bounds this very submission. Genshin retains
    // its post-Present reader; only an unknown/changed owner needs a cold retry.
    sl::DLSSGState actualState {};
    auto stateResult = sl::Result::eErrorInvalidState;
    stateResult = QueryProviderState(actualState);if (!CanSubmitInputs() || !_isActive || sourceFrameCount != _frameCount ||
        sourceGeneration != _inputContinuity.SourceGeneration() || !(sourceInputOwner == CurrentInputOwner()))
        return false;
    const int requestedInterpolationCount = Config::Instance()->FGDLSSGInterpolationCount.value_or_default();
    const auto selection = _generationState.Select(CurrentProviderOwner(), requestedInterpolationCount,
        Config::Instance()->FGDLSSGForceDMFG.value_or_default());
    if (!selection) return false; // Unknown/zero capability is never assumed to support 2x, let alone 6x.

    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = selection.attemptedGenerated;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    // Allocate Streamline's UI-recomposition path only when this exact frame
    // has both successfully tagged color inputs. A lone or stale tag must not enable it.
    const auto tagAccepted = [&](FG_ResourceType type)
    {
        const auto tag = _resourceReady[fIndex].find(type);
        return tag != _resourceReady[fIndex].end() && tag->second;
    };
    const bool currentColourPair = _guideUiTagFrame[fIndex] == willDispatchFrame &&
        _guideUiTagGeneration[fIndex] == sourceGeneration;
    options.enableUserInterfaceRecomposition =
        dlssg::options::CanRecomposeUserInterface(
            tagAccepted(FG_ResourceType::HudlessColor), tagAccepted(FG_ResourceType::UIColor), currentColourPair)
            ? sl::eTrue : sl::eFalse;
    // Temporary panel control only. The normal acknowledged options transaction
    // detects the flag change, and restores it on a later safe Dispatch.
    if (GuideDiagnostics::OnlyGenerated().load(std::memory_order_acquire))
        options.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame;

    if (selection.dynamic)
    {
        options.mode = sl::DLSSGMode::eDynamic;
        options.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default();
    }

    if (CanRetainForInputGap())
        options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    StreamlineHooks::applyMenuDlssgInterlock(options, true);
    const auto optionsScope = CurrentDispatchScope(fIndex);
    const auto optionsFrame = _frameCount;
    const auto setAndObserveOptions = [&]() {
        // Any enable attempt can supersede an old Off, even if it reenters a
        // new clock before returning. Final SDK admission must suppress again.
        if (starRailFrame) _starRailGapOffAccepted = false;
        // Save the actual attempted mode/flags before the vendor call: even a
        // rejected call can leave state unknown and needs this Off recipe.
        const auto optionOwnerEpoch = _inputOwnerEpoch;
        const auto outcome = SetDispatchOptions(_dlssgOptionsAck, optionsScope, fIndex, options,
            willDispatchFrame, resumingInputGap && (!starRailFrame || restartStarRailRuntime),
            [&]() {
                // Invalidate even if this provider temporarily left the native
                // route: any actual options attempt supersedes its earlier Off.
                _nativeGapOffProof.Invalidate();
                if (nativeSource)
                {
                    _teardownOffAccepted=false;
                    _lastRunningOptions=options;
                }
                const auto setOptions = StreamlineProxy::DLSSGSetOptions();
                return setOptions ? StreamlineHooks::SubmitDlssgOptions(setOptions,viewport,options) : sl::Result::eErrorMissingOrInvalidAPI;
            });
        const bool optionsSubmitted = outcome.submitted;
        _generationState.Record(optionsScope, willDispatchFrame, selection, options, outcome);
        const auto result = outcome.Result() == sl::Result::eOk && !_generationState.acceptedKnown ?
            sl::Result::eErrorInvalidState : outcome.Result();
        try
        {
            auto& observation=_guideObservations[fIndex];
            if(observation.frame==willDispatchFrame && observation.snapshot.epoch==sourceGeneration)
            {
                auto& snapshot=observation.snapshot;
                snapshot.sdkOptionsKnown=_generationState.acceptedKnown;
                snapshot.sdkOptionsCalled=outcome.submitted;snapshot.sdkOptionsResult=static_cast<int>(result);
                snapshot.sdkOptionsMode=static_cast<int>(options.mode);
                GuideDiagnostics::Observe(GuideDiagnostics::Consumer::Fg,snapshot,
                    GuideDiagnostics::TraceFacts(snapshot,FrameTrace::Kind::SdkOptions),"SDK options",true,
                    static_cast<int>(result));
                PublishGuideObservation(fIndex,snapshot.stage,"SDK options transaction observed; acknowledgement is not generation proof");
            }
        }catch(...){}
        _framesToInterpolate = _generationState.acceptedKnown ? static_cast<int>(_generationState.acceptedGenerated) : 0;
        // Only this exact failed On/options transaction may suspend the child.
        // Off, GetState warnings and recursive owner changes cannot sign it.
        if (optionsSubmitted)
            framegen::DlssgTelemetry::RecordSetOptions(static_cast<unsigned int>((std::max)(requestedInterpolationCount, 0)),
                options.numFramesToGenerate, options.mode != sl::DLSSGMode::eOff,
                static_cast<unsigned int>(outcome.sdkResult));
        // GetState consumes numFramesActuallyPresented. Genshin observes it
        // only in the existing EndGenshinPresent query, after original Present.
        // Pre-Present telemetry explicitly describes an older observation.
        if (stateResult == sl::Result::eOk)
            framegen::DlssgTelemetry::RecordState(actualState.numFramesActuallyPresented);
        // Source metadata is read under Dispatch's existing continuity lock.
        // Reuse the one vendor result/query above; presented is a consumed,
        // previous observation, not this source frame's generated output.
        if (HookCpuTiming::Enabled() && [] {
            static const bool starRail = _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0;
            return starRail;
        }())
        {
            struct Observation
            {
                const void* provider = nullptr;
                uint64_t epoch = 0, changes = 0, calls = 0;
                int requested = 0, mode = 0, optionsResult = 0, stateResult = 0, status = 0;
                unsigned sent = 0, flags = 0, cap = 0, queueMode = 0;
                ULONGLONG lastRow = 0;
                bool observed = false, emitted = false;
            };
            static thread_local Observation previous;
            const bool changed = !previous.observed || previous.provider != this ||
                previous.epoch != _inputOwnerEpoch || previous.requested != requestedInterpolationCount ||
                previous.sent != options.numFramesToGenerate || previous.mode != (int)options.mode ||
                previous.flags != (unsigned)options.flags || previous.queueMode != (unsigned)options.queueParallelismMode ||
                previous.optionsResult != (int)result || previous.stateResult != (int)stateResult ||
                previous.cap != actualState.numFramesToGenerateMax || previous.status != (int)actualState.status;
            ++previous.calls;
            previous.changes += changed;
            previous.observed = true;
            previous.provider = this;
            previous.epoch = _inputOwnerEpoch;
            previous.requested = requestedInterpolationCount;
            previous.sent = options.numFramesToGenerate;
            previous.mode = (int)options.mode;
            previous.flags = (unsigned)options.flags;
            previous.queueMode = (unsigned)options.queueParallelismMode;
            previous.optionsResult = (int)result;
            previous.stateResult = (int)stateResult;
            previous.cap = actualState.numFramesToGenerateMax;
            previous.status = (int)actualState.status;
            const auto now = GetTickCount64();
            if (previous.changes && (!previous.emitted || now - previous.lastRow >= 500))
            {
                LOG_INFO("R789 StarRail FG options: pid={} thread={} provider={} input_epoch={} source_frame={} slot={} input_queue={} swapchain={} requested={} sent={} mode={} flags={} queue_mode={} options_result={} options_submitted={} state_result={} state_valid={} provider_cap={} observed_presented={} status={} state_is_not_current_source=true generated=UNKNOWN calls_since_row={} changes_since_row={} intermediate_order=UNKNOWN",
                    GetCurrentProcessId(), GetCurrentThreadId(), (uintptr_t)this, _inputOwnerEpoch,
                    willDispatchFrame, fIndex, (uintptr_t)_gameCommandQueue, (uintptr_t)_swapChain,
                    requestedInterpolationCount, options.numFramesToGenerate, (int)options.mode, (unsigned)options.flags,
                    (unsigned)options.queueParallelismMode, (int)result, optionsSubmitted, (int)stateResult, stateResult == sl::Result::eOk,
                    actualState.numFramesToGenerateMax, actualState.numFramesActuallyPresented, (int)actualState.status,
                    previous.calls, previous.changes);
                previous.emitted = true;
                previous.lastRow = now;
                previous.calls = previous.changes = 0;
            }
        }
        if (++_optionsTelemetryCalls % 300 == 1)
        {
            LOG_INFO("F5 actual MFG: requested {}, sent {}, optionsResult {}, optionsSubmitted {}, stateResult {}, providerCap {}, presented {}, status {}; generated UNKNOWN",
                    requestedInterpolationCount, options.numFramesToGenerate, (int) result, optionsSubmitted, (int) stateResult,
                    actualState.numFramesToGenerateMax, actualState.numFramesActuallyPresented, (int) actualState.status);
        }
        if (_optionsTelemetryCalls % 300 == 1 || result != sl::Result::eOk)
            LOG_INFO("F6 FG configuration: owner={} lifecycle={} source={} requested_generated={} attempted_generated={} options_result={} admission_result={} submitted={} accepted_known={} accepted_generated={} accepted_mode={} configured_fixed_total={} capability_known={} raw_max_generated={} sdk_interval_presented={}; generated=UNKNOWN scanout=UNKNOWN",
                _providerOwnerGeneration, _providerOptionsLifecycle, willDispatchFrame, requestedInterpolationCount,
                options.numFramesToGenerate, static_cast<int>(outcome.sdkResult), static_cast<int>(result),
                optionsSubmitted, _generationState.acceptedKnown, _generationState.acceptedGenerated,
                static_cast<int>(_generationState.acceptedMode), _generationState.FixedTotal(),
                _generationState.capabilityKnown, _generationState.rawMaxGenerated, _generationState.rawPresented);
        return result;
    };
    const auto setReflexOptions = [&]() {
        const auto reflexConst = dlssg::options::ProviderReflexOptions(
            native::lowlatency::WantsBoostForFrameGeneration());
        _reflexOwnership = ReflexOwnership::Running;
        _reflexOffReported = false;
        const auto outcome = SetDispatchOptions(_reflexOptionsAck, optionsScope, fIndex, reflexConst,
            willDispatchFrame, resumingInputGap && !starRailFrame,
            [&]() {
                const auto setOptions = StreamlineProxy::ReflexSetOptions();
                return setOptions ? setOptions(reflexConst) : sl::Result::eErrorMissingOrInvalidAPI;
            });
        const auto result = outcome.Result();
        if (result != sl::Result::eOk)
            LOG_ERROR("Couldn't set Reflex options, error: {}", magic_enum::enum_name(result));
        return result;
    };
    // F5-DS submits DLSSG options before Reflex on real guide Dispatches.
    sl::Result dispatchOptionsResult = sl::Result::eOk;
    if (!resumingInputGap && (dispatchOptionsResult = setAndObserveOptions()) != sl::Result::eOk)
    {
        LOG_ERROR("Couldn't set DLSSG options, error: {}", magic_enum::enum_name(dispatchOptionsResult));
        Deactivate();
        UpdateTarget();
        return false; // No owner may publish an unaccepted generated-frame count.
    }
    const auto reflexResult = setReflexOptions();
    if (reflexResult != sl::Result::eOk)
    {
        InvalidateDispatchOptions();
        if (nativeSource) return RefuseDepthContract("native-reflex-options");
        return false;
    }
    if (starRailFrame && !sourceStillCurrent()) return false;

    if (!_haveHudless.has_value())
    {
        _haveHudless = IsUsingHudless(fIndex);
    }

    // Direct upscaler colour was joined/tagged at the final endpoint. Retagging
    // an earlier raw candidate here could replace the final NR colour route.
    if (!_noHudless[fIndex] && !OwnsUpscalerDepthContract())
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    if (!_noDistortionField[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::Distortion];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    if (starRailFrame && !sourceStillCurrent()) return false;
    if (resumingInputGap && (!_isActive || !_inputContinuity.Held())) return false;
    sl::Constants constData = {};
    if (nativeSource) constData=nativeRecording->constants;
    else if (!BuildGuideConstants(fIndex,constData,!false)) return false;

    if (_depthSourceGap || (resumingInputGap && (!starRailFrame || resetStarRailHistory)))
        constData.reset = sl::Boolean::eTrue; // Real gap/source-generation reset, never each transient omission.

    // Never synthesize Genshin projection or previous-frame transforms from
    // requested near/FOV/default axes. Preserve the actual observed matrices.
    auto frameId = nativeSource ? nativeSource.sdkFrame : static_cast<uint32_t>(willDispatchFrame);

    sl::FrameToken* frameToken = nullptr;
    auto tokenResult = nativeSource ?
        (NativeGuideToken(nativeSource,frameToken) ? sl::Result::eOk : sl::Result::eErrorInvalidState) :
        StreamlineProxy::GetNewFrameToken()(frameToken, &frameId);
    if (tokenResult != sl::Result::eOk || frameToken == nullptr)
    {
        LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }


    if (starRailFrame && !sourceStillCurrent()) return false;
    if (depthContractFrame)
    {
        if (!GuideTagsCurrent(fIndex,willDispatchFrame))
            return RefuseDepthContract("guide-pair-changed-in-sdk");
        if (!std::isfinite(constData.mvecScale.x) || !std::isfinite(constData.mvecScale.y) ||
            !std::isfinite(constData.jitterOffset.x) || !std::isfinite(constData.jitterOffset.y) ||
            !std::isfinite(constData.cameraFOV) || constData.cameraFOV<=0 ||
            !std::isfinite(constData.cameraAspectRatio) || constData.cameraAspectRatio<=0)
            return RefuseDepthContract("guide-coordinate-constants");
        if (++_depthContractCalls<=3 || _depthContractCalls%600==0)
        {
            const auto& d=_guideTags[fIndex][0]; const auto& m=_guideTags[fIndex][1];
            LOG_INFO("R1054 FG tagged depth input: stage=pre-constants pid={} game={} provider={} input_epoch={} source_generation={} frame={} slot={} queue={} depth={} depth_format={} depth_texture={}x{} depth_rect={},{},{}x{} depth_state={} depth_lifecycle={} depth_flip={} motion={} motion_format={} motion_texture={}x{} motion_rect={},{},{}x{} motion_state={} motion_lifecycle={} motion_flip={} reversed_z={} mv_scale={},{} jitter={},{} tags_accepted=true sdk_depth_snapshot_contract={} sdk_motion_snapshot_contract={} gpu_copy_recorded=UNKNOWN gpu_read=UNKNOWN pixel_correct=UNKNOWN generated=UNKNOWN",
                GetCurrentProcessId(),(int)dlssg::depth_contract::Game(),(uintptr_t)this,_inputOwnerEpoch,
                _inputContinuity.SourceGeneration(),willDispatchFrame,fIndex,(uintptr_t)_gameCommandQueue,
                (uintptr_t)(d.input.copy?d.input.copy:d.input.resource),(int)d.description.Format,
                d.description.Width,d.description.Height,d.input.left,d.input.top,d.input.width,d.input.height,
                (unsigned)d.input.state,(int)d.input.validity,d.flipped,
                (uintptr_t)(m.input.copy?m.input.copy:m.input.resource),(int)m.description.Format,
                m.description.Width,m.description.Height,m.input.left,m.input.top,m.input.width,m.input.height,
                (unsigned)m.input.state,(int)m.input.validity,m.flipped,
                (int)constData.depthInverted,constData.mvecScale.x,constData.mvecScale.y,
                constData.jitterOffset.x,constData.jitterOffset.y,
                d.input.validity==FG_ResourceValidity::ValidNow,m.input.validity==FG_ResourceValidity::ValidNow);
        }
    }
    const auto nativeTokenFrame=nativeSource ? static_cast<uint32_t>(*frameToken) : 0;
    auto result = StreamlineProxy::SetConstants()(constData, *frameToken, viewport);
    if (nativeSource && (nativeSource.serial<=3 || nativeSource.serial%600==0 || result!=sl::Result::eOk))
        LOG_INFO("F6 ZZZ source constants: frame={} serial={} generation={} epoch={} sdk_frame={} token_frame={} list={} output={} result={} reset={} mv_scale={},{} jitter={},{} gpu_complete=UNKNOWN generated=UNKNOWN",
            nativeSource.frame,nativeSource.serial,nativeSource.generation,nativeSource.epoch,nativeSource.sdkFrame,
            nativeTokenFrame,nativeSource.list,nativeSource.output,static_cast<int>(result),static_cast<int>(constData.reset),
            constData.mvecScale.x,constData.mvecScale.y,constData.jitterOffset.x,constData.jitterOffset.y);
    if (result != sl::Result::eOk)
    {
        LOG_ERROR("SetConstants error: {} ({})", magic_enum::enum_name(result), (UINT) result);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    if (starRailFrame && !sourceStillCurrent()) return false;
    if (resumingInputGap)
    {
        if (options.mode == sl::DLSSGMode::eOff ||
            setAndObserveOptions() != sl::Result::eOk)
        {
            Deactivate();
            UpdateTarget();
            return false;
        }
        if (starRailFrame && !sourceStillCurrent()) return false;
        _lastDispatchedFrame = willDispatchFrame;
        _lastFGFrame = State::Instance().fgLastFrame;
        _inputContinuity.Resume();
        if (!_starRailUiGap)
            LOG_INFO("F5 FG continuity: fresh guides frame {}, history_reset={}, runtime_reenabled={}; generated UNKNOWN",
                willDispatchFrame, !starRailFrame || resetStarRailHistory, !starRailFrame || restartStarRailRuntime);
        _starRailUiGap = false;
    }
    if (depthContractFrame && (!GuideTagsCurrent(fIndex,willDispatchFrame) ||
        (nativeSource && !NativeGuideSourceCurrent(nativeSource))))
        return RefuseDepthContract("guide-pair-changed-after-constants");
    // SetConstants/retagging can reenter the owner just like SetOptions. Never
    // publish an old successful configuration as the new source's admission.
    if (!CanSubmitInputs() || !_isActive || optionsFrame != _frameCount ||
        !(optionsScope == CurrentDispatchScope(fIndex)) ||
        !_generationState.Accepted(optionsScope, willDispatchFrame)) return false;
    if (nativeSource && options.mode!=sl::DLSSGMode::eOff)
    {
        nativeRecording->dispatched=true;
        _nativeGuideDispatch=nativeSource; // Exact successful Dispatch, not a marker or latest-slot lookup.
    }
    if (CanRetainForInputGap()) _managedRetained = true;
    if (_managedRetained) _continuityOwner = true;
    _runningInputOwner = CurrentInputOwner();
    _lastRunningOptions = options;
    _haveRunningOptions = options.mode != sl::DLSSGMode::eOff;
    _depthSourceGap=false;
    if (IsStarRailUiOwner() || OwnsDepthContract())
    {
        _lastDispatchedFrame = willDispatchFrame;
        _lastFGFrame = State::Instance().fgLastFrame;
    }
    if (IsStarRailContinuousOwner())
    {
        if (!_starRailRuntimeOwner)
            LOG_INFO("R957 StarRail F5-DS scheduling selected: input_epoch={} source_frame={}; SDK unchanged, real GPU owners retained",
                _inputOwnerEpoch, willDispatchFrame);
        _starRailRuntimeOwner = true;
        _starRailGapOffAccepted = false;
        _starRailDispatchPending = _haveRunningOptions;
        _starRailDispatchFrame = willDispatchFrame;
        _starRailDispatchGeneration = sourceGeneration;
        _starRailDispatchOwner = _runningInputOwner;
    }
    LOG_DEBUG("Result: Ok");if (depthContractFrame)
    {
        try
        {
            auto& observation=_guideObservations[fIndex];auto& snapshot=observation.snapshot;
            snapshot.scaleX=constData.mvecScale.x;snapshot.scaleY=constData.mvecScale.y;
            snapshot.jitterX=constData.jitterOffset.x;snapshot.jitterY=constData.jitterOffset.y;
            snapshot.depthInverted=constData.depthInverted==sl::Boolean::eTrue;
            snapshot.accepted=true;
            snapshot.uiProtected=options.enableUserInterfaceRecomposition==sl::eTrue;
            snapshot.resetReason=constData.reset==sl::Boolean::eTrue ? "actual SDK reset=true" : "actual SDK reset=false";
            auto facts=GuideDiagnostics::TraceFacts(snapshot,FrameTrace::Kind::ConstantsAccepted);
            facts.token=static_cast<uint64_t>(*frameToken);facts.tokenKnown=true;
            facts.scaleX=constData.mvecScale.x;facts.scaleY=constData.mvecScale.y;
            facts.jitterX=constData.jitterOffset.x;facts.jitterY=constData.jitterOffset.y;
            facts.constantsKnown=true;facts.depthInverted=snapshot.depthInverted;
            facts.reset=constData.reset==sl::Boolean::eTrue;
            const auto& depthTag=_guideTags[fIndex][0];
            const auto& motionTag=_guideTags[fIndex][1];
            facts.depth=reinterpret_cast<uint64_t>(depthTag.input.copy?depthTag.input.copy:depthTag.input.resource);
            facts.motion=reinterpret_cast<uint64_t>(motionTag.input.copy?motionTag.input.copy:motionTag.input.resource);
            GuideDiagnostics::Observe(GuideDiagnostics::Consumer::Fg,snapshot,facts,"SDK constants",true,
                static_cast<int>(result));
            PublishGuideObservation(fIndex,observation.submitted?GuideDiagnostics::Stage::Submitted:
                GuideDiagnostics::Stage::Recorded,
                "current guide tags and constants accepted; SDK enabled/generated identity unknown; UI optional");
            // Producer completion may arrive before SDK constants. Preserve the
            // source-bound constants without granting SDK completion authority.
            auto& pending=_guidePendingCompletions[fIndex];
            if (pending.submitted && pending.snapshot.source==snapshot.source &&
                pending.frame==observation.frame && pending.snapshot.epoch==snapshot.epoch &&
                pending.snapshot.configuration==snapshot.configuration)
                pending.snapshot=snapshot;
        }
        catch (...) {}
    }

    // Menu interlocks may successfully submit Off. That does not authorize a
    // native source transaction at the next SDK Present.
    return !nativeSource || options.mode!=sl::DLSSGMode::eOff;
}

void* DLSSG_Dx12::FrameGenerationContext() { return (void*) 0x13371337; }

void* DLSSG_Dx12::SwapchainContext() { return (void*) 0x23372337; }

DLSSG_Dx12::~DLSSG_Dx12()
{
    framegen::final_output::RetireProvider(_finalOutputProvider);
    if (!Shutdown()) QuarantineFailedTeardown();
}

bool DLSSG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount)
{
    // The next authenticated Dispatch applies menu/config requests. This API
    // cannot acknowledge a new count merely because it is within capability.
    std::lock_guard continuityLock(_inputContinuityMutex);
    return _generationState.acceptedKnown && _generationState.acceptedOwner == CurrentProviderOwner() &&
        _generationState.acceptedMode == sl::DLSSGMode::eOn &&
        interpolatedFrameCount == _generationState.acceptedGenerated;
}

FGExecutionGuard DLSSG_Dx12::BeginNativeUpscalerInputs(ID3D12Device* device, FG_Constants& fgConstants)
{
    // Reentrant source callbacks already own continuity. Return a nested guard
    // so StartNewFrame refuses them before touching any slot or state. A busy
    // different thread follows the established FG Mutex -> continuity order.
    {
        std::unique_lock execution(_inputContinuityMutex,std::try_to_lock);
        if (execution.owns_lock() && (_presentExecutionDepth || _inputRecordingDepth))
            return GuardExecution(FGExecutionScope::Inputs);
    }
    OwnedLockGuard lock(Mutex,555);
    auto guard=GuardExecution(FGExecutionScope::Inputs);
    if (OwnsNativeZzzSource() && CanSubmitInputs()) EvaluateStateLocked(device,fgConstants);
    return guard;
}

void DLSSG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);
    std::lock_guard continuityLock(_inputContinuityMutex);
    EvaluateStateLocked(device,fgConstants);
}

void DLSSG_Dx12::EvaluateStateLocked(ID3D12Device* device, FG_Constants& fgConstants)
{
    auto& state = State::Instance();

    // If needed hooks are missing or XeFG proxy is not inited or FG swapchain is not created
    if (!StreamlineProxy::LoadStreamline() || state.currentFGSwapchain == nullptr)
        return;

    if (state.isShuttingDown)
    {
        return;
    }

    _constants = fgConstants;
    if (_hookRetiring) return;

    // If FG Enabled from menu
    if (Config::Instance()->FGEnabled.value_or_default())
    {
        if (_device == nullptr)
        {
            // Create it again
            CreateContext(device, fgConstants);
        }
        else if (state.fgChanged)
        {
            LOG_DEBUG("FGChanged");
            Deactivate();

            // Pause for 10 frames
            UpdateTarget();
        }

        if (State::Instance().activeFgInput == FGInput::Upscaler && !IsPaused() && !IsActive())
            Activate();
    }
    else
    {
        LOG_DEBUG("!FGEnabled");
        Deactivate();

        state.clearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();
    }

    if (state.fgChanged)
    {
        LOG_DEBUG("FGchanged");

        state.fgChanged = false;

        Hudfix_Dx12::ResetCounters();

        // Pause for 10 frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    state.scChanged = false;
}





void DLSSG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_hookRetiring || _teardownCompleting || _teardownPending || _teardownQuarantined ||
        g_dlssgTeardownQuarantined.load()) return;
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    InvalidateProviderCapabilities();
    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;
        ID3D12CommandQueue* cmdQueue = nullptr;

        // FG
        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            // Reset command list state
            _scCommandListResetted[i] = false;
            _scAllocatorFenceValues[i] = 0;

            _uiCommandListResetted[i] = false;
            _uiAllocatorFenceValues[i] = 0;

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_uiFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create UI fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_uiFenceEvent == nullptr)
            {
                _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_uiFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for UI fence failed");
                    break;
                }
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_scFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create SC fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_scFenceEvent == nullptr)
            {
                _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_scFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for SC fence failed");
                    break;
                }
            }
        }

    } while (false);
}

bool DLSSG_Dx12::IsZzzSCOwner() const
{
    static const bool zzz = [] {
        wchar_t path[32768] {};
        const auto size = GetModuleFileNameW(nullptr, path, (DWORD) std::size(path));
        if (!size || size >= std::size(path)) return false;
        const auto* name = wcsrchr(path, L'\\');
        return _wcsicmp(name ? name + 1 : path, L"ZenlessZoneZero.exe") == 0;
    }();
    const auto& state = State::Instance();
    return zzz && state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG &&
        state.currentFG == this && dlssg::depth_contract::SameOwner(state.currentFGSwapchain, _swapChain);
}

bool DLSSG_Dx12::HasZzzSCWork() const
{
    for (const auto& work : _zzzSCWork)
        if (work.queue) return true;
    return false;
}

bool DLSSG_Dx12::FailZzzSCWork(UINT index, const char* stage, HRESULT result)
{
    auto& work = _zzzSCWork[index];
    work.poisoned = true;
    _zzzSCFailed = true;
    // Also covers Reset / partially recorded shader failure. Only the existing
    // completed host drain may discard and reconstruct this list.
    _scCloseFailed[index] = true;
    if (!_zzzSCFailureReported)
    {
        _zzzSCFailureReported = true;
        LOG_ERROR("R711 SC failure: pid={} stage={} slot={} list={:X} allocator={:X} queue={:X} fence={:X} target={} submitted={} signalled={} hr={:X}; owners retained",
            GetCurrentProcessId(), stage, index, (size_t)_scCommandList[index], (size_t)_scCommandAllocator[index],
            (size_t)work.queue.Get(), (size_t)work.fence.Get(), _scAllocatorFenceValues[index],
            work.submitted, work.signalled, (UINT)result);
    }
    return false;
}

bool DLSSG_Dx12::PollZzzSCWork(bool* busy)
{
    if (busy) *busy = false; // Missing/failed ownership or Signal is never ordinary busy.
    // Caller holds _inputContinuityMutex. No CPU wait and no frame-number proof.
    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        auto& work = _zzzSCWork[i];
        if (!work.queue) continue;
        if (work.poisoned) return false;
        if (work.queue.Get() != _gameCommandQueue || work.fence.Get() != _scFence ||
            !dlssg::depth_contract::SameOwner(work.device.Get(), _device))
            return FailZzzSCWork(i, "owner-changed", DXGI_ERROR_INVALID_CALL);
        if (!work.submitted || !work.signalled || !_scAllocatorFenceValues[i]) return false;
        const auto completed = work.fence->GetCompletedValue();
        if (completed == UINT64_MAX || work.device->GetDeviceRemovedReason() != S_OK)
            return FailZzzSCWork(i, "device-removed", DXGI_ERROR_DEVICE_REMOVED);
        if (completed < _scAllocatorFenceValues[i])
        {
            if (busy) *busy = true;
            if (!_zzzSCReuseReported)
            {
                _zzzSCReuseReported = true;
                LOG_INFO("R711 SC reuse refused: pid={} slot={} list={:X} allocator={:X} queue={:X} fence={:X} target={} completed={}; poll only",
                    GetCurrentProcessId(), i, (size_t)_scCommandList[i], (size_t)_scCommandAllocator[i],
                    (size_t)work.queue.Get(), (size_t)work.fence.Get(), _scAllocatorFenceValues[i], completed);
            }
            return false;
        }
        // SC completion retires SC references only, never internal FG readers.
        work = {};
        _scAllocatorFenceValues[i] = 0;
    }
    return true;
}

ID3D12GraphicsCommandList* DLSSG_Dx12::GetSCCommandList(int index)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!CanSubmitInputs()) return nullptr;
    if (!_zzzSCCompletion && !IsZzzSCOwner()) return IFGFeature_Dx12::GetSCCommandList(index);
    _zzzSCBusy = false;
    _zzzSCCompletion = true;
    if (!IsZzzSCOwner() || !CanSubmitInputs()) return nullptr;
    if (index < 0) index = GetIndex();
    if (index < 0 || index >= BUFFER_COUNT) return nullptr;
    if (!_scCommandAllocator[0])
    {
        if (!_device) return nullptr;
        CreateObjects(_device);
    }
    if (!_scCommandAllocator[index] || !_scCommandList[index] || !_scFence || !_gameCommandQueue || !_swapChain)
        return nullptr;
    auto& work = _zzzSCWork[index];
    if (work.poisoned || _scCloseFailed[index]) return nullptr;
    if (_scCommandListResetted[index])
    {
        if (work.queue.Get() == _gameCommandQueue && work.fence.Get() == _scFence && !work.submitted)
            return _scCommandList[index]; // Append the second SC shader to this exact recording.
        FailZzzSCWork((UINT)index, "recording-owner", DXGI_ERROR_INVALID_CALL);
        return nullptr;
    }
    // RenderUI / HudlessCompare share mutable two-entry shader heaps across the
    // SC slots. Poll the SC pool before opening another recording as well as
    // protecting this allocator. Do not close/discard a different open slot.
    if (!PollZzzSCWork(&_zzzSCBusy)) return nullptr;
    work.device = _device;
    work.queue = _gameCommandQueue;
    work.fence = _scFence;
    Microsoft::WRL::ComPtr<ID3D12Device> queueDevice, fenceDevice;
    if (work.device->GetDeviceRemovedReason() != S_OK ||
        work.queue->GetDevice(IID_PPV_ARGS(&queueDevice)) != S_OK ||
        work.fence->GetDevice(IID_PPV_ARGS(&fenceDevice)) != S_OK ||
        !dlssg::depth_contract::SameOwner(queueDevice.Get(), _device) ||
        !dlssg::depth_contract::SameOwner(fenceDevice.Get(), _device))
    {
        FailZzzSCWork((UINT)index, "recording-device", DXGI_ERROR_DEVICE_REMOVED);
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    if (FAILED(_swapChain->QueryInterface(IID_PPV_ARGS(&swapchain))) ||
        FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&work.backbuffer))))
    {
        FailZzzSCWork((UINT)index, "backbuffer", DXGI_ERROR_INVALID_CALL);
        return nullptr;
    }
    auto result = _scCommandAllocator[index]->Reset();
    if (result == S_OK) result = _scCommandList[index]->Reset(_scCommandAllocator[index], nullptr);
    if (result != S_OK)
    {
        FailZzzSCWork((UINT)index, "Reset", result);
        return nullptr;
    }
    _scCommandListResetted[index] = true;
    return _scCommandList[index];
}

bool DLSSG_Dx12::SubmitZzzSCWork(UINT index)
{
    auto& work = _zzzSCWork[index];
    if (work.poisoned || _scCloseFailed[index]) return false;
    if (!_scCommandListResetted[index]) return true;
    if (!work.queue || !work.fence || work.submitted || work.queue.Get() != _gameCommandQueue ||
        work.fence.Get() != _scFence || !dlssg::depth_contract::SameOwner(work.device.Get(), _device) ||
        work.device->GetDeviceRemovedReason() != S_OK || _zzzSCFenceValue >= UINT64_MAX - 1)
        return FailZzzSCWork(index, "submit-owner", DXGI_ERROR_INVALID_CALL);
    const auto closeResult = _scCommandList[index]->Close();
    if (closeResult != S_OK) return FailZzzSCWork(index, "Close", closeResult);
    _scAllocatorFenceValues[index] = ++_zzzSCFenceValue;
    work.submitted = true; // Execute is void; subsequent Signal failure cannot cancel it.
    work.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&_scCommandList[index]);
    _scCommandListResetted[index] = false;
    const auto result = work.queue->Signal(work.fence.Get(), _scAllocatorFenceValues[index]);
    if (result != S_OK) return FailZzzSCWork(index, "Signal", result);
    work.signalled = true;
    if (!_zzzSCSubmitReported)
    {
        _zzzSCSubmitReported = true;
        LOG_INFO("R711 SC submitted: pid={} slot={} list={:X} allocator={:X} queue={:X} fence={:X} target={} completed={}; SC only, FG reader completion UNKNOWN",
            GetCurrentProcessId(), index, (size_t)_scCommandList[index], (size_t)_scCommandAllocator[index],
            (size_t)work.queue.Get(), (size_t)work.fence.Get(), _scAllocatorFenceValues[index], work.fence->GetCompletedValue());
    }
    return true;
}

bool DLSSG_Dx12::Present()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!CanSubmitInputs()) return false;
    ResetCompletedGuideRecordings();
    if (OwnsUpscalerDepthContract() && _depthSourceRejected) return false;
    const bool inputGap = _inputContinuity.Held();
    if (inputGap)
    {
        if (!CanRetainForInputGap() || !_inputContinuity.OwnerMatches(CurrentInputOwner()) ||
            _frameCount < _inputContinuity.HoldFrame())
        {
            Deactivate();
            UpdateTarget();
            return false;
        }
        // The Dx11 producer can still be recording depth work after tagging MV.
        // Do not close its command list until both same-frame guide tags completed.
        if (!_inputContinuity.Fresh(GetIndex(), _frameCount, _lastDispatchedFrame, CurrentInputOwner(),
                !IsStarRailContinuousOwner() ||
                _starRailDispatchGeneration == _inputContinuity.SourceGeneration()))
            return false;
    }
    // Fresh guides may have pending commands in this slot; submit them using the
    // existing queue/fence path before enabling the provider again.
    UINT64 selectedStarRailFrame = 0;
    auto fIndex = IsStarRailF5DsOwner() && !inputGap ? SelectStarRailF5DsFrame(selectedStarRailFrame) :
        (inputGap || IsStarRailUiOwner() || OwnsDepthContract()) ? GetIndex() : GetIndexWillBeDispatched();
    if (fIndex < 0) return false;
    if (!OrderNativeGuides(static_cast<UINT>(fIndex)))
        return CanSubmitInputs() ? RefuseDepthContract("native-source-not-submitted",false) : false;
    if (!inputGap && OwnsUpscalerDepthContract() &&
        (_dx11GuideSealedFrame[fIndex]!=_frameCount ||
         GuideCapture::GetPreviewMode()==GuideCapture::PreviewMode::FgColour))
        CaptureAcceptedGuides(static_cast<UINT>(fIndex));
    LOG_DEBUG("fIndex: {}", fIndex);const auto failSubmission = [&]() {
        // Includes a UI Execute followed by failed Signal: commands are already
        // submitted, so retain every real owner until the existing safe host drain.
        // No drain or reconstruction runs on this Present/recording stack.
        _managedRetained = true;
        Deactivate();
        UpdateTarget();
        std::unique_lock resources(_resourceMutex[fIndex]);
        _resourceReady[fIndex].clear();
        return false;
    };

    bool scRecordingFailed = false;
    bool scDeferred = false;

    if (!inputGap && Config::Instance()->FGDrawUIOverFG.value_or_default())
    {
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        if (ui && (ui->validity == FG_ResourceValidity::UntilPresent ||
                   ui->validity == FG_ResourceValidity::UntilPresentFromDispatch))
        {
            LOG_DEBUG("UI[{}] resource: {:X}, copy: {}", fIndex, (size_t) ui->resource, (size_t) ui->copy);
            if (_renderUI.get() == nullptr)
            {
                _renderUI = std::make_unique<RUI_Dx12>("RenderUI", _device,
                                                       Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
            }
            else
            {
                if (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() != _renderUI->IsPreMultipliedAlpha())
                {
                    bool busy = false;
                    if (_zzzSCCompletion && !PollZzzSCWork(&busy))
                    {
                        // Keep the old shader and its actual SC owners. Only this
                        // optional work is deferred; independent FG inputs still submit.
                        if (busy && !_zzzSCFailed) scDeferred = true;
                        else scRecordingFailed = true;
                    }
                    else
                    {
                        LOG_INFO("UI premultiplied alpha changed, recreating RenderUI");
                        _renderUI = std::make_unique<RUI_Dx12>(
                            "RenderUI", _device, Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
                    }
                }
                else if (_renderUI->IsInit())
                {
                    auto commandList = GetSCCommandList(fIndex);
                    if (!commandList)
                    {
                        if (!_zzzSCCompletion) return false; // Preserve non-ZZZ failure handling.
                        if (_zzzSCBusy && !_zzzSCFailed) scDeferred = true;
                        else scRecordingFailed = true;
                    }
                    else
                    {
                        if (_zzzSCCompletion) _zzzSCWork[fIndex].ui = ui->GetResource();
                        const bool recorded = _renderUI->Dispatch(
                            (IDXGISwapChain3*)_swapChain, commandList, ui->GetResource(), ui->state);
                        if (_zzzSCCompletion && !recorded)
                        {
                            FailZzzSCWork((UINT)fIndex, "RenderUI", E_FAIL);
                            scRecordingFailed = true;
                        }
                    }
                }
            }
        }
        else if (!ui)
        {
            LOG_WARN("UI resource is nullptr");
        }
    }

    // Resource shared locks above must end before the exceptional teardown request.
    if (scRecordingFailed) return failSubmission();

    // Deferred SC must not bypass the independent UI Execute/Signal or FG Dispatch below.
    if (!inputGap && !scDeferred && IsActive() && !IsPaused())
    {
        if (State::Instance().fgHudlessCompare)
        {
            auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
            if (hudless && (hudless->validity == FG_ResourceValidity::UntilPresent ||
                            hudless->validity == FG_ResourceValidity::UntilPresentFromDispatch))
            {
                LOG_DEBUG("Hudless[{}] resource: {:X}, copy: {}", fIndex, (size_t) hudless->resource,
                          (size_t) hudless->copy);
                if (_hudlessCompare.get() == nullptr)
                {
                    _hudlessCompare = std::make_unique<HC_Dx12>("HudlessCompare", _device);
                }
                else
                {
                    if (_hudlessCompare->IsInit())
                    {
                        auto commandList = GetSCCommandList(fIndex);
                        if (!commandList)
                        {
                            if (!_zzzSCCompletion) return false;
                            if (_zzzSCBusy && !_zzzSCFailed) scDeferred = true;
                            else scRecordingFailed = true;
                        }
                        else
                        {
                            if (_zzzSCCompletion) _zzzSCWork[fIndex].hudless = hudless->GetResource();
                            const bool recorded = _hudlessCompare->Dispatch(
                                (IDXGISwapChain3*)_swapChain, commandList, hudless->GetResource(), hudless->state);
                            if (_zzzSCCompletion && !recorded)
                            {
                                FailZzzSCWork((UINT)fIndex, "HudlessCompare", E_FAIL);
                                scRecordingFailed = true;
                            }
                        }
                    }
                }
            }
            else if (!hudless)
            {
                LOG_WARN("Hudless resource is nullptr");
            }
        }
    }

    if (scRecordingFailed) return failSubmission();

    // if (IsActive() && !IsPaused())
    {
        // SR's earlier output handback does not cover these later FG guide
        // reads. Queue the reverse dependency after their actual submission,
        // before DX11 is allowed to overwrite the shared input cache.
        if (_dx11GuideBorrowed[fIndex] &&
            !SealDx11GuideInputs(true,_gameCommandQueue)) return failSubmission();
        if (IsStarRailUiOwner())
        {
            if (!SubmitUICommandList(static_cast<UINT>(fIndex))) return false;
        }
        else if (_uiCommandListResetted[fIndex])
        {
            // The same real submission path serves early shared-guide sealing,
            // optional final-colour preview and ordinary Present recordings.
            if (!SubmitUICommandList(static_cast<UINT>(fIndex))) return failSubmission();
        }

        if (_zzzSCCompletion)
        {
            if (!SubmitZzzSCWork((UINT)fIndex)) return failSubmission();
        }
        else if (_scCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
            auto closeResult = _scCommandList[fIndex]->Close();

            if (closeResult != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);
                _scCloseFailed[fIndex] = true; // UI may already be executing on the real queue.
                return failSubmission();
            }
            _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
            _scCommandListResetted[fIndex] = false;
        }
    }

    // StarRail uses the successful fresh Dispatch at final SDK admission, never
    // the generic Present-counter grace to authorize missing-source generation.
    if (inputGap || IsStarRailF5DsOwner()) return Dispatch();

    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        if (SuspendForInputGap()) return false;
        LOG_DEBUG("Pausing FG");
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;

    return Dispatch();
}



// Mutable counters are touched only under the provider continuity lock. The
// actual fence/device/queue and epoch association never change after publishing
// any receipt. A replacement cache cannot release or relabel an old receipt.
void DLSSG_Dx12::RememberPresentQueue(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    _starRailGuidePresentQueue.Reset(); _starRailGuideSwapchain=0; _starRailGuideQueueCurrent=false;
    if (dlssg::depth_contract::Game()==GameRouting::Identity::StarRail && swapchain && queue) {
        Microsoft::WRL::ComPtr<IUnknown> identity;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&identity))) && identity) {
            _starRailGuidePresentQueue=queue;
            _starRailGuideSwapchain=reinterpret_cast<uintptr_t>(identity.Get());
            _starRailGuideQueueCurrent=dlssg::depth_contract::SameOwner(queue,_gameCommandQueue);
        }
    }
}

bool DLSSG_Dx12::SetResource(Dx12Resource* inputResource)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    // The ordinary upscaler can still call the shared virtual API. It cannot
    // overwrite the adapter's exact current frame or introduce a second clock.
    if (!CanSubmitInputs()) return false;
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // Revalidate at the shared virtual entry as well: a reentrant generic
    // caller inside vendor tagging cannot borrow the private tagging scope.
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    if (fIndex < 0 || fIndex >= BUFFER_COUNT) return false;

    auto& type = inputResource->type;
    if (OwnsUpscalerDepthContract() && type==FG_ResourceType::HudlessColor &&
        inputResource!=_upscalerColourPublication)
    {
        // Only the explicit post-successful-Evaluate capture scope can offer a
        // candidate. Ambient tracker callbacks cannot label an old image current.
        const auto source=_upscalerHudlessCapture;
        const auto captureSlot=GetIndex();
        if (!UpscalerColourCurrent(source) || !_upscalerColourSucceeded[captureSlot] ||
            source->list!=reinterpret_cast<uintptr_t>(inputResource->cmdList) || inputResource->copy ||
            inputResource->validity!=FG_ResourceValidity::UntilPresent ||
            !dlssg::depth_contract::OnDevice(_device,inputResource->resource)) return false;
        // Hudfix's capture counter used source->Frame(). Its legacy
        // GetIndexWillBeDispatched hint cannot redirect this exact-source copy.
        _upscalerPendingHudless[captureSlot]={source,*inputResource};
        _upscalerPendingHudless[captureSlot].input.frameIndex=captureSlot;
        _teardownBorrowedInputs=true;
        return true; // No SDK colour tag until the final NR mutation is known.
    }
    const auto colourJoin=_upscalerFinalColours[fIndex];
    const bool joinedColour=inputResource==_upscalerColourPublication && colourJoin &&
        UpscalerColourCurrent(colourJoin->source) && _upscalerColourSucceeded[fIndex];
    if (OwnsUpscalerDepthContract() && type==FG_ResourceType::HudlessColor &&
        (!joinedColour || !dlssg::colour::CanUseHudless(colourJoin->source->key,
            _upscalerPendingHudless[fIndex].source ? _upscalerPendingHudless[fIndex].source->key : dlssg::colour::Source{},
            colourJoin->mutation))) return false;
    const bool checkedGuide=OwnsDepthContract() &&
        (type==FG_ResourceType::Depth || type==FG_ResourceType::Velocity);
    const auto publicationOwner=CurrentInputOwner();
    if (checkedGuide)
    {
        if (!false && inputResource!=_upscalerGuidePublication)
            return RefuseDepthContract("unsealed-upscaler-guide-publication");
        if (fIndex!=GetIndex() || !dlssg::depth_contract::OnDevice(_device,inputResource->resource) ||
            !dlssg::depth_contract::Region(inputResource->resource->GetDesc(),inputResource->left,
                inputResource->top,inputResource->width,inputResource->height) || inputResource->copy)
            return RefuseDepthContract("guide-resource-device-or-region");
        if (!false && (!inputResource->cmdList ||
            inputResource->cmdList->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !dlssg::depth_contract::OnDevice(_device,inputResource->cmdList)))
            return RefuseDepthContract("guide-producer-recording");
    }
    const bool starRailGuide = IsStarRailContinuousOwner() &&
        (type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity);
    const auto sourceGeneration = _inputContinuity.SourceGeneration();
    const auto sourceFrame = _frameCount;
    // Optional HUD/UI tags use this source's SDK ID too. They cannot allocate
    // an ordinal-derived token in the independent SDK clock.
    const auto nativeRecording=OwnsNativeZzzSource() ? FindNativeGuideSource(NativeGuideFrameSource(fIndex)) : nullptr;
    const auto nativeSource=nativeRecording ? nativeRecording->source : dlssg::NativeSource{};
    if (OwnsNativeZzzSource() && (!nativeSource || !NativeGuideSourceCurrent(nativeSource))) return false;
    if (checkedGuide && nativeSource && nativeSource.list!=(uintptr_t)inputResource->cmdList)
        return RefuseDepthContract("native-guide-recording-identity");
    if (starRailGuide && (!_inputRecordingDepth ||
        _starRailRecordingGeneration != sourceGeneration || _starRailRecordingFrame != sourceFrame))
        return false; // A marker alone cannot turn an old recording into new-source guides.
    if (type == FG_ResourceType::PresentColor)
    {
        if (joinedColour)
        {
            const auto& endpoint=colourJoin->endpoint;const auto& desc=colourJoin->description;
            if (fIndex!=GetIndex() || ComIdentity::Get(inputResource->resource)!=endpoint.colour ||
                inputResource->validity!=FG_ResourceValidity::UntilPresent || inputResource->cmdList || inputResource->copy ||
                inputResource->state!=D3D12_RESOURCE_STATE_PRESENT || inputResource->top || inputResource->left ||
                inputResource->width!=desc.Width || inputResource->height!=desc.Height) return false;
        }
        else
        {
            return false; // Only a joined current upscaler colour may be tagged.
        }
    }

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (native::OpticalFgNrEnabled() && type == FG_ResourceType::HudlessColor) return false;

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        // Making a copy if it's just valid now to be able to use it later
        if (State::Instance().fgHudlessCompare && inputResource->validity == FG_ResourceValidity::ValidNow)
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;

        if (!_noHudless[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor)
    {
        if (Config::Instance()->FGDisableUI.value_or_default())
            return false;

        // Making a copy if it's just valid now
        if (Config::Instance()->FGDrawUIOverFG.value_or_default() &&
            inputResource->validity == FG_ResourceValidity::ValidNow)
        {
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;
        }

        if (!_noUi[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if (type == FG_ResourceType::Distortion)
    {
        if (!_noDistortionField[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) && _frameResources[fIndex].contains(type))
    {
        return false;
    }

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    if (type == FG_ResourceType::Distortion)
    {
        LOG_TRACE("Distortion field is not supported by XeFG");
        return false;
    }

    // Capture the policy at publication, before FlipResource records a read or
    // SDK tagging can partially accept it. A later route/config change cannot
    // turn a historical borrowed input into an immutable leased publication.
    _teardownBorrowedInputs = true;if (checkedGuide)
    {
        _guideContractOwned=true;
        _guideTags[fIndex][type==FG_ResourceType::Depth ? 0 : 1]={};
        _resourceReady[fIndex].erase(type);
    }
    auto fResource = &_frameResources[fIndex][type];
    if (checkedGuide) fResource->copy = nullptr; // An old allocation is not this guide publication's result.
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->top = inputResource->top;
    fResource->left = inputResource->left;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    const auto orientation=framegen::ResolveGuideOrientation(*Config::Instance());
    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    !native::OpticalFgNrEnabled() &&
                    !false && // R727: this exact private transaction already matches picture raster
                    ((type == FG_ResourceType::Velocity && orientation.MotionCopy()) ||
                     (type == FG_ResourceType::Depth && orientation.DepthCopy()));

    // A cached slot output is not a conversion result for this input.
    if (willFlip)
        fResource->copy = nullptr;
    if (checkedGuide && willFlip)
    {
        // RF dispatches this guide's exact extent, with its own bounds checks.
        // Nonzero NGX bases still require an explicit mapping contract.
        if (fResource->left || fResource->top)
            return RefuseDepthContract("flip-subrect-or-dispatch-coverage");
        // The selected route uses its own per-slot shader/constants/descriptors.
        // A shared shader's CPU upload must not race another in-flight slot.
    }
    const auto sourceState=fResource->state;
    if (checkedGuide && willFlip)
        ResourceBarrier(fResource->cmdList,fResource->resource,sourceState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool flipped = willFlip && _device != nullptr &&
        (checkedGuide ? FlipOwnedUpscalerGuide(fResource) : FlipResource(fResource));
    if (checkedGuide && willFlip)
        ResourceBarrier(inputResource->cmdList,inputResource->resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,sourceState);
    if (checkedGuide && willFlip && !flipped)
        return RefuseDepthContract("guide-flip-not-recorded");

    // We usually don't copy any resources for DLSSG, the ones with this tag are the exception
    if (inputResource->cmdList != nullptr && fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
    {
        LOG_DEBUG("Making a resource copy of: {}", magic_enum::enum_name(type));

        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex][type];

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        _resourceCopy[fIndex][type] = copyOutput;
        _resourceCopy[fIndex][type]->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;

        fResource->validity = FG_ResourceValidity::UntilPresent;
    }

    if (type == FG_ResourceType::UIColor)
        _noUi[fIndex] = false;
    else if (type == FG_ResourceType::Distortion)
        _noDistortionField[fIndex] = false;
    else if (type == FG_ResourceType::HudlessColor)
        _noHudless[fIndex] = false;

    if (joinedColour || (type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity || type == FG_ResourceType::PresentColor) ||
        fResource->validity != FG_ResourceValidity::UntilPresent)
    {
        // Only an actual conversion may extend a ValidNow caller's lifetime.
        fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || flipped)
                                  ? FG_ResourceValidity::UntilPresent
                                  : FG_ResourceValidity::ValidNow;

        if (type == FG_ResourceType::HudlessColor)
        {
            static DXGI_FORMAT lastFormat[BUFFER_COUNT] = {};
            auto desc = fResource->GetResource()->GetDesc();

            if (lastFormat[fIndex] != DXGI_FORMAT_UNKNOWN && lastFormat[fIndex] != desc.Format)
            {
                if (!joinedColour) State::Instance().fgChanged = true;
                return false;
            }

            lastFormat[fIndex] = desc.Format;
        }

        // ZZZ's external producer list uses SL's eOnlyValidNow contract.
        // StarRail preserves raw=ValidNow / owned conversion=UntilPresent;
        // no additional SDK snapshot is requested for its converted outputs.
        if (checkedGuide && !false &&
            dlssg::depth_contract::Game()!=GameRouting::Identity::StarRail)
            fResource->validity=FG_ResourceValidity::ValidNow;
        const auto actualDescription=checkedGuide ? fResource->GetResource()->GetDesc() : D3D12_RESOURCE_DESC{};
        if (checkedGuide && !dlssg::depth_contract::Region(actualDescription,fResource->left,
            fResource->top,fResource->width,fResource->height))
            return RefuseDepthContract("converted-guide-region");
        sl::Resource resource {};
        resource.height = checkedGuide ? actualDescription.Height : fResource->height;
        resource.native = fResource->GetResource();
        resource.state = fResource->state;
        resource.type = sl::ResourceType::eTex2d;
        resource.width = (uint32_t)(checkedGuide ? actualDescription.Width : fResource->width);
        if (checkedGuide) resource.nativeFormat=(uint32_t)actualDescription.Format;
        else if (joinedColour && type==FG_ResourceType::PresentColor)
            resource.nativeFormat=(uint32_t)colourJoin->description.Format;

        sl::ResourceTag resourceTag {};
        resourceTag.resource = &resource;

        switch (fResource->type)
        {
        case FG_ResourceType::PresentColor:
            resourceTag.type = sl::kBufferTypeBackbuffer;
            break;

        case FG_ResourceType::Depth:
            resourceTag.type = sl::kBufferTypeDepth;
            break;

        case FG_ResourceType::HudlessColor:
            resourceTag.type = sl::kBufferTypeHUDLessColor;
            break;

        case FG_ResourceType::UIColor:
            resourceTag.type = sl::kBufferTypeUIColorAndAlpha;
            break;

        case FG_ResourceType::Velocity:
            resourceTag.type = sl::kBufferTypeMotionVectors;
            break;

        default:
            return false;
        }

        resourceTag.lifecycle = fResource->validity == FG_ResourceValidity::UntilPresent
                                    ? ::sl::ResourceLifecycle::eValidUntilPresent
                                    : sl::ResourceLifecycle::eOnlyValidNow;

        resourceTag.extent.left = fResource->left;
        resourceTag.extent.top = fResource->top;
        resourceTag.extent.width = (uint32_t) fResource->width;
        resourceTag.extent.height = fResource->height;

        int indexDiff = GetIndex() - fIndex;
        if (indexDiff < 0)
            indexDiff += BUFFER_COUNT;

        // We will us UI color later with Render UI
        {
            auto frameId = nativeSource ? nativeSource.sdkFrame : static_cast<uint32_t>(_frameCount - indexDiff);

            sl::FrameToken* frameToken = nullptr;
            auto tokenResult = nativeSource ?
                (NativeGuideToken(nativeSource,frameToken) ? sl::Result::eOk : sl::Result::eErrorInvalidState) :
                StreamlineProxy::GetNewFrameToken()(frameToken, &frameId);
            if (tokenResult != sl::Result::eOk || frameToken == nullptr)
            {
                LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);
                if (!joinedColour && _inputContinuity.Held())
                {
                    State::Instance().fgChanged = true;
                    UpdateTarget();
                    Deactivate();
                }
                return false;
            }

            // Pin this exact input receipt across the SDK call; a reentrant
            // owner change must not publish the result into a newer slot.

            const auto nativeTagToken=nativeSource ? static_cast<uint32_t>(*frameToken) : 0;
            auto result = StreamlineProxy::SetTagForFrame()(*frameToken, viewport, &resourceTag, 1, fResource->cmdList);
            if (nativeSource && (nativeSource.serial<=3 || nativeSource.serial%600==0))
                LOG_INFO("F6 ZZZ source tag: api={} game_viewport={} provider_viewport={} serial={} frame={} upstream_raw={} sdk_frame={} token_frame={} type={} list={} thread={} result={}",
                    nativeSource.upstreamApi,nativeSource.upstreamViewport,nativeSource.providerViewport,
                    nativeSource.serial,nativeSource.frame,
                    nativeSource.sdkFrame,nativeSource.sdkFrame,nativeTagToken,static_cast<uint32_t>(resourceTag.type),
                    reinterpret_cast<uintptr_t>(fResource->cmdList),GetCurrentThreadId(),static_cast<int>(result));
            LOG_DEBUG("SetTagForFrame, frameId: {}, type: {} result: {} ({})", frameId, magic_enum::enum_name(type),
                          magic_enum::enum_name(result), (int32_t) result);if (result != sl::Result::eOk)
            {
                // This optional colour join cannot revoke the independent
                // accepted guide contract. Preserve normal failure handling
                // for required guides and all other publication routes.
                if (!joinedColour)
                {
                    State::Instance().fgChanged = true;
                    UpdateTarget();
                    Deactivate();
                }

                return false;
            }
            // Map retention is not SDK tag acceptance. Observe only the exact
            // leased HUDless input after its real successful SetTagForFrame.
            }

        // Potentially we don't need to restore but do it just to be safe
        if (inputResource->state == D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            ResourceBarrier(inputResource->cmdList, inputResource->resource, D3D12_RESOURCE_STATE_COPY_DEST,
                            inputResource->state);
        }

        // A recursive real marker may have changed clocks inside SDK tagging.
        // Do not publish that old call's receipt into the new generation.
        if (starRailGuide && (sourceGeneration != _inputContinuity.SourceGeneration() ||
            sourceFrame != _frameCount || _starRailRecordingGeneration != sourceGeneration ||
            _starRailRecordingFrame != sourceFrame)) return false;
        if (joinedColour && (!CanSubmitInputs() || !UpscalerColourCurrent(colourJoin->source) ||
            _upscalerFinalColours[fIndex]!=colourJoin)) return false;
        if (checkedGuide)
        {
            if (!OwnsDepthContract() || !CanSubmitInputs() || sourceFrame!=_frameCount ||
                sourceGeneration!=_inputContinuity.SourceGeneration() || !(publicationOwner==CurrentInputOwner()) ||
                (nativeSource && !NativeGuideSourceCurrent(nativeSource)))
                return RefuseDepthContract("guide-publication-reentered");
            auto& tag=_guideTags[fIndex][type==FG_ResourceType::Depth ? 0 : 1];
            tag.owner=publicationOwner;tag.input=*fResource;tag.description=actualDescription;
            tag.frame=sourceFrame;tag.generation=sourceGeneration;tag.nativeSerial=nativeSource.serial;
            tag.accepted=true;tag.flipped=flipped;
            // The SDK token clock remains separate from the Evaluate/cache ID.
            _guideObservations[fIndex].snapshot.providerFrame=nativeSource ?
                nativeSource.sdkFrame : static_cast<uint32_t>(sourceFrame);
        }
        if ((type==FG_ResourceType::HudlessColor || type==FG_ResourceType::UIColor) &&
            sourceFrame==_frameCount && sourceGeneration==_inputContinuity.SourceGeneration() &&
            publicationOwner==CurrentInputOwner())
        {
            // Only the successful SDK tag above counts; map retention and
            // optional capture/UI presence alone never assert protection.
            _guideUiTagFrame[fIndex]=sourceFrame;
            _guideUiTagGeneration[fIndex]=sourceGeneration;
        }
        SetResourceReady(type, fIndex);
        if (type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity)
        {
            // This stamp follows successful tagging and preserves the existing resource/fence contract.
            int age = GetIndex() - fIndex;
            if (age < 0) age += BUFFER_COUNT;
            if (SameDeviceOwner(_device, fResource->GetResource()))
                _inputContinuity.Observe(fIndex, _frameCount - age, CurrentInputOwner(),
                                         type == FG_ResourceType::Depth, sourceGeneration);
        }
    }

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());

    return true;
}

void DLSSG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_hookRetiring || _teardownCompleting || _teardownQuarantined || g_dlssgTeardownQuarantined.load()) return;
    if (_gameCommandQueue != queue)
    {
        _pacingLifetime->CloseAdmission();
        if (!_pacingLifetime->Idle()) return;
        // Mutable input queue setters cannot silently inherit a creation receipt.
        // This comparison runs only on a queue transition, never a frame wait.
        _starRailGuideQueueCurrent=_starRailGuidePresentQueue &&
            dlssg::depth_contract::SameOwner(_starRailGuidePresentQueue.Get(),queue);
        // Keep the old queue and its owners when SC completion is still unknown.
        if (_zzzSCCompletion && !PollZzzSCWork()) return;
        if (_managedRetained || _inputContinuity.Held()) Deactivate();
        if (!CompletePendingTeardown())
        {
            LOG_ERROR("F5 FG queue owner replacement vetoed: teardown incomplete");
            return;
        }
        ++_inputOwnerEpoch;
        _inputContinuity.Clear();
    }
    _gameCommandQueue = queue;
}
