#include "pch.h"
#include <ui_mask/DrawUiMask.h>

#include <native/OpticalFgNrPolicy.h>

#include "DLSSG_Dx12.h"
#include <hooks/SwapchainCreateRule.h>
#include "StarRailF5DsPolicy.h"
#include "FgDepthContract.h"
#include <resource_tracking/ResTrack_dx12.h>
#include <cmath>
#include <bit>
#include <string_view>
#include <inputs/GenshinFsrContract.h>
#include <genshin/GenshinHookCpuTiming.h>
#include <inputs/FG/GenshinBridge_Present_Dx12.h>
#include "Kcd2Hdr.h"

#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <hooks/Reflex_Hooks.h>
#include <hooks/Streamline_Hooks.h>
#include <native/NativeLowLatency.h>
#include "MfgUnlock.h"
#include <hooks/DxgiFactory_Hooks.h>

#include <magic_enum.hpp>

#include <DirectXMath.h>

using namespace DirectX;

// A failed terminal drain leaves one viewport quarantined until process exit.
// Do not create another owner over the same retained provider allocations.
static std::atomic<bool> g_dlssgTeardownQuarantined { false };

// Used by current-frame guide validation in Dispatch before its definition.
static bool SameGenshinComOwner(IUnknown* left, IUnknown* right);
// Keep startup and sparse steady-state detail without six emitted Debug lines
// on every Genshin frame. Generic routes retain their existing logging.
static bool DetailedGenshinFrameLog(UINT64 frame) { return frame <= 32 || frame % 300 == 1; }

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
    else
    {
        slFactory->Release();
    }

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

    sl::DLSSGState dlssgState {};
    sl::DLSSGOptions dlssgOptions {};
    const auto queryState = [&]() {
        std::lock_guard continuityLock(_inputContinuityMutex);
        return StreamlineProxy::DLSSGGetState()(viewport, dlssgState, &dlssgOptions);
    };
    if (queryState() == sl::Result::eOk)
    {
        _maxInterpolationCount = std::max(dlssgState.numFramesToGenerateMax, MfgUnlock::UnlockedMax());
        LOG_INFO("Max supported interpolations: {}", dlssgState.numFramesToGenerateMax);

        _supportsDMFG = dlssgState.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }

    SetCommandQueue(FG_ResourceType::Depth, cmdQueue);
    _swapChain = *swapChain;
    framegen::final_output::BindInput(_finalOutputProvider, _swapChain);
    RememberGenshinPresentQueue(_swapChain, cmdQueue);
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
        else
        {
            slFactory->Release();
        }

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

    sl::DLSSGState dlssgState {};
    sl::DLSSGOptions dlssgOptions {};
    const auto queryState = [&]() {
        std::lock_guard continuityLock(_inputContinuityMutex);
        return StreamlineProxy::DLSSGGetState()(viewport, dlssgState, &dlssgOptions);
    };
    if (queryState() == sl::Result::eOk)
    {
        _maxInterpolationCount = std::max(dlssgState.numFramesToGenerateMax, MfgUnlock::UnlockedMax());
        LOG_INFO("Max supported interpolations: {}", dlssgState.numFramesToGenerateMax);

        _supportsDMFG = dlssgState.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }

    SetCommandQueue(FG_ResourceType::Depth, cmdQueue);
    _swapChain = *swapChain;
    framegen::final_output::BindInput(_finalOutputProvider, _swapChain);
    RememberGenshinPresentQueue(_swapChain, cmdQueue);
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
            (uint32_t) Config::Instance()->FGResourceFlip.value_or_default(),
            (uint32_t) Config::Instance()->FGEnableDepthScale.value_or_default(),
            Config::Instance()->FGResourceFlip.value_or_default() ?
                (uint32_t)Config::Instance()->FGResourceFlipOffset.value_or_default() : 0u,
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
        // Called after the F5-DS Present-counter threshold, or an explicit
        // source discontinuity. Off/null tags are not GPU completion.
        _inputContinuity.Hold(_frameCount, CurrentInputOwner());
        _starRailDispatchPending = false;
        return SuppressStarRailGapForPresent();
    }
    InvalidateStarRailOptions();
    auto options = _lastRunningOptions;
    options.mode = sl::DLSSGMode::eOff; // Other selected SR routes retain their policy.
    auto result = StreamlineProxy::DLSSGSetOptions()(viewport, options);
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
    // The exact Genshin DX11 bridge has one Present-owned clock. Ordinary
    // upscaler callbacks cannot advance/recycle that ring a second time.
    for (const auto& retained : _genshinInputs)
        if (retained)
        {
            // A mutable route change cannot grant another caller ownership of
            // a still-live slot. Request Off, leave drain to its safe boundary.
            if (!IsGenshinBridgeOwner() && !_teardownPending) Deactivate();
            return _frameCount;
        }
    if (IsGenshinBridgeOwner() || _teardownPending || _teardownQuarantined ||
        g_dlssgTeardownQuarantined.load()) return _frameCount;
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
    _genshinConstantsFrame[GetIndex()] = 0;
    return frame;
}

void DLSSG_Dx12::SetFrameCountFromReflex(UINT64 frameId)
{
    // Use the same owner predicate and lock as BeginGenshinPresent/StartNewFrame.
    // A marker may arrive between Prepare and Dispatch; it cannot choose a new
    // ring index for this exact Present-owned route. Do not acquire FG Mutex here.
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_hookRetiring) return;
    for (const auto& retained : _genshinInputs)
        if (retained)
        {
            if (!IsGenshinBridgeOwner() && !_teardownPending) Deactivate();
            return;
        }
    if (IsGenshinBridgeOwner() || _teardownPending || _teardownQuarantined ||
        g_dlssgTeardownQuarantined.load()) return;

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
    if (_inputContinuity.Held()) return SuppressStarRailGapForPresent();
    if (_starRailPresentCounted) return true; // Present() already counted this actual SDK boundary.
    // Exact old Present() threshold. The early Prepare call does not count.
    // No old guides are retagged/redispatched during these display-only gaps.
    if ((_fgFramePresentId - _lastFGFramePresentId) > 3)
        return SuspendForInputGap();
    ++_fgFramePresentId;
    _starRailPresentCounted = true;
    return true;
}

bool DLSSG_Dx12::SuppressStarRailGapForPresent()
{
    if (_starRailGapOffAccepted) return true;
    if (!IsStarRailContinuousOwner() || !_inputContinuity.Held() || !_haveRunningOptions ||
        !_inputContinuity.OwnerMatches(CurrentInputOwner()) ||
        !(CurrentInputOwner() == _runningInputOwner)) return false;
    InvalidateStarRailOptions(); // A later resume must really send On, even if Off/tagging fails.
    auto options = _lastRunningOptions;
    options.mode = sl::DLSSGMode::eOff;
    options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    const auto setOptions = StreamlineProxy::DLSSGSetOptions();
    if (!setOptions || setOptions(viewport, options) != sl::Result::eOk) return false;

    // F5-DS's counter-qualified no-guide boundary, retaining owner safety.
    // NR consumption is independent; null tags cannot retire GPU readers.
    uint32_t id = static_cast<uint32_t>(_frameCount);
    sl::FrameToken* token = nullptr;
    const auto getToken = StreamlineProxy::GetNewFrameToken();
    const auto setTags = StreamlineProxy::SetTagForFrame();
    if (!getToken || !setTags || getToken(token, &id) != sl::Result::eOk || !token) return false;
    sl::ResourceTag tags[4] {};
    tags[0].type = sl::kBufferTypeDepth;
    tags[1].type = sl::kBufferTypeMotionVectors;
    tags[2].type = sl::kBufferTypeHUDLessColor;
    tags[3].type = sl::kBufferTypeUIColorAndAlpha;
    if (setTags(*token, viewport, tags, 4, nullptr) != sl::Result::eOk) return false;
    // F5-DS leaves Reflex running during input gaps. User Off and real
    // resize/device teardown still use the existing complete Off recipe.
    _starRailGapOffAccepted = true;
    _starRailDispatchPending = false;
    State::Instance().dlssgDetectedInterpolationCount = 0;
    ReflexHooks::setDlssgFrameCount(0);
    if (!_starRailUiGap)
        LOG_INFO("R957 StarRail F5-DS input gap: SDK Off+retain, Reflex retained; owner/slots/NR retained, frame {}; no synthetic guide dispatch, displayed output UNKNOWN", _frameCount);
    return true;
}

// Compare the GUID fields explicitly: neither SDK option padding nor an
// unknown extension/callback lifetime participates in a cache key.
static bool SameOptionStructType(const sl::StructType& a, const sl::StructType& b)
{
    if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3) return false;
    for (unsigned i = 0; i < 8; ++i)
        if (a.data4[i] != b.data4[i]) return false;
    return true;
}

static bool CacheableStarRailOptions(const sl::DLSSGOptions& o)
{
    return o.next == nullptr && o.structVersion == sl::kStructVersion5 &&
        SameOptionStructType(o.structType, sl::DLSSGOptions::s_structType) &&
        o.onErrorCallback == nullptr && o.mode != sl::DLSSGMode::eOff;
}

static bool SameStarRailOptions(const sl::DLSSGOptions& a, const sl::DLSSGOptions& b)
{
    // Complete payload for the explicitly supported v5, including default fields.
    // Header/extension/callback eligibility is checked before any copy or hit.
    return a.mode == b.mode && a.numFramesToGenerate == b.numFramesToGenerate &&
        a.flags == b.flags && a.dynamicResWidth == b.dynamicResWidth &&
        a.dynamicResHeight == b.dynamicResHeight && a.numBackBuffers == b.numBackBuffers &&
        a.mvecDepthWidth == b.mvecDepthWidth && a.mvecDepthHeight == b.mvecDepthHeight &&
        a.colorWidth == b.colorWidth && a.colorHeight == b.colorHeight &&
        a.colorBufferFormat == b.colorBufferFormat && a.mvecBufferFormat == b.mvecBufferFormat &&
        a.depthBufferFormat == b.depthBufferFormat && a.hudLessBufferFormat == b.hudLessBufferFormat &&
        a.uiBufferFormat == b.uiBufferFormat && a.bReserved15 == b.bReserved15 &&
        a.queueParallelismMode == b.queueParallelismMode &&
        a.enableUserInterfaceRecomposition == b.enableUserInterfaceRecomposition &&
        a.dynamicTargetFrameRate == b.dynamicTargetFrameRate;
}

static bool CacheableStarRailOptions(const sl::ReflexOptions& o)
{
    return o.next == nullptr && o.structVersion == sl::kStructVersion1 &&
        SameOptionStructType(o.structType, sl::ReflexOptions::s_structType) &&
        o.mode != sl::ReflexMode::eOff;
}

static bool SameStarRailOptions(const sl::ReflexOptions& a, const sl::ReflexOptions& b)
{
    return a.mode == b.mode && a.frameLimitUs == b.frameLimitUs &&
        a.useMarkersToOptimize == b.useMarkersToOptimize && a.virtualKey == b.virtualKey &&
        a.idThread == b.idThread;
}

void DLSSG_Dx12::InvalidateStarRailOptions()
{
    _starRailDlssgOptions.valid = _starRailReflexOptions.valid = false;
    _starRailOptionsScopeValid = false;
    // Saturate rather than allow a recursive publication ticket to recur.
    if (_starRailOptionsSerial != UINT64_MAX) ++_starRailOptionsSerial;
}

void DLSSG_Dx12::PrepareStarRailOptionsCache(int index)
{
    const auto& state = State::Instance();
    if (!IsStarRailContinuousOwner() || !_device || !_gameCommandQueue || !_swapChain ||
        !StreamlineProxy::IsD3D12Inited() || _starRailOptionsSerial == UINT64_MAX ||
        (state.gameQuirks & GameQuirk::CreateSLOnThe2ndDevice) ||
        viewport.next != nullptr || viewport.structVersion != sl::kStructVersion1 ||
        !SameOptionStructType(viewport.structType, sl::ViewportHandle::s_structType))
    {
        InvalidateStarRailOptions();
        return; // All other owners and unknown schemas keep their original calls.
    }
    StarRailOptionsScope scope {};
    scope.ownerEpoch = _inputOwnerEpoch;
    scope.sourceGeneration = _inputContinuity.SourceGeneration();
    scope.viewport = static_cast<uint32_t>(viewport);
    scope.width = _width; scope.height = _height;
    scope.displayWidth = _constants.displayWidth; scope.displayHeight = _constants.displayHeight;
    scope.guideFlags = (uint32_t)_constants.flags[FG_Flags::Hdr] |
        ((uint32_t)_constants.flags[FG_Flags::InvertedDepth] << 1) |
        ((uint32_t)_constants.flags[FG_Flags::JitteredMVs] << 2) |
        ((uint32_t)_constants.flags[FG_Flags::DisplayResolutionMVs] << 3) |
        ((uint32_t)_constants.flags[FG_Flags::Async] << 4) |
        ((uint32_t)_constants.flags[FG_Flags::InfiniteDepth] << 5);
    scope.consumers = (uint32_t)!_noHudless[index] | ((uint32_t)!_noUi[index] << 1) |
        ((uint32_t)!_noDistortionField[index] << 2);
    scope.providerCap = _maxInterpolationCount;
    scope.forceReflex = (uint32_t)Config::Instance()->FN_ForceReflex.value_or_default();
    scope.colorSpace = state.outputColorSpace.dxgiColorSpace;
    scope.transfer = (uint32_t)state.outputColorSpace.transfer;
    scope.primaries = (uint32_t)state.outputColorSpace.primaries;
    scope.range = (uint32_t)state.outputColorSpace.range;
    scope.colorModel = (uint32_t)state.outputColorSpace.model;
    scope.colorSpaceValid = state.outputColorSpace.valid;
    scope.flip = Config::Instance()->FGResourceFlip.value_or_default();
    scope.depthScale = Config::Instance()->FGEnableDepthScale.value_or_default();
    if (!_starRailOptionsScopeValid || !(scope == _starRailOptionsScope))
    {
        InvalidateStarRailOptions();
        _starRailOptionsScope = scope;
        _starRailOptionsScopeValid = _starRailOptionsSerial != UINT64_MAX;
    }
}

template<typename Options, typename Submit>
sl::Result DLSSG_Dx12::SetStarRailDispatchOptions(StarRailOptionsCache<Options>& cache,
    const Options& options, UINT64 sourceFrame, bool force, Submit&& submit, bool* submitted)
{
    if (submitted) *submitted = false;
    if (IsStarRailF5DsOwner())
    {
        InvalidateStarRailOptions();
        if (submitted) *submitted = true;
        return submit(); // F5-DS: each actual Dispatch submits SDK options.
    }
    const bool cacheable = _starRailOptionsScopeValid && sourceFrame == _frameCount &&
        _starRailOptionsScope.ownerEpoch == _inputOwnerEpoch &&
        _starRailOptionsScope.sourceGeneration == _inputContinuity.SourceGeneration() &&
        IsStarRailContinuousOwner() && CacheableStarRailOptions(options);
    // This is the real source frame selected by Dispatch, not a call counter or
    // an SDK generated-output count. Same-frame retries cannot age a keepalive.
    constexpr UINT64 keepaliveSourceFrames = 600;
    if (cacheable && !force && cache.valid && SameStarRailOptions(options, cache.options) &&
        sourceFrame >= cache.sourceFrame && sourceFrame - cache.sourceFrame < keepaliveSourceFrames)
        return sl::Result::eOk; // The identical request was actually acknowledged in this scope.

    cache.valid = false; // Failure (including keepalive failure) must retry next eligible call.
    if (!cacheable) InvalidateStarRailOptions();
    if (_starRailOptionsSerial != UINT64_MAX) ++_starRailOptionsSerial;
    const auto serial = _starRailOptionsSerial;
    if (submitted) *submitted = true;
    const auto result = submit();
    if (cacheable && result == sl::Result::eOk && serial != UINT64_MAX &&
        serial == _starRailOptionsSerial && _starRailOptionsScopeValid &&
        sourceFrame == _frameCount && _starRailOptionsScope.ownerEpoch == _inputOwnerEpoch &&
        _starRailOptionsScope.sourceGeneration == _inputContinuity.SourceGeneration() &&
        IsStarRailContinuousOwner())
    {
        cache.options = options; // Only the known pointer-free schema may be copied.
        cache.sourceFrame = sourceFrame;
        cache.valid = true;
    }
    else
        cache.valid = false; // A recursive success cannot certify a later outer failure/transition.
    return result;
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

bool DLSSG_Dx12::SubmitUICommandList(UINT index)
{
    if (_starRailUiFailed || !CanSubmitInputs()) return false;
    if (!IsStarRailUiOwner()) return IFGFeature_Dx12::SubmitUICommandList(index);
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
    return true;
}

DLSSG_Dx12::StarRailPresent DLSSG_Dx12::PrepareStarRailPresent(bool beforeSdkPresent)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
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
        if (!beforeSdkPresent) _starRailPresentCounted = false;
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

        // Only the real final SDK Present boundary advances the old counter.
        // Early NR selection cannot create a second missing-source observation.
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
    return !_hookRetiring && !_ownerChanging && !_teardownCompleting && !_teardownPending && !_teardownQuarantined &&
        !g_dlssgTeardownQuarantined.load() && !_starRailUiBusy && !_starRailUiFailed &&
        _inputContinuity.SourceClockValid();
}

bool DLSSG_Dx12::DisableRetainedProvider()
{
    InvalidateStarRailOptions();
    auto options = _lastRunningOptions;
    options.mode = sl::DLSSGMode::eOff;
    options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    const auto setOptions = StreamlineProxy::DLSSGSetOptions();
    const auto result = setOptions ? setOptions(viewport, options) : sl::Result::eErrorMissingOrInvalidAPI;
    _teardownOffAccepted = result == sl::Result::eOk;
    if (!_teardownOffAccepted)
        LOG_ERROR("F5 FG teardown: Off+retain failed, result {} ({}); submissions blocked",
            static_cast<int>(result), magic_enum::enum_name(result));
    return _teardownOffAccepted;
}

void DLSSG_Dx12::RetryGenshinReflexOff()
{
    if (_genshinReflexOwnership != GenshinReflexOwnership::ReleasePending ||
        State::Instance().currentFG != this) return; // Never disable a replacement provider's mode.
    const auto setOptions = StreamlineProxy::ReflexSetOptions();
    if (!setOptions)
    {
        if (!_genshinReflexOffReported)
            LOG_WARN("F5 Genshin Reflex Off pending: SDK entry unavailable; retry at existing Present boundary");
        _genshinReflexOffReported = true;
        return;
    }
    sl::ReflexOptions options {};
    options.mode = sl::ReflexMode::eOff;
    options.frameLimitUs = 0; // Release only this provider's SDK options, not the upstream NVAPI cap.
    options.useMarkersToOptimize = false;
    const auto result = setOptions(options);
    if (result != sl::Result::eOk)
    {
        if (!_genshinReflexOffReported)
            LOG_WARN("F5 Genshin Reflex Off pending: SDK result {}; retry at existing Present boundary", (int)result);
        _genshinReflexOffReported = true;
        return;
    }
    _genshinReflexOwnership = GenshinReflexOwnership::None;
    _genshinReflexOffReported = false;
    LOG_INFO("F5 Genshin Reflex Off: SDK accepted mode=eOff frame_limit_us=0 markers=false; driver state UNKNOWN");
}

