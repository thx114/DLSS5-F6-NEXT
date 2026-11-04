#include "pch.h"
#include "Reflex_Hooks.h"
#include <Config.h>
#include <Util.h>

#include <nvapi/fakenvapi.h>

#include <native/VkPresentBridge.h>
#include <native/NativeLowLatency.h>
#include <misc/IdentifyGpu.h>
#include <proxies/Streamline_Proxy.h>
#include <framegen/dlssg/DLSSG_Dx12.h>
#include <framegen/dlssg/StarRailF5DsPolicy.h>
#include <genshin/GenshinHookCpuTiming.h>
#include <low_latency/GpuTimeline.h>

#include <magic_enum.hpp>
#include <nvapi/fakenvapi/nvapi_calls.h>

#include <math.h>
#include <imgui/ImGuiNotify.hpp>

#include <intrin.h>
#include <unordered_map>

// Written on the marker's thread, read by XeFG on the threads that tag its frames
static inline std::atomic<uint64_t> _lastFrameId[20] = {};
// Not a reference: dropped by forgetSleepDevice when the device goes
static inline std::atomic<IUnknown*> _lastDev[20] = {};

// #define LOG_REFLEX_CALLS

void ReflexHooks::noteMarker(uint64_t frameId, uint32_t markerType, IUnknown* device)
{
    // TODO: reflexFrameId gets constantly changed, up and down depending on the marker
    State::Instance().reflexFrameId = frameId;

    // if (markerType == PRESENT_END)
    if (markerType < std::size(_lastFrameId))
    {
        _lastFrameId[markerType] = frameId;
        _lastDev[markerType] = device;
    }

    // Our own markers (Optical F5Low's low latency) are not the game sending markers
    if (markerType == SIMULATION_START && !_ownCall)
        _lastMarkerFrame = State::Instance().fgLastFrame;
}

ID3D12Device* ReflexHooks::bridgedXellDevice()
{
    const bool xefgOutput = State::Instance().activeFgOutput == FGOutput::XeFG;

    // Lock-free (XeFG's present thread asks while the bridge's teardown holds its lock and waits for that thread)
    ID3D12Device* device = xefgOutput ? VkPresentBridge::Device() : nullptr;
    const bool dxvkWithoutVulkanBridge = VkPresentBridge::DxvkGame() && !VkPresentBridge::DxvkThroughVulkan();
    const bool forward =
        native::xefgreflex::ShouldForward(xefgOutput, device != nullptr, dxvkWithoutVulkanBridge);

    return forward ? device : nullptr;
}

void ReflexHooks::noteBridgedForwarding(bool forwarding)
{
    if (_bridgedForwarding.exchange(forwarding) != forwarding)
    {
        if (forwarding)
            LOG_INFO("The game's Vulkan Reflex now feeds XeFG's XeLL on the bridge's D3D12 device");
        else
            LOG_INFO("The game's Vulkan Reflex no longer feeds XeFG's XeLL");
    }
}

bool ReflexHooks::gameReflexFeedsXell()
{
    return bridgedReflexForwarding() || (State::Instance().activeFgOutput == FGOutput::XeFG && gameCalledReflex());
}

namespace
{
native::xefgreflex::SleepMode ToMode(const NV_VULKAN_SET_SLEEP_MODE_PARAMS& params)
{
    native::xefgreflex::SleepMode mode;
    mode.lowLatency = params.bLowLatencyMode != 0;
    mode.boost = params.bLowLatencyBoost != 0;
    mode.minimumIntervalUs = params.minimumIntervalUs;
    return mode;
}

bool ForceDisabled() { return Config::Instance()->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable; }

bool XefgOutput() { return State::Instance().activeFgOutput == FGOutput::XeFG; }

// XefgReflexRule.h XellHoldsCap, from frame generation's own state: update()'s fgActive is false on every generated
// frame's present, and the answer must not flip with it
bool XellHoldsCap()
{
    static const bool intelGpu = IdentifyGpu::getPrimaryGpu().vendorId == VendorId::Intel;
    return native::xefgreflex::XellHoldsCap(ForceDisabled(), intelGpu, native::lowlatency::XefgGenerating());
}

// A D3D game's sleep mode (cap already in) as its Reflex output gets it: XefgReflexRule.h SleepModeForOutput
NV_SET_SLEEP_MODE_PARAMS ForOutput(const NV_SET_SLEEP_MODE_PARAMS& params)
{
    native::xefgreflex::SleepMode mode;
    mode.lowLatency = params.bLowLatencyMode != 0;
    mode.boost = params.bLowLatencyBoost != 0;
    mode.minimumIntervalUs = params.minimumIntervalUs;
    mode = native::xefgreflex::SleepModeForOutput(mode, 0, XefgOutput(), ForceDisabled());

    NV_SET_SLEEP_MODE_PARAMS out = params;
    out.bLowLatencyMode = mode.lowLatency;
    return out;
}
// Build from the requested interval without publishing it as an accepted cap.
NV_SET_SLEEP_MODE_PARAMS XellSleepModeParams(const native::xefgreflex::SleepMode& game, uint32_t intervalUs)
{
    const auto mode = native::xefgreflex::SleepModeForOutput(game, intervalUs, true, ForceDisabled());

    NV_SET_SLEEP_MODE_PARAMS params {};
    params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    params.bLowLatencyMode = mode.lowLatency;
    params.bLowLatencyBoost = mode.boost;
    params.minimumIntervalUs = mode.minimumIntervalUs;
    params.bUseMarkersToOptimize = mode.useMarkersToOptimize;
    return params;
}
} // namespace

NvAPI_Status ReflexHooks::xellSetSleepMode(ID3D12Device* xellDevice, const native::xefgreflex::SleepMode& game)
{
    auto params = XellSleepModeParams(game, _minimumIntervalUs);
    const auto result = nvapi_calls::NvAPI_D3D_SetSleepMode(xellDevice, &params);

    static bool logged = false;
    if (result != NVAPI_OK && !logged)
    {
        logged = true;
        LOG_WARN("XeLL refused the game's sleep mode: {}", magic_enum::enum_name(result));
    }

    return result;
}

void ReflexHooks::noteLowLatency2SleepMode(const native::xefgreflex::SleepMode* mode)
{
    std::scoped_lock lock(_sleepModeMutex);

    _lastLl2ModeSet = mode != nullptr;
    _lastLl2Mode = mode != nullptr ? *mode : native::xefgreflex::SleepMode {};
}

NvAPI_Status ReflexHooks::xellSleep(ID3D12Device* xellDevice)
{
    const auto result = nvapi_calls::NvAPI_D3D_Sleep(xellDevice);

    static bool logged = false;
    if (result != NVAPI_OK && !logged)
    {
        logged = true;
        LOG_WARN("XeLL Sleep failed: {}", magic_enum::enum_name(result));
    }

    return result;
}

void ReflexHooks::xellMarker(ID3D12Device* xellDevice, uint64_t frameId, uint32_t nvapiMarker)
{
    _updatesWithoutMarker = 0;
    noteMarker(frameId, nvapiMarker, nullptr);

    if (!native::xefgreflex::XellTakesMarker(nvapiMarker))
        return;

    NV_LATENCY_MARKER_PARAMS params {};
    params.version = NV_LATENCY_MARKER_PARAMS_VER;
    params.frameID = frameId;
    params.markerType = (NV_LATENCY_MARKER_TYPE) nvapiMarker;
    nvapi_calls::NvAPI_D3D_SetLatencyMarker(xellDevice, &params);
}

ID3D12Device* ReflexHooks::vkForwardDevice()
{
    auto* device = bridgedXellDevice();
    noteBridgedForwarding(device != nullptr);

    if ((device != nullptr) == _vkNvApiForwarding.load(std::memory_order_acquire))
        return device;

    std::scoped_lock lock(_sleepModeMutex);

    if ((device != nullptr) == _vkNvApiForwarding.load(std::memory_order_relaxed))
        return device;

    _vkNvApiForwarding = device != nullptr;

    // The game set its sleep mode to the other side: its new owner gets it (the old one keeps what it had). The driver
    // gets the cap back with it, as setFPSLimit gives it while the driver is the owner.
    if (_lastVkSleepDev != nullptr)
    {
        if (device != nullptr)
        {
            forwardVkSleepMode(device, _lastVkSleepDev, _lastVkSleepParams);
        }
        else
        {
            NV_VULKAN_SET_SLEEP_MODE_PARAMS params = _lastVkSleepParams;

            if (_minimumIntervalUs != 0)
                params.minimumIntervalUs = (std::max<decltype(params.minimumIntervalUs)>)(params.minimumIntervalUs, _minimumIntervalUs);

            o_NvAPI_Vulkan_SetSleepMode(_lastVkSleepDev, &params);
        }
    }

    return device;
}

