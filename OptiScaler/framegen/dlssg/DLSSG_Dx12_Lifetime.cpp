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

template<typename Options, typename Submit>
dlssg::options::Submission DLSSG_Dx12::SetReleaseOptions(const Options& options, Submit&& submit)
{
    if (!ModuleLifetime::CleanupAllowed()) return {};
    // Called under continuity, including after logical FG was turned off.
    // Do not require CanSubmitInputs/current source eligibility for release.
    const auto owner = CurrentProviderOwner();
    const auto inputEpoch = _inputOwnerEpoch;
    const auto selected = State::Instance().currentFG;
    const auto reflexPlugin = State::Instance().optiSlReflex;
    const auto reflexApi = StreamlineProxy::ReflexSetOptions();
    const auto current = [&] {
        return (!selected || selected == this) && selected == State::Instance().currentFG &&
            owner == CurrentProviderOwner() &&
            inputEpoch == _inputOwnerEpoch && reflexPlugin == State::Instance().optiSlReflex &&
            reflexApi == StreamlineProxy::ReflexSetOptions() && !_teardownQuarantined &&
            !g_dlssgTeardownQuarantined.load() && !_providerStateQueryActive && _pacingLifetime->Idle();
    };
    try
    {
        return _dispatchOptions.Release(options, current, std::forward<Submit>(submit));
    }
    catch (...)
    {
        InvalidateDispatchOptions();
        throw;
    }
}

bool DLSSG_Dx12::DisableRetainedProvider()
{
    _nativeGapOffProof.Invalidate();
    auto options = _lastRunningOptions;
    options.mode = sl::DLSSGMode::eOff;
    options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
    const auto setOptions = StreamlineProxy::DLSSGSetOptions();
    const auto outcome = SetReleaseOptions(options, [&] {
        return setOptions ? StreamlineHooks::SubmitDlssgOptions(setOptions,viewport,options) : sl::Result::eErrorMissingOrInvalidAPI;
    });
    const auto result = outcome.Result();
    _teardownOffAccepted = result == sl::Result::eOk;
    if (!_teardownOffAccepted)
        LOG_ERROR("F5 FG teardown: Off+retain failed, result {} ({}); submissions blocked",
            static_cast<int>(result), magic_enum::enum_name(result));
    return _teardownOffAccepted;
}

void DLSSG_Dx12::RetryReflexOff()
{
    if (_reflexOwnership != ReflexOwnership::ReleasePending ||
        State::Instance().currentFG != this) return; // Never disable a replacement provider's mode.
    const auto setOptions = StreamlineProxy::ReflexSetOptions();
    if (!setOptions)
    {
        if (!_reflexOffReported)
            LOG_WARN("F5 Genshin Reflex Off pending: SDK entry unavailable; retry at existing Present boundary");
        _reflexOffReported = true;
        return;
    }
    sl::ReflexOptions options {};
    options.mode = sl::ReflexMode::eOff;
    options.frameLimitUs = 0; // Release only this provider's SDK options, not the upstream NVAPI cap.
    options.useMarkersToOptimize = false;
    const auto outcome = SetReleaseOptions(options, [&] { return setOptions(options); });
    const auto result = outcome.Result();
    if (result != sl::Result::eOk)
    {
        if (!_reflexOffReported)
            LOG_WARN("F5 Genshin Reflex Off pending: SDK result {}; retry at existing Present boundary", (int)result);
        _reflexOffReported = true;
        return;
    }
    _reflexOwnership = ReflexOwnership::None;
    _reflexOffReported = false;
    LOG_INFO("F5 Genshin Reflex Off: SDK accepted mode=eOff frame_limit_us=0 markers=false; driver state UNKNOWN");
}