void DLSSG_Dx12::Deactivate()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    InvalidateStarRailOptions();
    const bool wasActive = _isActive || _waitingNewFrameData || _inputContinuity.Held();
    _starRailDispatchPending = false;
    _starRailRecordingGeneration = 0;
    _starRailGapOffAccepted = false;
    _starRailPresentCounted = false;
    _starRailF5DsMarker.store(UINT64_MAX, std::memory_order_release);
    _starRailUiBusy = _starRailUiGap = false;
    _inputContinuity.Clear();
    _haveRunningOptions = false;
    ++_inputOwnerEpoch;
    _isActive = false;
    // IsActive also includes the input-gap flag. Off/recreate must leave the
    // retained provider inactive, even without another upscaler frame.
    _waitingNewFrameData = false;
    if (_teardownQuarantined) return;
    // An explicit teardown must drain even SC work recorded before the first
    // successful provider Dispatch; normal SC polling does not deactivate FG.
    if (_zzzSCCompletion && HasZzzSCWork()) _managedRetained = true;
    if (_managedRetained)
    {
        // Never free on this arbitrary caller/recording stack. The complete boundary
        // submits owned lists once, polls their actual queue, then calls slFreeResources.
        if (!_teardownPending)
        {
            _teardownPending = true;
            DisableRetainedProvider();
            LOG_INFO("F5 FG teardown requested: Off+retain, owner {:X}, frame {}", (size_t)_gameCommandQueue, _frameCount);
        }
    }
    else if (wasActive)
    {
        sl::DLSSGOptions options {};
        options.mode = sl::DLSSGMode::eOff;
        options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
        StreamlineProxy::DLSSGSetOptions()(viewport, options);
    }
    if (_genshinReflexOwnership != GenshinReflexOwnership::None)
    {
        // Do not lose an unsuccessful SDK Off when the logical-active flags above
        // are cleared, or when independent GPU retirement finishes successfully.
        _genshinReflexOwnership = GenshinReflexOwnership::ReleasePending;
        RetryGenshinReflexOff();
    }
    else if (wasActive)
    {
        sl::ReflexOptions reflexConst {};
        reflexConst.mode = sl::ReflexMode::eOff;
        reflexConst.useMarkersToOptimize = false;
        StreamlineProxy::ReflexSetOptions()(reflexConst);
    }
}

bool DLSSG_Dx12::CompletePendingTeardown()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownQuarantined || g_dlssgTeardownQuarantined.load() || _teardownCompleting) return false;
    _teardownCompleting = true;
    struct FinishCompletion { bool& active; ~FinishCompletion() { active = false; } } finish { _teardownCompleting };
    // SDK mode release can still be pending after GPU retirement is complete.
    // Its result never grants resource retirement or blocks the existing safe drain.
    RetryGenshinReflexOff();
    // The real Present hook calls this after FinishWork, including skipped
    // frames. Pending zero receipts cannot depend on another authenticated frame.
    CheckGenshinPendingInputs();
    if (!_teardownPending) {CollectGenshinInputs();return true;}
    if (_nativeGuideFailed) {QuarantineFailedTeardown();return false;}
    // The UI/presenting-queue drain cannot retire an external native recording.
    // Require its successful Reset plus actual producer completion, with no wait.
    if (!NativeGuidesRetired()) return false;
    // FinishWork on the exact real Present guard must have run. A failure inside
    // SetResource cannot close a producer's half-recorded command list.
    if (_inputRecordingDepth || _presentExecutionDepth) return false;
    if (!_teardownOffAccepted && !DisableRetainedProvider()) return false;
    if (!_device || !_gameCommandQueue || !StreamlineProxy::FreeResources()) return false;

    auto& drain = *_teardownDrain;
    if (drain.phase == TeardownDrain::Phase::Idle)
    {
        // Resolve fallible prerequisites BEFORE Execute. Once Submitting,
        // no failure or later Present may replay these command lists.
        if (!_teardownFence && FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_teardownFence))))
            return false;
        if (!_teardownFence) return false;
        drain.device = _device;
        drain.queue = _gameCommandQueue;
        drain.fence = _teardownFence;
        drain.swapchain = _swapChain;
        // CR-SR1: wDX12 cache publication has no reader lease and can refresh
        // before FG admission. Do not return across Present with those borrowed
        // inputs still in flight. Unknown producers and ZZZ remain synchronous too.
        drain.asynchronous = IsGenshinBridgeOwner() && !_teardownBorrowedInputs && !HasZzzSCWork();
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice, fenceDevice;
        if (_teardownFenceValue >= UINT64_MAX - 1 || drain.device->GetDeviceRemovedReason() != S_OK ||
            drain.queue->GetDevice(IID_PPV_ARGS(&queueDevice)) != S_OK ||
            drain.fence->GetDevice(IID_PPV_ARGS(&fenceDevice)) != S_OK ||
            !SameGenshinComOwner(queueDevice.Get(), drain.device.Get()) ||
            !SameGenshinComOwner(fenceDevice.Get(), drain.device.Get()))
        {
            QuarantineFailedTeardown();
            return false;
        }
        if (_zzzSCCompletion)
        {
            // A host fence covers failed SC Signal only on its retained queue.
            // A replacement queue/device supplies no completion authority.
            for (const auto& work : _zzzSCWork)
                if (work.queue && (work.queue.Get() != drain.queue.Get() || work.fence.Get() != _scFence ||
                                   !SameGenshinComOwner(work.device.Get(), drain.device.Get())))
                {
                    QuarantineFailedTeardown();
                    return false;
                }
        }
        for (UINT i = 0; i < BUFFER_COUNT; ++i)
        {
            std::shared_lock resources(_resourceMutex[i]);
            // Pin F5-owned copies only. Historical frame-map source pointers
            // can have expired; late AddRef cannot resurrect a borrowed input.
            // Only the Genshin transaction has complete immutable input/reader
            // leases. ZZZ SC owners cover SC inputs, not every borrowed guide;
            // its host boundary therefore remains synchronous in this candidate.
            for (const auto& [type, resource] : _resourceCopy[i])
                if (resource) drain.resources.emplace_back(resource);
        }
        drain.value = ++_teardownFenceValue;
        drain.phase = TeardownDrain::Phase::Submitting;
        for (UINT i = 0; i < BUFFER_COUNT; ++i)
        {
            if (_uiCommandListResetted[i] && !_uiCloseFailed[i])
            {
                if (!_uiCommandList[i]) { QuarantineFailedTeardown(); return false; }
                if (FAILED(_uiCommandList[i]->Close()))
                    _uiCloseFailed[i] = true;
                else
                {
                    drain.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&_uiCommandList[i]);
                    _uiCommandListResetted[i] = false; // Execute survives a failed Signal.
                }
            }
            if (_zzzSCCompletion && _scCommandListResetted[i] && !_scCloseFailed[i])
            {
                // Poisoned SC owners stay retained until the following single
                // host Signal completes; this is not an SC retry or Reset.
                SubmitZzzSCWork(i);
            }
            else if (!_zzzSCCompletion && _scCommandListResetted[i] && !_scCloseFailed[i])
            {
                if (!_scCommandList[i]) { QuarantineFailedTeardown(); return false; }
                if (FAILED(_scCommandList[i]->Close()))
                    _scCloseFailed[i] = true;
                else
                {
                    drain.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&_scCommandList[i]);
                    _scCommandListResetted[i] = false;
                }
            }
        }
        const auto signalResult = drain.queue->Signal(drain.fence.Get(), drain.value);
        if (signalResult != S_OK)
        {
            LOG_ERROR("R865 FG host drain Signal failed {:X}, target {}; submitted owners quarantined, restart required",
                      (UINT)signalResult, drain.value);
            QuarantineFailedTeardown();
            return false;
        }
        drain.phase = TeardownDrain::Phase::Polling;
    }
    // Qualified later Presents do not Execute, Signal, increment, register an event or wait.
    // Every slot and owner stays retained until this exact target completes.
    if (drain.phase != TeardownDrain::Phase::Polling && drain.phase != TeardownDrain::Phase::HostComplete) return false;
    if (drain.device.Get() != _device || drain.queue.Get() != _gameCommandQueue ||
        drain.fence.Get() != _teardownFence || drain.swapchain != _swapChain ||
        !drain.value || drain.value == UINT64_MAX || drain.device->GetDeviceRemovedReason() != S_OK)
    {
        QuarantineFailedTeardown();
        return false;
    }
    auto completed = drain.fence->GetCompletedValue();
    if (completed == UINT64_MAX)
    {
        QuarantineFailedTeardown();
        return false; // Device removal is not successful completion.
    }
    if (completed < drain.value)
    {
        if (drain.asynchronous) return false;
        // Preserve the synchronous host boundary for StarRail/other unleased
        // publications. A timeout is failure, never permission to free or reuse.
        if (!WaitForTeardownTarget()) { QuarantineFailedTeardown(); return false; }
        completed = drain.fence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < drain.value)
        { QuarantineFailedTeardown(); return false; }
    }
    if (drain.device->GetDeviceRemovedReason() != S_OK)
    {
        QuarantineFailedTeardown();
        return false;
    }
    drain.phase = TeardownDrain::Phase::HostComplete;
    const auto waitValue = drain.value;
    // NVIDIA official sample's public host-queue contract. This is not a claim
    // that private SL worker queues are exposed or individually waited by F5.
    InvalidateStarRailOptions(); // An SDK resource generation never inherits an option hit.
    const auto freeResult = StreamlineProxy::FreeResources()(sl::kFeatureDLSS_G, viewport);
    if (_teardownQuarantined) return false;
    if (freeResult != sl::Result::eOk)
    {
        LOG_ERROR("F5 FG teardown: slFreeResources failed {}, owner blocked", (int)freeResult);
        if (!drain.asynchronous) QuarantineFailedTeardown(); // No cross-Present reader lease on this route.
        return false;
    }
    // Only this existing safe boundary may discard an unsubmitted, poisoned list.
    // Never Reset/re-Close it on Present, or release an allocator before the host drain.
    const auto rebuildFailedList = [&](bool& failed, bool& recording,
                                       ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList*& list) {
        if (!failed) return true;
        if (!allocator) return false;
        SAFE_RELEASE(list);
        if (allocator->Reset() != S_OK ||
            _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                      IID_PPV_ARGS(&list)) != S_OK || !list)
            return false;
        if (list->Close() != S_OK) return false;
        ID3D12GraphicsCommandList* realList = nullptr;
        if (CheckForRealObject(__FUNCTION__, list, (IUnknown**)&realList)) list = realList;
        recording = false;
        failed = false;
        return true;
    };
    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        if (_uiCloseFailed[i] || _scCloseFailed[i])
        {
            std::unique_lock resources(_resourceMutex[i]);
            _resourceReady[i].clear(); // Discarded commands cannot authorize replay of old tags.
        }
        if (!rebuildFailedList(_uiCloseFailed[i], _uiCommandListResetted[i],
                               _uiCommandAllocator[i], _uiCommandList[i]) ||
            !rebuildFailedList(_scCloseFailed[i], _scCommandListResetted[i],
                               _scCommandAllocator[i], _scCommandList[i]))
        {
            // One attempt at the safe boundary; never retry reconstruction every frame.
            QuarantineFailedTeardown();
            return false;
        }
    }
    // A failed shader may have changed its CPU-side resource states while its
    // poisoned recording was never executed. Recreate only after this real drain.
    if (_zzzSCCompletion && _zzzSCFailed)
    {
        _renderUI.reset();
        _hudlessCompare.reset();
    }
    // Publish only after real Off, queue completion AND FreeResources=eOk.
    // Failed earlier calls leave these exact owner/epoch/frame receipts pending.
    CollectGenshinInputs(true);
    // Pins release only after host completion, SDK release and list recovery.
    // Retrying FreeResources never replays Execute or signals a fresh target.
    drain.resources.clear();
    // Only this already-proven boundary may discard resized guide/scale
    // allocations. A failed tag/Signal/FreeResources never reaches this loop.
    for (UINT i=0;_guideContractOwned && i<BUFFER_COUNT;++i)
    {
        std::unique_lock resources(_resourceMutex[i]);
        _guideDepthScale[i].reset();
        _guideFlip[i][0].reset();_guideFlip[i][1].reset();
        for (auto type : {FG_ResourceType::Depth,FG_ResourceType::Velocity})
        {
            auto copy=_resourceCopy[i].find(type);
            if (copy!=_resourceCopy[i].end()) { SAFE_RELEASE(copy->second); _resourceCopy[i].erase(copy); }
            _frameResources[i].erase(type); _resourceReady[i].erase(type);
        }
        _guideTags[i][0]={};_guideTags[i][1]={};
    }
    _guideContractOwned=false;
    for (auto& use:_nativeGuideUses) use={}; // Native recordings already physically retired above.
    _nativeGuideTracking.store(false,std::memory_order_release);
    _depthSourceRejected=_depthSourceGap=false;
    drain.swapchain = nullptr; drain.fence.Reset(); drain.queue.Reset(); drain.device.Reset();
    drain.value = 0;
    drain.phase = TeardownDrain::Phase::Idle;
    _teardownPending = false;
    _teardownBorrowedInputs = false;
    // The real Present/resize callback may finish a hook-requested drain before
    // the next factory/Release retry. Publish that completion to the same state
    // machine; otherwise that retry would submit a second host drain/target.
    if (_hookRetiring) _hookHostFreed = true;
    _teardownOffAccepted = false;
    _managedRetained = false;
    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        _uiAllocatorFenceValues[i] = 0; // Host queue completion covers every submitted slot.
        _scAllocatorFenceValues[i] = 0;
        if (_zzzSCCompletion) _zzzSCWork[i] = {};
    }
    _zzzSCFailed = false;
    _starRailUiFailed = false; // Only after Off, real host completion, free and list recovery.
    _starRailF5DsMarker.store(UINT64_MAX, std::memory_order_release);
    _starRailRuntimeOwner = _starRailGapOffAccepted = _starRailDispatchPending = false;
    _starRailDispatchFrame = _starRailDispatchGeneration = 0;
    _starRailRecordingFrame = _starRailRecordingGeneration = 0;
    _starRailDispatchOwner = {};
    LOG_INFO("F5 FG teardown complete: owned queue fence {}, slFreeResources=eOk", waitValue);
    return true;
}

bool DLSSG_Dx12::WaitForTeardownTarget()
{
    // Caller owns continuity. Used only at termination or the pre-existing
    // synchronous boundary for producers without an immutable input/reader lease.
    if (!_teardownDrain || _teardownDrain->phase != TeardownDrain::Phase::Polling) return false;
    const auto& drain = *_teardownDrain;
    if (!drain.fence || !drain.device || !drain.value || drain.value == UINT64_MAX ||
        drain.device->GetDeviceRemovedReason() != S_OK) return false;
    if (!_teardownEvent) _teardownEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!_teardownEvent || FAILED(drain.fence->SetEventOnCompletion(drain.value, _teardownEvent))) return false;
    const auto waitResult = WaitForSingleObject(_teardownEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("R872 FG host drain incomplete: target {}, wait {:X}; no release authority",
                  drain.value, waitResult);
        return false;
    }
    return true; // Wake is a hint; both callers recheck the actual target/device.
}

bool DLSSG_Dx12::CompletePendingTeardownForTermination()
{
    if (CompletePendingTeardown()) return true;
    if (_teardownCompleting || _teardownQuarantined || g_dlssgTeardownQuarantined.load() ||
        _inputRecordingDepth || _presentExecutionDepth || !_teardownPending || !WaitForTeardownTarget()) return false;
    return CompletePendingTeardown(); // Same target, no new Execute/Signal; not wake==completion.
}

FGHookRetirementResult DLSSG_Dx12::CompletePendingTeardownForHook(bool terminal)
{
    using Result = FGHookRetirementState;
    std::unique_lock execution(_inputContinuityMutex, std::try_to_lock);
    if (!execution.owns_lock() || _inputRecordingDepth || _presentExecutionDepth || _ownerChanging ||
        _teardownCompleting || _hookRetirementActive)
        return { Result::Refused, DXGI_ERROR_WAS_STILL_DRAWING };
    if (_teardownQuarantined || g_dlssgTeardownQuarantined.load())
        return { Result::Quarantined, DXGI_ERROR_WAS_STILL_DRAWING };
    const bool complete = terminal ? CompletePendingTeardownForTermination() : CompletePendingTeardown();
    if (complete) return { Result::Completed, S_OK };
    if (terminal || _teardownQuarantined)
    {
        QuarantineFailedTeardown();
        return { Result::Quarantined, DXGI_ERROR_WAS_STILL_DRAWING };
    }
    return { Result::Pending, DXGI_ERROR_WAS_STILL_DRAWING };
}

bool DLSSG_Dx12::PrepareForSwapchainChange(bool terminal)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    InvalidateStarRailOptions();
    if (_hookRetiring || _teardownCompleting || g_dlssgTeardownQuarantined.load()) return false;
    if (!_managedRetained && !_teardownPending && !_teardownQuarantined &&
        (!_zzzSCCompletion || !HasZzzSCWork())) return true;
    Deactivate();
    const bool completed = terminal ? CompletePendingTeardownForTermination() : CompletePendingTeardown();
    if (!completed && terminal) QuarantineFailedTeardown();
    return completed;
}