// XeLL gets the game's mode, the driver the same call with low latency off (it still has to run its Sleep)
NvAPI_Status ReflexHooks::forwardVkSleepMode(ID3D12Device* xellDevice, HANDLE vkDevice,
                                             const NV_VULKAN_SET_SLEEP_MODE_PARAMS& game)
{
    xellSetSleepMode(xellDevice, ToMode(game));

    constexpr auto off = native::xefgreflex::kDriverSleepMode;
    NV_VULKAN_SET_SLEEP_MODE_PARAMS driver = game;
    driver.bLowLatencyMode = off.lowLatency;
    driver.bLowLatencyBoost = off.boost;
    driver.minimumIntervalUs = off.minimumIntervalUs;

    return o_NvAPI_Vulkan_SetSleepMode(vkDevice, &driver);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif
    std::scoped_lock lock(_sleepModeMutex);

    if (isGameCall(_ReturnAddress()))
    {
        _gameCalledReflex = true;
        _gameCalledSetSleepMode = true;
    }

    // Our own calls (Optical F5Low's low latency) are not stored: update() would hand the cap to Reflex
    // (reflexLimitsFps, setFPSLimit) with no halving for frame generation. They carry the cap themselves when the low
    // latency keeps it (native/NativeLowLatencyRule.h PlanCap) and _minimumIntervalUs is never put into them.
    // With XeFG they take SleepModeForOutput too: our "off" (stopped, switched off) leaves XeLL's low latency on, as
    // XeFG wants it; with no one marking frames or sleeping any more, XeLL then waits for nothing.
    if (_ownCall)
    {
        if (XefgOutput())
        {
            auto forXefg = ForOutput(*pSetSleepModeParams);
            return nvapi_calls::NvAPI_D3D_SetSleepMode(pDev, &forXefg);
        }

        return o_NvAPI_D3D_SetSleepMode(pDev, pSetSleepModeParams);
    }

    // Store for later so we can adjust the fps whenever we want
    memcpy(&_lastSleepParams, pSetSleepModeParams, sizeof(NV_SET_SLEEP_MODE_PARAMS));
    _lastSleepDev = pDev;

    if (State::Instance().gameQuirks & GameQuirk::HitmanReflexHacks)
        _lastSetSleepThread = std::this_thread::get_id();

    if (_minimumIntervalUs != 0)
        pSetSleepModeParams->minimumIntervalUs = (std::max<decltype(pSetSleepModeParams->minimumIntervalUs)>)(pSetSleepModeParams->minimumIntervalUs, _minimumIntervalUs);

    if (XefgOutput())
    {
        auto forXefg = ForOutput(*pSetSleepModeParams);
        return nvapi_calls::NvAPI_D3D_SetSleepMode(pDev, &forXefg);
    }

    return o_NvAPI_D3D_SetSleepMode(pDev, pSetSleepModeParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_Sleep(IUnknown* pDev)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    const auto returnAddress = _ReturnAddress();
    // Unknown caller identity cannot authorize redirecting its Sleep to SL.
    const bool gameCall = Util::GetCallerModule(returnAddress) != nullptr && isGameCall(returnAddress);
    if (gameCall)
        _gameCalledReflex = true;

    static thread_local bool skip = false;
    if (gameCall && State::Instance().activeFgOutput == FGOutput::DLSSG &&
        Config::Instance()->FGEnabled.value_or_default() &&
        Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default() && State::Instance().currentFG &&
        State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused())
    {
        if (!skip)
        {
            uint32_t frameCount = (uint32_t) _lastFrameId[SIMULATION_START] + 1;

            sl::FrameToken* frameToken = nullptr;
            const auto getToken = StreamlineProxy::GetNewFrameToken();
            const auto sleep = StreamlineProxy::ReflexSleep();
            if (getToken && sleep && getToken(frameToken, &frameCount) == sl::Result::eOk && frameToken)
            {
                LOG_TRACE("Sleep for frame {}", frameCount);

                skip = true;
                sleep(*frameToken);
                skip = false;

                // Once SL Sleep ran, never append another native Sleep, even
                // if SL returned a failure. Preserve the existing return policy.
                return NVAPI_OK;
            }
            // SL Sleep has not run: use the original NVAPI fallback below.
        }
        else
        {
            _lastSleepDev = pDev;
            return o_NvAPI_D3D_Sleep(pDev);
        }
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
        return nvapi_calls::NvAPI_D3D_Sleep(pDev);

    // Our own Sleep (Optical F5Low's low latency) is not a Reflex device of the game's: stored, update() would see one
    // and hand the fps cap to Reflex (reflexLimitsFps) with the game's sleep parameters, which were never set (version
    // 0, low latency off), skipping OptiScaler's own limiter. Our SetSleepMode carries the cap itself
    // (native/NativeLowLatency.cpp).
    // The menu's Reflex timing graph still reads ours (updateTimingData).
    if (_ownCall)
        _ownSleepDev = pDev;
    else
        _lastSleepDev = pDev;

    return o_NvAPI_D3D_Sleep(pDev);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
        return nvapi_calls::NvAPI_D3D_GetLatency(pDev, pGetLatencyParams);

    return o_NvAPI_D3D_GetLatency(pDev, pGetLatencyParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D_SetLatencyMarker(IUnknown* pDev,
                                                       NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    // Bracket the actual driver call, not entry into the forwarding hook.
    const auto forwardDriver = [&] {
        const bool collect = pSetLatencyMarkerParams->markerType == RENDERSUBMIT_START && GpuTimeline::Enabled();
        const double begin = collect ? GpuTimeline::QpcUs() : 0.0;
        const auto result = o_NvAPI_D3D_SetLatencyMarker(pDev, pSetLatencyMarkerParams);
        const double end = collect ? GpuTimeline::QpcUs() : 0.0;
        if (collect && result == NVAPI_OK)
            GpuTimeline::Marker(pSetLatencyMarkerParams->frameID, pDev, begin, end);
        return result;
    };
    _updatesWithoutMarker = 0;

    const bool gameCall = isGameCall(_ReturnAddress());
    if (gameCall)
        _gameCalledReflex = true;

    // LOG_DEBUG("frameID: {}, markerType: {}", pSetLatencyMarkerParams->frameID,
    //           magic_enum::enum_name(pSetLatencyMarkerParams->markerType));

    // Some games just stop sending any async markers when DLSSG is disabled, so a reset is needed
    if (_lastAsyncMarkerFrameId + 10 < pSetLatencyMarkerParams->frameID)
    {
        _FgNumFramesToGenerate = 0;
    }

    // RTSS reflex markers have a frameid marker in the high bits
    if (pSetLatencyMarkerParams->frameID >> 32)
    {
        if (!State::Instance().rtssReflexInjection && State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            ImGuiToast notification { ImGuiToastType::Warning, 10000 };
            notification.setTitle("RTSS + XeFG detected");
            notification.setContent(
                "RTSS Reflex Injection is known to cause issues.\nEspecially when using XeFG.\nPlease disable it.");
            ImGui::InsertNotification(notification);
        }

        State::Instance().rtssReflexInjection = true;
    }

    noteMarker(pSetLatencyMarkerParams->frameID, pSetLatencyMarkerParams->markerType, pDev);

    static bool skip[20] = {};

    if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
        Config::Instance()->FGEnabled.value_or_default() &&
        Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default() && State::Instance().currentFG &&
        State::Instance().currentFG->IsActive() && !State::Instance().currentFG->IsPaused())
    {
        sl::PCLMarker marker {};
        bool noMarker = false;

        switch (pSetLatencyMarkerParams->markerType)
        {
        case SIMULATION_START:
            marker = sl::PCLMarker::eSimulationStart;
            break;

        case SIMULATION_END:
            marker = sl::PCLMarker::eSimulationEnd;
            break;

        case RENDERSUBMIT_START:
            marker = sl::PCLMarker::eRenderSubmitStart;
            break;

        case RENDERSUBMIT_END:
            marker = sl::PCLMarker::eRenderSubmitEnd;
            break;

        case PRESENT_START:
            marker = sl::PCLMarker::ePresentStart;
            break;

        case PRESENT_END:
            marker = sl::PCLMarker::ePresentEnd;
            break;

        case INPUT_SAMPLE:
            marker = sl::PCLMarker::eControllerInputSample;
            break;

        case TRIGGER_FLASH:
            marker = sl::PCLMarker::eTriggerFlash;
            break;

        case PC_LATENCY_PING:
            marker = sl::PCLMarker::eDeltaTCalculation;
            break;

        case OUT_OF_BAND_RENDERSUBMIT_START:
            marker = sl::PCLMarker::eOutOfBandRenderSubmitStart;
            break;

        case OUT_OF_BAND_RENDERSUBMIT_END:
            marker = sl::PCLMarker::eOutOfBandRenderSubmitEnd;
            break;

        case OUT_OF_BAND_PRESENT_START:
            marker = sl::PCLMarker::eOutOfBandPresentStart;
            break;

        case OUT_OF_BAND_PRESENT_END:
            marker = sl::PCLMarker::eOutOfBandPresentEnd;
            break;

        default:
            noMarker = true;
        }

        auto index = (UINT) marker;

        if (!noMarker && !skip[index])
        {
            if (pSetLatencyMarkerParams->markerType == PRESENT_START && State::Instance().currentFG != nullptr)
            {
                auto* fg = State::Instance().currentFG;
                if (auto* dlssg = dynamic_cast<DLSSG_Dx12*>(fg))
                {
                    static const bool starRailExecutable =
                        _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0;
                    // Only F5-loaded SDK callers are non-source here. Our native
                    // low-latency markers, game-owned Streamline and unknown callers
                    // keep their original source-clock path.
                    const bool sdkStarRailMarker = !gameCall && !_ownCall && starRailExecutable &&
                        !State::Instance().gameCalledSlInit && !State::Instance().externalFrameGeneration &&
                        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
                        State::Instance().activeFgInput == FGInput::Upscaler;
                    // F5-DS uses these actual marker IDs for the source clock.
                    // Defer only clock adoption to the next guarded source entry:
                    // SDK worker -> source lock -> SDK Present would deadlock.
                    // Token/PCL forwarding below keeps the original marker ID.
                    // Native ZZZ owns a selected-SR ordinal, independent of all
                    // incoming game/SDK marker counters. Do not enter its source
                    // lock from an SDK worker while Present waits for that worker.
                    const bool nativeZzzMarker = dlssg->OwnsNativeZzzSource() || dlssg->TracksNativeGuides();
                    if (sdkStarRailMarker && StarRailF5Ds::Selected())
                        dlssg->ObserveStarRailF5DsMarker(pSetLatencyMarkerParams->frameID);
                    else if (!sdkStarRailMarker && !nativeZzzMarker)
                        dlssg->SetFrameCountFromReflex(pSetLatencyMarkerParams->frameID);
                }
                else
                {
                    const auto frameCount = fg->FrameCount();
                    if (pSetLatencyMarkerParams->frameID - frameCount > 1)
                        LOG_WARN("FrameId Moved too much??? {} -> {}", frameCount, pSetLatencyMarkerParams->frameID);

                    if (pSetLatencyMarkerParams->frameID != frameCount)
                        fg->SetFrameCount(pSetLatencyMarkerParams->frameID);
                }

                State::Instance().reflexFrameId = pSetLatencyMarkerParams->frameID;
            }

            sl::FrameToken* frameToken = nullptr;
            uint32_t frameCount = (uint32_t) pSetLatencyMarkerParams->frameID;
            const sl::Result tokenResult = StreamlineProxy::GetNewFrameToken()(frameToken, &frameCount);
            // Independent robustness: no PCL/timing/reentry state without a
            // valid token. Preserve the original NVAPI fallback and noMarker.
            if (tokenResult != sl::Result::eOk || frameToken == nullptr)
                return noMarker ? NVAPI_OK : forwardDriver();

            LOG_TRACE("{} for frame {}", magic_enum::enum_name(marker), frameCount);

            // R897: the process was admitted by the actual StarRail source wrapper.
            // Unknown/worker callers keep parentKnown=false; no provider query or lock here.
            const bool collectMarker=GenshinHookCpuTiming::StarRailTimingActive() &&
                !State::Instance().externalFrameGeneration && !State::Instance().gameCalledSlInit &&
                State::Instance().swapchainInteropApi==SwapchainInteropApi::Dx11wDx12 &&
                State::Instance().activeFgInput==FGInput::Upscaler;
            auto markerStamp=collectMarker ? GenshinHookCpuTiming::MakeStarRailStamp(gameCall ?
                GenshinHookCpuTiming::StarRailOrigin::NvapiForwardGame : (_ownCall ?
                GenshinHookCpuTiming::StarRailOrigin::NvapiForwardOwn : GenshinHookCpuTiming::StarRailOrigin::NvapiForwardOther)) :
                GenshinHookCpuTiming::StarRailStamp{};
            if(collectMarker){
                markerStamp.marker=static_cast<uint32_t>(marker);markerStamp.frame=pSetLatencyMarkerParams->frameID;
                if (tokenResult == sl::Result::eOk && frameToken != nullptr)
                {
                    markerStamp.markerFrame=static_cast<uint64_t>(*frameToken);markerStamp.markerFrameKnown=true;
                }
            }
            skip[index] = true;
            GenshinHookCpuTiming::Scope markerTiming(GenshinHookCpuTiming::Stage::FgPclMarker,collectMarker);
            const auto markerResult=StreamlineProxy::PCLSetMarker()(marker, *frameToken);
            const auto markerWall=markerTiming.Finish();
            skip[index] = false;
            if(collectMarker){
                markerStamp.result=static_cast<uint32_t>(markerResult);markerStamp.resultKnown=true;
                GenshinHookCpuTiming::RecordStarRailCall(GenshinHookCpuTiming::StarRailCall::PclMarker,markerStamp,markerWall);
            }

            return NvAPI_Status::NVAPI_OK;
        }
        else
        {
            if (noMarker)
                return NVAPI_OK;
            else
                return forwardDriver();
        }
    }

    if (State::Instance().gameQuirks & GameQuirk::HitmanReflexHacks)
    {
        if ((pSetLatencyMarkerParams->markerType != PRESENT_START &&
             pSetLatencyMarkerParams->markerType != PRESENT_END) &&
            _lastSetSleepThread != std::this_thread::get_id())
        {
            LOG_TRACE("Skipping marker for a hack");
            return NVAPI_OK;
        }

        if (pSetLatencyMarkerParams->markerType == SIMULATION_END)
        {
            NV_LATENCY_MARKER_PARAMS newParams = *pSetLatencyMarkerParams;
            auto previousThread = _lastSetSleepThread;
            _lastSetSleepThread = std::this_thread::get_id();

            newParams.markerType = RENDERSUBMIT_START;
            hkNvAPI_D3D_SetLatencyMarker(pDev, &newParams);

            newParams.markerType = RENDERSUBMIT_END;
            hkNvAPI_D3D_SetLatencyMarker(pDev, &newParams);

            _lastSetSleepThread = previousThread;
        }
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
        return nvapi_calls::NvAPI_D3D_SetLatencyMarker(pDev, pSetLatencyMarkerParams);

    return forwardDriver();
}

NvAPI_Status ReflexHooks::hkNvAPI_D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                            NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (isGameCall(_ReturnAddress()))
        _gameCalledReflex = true;

    _lastAsyncMarkerFrameId = pSetAsyncFrameMarkerParams->frameID;

    // if (pSetAsyncFrameMarkerParams->markerType == OUT_OF_BAND_PRESENT_START)
    //{
    //     constexpr size_t history_size = 20;
    //     static size_t counter = 0;
    //     static NvU64 previous_frame_ids[history_size] = {};

    //    previous_frame_ids[counter % history_size] = pSetAsyncFrameMarkerParams->frameID;
    //    counter++;

    //    size_t valid_count = (counter < history_size) ? counter : history_size;

    //    size_t max_run_length = 1;
    //    size_t current_run = 1;

    //    for (size_t i = 1; i < valid_count; i++)
    //    {
    //        if (previous_frame_ids[i] == previous_frame_ids[i - 1])
    //        {
    //            current_run++;
    //        }
    //        else
    //        {
    //            max_run_length = std::max(max_run_length, current_run);
    //            current_run = 1;
    //        }
    //    }

    //    max_run_length = std::max(max_run_length, current_run);

    //    int detected_mode = 0;
    //    if (max_run_length >= 4)
    //        detected_mode = 3;
    //    else if (max_run_length == 3)
    //        detected_mode = 2;
    //    else if (max_run_length == 2)
    //        detected_mode = 1;

    //    // --- Stability filter ---
    //    static int candidate_mode = 0; // No FG at start
    //    static int candidate_count = 0;
    //    constexpr int stability_threshold = 4;

    //    if (detected_mode == candidate_mode)
    //    {
    //        candidate_count++;
    //    }
    //    else
    //    {
    //        candidate_mode = detected_mode;
    //        candidate_count = 1;
    //    }

    //    // Redundant
    //    // max_run_length = std::max(max_run_length, current_run);

    //    // Only commit after stable detection
    //    if (candidate_count >= stability_threshold)
    //    {
    //        if (candidate_mode == 0 && _FgNumFramesToGenerate > 0)
    //        {
    //            _FgNumFramesToGenerate = 0;
    //            LOG_DEBUG("DLSS FG no longer detected");
    //        }
    //        else if (_FgNumFramesToGenerate != candidate_mode)
    //        {
    //            _FgNumFramesToGenerate = candidate_mode;

    //            LOG_DEBUG("DLSS FG detected: {}x mode", candidate_mode + 1);
    //        }
    //    }
    //}

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
        return nvapi_calls::NvAPI_D3D12_SetAsyncFrameMarker(pCommandQueue, pSetAsyncFrameMarkerParams);

    return o_NvAPI_D3D12_SetAsyncFrameMarker(pCommandQueue, pSetAsyncFrameMarkerParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_Vulkan_SetLatencyMarker(HANDLE vkDevice,
                                                          NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (isGameCall(_ReturnAddress()))
        _gameCalledReflex = true;

    _updatesWithoutMarker = 0;

    // A bridged game: XeFG's XeLL gets the marker (and its frame ID is noted for XeFG to follow), the driver still gets
    // it too. Our own markers are not the game's.
    if (!_ownCall)
    {
        if (auto* xellDevice = vkForwardDevice())
        {
            xellMarker(xellDevice, pSetLatencyMarkerParams->frameID, (uint32_t) pSetLatencyMarkerParams->markerType);
            return o_NvAPI_Vulkan_SetLatencyMarker(vkDevice, pSetLatencyMarkerParams);
        }
    }

    State::Instance().reflexFrameId = pSetLatencyMarkerParams->frameID;

    return o_NvAPI_Vulkan_SetLatencyMarker(vkDevice, pSetLatencyMarkerParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_Vulkan_SetSleepMode(HANDLE vkDevice,
                                                      NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif
    std::scoped_lock lock(_sleepModeMutex);

    if (isGameCall(_ReturnAddress()))
    {
        _gameCalledReflex = true;
        _gameCalledSetSleepMode = true;
    }

    // Our own calls: as in hkNvAPI_D3D_SetSleepMode, not stored (update() would see a Vulkan sleep device and hand the
    // cap to Reflex); they carry the cap themselves when the low latency keeps it.
    if (_ownCall)
        return o_NvAPI_Vulkan_SetSleepMode(vkDevice, pSetSleepModeParams);

    // A bridged game: the mode goes to XeFG's XeLL, the driver gets it with low latency off. Asked before the new mode
    // is stored: a change of owner hands the previous one over first.
    auto* xellDevice = vkForwardDevice();

    // Store for later so we can adjust the fps whenever we want
    memcpy(&_lastVkSleepParams, pSetSleepModeParams, sizeof(NV_VULKAN_SET_SLEEP_MODE_PARAMS));
    _lastVkSleepDev = vkDevice;

    if (xellDevice != nullptr)
        return forwardVkSleepMode(xellDevice, vkDevice, *pSetSleepModeParams);

    if (_minimumIntervalUs != 0)
        pSetSleepModeParams->minimumIntervalUs = (std::max<decltype(pSetSleepModeParams->minimumIntervalUs)>)(pSetSleepModeParams->minimumIntervalUs, _minimumIntervalUs);

    return o_NvAPI_Vulkan_SetSleepMode(vkDevice, pSetSleepModeParams);
}

NvAPI_Status ReflexHooks::hkNvAPI_Vulkan_Sleep(HANDLE vkDevice, NvU64 signalValue)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    if (isGameCall(_ReturnAddress()))
        _gameCalledReflex = true;

    // A bridged game: XeFG's XeLL sleeps here. The driver's Sleep (its mode is off, nothing to wait for) still runs
    // whatever XeLL said, it signals the semaphore the game waits on.
    if (!_ownCall)
    {
        if (auto* xellDevice = vkForwardDevice())
            xellSleep(xellDevice);
    }

    return o_NvAPI_Vulkan_Sleep(vkDevice, signalValue);
}

NvAPI_Status ReflexHooks::hkNvAPI_Vulkan_GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams)
{
#ifdef LOG_REFLEX_CALLS
    LOG_FUNC();
#endif

    // A bridged game: XeLL's report (fakenvapi's), copied field by field into the Vulkan layout
    if (!_ownCall)
    {
        if (auto* xellDevice = vkForwardDevice())
        {
            std::scoped_lock lock(_sleepModeMutex);

            // Not freed, like the others: static and big
            static NV_LATENCY_RESULT_PARAMS* d3dResults = new NV_LATENCY_RESULT_PARAMS();
            *d3dResults = {};
            d3dResults->version = NV_LATENCY_RESULT_PARAMS_VER;

            const auto result = nvapi_calls::NvAPI_D3D_GetLatency(xellDevice, d3dResults);

            if (result != NVAPI_OK)
                return result;

            for (size_t i = 0; i < std::size(d3dResults->frameReport); i++)
            {
                const auto& from = d3dResults->frameReport[i];
                auto& to = pGetLatencyParams->frameReport[i];

                to = {};
                to.frameID = from.frameID;
                to.inputSampleTime = from.inputSampleTime;
                to.simStartTime = from.simStartTime;
                to.simEndTime = from.simEndTime;
                to.renderSubmitStartTime = from.renderSubmitStartTime;
                to.renderSubmitEndTime = from.renderSubmitEndTime;
                to.presentStartTime = from.presentStartTime;
                to.presentEndTime = from.presentEndTime;
                to.driverStartTime = from.driverStartTime;
                to.driverEndTime = from.driverEndTime;
                to.osRenderQueueStartTime = from.osRenderQueueStartTime;
                to.osRenderQueueEndTime = from.osRenderQueueEndTime;
                to.gpuRenderStartTime = from.gpuRenderStartTime;
                to.gpuRenderEndTime = from.gpuRenderEndTime;
            }

            return NVAPI_OK;
        }
    }

    return o_NvAPI_Vulkan_GetLatency(vkDevice, pGetLatencyParams);
}

void ReflexHooks::hookReflex(PFN_NvApi_QueryInterface& queryInterface)
{
    if (!_inited)
    {
#ifdef _DEBUG
        LOG_FUNC();
#endif

        o_NvAPI_D3D_SetSleepMode = GET_INTERFACE(NvAPI_D3D_SetSleepMode, queryInterface);
        o_NvAPI_D3D_Sleep = GET_INTERFACE(NvAPI_D3D_Sleep, queryInterface);
        o_NvAPI_D3D_GetLatency = GET_INTERFACE(NvAPI_D3D_GetLatency, queryInterface);
        o_NvAPI_D3D_SetLatencyMarker = GET_INTERFACE(NvAPI_D3D_SetLatencyMarker, queryInterface);
        o_NvAPI_D3D12_SetAsyncFrameMarker = GET_INTERFACE(NvAPI_D3D12_SetAsyncFrameMarker, queryInterface);
        o_NvAPI_Vulkan_SetLatencyMarker = GET_INTERFACE(NvAPI_Vulkan_SetLatencyMarker, queryInterface);
        o_NvAPI_Vulkan_SetSleepMode = GET_INTERFACE(NvAPI_Vulkan_SetSleepMode, queryInterface);
        o_NvAPI_Vulkan_Sleep = GET_INTERFACE(NvAPI_Vulkan_Sleep, queryInterface);
        o_NvAPI_Vulkan_GetLatency = GET_INTERFACE(NvAPI_Vulkan_GetLatency, queryInterface);
        o_NvAPI_Vulkan_InitLowLatencyDevice = GET_INTERFACE(NvAPI_Vulkan_InitLowLatencyDevice, queryInterface);

        _inited = o_NvAPI_D3D_SetSleepMode && o_NvAPI_D3D_Sleep && o_NvAPI_D3D_GetLatency &&
                  o_NvAPI_D3D_SetLatencyMarker && o_NvAPI_D3D12_SetAsyncFrameMarker &&
                  o_NvAPI_Vulkan_SetLatencyMarker && o_NvAPI_Vulkan_SetSleepMode && o_NvAPI_Vulkan_Sleep &&
                  o_NvAPI_Vulkan_GetLatency;

        if (_inited)
            LOG_DEBUG("Inited Reflex hooks");
    }
}

uint8_t ReflexHooks::dlssgFrameCountToGenerate() { return _FgNumFramesToGenerate; }

void ReflexHooks::setDlssgFrameCount(uint8_t count) { _FgNumFramesToGenerate = count; }

bool ReflexHooks::isReflexHooked() { return _inited; }

void* ReflexHooks::getHookedReflex(unsigned int InterfaceId)
{
    if (InterfaceId == GET_ID(NvAPI_D3D_SetSleepMode) && o_NvAPI_D3D_SetSleepMode)
    {
        return &hkNvAPI_D3D_SetSleepMode;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_Sleep) && o_NvAPI_D3D_Sleep)
    {
        return &hkNvAPI_D3D_Sleep;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_GetLatency) && o_NvAPI_D3D_GetLatency)
    {
        return &hkNvAPI_D3D_GetLatency;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D_SetLatencyMarker) && o_NvAPI_D3D_SetLatencyMarker)
    {
        return &hkNvAPI_D3D_SetLatencyMarker;
    }
    if (InterfaceId == GET_ID(NvAPI_D3D12_SetAsyncFrameMarker) && o_NvAPI_D3D12_SetAsyncFrameMarker)
    {
        return &hkNvAPI_D3D12_SetAsyncFrameMarker;
    }
    if (InterfaceId == GET_ID(NvAPI_Vulkan_SetLatencyMarker) && o_NvAPI_Vulkan_SetLatencyMarker)
    {
        return &hkNvAPI_Vulkan_SetLatencyMarker;
    }
    if (InterfaceId == GET_ID(NvAPI_Vulkan_SetSleepMode) && o_NvAPI_Vulkan_SetSleepMode)
    {
        return &hkNvAPI_Vulkan_SetSleepMode;
    }
    if (InterfaceId == GET_ID(NvAPI_Vulkan_Sleep) && o_NvAPI_Vulkan_Sleep)
    {
        return &hkNvAPI_Vulkan_Sleep;
    }
    if (InterfaceId == GET_ID(NvAPI_Vulkan_GetLatency) && o_NvAPI_Vulkan_GetLatency)
    {
        return &hkNvAPI_Vulkan_GetLatency;
    }

    return nullptr;
}

#define UPDATE_TIMING_ENTRY(name, type)                                                                                \
    if (validTimingInterval(frameReport.name##StartTime, frameReport.name##EndTime))                                  \
    {                                                                                                                  \
        double name##Pos = (double) (frameReport.name##StartTime - start) / rangeUs;                                   \
        double name##Length = (double) (frameReport.name##EndTime - frameReport.name##StartTime) / rangeUs;            \
        timingData[TimingType::type] = TimingEntry { .position = name##Pos, .length = name##Length };                  \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        timingData[TimingType::type].reset();                                                                          \
    }

bool ReflexHooks::updateTimingData(TimingReportSnapshot& snapshot)
{
    // Only the polling UI owns this cache; no marker thread can mutate it.
    snapshot = {};
    if (!_inited)
        return false;

    auto processFrameReport = [&](const auto& frameReport, IUnknown* reportDevice = nullptr) -> bool
    {
        TimingReportSnapshot report {};
        auto& timingData = report.timingData;
        // Missing endpoints are zero, not timestamps. Use the same complete-pair
        // rule for the graph range, individual entries and raw durations.
        const auto validTimingInterval = [](uint64_t begin, uint64_t finish) {
            return begin != 0 && finish != 0 && finish >= begin;
        };
        uint64_t start = UINT64_MAX;
        uint64_t end = 0;

        // Please don't look, just thought it would be least work
        auto pTimes = (const uint64_t*) &frameReport.simStartTime;
        for (auto i = 0; i < 12; i += 2)
        {
            if (!validTimingInterval(pTimes[i], pTimes[i + 1]))
                continue;

            if (pTimes[i] < start)
                start = pTimes[i];

            if (pTimes[i + 1] > end)
                end = pTimes[i + 1];
        }

        if (start == UINT64_MAX || end <= start)
            return false;

        // Monitor-only freshness: fgLastFrame does not advance with FG off.
        // Repeated driver reports must not keep an old timing bar alive forever.
        static uint64_t lastReportFrame=0,lastReportStart=0,lastReportEnd=0;
        static ULONGLONG lastReportChange=0;
        const auto reportTick=GetTickCount64();
        if(frameReport.frameID!=lastReportFrame || start!=lastReportStart || end!=lastReportEnd){
            lastReportFrame=frameReport.frameID;lastReportStart=start;lastReportEnd=end;lastReportChange=reportTick;
        }
        if(reportTick-lastReportChange>2000)
            return false;
        const auto anchor = GpuTimeline::Anchor(frameReport.frameID, reportDevice);
        const bool calibrated = GpuTimeline::Enabled() && !_timingEstimated.load(std::memory_order_relaxed) &&
            anchor.frame && frameReport.renderSubmitStartTime != 0;
        std::array<std::optional<std::pair<double, double>>, 2> gpuRanges{};
        if (calibrated)
        {
            const double markerQpc = (anchor.beginUs + anchor.endUs) * 0.5;
            report.clockUncertaintyUs = (anchor.endUs - anchor.beginUs) * 0.5;
            const double offset = double(frameReport.renderSubmitStartTime) - markerQpc;
            for (unsigned i = 0; i < gpuRanges.size(); ++i)
            {
                const auto span = GpuTimeline::Read(static_cast<GpuTimeline::Stream>(i), frameReport.frameID);
                if (!span.frame || span.device != anchor.device || std::abs(span.beginUs - markerQpc) > 2e6) continue;
                const double begin = span.beginUs + offset, finish = span.endUs + offset;
                if (begin <= 0.0 || finish < begin || finish >= double(UINT64_MAX)) continue;
                gpuRanges[i] = std::pair{begin, finish};
                start = std::min(start, static_cast<uint64_t>(std::floor(begin)));
                end = std::max(end, static_cast<uint64_t>(std::ceil(finish)));
            }
        }
        double rangeUs = static_cast<double>(end - start);
        for (unsigned i = 0; i < gpuRanges.size(); ++i)
            if (gpuRanges[i])
            {
                const auto [begin, finish] = *gpuRanges[i];
                TimingEntry entry{ .position = (begin - double(start)) / rangeUs, .length = (finish - begin) / rangeUs };
                if (i == 0) { report.bridgeGpu = entry; report.bridgeGpuMs = (finish - begin) / 1000.0; }
                else { report.nrGpu = entry; report.nrGpuMs = (finish - begin) / 1000.0; }
            }


        report.frameId = frameReport.frameID;
        report.estimated = _timingEstimated.load(std::memory_order_relaxed);
        // The current D3D SDK declares this field; the Vulkan report does not.
        // Never interpret reserved bytes or fakenvapi's synthetic GPU durations.
        if constexpr (requires { frameReport.gpuActiveRenderTimeUs; })
        {
            if (!report.estimated && frameReport.gpuActiveRenderTimeUs != 0)
                report.gpuActiveRenderTimeUs = frameReport.gpuActiveRenderTimeUs;
        }

        static std::atomic<ULONGLONG> nextRawReport{0};const auto rawTick=GetTickCount64();
        if(rawTick>=nextRawReport.load(std::memory_order_relaxed)){
            nextRawReport.store(rawTick+500,std::memory_order_relaxed);
            const auto duration = [&](const uint64_t* times, size_t index) {
                if (!validTimingInterval(times[index], times[index + 1]))
                    return -1.0;
                return static_cast<double>(times[index + 1] - times[index]) / 1000.0;
            };
            LOG_INFO("R858 Reflex raw: pid={} frame={} range_ms={:.4f} submit_ms={:.4f} present_ms={:.4f} driver_ms={:.4f} queue_ms={:.4f} gpu_ms={:.4f} estimated={} raw_units=us gpu_active_ms={:.4f} gpu_active_valid={} report_frame_valid={} report_begin_us={} report_end_us={} queue_valid={} queue_begin_us={} queue_end_us={} gpu_valid={} gpu_begin_us={} gpu_end_us={}",
                GetCurrentProcessId(),report.frameId,rangeUs/1000.0,duration(pTimes,2),duration(pTimes,4),
                duration(pTimes,6),duration(pTimes,8),duration(pTimes,10),report.estimated,
                report.gpuActiveRenderTimeUs ? static_cast<double>(*report.gpuActiveRenderTimeUs)/1000.0 : -1.0,
                report.gpuActiveRenderTimeUs.has_value(),report.frameId!=0,start,end,
                validTimingInterval(frameReport.osRenderQueueStartTime,frameReport.osRenderQueueEndTime),
                frameReport.osRenderQueueStartTime,frameReport.osRenderQueueEndTime,
                validTimingInterval(frameReport.gpuRenderStartTime,frameReport.gpuRenderEndTime),
                frameReport.gpuRenderStartTime,frameReport.gpuRenderEndTime);
        }
        timingData[TimingType::TimeRange] = TimingEntry { .position = 0, .length = rangeUs };
        UPDATE_TIMING_ENTRY(sim, Simulation)
        UPDATE_TIMING_ENTRY(renderSubmit, RenderSubmit)
        UPDATE_TIMING_ENTRY(present, Present)
        UPDATE_TIMING_ENTRY(driver, Driver)
        UPDATE_TIMING_ENTRY(osRenderQueue, OsRenderQueue)
        UPDATE_TIMING_ENTRY(gpuRender, GpuRender)

        if (frameReport.frameID != 0)
            State::Instance().reflexFrameId = frameReport.frameID;

        // Publish all timings, their report ID and provenance together before returning to the UI.
        snapshot = report;
        return true;
    };

    std::scoped_lock lock(_sleepModeMutex);

    // The game's Reflex device, else Optical F5Low's own Sleep device
    // A marker-only game can provide timing with FG off too. Read its existing
    // device under the same lifetime lock; this does not enable Sleep or FG.
    auto* markersDev = _lastFrameId[SIMULATION_START].load() != 0 ? _lastDev[SIMULATION_START].load() : nullptr;

    if (auto sleepDev =
            _lastSleepDev != nullptr ? _lastSleepDev : (_ownSleepDev != nullptr ? _ownSleepDev : markersDev);
        sleepDev && o_NvAPI_D3D_GetLatency)
    {
        // With XeFG, hkNvAPI_D3D_GetLatency answers from XeFG's XeLL through fakenvapi
        _timingEstimated = XefgOutput() || fakenvapi::isUsingAsMainNvapi();

        // Not calling free on this but it's static so hopefully fine
        static NV_LATENCY_RESULT_PARAMS* results = new NV_LATENCY_RESULT_PARAMS();
        results->version = NV_LATENCY_RESULT_PARAMS_VER;

        if (auto result = hkNvAPI_D3D_GetLatency(sleepDev, results); result != NVAPI_OK)
        {
            LOG_WARN("NvAPI_D3D_GetLatency failed: {}", magic_enum::enum_name(result));
            return false;
        }

        // Choose the completed sample's actual report, not an unrelated newest frame.
        if (GpuTimeline::Enabled() && !_timingEstimated.load(std::memory_order_relaxed))
        {
            // Prefer a complete shared sample over a newer NR-only report:
            // bridge queries are intentionally sparse (at most 10 per second).
            int selected = -1;
            unsigned bestScore = 0;
            for (int i = 63; i >= 0; --i)
            {
                const auto frame = results->frameReport[i].frameID;
                const auto anchor = GpuTimeline::Anchor(frame, sleepDev);
                if (!anchor.frame) continue;
                const auto bridge = GpuTimeline::Read(GpuTimeline::Stream::Bridge, frame);
                const auto nr = GpuTimeline::Read(GpuTimeline::Stream::Nr, frame);
                const unsigned score = (bridge.frame && bridge.device == anchor.device ? 2u : 0u) +
                    (nr.frame && nr.device == anchor.device ? 1u : 0u);
                if (score > bestScore) { bestScore = score; selected = i; }
                if (score == 3) break;
            }
            if (selected >= 0 && processFrameReport(results->frameReport[selected], sleepDev)) return true;
        }
        return processFrameReport(results->frameReport[63], sleepDev);
    }

    if (_lastVkSleepDev && o_NvAPI_Vulkan_GetLatency)
    {
        // Not calling free on this but it's static so hopefully fine
        static NV_VULKAN_LATENCY_RESULT_PARAMS* results = new NV_VULKAN_LATENCY_RESULT_PARAMS();
        results->version = NV_VULKAN_LATENCY_RESULT_PARAMS_VER;

        const auto result = hkNvAPI_Vulkan_GetLatency(_lastVkSleepDev, results);

        // Bridged, hkNvAPI_Vulkan_GetLatency answered from XeFG's XeLL through fakenvapi (asked after the call: it
        // hands the game's Reflex over when the owner changed)
        _timingEstimated = bridgedReflexForwarding() || fakenvapi::isUsingAsMainNvapi();

        if (result != NVAPI_OK)
        {
            LOG_WARN("NvAPI_Vulkan_GetLatency failed: {}", magic_enum::enum_name(result));
            return false;
        }

        // 64th element has the latest data
        return processFrameReport(results->frameReport[63]);
    }

    return false;
}

// For updating information about Reflex hooks
void ReflexHooks::update(bool fgActive, bool isVulkan)
{
    if (State::Instance().externalFrameGeneration)
    {
        State::Instance().reflexLimitsFps = false;
        State::Instance().reflexShowWarning = false;
        return;
    }
    // We can still use just the markers to limit the fps with Reflex disabled
    // But need to fallback in case a game stops sending them for some reason
    _updatesWithoutMarker++;

    State::Instance().reflexShowWarning = false;

    // Only the game's own Reflex calls say it is forwarded (vkForwardDevice, VK_NV_low_latency2); with no bridge to
    // forward to it no longer is, whenever the game calls next
    auto* bridgedXell = isVulkan ? bridgedXellDevice() : nullptr;

    if (bridgedXell == nullptr)
        noteBridgedForwarding(false);

    // Reject before reading provider metadata or waiting for continuity.
    // This existing path only releases our cap; it has no FG-input work.
    if (_updatesWithoutMarker > 20 || !_inited)
    {
        State::Instance().reflexLimitsFps = false;
        // No marker owner remains to update the old divided cap. Release only
        // our override; setFPSLimit restores the game's recorded interval.
        if (_minimumIntervalUs != 0 || _fpsLimitRetry.load(std::memory_order_relaxed))
            setFPSLimit(0);
        return;
    }

    // Keep provider access inside its existing execution/lifetime contract.
    // Lifecycle takes DLSSG's continuity lock without adding work depth; only
    // scalars escape, before any setFPSLimit/NVAPI call or driver pacing wait.
    const auto fgOutput = State::Instance().activeFgOutput;
    bool hasFgProvider = false;
    UINT appliedFramesToGenerate = 0;
    if (auto* fg = State::Instance().currentFG; fg != nullptr)
    {
        hasFgProvider = true;
        // An Off/caller-inactive cap update needs no provider metadata. In
        // particular, do not wait for a retained provider's continuity lock
        // before restoring the undivided cap below. This is bookkeeping only;
        // real Present/submission/teardown keep their execution guards.
        fgActive = fgActive && Config::Instance()->FGEnabled.value_or_default();
        if (fgActive)
        {
            auto pacingExecution = fg->GuardExecution(FGExecutionScope::Lifecycle);
            fgActive = Config::Instance()->FGEnabled.value_or_default() && fg->IsActive() && !fg->IsPaused();
            if (fgActive && fgOutput == FGOutput::DLSSG)
                appliedFramesToGenerate = fg->GetInterpolatedFrameCount();
        }
    }

    if (bridgedXell != nullptr && (_lastVkSleepDev || _lastLl2ModeSet || gameIsSendingMarkers()))
    {
        // XeFG's XeLL holds the cap, with the game's sleep mode or one we send (setFPSLimit) once markers come, as on
        // D3D; while it cannot (XellHoldsCap) OptiScaler's limiter does. The Vulkan present and the bridge's D3D12
        // present both come here: the answer must not depend on fgActive, or it flips every frame.
        State::Instance().reflexLimitsFps = XellHoldsCap();
    }
    else if (isVulkan && _lastVkSleepDev)
    {
        // fgActive doesn't matter for vulkan
        // isUsingAsMainNvapi() because fakenvapi might override the reflex' setting and we don't know it
        State::Instance().reflexLimitsFps = fakenvapi::isUsingAsMainNvapi() || _lastVkSleepParams.bLowLatencyMode;
    }
    else if (!isVulkan && XefgOutput() && (_lastSleepDev || gameIsSendingMarkers()))
    {
        // The game's Reflex goes to XeFG's XeLL with low latency on (SleepModeForOutput), with a mode from us when it
        // never sets one: XeLL holds the cap while it can (XellHoldsCap), OptiScaler's limiter otherwise.
        State::Instance().reflexLimitsFps = XellHoldsCap();
    }
    else if (_lastSleepDev)
    {
        // Don't use when: Real Reflex markers + FSR FG + Reflex disabled, causes huge input latency
        State::Instance().reflexLimitsFps =
            fakenvapi::isUsingAsMainNvapi() || !fgActive || _lastSleepParams.bLowLatencyMode;
        State::Instance().reflexShowWarning = State::Instance().activeFgOutput == FGOutput::FSRFG &&
                                              !fakenvapi::isUsingAsMainNvapi() && fgActive &&
                                              _lastSleepParams.bLowLatencyMode;
    }
    else
    {
        State::Instance().reflexLimitsFps = false;
    }

    // Disable reflex fps limiting when reflex is force disabled
    // TODO: It changes without pressing apply, XeLL with XeFG can't actually be disabled so this is wrong
    //
    // Wait for fakenvapi to be merged into Opti for better integration
    //
    // if (State::Instance().reflexLimitsFps && (fakenvapi::isUsingAsMainNvapi() ||
    // fakenvapi::isUsingOnNvidia())
    // &&
    //    Config::Instance()->FN_ForceReflex.value_or_default() == 1)
    //{
    //    State::Instance().reflexLimitsFps = false;
    //}

    static float lastFps = 0;
    static bool lastReflexLimitsFps = State::Instance().reflexLimitsFps;
    static LowLatencyMode lastLowLatencyMode = fakenvapi::getCurrentMode();

    // Reset required when toggling Reflex
    const bool reflexLimitsFps = State::Instance().reflexLimitsFps;
    if (reflexLimitsFps != lastReflexLimitsFps)
    {
        if (!setFPSLimit(0)) return; // Keep the transition pending after a rejected release.
        lastReflexLimitsFps = reflexLimitsFps;
        lastFps = 0;
    }
    else if (fakenvapi::isUsingAsMainNvapi() && lastLowLatencyMode != fakenvapi::getCurrentMode())
    {
        const auto mode = fakenvapi::getCurrentMode();
        if (!setFPSLimit(0)) return;
        lastLowLatencyMode = mode;
        lastFps = 0;
    }

    // With XeFG, a game that only marks frames gets its sleep mode from setFPSLimit alone: once more when its markers
    // start, or with no cap change and no change of the cap's owner XeLL would never get one and never sleep
    static bool lastXefgMarkers = false;

    if (const bool xefgMarkers = XefgOutput() && gameIsSendingMarkers(); xefgMarkers != lastXefgMarkers)
    {
        if (xefgMarkers && !setFPSLimit(lastFps)) return;
        lastXefgMarkers = xefgMarkers;
    }

    if (!State::Instance().reflexLimitsFps)
        return;

    float currentFps = Config::Instance()->FramerateLimit.value_or_default();
    // Keep native-game detection's existing compatibility policy separate
    // from the provider count used solely for the cap calculation below.
    static uint8_t lastFgNumFramesToGenerate = 0;

    if (lastFgNumFramesToGenerate != _FgNumFramesToGenerate)
    {
        if (fakenvapi::isUsingAsMainNvapi())
        {
            // Don't reload when using Dynamic MFG
            if (State::Instance().dlssgLastSetMode != sl::DLSSGMode::eDynamic &&
                !Config::Instance()->FGDLSSGForceDMFG.value_or_default())
            {
                State::Instance().fakenvapiReloadLowLatency = true;
            }

            // fakenvapi's latency techs fall apart with more than 1 fake frame
            if (Config::Instance()->FN_ForceReflex.value_or_default() != ForceReflex::ForceEnable &&
                (State::Instance().dlssgLastSetMode == sl::DLSSGMode::eDynamic || _FgNumFramesToGenerate > 1))
            {
                Config::Instance()->FN_ForceReflex.set_volatile_value(ForceReflex::ForceDisable);
            }
        }

        lastFgNumFramesToGenerate = _FgNumFramesToGenerate;

        if (_FgNumFramesToGenerate == 0)
            LOG_DEBUG("DLSS FG no longer detected");
        else
            LOG_DEBUG("DLSS FG detected, mode: {}x", _FgNumFramesToGenerate + 1);
    }

    // Native Evaluate reports the game's count. Our DLSSG provider must use
    // its applied count, never that stale report or the saved request while
    // disabled/paused/recreating. Other outputs retain their own cap policy.
    const UINT framesToGenerate = hasFgProvider ? appliedFramesToGenerate : _FgNumFramesToGenerate;
    if (fgActive && fgOutput == FGOutput::FSRFG)
    {
        currentFps /= 2;
    }
    else if (framesToGenerate > 0 && fakenvapi::isUsingAsMainNvapi() &&
             fakenvapi::getCurrentMode() != LowLatencyMode::XeLL)
    {
        currentFps /= (static_cast<float>(framesToGenerate) + 1.0f);
    }

    // A no-marker return may have released the override since lastFps was
    // recorded. Reacquiring that owner must resend even an unchanged cap.
    const auto currentInterval = currentFps <= 0.0f ? 0u
        : static_cast<uint32_t>(std::round(1'000'000 / currentFps));
    if (currentFps != lastFps || currentInterval != _minimumIntervalUs ||
        _fpsLimitRetry.load(std::memory_order_relaxed))
    {
        if (setFPSLimit(currentFps))
            lastFps = currentFps;
    }
}

// 0 - disables the fps cap
bool ReflexHooks::setFPSLimit(float fps)
{
    if (State::Instance().externalFrameGeneration)
        return false;
    const auto requestedIntervalUs = fps == 0.0f ? 0u
        : static_cast<uint32_t>(std::round(1'000'000 / fps));

    std::scoped_lock lock(_sleepModeMutex);

    bool submitted = false;
    NvAPI_Status result = NVAPI_OK;
    const char* resultApi = "none";
    uint32_t submittedIntervalUs = 0;
    const auto observe = [&](const char* api, uint32_t intervalUs, NvAPI_Status status) {
        submitted = true;
        // Preserve the first refusal; otherwise report the last accepted endpoint.
        if (result == NVAPI_OK)
        {
            result = status;
            resultApi = api;
            submittedIntervalUs = intervalUs;
        }
    };

    if (_lastSleepDev != nullptr)
    {
        NV_SET_SLEEP_MODE_PARAMS temp {};
        memcpy(&temp, &_lastSleepParams, sizeof(NV_SET_SLEEP_MODE_PARAMS));
        // Removing our override restores the independent upstream interval.
        temp.minimumIntervalUs = (std::max<decltype(temp.minimumIntervalUs)>)(temp.minimumIntervalUs, requestedIntervalUs);

        if (XefgOutput())
        {
            temp = ForOutput(temp);
            const auto intervalUs = temp.minimumIntervalUs;
            const auto status = nvapi_calls::NvAPI_D3D_SetSleepMode(_lastSleepDev, &temp);
            observe("D3D/XeLL", intervalUs, status);
        }
        else
        {
            const auto intervalUs = temp.minimumIntervalUs;
            const auto status = o_NvAPI_D3D_SetSleepMode(_lastSleepDev, &temp);
            observe("D3D", intervalUs, status);
        }
    }
    else if (auto* markerDev = _lastDev[SIMULATION_START].load();
             XefgOutput() && markerDev != nullptr && gameIsSendingMarkers() && bridgedXellDevice() == nullptr)
    {
        // A D3D game that marks frames but never sets a sleep mode: XeLL gets one from us, or it never sleeps
        NV_SET_SLEEP_MODE_PARAMS temp {};
        temp.version = NV_SET_SLEEP_MODE_PARAMS_VER;
        temp.minimumIntervalUs = requestedIntervalUs;
        temp = ForOutput(temp);
        const auto intervalUs = temp.minimumIntervalUs;
        const auto status = nvapi_calls::NvAPI_D3D_SetSleepMode(markerDev, &temp);
        observe("D3D/markers", intervalUs, status);
    }

    // A bridged Vulkan game: retain the existing owner hand-over before submitting its cap.
    auto* xellDevice = _lastVkSleepDev != nullptr ? vkForwardDevice() : bridgedXellDevice();

    if (xellDevice != nullptr)
    {
        native::xefgreflex::SleepMode mode;

        if (_lastVkSleepDev != nullptr)
            mode = ToMode(_lastVkSleepParams);
        else if (_lastLl2ModeSet)
            mode = _lastLl2Mode;
        else
            mode.lowLatency = true;

        if (_lastVkSleepDev != nullptr || _lastLl2ModeSet || gameIsSendingMarkers())
        {
            const auto intervalUs = (std::max)(mode.minimumIntervalUs, requestedIntervalUs);
            auto temp = XellSleepModeParams(mode, intervalUs);
            const auto submittedUs = temp.minimumIntervalUs;
            const auto status = nvapi_calls::NvAPI_D3D_SetSleepMode(xellDevice, &temp);
            observe("bridged-XeLL", submittedUs, status);
        }
    }
    else if (_lastVkSleepDev != nullptr)
    {
        NV_VULKAN_SET_SLEEP_MODE_PARAMS temp {};
        memcpy(&temp, &_lastVkSleepParams, sizeof(NV_VULKAN_SET_SLEEP_MODE_PARAMS));
        temp.minimumIntervalUs = (std::max<decltype(temp.minimumIntervalUs)>)(temp.minimumIntervalUs, requestedIntervalUs);
        const auto intervalUs = temp.minimumIntervalUs;
        const auto status = o_NvAPI_Vulkan_SetSleepMode(_lastVkSleepDev, &temp);
        observe("Vulkan", intervalUs, status);
    }

    const bool accepted = submitted && result == NVAPI_OK;
    // Existing cap-change log, also once on failure/recovery. Repeated refusals
    // of the same request do no IO, even when the driver changes its error code.
    static bool logged = false, lastAccepted = false, lastHadSubmission = false;
    static uint32_t lastRequestedUs = 0, lastSubmittedUs = 0;
    if (!logged || requestedIntervalUs != lastRequestedUs || accepted != lastAccepted || submitted != lastHadSubmission ||
        (accepted && submittedIntervalUs != lastSubmittedUs))
    {
        logged = true;
        lastAccepted = accepted;
        lastHadSubmission = submitted;
        lastRequestedUs = requestedIntervalUs;
        lastSubmittedUs = submittedIntervalUs;
        if (accepted)
            LOG_INFO("Set FPS Limit to: {} requested_interval_us={} result_api={} submitted_interval_us={} status={} accepted=true",
                     fps, requestedIntervalUs, resultApi, submittedIntervalUs, static_cast<int>(result));
        else if (submitted)
            LOG_WARN("Set FPS Limit to: {} requested_interval_us={} result_api={} submitted_interval_us={} status={} accepted=false; cache unchanged, retry on update",
                     fps, requestedIntervalUs, resultApi, submittedIntervalUs, static_cast<int>(result));
        else
            LOG_INFO("Set FPS Limit pending: {} requested_interval_us={} submitted=false status=UNKNOWN accepted=false; no sleep-mode owner",
                     fps, requestedIntervalUs);
    }

    // A failure (or no endpoint) must not make an unchanged next update look applied.
    if (accepted)
        _minimumIntervalUs = requestedIntervalUs;
    // Partial submissions or a later return to the old target still need a retry.
    _fpsLimitRetry.store(!accepted, std::memory_order_relaxed);
    return accepted;
}

uint64_t ReflexHooks::lastMarkerFrameId(NV_LATENCY_MARKER_TYPE type)
{
    return (size_t) type < std::size(_lastFrameId) ? _lastFrameId[type].load(std::memory_order_relaxed) : 0;
}

uint32_t ReflexHooks::gameXellFrameId()
{
    if (!gameCalledReflex() || !gameIsSendingMarkers())
        return 0;

    if (const auto renderSubmit = lastMarkerFrameId(RENDERSUBMIT_START); renderSubmit != 0)
        return (uint32_t) renderSubmit;

    return (uint32_t) lastMarkerFrameId(SIMULATION_START);
}

bool ReflexHooks::gameIsSendingMarkers()
{
    return _lastMarkerFrame != 0 && ((State::Instance().fgLastFrame < _lastMarkerFrame) ||
                                     (State::Instance().fgLastFrame - _lastMarkerFrame) < 5);
}

bool ReflexHooks::ensureTable(PFN_NvApi_QueryInterface queryInterface)
{
    if (!_inited && queryInterface != nullptr)
        hookReflex(queryInterface);

    return _inited;
}

namespace
{
struct OwnCallScope
{
    explicit OwnCallScope(bool& flag) : _flag(flag) { _flag = true; }
    ~OwnCallScope() { _flag = false; }
    bool& _flag;
};
} // namespace

NvAPI_Status ReflexHooks::ownSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams)
{
    std::scoped_lock lock(_sleepModeMutex);

    // The game's own SetSleepMode is the last word, even one that came in just before ours. With XeFG so is any mode
    // its side set (Streamline's calls are not the game's module) or that we sent for a game that only marks frames
    // (setFPSLimit): our stop would turn XeLL's low latency and cap off under it.
    if (_gameCalledSetSleepMode || (XefgOutput() && (_lastSleepDev != nullptr || gameIsSendingMarkers())))
        return NVAPI_OK;

    OwnCallScope scope(_ownCall);
    return hkNvAPI_D3D_SetSleepMode(pDev, pParams);
}

// Streamline's own modules (sl.interposer, sl.reflex, sl.pcl, sl.dlss_g ...) and the DLSS frame generation module
static bool IsStreamlineModule(HMODULE module)
{
    if (module == nullptr)
        return false;

    static std::mutex cacheMutex;
    static std::unordered_map<HMODULE, bool> cache;

    std::scoped_lock lock(cacheMutex);

    if (auto it = cache.find(module); it != cache.end())
        return it->second;

    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(module, path, MAX_PATH);

    auto name = std::filesystem::path(path).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);

    const bool streamline = name.starts_with(L"sl.") || name == L"nvngx_dlssg.dll";
    cache[module] = streamline;
    return streamline;
}

bool ReflexHooks::isGameCall(void* returnAddress)
{
    if (_ownCall)
        return false;

    // A game that started Streamline itself may run its own Reflex through it (NBA 2K27): Streamline's calls are its
    if (State::Instance().gameCalledSlInit)
        return true;

    // Otherwise Streamline is the one OptiScaler loaded for its own DLSS frame generation output: its Reflex calls are
    // OptiScaler's, not the game's
    return !IsStreamlineModule(Util::GetCallerModule(returnAddress));
}

void ReflexHooks::forgetSleepDevice(IUnknown* pDev)
{
    std::scoped_lock lock(_sleepModeMutex);

    if (pDev != nullptr && _lastSleepDev == pDev)
        _lastSleepDev = nullptr;

    if (pDev != nullptr && _ownSleepDev == pDev)
        _ownSleepDev = nullptr;

    // The markers' devices too: GetLatency and setFPSLimit fall back to the last simulation start's (the next marker
    // puts a live one back)
    for (auto& dev : _lastDev)
    {
        if (auto* expected = pDev; expected != nullptr)
            dev.compare_exchange_strong(expected, nullptr);
    }
}

NvAPI_Status ReflexHooks::ownSleep(IUnknown* pDev)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_D3D_Sleep(pDev);
}

NvAPI_Status ReflexHooks::ownSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_D3D_SetLatencyMarker(pDev, pParams);
}

NvAPI_Status ReflexHooks::ownGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_D3D_GetLatency(pDev, pParams);
}