void DLSSG_Dx12::Deactivate()
{
    if (!ModuleLifetime::CleanupAllowed()) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    _pacingLifetime->CloseAdmission();
    const bool wasActive = _isActive || _waitingNewFrameData || _inputContinuity.Held();
    const bool releaseOwnerChanged = _deactivationOwner != CurrentProviderOwner() ||
        _deactivationInputEpoch != _inputOwnerEpoch;
    if (!_dispatchOptions.Deactivate(wasActive || _haveRunningOptions || releaseOwnerChanged, _deactivationRequested, [&] {
        InvalidateDispatchOptions();
        ++_inputOwnerEpoch;
        _teardownOffAccepted = false;
    }))
    {
        // A same-owner duplicate must neither advance input epoch nor revoke
        // an in-flight Off. Existing safe boundaries retry pending releases.
        return;
    }
    _deactivationOwner = CurrentProviderOwner();
    _deactivationInputEpoch = _inputOwnerEpoch;
    _nativeGapOffProof.Invalidate();
    _starRailDispatchPending = false;
    _starRailRecordingGeneration = 0;
    _starRailGapOffAccepted = false;
    _starRailF5DsMarker.store(UINT64_MAX, std::memory_order_release);
    _starRailUiBusy = _starRailUiGap = false;
    _inputContinuity.Clear();
    _haveRunningOptions = false;
    _isActive = false;
    // IsActive also includes the input-gap flag. Off/recreate must leave the
    // retained provider inactive, even without another upscaler frame.
    _waitingNewFrameData = false;
    if (_teardownQuarantined) return;
    // Publish every release intent before entering either SDK. If FG Off
    // throws, the next safe boundary must still know that Reflex needs Off.
    if (wasActive || _reflexOwnership != ReflexOwnership::None)
        _reflexOwnership = ReflexOwnership::ReleasePending;
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
            LOG_INFO("F5 FG teardown requested: Off+retain, owner {:X}, frame {}, off_accepted={}",
                (size_t)_gameCommandQueue, _frameCount, _teardownOffAccepted);
        }
    }
    else if (wasActive)
    {
        _unretainedOffPending = true;
        RetryUnretainedProviderOff();
    }
    if (_reflexOwnership == ReflexOwnership::ReleasePending)
        RetryReflexOff();
}

void DLSSG_Dx12::RetryUnretainedProviderOff()
{
    if (!_unretainedOffPending) return;
    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOff;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    const auto setOptions = StreamlineProxy::DLSSGSetOptions();
    const auto outcome = SetReleaseOptions(options, [&] {
        return setOptions ? StreamlineHooks::SubmitDlssgOptions(setOptions,viewport,options) : sl::Result::eErrorMissingOrInvalidAPI;
    });
    if (outcome.confirmed) _unretainedOffPending = false;
}

bool DLSSG_Dx12::CompletePendingTeardown()
{
    if (!ModuleLifetime::CleanupAllowed()) return false;
    std::lock_guard continuityLock(_inputContinuityMutex);
    // A live Sleep owns CPU/SDK lifetime only. Leave its locks available and
    // retry Off/free at the next real boundary; never wait under continuity.
    if (!_pacingLifetime->Idle())
        return !_teardownPending && !_unretainedOffPending &&
            _reflexOwnership!=ReflexOwnership::ReleasePending && !_hookRetiring && !_ownerChanging;
    if (_teardownQuarantined || g_dlssgTeardownQuarantined.load() || _teardownCompleting ||
        _dispatchOptions.InFlight() || _providerStateQueryActive) return false;
    _teardownCompleting = true;
    struct FinishCompletion { bool& active; ~FinishCompletion() { active = false; } } finish { _teardownCompleting };
    // SDK mode release can still be pending after GPU retirement is complete.
    // Its result never grants resource retirement or blocks the existing safe drain.
    RetryReflexOff();
    RetryUnretainedProviderOff();
    if (!_teardownPending) return !_unretainedOffPending;
    // The real Present hook calls this after FinishWork, including skipped
    // frames. Pending zero receipts cannot depend on another authenticated frame.
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
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice, fenceDevice;
        if (_teardownFenceValue >= UINT64_MAX - 1 || drain.device->GetDeviceRemovedReason() != S_OK ||
            drain.queue->GetDevice(IID_PPV_ARGS(&queueDevice)) != S_OK ||
            drain.fence->GetDevice(IID_PPV_ARGS(&fenceDevice)) != S_OK ||
            !D3D12DeviceIdentity::Same(queueDevice.Get(), drain.device.Get()) ||
            !D3D12DeviceIdentity::Same(fenceDevice.Get(), drain.device.Get()))
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
                    !dlssg::depth_contract::SameOwner(work.device.Get(), drain.device.Get())))
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
                    const auto callbacks=DlssNr::FinishedPictureSubmissionCallbacks();
                    const auto previewReceipt=DlssNr::FinishedPictureSubmitting(drain.queue.Get(),1,
                        (ID3D12CommandList**)&_uiCommandList[i]);
                    drain.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&_uiCommandList[i]);
                    if(callbacks==DlssNr::FinishedPictureSubmissionCallbacks())
                        DlssNr::FinishedPictureSubmitted(drain.queue.Get(),1,
                            (ID3D12CommandList**)&_uiCommandList[i],previewReceipt);
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
        LOG_INFO("F6 FG teardown host drain submitted: queue={:X} fence={:X} target={} asynchronous={}; retirement pending",
            reinterpret_cast<uintptr_t>(drain.queue.Get()), reinterpret_cast<uintptr_t>(drain.fence.Get()),
            drain.value, false);
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
    if (drain.phase != TeardownDrain::Phase::HostComplete)
        LOG_INFO("F6 FG teardown host drain complete: queue={:X} fence={:X} target={} completed={}; entering SDK FreeResources, SDK release pending",
            reinterpret_cast<uintptr_t>(drain.queue.Get()), reinterpret_cast<uintptr_t>(drain.fence.Get()),
            drain.value, completed);
    drain.phase = TeardownDrain::Phase::HostComplete;
    const auto waitValue = drain.value;
    // NVIDIA official sample's public host-queue contract. This is not a claim
    // that private SL worker queues are exposed or individually waited by F5.
    InvalidateProviderCapabilities(); // An SDK resource generation never inherits configuration/capability.
    const auto freeResult = StreamlineProxy::FreeResources()(sl::kFeatureDLSS_G, viewport);
    if (_teardownQuarantined) return false;
    if (freeResult != sl::Result::eOk)
    {
        LOG_ERROR("F5 FG teardown: slFreeResources failed {}, owner blocked", (int)freeResult);
        QuarantineFailedTeardown();return false;
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
        _upscalerColourSources[i].reset();_upscalerFinalColours[i].reset();
        _upscalerColourSucceeded[i]=false;_upscalerPendingHudless[i]={};
    }
    _guideContractOwned=false;
    for (auto& frame:_nativeGuideFrames) frame=nullptr;
    _nativeGuideRecordings.clear(); // Every recording already passed its own Reset/fence above.
    _nativeGuideListInterest.Clear();
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
    if (!ModuleLifetime::CleanupAllowed()) return false;
    if (CompletePendingTeardown()) return true;
    if (_teardownCompleting || _teardownQuarantined || g_dlssgTeardownQuarantined.load() ||
        _inputRecordingDepth || _presentExecutionDepth || !_teardownPending || !WaitForTeardownTarget()) return false;
    return CompletePendingTeardown(); // Same target, no new Execute/Signal; not wake==completion.
}