void DLSSG_Dx12::QuarantineFailedTeardown()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownQuarantined) return;
    _teardownQuarantined = true;
    g_dlssgTeardownQuarantined.store(true);
    _isActive = false;
    _waitingNewFrameData = false;
    _teardownPending = true;
    // Preserve the preallocated graph without callbacks or a raw provider pointer.
    // Failed Shutdown must not let member destructors release pending guides,
    // UI masks, input resources, or their independent queue/fence timelines.
    auto* retained = _teardownDrain.release();
    for (UINT i = 0; i < BUFFER_COUNT; ++i) {
        if(const auto& lease=_genshinInputs[i])
            DrawUiMask::Observe(lease->uiMask,DrawUiMask::Consumer::Fg,DrawUiMask::Lifecycle::Quarantined,
                reinterpret_cast<uintptr_t>(lease.get()),lease->epoch,lease->frame);
        retained->inputs[i] = std::move(_genshinInputs[i]);
    }
    retained->waitFence = std::move(_genshinWaitFence);
    retained->hostFence = std::move(_genshinHostFence);
    _genshinCreatedPresentQueue.Detach();
    _starRailGuidePresentQueue.Detach(); // Keep the real presenting-queue owner on terminal failure.
    // Exceptional terminal failure: keep owned references alive for process lifetime,
    // and globally veto a replacement owner for this viewport. Never release in-flight
    // objects merely to make a timeout look like successful destruction.
    if (_device) _device->AddRef();
    if (_gameCommandQueue) _gameCommandQueue->AddRef();
    if (_swapChain) _swapChain->AddRef();
    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        // Historical borrowed source pointers cannot be resurrected here.
        // Only already-owned copies/receipts below may survive a failed drain.
        for (const auto& [type, resource] : _resourceCopy[i])
            if (resource) resource->AddRef();
        _uiCommandAllocator[i] = nullptr; _uiCommandList[i] = nullptr;
        _scCommandAllocator[i] = nullptr; _scCommandList[i] = nullptr;
        // Match the existing terminal quarantine: no ComPtr destructor may
        // release a submitted owner merely because this provider is destroyed.
        auto& work = _zzzSCWork[i];
        work.device.Detach(); work.queue.Detach(); work.fence.Detach();
        work.backbuffer.Detach(); work.ui.Detach(); work.hudless.Detach();
        dlssgFence[i] = nullptr;
        _guideDepthScale[i].release(); // Preserve descriptors/constants/buffer after uncertain submission.
        _guideFlip[i][0].release();_guideFlip[i][1].release();
        auto& native=_nativeGuideUses[i];
        native.list.Detach(); native.nativeList.Detach(); native.queue.Detach(); native.fence.Detach();
        native.depth.Detach(); native.motion.Detach();
    }
    _renderUI.release(); _hudlessCompare.release(); _mvFlip.release(); _depthFlip.release();
    _uiFence = nullptr; _scFence = nullptr;
    _teardownFence = nullptr; _teardownEvent = nullptr;
    LOG_ERROR("F5 FG terminal teardown quarantined; provider references kept until process exit; restart required");
}

void DLSSG_Dx12::DestroyFGContext()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownCompleting) return;
    Deactivate();
    if (!CompletePendingTeardown()) return;
    ReleaseObjects();
}

bool DLSSG_Dx12::Shutdown()
{
    std::lock_guard continuityLock(_inputContinuityMutex);

    if (_teardownCompleting) return false;
    Deactivate();
    if (!CompletePendingTeardownForTermination())
    {
        QuarantineFailedTeardown();
        return false;
    }
    MenuOverlayDx::CleanupRenderTarget(true, NULL);
    if (!ReleaseObjectsChecked())
    {
        QuarantineFailedTeardown();
        return false;
    }

    if (State::Instance().isShuttingDown)
        StreamlineProxy::Shutdown()();

    return true;
}

bool DLSSG_Dx12::OwnsDepthContract() const
{
    const auto& state=State::Instance();
    const auto game=dlssg::depth_contract::Game();
    return game!=GameRouting::Identity::Other && state.currentFG==this &&
        state.activeFgInput==FGInput::Upscaler && state.activeFgOutput==FGOutput::DLSSG &&
        !state.externalFrameGeneration && !native::OpticalFgNrEnabled();
}

bool DLSSG_Dx12::RefuseDepthContract(const char* stage, bool publicationStarted)
{
    const auto now=GetTickCount64();
    if (_depthContractRows<64 && (_depthContractRows<3 ||
        (stage!=_depthContractStage && now-_depthContractLogTick>=1000)))
    {
        ++_depthContractRows; _depthContractLogTick=now; _depthContractStage=stage;
        LOG_WARN("R1054 FG depth refusal: pid={} game={} provider={} epoch={} frame={} stage={} pixel_correct=UNKNOWN generated=UNKNOWN",
            GetCurrentProcessId(),(int)dlssg::depth_contract::Game(),(uintptr_t)this,
            _inputOwnerEpoch,_frameCount,stage);
    }
    // Runtime activation/FGEnabled/fgChanged are deliberately NOT route identity:
    // before the first EvaluateState they can be false for a legitimate StarRail
    // source (e.g. the missing-exposure backend replacement).
    const bool starRailSource=OwnsUpscalerDepthContract() &&
        dlssg::depth_contract::Game()==GameRouting::Identity::StarRail &&
        State::Instance().swapchainInteropApi==SwapchainInteropApi::Dx11wDx12 &&
        !State::Instance().gameCalledSlInit;
    if (starRailSource || !publicationStarted)
    {
        // A missing/rejected source is a retained input gap in adamfg/F5-DS,
        // not a physical teardown. Do not introduce FreeResources, allocator
        // waits, pool reconstruction, Reflex Off, or same-frame retries.
        _guideTags[GetIndex()][0].accepted=false;
        _guideTags[GetIndex()][1].accepted=false;
        _starRailDispatchPending=false;
        _depthSourceRejected=true;
        if (starRailSource)
        {
            if (_haveRunningOptions)
                _inputContinuity.Hold(_frameCount,CurrentInputOwner());
            // The existing F5-DS final admission sends Off+retain/null tags.
            // With no running inputs there is no GPU work to tear down.
            return false;
        }
        if (!_haveRunningOptions) return false;
        // Native missing/busy sources suspend SDK generation, not SR/NR or
        // physical owners. A later complete source sends On/reset normally.
        // Do not create _managedRetained merely because validation failed.
        if (!_depthSourceGap)
        {
            auto options=_lastRunningOptions;
            options.mode=sl::DLSSGMode::eOff;
            options.flags|=sl::DLSSGFlags::eRetainResourcesWhenOff;
            uint32_t id=static_cast<uint32_t>(_frameCount);
            sl::FrameToken* token=nullptr;
            sl::ResourceTag tags[4]{};
            tags[0].type=sl::kBufferTypeDepth; tags[1].type=sl::kBufferTypeMotionVectors;
            tags[2].type=sl::kBufferTypeHUDLessColor; tags[3].type=sl::kBufferTypeUIColorAndAlpha;
            if (StreamlineProxy::DLSSGSetOptions()(viewport,options)!=sl::Result::eOk ||
                StreamlineProxy::GetNewFrameToken()(token,&id)!=sl::Result::eOk || !token ||
                StreamlineProxy::SetTagForFrame()(*token,viewport,tags,4,nullptr)!=sl::Result::eOk)
            {
                // Actual SDK failure retains the original safe failure boundary.
                Deactivate();
                return false;
            }
            _depthSourceGap=true;
        }
        return false;
    }
    // Other routes keep the existing Off+retain/drain/free failure boundary.
    // A partial tag/conversion can already have recorded real GPU reads.
    _managedRetained=true;
    Deactivate();
    return false;
}

bool DLSSG_Dx12::OwnsUpscalerDepthContract() const
{
    return OwnsDepthContract() && dlssg::depth_contract::Game()!=GameRouting::Identity::Genshin;
}

void DLSSG_Dx12::RejectUpscalerGuideSource(const char* stage)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (OwnsUpscalerDepthContract() && CanSubmitInputs()) RefuseDepthContract(stage,false);
}

bool DLSSG_Dx12::FlipOwnedUpscalerGuide(Dx12Resource* input)
{
    const auto slot=GetIndex();
    const bool depth=input->type==FG_ResourceType::Depth;
    auto& shader=_guideFlip[slot][depth?0:1];
    if (!shader) shader=std::make_unique<RF_Dx12>(depth?"FG owned depth flip":"FG owned motion flip",_device);
    if (!shader->IsInit()) return false;
    auto& output=_resourceCopy[slot][input->type];
    if (!CreateBufferResource(_device,input->resource,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        &output,true,depth)) return false;
    if (!shader->Dispatch(input->cmdList,input->resource,output,input->width,input->height,!depth)) return false;
    input->copy=output;input->state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

bool DLSSG_Dx12::SetUpscalerGuidePair(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Parameter* parameters,
    ID3D12Resource* depth, ID3D12Resource* motion, UINT width, UINT height, UINT mvWidth, UINT mvHeight,
    D3D12_RESOURCE_STATES depthState, D3D12_RESOURCE_STATES motionState)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!OwnsUpscalerDepthContract() || !CanSubmitInputs()) return false;
    const bool nativeZzzSource=dlssg::depth_contract::Game()==GameRouting::Identity::ZenlessZoneZero &&
        State::Instance().swapchainInteropApi!=SwapchainInteropApi::Dx11wDx12;
    // Activate starts the normal target-frame pause. SetResource rejects guides
    // during that pause, so defer before recording conversions or reserving a
    // native receipt. This is scheduled warmup, not partial SDK publication.
    // UpscaleStart already advanced the frame; SR/NR continue toward the target.
    if (nativeZzzSource && (!IsActive() || IsPaused())) return false;
    if (!_inputRecordingDepth || !parameters || !list || list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !dlssg::depth_contract::OnDevice(_device,list) ||
        !dlssg::depth_contract::OnDevice(_device,depth) || !dlssg::depth_contract::OnDevice(_device,motion) ||
        dlssg::depth_contract::SameOwner(depth,motion)) return RefuseDepthContract("source-guide-owner",false);
    UINT left=0,top=0,mvLeft=0,mvTop=0;
    dlssg::depth_contract::InputRect(parameters,true,left,top);
    dlssg::depth_contract::InputRect(parameters,false,mvLeft,mvTop);
    const auto depthDesc=depth->GetDesc(),motionDesc=motion->GetDesc();
    if (!dlssg::depth_contract::DepthFormat(depthDesc.Format) ||
        !dlssg::depth_contract::MotionFormat(motionDesc.Format))
        return RefuseDepthContract("source-guide-format",false);
    if (!dlssg::depth_contract::Region(depthDesc,left,top,width,height) ||
        !dlssg::depth_contract::Region(motionDesc,mvLeft,mvTop,mvWidth,mvHeight))
        return RefuseDepthContract("source-guide-subrect",false);
    const bool scale=Config::Instance()->FGEnableDepthScale.value_or_default();
    const bool flip=Config::Instance()->FGResourceFlip.value_or_default();
    if ((scale && (left || top)) || (flip && (left || top || mvLeft || mvTop)))
        return RefuseDepthContract("conversion-subrect-not-supported",false);
    auto* feature=State::Instance().currentFeature;
    const UINT scaleWidth=feature?feature->DisplayWidth():0;
    const UINT scaleHeight=feature?feature->DisplayHeight():0;
    if (scale && (!feature || width>scaleWidth || height>scaleHeight || !scaleWidth || !scaleHeight))
        return RefuseDepthContract("depth-scale-output-domain",false);
    // The existing helper cannot make a readable view of R16_TYPELESS and
    // throws for DENY_SHADER_RESOURCE. Do not record an invalid conversion.
    if ((scale || flip) && ((depthDesc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        depthDesc.Format==DXGI_FORMAT_R16_TYPELESS ||
        (flip && (motionDesc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))))
        return RefuseDepthContract("conversion-srv-unavailable",false);
    const auto slot=GetIndex();
    const bool ownedRecording=list==_uiCommandList[slot] && _uiCommandListResetted[slot] &&
        _uiFence && _uiAllocatorFenceValues[slot] && _gameCommandQueue &&
        dlssg::depth_contract::OnDevice(_device,_gameCommandQueue);
    const bool nativeZzz=!ownedRecording && nativeZzzSource;
    // Native ZZZ records on the real SR list, not a provider UI list. Track
    // that exact recording's Execute/Signal/Reset before reusing shader state.
    if (!ownedRecording && !nativeZzz)
        return RefuseDepthContract("conversion-producer-completion-unavailable",false);
    if (nativeZzz)
    {
        if (!_nativeGuideHooksRequested)
        {
            ResTrack_Dx12::HookLateNrQueue(_device); // Reuse the existing hooks, also when NR is off.
            _nativeGuideHooksRequested=true;
        }
        auto& use=_nativeGuideUses[slot];
        if (_nativeGuideFailed) return false;
        if (_device->GetDeviceRemovedReason()!=S_OK ||
            (use.fence && use.fence->GetCompletedValue()==UINT64_MAX))
        {
            _nativeGuideFailed=true; _managedRetained=true; Deactivate();
            return false;
        }
        if (!NativeGuideRetired(use))
            return RefuseDepthContract("native-guide-slot-in-flight",false);
        if (!use.fence && FAILED(_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&use.fence))))
            return RefuseDepthContract("native-guide-fence-unavailable",false);
        if (!use.fence || use.value>=UINT64_MAX-1 || use.fence->GetCompletedValue()==UINT64_MAX)
            return RefuseDepthContract("native-guide-fence-invalid",false);
        if (_nativeGuideSerial>=UINT64_MAX-1)
        {
            _nativeGuideFailed=true; _managedRetained=true; Deactivate();
            return false;
        }
        ++use.value; use.serial=++_nativeGuideSerial;
        use.generation=_inputContinuity.SourceGeneration(); use.epoch=_inputOwnerEpoch;
        use.list=list; use.nativeList=dlssg::depth_contract::NativeObject(list);
        use.queue.Reset(); use.depth=depth; use.motion=motion;
        use.frame=_frameCount;
        use.closed=use.submitted=use.executing=use.signalled=use.srSucceeded=use.ordered=false;
        _nativeGuideTracking.store(true,std::memory_order_release);
    }
    if ((scale || flip) && dlssg::depth_contract::Game()==GameRouting::Identity::StarRail &&
        (!_starRailGuideSwapchain || !_starRailGuideQueueCurrent))
        return RefuseDepthContract("starrail-conversion-not-on-presenting-queue",false);
    // Once commands/tags can be recorded, even an undispatched frame owns real
    // GPU/SDK state. A CPU failure cannot bypass the established teardown path.
    _managedRetained=true;_continuityOwner=true;_teardownBorrowedInputs=true;_guideContractOwned=true;
    // RF's allocation helper releases a mismatched destination immediately.
    // Detect the generation change before recording anything; the old real
    // owners must first pass Off/host completion/SDK free above.
    if (flip)
    {
        for (auto type : {FG_ResourceType::Depth,FG_ResourceType::Velocity})
        {
            const auto previous=_resourceCopy[slot].find(type);
            if (previous==_resourceCopy[slot].end() || !previous->second) continue;
            const auto old=previous->second->GetDesc();
            const auto& source=type==FG_ResourceType::Depth ? depthDesc : motionDesc;
            const auto expectedWidth=type==FG_ResourceType::Depth && scale ? scaleWidth : source.Width;
            const auto expectedHeight=type==FG_ResourceType::Depth && scale ? scaleHeight : source.Height;
            const auto expectedFormat=type==FG_ResourceType::Depth ? DXGI_FORMAT_R32_FLOAT : source.Format;
            if (old.Width!=expectedWidth || old.Height!=expectedHeight || old.Format!=expectedFormat ||
                old.Flags!=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                return RefuseDepthContract("guide-copy-generation-needs-drain");
        }
    }
    ID3D12Resource* actualDepth=depth;
    if (scale)
    {
        const auto amount=Config::Instance()->FGDepthScaleMax.value_or_default();
        if (!std::isfinite(amount) || amount<=0 || !feature ||
            width>(feature->LowResMV()?feature->RenderWidth():feature->DisplayWidth()) ||
            height>(feature->LowResMV()?feature->RenderHeight():feature->DisplayHeight()))
            return RefuseDepthContract("depth-scale-domain");
        auto& shader=_guideDepthScale[slot];
        if (shader && shader->Buffer())
        {
            const auto old=shader->Buffer()->GetDesc();
            if (old.Width!=scaleWidth || old.Height!=scaleHeight)
                return RefuseDepthContract("depth-scale-generation-needs-drain");
        }
        if (!shader) shader=std::make_unique<DS_Dx12>("FG owned depth scale",_device);
        // Preserve the rollback's display-sized allocation. A changing render
        // subrect only changes the tag; it must not rebuild the FG pool.
        if (!shader->CreateBufferResource(_device,depth,scaleWidth,scaleHeight,D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
            !shader->Buffer()) return RefuseDepthContract("depth-scale-allocation");
        shader->SetBufferState(list,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ResourceBarrier(list,depth,depthState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool recorded=shader->Dispatch(list,depth,shader->Buffer());
        ResourceBarrier(list,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,depthState);
        if (!recorded) return RefuseDepthContract("depth-scale-not-recorded");
        actualDepth=shader->Buffer();depthState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    const auto publish=[&](FG_ResourceType type,ID3D12Resource* resource,UINT x,UINT y,
        UINT w,UINT h,D3D12_RESOURCE_STATES state)
    {
        Dx12Resource tag{};tag.type=type;tag.resource=resource;tag.cmdList=list;
        tag.left=x;tag.top=y;tag.width=w;tag.height=h;tag.state=state;
        // Owned StarRail outputs preserve the rollback's UntilPresent tag.
        // Their writes are ordered on the real presenting queue; raw/cache
        // inputs keep the rollback's existing eOnlyValidNow SDK snapshot.
        tag.validity=type==FG_ResourceType::Depth && scale &&
            dlssg::depth_contract::Game()==GameRouting::Identity::StarRail ?
            FG_ResourceValidity::UntilPresent : FG_ResourceValidity::ValidNow;
        const auto* previous=_upscalerGuidePublication;
        _upscalerGuidePublication=&tag;
        struct EndPublication { const Dx12Resource*& slot; const Dx12Resource* previous;
            ~EndPublication(){slot=previous;} } finish{_upscalerGuidePublication,previous};
        return SetResource(&tag);
    };
    if (!publish(FG_ResourceType::Velocity,motion,mvLeft,mvTop,mvWidth,mvHeight,motionState) ||
        !publish(FG_ResourceType::Depth,actualDepth,left,top,width,height,depthState))
    {
        if (CanSubmitInputs()) RefuseDepthContract("guide-pair-partial-publication");
        return false;
    }
    _depthSourceRejected=false; // Only a complete real pair can recover admission.
    return true;
}

bool DLSSG_Dx12::NativeGuideRetired(const NativeGuideUse& use) const
{
    if (!use.list) return true;
    if (_nativeGuideFailed || !use.closed) return false;
    if (!use.submitted) return true; // Successful Reset discarded this unexecuted recording.
    if (!use.signalled || !use.fence || !use.value || use.value==UINT64_MAX) return false;
    const auto done=use.fence->GetCompletedValue();
    return done!=UINT64_MAX && done>=use.value;
}

bool DLSSG_Dx12::NativeGuidesRetired() const
{
    for (const auto& use:_nativeGuideUses) if (!NativeGuideRetired(use)) return false;
    return true;
}

void DLSSG_Dx12::NativeGuidesSubmitting(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!TracksNativeGuides() || !lists) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownQuarantined) return;
    for (UINT i=0;i<count;++i) for (auto& use:_nativeGuideUses)
    {
        if (!use.list || use.closed ||
            (use.list.Get()!=lists[i] && use.nativeList.Get()!=lists[i])) continue;
        if (use.submitted)
        {
            _nativeGuideFailed=true;
            if (queue) queue->AddRef(); // A replay on a second queue retains both timelines.
        }
        else
        {
            // Reserve before Execute: a concurrent successful Reset cannot
            // mistake an escaped recording for an unsubmitted discard.
            use.queue=queue; use.submitted=true; use.executing=true;
        }
    }
}

void DLSSG_Dx12::NativeGuidesSubmitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!TracksNativeGuides() || !lists) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownQuarantined) return;
    if (_nativeGuideFailed) { _managedRetained=true; Deactivate(); return; }
    for (UINT i=0;i<count;++i) for (auto& use:_nativeGuideUses)
    {
        if (!use.list || !use.executing ||
            (use.list.Get()!=lists[i] && use.nativeList.Get()!=lists[i])) continue;
        // Called after the actual Execute, even if another thread Reset already
        // closed the recording. Signal is the only GPU completion authority.
        if (_nativeGuideFailed || !queue || use.queue.Get()!=queue ||
            queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !dlssg::depth_contract::OnDevice(_device,queue) || !use.fence || !use.value ||
            use.value==UINT64_MAX || FAILED(queue->Signal(use.fence.Get(),use.value)))
        {
            _nativeGuideFailed=true;
            _managedRetained=true;
            Deactivate(); // Existing drain cannot release this poisoned native graph.
            return;
        }
        use.executing=false; use.signalled=true;
        if (++_nativeGuideSubmissions<=3 || _nativeGuideSubmissions%600==0)
            LOG_INFO("R1058 native FG producer submitted: pid={} frame={} list={} native_list={} producer_queue={} fence={} target={} sr_succeeded={} SDK_snapshot=ValidNow generated=UNKNOWN",
                GetCurrentProcessId(),use.frame,(uintptr_t)use.list.Get(),(uintptr_t)use.nativeList.Get(),
                (uintptr_t)queue,(uintptr_t)use.fence.Get(),use.value,use.srSucceeded);
    }
}