void ReflexHooks::forgetSleepDeviceVulkan(HANDLE vkDevice)
{
    std::scoped_lock lock(_sleepModeMutex);

    if (vkDevice != nullptr && _lastVkSleepDev == vkDevice)
        _lastVkSleepDev = nullptr;
}

NvAPI_Status ReflexHooks::ownSetSleepModeVulkan(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pParams)
{
    std::scoped_lock lock(_sleepModeMutex);

    // The game's own SetSleepMode is the last word, even one that came in just before ours
    if (_gameCalledSetSleepMode)
        return NVAPI_OK;

    OwnCallScope scope(_ownCall);
    return hkNvAPI_Vulkan_SetSleepMode(vkDevice, pParams);
}

NvAPI_Status ReflexHooks::ownSleepVulkan(HANDLE vkDevice, NvU64 signalValue)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_Vulkan_Sleep(vkDevice, signalValue);
}

NvAPI_Status ReflexHooks::ownSetLatencyMarkerVulkan(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* pParams)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_Vulkan_SetLatencyMarker(vkDevice, pParams);
}

NvAPI_Status ReflexHooks::ownGetLatencyVulkan(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pParams)
{
    OwnCallScope scope(_ownCall);
    return hkNvAPI_Vulkan_GetLatency(vkDevice, pParams);
}

NvAPI_Status ReflexHooks::ownInitLowLatencyDeviceVulkan(HANDLE vkDevice, HANDLE* signalSemaphore)
{
    if (o_NvAPI_Vulkan_InitLowLatencyDevice == nullptr)
        return NVAPI_NO_IMPLEMENTATION;

    return o_NvAPI_Vulkan_InitLowLatencyDevice(vkDevice, signalSemaphore);
}

void ReflexHooks::noteGameVulkanLowLatency2(bool setSleepMode)
{
    std::scoped_lock lock(_sleepModeMutex);

    _gameCalledReflex = true;

    if (setSleepMode)
        _gameCalledSetSleepMode = true;
}