FGHookRetirementResult DLSSG_Dx12::CompletePendingTeardownForHook(bool terminal)
{
    using Result = FGHookRetirementState;
    if (!ModuleLifetime::CleanupAllowed()) return { Result::Refused, DXGI_ERROR_WAS_STILL_DRAWING };
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
    if (!ModuleLifetime::CleanupAllowed()) return false;
    std::lock_guard continuityLock(_inputContinuityMutex);
    _pacingLifetime->CloseAdmission();
    if (!_pacingLifetime->Idle()) return false;
    InvalidateProviderCapabilities();
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
    _pacingLifetime->CloseAdmission();
    if (_teardownQuarantined) return;
    InvalidateProviderCapabilities();
    _teardownQuarantined = true;
    g_dlssgTeardownQuarantined.store(true);
    _isActive = false;
    _waitingNewFrameData = false;
    _teardownPending = true;
    // Preserve the preallocated graph without callbacks or a raw provider pointer.
    // Failed Shutdown must not let member destructors release pending guides,
    // UI masks, input resources, or their independent queue/fence timelines.
    auto* retained = _teardownDrain.release();
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
        _dx11GuideInputs[i][0].Detach(); _dx11GuideInputs[i][1].Detach();
        _guideDepthScale[i].release(); // Preserve descriptors/constants/buffer after uncertain submission.
        _guideFlip[i][0].release();_guideFlip[i][1].release();
    }
    // Preserve all live recording owners, including those no longer referenced
    // by a presentation slot. Their destructors must not release uncertain GPU state.
    for (auto& recording:_nativeGuideRecordings) recording->quarantineSelf=recording;
    _nativeGuideRecordings.clear(); // Intentional self-owners preserve the complete GPU graph.
    for (auto& frame:_nativeGuideFrames) frame=nullptr;
    _renderUI.release(); _hudlessCompare.release(); _mvFlip.release(); _depthFlip.release();
    _uiFence = nullptr; _scFence = nullptr;
    _teardownFence = nullptr; _teardownEvent = nullptr;
    LOG_ERROR("F5 FG terminal teardown quarantined; provider references kept until process exit; restart required");
}

void DLSSG_Dx12::DestroyFGContext()
{
    if (!ModuleLifetime::CleanupAllowed()) return;
    std::lock_guard continuityLock(_inputContinuityMutex);
    if (_teardownCompleting) return;
    Deactivate();
    if (!CompletePendingTeardown()) return;
    ReleaseObjects();
}