void DLSSG_Dx12::NativeGuidesReset(ID3D12CommandList* list)
{
    if (!TracksNativeGuides() || !list) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    for (auto& use:_nativeGuideUses)
        if ((use.list.Get()==list || use.nativeList.Get()==list) && !use.closed) use.closed=true;
    // Reset closes recording replay authority; it is NOT GPU completion.
}

UINT64 DLSSG_Dx12::NativeGuideSource(ID3D12CommandList* list)
{
    if (!TracksNativeGuides()) return 0;
    std::lock_guard continuityLock(_inputContinuityMutex);
    const auto& use=_nativeGuideUses[GetIndex()];
    return use.list.Get()==list && use.frame==_frameCount && !use.closed && !use.submitted &&
        use.epoch==_inputOwnerEpoch && use.generation==_inputContinuity.SourceGeneration() ? use.serial : 0;
}

void DLSSG_Dx12::CompleteNativeGuideSource(ID3D12CommandList* list, UINT64 serial, bool succeeded)
{
    if (!serial || !TracksNativeGuides()) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    auto& use=_nativeGuideUses[GetIndex()];
    if (use.list.Get()!=list || use.serial!=serial || use.frame!=_frameCount || use.closed || use.submitted ||
        use.epoch!=_inputOwnerEpoch || use.generation!=_inputContinuity.SourceGeneration()) return;
    use.srSucceeded=succeeded;
    if (!succeeded) RefuseDepthContract("native-selected-sr-failed",false);
}

bool DLSSG_Dx12::OrderNativeGuides(UINT slot)
{
    auto& use=_nativeGuideUses[slot];
    if (!use.list || use.frame!=_frameCount) return true; // Existing wDX12 UI submission path.
    if (_nativeGuideFailed) return false;
    if ((use.fence && use.fence->GetCompletedValue()==UINT64_MAX) ||
        (_device && _device->GetDeviceRemovedReason()!=S_OK))
    {
        _nativeGuideFailed=true; _managedRetained=true; Deactivate();
        return false;
    }
    if (!use.srSucceeded || !use.submitted || !use.signalled ||
        !use.queue || !use.fence || !_gameCommandQueue || !use.value || use.value==UINT64_MAX) return false;
    if (use.ordered) return true;
    // Same-queue FIFO already orders the producer and SDK snapshots. A different
    // actual producer queue needs its real post-Execute fence, never the UI fence.
    const auto presentingQueue=dlssg::depth_contract::NativeObject(_gameCommandQueue);
    if (!dlssg::depth_contract::SameOwner(use.queue.Get(),presentingQueue.Get()) &&
        FAILED(_gameCommandQueue->Wait(use.fence.Get(),use.value)))
    {
        _nativeGuideFailed=true; _managedRetained=true; Deactivate();
        return false;
    }
    use.ordered=true;
    return true;
}

bool DLSSG_Dx12::GuideTagsCurrent(int slot, UINT64 frame)
{
    if (!OwnsDepthContract()) return false;
    const auto owner=CurrentInputOwner();
    if (slot<0 || slot>=BUFFER_COUNT || frame!=_frameCount ||
        !_inputContinuity.Current(slot,frame,owner)) return false;
    const auto& native=_nativeGuideUses[slot];
    if (native.list && native.frame==frame &&
        (_nativeGuideFailed || !native.srSucceeded || !native.submitted || !native.signalled || !native.ordered))
        return false;
    std::shared_lock resources(_resourceMutex[slot]);
    for (auto type : {FG_ResourceType::Depth,FG_ResourceType::Velocity})
    {
        const auto& tag=_guideTags[slot][type==FG_ResourceType::Depth ? 0 : 1];
        const auto found=_frameResources[slot].find(type);
        if (!tag.accepted || tag.frame!=frame ||
            tag.generation!=_inputContinuity.SourceGeneration() || !(tag.owner==owner) ||
            found==_frameResources[slot].end()) return false;
        const auto& a=found->second; const auto& b=tag.input;
        // No borrowed-resource dereference. eOnlyValidNow requests the SDK's
        // source-lifetime contract, not proof of a GPU copy. UntilPresent is
        // a sealed Genshin lease or a StarRail-owned presenting-queue output.
        if (a.resource!=b.resource || a.copy!=b.copy || a.cmdList!=b.cmdList ||
            a.state!=b.state || a.validity!=b.validity || a.left!=b.left || a.top!=b.top ||
            a.width!=b.width || a.height!=b.height ||
            !dlssg::depth_contract::Region(tag.description,a.left,a.top,a.width,a.height)) return false;
        if (a.validity==FG_ResourceValidity::ValidNow && !a.cmdList) return false;
        if (a.cmdList && a.cmdList==_uiCommandList[slot] &&
            (_uiCommandListResetted[slot] || _uiCloseFailed[slot] || !_uiFence ||
             !_uiAllocatorFenceValues[slot] || _uiFence->GetCompletedValue()==UINT64_MAX)) return false;
        if (a.validity!=FG_ResourceValidity::ValidNow && !IsGenshinBridgeOwner())
        {
            if (dlssg::depth_contract::Game()!=GameRouting::Identity::StarRail ||
                !_starRailGuideQueueCurrent || a.cmdList!=_uiCommandList[slot]) return false;
            const auto* scaled=_guideDepthScale[slot].get();
            const bool ownedCopy=a.copy && _resourceCopy[slot].contains(type) &&
                _resourceCopy[slot].at(type)==a.copy;
            const bool ownedScale=type==FG_ResourceType::Depth && scaled &&
                _guideDepthScale[slot]->Buffer()==a.resource;
            if (!ownedCopy && !ownedScale) return false;
        }
    }
    const auto& depth=_guideTags[slot][0].input;
    const auto& motion=_guideTags[slot][1].input;
    return (depth.copy ? depth.copy : depth.resource)!=(motion.copy ? motion.copy : motion.resource);
}

bool DLSSG_Dx12::Dispatch()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    LOG_FUNC();

    if (!CanSubmitInputs()) return false;
    const bool depthContractFrame=OwnsDepthContract();
    if (depthContractFrame && _depthSourceRejected) return false;
    const bool resumingInputGap = _inputContinuity.Held();
    const bool starRailFrame = IsStarRailContinuousOwner();
    const bool restartStarRailRuntime = starRailFrame && _starRailGapOffAccepted;
    const auto sourceGeneration = _inputContinuity.SourceGeneration();
    const bool resetStarRailHistory = starRailFrame && (resumingInputGap ||
        restartStarRailRuntime || _starRailDispatchGeneration != sourceGeneration);
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
    else if (IsGenshinBridgeOwner() || IsStarRailUiOwner() || OwnsDepthContract())
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

    const bool genshinFrame = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
        State::Instance().activeFgInput == FGInput::Upscaler &&
        State::Instance().activeFgOutput == FGOutput::DLSSG;
    if (genshinFrame && (_genshinConstantsFrame[fIndex] != willDispatchFrame ||
                        !HasResource(FG_ResourceType::PresentColor, fIndex))) return false;
    if (genshinFrame)
    {
        const auto& lease = _genshinInputs[fIndex];
        if (!lease || lease->frame != willDispatchFrame || lease->epoch != _inputOwnerEpoch ||
            lease->owner != _genshinOwnerGeneration || !GenshinGuidesCurrent(lease->guides)) return false;
        // Recheck the actual resources consumed at Dispatch, not only the API
        // arguments seen at tagging. Generic aliases/copies cannot inherit policy.
        std::shared_lock resources(_resourceMutex[fIndex]);
        for (auto type : {FG_ResourceType::Depth, FG_ResourceType::Velocity})
        {
            const auto found = _frameResources[fIndex].find(type);
            auto* expected = type == FG_ResourceType::Depth ? lease->guides->depth.Get() : lease->guides->motion.Get();
            if (found == _frameResources[fIndex].end() || found->second.copy ||
                !SameDeviceOwner(_device, found->second.GetResource()) ||
                !SameGenshinComOwner(found->second.GetResource(), expected) ||
                found->second.left || found->second.top ||
                found->second.width!=lease->guides->key.width ||
                found->second.height!=lease->guides->key.height ||
                found->second.state!=D3D12_RESOURCE_STATE_COMMON ||
                found->second.validity!=FG_ResourceValidity::UntilPresent) return false;
        }
        const auto color=_frameResources[fIndex].find(FG_ResourceType::PresentColor);
        if (!lease->color || color==_frameResources[fIndex].end() || color->second.copy ||
            !SameGenshinComOwner(color->second.resource,lease->color.Get()) ||
            color->second.left || color->second.top ||
            color->second.width!=lease->guides->key.width || color->second.height!=lease->guides->key.height ||
            color->second.state!=D3D12_RESOURCE_STATE_PRESENT ||
            color->second.validity!=FG_ResourceValidity::UntilPresent) return false;
    }

    if (!genshinFrame || DetailedGenshinFrameLog(willDispatchFrame))
        LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    auto& state = State::Instance();

    // Preserve the saved request across temporary provider limits. Only the
    // value submitted to this provider is bounded by its current capability.
    if (_maxInterpolationCount < 1)
        return false;
    const int requestedInterpolationCount = Config::Instance()->FGDLSSGInterpolationCount.value_or_default();
    const int appliedInterpolationCount = (std::clamp)(requestedInterpolationCount, 1,
        static_cast<int>(_maxInterpolationCount));
    if (_framesToInterpolate != appliedInterpolationCount)
    {
        LOG_INFO("Interpolation count changed {} -> {} (saved request {}, provider max {})", _framesToInterpolate,
                 appliedInterpolationCount, requestedInterpolationCount, _maxInterpolationCount);
        _framesToInterpolate = appliedInterpolationCount;
    }

    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = _framesToInterpolate;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

    if (Config::Instance()->FGDLSSGForceDMFG.value_or_default())
    {
        options.mode = sl::DLSSGMode::eDynamic;
        options.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default();
    }

    if (CanRetainForInputGap() || _genshinConstantsFrame[fIndex] == willDispatchFrame)
        options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    StreamlineHooks::applyMenuDlssgInterlock(options, true);
    PrepareStarRailOptionsCache(fIndex);
    const auto setAndObserveOptions = [&]() {
        // Any enable attempt can supersede an old Off, even if it reenters a
        // new clock before returning. Final SDK admission must suppress again.
        if (starRailFrame) _starRailGapOffAccepted = false;
        // Save the actual attempted mode/flags before the vendor call: even a
        // rejected call can leave state unknown and needs this Off recipe.
        if (genshinFrame) _lastRunningOptions = options;
        const auto optionOwnerEpoch = _inputOwnerEpoch;
        const auto optionPresentEpoch = _genshinPresentEpoch;
        if (genshinFrame) _genshinBudgetRefusalPresent = 0;
        bool optionsSubmitted = false;
        const auto result = SetStarRailDispatchOptions(_starRailDlssgOptions, options,
            willDispatchFrame, resumingInputGap && (!starRailFrame || restartStarRailRuntime),
            [&]() { return StreamlineProxy::DLSSGSetOptions()(viewport, options); }, &optionsSubmitted);
        // Only this exact failed On/options transaction may suspend the child.
        // Off, GetState warnings and recursive owner changes cannot sign it.
        if (genshinFrame && optionsSubmitted && options.mode != sl::DLSSGMode::eOff &&
            result == sl::Result::eWarnOutOfVRAM &&
            State::Instance().currentFG == this && optionOwnerEpoch == _inputOwnerEpoch &&
            optionPresentEpoch && optionPresentEpoch == _genshinPresentEpoch && willDispatchFrame == _frameCount)
        {
            _genshinBudgetRefusalPresent = optionPresentEpoch;
            _genshinBudgetRefusalFrame = willDispatchFrame;
            _genshinBudgetRefusalResumeRequest = GenshinResumeRequests();
        }
        if (genshinFrame && result == sl::Result::eOk)
        {
            _teardownOffAccepted = false; // a previous Off is no longer authority
            _haveRunningOptions = options.mode != sl::DLSSGMode::eOff;
            auto lease = _genshinInputs[fIndex];
            if (lease && lease->owner == _genshinOwnerGeneration && lease->frame == willDispatchFrame)
            {
                lease->acceptedBlocking = _haveRunningOptions &&
                    options.queueParallelismMode == sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
                lease->inputOwner = CurrentInputOwner();
            }
        }
        if (optionsSubmitted)
            MfgUnlock::RecordSetOptions(_framesToInterpolate, options.numFramesToGenerate,
                                       options.mode != sl::DLSSGMode::eOff, (unsigned int) result);
        // GetState consumes numFramesActuallyPresented. Genshin observes it
        // only in the existing EndGenshinPresent query, after original Present.
        // Pre-Present telemetry explicitly describes an older observation.
        sl::DLSSGState actualState {};
        auto stateResult = sl::Result::eOk;
        if (!genshinFrame)
        {
            stateResult = StreamlineProxy::DLSSGGetState()(viewport, actualState, nullptr);
            if (stateResult == sl::Result::eOk) MfgUnlock::RecordState(actualState.numFramesActuallyPresented);
        }
        // Source metadata is read under Dispatch's existing continuity lock.
        // Reuse the one vendor result/query above; presented is a consumed,
        // previous observation, not this source frame's generated output.
        if (GenshinHookCpuTiming::Enabled() && [] {
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
        static unsigned int telemetryInterval = 0;
        if (++telemetryInterval % 300 == 1)
        {
            if (genshinFrame)
                LOG_INFO("R827 FG options: pid={} owner={} epoch={} frame={} slot={} thread={} requested={} sent={} mode={} flags={} options_result={} previous_observation_valid={} previous_frame={} previous_epoch={} previous_result={} previous_cap={} previous_presented={} previous_status={} generated=UNKNOWN observation_is_not_current=true",
                    GetCurrentProcessId(),_genshinOwnerGeneration,_inputOwnerEpoch,willDispatchFrame,fIndex,GetCurrentThreadId(),
                    _framesToInterpolate,options.numFramesToGenerate,(int)options.mode,(unsigned)options.flags,(int)result,
                    _genshinStateObservation.valid,_genshinStateObservation.frame,_genshinStateObservation.epoch,
                    _genshinStateObservation.result,_genshinStateObservation.cap,_genshinStateObservation.presented,_genshinStateObservation.status);
            else
                LOG_INFO("F5 actual MFG: requested {}, sent {}, optionsResult {}, optionsSubmitted {}, stateResult {}, providerCap {}, presented {}, status {}; generated UNKNOWN",
                    _framesToInterpolate, options.numFramesToGenerate, (int) result, optionsSubmitted, (int) stateResult,
                    actualState.numFramesToGenerateMax, actualState.numFramesActuallyPresented, (int) actualState.status);
        }
        return result;
    };
    const auto setReflexOptions = [&]() {
        sl::ReflexOptions reflexConst {};
        reflexConst.mode = native::lowlatency::WantsBoostForFrameGeneration() ? sl::ReflexMode::eLowLatencyWithBoost
                                                                               : sl::ReflexMode::eLowLatency;
        reflexConst.useMarkersToOptimize = ReflexHooks::gameIsSendingMarkers();
        _genshinReflexOwnership = genshinFrame ? GenshinReflexOwnership::Running : GenshinReflexOwnership::None;
        _genshinReflexOffReported = false;
        const auto result = SetStarRailDispatchOptions(_starRailReflexOptions, reflexConst,
            willDispatchFrame, resumingInputGap && (!starRailFrame || restartStarRailRuntime),
            [&]() { return StreamlineProxy::ReflexSetOptions()(reflexConst); });
        if (result != sl::Result::eOk)
            LOG_ERROR("Couldn't set Reflex options, error: {}", magic_enum::enum_name(result));
        return result;
    };
    // F5-DS submits DLSSG options before Reflex on real guide Dispatches.
    sl::Result dispatchOptionsResult = sl::Result::eOk;
    if (!resumingInputGap && (dispatchOptionsResult = setAndObserveOptions()) != sl::Result::eOk)
    {
        LOG_ERROR("Couldn't set DLSSG options, error: {}", magic_enum::enum_name(dispatchOptionsResult));
        if (genshinFrame || CanRetainForInputGap())
        {
            Deactivate();
            UpdateTarget();
            return false;
        }
    }
    if (genshinFrame && (!_genshinInputs[fIndex] || !_genshinInputs[fIndex]->acceptedBlocking))
    {
        // Includes a menu-interlocked Off: configuration was not an FG submit.
        Deactivate();
        return false;
    }

    const auto reflexResult = setReflexOptions();
    if (starRailFrame && (reflexResult != sl::Result::eOk || !sourceStillCurrent())) return false;

    if (!_haveHudless.has_value())
    {
        _haveHudless = IsUsingHudless(fIndex);
    }

    if (!_noHudless[fIndex])
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

    if (IsInfiniteDepth() && _cameraFar[fIndex] > _cameraNear[fIndex])
        _cameraFar[fIndex] = std::numeric_limits<float>::infinity();
    else if (IsInfiniteDepth() && _cameraNear[fIndex] > _cameraFar[fIndex])
        _cameraNear[fIndex] = std::numeric_limits<float>::infinity();

    if (!genshinFrame)
    {
        if (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f || _cameraPosition[fIndex][2] != 0.0f)
        {
            constData.cameraPos.x = _cameraPosition[fIndex][0];
            constData.cameraPos.y = _cameraPosition[fIndex][1];
            constData.cameraPos.z = _cameraPosition[fIndex][2];

            constData.cameraUp.x = _cameraUp[fIndex][0];
            constData.cameraUp.y = _cameraUp[fIndex][1];
            constData.cameraUp.z = _cameraUp[fIndex][2];

            constData.cameraFwd.x = _cameraForward[fIndex][0];
            constData.cameraFwd.y = _cameraForward[fIndex][1];
            constData.cameraFwd.z = _cameraForward[fIndex][2];

            constData.cameraRight.x = _cameraRight[fIndex][0];
            constData.cameraRight.y = _cameraRight[fIndex][1];
            constData.cameraRight.z = _cameraRight[fIndex][2];
        }
        else
        {
            constData.cameraPos = { 0.0f, 0.0f, 0.0f };
            constData.cameraUp = { 0.0f, 0.0f, 1.0f };
            constData.cameraRight = { 0.0f, 1.0f, 0.0f };
            constData.cameraFwd = { 1.0f, 0.0f, 0.0f };
            constData.cameraPinholeOffset = { 0.0f, 0.0f };

            XMMATRIX cameraViewToClip {};

            // XMMatrixPerspectiveFovRH will fail if input values are incorrect
            if (_cameraNear[fIndex] > 0.f && _cameraFar[fIndex] > 0.f &&
                !XMScalarNearEqual(_cameraVFov[fIndex], 0.0f, 0.00001f) &&
                !XMScalarNearEqual(_cameraAspectRatio[fIndex], 0.0f, 0.00001f))
            {
                if (XMScalarNearEqual(_cameraNear[fIndex], _cameraFar[fIndex], 0.00001f))
                    _cameraFar[fIndex]++;

                cameraViewToClip = XMMatrixPerspectiveFovRH(_cameraVFov[fIndex], _cameraAspectRatio[fIndex],
                                                            _cameraNear[fIndex], _cameraFar[fIndex]);
            }
            else
            {
                LOG_WARN("Can't calculate projectionMatrix");
            }

            XMMATRIX clipToCameraView = XMMatrixInverse(nullptr, cameraViewToClip);

            auto prev = XMMatrixIdentity();

            // Convert to sl::float4x4 for Streamline
            XMFLOAT4X4 temp;
            XMStoreFloat4x4(&temp, cameraViewToClip);
            memcpy(&constData.cameraViewToClip, &temp, sizeof(sl::float4x4));
            XMStoreFloat4x4(&temp, clipToCameraView);
            memcpy(&constData.clipToCameraView, &temp, sizeof(sl::float4x4));

            XMStoreFloat4x4(&temp, prev);
            memcpy(&constData.clipToLensClip, &temp, sizeof(sl::float4x4));
            memcpy(&constData.clipToPrevClip, &temp, sizeof(sl::float4x4));
            memcpy(&constData.prevClipToClip, &temp, sizeof(sl::float4x4));
        }

    }

    constData.cameraAspectRatio = _cameraAspectRatio[fIndex];
    constData.cameraFOV = _cameraVFov[fIndex];
    constData.cameraNear = _cameraNear[fIndex];
    constData.cameraFar = _cameraFar[fIndex];

    constData.jitterOffset.x = _jitterX[fIndex];
    constData.jitterOffset.y = _jitterY[fIndex];

    {
        auto mv = GetResource(FG_ResourceType::Velocity, fIndex);

        if (!mv)
        {
            LOG_ERROR("Motion vectors missing for: {}", fIndex);

            return false;
        }

        constData.mvecScale.x = _mvScaleX[fIndex] / (float) mv->width;
        constData.mvecScale.y = _mvScaleY[fIndex] / (float) mv->height;
    }

    // LOG_DEBUG("MvRes: {}x{}, Games MvScale : {}x{}, SL MvScale: {}x{}", mv->width, mv->height, _mvScaleX[fIndex],
    //           _mvScaleY[fIndex], constData.mvecScale.x, constData.mvecScale.y);

    // if (State::Instance().currentFeature != nullptr)
    //{
    //     auto fResX = State::Instance().currentFeature->RenderWidth();
    //     auto fResY = State::Instance().currentFeature->RenderHeight();

    //    LOG_DEBUG("Feature LowResMV: {} RenderRes : {}x{}, CMvScale: {}x{}",
    //              State::Instance().currentFeature->LowResMV(), fResX, fResY, 1.0f / (float) fResX,
    //              1.0f / (float) fResY);
    //}

    if (!Config::Instance()->FGSkipReset.value_or_default())
        constData.reset = _reset[fIndex] != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    else
        constData.reset = sl::Boolean::eFalse;

    if (_depthSourceGap || (resumingInputGap && (!starRailFrame || resetStarRailHistory)))
        constData.reset = sl::Boolean::eTrue; // Real gap/source-generation reset, never each transient omission.

    constData.depthInverted = IsInvertedDepth() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constData.cameraMotionIncluded = sl::Boolean::eTrue;
    constData.motionVectors3D = sl::Boolean::eFalse;
    // constData.motionVectorsInvalidValue = 0.0f;
    constData.orthographicProjection = sl::Boolean::eFalse;
    constData.motionVectorsDilated = IsLowResMV() ? sl::Boolean::eFalse : sl::Boolean::eTrue;
    constData.motionVectorsJittered = IsJitteredMVs() ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    // Never synthesize Genshin projection or previous-frame transforms from
    // requested near/FOV/default axes. Preserve the actual observed matrices.
    if (genshinFrame) constData = _genshinConstants[fIndex];

    auto frameId = static_cast<uint32_t>(willDispatchFrame);

    sl::FrameToken* frameToken = nullptr;
    auto tokenResult = StreamlineProxy::GetNewFrameToken()(frameToken, &frameId);
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
    auto result = StreamlineProxy::SetConstants()(constData, *frameToken, viewport);
    if (genshinFrame)
    {
        const auto now=GetTickCount64();
        const int reset=static_cast<int>(constData.reset);
        const bool changed=reset!=_genshinConstantsTraceReset;
        const bool initial=_genshinConstantsTraceCalls++<32;
        const bool firstFailure=result!=sl::Result::eOk && !_genshinConstantsFailureSeen;
        if (result!=sl::Result::eOk) _genshinConstantsFailureSeen=true;
        if (initial || firstFailure || (changed && _genshinConstantsTraceTransitions<128) ||
            now-_genshinConstantsTraceTick>=5000)
        {
            if (changed) ++_genshinConstantsTraceTransitions;
            _genshinConstantsTraceReset=reset;_genshinConstantsTraceTick=now;
            LOG_INFO("R827 FG constants: pid={} owner={} epoch={} frame={} slot={} thread={} token={} token_result={} constants_result={} reset={} source_reset={} gap_resume={} pinhole_x={} pinhole_y={} generated=UNKNOWN no_authority=true",
                GetCurrentProcessId(),_genshinOwnerGeneration,_inputOwnerEpoch,willDispatchFrame,fIndex,GetCurrentThreadId(),
                reinterpret_cast<uintptr_t>(frameToken),(int)tokenResult,(int)result,reset,_reset[fIndex],resumingInputGap,
                constData.cameraPinholeOffset.x,constData.cameraPinholeOffset.y);
        }
    }
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
    if (depthContractFrame && !GuideTagsCurrent(fIndex,willDispatchFrame))
        return RefuseDepthContract("guide-pair-changed-after-constants");
    if (CanRetainForInputGap()) _managedRetained = true;
    if (_managedRetained) _continuityOwner = true;
    _runningInputOwner = CurrentInputOwner();
    _lastRunningOptions = options;
    _haveRunningOptions = options.mode != sl::DLSSGMode::eOff;
    _depthSourceGap=false;
    if (genshinFrame || IsStarRailUiOwner() || OwnsDepthContract())
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
    if (!genshinFrame || DetailedGenshinFrameLog(willDispatchFrame)) LOG_DEBUG("Result: Ok");

    return true;
}

void* DLSSG_Dx12::FrameGenerationContext() { return (void*) 0x13371337; }

void* DLSSG_Dx12::SwapchainContext() { return (void*) 0x23372337; }

DLSSG_Dx12::~DLSSG_Dx12()
{
    framegen::final_output::RetireProvider(_finalOutputProvider);
    if (!Shutdown()) QuarantineFailedTeardown();
}

bool DLSSG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount) { return true; }

void DLSSG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);
    std::lock_guard continuityLock(_inputContinuityMutex);

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
    const bool resumePending=_genshinResumePending.exchange(false,std::memory_order_acq_rel);

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
            if(resumePending) ResetCounters();
            else UpdateTarget();
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
        if(resumePending) ResetCounters();
        else UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    state.scChanged = false;
}

void DLSSG_Dx12::RequestGenshinResume()
{
    // Menu rendering can already be inside the FG Present mutex. Defer the
    // actual scalar transition to EvaluateState's established lock ordering.
    const auto& state=State::Instance();
    if(state.isShuttingDown || !Config::Instance()->FGEnabled.value_or_default() ||
       !GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) || state.currentFG!=this ||
       state.activeFgInput!=FGInput::Upscaler || state.activeFgOutput!=FGOutput::DLSSG) return;
    auto request = _genshinResumeRequests.load(std::memory_order_relaxed);
    while (request != UINT64_MAX && !_genshinResumeRequests.compare_exchange_weak(
        request, request + 1, std::memory_order_release, std::memory_order_relaxed)) {}
    _genshinResumePending.store(true,std::memory_order_release);
    LOG_INFO("R15 FG resume queued no_authority=true");
}

bool DLSSG_Dx12::GenshinBudgetRefusedAt(UINT64 presentEpoch, UINT64& resumeRequest)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!IsGenshinBridgeOwner() || _hookRetiring || _ownerChanging || _teardownQuarantined ||
        g_dlssgTeardownQuarantined.load() || !presentEpoch ||
        _genshinBudgetRefusalPresent != presentEpoch || _genshinBudgetRefusalFrame != _frameCount)
        return false;
    resumeRequest = _genshinBudgetRefusalResumeRequest;
    return resumeRequest != UINT64_MAX;
}

void DLSSG_Dx12::ReleaseObjects() { (void)ReleaseObjectsChecked(); }

bool DLSSG_Dx12::ReleaseObjectsChecked()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    InvalidateStarRailOptions();
    if (_teardownCompleting) return false;
    if (_managedRetained || _inputContinuity.Held()) Deactivate();
    if (!CompletePendingTeardown()) return false;
    if (_zzzSCCompletion && !PollZzzSCWork()) return false;
    // Outstanding frame receipts keep their own actual owners, including a
    // failed pre-provider Wait/Signal. Cache release is never retirement proof.
    _genshinWaitFence.reset();
    _genshinHostFence.reset();
    SAFE_RELEASE(_teardownFence);
    if (_teardownEvent) { CloseHandle(_teardownEvent); _teardownEvent = nullptr; }
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);
        SAFE_RELEASE(dlssgFence[i]);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }

    _renderUI.reset();
    _hudlessCompare.reset();
    _mvFlip.reset();
    _depthFlip.reset();
    return true;
}

void DLSSG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_hookRetiring || _teardownCompleting || _teardownPending || _teardownQuarantined ||
        g_dlssgTeardownQuarantined.load()) return;
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    InvalidateStarRailOptions();
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
        state.currentFG == this && SameGenshinComOwner(state.currentFGSwapchain, _swapChain);
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
            !SameGenshinComOwner(work.device.Get(), _device))
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
        !SameGenshinComOwner(queueDevice.Get(), _device) || !SameGenshinComOwner(fenceDevice.Get(), _device))
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
        work.fence.Get() != _scFence || !SameGenshinComOwner(work.device.Get(), _device) ||
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
    const bool genshinFrame = IsGenshinBridgeOwner();
    UINT64 selectedStarRailFrame = 0;
    auto fIndex = IsStarRailF5DsOwner() && !inputGap ? SelectStarRailF5DsFrame(selectedStarRailFrame) :
        (inputGap || genshinFrame || IsStarRailUiOwner() || OwnsDepthContract()) ? GetIndex() : GetIndexWillBeDispatched();
    if (fIndex < 0) return false;
    if (!OrderNativeGuides(static_cast<UINT>(fIndex)))
        return CanSubmitInputs() ? RefuseDepthContract("native-source-not-submitted",false) : false;
    if (!genshinFrame || DetailedGenshinFrameLog(_frameCount)) LOG_DEBUG("fIndex: {}", fIndex);

    const auto failSubmission = [&]() {
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
        if (IsStarRailUiOwner())
        {
            if (!SubmitUICommandList(static_cast<UINT>(fIndex))) return false;
        }
        else if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();

            if (closeResult != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);
                _uiCloseFailed[fIndex] = true; // Not submitted; never re-Close/replay this list.
                return failSubmission();
            }
            _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
            _uiCommandListResetted[fIndex] = false; // Submitted even when Signal fails.
            const auto signalResult = _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);
            if (FAILED(signalResult))
            {
                LOG_ERROR("UI allocator fence Signal failed. slot {}, HRESULT {:X}", fIndex, (UINT) signalResult);
                return failSubmission();
            }
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

    if (inputGap) return Dispatch();

    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        if (SuspendForInputGap()) return false;
        LOG_DEBUG("Pausing FG");
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;
    if (IsStarRailF5DsOwner()) _starRailPresentCounted = true;

    return Dispatch();
}

static bool SameGenshinComOwner(IUnknown* left, IUnknown* right)
{
    if (!left || !right) return false;
    Microsoft::WRL::ComPtr<IUnknown> a, b;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() && b.Get() && a.Get() == b.Get();
}

// Mutable counters are touched only under the provider continuity lock. The
// actual fence/device/queue and epoch association never change after publishing
// any receipt. A replacement cache cannot release or relabel an old receipt.
struct DLSSG_Dx12::GenshinFenceOwner final
{
    const UINT64 providerOwner, inputEpoch, fenceEpoch;
    const uintptr_t swapchainIdentity;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 reservedValue=0, lastSignalValue=0;
    bool failed=false;
    GenshinFenceOwner(UINT64 provider, UINT64 input, UINT64 epoch, uintptr_t swapchain)
        : providerOwner(provider), inputEpoch(input), fenceEpoch(epoch), swapchainIdentity(swapchain) {}
};

UINT64 DLSSG_Dx12::GenshinFenceReceipt::Epoch() const noexcept {return timeline->fenceEpoch;}

bool DLSSG_Dx12::GenshinFenceReceipt::Completed() const noexcept
{
    if (!signalled.load(std::memory_order_acquire) || !value || value == UINT64_MAX ||
        !timeline || !timeline->fence || !timeline->device ||
        FAILED(timeline->device->GetDeviceRemovedReason())) return false;
    const auto completed = timeline->fence->GetCompletedValue();
    // A later failed submission does not revoke an earlier positive Signal;
    // nor can a later value stand in for THIS receipt's missing Signal.
    return completed != UINT64_MAX && completed >= value;
}

std::shared_ptr<DLSSG_Dx12::GenshinFenceReceipt>
DLSSG_Dx12::ReserveGenshinFence(std::shared_ptr<GenshinFenceOwner>& owner)
{
    // Caller holds continuity through reservation and the real queue operation.
    if (!_device || !_gameCommandQueue || !_genshinCreatedSwapchain ||
        !_inputOwnerEpoch || _inputOwnerEpoch == UINT64_MAX ||
        !_genshinOwnerGeneration || _genshinOwnerGeneration == UINT64_MAX ||
        FAILED(_device->GetDeviceRemovedReason())) return {};
    const bool current = owner && !owner->failed &&
        owner->providerOwner == _genshinOwnerGeneration && owner->inputEpoch == _inputOwnerEpoch &&
        owner->swapchainIdentity == _genshinCreatedSwapchain && owner->reservedValue < UINT64_MAX - 1 &&
        SameGenshinComOwner(owner->device.Get(), _device) &&
        SameGenshinComOwner(owner->queue.Get(), _gameCommandQueue);
    try
    {
        if (!current)
        {
            if (_genshinFenceSerial >= UINT64_MAX - 1) return {};
            Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
            if (_gameCommandQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
                FAILED(_gameCommandQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
                !SameGenshinComOwner(queueDevice.Get(), _device)) return {};
            auto next = std::make_shared<GenshinFenceOwner>(_genshinOwnerGeneration,
                _inputOwnerEpoch, _genshinFenceSerial + 1, _genshinCreatedSwapchain);
            next->device = _device; next->queue = _gameCommandQueue;
            if (FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&next->fence)))) return {};
            ++_genshinFenceSerial;
            owner = std::move(next); // old receipts retain the old actual owner
        }
        const auto value = owner->reservedValue + 1; // UINT64_MAX is never issued
        auto receipt = std::shared_ptr<GenshinFenceReceipt>(new GenshinFenceReceipt(owner, value));
        owner->reservedValue = value;
        return receipt;
    }
    catch (const std::bad_alloc&) {return {};}
}

HRESULT DLSSG_Dx12::SignalGenshinFence(const std::shared_ptr<GenshinFenceReceipt>& receipt)
{
    // Caller owns continuity; never allow a fence rewind when a pre-reserved
    // frame arrives after a newer frame's Signal. Missing values stay unknown.
    if (!receipt || !receipt->timeline || receipt->signalled.load(std::memory_order_acquire)) return E_INVALIDARG;
    auto& owner = *receipt->timeline;
    if (owner.failed || !receipt->value || receipt->value == UINT64_MAX ||
        receipt->value <= owner.lastSignalValue || receipt->value > owner.reservedValue ||
        owner.providerOwner != _genshinOwnerGeneration || owner.inputEpoch != _inputOwnerEpoch ||
        owner.swapchainIdentity != _genshinCreatedSwapchain || !owner.fence ||
        !SameGenshinComOwner(owner.device.Get(), _device) ||
        !SameGenshinComOwner(owner.queue.Get(), _gameCommandQueue) ||
        FAILED(owner.device->GetDeviceRemovedReason()))
    {
        owner.failed = true;
        return E_FAIL;
    }
    owner.lastSignalValue = receipt->value;
    const auto result = owner.queue->Signal(owner.fence.Get(), receipt->value);
    if (SUCCEEDED(result)) receipt->signalled.store(true, std::memory_order_release);
    else owner.failed = true; // no retry or later Signal on this timeline
    return result;
}

bool DLSSG_Dx12::WaitGenshinInputs(ID3D12Device* device, IDXGISwapChain* swapchain,
    ID3D12CommandQueue* queue, ID3D12Fence* produced, UINT64 producedValue,
    ID3D12Fence* copied, UINT64 copiedValue, std::shared_ptr<const GenshinFenceReceipt>& receipt)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (receipt || !CanSubmitInputs() || !IsGenshinBridgeOwner() ||
        !SameGenshinComOwner(device, _device) || !SameGenshinComOwner(swapchain, _swapChain) ||
        !SameGenshinComOwner(queue, GenshinColorWriteQueue(swapchain, device).Get()) ||
        !produced || !copied || !producedValue || producedValue == UINT64_MAX ||
        !copiedValue || copiedValue == UINT64_MAX ||
        !SameDeviceOwner(device, produced) || !SameDeviceOwner(device, copied)) return false;
    auto pending = ReserveGenshinFence(_genshinWaitFence);
    if (!pending) return false;
    // Frame already retains every actual input allocation/fence. Publish this
    // owner BEFORE either Wait can enqueue work and then return a failure.
    receipt = pending;
    const auto* scope = GenshinBridgeFg::PresentScope::Current();
    const bool copiedOrdered = scope && scope->CopiedOrderCovers(queue, copied, copiedValue);
    // Reuse only same-Present native color order on this exact queue/fence.
    // The producer Wait and our real retirement Signal are still required.
    if (FAILED(queue->Wait(produced, producedValue)) ||
        (!copiedOrdered && FAILED(queue->Wait(copied, copiedValue))))
    {
        pending->timeline->failed = true;
        return false; // a partially queued wait never gains completion authority
    }
    return SUCCEEDED(SignalGenshinFence(pending));
}