bool DLSSG_Dx12::Shutdown()
{
    if (!ModuleLifetime::CleanupAllowed()) return false;
    std::lock_guard continuityLock(_inputContinuityMutex);

    _pacingLifetime->CloseAdmission();
    if (!_pacingLifetime->Idle()) return false;
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

void DLSSG_Dx12::ReleaseObjects() { (void)ReleaseObjectsChecked(); }

bool DLSSG_Dx12::ReleaseObjectsChecked()
{
    if (!ModuleLifetime::CleanupAllowed()) return false;
    std::lock_guard continuityLock(_inputContinuityMutex);
    _pacingLifetime->CloseAdmission();
    if (!_pacingLifetime->Idle()) return false;
    InvalidateProviderCapabilities();
    if (_teardownCompleting) return false;
    if (_managedRetained || _inputContinuity.Held()) Deactivate();
    if (!CompletePendingTeardown()) return false;
    if (_zzzSCCompletion && !PollZzzSCWork()) return false;
    // Outstanding frame receipts keep their own actual owners, including a
    // failed pre-provider Wait/Signal. Cache release is never retirement proof.
    SAFE_RELEASE(_teardownFence);
    if (_teardownEvent) { CloseHandle(_teardownEvent); _teardownEvent = nullptr; }
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);
        SAFE_RELEASE(dlssgFence[i]);
        // Host drain and SDK free preceded release of these executable lists.
        _dx11GuideInputs[i][0].Reset(); _dx11GuideInputs[i][1].Reset();
        _dx11GuideBorrowed[i]=false; _dx11GuideSealedFrame[i]=0;
        _dx11GuideSubmissions[i]={};

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

FGHookRetirementReceipt DLSSG_Dx12::RetireSwapchainForHook(const FGHookRetirementRequest& request)
{
    if (!ModuleLifetime::CleanupAllowed()) return FGHookRetirementReceipt::Refused(DXGI_ERROR_WAS_STILL_DRAWING);
    std::unique_lock execution(_inputContinuityMutex, std::try_to_lock);
    const auto refused = [](HRESULT hr) { return FGHookRetirementReceipt::Refused(hr); };
    if (!execution.owns_lock() || _inputRecordingDepth || _presentExecutionDepth || _ownerChanging ||
        _hookRetirementActive || _teardownCompleting)
        return refused(DXGI_ERROR_WAS_STILL_DRAWING);
    if (_teardownQuarantined || g_dlssgTeardownQuarantined.load())
        return FGHookRetirementReceipt::Quarantined(DXGI_ERROR_WAS_STILL_DRAWING);
    if (!request.Known() || request.provider != static_cast<IFGFeature_Dx12*>(this) ||
        !_providerOwnerGeneration || _providerOwnerGeneration == UINT64_MAX || _inputOwnerEpoch >= UINT64_MAX - 2)
        return refused(DXGI_ERROR_INVALID_CALL);
    if (State::Instance().isShuttingDown) return refused(DXGI_ERROR_WAS_STILL_DRAWING);
    if (_hookRetiring && !_hookRetirementBinding.SameOwner(request)) return refused(DXGI_ERROR_INVALID_CALL);
    // Post-cleanup hook queue polling may cross calls, but a recursive_mutex
    // lease must not. Reissue only for this retired owner/serial/current epoch.
    if (_hookRetired)
    {
        if (_inputOwnerEpoch != _hookRetiredEpoch ||
            !dlssg::depth_contract::SameOwner(request.queue, _gameCommandQueue) || !D3D12DeviceIdentity::Same(request.device, _device))
            return refused(DXGI_ERROR_INVALID_CALL);
        return FGHookRetirementReceipt(request, _providerOwnerGeneration, _hookRetiredEpoch, &_inputOwnerEpoch,
                                       std::move(execution));
    }
    if (request.window != _hwnd || !dlssg::depth_contract::SameOwner(request.chain, _swapChain) ||
        !dlssg::depth_contract::SameOwner(request.queue, _gameCommandQueue) || !D3D12DeviceIdentity::Same(request.device, _device))
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
    _starRailGuidePresentQueue.Reset();_starRailGuideSwapchain=0;_starRailGuideQueueCurrent=false;
    _swapChain = nullptr; _hwnd = nullptr;
    ++_inputOwnerEpoch;
    _hookRetired = true;
    _hookRetiredEpoch = _inputOwnerEpoch;
    return FGHookRetirementReceipt(request, _providerOwnerGeneration, _inputOwnerEpoch, &_inputOwnerEpoch, std::move(execution));
}

bool DLSSG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (!ModuleLifetime::CleanupAllowed()) return false;
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