DLSSG_Dx12::GenshinNormalizedGuides::GenshinNormalizedGuides(const DLSSG_Dx12& provider)
    : owner(provider._genshinOwnerGeneration) {}

// Inequality grants reuse only when BOTH identities are actually proved.
// QueryInterface failure is unknown and must keep the allocation retained.
static bool DistinctGenshinComOwners(IUnknown* left, IUnknown* right)
{
    Microsoft::WRL::ComPtr<IUnknown> a, b;
    return left && right && SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() && b.Get() && a.Get() != b.Get();
}

// Exact route only; Star Rail/ZZZ keep their existing clock/provider behavior.
bool DLSSG_Dx12::IsGenshinBridgeOwner() const
{
    const auto& state = State::Instance();
    return GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
        state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG &&
        state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
        state.currentFG == this && SameGenshinComOwner(state.currentFGSwapchain, _swapChain);
}

bool DLSSG_Dx12::GenshinGuidesCurrent(const std::shared_ptr<const GenshinNormalizedGuides>& guides) const
{
    if (!IsGenshinBridgeOwner() || !guides || guides->owner != _genshinOwnerGeneration ||
        !guides->key.Known() || !Config::Instance()->GenshinControlsCurrent(guides->key.configEpoch) ||
        guides->key.present != _genshinPresentEpoch ||
        guides->key.present != State::Instance().frameCount || guides->key.thread != GetCurrentThreadId() ||
        !guides->guideEpoch || guides->guideEpoch == UINT64_MAX || guides->guideCutSerial == UINT64_MAX ||
        !SameGenshinComOwner(guides->device.Get(), _device) || !guides->producerQueue ||
        guides->producerQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !SameDeviceOwner(_device, guides->depth.Get()) || !SameDeviceOwner(_device, guides->motion.Get())) return false;
    if (!guides->preparedPacket || !guides->preparedPacket->Source() ||
        !guides->preparedPacket->Transfer()) return false;
    const auto& source = *guides->preparedPacket->Source();
    const auto& owner = source.OwnerCanonical();
    const auto& key = guides->key;
    if (owner.device != key.device || owner.context != key.context || owner.deviceEpoch != key.deviceEpoch ||
        owner.producerEpoch != key.ownerEpoch || owner.configEpoch != key.configEpoch ||
        owner.instance != key.instance || owner.generation != key.generation ||
        owner.historyEpoch != key.historyEpoch || owner.sourceFrame != key.sourceFrame || owner.thread != key.thread ||
        source.Grid().outputWidth != key.width || source.Grid().outputHeight != key.height ||
        !guides->privateOutput || source.Textures()[3].identity != guides->privateOutput ||
        !GenshinGuides::SameOwner(guides->preparedPacket->Transfer()->Get().owner, source.OwnerInterfaces())) return false;
    Microsoft::WRL::ComPtr<IUnknown> swapchain;
    Microsoft::WRL::ComPtr<ID3D12Device> producerDevice;
    if (!_swapChain || FAILED(_swapChain->QueryInterface(IID_PPV_ARGS(&swapchain))) ||
        guides->swapchain != reinterpret_cast<uintptr_t>(swapchain.Get()) ||
        FAILED(guides->producerQueue->GetDevice(IID_PPV_ARGS(&producerDevice))) ||
        !SameGenshinComOwner(producerDevice.Get(), _device)) return false;
    const auto depth = guides->depth->GetDesc(), motion = guides->motion->GetDesc();
    const auto shape = [&](const D3D12_RESOURCE_DESC& d) {
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == guides->key.width &&
            d.Height == guides->key.height && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
            d.SampleDesc.Count == 1 && !d.SampleDesc.Quality && d.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN;
    };
    const auto& c = guides->constants;
    return shape(depth) && shape(motion) && depth.Format == DXGI_FORMAT_R32_FLOAT &&
        motion.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
        !SameGenshinComOwner(guides->depth.Get(), guides->motion.Get()) &&
        _constants.displayWidth == guides->key.width && _constants.displayHeight == guides->key.height &&
        _constants.flags[FG_Flags::DisplayResolutionMVs] && !_constants.flags[FG_Flags::JitteredMVs] &&
        !_constants.flags[FG_Flags::Hdr] && !_constants.flags[FG_Flags::Async] &&
        !c.next && c.mvecScale.x == 1.0f / guides->key.width && c.mvecScale.y == 1.0f / guides->key.height &&
        c.jitterOffset.x == 0 && c.jitterOffset.y == 0 && c.motionVectorsJittered == sl::Boolean::eFalse &&
        c.cameraPinholeOffset.x == 0 && c.cameraPinholeOffset.y == 0 &&
        c.motionVectors3D == sl::Boolean::eFalse && c.cameraMotionIncluded == sl::Boolean::eTrue &&
        c.motionVectorsDilated == sl::Boolean::eTrue &&
        c.motionVectorsInvalidValue == 65504.0f &&
        (c.reset == sl::Boolean::eFalse || c.reset == sl::Boolean::eTrue) &&
        (c.depthInverted == sl::Boolean::eFalse || c.depthInverted == sl::Boolean::eTrue) &&
        (c.depthInverted == sl::Boolean::eTrue) == bool(_constants.flags[FG_Flags::InvertedDepth]);
}

void DLSSG_Dx12::CheckGenshinPendingInputs()
{
    // Caller owns continuity. Do not query GetState again or assign a later
    // Present's positive vendor value to any previously zero-valued receipt.
    if (_teardownPending || _teardownQuarantined) return;
    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        const auto& lease = _genshinInputs[i];
        if (!lease || !lease->pendingDeadline) continue;
        const auto now = GetTickCount64();
        const char* stage = nullptr;
        if (now >= lease->pendingDeadline)
            stage = "deadline-drain";
        else if (!lease->acceptedBlocking || !lease->presentReturned || lease->signalled ||
            lease->vendorValue || !lease->vendorFence || !lease->hostCompletion || !lease->guides || !lease->color ||
            lease->Retired() || lease->owner != _genshinOwnerGeneration || lease->epoch != _inputOwnerEpoch ||
            !lease->frame || lease->frame % BUFFER_COUNT != i || _genshinConstantsFrame[i] != lease->frame)
            stage = "pending-receipt-drain";
        else if (!_managedRetained || _ownerChanging || !IsGenshinBridgeOwner() ||
            State::Instance().isShuttingDown || State::Instance().fgChanged ||
            !Config::Instance()->FGEnabled.value_or_default() || native::OpticalFgNrEnabled() ||
            !Config::Instance()->GenshinControlsCurrent(lease->guides->key.configEpoch) ||
            !(lease->inputOwner == CurrentInputOwner()))
            stage = "pending-owner-config-drain";
        else if (lease->thread != GetCurrentThreadId() ||
            !SameGenshinComOwner(lease->device.Get(), _device) ||
            !SameGenshinComOwner(lease->queue.Get(), _gameCommandQueue) ||
            !SameGenshinComOwner(lease->swapchain.Get(), _swapChain))
            stage = "pending-identity-drain";
        else if (FAILED(lease->device->GetDeviceRemovedReason()) ||
            lease->vendorFence->GetCompletedValue() == UINT64_MAX)
            stage = "pending-device-drain";
        if (!stage) continue;
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
            LOG_INFO("R827 FG bounded retirement: pid={} frame={} slot={} stage={} deadline={} now={} vendor_value={} no_authority=true",
                GetCurrentProcessId(), lease->frame, i, stage, lease->pendingDeadline, now, lease->vendorValue);
        // This only requests Off+retain. The existing guarded host drain and
        // successful FreeResources remain the sole fallback retirement boundary.
        Deactivate();
        return;
    }
}

void DLSSG_Dx12::CollectGenshinInputs(bool providerFreed)
{
    // Caller owns continuity. No FG Mutex is acquired from here.
    for (UINT i = 0; i < BUFFER_COUNT; ++i)
    {
        auto lease = _genshinInputs[i];
        if (!lease || lease->owner != _genshinOwnerGeneration || lease->frame % BUFFER_COUNT != i ||
            _genshinConstantsFrame[i] != lease->frame) continue;
        if (!providerFreed)
        {
            if (!lease->acceptedBlocking || !lease->presentReturned || !lease->signalled ||
                !lease->hostCompletion || !lease->hostCompletion->Completed()) continue;
            if (lease->presentingQueueRetirement)
            {
                // sl_dlss_g.h: eBlockPresentingClientQueue blocks this exact
                // queue until the DLSSG workload completes. EndGenshinPresent
                // enqueues the positive host Signal AFTER real original Present.
                // CPU destruction is allowed only after that signal completes;
                // this is not vendorValue==0 completion or a later vendor query.
                if (lease->epoch != _inputOwnerEpoch ||
                    !SameGenshinComOwner(lease->queue.Get(),
                        GenshinColorWriteQueue(lease->swapchain.Get(), lease->device.Get()).Get()) ||
                    !SameGenshinComOwner(lease->queue.Get(), _gameCommandQueue) ||
                    !SameGenshinComOwner(lease->device.Get(), _device) ||
                    !SameGenshinComOwner(lease->swapchain.Get(), _swapChain)) continue;
            }
            else
            {
                // Original non-ordered fallback still needs exact positive
                // vendor input-processing AND host completion. Zero stays unknown.
                if (!lease->vendorFence || !lease->vendorValue) continue;
                const auto vendorCompleted = lease->vendorFence->GetCompletedValue();
                if (vendorCompleted == UINT64_MAX || vendorCompleted < lease->vendorValue) continue;
            }
        }
        std::unique_lock resources(_resourceMutex[i]);
        for (auto type : {FG_ResourceType::Depth, FG_ResourceType::Velocity, FG_ResourceType::PresentColor})
        {
            _frameResources[i].erase(type);
            _resourceReady[i].erase(type);
        }
        _genshinConstantsFrame[i] = 0;
        _genshinInputs[i].reset();
        // Pool readers never need to dereference a destroyed provider to learn
        // that its later successful drain completed this exact generation.
        lease->retired.store(true, std::memory_order_release);
        if(lease->uiMaskTagged)DrawUiMask::Observe(lease->uiMask,DrawUiMask::Consumer::Fg,DrawUiMask::Lifecycle::Completed,
            reinterpret_cast<uintptr_t>(lease.get()),lease->epoch,lease->frame);
    }
}

UINT64 DLSSG_Dx12::BeginGenshinPresent(ID3D12Device* device, IDXGISwapChain* swapchain,
    ID3D12CommandQueue* queue, UINT64 presentEpoch, FG_Constants& constants)
{
    const auto refuse = [this]() -> UINT64 {
        std::lock_guard continuityLock(_inputContinuityMutex);
        for (const auto& lease : _genshinInputs)
        {
            if (!lease || !lease->pendingDeadline) continue;
            static std::atomic<unsigned> reports{0};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
                LOG_INFO("R827 FG bounded retirement: pid={} frame={} stage=begin-authentication-drain deadline={} no_authority=true",
                    GetCurrentProcessId(), lease->frame, lease->pendingDeadline);
            Deactivate();
            CompletePendingTeardown(); // execution-depth guards still veto an unsafe drain
            break;
        }
        return 0;
    };
    if (!IsGenshinBridgeOwner() || !SameGenshinComOwner(swapchain, _swapChain) ||
        !SameGenshinComOwner(queue, _gameCommandQueue) || !device || !presentEpoch) return refuse();
    // Called before FGPresent takes either owner2 or the continuity execution
    // guard. EvaluateState's only nesting remains FG Mutex -> continuity.
    EvaluateState(device, constants);
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!IsGenshinBridgeOwner() || !SameGenshinComOwner(device, _device) ||
        !SameGenshinComOwner(queue, _gameCommandQueue) ||
        _genshinPresentEpoch == presentEpoch || _inputRecordingDepth || _presentExecutionDepth) return refuse();
    _genshinPresentEpoch = presentEpoch;
    if (!CompletePendingTeardown() || !CanSubmitInputs()) return 0;
    CollectGenshinInputs();
    if (_genshinInputs[(_frameCount + 1) % BUFFER_COUNT])
    {
        // A plain return would leave this sequential ring permanently stuck.
        // Never call StartNewFrame/NewFrame over a live slot, even if another
        // slot is free: request the existing safe drain and fail this frame.
        const auto& blocked = _genshinInputs[(_frameCount + 1) % BUFFER_COUNT];
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
            LOG_INFO("R827 FG bounded retirement: pid={} frame={} slot={} stage=next-slot-drain deadline={} no_authority=true",
                GetCurrentProcessId(), blocked->frame, (_frameCount + 1) % BUFFER_COUNT, blocked->pendingDeadline);
        Deactivate();
        CompletePendingTeardown();
        return 0;
    }
    const auto frame = IFGFeature::StartNewFrame(); // advances even while paused/inactive
    _genshinConstantsFrame[GetIndex()] = 0;
    return frame;
}

std::shared_ptr<const DLSSG_Dx12::GenshinInputRetirement>
DLSSG_Dx12::SetGenshinConstants(const std::shared_ptr<const GenshinNormalizedGuides>& guides,
    const GenshinBridgeFg::Key& key, UINT64 frame, bool reset)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!GenshinGuidesCurrent(guides) || guides->key != key || !CanSubmitInputs() ||
        frame != _frameCount || !frame || frame > UINT32_MAX) return {};
    const auto index = GetIndex();
    if (_genshinInputs[index] || !_device || !_gameCommandQueue) return {};
    for (const auto& retained : _genshinInputs)
    {
        if (!retained || !retained->guides) continue;
        // A new slot must contain independent private guide allocations. Neither
        // an unresolved zero nor a pending positive fence permits old-root reuse.
        if (!DistinctGenshinComOwners(guides->depth.Get(), retained->guides->depth.Get()) ||
            !DistinctGenshinComOwners(guides->depth.Get(), retained->guides->motion.Get()) ||
            !DistinctGenshinComOwners(guides->motion.Get(), retained->guides->depth.Get()) ||
            !DistinctGenshinComOwners(guides->motion.Get(), retained->guides->motion.Get()))
        {
            static std::atomic<unsigned> reports{0};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
                LOG_INFO("R827 FG bounded retirement: pid={} frame={} retained_frame={} stage=private-root-alias-drain no_authority=true",
                    GetCurrentProcessId(), frame, retained->frame);
            Deactivate();
            return {};
        }
    }
    std::shared_ptr<GenshinInputRetirement> lease;
    try { lease = std::make_shared<GenshinInputRetirement>(); }
    catch (const std::bad_alloc&) {return {};}

    lease->owner = _genshinOwnerGeneration; lease->epoch = _inputOwnerEpoch; lease->frame = frame;
    lease->presentEpoch = _genshinPresentEpoch; lease->thread = GetCurrentThreadId();
    lease->device = _device; lease->queue = _gameCommandQueue; lease->swapchain = _swapChain;
    lease->hostCompletion = ReserveGenshinFence(_genshinHostFence);
    if (!lease->hostCompletion) return {};
    lease->guides = guides;
    try {
        lease->source = std::shared_ptr<const GenshinBridgeFg::SdkInputSource>(
            new GenshinBridgeFg::SdkInputSource(key, frame, lease->owner, lease->epoch, guides->preparedPacket));
    } catch (const std::bad_alloc&) { return {}; } // no tag/submission has consumed this lease
    _genshinConstants[index] = guides->constants;
    // Continuity may assert reset; it cannot replace the authenticated camera.
    if (reset) _genshinConstants[index].reset = sl::Boolean::eTrue;
    _genshinConstantsFrame[index] = frame;
    _genshinInputs[index] = lease;
    _managedRetained = true; // armed before any potentially partially accepted tag
    return lease;
}

bool DLSSG_Dx12::GenshinSourceCurrent(const std::shared_ptr<const GenshinInputRetirement>& receipt,
    const GenshinBridgeFg::Key& key, UINT64 frame)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!receipt || !receipt->source || !receipt->source->Matches(key, frame) || receipt->Retired() ||
        receipt->owner != _genshinOwnerGeneration || receipt->epoch != _inputOwnerEpoch ||
        receipt->frame != frame || frame != _frameCount || _ownerChanging || _hookRetiring ||
        _teardownPending || _teardownQuarantined || !GenshinGuidesCurrent(receipt->guides) ||
        _genshinInputs[frame % BUFFER_COUNT].get() != receipt.get() ||
        !SameGenshinComOwner(receipt->device.Get(), _device) ||
        !SameGenshinComOwner(receipt->queue.Get(), _gameCommandQueue) ||
        !SameGenshinComOwner(receipt->swapchain.Get(), _swapChain) ||
        FAILED(receipt->device->GetDeviceRemovedReason())) return false;
    return receipt->source->Packet() == receipt->guides->preparedPacket;
}

void DLSSG_Dx12::EndGenshinPresent(const std::shared_ptr<const GenshinInputRetirement>& receipt, bool presented)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    const char* stage="receipt";
    int stateResult=-1;
    uintptr_t vendorIdentity=0;
    uint64_t vendorValue=0;
    int postPresentStatus=-1;
    uint32_t postPresentCount=0;
    HRESULT deviceResult=E_PENDING,expectedResult=E_PENDING,actualResult=E_PENDING,signalResult=E_PENDING;
    const auto trace=[&] {
        // Warmup must not exhaust late failure evidence. Bound rapid stage
        // alternation, retaining periodic summaries for arbitrarily long runs.
        const auto now=GetTickCount64();
        const bool changed=!_genshinRetirementTraceStage ||
            std::string_view(_genshinRetirementTraceStage)!=stage;
        const bool initial=_genshinRetirementTraceCalls++<32;
        const bool transition=changed && _genshinRetirementTraceTransitions<128;
        if (!initial && !transition && now-_genshinRetirementTraceTick<5000) return;
        if (changed) ++_genshinRetirementTraceTransitions;
        _genshinRetirementTraceStage=stage;_genshinRetirementTraceTick=now;
        // Streamline's DLSSGState contract defines numFramesActuallyPresented as
        // frames presented since the previous slDLSSGGetState call. It is a
        // presentation batch, not a generated-frame count. This call may pass
        // through a wrapper, so direct-provider authority is not established. The fence
        // captured below is likewise the input-processing completion fence;
        // neither field is an output-resource or output-retirement witness.
        LOG_INFO("R827 FG retirement: pid={} frame={} stage={} presented={} state_result={} vendor={} vendor_value={} device_hr={:X} expected_hr={:X} actual_hr={:X} signal_hr={:X} post_present_status={} post_present_count={} provider_state_source=slDLSSGGetState provider_state_authority=UNKNOWN_WRAPPED_OR_DIRECT provider_count_scope=frames_presented_since_previous_query provider_generated_count=UNKNOWN provider_output_resource=UNKNOWN provider_output_retirement=UNKNOWN provider_fence_scope=inputs_processing_only owner={} epoch={} present_epoch={} thread={} trace_calls={} count_is_not_generated_count=true no_authority=true",
            GetCurrentProcessId(),receipt?receipt->frame:0,stage,presented,stateResult,vendorIdentity,vendorValue,
            static_cast<unsigned>(deviceResult),static_cast<unsigned>(expectedResult),static_cast<unsigned>(actualResult),static_cast<unsigned>(signalResult),
            postPresentStatus,postPresentCount,_genshinOwnerGeneration,receipt?receipt->epoch:0,
            receipt?receipt->presentEpoch:0,GetCurrentThreadId(),_genshinRetirementTraceCalls);
    };
    if (!receipt || receipt->owner != _genshinOwnerGeneration || !receipt->frame || receipt->Retired()) {trace();return;}
    const auto index = receipt->frame % BUFFER_COUNT;
    auto lease = _genshinInputs[index];
    stage="lease";
    if (!lease || lease.get() != receipt.get() || lease->epoch != receipt->epoch ||
        _genshinConstantsFrame[index] != receipt->frame) {trace();return;}
    struct MaskOutcome {
        const std::shared_ptr<GenshinInputRetirement> lease;
        ~MaskOutcome() {
            // A retained tag or CPU Present return has no GPU completion proof.
            // Host retirement without an accepted HUDless tag cannot prove mask use.
            if(!lease->uiMaskTagged || (!lease->signalled && !lease->Retired()))
                DrawUiMask::Observe(lease->uiMask,DrawUiMask::Consumer::Fg,DrawUiMask::Lifecycle::Unknown,
                    reinterpret_cast<uintptr_t>(lease.get()),lease->epoch,lease->frame);
        }
    } maskOutcome{lease};
    if ((stage="receipt-open",!lease->presentReturned) && // never rebind an older receipt
        (stage="presented",presented) && (stage="blocking-options",lease->acceptedBlocking) &&
        (stage="teardown-pending",!_teardownPending) && (stage="teardown-quarantined",!_teardownQuarantined) &&
        (stage="queue-owner",SameGenshinComOwner(lease->queue.Get(), _gameCommandQueue)) &&
        (stage="device-owner",SameGenshinComOwner(lease->device.Get(), _device)) &&
        (stage="swapchain-owner",SameGenshinComOwner(lease->swapchain.Get(), _swapChain)) &&
        (stage="owner-epoch",lease->epoch == _inputOwnerEpoch) &&
        (stage="present-epoch",lease->presentEpoch == _genshinPresentEpoch) &&
        (stage="thread",lease->thread == GetCurrentThreadId()))
    {
        // Query on the SAME actual present thread after its original Present.
        // sl_dlss_g.h requires this vendor input fence before destruction/use
        // on a non-presenting queue, even for the DX blocking mode. Capture the
        // actual SDK fence pointer, never manufacture one from its numeric value.
        sl::DLSSGState actualState{};
        const auto getState = StreamlineProxy::DLSSGGetState();
        const auto observeState = [&]() {
            // SAME existing post-Present query, never a second consumptive query.
            const auto result = getState(viewport, actualState, nullptr);
            stateResult = static_cast<int>(result);
            _genshinStateObservation={};
            _genshinStateObservation.frame=receipt->frame;
            _genshinStateObservation.epoch=receipt->epoch;
            _genshinStateObservation.result=stateResult;
            if (result == sl::Result::eOk)
            {
                postPresentStatus = static_cast<int>(actualState.status);
                postPresentCount = actualState.numFramesActuallyPresented;
                _genshinStateObservation.valid=true;
                _genshinStateObservation.status=postPresentStatus;
                _genshinStateObservation.presented=postPresentCount;
                _genshinStateObservation.cap=actualState.numFramesToGenerateMax;
                MfgUnlock::RecordState(postPresentCount);
            }
            return result == sl::Result::eOk;
        };
        const bool stateAvailable = getState && observeState();
        // A budget warning is not a vendor input-retirement receipt. In the exact
        // D3D presenting-queue blocking mode, the already accepted frame instead
        // has an independent proof: a real host Signal ordered AFTER original
        // Present on that same queue, collected only after its GPU completion.
        // No warning/default state fields, old fence values or later queries are
        // used as authority. All other errors retain the original safe drain.
        if (stateResult == static_cast<int>(sl::Result::eWarnOutOfVRAM) &&
            IsGenshinBridgeOwner() && !_ownerChanging && _managedRetained && _haveRunningOptions &&
            lease->inputOwner == CurrentInputOwner() && GenshinGuidesCurrent(lease->guides) &&
            lease->color && _lastRunningOptions.mode != sl::DLSSGMode::eOff &&
            _lastRunningOptions.queueParallelismMode == sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue &&
            (static_cast<uint32_t>(_lastRunningOptions.flags) &
                static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff)) &&
            SameGenshinComOwner(lease->queue.Get(),
                GenshinColorWriteQueue(lease->swapchain.Get(), lease->device.Get()).Get()) &&
            lease->queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
            SUCCEEDED(deviceResult = lease->device->GetDeviceRemovedReason()))
        {
            lease->presentReturned = true;
            stage = "budget-warning-presenting-queue-signal";
            lease->signalled = SUCCEEDED(signalResult = SignalGenshinFence(lease->hostCompletion));
            if (lease->signalled)
            {
                lease->presentingQueueRetirement = true;
                if(lease->uiMaskTagged)DrawUiMask::Observe(lease->uiMask,DrawUiMask::Consumer::Fg,DrawUiMask::Lifecycle::Submitted,
                    reinterpret_cast<uintptr_t>(lease.get()),lease->epoch,lease->frame);
                CollectGenshinInputs();
                stage = "budget-warning-presenting-queue-retirement-bound";
                trace();
                return;
            }
            trace();Deactivate();CompletePendingTeardown();return;
        }
        if ((stage="get-state-function",getState) &&
            (stage="get-state",stateAvailable) &&
            (stage="vendor-fence",vendorIdentity=reinterpret_cast<uintptr_t>(actualState.inputsProcessingCompletionFence),actualState.inputsProcessingCompletionFence))
        {
            auto* vendor = static_cast<ID3D12Fence*>(actualState.inputsProcessingCompletionFence);
            Microsoft::WRL::ComPtr<ID3D12Device> vendorDevice;
            Microsoft::WRL::ComPtr<IUnknown> expected, actual;
            if ((stage="vendor-device",SUCCEEDED(deviceResult=vendor->GetDevice(IID_PPV_ARGS(&vendorDevice)))) &&
                (stage="expected-identity",SUCCEEDED(expectedResult=lease->device.As(&expected))) &&
                (stage="actual-identity",SUCCEEDED(actualResult=vendorDevice.As(&actual))) &&
                (stage="device-identity",expected.Get() && actual.Get() && expected.Get() == actual.Get()))
            {
                vendorValue = actualState.lastPresentInputsProcessingCompletionFenceValue;
                lease->vendorFence = vendor;
                lease->vendorValue = vendorValue;
                lease->presentReturned = true;
                // R846: an independent completion proof, not a vendor receipt.
                // The accepted exact-frame options block the authenticated
                // presenting queue until FG finishes. A real Signal(value), ordered
                // after original Present on that queue, covers immutable guides
                // as well as color. Keep the observed vendor pointer/value intact.
                if (IsGenshinBridgeOwner() && !_ownerChanging && _haveRunningOptions &&
                    lease->inputOwner == CurrentInputOwner() && GenshinGuidesCurrent(lease->guides) &&
                    lease->color && actualState.status == sl::DLSSGStatus::eOk &&
                    SameGenshinComOwner(lease->queue.Get(),
                        GenshinColorWriteQueue(lease->swapchain.Get(), lease->device.Get()).Get()) &&
                    _lastRunningOptions.queueParallelismMode == sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue &&
                    lease->queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
                    SUCCEEDED(lease->device->GetDeviceRemovedReason()) && vendor->GetCompletedValue() != UINT64_MAX)
                {
                    stage="presenting-queue-signal";
                    lease->signalled = SUCCEEDED(signalResult=SignalGenshinFence(lease->hostCompletion));
                    if (lease->signalled)
                    {
                        lease->presentingQueueRetirement = true;
                        if(lease->uiMaskTagged)DrawUiMask::Observe(lease->uiMask,DrawUiMask::Consumer::Fg,DrawUiMask::Lifecycle::Submitted,
                            reinterpret_cast<uintptr_t>(lease.get()),lease->epoch,lease->frame);
                        CollectGenshinInputs();
                        stage="presenting-queue-retirement-bound";trace();
                        return;
                    }
                    // A failed signal is never retried/relabelled as successful.
                    trace();Deactivate();CompletePendingTeardown();return;
                }
                if (!vendorValue)
                {
                    // Genuine SAME-Present state/fence, but no retirement value.
                    // Retain every root and this exact immutable zero receipt;
                    // do not Signal, collect, acknowledge completion or query a
                    // later Present to manufacture retirement for this frame.
                    const auto now = GetTickCount64();
                    if ((stage="pending-authentication",IsGenshinBridgeOwner() && !_ownerChanging &&
                        _managedRetained && _haveRunningOptions && GenshinGuidesCurrent(lease->guides) &&
                        lease->color && lease->inputOwner == CurrentInputOwner() &&
                        !State::Instance().isShuttingDown && !State::Instance().fgChanged &&
                        Config::Instance()->FGEnabled.value_or_default() && !native::OpticalFgNrEnabled() &&
                        (static_cast<uint32_t>(_lastRunningOptions.flags) &
                            static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff)) &&
                        SUCCEEDED(lease->device->GetDeviceRemovedReason()) &&
                        vendor->GetCompletedValue() != UINT64_MAX && now <= UINT64_MAX - 1000))
                    {
                        lease->pendingDeadline = now + 1000;
                        CheckGenshinPendingInputs();
                        if (!_teardownPending)
                        {
                            static std::atomic<unsigned> reports{0};
                            if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
                                LOG_INFO("R827 FG bounded retirement: pid={} frame={} slot={} stage=zero-pending deadline={} now={} vendor_value=0 no_authority=true",
                                    GetCurrentProcessId(), lease->frame, index, lease->pendingDeadline, now);
                            stage="zero-pending";trace();
                            return; // only a different unused slot may take the next authenticated frame
                        }
                    }
                }
                else
                {
                    stage="host-signal";
                    lease->signalled = SUCCEEDED(signalResult=SignalGenshinFence(lease->hostCompletion));
                    if (lease->signalled)
                    {
                        if(lease->uiMaskTagged)DrawUiMask::Observe(lease->uiMask,DrawUiMask::Consumer::Fg,DrawUiMask::Lifecycle::Submitted,
                            reinterpret_cast<uintptr_t>(lease.get()),lease->epoch,lease->frame);
                        CollectGenshinInputs();
                        stage="retirement-bound";trace();
                        return; // keep allocations/options for the next real frame
                    }
                }
            }
        }
    }
    // Includes no original Present, rejected options/tags and unknown signal.
    // On failure retain the receipt; CompletePendingTeardown retries on a later
    // safe boundary and publishes it only after actual Off/drain/free succeeds.
    trace();
    Deactivate();
    CompletePendingTeardown();
}

void DLSSG_Dx12::RememberGenshinPresentQueue(IDXGISwapChain* swapchain, ID3D12CommandQueue* queue)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    _genshinCreatedPresentQueue.Reset(); _genshinCreatedSwapchain = 0;
    _starRailGuidePresentQueue.Reset();_starRailGuideSwapchain=0;_starRailGuideQueueCurrent=false;
    if (dlssg::depth_contract::Game()==GameRouting::Identity::StarRail && swapchain && queue)
    {
        Microsoft::WRL::ComPtr<IUnknown> identity;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&identity))) && identity)
        {
            _starRailGuidePresentQueue=queue;
            _starRailGuideSwapchain=reinterpret_cast<uintptr_t>(identity.Get());
            _starRailGuideQueueCurrent=dlssg::depth_contract::SameOwner(queue,_gameCommandQueue);
        }
    }
    if (!GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) || !swapchain || !queue) return;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&identity))) || !identity) return;
    _genshinCreatedPresentQueue = queue;
    _genshinCreatedSwapchain = reinterpret_cast<uintptr_t>(identity.Get());
}

Microsoft::WRL::ComPtr<ID3D12CommandQueue> DLSSG_Dx12::GenshinColorWriteQueue(
    IDXGISwapChain* swapchain, ID3D12Device* device)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    Microsoft::WRL::ComPtr<ID3D12Device> actual;
    Microsoft::WRL::ComPtr<IUnknown> actualSwapchain;
    if (!_genshinCreatedSwapchain || !swapchain ||
        FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&actualSwapchain))) || !actualSwapchain ||
        reinterpret_cast<uintptr_t>(actualSwapchain.Get()) != _genshinCreatedSwapchain ||
        !SameGenshinComOwner(_genshinCreatedPresentQueue.Get(), _gameCommandQueue) ||
        !IsGenshinBridgeOwner() || !SameGenshinComOwner(swapchain, _swapChain) || !device ||
        !_gameCommandQueue || _ownerChanging || _teardownQuarantined || g_dlssgTeardownQuarantined.load() ||
        _gameCommandQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(_gameCommandQueue->GetDevice(IID_PPV_ARGS(&actual))) ||
        !SameGenshinComOwner(actual.Get(), device) || FAILED(device->GetDeviceRemovedReason())) return {};
    // Does not initialize/activate the provider or alter its legitimate pause.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> result = _gameCommandQueue;
    return result;
}

bool DLSSG_Dx12::PrepareGenshinColorWrite(ID3D12Resource* color, ID3D12CommandQueue* writeQueue)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    CheckGenshinPendingInputs();
    // The first private color copy precedes Begin/EvaluateState/CreateContext.
    // With no retained receipt only, authenticate against the already-owned
    // real Present queue device without creating/mutating provider context.
    bool retainedInputs = false;
    for (const auto& retained : _genshinInputs) retainedInputs |= bool(retained);
    Microsoft::WRL::ComPtr<ID3D12Device> writeDevice;
    if (_device) writeDevice = _device;
    else if (!retainedInputs && !_managedRetained && !_teardownPending && _gameCommandQueue)
        _gameCommandQueue->GetDevice(IID_PPV_ARGS(&writeDevice));
    if (!IsGenshinBridgeOwner() || !color || !writeDevice || !_gameCommandQueue ||
        _ownerChanging || _teardownQuarantined || g_dlssgTeardownQuarantined.load() ||
        FAILED(writeDevice->GetDeviceRemovedReason()) || !SameDeviceOwner(writeDevice.Get(), color))
    {
        // Invalid identity never supplies write authority. Existing retained
        // receipts stay alive unless the original owner can safely drain them.
        if (_managedRetained) {Deactivate();CompletePendingTeardown();}
        return false;
    }
    // Authenticate the actual current provider backbuffer, not an arbitrary
    // same-device texture supplied by another route or a stale wrapper.
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    Microsoft::WRL::ComPtr<ID3D12Resource> current;
    if (!_swapChain || FAILED(_swapChain->QueryInterface(IID_PPV_ARGS(&swapchain))) ||
        FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&current))) ||
        !SameGenshinComOwner(color, current.Get()))
    {
        if (_managedRetained) {Deactivate();CompletePendingTeardown();}
        return false;
    }
    if (_teardownPending && !CompletePendingTeardown()) return false;
    CollectGenshinInputs(); // positive ordered host proof or original vendor+host pair
    const auto aliases = [&]() {
        for (const auto& lease : _genshinInputs)
            if (lease && lease->color && !DistinctGenshinComOwners(color, lease->color.Get())) return true;
        return false;
    };
    const auto orderedAliases = [&]() {
        // The caller holds GuardExecution(Inputs) through Wait/Execute/Signal.
        // Permission is for writes on this exact presenting queue only. Unknown
        // COM identity, partial tagging and a different owner remain fail closed.
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
        if (!writeQueue || !SameGenshinComOwner(writeQueue,
                GenshinColorWriteQueue(_swapChain, writeDevice.Get()).Get()) ||
            !SameGenshinComOwner(writeQueue, _gameCommandQueue) ||
            writeQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            FAILED(writeQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
            !SameGenshinComOwner(writeDevice.Get(), queueDevice.Get())) return false;
        for (const auto& lease : _genshinInputs)
        {
            if (!lease || !lease->color || DistinctGenshinComOwners(color, lease->color.Get())) continue;
            if (!SameGenshinComOwner(color, lease->color.Get()) || !lease->acceptedBlocking ||
                !lease->presentReturned || lease->owner != _genshinOwnerGeneration || lease->epoch != _inputOwnerEpoch ||
                lease->thread != GetCurrentThreadId() || !(lease->inputOwner == CurrentInputOwner()) ||
                !SameGenshinComOwner(lease->queue.Get(), writeQueue) ||
                !SameGenshinComOwner(lease->device.Get(), writeDevice.Get()) ||
                !SameGenshinComOwner(lease->swapchain.Get(), _swapChain)) return false;
        }
        return true; // preserves retained leases; does not claim retirement
    };
    if (aliases() && !orderedAliases())
    {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
            LOG_INFO("R827 FG bounded retirement: pid={} frame={} stage=color-write-alias-drain no_authority=true",
                GetCurrentProcessId(), _frameCount);
        // Includes positive-but-incomplete receipts. A ComPtr, zero value,
        // elapsed deadline or host signal alone cannot authorize an overwrite.
        Deactivate();
        if (!CompletePendingTeardown()) return false;
    }
    return CanSubmitInputs() && (!aliases() || orderedAliases());
}

void DLSSG_Dx12::SuspendGenshinPresent()
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    // Check before the route predicate: owner/config loss must also drain.
    CheckGenshinPendingInputs();
    if (_teardownPending || _genshinReflexOwnership == GenshinReflexOwnership::ReleasePending)
    {CompletePendingTeardown();return;}
    // A completed positive receipt must not wait for fresh input that its own
    // retained Frame may be preventing. Original vendor+host gates still apply.
    CollectGenshinInputs();
    if (!IsGenshinBridgeOwner()) return;
    // Explicit Off ends logical FG and SDK Reflex; ordinary input gaps retain them.
    // Request only here; the existing pending boundary drains on a later call.
    if (!Config::Instance()->FGEnabled.value_or_default())
    {
        if (!_teardownQuarantined &&
            (_isActive || _waitingNewFrameData || _inputContinuity.Held() ||
             _haveRunningOptions || _managedRetained))
            Deactivate();
        return;
    }
    const auto current = _genshinInputs[GetIndex()];
    if (current && !current->presentReturned)
    {
        Deactivate(); // partial tagging must use the full safe retirement boundary
        return;
    }
    if (_haveRunningOptions)
    {
        if (!DisableRetainedProvider()) {Deactivate();return;}
        _haveRunningOptions = false;
    }
    // Successful ordinary skips retain allocations and the logical clock.
    // They do not Activate/UpdateTarget anew and starve cold-start pause expiry.
}

bool DLSSG_Dx12::SetGenshinUiMask(const std::shared_ptr<const GenshinInputRetirement>& receipt,
    const std::shared_ptr<DrawUiMask::Frame>& mask)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    // Optional capability: absence never disables the selected ordinary FG path.
    if(!receipt || receipt->Retired() || receipt->presentReturned ||
       _genshinInputs[GetIndex()].get()!=receipt.get() ||
       native::OpticalFgNrEnabled() || Config::Instance()->FGDisableHudless.value_or_default())return false;
    auto* resource=DrawUiMask::Hudless(mask);
    if(!resource || !SameDeviceOwner(_device,resource))return false;
    auto& current=*_genshinInputs[GetIndex()];
    if(current.uiHudless && !SameGenshinComOwner(current.uiHudless.Get(),resource))return false;
    // Retain before the shared tag map can contain a raw pointer. The caller
    // already ordered the exact final-color copy fence on the presenting queue.
    current.uiMask=mask;current.uiHudless=resource;
    const auto desc=resource->GetDesc();Dx12Resource input{};
    input.type=FG_ResourceType::HudlessColor;input.resource=resource;
    input.width=desc.Width;input.height=desc.Height;input.state=D3D12_RESOURCE_STATE_COMMON;
    input.frameIndex=GetIndex();input.validity=FG_ResourceValidity::UntilPresent;
    const bool tagged=SetGenshinResource(receipt,&input);
    if(!tagged)
        DrawUiMask::Observe(mask,DrawUiMask::Consumer::Fg,DrawUiMask::Lifecycle::Unknown,
            reinterpret_cast<uintptr_t>(receipt.get()),current.epoch,current.frame);
    return tagged;
}

bool DLSSG_Dx12::SetGenshinResource(const std::shared_ptr<const GenshinInputRetirement>& receipt, Dx12Resource* input)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (!IsGenshinBridgeOwner() || !receipt || !input || receipt->Retired() || receipt->presentReturned ||
        receipt->owner != _genshinOwnerGeneration || receipt->epoch != _inputOwnerEpoch ||
        receipt->frame != _frameCount || receipt->presentEpoch != _genshinPresentEpoch ||
        receipt->thread != GetCurrentThreadId() || input->frameIndex != GetIndex() ||
        _genshinInputs[GetIndex()].get() != receipt.get() || _genshinTagging ||
        !GenshinGuidesCurrent(receipt->guides) ||
        !SameGenshinComOwner(receipt->device.Get(), _device) ||
        !SameGenshinComOwner(receipt->queue.Get(), _gameCommandQueue) ||
        !SameGenshinComOwner(receipt->swapchain.Get(), _swapChain)) return false;
    const auto& guides = *receipt->guides;
    // No mutable resource flag or generic SetResource caller can claim this
    // policy. Reject substituted pointers, cropped extents, copies and commands.
    if (input->validity != FG_ResourceValidity::UntilPresent || input->cmdList || input->copy || input->top || input->left ||
        input->width != guides.key.width || input->height != guides.key.height) return false;
    if (input->type == FG_ResourceType::Depth || input->type == FG_ResourceType::Velocity)
    {
        auto* expected = input->type == FG_ResourceType::Depth ? guides.depth.Get() : guides.motion.Get();
        if (!SameGenshinComOwner(input->resource, expected) ||
            input->state != D3D12_RESOURCE_STATE_COMMON) return false;
    }
    else if(input->type==FG_ResourceType::HudlessColor) {
        if(!receipt->uiMask || !receipt->uiHudless || input->state!=D3D12_RESOURCE_STATE_COMMON ||
           DrawUiMask::Hudless(receipt->uiMask)!=receipt->uiHudless.Get() ||
           !SameGenshinComOwner(input->resource,receipt->uiHudless.Get()))return false;
    }
    else if (input->type != FG_ResourceType::PresentColor) return false;
    struct TaggingScope {
        bool& flag;
        explicit TaggingScope(bool& f):flag(f) {flag=true;}
        ~TaggingScope() {flag=false;}
    } tagging(_genshinTagging);
    return SetResource(input);
}

bool DLSSG_Dx12::SetResource(Dx12Resource* inputResource)
{
    std::lock_guard continuityLock(_inputContinuityMutex);
    // The ordinary upscaler can still call the shared virtual API. It cannot
    // overwrite the adapter's exact current frame or introduce a second clock.
    const bool genshinOwner = IsGenshinBridgeOwner();
    if (genshinOwner && !_genshinTagging) return false;
    // Only SetGenshinResource can enter this scope, after exact resource checks.
    const bool normalizedGenshin = genshinOwner && _genshinTagging &&
        _genshinInputs[GetIndex()] && GenshinGuidesCurrent(_genshinInputs[GetIndex()]->guides);
    if (_genshinTagging && (!normalizedGenshin || _genshinInputs[GetIndex()]->presentReturned)) return false;
    if (!CanSubmitInputs()) return false;
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // Revalidate at the shared virtual entry as well: a reentrant generic
    // caller inside vendor tagging cannot borrow the private tagging scope.
    if (normalizedGenshin)
    {
        const auto& guides = *_genshinInputs[GetIndex()]->guides;
        if (inputResource->frameIndex != GetIndex() || inputResource->cmdList || inputResource->copy ||
            inputResource->validity != FG_ResourceValidity::UntilPresent || inputResource->top || inputResource->left ||
            inputResource->width != guides.key.width || inputResource->height != guides.key.height) return false;
        if (inputResource->type == FG_ResourceType::Depth || inputResource->type == FG_ResourceType::Velocity)
        {
            auto* expected = inputResource->type == FG_ResourceType::Depth ? guides.depth.Get() : guides.motion.Get();
            if (!SameGenshinComOwner(inputResource->resource, expected) ||
                inputResource->state != D3D12_RESOURCE_STATE_COMMON) return false;
        }
        else if(inputResource->type==FG_ResourceType::HudlessColor) {
            const auto& receipt=*_genshinInputs[GetIndex()];
            if(!receipt.uiMask || !receipt.uiHudless || inputResource->state!=D3D12_RESOURCE_STATE_COMMON ||
               DrawUiMask::Hudless(receipt.uiMask)!=receipt.uiHudless.Get() ||
               !SameGenshinComOwner(inputResource->resource,receipt.uiHudless.Get()))return false;
        }
        else if (inputResource->type != FG_ResourceType::PresentColor) return false;
    }

    // For late sent SL resources
    // we use provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    if (fIndex < 0 || fIndex >= BUFFER_COUNT) return false;

    auto& type = inputResource->type;
    const bool checkedGuide=OwnsDepthContract() &&
        (type==FG_ResourceType::Depth || type==FG_ResourceType::Velocity);
    const auto publicationOwner=CurrentInputOwner();
    if (checkedGuide)
    {
        if (!normalizedGenshin && inputResource!=_upscalerGuidePublication)
            return RefuseDepthContract("unsealed-upscaler-guide-publication");
        if (fIndex!=GetIndex() || !dlssg::depth_contract::OnDevice(_device,inputResource->resource) ||
            !dlssg::depth_contract::Region(inputResource->resource->GetDesc(),inputResource->left,
                inputResource->top,inputResource->width,inputResource->height) || inputResource->copy)
            return RefuseDepthContract("guide-resource-device-or-region");
        if (!normalizedGenshin && (!inputResource->cmdList ||
            inputResource->cmdList->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !dlssg::depth_contract::OnDevice(_device,inputResource->cmdList)))
            return RefuseDepthContract("guide-producer-recording");
    }
    const bool starRailGuide = IsStarRailContinuousOwner() &&
        (type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity);
    const auto sourceGeneration = _inputContinuity.SourceGeneration();
    const auto sourceFrame = _frameCount;
    if (starRailGuide && (!_inputRecordingDepth ||
        _starRailRecordingGeneration != sourceGeneration || _starRailRecordingFrame != sourceFrame))
        return false; // A marker alone cannot turn an old recording into new-source guides.
    if (type == FG_ResourceType::PresentColor)
    {
        // This is the actual provider backbuffer. It is not a HUDless proxy,
        // and only the authenticated Genshin transaction can supply this tag.
        if (fIndex != GetIndex() || _genshinConstantsFrame[fIndex] != _frameCount || !_swapChain ||
            inputResource->validity != FG_ResourceValidity::UntilPresent ||
            inputResource->state != D3D12_RESOURCE_STATE_PRESENT || inputResource->top || inputResource->left ||
            !SameDeviceOwner(_device, inputResource->resource)) return false;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> sc;
        Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
        Microsoft::WRL::ComPtr<IUnknown> expected, actual;
        if (FAILED(_swapChain->QueryInterface(IID_PPV_ARGS(&sc))) ||
            FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer))) ||
            FAILED(backbuffer.As(&expected)) || FAILED(inputResource->resource->QueryInterface(IID_PPV_ARGS(&actual))) ||
            expected.Get() != actual.Get()) return false;
        const auto desc = backbuffer->GetDesc();
        if (inputResource->width != desc.Width || inputResource->height != desc.Height) return false;
        if (normalizedGenshin)
        {
            auto& color = _genshinInputs[fIndex]->color;
            if (color && !SameGenshinComOwner(color.Get(), backbuffer.Get())) return false;
            // Retain before tagging: even a failed/partial SetTagForFrame must
            // not leave only a raw PresentColor pointer in _frameResources.
            color = backbuffer;
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
    if (!normalizedGenshin) _teardownBorrowedInputs = true;
    if (checkedGuide)
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

    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    !native::OpticalFgNrEnabled() && Config::Instance()->FGResourceFlip.value_or_default() &&
                    !normalizedGenshin && // R727: this exact private transaction already matches picture raster
                    (type == FG_ResourceType::Velocity || type == FG_ResourceType::Depth);

    // A cached slot output is not a conversion result for this input.
    if (willFlip)
        fResource->copy = nullptr;
    if (checkedGuide && willFlip)
    {
        // The existing RF shader has no NGX subrect-base arguments. Refuse that
        // unsupported transform rather than quietly changing raster direction.
        auto* feature=State::Instance().currentFeature;
        if (fResource->left || fResource->top || !feature ||
            (Config::Instance()->FGResourceFlipOffset.value_or_default() &&
                fResource->resource->GetDesc().Height!=fResource->height) ||
            fResource->width>(feature->LowResMV()?feature->RenderWidth():feature->DisplayWidth()) ||
            fResource->height>(feature->LowResMV()?feature->RenderHeight():feature->DisplayHeight()))
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

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity || type == FG_ResourceType::PresentColor) ||
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
                State::Instance().fgChanged = true;
                return false;
            }

            lastFormat[fIndex] = desc.Format;
        }

        // ZZZ's external producer list uses SL's eOnlyValidNow contract.
        // StarRail preserves raw=ValidNow / owned conversion=UntilPresent;
        // no additional SDK snapshot is requested for its converted outputs.
        if (checkedGuide && !normalizedGenshin &&
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
            auto frameId = static_cast<uint32_t>(_frameCount - indexDiff);

            sl::FrameToken* frameToken = nullptr;
            auto tokenResult = StreamlineProxy::GetNewFrameToken()(frameToken, &frameId);
            if (tokenResult != sl::Result::eOk || frameToken == nullptr)
            {
                LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);
                if (_inputContinuity.Held())
                {
                    State::Instance().fgChanged = true;
                    UpdateTarget();
                    Deactivate();
                }
                return false;
            }

            // Pin this exact input receipt across the SDK call; a reentrant
            // owner change must not publish the result into a newer slot.
            const auto maskReceipt=normalizedGenshin && type==FG_ResourceType::HudlessColor?
                _genshinInputs[fIndex]:std::shared_ptr<GenshinInputRetirement>{};
            auto result = StreamlineProxy::SetTagForFrame()(*frameToken, viewport, &resourceTag, 1, fResource->cmdList);
            if (result != sl::Result::eOk || !normalizedGenshin || DetailedGenshinFrameLog(_frameCount))
                LOG_DEBUG("SetTagForFrame, frameId: {}, type: {} result: {} ({})", frameId, magic_enum::enum_name(type),
                          magic_enum::enum_name(result), (int32_t) result);

            if (result != sl::Result::eOk)
            {
                State::Instance().fgChanged = true;
                UpdateTarget();
                Deactivate();

                return false;
            }
            // Map retention is not SDK tag acceptance. Observe only the exact
            // leased HUDless input after its real successful SetTagForFrame.
            if(maskReceipt && maskReceipt->uiMask && maskReceipt->uiHudless &&
               SameGenshinComOwner(static_cast<ID3D12Resource*>(resource.native),maskReceipt->uiHudless.Get()))
                maskReceipt->uiMaskTagged=true;
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
        if (checkedGuide)
        {
            if (!OwnsDepthContract() || !CanSubmitInputs() || sourceFrame!=_frameCount ||
                sourceGeneration!=_inputContinuity.SourceGeneration() || !(publicationOwner==CurrentInputOwner()))
                return RefuseDepthContract("guide-publication-reentered");
            auto& tag=_guideTags[fIndex][type==FG_ResourceType::Depth ? 0 : 1];
            tag.owner=publicationOwner;tag.input=*fResource;tag.description=actualDescription;
            tag.frame=sourceFrame;tag.generation=sourceGeneration;tag.accepted=true;tag.flipped=flipped;
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

FGHookRetirementReceipt DLSSG_Dx12::RetireSwapchainForHook(const FGHookRetirementRequest& request)
{
    std::unique_lock execution(_inputContinuityMutex, std::try_to_lock);
    const auto refused = [](HRESULT hr) { return FGHookRetirementReceipt::Refused(hr); };
    if (!execution.owns_lock() || _inputRecordingDepth || _presentExecutionDepth || _ownerChanging ||
        _hookRetirementActive || _teardownCompleting)
        return refused(DXGI_ERROR_WAS_STILL_DRAWING);
    if (_teardownQuarantined || g_dlssgTeardownQuarantined.load())
        return FGHookRetirementReceipt::Quarantined(DXGI_ERROR_WAS_STILL_DRAWING);
    if (!request.Known() || request.provider != static_cast<IFGFeature_Dx12*>(this) ||
        !_genshinOwnerGeneration || _genshinOwnerGeneration == UINT64_MAX || _inputOwnerEpoch >= UINT64_MAX - 2)
        return refused(DXGI_ERROR_INVALID_CALL);
    if (State::Instance().isShuttingDown) return refused(DXGI_ERROR_WAS_STILL_DRAWING);
    if (_hookRetiring && !_hookRetirementBinding.SameOwner(request)) return refused(DXGI_ERROR_INVALID_CALL);
    // Post-cleanup hook queue polling may cross calls, but a recursive_mutex
    // lease must not. Reissue only for this retired owner/serial/current epoch.
    if (_hookRetired)
    {
        if (_inputOwnerEpoch != _hookRetiredEpoch ||
            !SameGenshinComOwner(request.queue, _gameCommandQueue) || !SameGenshinComOwner(request.device, _device))
            return refused(DXGI_ERROR_INVALID_CALL);
        return FGHookRetirementReceipt(request, _genshinOwnerGeneration, _hookRetiredEpoch, &_inputOwnerEpoch,
                                       std::move(execution));
    }
    if (request.window != _hwnd || !SameGenshinComOwner(request.chain, _swapChain) ||
        !SameGenshinComOwner(request.queue, _gameCommandQueue) || !SameGenshinComOwner(request.device, _device))
        return refused(DXGI_ERROR_INVALID_CALL);

    _hookRetirementActive = true;
    struct FinishRetirement { bool& active; ~FinishRetirement() { active = false; } } finish { _hookRetirementActive };
    if (!_hookRetiring) _hookRetirementBinding = request;
    _hookRetiring = true; // Sticky admission veto, also while endpoints/host/SDK are pending.
    const auto incomplete = [&](HRESULT hr) {
        if (request.terminal || _teardownQuarantined)
        {
            QuarantineFailedTeardown();
            return FGHookRetirementReceipt::Quarantined(hr);
        }
        return FGHookRetirementReceipt::Pending();
    };
    auto endpoints = framegen::final_output::TryRetireProvider(_finalOutputProvider, _swapChain);
    if (endpoints.Status() != S_OK) return incomplete(endpoints.Status());
    if (!_hookHostFreed)
    {
        if (!_teardownPending)
        {
            _managedRetained = true;
            Deactivate(); // Once; retry must not invalidate the retained generation/target.
        }
        const bool complete = request.terminal ? CompletePendingTeardownForTermination() : CompletePendingTeardown();
        if (!complete) return incomplete(DXGI_ERROR_WAS_STILL_DRAWING);
        _hookHostFreed = true;
    }
    if (_device->GetDeviceRemovedReason() != S_OK)
    { QuarantineFailedTeardown(); return FGHookRetirementReceipt::Quarantined(DXGI_ERROR_DEVICE_REMOVED); }
    MenuOverlayDx::CleanupRenderTarget(true, NULL);
    if (!ReleaseObjectsChecked()) return incomplete(DXGI_ERROR_WAS_STILL_DRAWING);
    const auto endpointResult = endpoints.Commit();
    if (endpointResult != S_OK) return incomplete(endpointResult);
    _finalOutputProvider.reset();
    _genshinCreatedPresentQueue.Reset(); _genshinCreatedSwapchain = 0;
    _starRailGuidePresentQueue.Reset();_starRailGuideSwapchain=0;_starRailGuideQueueCurrent=false;
    _swapChain = nullptr; _hwnd = nullptr;
    ++_inputOwnerEpoch;
    _hookRetired = true;
    _hookRetiredEpoch = _inputOwnerEpoch;
    return FGHookRetirementReceipt(request, _genshinOwnerGeneration, _inputOwnerEpoch, &_inputOwnerEpoch, std::move(execution));
}

bool DLSSG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");
    bool useLegacyMutex = false;
    bool mutexLocked = false;
    {
        std::lock_guard continuityLock(_inputContinuityMutex);
        if (_hookRetiring || _teardownCompleting) return false;
        useLegacyMutex = !_continuityOwner &&
            Config::Instance()->FGUseMutexForSwapchain.value_or_default();
    }

    if (useLegacyMutex)
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("ReleaseSwapchain refused: mutex tag 1 supplies no completion receipt");
            return false;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        mutexLocked = true;
        LOG_TRACE("Accuired Mutex: {}", Mutex.getOwner());
    }

    auto executionGuard = GuardExecution(FGExecutionScope::Lifecycle);
    if (!PrepareForSwapchainChange(true))
    {
        QuarantineFailedTeardown();
        if (mutexLocked) Mutex.unlockThis(1);
        return false;
    }
    framegen::final_output::RetireProvider(_finalOutputProvider);
    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    // if (_fgContext != nullptr)
    //     DestroyFGContext();

    // if (!State::Instance().isShuttingDown)
    //{
    //     if (_swapChainContext != nullptr)
    //         DestroySwapchainContext();

    //    _swapChainContext = nullptr;
    //    State::Instance().currentFGSwapchain = nullptr;
    //}

    const bool released = ReleaseObjectsChecked();

    if (mutexLocked)
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return released;
}
