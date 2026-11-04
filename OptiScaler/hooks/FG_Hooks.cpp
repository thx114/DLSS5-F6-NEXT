#include "pch.h"
#include <framegen/FinalOutputEndpoint.h>
#include <integration/HoYoShade.h>
#include <with_dx12/GenshinVisibleChild.h>
#include <dlssnr/DlssNr.h>
#include "FG_Hooks.h"
#include "FGQueueIdleGate.h"
#include <memory>
#include <mutex>
#include <utility>
#include <string_view>
#include <Config.h>
#include <genshin/GenshinFgOutcome.h>
#include <genshin/GenshinHookCpuTiming.h>
#include <inputs/FG/GenshinBridge_Present_Dx12.h>
#include <inputs/GenshinFsrContract.h>

#include <framegen/ffx/FSRFG_Dx12.h>
#include <framegen/xefg/XeFG_Dx12.h>
#include <framegen/dlssg/DLSSG_Dx12.h>

#include <inputs/FG/FSR3_Dx12_FG.h>
#include <inputs/FG/FfxApi_Dx12_FG.h>

#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_Dx12.h>

#include <misc/FrameLimit.h>

#include <misc/IdentifyGpu.h>
#include <misc/REFrameworkCompat.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <native/NativeDriverDx12.h>
#include <native/NativeLowLatency.h>
#include <native/PresentStageTiming.h>
#include <native/VkPresentBridge.h>

#include <d3d12.h>
#include <detours/detours.h>

static bool _forcedHdrForXeFG = false;
static HANDLE _semaphore = nullptr;

namespace
{
// Only these hook lifecycle operations use this mutex. They never wait for it while inside a provider.
// Recursive entry is needed for preserve-create -> ResizeBuffers and DXGI's ResizeBuffers1 forwarding.
// R654: Present-only TLS metadata accompanies the existing R448 sampled stages.
// Scalars only: no resource ownership, provider access, lock, allocation or log here.
struct FgPacingContext
{
    uint64_t presentFrame=0,guardFrame=0,providerFrame=0,presentEpoch=0,sourceSequence=0;
    bool providerKnown=false,epochKnown=false,active=false;
    uint32_t preparedGeneratedCount=0;
    float capFps=0.0f;
    bool reflexLimitsFps=false;
    int nativeOwnsCap=-1,xellCanLimit=-1; // -1: not evaluated by the real short-circuit branch
    const char* capOwner="UNKNOWN";
    const char* reason="UNKNOWN";
    GenshinHookCpuTiming::StarRailStamp attribution{};
};
struct FgPacingRow
{
    uint64_t decisions=0,called=0,skipped=0,samples=0,clockFailed=0,ticks=0,maxTicks=0;
    bool lastCalled=false,hasSample=false,hasMaximum=false,lastResultKnown=false,maxResultKnown=false;
    uint32_t lastResult=0,maxResult=0;
    FgPacingContext last{},sample{},maxContext{};
    GenshinHookCpuTiming::WallSample wall{},maxWall{};
    void Record(const FgPacingContext& context,bool invoked,GenshinHookCpuTiming::WallSample observed,
                uint32_t result=0,bool resultKnown=false) noexcept
    {
        ++decisions;if(invoked)++called;else ++skipped;last=context;lastCalled=invoked;
        lastResult=result;lastResultKnown=resultKnown;
        if(!observed.sampled)return;
        if(observed.begin<=0 || observed.end<observed.begin){++clockFailed;return;}
        ++samples;const auto span=uint64_t(observed.end-observed.begin);ticks+=span;
        if(!hasMaximum || maxTicks<span){
            maxTicks=span;maxContext=context;maxWall=observed;maxResult=result;
            maxResultKnown=resultKnown;hasMaximum=true;
        }
        sample=context;wall=observed;hasSample=true;
    }
};
struct FgPacingWindow
{
    uint64_t epoch=UINT64_MAX,drain=0;
    std::array<FgPacingRow,2> rows{};
    void Begin(uint64_t currentEpoch) noexcept
    {
        if(epoch==currentEpoch)return;epoch=currentEpoch;rows={};
    }
    void Emit(bool starRail=false) noexcept
    {
        try {
        using namespace GenshinHookCpuTiming;
        if(!ConsumePresentDrain(drain,epoch))return;
        const auto hz=frequency.load(std::memory_order_relaxed);
        const auto us=[&](uint64_t ticks){return hz>0?double(ticks)*1000000.0/double(hz):-1.0;};
        for(unsigned i=0;i<rows.size();++i)
        {
            const auto& r=rows[i];if(!r.decisions)continue;const auto& c=r.last;const auto& s=r.sample;
            LOG_INFO("R654 FG pacing: pid={} thread={} timing_epoch={} drain_ms={} type={} stride=64 decisions={} called={} skipped={} samples={} clock_failed={} wall_avg_us={} wall_sampled_max_us={} last_present_frame={} last_guard_frame={} last_provider_known={} last_provider_frame={} last_present_epoch_known={} last_present_epoch={} last_fg_active_guard={} last_prepared_generated_count={} last_cap_fps_observed={} last_reflex_limits_fps_at_call={} last_native_owns_cap_selection={} last_xell_can_limit_selection={} last_cap_owner_selection={} last_called={} last_reason={} sample_known={} sample_present_frame={} sample_guard_frame={} sample_provider_known={} sample_provider_frame={} sample_present_epoch_known={} sample_present_epoch={} sample_fg_active_guard={} sample_prepared_generated_count={} sample_cap_fps_observed={} sample_reflex_limits_fps_at_call={} sample_native_owns_cap_selection={} sample_xell_can_limit_selection={} sample_cap_owner_selection={} sample_reason={} sample_begin_qpc={} sample_end_qpc={} qpc_hz={} sample_wall_us={} cap_fps_inside_sleep=UNKNOWN game_minimum_interval_us=UNKNOWN project_override_us=UNKNOWN actual_blocked_us=UNKNOWN owner_scope=FGPresent_branch_selection counters=thread_timing_epoch_cumulative unknown_bool=-1 ids_require_known_bit=true wall_not_cpu_busy=true not_gpu_time=true inclusive_not_additive=true timing_overhead_included=true prepared_count_not_effective_multiplier=true cap_owner_phase={} game_scope={} last_source_sequence={} sample_source_sequence={} source_sequence_zero_is_unknown=true{} provider_epoch=UNKNOWN unqueried_fields=UNKNOWN",
                GetCurrentProcessId(),GetCurrentThreadId(),epoch,drain,i==0?"ReflexSleep":(starRail?"SdkPresent":"FrameLimitSleep"),
                r.decisions,r.called,r.skipped,r.samples,r.clockFailed,r.samples?us(r.ticks)/double(r.samples):-1.0,r.samples?us(r.maxTicks):-1.0,
                c.presentFrame,c.guardFrame,c.providerKnown,c.providerFrame,c.epochKnown,c.presentEpoch,c.active,c.preparedGeneratedCount,
                c.capFps,c.reflexLimitsFps,c.nativeOwnsCap,c.xellCanLimit,c.capOwner,r.lastCalled,c.reason,
                r.hasSample,s.presentFrame,s.guardFrame,s.providerKnown,s.providerFrame,s.epochKnown,s.presentEpoch,s.active,s.preparedGeneratedCount,
                r.hasSample?s.capFps:-1.0f,s.reflexLimitsFps,s.nativeOwnsCap,s.xellCanLimit,s.capOwner,s.reason,
                r.wall.begin,r.wall.end,hz,r.hasSample?us(uint64_t(r.wall.end-r.wall.begin)):-1.0,
                starRail?"not_queried":"after_reflex_before_frame_limit",
                starRail?"StarRail_F5Owned_Dx11wDx12_DLSSG":"Genshin",c.sourceSequence,s.sourceSequence,
                starRail?" last_guard_frame_known=false sample_guard_frame_known=false guard_frame_status=UNKNOWN guard_frame_zero_is_unknown=true":"");
            if(starRail){
                const auto& m=r.maxContext;const auto& a=m.attribution;const auto& parent=a.parent;
                LOG_INFO("R897 FG pacing maximum: pid={} thread={} timing_epoch={} type={} max_known={} max_begin_qpc={} max_end_qpc={} qpc_hz={} max_wall_us={} max_source_sequence={} max_present_frame={} max_provider_known={} max_provider_frame={} max_parent_known={} max_entry_qpc={} max_parent_pid={} max_parent_tid={} max_wrapper={:X} max_child={:X} max_chain={:X} max_sync={} max_flags={:X} max_result_known={} max_result_raw={:08X} last_source_sequence={} last_called={} last_result_known={} last_result_raw={:08X} maximum_context_is_same_sample=true last_is_independent=true wall_not_cpu_busy=true inclusive_not_additive=true sdk_private_fence=UNKNOWN",
                    GetCurrentProcessId(),GetCurrentThreadId(),epoch,i==0?"ReflexSleep":"SdkPresent",r.hasMaximum,
                    r.maxWall.begin,r.maxWall.end,hz,r.hasMaximum?us(r.maxTicks):-1.0,
                    m.sourceSequence,m.presentFrame,m.providerKnown,m.providerFrame,parent.known,parent.entryQpc,
                    parent.pid,parent.tid,parent.wrapper,parent.child,a.chain,a.sync,a.flags,
                    r.maxResultKnown,r.maxResult,c.sourceSequence,r.lastCalled,r.lastResultKnown,r.lastResult);
            }
        }
        } catch (...) {} // Timing/log failures never change Present or pacing.
    }
};
thread_local FgPacingWindow fgPacingWindow;
// R725 reuses R654 storage/aggregation; rows are Reflex and saved SDK Present.
thread_local FgPacingWindow starRailFgPacingWindow;
thread_local uint64_t starRailSourceSequence=0;

void PublishStarRailFgTiming(bool on) noexcept
{
    try {
        GenshinHookCpuTiming::OnPresent(on,false,
            [](DWORD tid,const char* name,const char* path,const GenshinHookCpuTiming::Values& v,
               int64_t hz,unsigned deferred,uint64_t refused,bool enabled) {
                using namespace GenshinHookCpuTiming;
                if(std::string_view(name)!="FgSdkPresent" && std::string_view(name)!="FgReflexSleep")return;
                const auto us=[&](uint64_t ticks){return hz>0?double(ticks)*1000000.0/double(hz):-1.0;};
                LOG_INFO("R725 FG API wall: pid={} thread={} type={} path={} enabled={} calls={} samples={} stride=64 wall_avg_us={} wall_sampled_max_us={} clock_failed={} deferred_rows={} refused_threads={} cumulative=true wall_not_cpu_busy=true not_gpu_time=true inclusive_not_additive=true sdk_present_includes_callbacks=true",
                    GetCurrentProcessId(),tid,name,path,enabled,v[Calls],v[Samples],
                    v[Samples]?us(v[Total])/double(v[Samples]):-1.0,v[Samples]?us(v[TotalMax]):-1.0,
                    v[ClockFailed],deferred,refused);
            });
        GenshinHookCpuTiming::DrainFinalRawPresent([](unsigned slot,DWORD tid,const GenshinHookCpuTiming::Values& v){
            using namespace GenshinHookCpuTiming;
            LOG_INFO("R830 final raw Present: pid={} thread={} thread_slot={} timing_epoch={} completed={} samples={} ticks_total={} ticks_max={} completed_ge50ms={} clock_failed={} nested_skipped={} max_call_sequence={} max_chain={:X} max_creation_queue_input={:X} max_endpoint_observed={:X} max_sync={} max_flags={:X} max_present1={} max_test={} max_begin_qpc={} max_end_qpc={} max_hr={:08X} max_clock_valid={} qpc_hz={} stride=1 completed_only=true in_flight=UNKNOWN source_frame=UNKNOWN identities_observed_not_lifetime_proof=true counters=thread_timing_epoch_cumulative inclusive_not_additive=true wall_not_cpu_busy=true not_gpu_time=true",
                GetCurrentProcessId(),tid,slot,v[RawEpoch],v[RawCompleted],v[RawSamples],v[RawTicks],v[RawMax],
                v[RawSlow],v[RawClockFailed],v[RawNested],v[RawSequence],v[RawChain],v[RawQueue],v[RawEndpoint],
                v[RawSync],v[RawFlags],v[RawPresent1],(v[RawFlags]&DXGI_PRESENT_TEST)!=0,
                v[RawBegin],v[RawEnd],v[RawHr],v[RawClockValid],frequency.load(std::memory_order_relaxed));
        });
    }catch(...){} // Timing publication cannot authorize work or change Present.
}

std::recursive_mutex fgLifetimeMutex;
thread_local bool fgLifecycleCallback = false;
struct ScopedFGCallback
{
    bool previous = std::exchange(fgLifecycleCallback, true);
    ~ScopedFGCallback() { fgLifecycleCallback = previous; }
};

struct FGResizeForward
{
    IDXGISwapChain* chain = nullptr;
    bool buffers1 = false;
};
thread_local FGResizeForward fgResizeForward;
struct ScopedFGResizeForward
{
    FGResizeForward previous;
    ScopedFGResizeForward(IDXGISwapChain* chain, bool buffers1)
        : previous(std::exchange(fgResizeForward, FGResizeForward { chain, buffers1 })) {}
    ~ScopedFGResizeForward() { fgResizeForward = previous; }
};

struct FGSwapchainOwner
{
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 value = 0;
    IDXGISwapChain* pin = nullptr; // Exactly one AddRef acquired by successful CreateSwapChain*.
    IFGFeature_Dx12* provider = nullptr; // Observer; refuse if the active provider no longer matches.
    HWND window = nullptr;
    bool quarantined = false;
    enum class RetireStage { Idle, BeforeProvider, Provider, AfterProvider, Ready };
    RetireStage retireStage = RetireStage::Idle;
    UINT64 queueTarget = 0, retirementSerial = 0; // One target per phase; never refreshed on pending.
    // A successful receipt is held only until ReleaseFGPin in the SAME call/thread.
    FGHookRetirementReceipt retirement;
    ~FGSwapchainOwner() { if (event) CloseHandle(event); }

    HRESULT Bind(ID3D12CommandQueue* q)
    {
        if (!q) return E_INVALIDARG;
        queue = q;
        auto hr = queue->GetDevice(IID_PPV_ARGS(&device));
        if (hr != S_OK) return fg_queue_idle::Refusal(hr);
        hr = device->GetDeviceRemovedReason();
        if (hr != S_OK) return fg_queue_idle::Refusal(hr);
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (hr != S_OK) return fg_queue_idle::Refusal(hr);
        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!event)
        {
            const auto error = GetLastError();
            return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        }
        return S_OK;
    }
};
// Deliberately no static COM destructor: pending/quarantined F5 references survive process teardown.
// Only a positively drained, retired owner is deleted during an ordinary hook call.
FGSwapchainOwner* fgOwner = nullptr;

// Caller holds fgLifetimeMutex and has a successful create/QI result.
bool CaptureStarRailSourceFrameLatencyOwner(IDXGISwapChain* createdSwapchain)
{
    // Reject other games before inspecting any provider type/lifetime metadata.
    if (_wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") != 0)
        return false;
    const auto& state = State::Instance();
    return fgOwner != nullptr && !fgOwner->quarantined && createdSwapchain != nullptr &&
        fgOwner->pin == createdSwapchain && fgOwner->pin == state.currentFGSwapchain &&
        !state.externalFrameGeneration && state.activeFgInput == FGInput::Upscaler &&
        state.activeFgOutput == FGOutput::DLSSG && fgOwner->provider != nullptr &&
        fgOwner->provider == state.currentFG && dynamic_cast<DLSSG_Dx12*>(fgOwner->provider) != nullptr;
}

bool SameComObject(IUnknown* a, IUnknown* b)
{
    if (!a || !b) return false;
    Microsoft::WRL::ComPtr<IUnknown> ai, bi;
    return a->QueryInterface(IID_PPV_ARGS(&ai)) == S_OK && b->QueryInterface(IID_PPV_ARGS(&bi)) == S_OK && ai == bi;
}

struct FGQueueIo
{
    FGSwapchainOwner& owner;
    bool Bound() const { return owner.queue && owner.device && owner.fence && owner.event; }
    HRESULT DeviceStatus() const
    {
        Microsoft::WRL::ComPtr<ID3D12Device> queueDevice, fenceDevice;
        auto hr = owner.queue->GetDevice(IID_PPV_ARGS(&queueDevice));
        if (hr != S_OK) return hr;
        hr = owner.fence->GetDevice(IID_PPV_ARGS(&fenceDevice));
        if (hr != S_OK) return hr;
        if (!SameComObject(queueDevice.Get(), owner.device.Get()) ||
            !SameComObject(fenceDevice.Get(), owner.device.Get())) return E_INVALIDARG;
        return owner.device->GetDeviceRemovedReason();
    }
    HRESULT Signal(UINT64 value) const { return owner.queue->Signal(owner.fence.Get(), value); }
    UINT64 Completed() const { return owner.fence->GetCompletedValue(); }
    HRESULT Arm(UINT64 value) const { return owner.fence->SetEventOnCompletion(value, owner.event); }
    DWORD Wait() const { return WaitForSingleObject(owner.event, 5000); }
};

FGHookRetirementResult CompleteFGQueue(FGSwapchainOwner* owner, bool terminal)
{
    using Result = FGHookRetirementState;
    if (!owner) return { Result::Refused, E_INVALIDARG };
    if (owner->quarantined) return { Result::Quarantined, DXGI_ERROR_WAS_STILL_DRAWING };
    ScopedFGCallback callback;
    FGQueueIo io { *owner };
    if (!io.Bound()) return { Result::Refused, E_UNEXPECTED };
    const auto failed = [&](HRESULT hr) -> FGHookRetirementResult {
        owner->quarantined = true; // Retain the exact pin/queue/device/fence on unknown completion.
        LOG_ERROR("FG queue retirement quarantined: hr {:X}, target {}; restart required", (UINT)hr, owner->queueTarget);
        return { Result::Quarantined, fg_queue_idle::Refusal(hr) };
    };
    auto hr = io.DeviceStatus();
    if (hr != S_OK) return failed(hr);
    if (!owner->queueTarget)
    {
        if (owner->value >= UINT64_MAX - 1) return failed(E_UNEXPECTED);
        owner->queueTarget = ++owner->value;
        hr = io.Signal(owner->queueTarget);
        if (hr != S_OK) return failed(hr); // Unknown Signal is not a new target/retry.
    }
    auto completed = io.Completed();
    if (completed == UINT64_MAX) return failed(DXGI_ERROR_DEVICE_REMOVED);
    if (completed < owner->queueTarget)
    {
        if (!terminal) return { Result::Pending, DXGI_ERROR_WAS_STILL_DRAWING };
        hr = io.Arm(owner->queueTarget);
        if (hr != S_OK) return failed(hr);
        if (io.Wait() != WAIT_OBJECT_0) return failed(DXGI_ERROR_WAS_STILL_DRAWING);
        completed = io.Completed();
    }
    if (completed == UINT64_MAX) return failed(DXGI_ERROR_DEVICE_REMOVED);
    if (completed < owner->queueTarget) return failed(DXGI_ERROR_WAS_STILL_DRAWING);
    hr = io.DeviceStatus();
    if (hr != S_OK) return failed(hr);
    owner->queueTarget = 0; // Clear only after this exact queue target really completes.
    return { Result::Completed, S_OK };
}

HRESULT PollFGQueueIdle(FGSwapchainOwner* owner)
{
    // Ordinary Create/resize is a poll. Only final Release passes terminal=true.
    return CompleteFGQueue(owner, false).hr;
}

HRESULT ValidateFGPresentQueues(FGSwapchainOwner* owner, IDXGISwapChain* chain, UINT count,
                                IUnknown* const* queues)
{
    if (!owner || owner->pin != chain) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (queues && count == 0)
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        const auto hr = chain->GetDesc(&desc);
        if (hr != S_OK) return fg_queue_idle::Refusal(hr);
        count = desc.BufferCount;
    }
    const auto hr = fg_queue_idle::PresentQueues(count, queues,
        [owner](IUnknown* queue) { return SameComObject(queue, owner->queue.Get()); });
    if (hr != S_OK) LOG_ERROR("FG ResizeBuffers1 refused: present queue transition is outside the bound queue epoch");
    return hr;
}

FGHookRetirementResult RetireFGResources(FGSwapchainOwner* owner, bool terminal)
{
    using Result = FGHookRetirementState;
    using Stage = FGSwapchainOwner::RetireStage;
    auto provider = State::Instance().currentFG;
    if (!owner) return { Result::Refused, E_INVALIDARG };
    if (owner->quarantined) return { Result::Quarantined, DXGI_ERROR_WAS_STILL_DRAWING };
    if (!provider || owner->provider != provider || owner->pin != State::Instance().currentFGSwapchain ||
        State::Instance().isShuttingDown) return { Result::Refused, DXGI_ERROR_WAS_STILL_DRAWING };
    ScopedFGCallback callback;
    if (owner->retireStage == Stage::Idle) owner->retireStage = Stage::BeforeProvider;
    if (owner->retireStage == Stage::BeforeProvider)
    {
        const auto result = CompleteFGQueue(owner, terminal);
        if (result.state != Result::Completed) return result;
        owner->retirementSerial = owner->value;
        owner->retireStage = Stage::Provider;
    }
    // Stable owner/queue epoch and serial across both phases and terminal escalation.
    const FGHookRetirementRequest request { owner, provider, owner->pin, owner->queue.Get(), owner->device.Get(),
                                           owner->fence.Get(), owner->window, owner->retirementSerial, terminal };
    auto receipt = provider->RetireSwapchainForHook(request);
    if (!receipt.Authorizes(request))
    {
        auto result = receipt.Result();
        if (result.state == Result::Completed) return { Result::Refused, E_UNEXPECTED };
        if (result.state == Result::Quarantined) owner->quarantined = true;
        return result; // Pending/Refused keep the F5 pin and are retryable, never blanket quarantine.
    }
    // Each call gets a fresh same-thread provider lease. A pending post-check
    // destroys it here, not in a future frame/thread; provider reissues by identity.
    if (owner->retireStage != Stage::Ready)
    {
        owner->retireStage = Stage::AfterProvider;
        const auto result = CompleteFGQueue(owner, terminal);
        if (result.state != Result::Completed) return result;
        owner->retireStage = Stage::Ready;
    }
    else
    {
        FGQueueIo io { *owner };
        const auto hr = io.DeviceStatus();
        if (hr != S_OK)
        { owner->quarantined = true; return { Result::Quarantined, fg_queue_idle::Refusal(hr) }; }
    }
    if (!receipt.Authorizes(request)) return { Result::Refused, DXGI_ERROR_WAS_STILL_DRAWING };
    owner->retirement = std::move(receipt);
    return { Result::Completed, S_OK };
}

template<class Release> ULONG ReleaseFGPin(FGSwapchainOwner* owner, Release release)
{
    ScopedFGCallback callback;
    auto retirement = std::move(owner->retirement); // Lease outlives owner deletion and original pin Release.
    auto pin = std::exchange(owner->pin, nullptr);
    auto& state = State::Instance();
    if (state.currentFGSwapchain == pin) state.currentFGSwapchain = nullptr;
    if (state.currentSwapchain == pin) state.currentSwapchain = nullptr;
    fgOwner = nullptr;
    const auto remaining = release(pin); // One F5-owned reference; never a loop or a guessed zero.
    delete owner; // Queue/fence/event were drained before provider retirement.
    return remaining;
}

FGHookRetirementResult CompleteExistingFGTeardown(FGSwapchainOwner* owner, bool terminal)
{
    using Result = FGHookRetirementState;
    auto provider = State::Instance().currentFG;
    if (!owner || !provider || provider != owner->provider) return { Result::Refused, DXGI_ERROR_WAS_STILL_DRAWING };
    ScopedFGCallback callback;
    if (auto dlssg = dynamic_cast<DLSSG_Dx12*>(provider))
    {
        const auto result = dlssg->CompletePendingTeardownForHook(terminal);
        if (result.state == Result::Quarantined) owner->quarantined = true;
        return result;
    }
    return provider->CompletePendingTeardown() ? FGHookRetirementResult { Result::Completed, S_OK }
                                             : FGHookRetirementResult { Result::Pending, DXGI_ERROR_WAS_STILL_DRAWING };
}

template<class Release> HRESULT RetireFGForCreate(FGSwapchainOwner* owner, Release release)
{
    ScopedFGCallback callback;
    if (owner->retireStage == FGSwapchainOwner::RetireStage::Idle)
    {
        // Existing teardown may own inputs/backbuffers. Progress it BEFORE the
        // advisory external-reference probe, even when the old chain has no Present.
        const auto completion = CompleteExistingFGTeardown(owner, false);
        if (completion.state != FGHookRetirementState::Completed) return completion.hr;
        owner->pin->AddRef();
        const auto remaining = release(owner->pin); // Balance only this temporary F5 probe.
        if (remaining != 1)
        {
            if (remaining == 0) owner->quarantined = true;
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
    }
    const auto result = RetireFGResources(owner, false);
    if (result.state != FGHookRetirementState::Completed) return result.hr;
    // Already-accepted pending retirement skips the initial probe. Only after
    // input retirement removed internal pins can external refs veto final deletion.
    owner->pin->AddRef();
    const auto remaining = release(owner->pin);
    if (remaining != 1)
    {
        owner->retirement = {}; // Never store a mutex lease across calls/threads.
        if (remaining == 0) owner->quarantined = true;
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }
    ReleaseFGPin(owner, release);
    return S_OK;
}

}

static bool CheckForFGStatus(HWND outputWindow)
{
    if (GenshinVisibleChild::IsRetainedChild(outputWindow) &&
        GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()))
    {
        const auto& state=State::Instance();
        // Availability is observational for the optional child. Failure must
        // leave the user's DLSSG/replacement selection and native route intact.
        return GenshinVisibleChild::IsOwnedChild(outputWindow) &&
            state.activeFgInput==FGInput::Upscaler && state.activeFgOutput==FGOutput::DLSSG &&
            state.swapchainInteropApi!=SwapchainInteropApi::VkwDx12 &&
            (!state.currentFG || dynamic_cast<DLSSG_Dx12*>(state.currentFG)) &&
            !state.currentFGSwapchain && StreamlineProxy::LoadStreamline();
    }
    // Need to check overlay menu parameter, goes to places it shouldn't go
    // if (!Config::Instance()->OverlayMenu.value_or_default())
    //    return false;

    if (State::Instance().activeFgInput == FGInput::NoFG || State::Instance().activeFgInput == FGInput::NvngxFG)
        return false;

    // Disable FG if amd dll is not found
    if (State::Instance().activeFgOutput == FGOutput::FSRFG)
    {
        FfxApiProxy::InitFfxDx12();
        if (!FfxApiProxy::IsFGReady())
        {
            ImGui::InsertNotification(
                { ImGuiToastType::Error, 20000, "Can't init FSR FG\nAre you missing the required DLLs?" });

            LOG_DEBUG("Can't init FfxApiProxy, disabling FGOutput");
            Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
            State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
        }
    }
    else if (State::Instance().activeFgOutput == FGOutput::XeFG && !XeFGProxy::InitXeFG())
    {
        ImGui::InsertNotification(
            { ImGuiToastType::Error, 20000, "Can't init XeFG\nAre you missing the required DLLs?" });

        LOG_DEBUG("Can't init XeFGProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
    }
    else if (State::Instance().activeFgOutput == FGOutput::DLSSG && !StreamlineProxy::LoadStreamline())
    {
        ImGui::InsertNotification(
            { ImGuiToastType::Error, 20000, "Can't init DLSSG Output\nAre you missing the streamline folder?" });

        LOG_DEBUG("Can't init StreamlineProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();

        Config::Instance()->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::None);
        State::Instance().activeFgNvngx = Config::Instance()->FGNvngxReplacement.value_or_default();
    }

    if (State::Instance().activeFgOutput == FGOutput::NoFG)
    {
        LOG_WARN("FGOutput doesn't have any FG active");
        return false;
    }

    return true;
}

HRESULT FGHooks::CreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                 IDXGISwapChain** ppSwapChain, bool* sourceFrameLatencyOwner)
{
    if (sourceFrameLatencyOwner != nullptr) *sourceFrameLatencyOwner = false;
    if (!ppSwapChain) return E_POINTER;
    *ppSwapChain = nullptr;
    if (!pDevice || !pDesc) return E_INVALIDARG;
    std::unique_lock lifetime(fgLifetimeMutex, std::try_to_lock);
    if (!lifetime.owns_lock() || fgLifecycleCallback || State::Instance().isShuttingDown)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    if (fgOwner && fgOwner->quarantined) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (State::Instance().currentFGSwapchain && (!fgOwner || fgOwner->pin != State::Instance().currentFGSwapchain))
        return E_UNEXPECTED;
    // Failed admission must leave the caller's descriptor and the selected game/provider route intact.
    auto* callerDesc = pDesc;
    DXGI_SWAP_CHAIN_DESC localDesc = *pDesc;
    pDesc = &localDesc;
    // Check if it's Dx12
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queueOwner;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&queueOwner)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    auto cq = queueOwner.Get();
    if (fgOwner && fgOwner->retireStage != FGSwapchainOwner::RetireStage::Idle)
    {
        // Resume an accepted retirement before availability/preserve/refcount
        // gates. In particular a retained Genshin child still has the old pin.
        const auto hr = RetireFGForCreate(fgOwner, o_FGRelease);
        if (hr != S_OK) return hr;
    }
    if (fgOwner)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default() &&
            fgOwner->retireStage == FGSwapchainOwner::RetireStage::Idle)
        {
            // ResizeBuffers' hook owns the queue poll; an outer poll would re-arm it on each retry.
            if (fgOwner->window != pDesc->OutputWindow || !SameComObject(cq, fgOwner->queue.Get()))
                return DXGI_ERROR_INVALID_CALL;
            auto hr = fgOwner->pin->ResizeBuffers(pDesc->BufferCount, pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
                pDesc->BufferDesc.Format, pDesc->Flags);
            if (hr != S_OK) return hr;
            // QI contributes the new caller reference required by the create API; keep the existing F5 pin.
            hr = fgOwner->pin->QueryInterface(IID_PPV_ARGS(ppSwapChain));
            if (hr == S_OK)
            {
                *callerDesc = *pDesc;
                if (sourceFrameLatencyOwner != nullptr)
                    *sourceFrameLatencyOwner = CaptureStarRailSourceFrameLatencyOwner((IDXGISwapChain*) *ppSwapChain);
            }
            return hr;
        }
    }
    if (!CheckForFGStatus(pDesc ? pDesc->OutputWindow : nullptr))
    {
        LOG_WARN("Can't init FG Feature or invalid FGOutput setting!");
        return E_NOINTERFACE;
    }

    auto nextOwner = std::make_unique<FGSwapchainOwner>();
    auto bindResult = nextOwner->Bind(cq);
    if (bindResult != S_OK) return bindResult;
    if (fgOwner)
    {
        const auto hr = RetireFGForCreate(fgOwner, o_FGRelease);
        if (hr != S_OK) return hr;
    }

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG = new DLSSG_Dx12();
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    bool scResult = false;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedFGCallback callback;

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (State::Instance().activeFgOutput == FGOutput::XeFG && !pDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        scResult = fg->CreateSwapchain(pFactory, cq, pDesc, ppSwapChain, false);

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        if (!*ppSwapChain) return E_UNEXPECTED;
        nextOwner->pin = *ppSwapChain;
        nextOwner->pin->AddRef(); // Acquire exactly one F5 lifetime pin, separate from the returned caller ref.
        nextOwner->provider = fg;
        nextOwner->window = pDesc->OutputWindow;
        fgOwner = nextOwner.release();
        *callerDesc = *pDesc;

        SetFGSwapchain(*ppSwapChain, pDesc->OutputWindow);
        if (sourceFrameLatencyOwner != nullptr)
            *sourceFrameLatencyOwner = CaptureStarRailSourceFrameLatencyOwner((IDXGISwapChain*) *ppSwapChain);

        return S_OK;
    }

    return E_INVALIDARG;
}

HRESULT FGHooks::CreateSwapChainForHwnd(IDXGIFactory* pFactory, IUnknown* pDevice, HWND hWnd,
                                        DXGI_SWAP_CHAIN_DESC1* pDesc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                        IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain,
                                        bool* sourceFrameLatencyOwner)
{
    if (sourceFrameLatencyOwner != nullptr) *sourceFrameLatencyOwner = false;
    if (!ppSwapChain) return E_POINTER;
    *ppSwapChain = nullptr;
    if (!pDevice || !pDesc) return E_INVALIDARG;
    std::unique_lock lifetime(fgLifetimeMutex, std::try_to_lock);
    if (!lifetime.owns_lock() || fgLifecycleCallback || State::Instance().isShuttingDown)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    if (fgOwner && fgOwner->quarantined) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (State::Instance().currentFGSwapchain && (!fgOwner || fgOwner->pin != State::Instance().currentFGSwapchain))
        return E_UNEXPECTED;
    // Failed admission must leave the caller's descriptor and the selected game/provider route intact.
    auto* callerDesc = pDesc;
    DXGI_SWAP_CHAIN_DESC1 localDesc = *pDesc;
    pDesc = &localDesc;
    // Check if it's Dx12
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queueOwner;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&queueOwner)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    auto cq = queueOwner.Get();
    if (fgOwner && fgOwner->retireStage != FGSwapchainOwner::RetireStage::Idle)
    {
        // Resume an accepted retirement before availability/preserve/refcount
        // gates. In particular a retained Genshin child still has the old pin.
        const auto hr = RetireFGForCreate(fgOwner, o_FGRelease);
        if (hr != S_OK) return hr;
    }
    if (fgOwner)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default() &&
            fgOwner->retireStage == FGSwapchainOwner::RetireStage::Idle)
        {
            // ResizeBuffers' hook owns the queue poll; an outer poll would re-arm it on each retry.
            if (fgOwner->window != hWnd || !SameComObject(cq, fgOwner->queue.Get()))
                return DXGI_ERROR_INVALID_CALL;
            auto hr = fgOwner->pin->ResizeBuffers(pDesc->BufferCount, pDesc->Width, pDesc->Height, pDesc->Format, pDesc->Flags);
            if (hr != S_OK) return hr;
            // QI contributes the new caller reference required by the create API; keep the existing F5 pin.
            hr = fgOwner->pin->QueryInterface(IID_PPV_ARGS(ppSwapChain));
            if (hr == S_OK)
            {
                *callerDesc = *pDesc;
                if (sourceFrameLatencyOwner != nullptr)
                    *sourceFrameLatencyOwner = CaptureStarRailSourceFrameLatencyOwner((IDXGISwapChain*) *ppSwapChain);
            }
            return hr;
        }
    }
    if (!CheckForFGStatus(hWnd))
    {
        LOG_WARN("Can't init FG Feature or invalid FGOutput setting!");
        return E_NOINTERFACE;
    }

    auto nextOwner = std::make_unique<FGSwapchainOwner>();
    auto bindResult = nextOwner->Bind(cq);
    if (bindResult != S_OK) return bindResult;
    if (fgOwner)
    {
        const auto hr = RetireFGForCreate(fgOwner, o_FGRelease);
        if (hr != S_OK) return hr;
    }

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG = new DLSSG_Dx12();
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    bool scResult = false;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
        ScopedFGCallback callback;

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (State::Instance().activeFgOutput == FGOutput::XeFG && pFullscreenDesc != nullptr &&
            !pFullscreenDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        scResult = fg->CreateSwapchain1(pFactory, cq, hWnd, pDesc, pFullscreenDesc, ppSwapChain, false);

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        if (!*ppSwapChain) return E_UNEXPECTED;
        nextOwner->pin = *ppSwapChain;
        nextOwner->pin->AddRef(); // Acquire exactly one F5 lifetime pin, separate from the returned caller ref.
        nextOwner->provider = fg;
        nextOwner->window = hWnd;
        fgOwner = nextOwner.release();
        *callerDesc = *pDesc;

        SetFGSwapchain((IDXGISwapChain*) *ppSwapChain, hWnd);
        if (sourceFrameLatencyOwner != nullptr)
            *sourceFrameLatencyOwner = CaptureStarRailSourceFrameLatencyOwner((IDXGISwapChain*) *ppSwapChain);

        return S_OK;
    }

    return E_INVALIDARG;
}

void FGHooks::SetFGSwapchain(IDXGISwapChain* pSwapChain, HWND hWnd)
{
    if (pSwapChain == nullptr)
        return;

    _hwnd = hWnd;

    if (_dx12InteropPresentSC == pSwapChain)
    {
        _dx12InteropPresentSC = nullptr;
        _dx12InteropPresentHwnd = nullptr;
    }

    State::Instance().currentFGSwapchain = pSwapChain;
    State::Instance().currentSwapchain = pSwapChain;

    HookFGSwapchain(pSwapChain);
}

void FGHooks::SetDx12InteropPresentSC(IDXGISwapChain* pSwapChain, HWND hWnd)
{
    if (pSwapChain == nullptr)
        return;

    _dx12InteropPresentSC = pSwapChain;
    _dx12InteropPresentHwnd = hWnd;

    LOG_INFO("Set DX12 presenting swapchain: {:X}, hWnd: {:X}", (size_t) pSwapChain, (size_t) hWnd);
}

void FGHooks::ClearDx12InteropPresentSC(IUnknown* pSwapChain)
{
    if (pSwapChain == nullptr || pSwapChain != _dx12InteropPresentSC)
        return;

    LOG_INFO("Clearing DX12 presenting swapchain: {:X}", (size_t) pSwapChain);

    _dx12InteropPresentSC = nullptr;
    _dx12InteropPresentHwnd = nullptr;
}

bool FGHooks::IsDx12InteropPresentSC(IUnknown* pSwapChain)
{
    return pSwapChain != nullptr && pSwapChain == _dx12InteropPresentSC;
}

namespace
{
struct MenuOutputRecord
{
    IUnknown* queue;
    HWND window;
};
std::mutex menuOutputMutex;
std::unordered_map<IUnknown*, MenuOutputRecord> menuOutputs;
IUnknown* MenuOutputIdentity(IDXGISwapChain* swapchain)
{
    IUnknown* identity = nullptr;
    if (swapchain == nullptr || FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&identity))))
        return nullptr;
    identity->Release(); // Caller holds the live swapchain throughout the registry operation.
    return identity;
}
}

void FGHooks::RegisterMenuOutput(IDXGISwapChain* swapchain, IUnknown* device, HWND hWnd)
{
    if (swapchain == nullptr || device == nullptr || hWnd == nullptr)
        return;
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr)
        return; // Hidden D3D11 swapchains never claim menu output ownership.
    IUnknown* realQueue = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, queue, &realQueue))
        realQueue = queue;
    std::lock_guard lock(menuOutputMutex);
    if (auto identity = MenuOutputIdentity(swapchain); identity != nullptr)
        menuOutputs[identity] = { realQueue, hWnd };
    queue->Release(); // The wrapped owner, not this non-owning registry, controls lifetime.
}

void FGHooks::UnregisterMenuOutput(IDXGISwapChain* swapchain)
{
    std::lock_guard lock(menuOutputMutex);
    menuOutputs.erase(MenuOutputIdentity(swapchain));
}

bool FGHooks::HasWrappedMenuOutput(HWND hWnd)
{
    auto& state = State::Instance();
    if (state.activeFgOutput != FGOutput::DLSSG || !OwnsMenuOverlay(hWnd))
        return false;
    std::lock_guard lock(menuOutputMutex);
    for (const auto& [swapchain, output] : menuOutputs)
        if (swapchain != state.currentFGSwapchain && output.window == hWnd &&
            output.queue != state.currentFG->GetCommandQueue())
            return true; // Exact captured output queue; never confuse the input/client queue with it.
    return false;
}

bool FGHooks::IsWrappedMenuOutput(IDXGISwapChain* swapchain, IUnknown* queue, HWND hWnd)
{
    auto& state = State::Instance();
    if (state.activeFgOutput != FGOutput::DLSSG || !OwnsMenuOverlay(hWnd))
        return false;
    std::lock_guard lock(menuOutputMutex);
    const auto output = menuOutputs.find(MenuOutputIdentity(swapchain));
    return output != menuOutputs.end() && swapchain != state.currentFGSwapchain &&
           output->second.window == hWnd && output->second.queue == queue &&
           queue != state.currentFG->GetCommandQueue();
}

bool FGHooks::OwnsMenuOverlay(HWND hWnd)
{
    const auto& state = State::Instance();
    return !state.isShuttingDown && hWnd != nullptr && hWnd == _hwnd &&
           state.currentFGSwapchain != nullptr && !IsDx12InteropPresentSC(state.currentFGSwapchain) &&
           o_FGSCPresent != nullptr && state.currentFG != nullptr &&
           state.currentFG->GetCommandQueue() != nullptr;
}

void FGHooks::HookFGSwapchain(IDXGISwapChain* pSwapChain)
{
    if (o_FGSCPresent != nullptr || pSwapChain == nullptr)
        return;

    void** pFactoryVTable = *reinterpret_cast<void***>(pSwapChain);

    o_FGRelease = (PFN_Release) pFactoryVTable[2];
    o_FGSCPresent = (PFN_Present) pFactoryVTable[8];
    o_FGSCSetFullscreenState = (PFN_SetFullscreenState) pFactoryVTable[10];
    o_FGSCGetFullscreenState = (PFN_GetFullscreenState) pFactoryVTable[11];
    o_FGSCResizeBuffers = (PFN_ResizeBuffers) pFactoryVTable[13];
    o_FGSCResizeTarget = (PFN_ResizeTarget) pFactoryVTable[14];
    o_FGSCGetFullscreenDesc = (PFN_GetFullscreenDesc) pFactoryVTable[19];
    o_FGSCPresent1 = (PFN_Present1) pFactoryVTable[22];
    o_FGSCGetFrameLatencyWaitableObject = (PFN_GetFrameLatencyWaitableObject) pFactoryVTable[33];
    o_FGSCResizeBuffers1 = (PFN_ResizeBuffers1) pFactoryVTable[39];

    if (o_FGSCPresent != nullptr)
    {
        LOG_INFO("Hooking FG SwapChain present");
        LOG_TRACE("FGRelease: {:X}", (size_t) o_FGRelease);
        LOG_TRACE("FGSCPresent: {:X}", (size_t) o_FGSCPresent);
        LOG_TRACE("FGSCSetFullscreenState: {:X}", (size_t) o_FGSCSetFullscreenState);
        LOG_TRACE("FGSCGetFullscreenState: {:X}", (size_t) o_FGSCGetFullscreenState);
        LOG_TRACE("FGSCResizeBuffers: {:X}", (size_t) o_FGSCResizeBuffers);
        LOG_TRACE("FGSCResizeTarget: {:X}", (size_t) o_FGSCResizeTarget);
        LOG_TRACE("FGSCGetFullscreenDesc: {:X}", (size_t) o_FGSCGetFullscreenDesc);
        LOG_TRACE("FGSCPresent1: {:X}", (size_t) o_FGSCPresent1);
        LOG_TRACE("FGSCResizeBuffers1: {:X}", (size_t) o_FGSCResizeBuffers1);
        LOG_TRACE("FGSCGetFrameLatencyWaitableObject: {:X}", (size_t) o_FGSCGetFrameLatencyWaitableObject);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_FGRelease, hkFGRelease);
        DetourAttach(&(PVOID&) o_FGSCPresent, hkFGPresent);
        DetourAttach(&(PVOID&) o_FGSCResizeTarget, hkResizeTarget);
        DetourAttach(&(PVOID&) o_FGSCResizeBuffers, hkResizeBuffers);
        DetourAttach(&(PVOID&) o_FGSCSetFullscreenState, hkSetFullscreenState);

        if (o_FGSCPresent1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCPresent1, hkFGPresent1);

        if (o_FGSCResizeBuffers1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCResizeBuffers1, hkResizeBuffers1);

        if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            if (o_FGSCGetFullscreenState != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenState, hkGetFullscreenState);

            if (o_FGSCGetFullscreenDesc != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenDesc, hkGetFullscreenDesc);

            if ((Config::Instance()->SimulateWaitableObject.value_or_default() ||
                 (State::Instance().gameEngine == GameEngineType::Unity &&
                  State::Instance().activeFgOutput == FGOutput::XeFG)) &&
                o_FGSCGetFrameLatencyWaitableObject != nullptr)
            {
                DetourAttach(&(PVOID&) o_FGSCGetFrameLatencyWaitableObject, hkGetFrameLatencyWaitableObject);
            }
        }

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to attach detour: {:X}", detourResult);
            o_FGRelease = nullptr;
            o_FGSCPresent = nullptr;
            o_FGSCSetFullscreenState = nullptr;
            o_FGSCGetFullscreenState = nullptr;
            o_FGSCResizeBuffers = nullptr;
            o_FGSCResizeTarget = nullptr;
            o_FGSCGetFullscreenDesc = nullptr;
            o_FGSCPresent1 = nullptr;
            o_FGSCResizeBuffers1 = nullptr;
            o_FGSCGetFrameLatencyWaitableObject = nullptr;
        }
    }
}

HRESULT FGHooks::hkSetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget)
{
    if (GenshinVisibleChild::IsOwnedChild(_hwnd))
        return Fullscreen ? DXGI_ERROR_UNSUPPORTED : o_FGSCSetFullscreenState(This, FALSE, pTarget);
    IFGFeature* fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    bool modeChanged = false;
    bool orgFS = Fullscreen;
    if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        if (Fullscreen)
        {
            Fullscreen = false;

            if (!State::Instance().SCExclusiveFullscreen)
            {
                State::Instance().SCExclusiveFullscreen = true;
                modeChanged = true;
            }

            LOG_DEBUG("Prevented exclusive fullscreen");
        }
        else
        {
            if (State::Instance().SCExclusiveFullscreen)
            {
                modeChanged = true;
                State::Instance().SCExclusiveFullscreen = false;
            }
        }
    }

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (State::Instance().activeFgOutput == FGOutput::XeFG && Fullscreen)
        LOG_WARN("Using exclusive fullscreen with XeFG!!!");

    auto result = S_OK;

    if (!Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        result = o_FGSCSetFullscreenState(This, Fullscreen, pTarget);
        LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Result: {:X}", Fullscreen, (size_t) pTarget, (UINT) result);
    }

    if (result == S_OK && modeChanged)
    {
        LOG_DEBUG("Mode changed");

        DXGI_SWAP_CHAIN_DESC scDesc {};
        This->GetDesc(&scDesc);

        if (State::Instance().SCExclusiveFullscreen)
        {
            SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

            Util::MonitorInfo info;

            if (pTarget != nullptr)
                info = Util::GetMonitorInfoForOutput(pTarget);
            else
                info = Util::GetMonitorInfoForWindow(_hwnd);

            LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                      info.y, wstring_to_string(info.name));

            SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            SetWindowLongPtr(_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_OVERLAPPEDWINDOW);
            SetWindowPos(_hwnd, nullptr, 0, 0, scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    return result;
}

HANDLE FGHooks::hkGetFrameLatencyWaitableObject(IDXGISwapChain2* This)
{
    if (State::Instance().activeFgOutput != FGOutput::XeFG)
        return o_FGSCGetFrameLatencyWaitableObject(This);

    if (_semaphore == nullptr)
    {
        _semaphore = CreateSemaphore(nullptr, 16, 16, nullptr);

        if (_semaphore == nullptr)
            return nullptr;
    }

    HANDLE duplicatedHandle = nullptr;

    if (!DuplicateHandle(GetCurrentProcess(), _semaphore, GetCurrentProcess(), &duplicatedHandle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        return nullptr;
    }

    return duplicatedHandle;
}

HRESULT FGHooks::hkGetFullscreenDesc(IDXGISwapChain1* This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    auto result = o_FGSCGetFullscreenDesc(This, pDesc);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        pDesc->Windowed = false;
    }

    return result;
}

HRESULT FGHooks::hkGetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    auto result = o_FGSCGetFullscreenState(This, pFullscreen, ppTarget);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        *pFullscreen = true;
    }

    return result;
}

HRESULT FGHooks::hkResizeBuffers(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                 UINT SwapChainFlags)
{
    if (State::Instance().currentFGSwapchain != This)
        return o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    if (HoYoShade::ColorConsumers(This) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
    // Only the same thread/object's already admitted ResizeBuffers1 may forward this inner call.
    if (fgResizeForward.chain == This && fgResizeForward.buffers1)
    {
        fgResizeForward.chain = nullptr; // Consume exactly one forwarding receipt.
        return o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    std::unique_lock lifetime(fgLifetimeMutex, std::try_to_lock);
    if (!lifetime.owns_lock() || !fgOwner || fgOwner->quarantined || fgOwner->pin != This || State::Instance().isShuttingDown)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    auto fgDx12 = State::Instance().currentFG;
    if (!fgDx12 || fgOwner->provider != fgDx12) return DXGI_ERROR_WAS_STILL_DRAWING;
    auto lifecycleExecution = fgDx12 != nullptr ? fgDx12->GuardExecution(FGExecutionScope::Lifecycle) : FGExecutionGuard {};
    const auto completion = CompleteExistingFGTeardown(fgOwner, false);
    if (completion.state != FGHookRetirementState::Completed) return completion.hr;
    if (fgOwner->retireStage != FGSwapchainOwner::RetireStage::Idle) return DXGI_ERROR_WAS_STILL_DRAWING;
    const auto idleResult = PollFGQueueIdle(fgOwner);
    if (idleResult != S_OK) return idleResult;
    ScopedFGCallback callback;
    GenshinBridgeFg::Revoke();

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    IFGFeature* fg = fgDx12;

    if (!State::Instance().SCExclusiveFullscreen && Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (NewFormat == desc.BufferDesc.Format || NewFormat == 0) &&
                State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fgDx12 != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fgDx12->GetUICommandList();

                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        // Docs say you cannot do that
        // SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    if (fgDx12 != nullptr && !fgDx12->PrepareForSwapchainChange())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    GenshinBridgeFg::Collect(); // observe exact successful drain receipts before resizing either swapchain
    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    State::Instance().SCLastFlags = SwapChainFlags;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();

        // Let's try Dx11 like approach on Dx12
        std::shared_lock<std::shared_mutex> resizeLock(_resizeMutex, std::defer_lock);
        if (State::Instance().activeFgOutput == FGOutput::XeFG &&
            State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
        {
            resizeLock.lock();
        }
    }



    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        ScopedFGResizeForward forwarding(This, false);
        result = o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }


    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    if (result == S_OK)
    {
        if (fg != nullptr)
        {
            State::Instance().fgChanged = true;
            fg->Deactivate();
            fg->UpdateTarget();
        }
    }

    // Resize window to cover the screen
    if (result == S_OK && !GenshinVisibleChild::IsOwnedChild(_hwnd) &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default() &&
        State::Instance().SCExclusiveFullscreen)
    {
        SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    return result;
}

HRESULT FGHooks::hkResizeTarget(IDXGISwapChain* This, const DXGI_MODE_DESC* pNewTargetParameters)
{
    if (GenshinVisibleChild::IsOwnedChild(_hwnd)) return DXGI_ERROR_UNSUPPORTED;
    if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        LOG_DEBUG("Skipping resize target.");
        return S_OK;
    }

    IFGFeature* fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    return o_FGSCResizeTarget(This, pNewTargetParameters);
}

HRESULT FGHooks::hkResizeBuffers1(IDXGISwapChain3* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                  UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    if (State::Instance().currentFGSwapchain != This)
        return o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    if (HoYoShade::ColorConsumers(This) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
    // Forwarding cannot bypass queue-epoch validation, even inside an admitted ResizeBuffers.
    if (fgResizeForward.chain == This && !fgResizeForward.buffers1)
    {
        const auto hr = ValidateFGPresentQueues(fgOwner, This, BufferCount, ppPresentQueue);
        if (hr != S_OK) return hr;
        fgResizeForward.chain = nullptr; // Consume exactly one forwarding receipt.
        return o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                    ppPresentQueue);
    }

    std::unique_lock lifetime(fgLifetimeMutex, std::try_to_lock);
    if (!lifetime.owns_lock() || !fgOwner || fgOwner->quarantined || fgOwner->pin != This || State::Instance().isShuttingDown)
        return DXGI_ERROR_WAS_STILL_DRAWING;
    auto fgDx12 = State::Instance().currentFG;
    if (!fgDx12 || fgOwner->provider != fgDx12) return DXGI_ERROR_WAS_STILL_DRAWING;
    auto lifecycleExecution = fgDx12 != nullptr ? fgDx12->GuardExecution(FGExecutionScope::Lifecycle) : FGExecutionGuard {};
    const auto queuesResult = ValidateFGPresentQueues(fgOwner, This, BufferCount, ppPresentQueue);
    if (queuesResult != S_OK) return queuesResult;
    const auto completion = CompleteExistingFGTeardown(fgOwner, false);
    if (completion.state != FGHookRetirementState::Completed) return completion.hr;
    if (fgOwner->retireStage != FGSwapchainOwner::RetireStage::Idle) return DXGI_ERROR_WAS_STILL_DRAWING;
    const auto idleResult = PollFGQueueIdle(fgOwner);
    if (idleResult != S_OK) return idleResult;
    ScopedFGCallback callback;
    GenshinBridgeFg::Revoke();

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}, Caller: {}", BufferCount,
              Width, Height, (UINT) Format, SwapChainFlags, Util::WhoIsTheCaller(_ReturnAddress()));

    IFGFeature* fg = fgDx12;

    if (!State::Instance().SCExclusiveFullscreen && Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (Format == desc.BufferDesc.Format || Format == 0) && State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fgDx12 != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fgDx12->GetUICommandList();
                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        // SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    if (fgDx12 != nullptr && !fgDx12->PrepareForSwapchainChange())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    GenshinBridgeFg::Collect(); // observe exact successful drain receipts before resizing either swapchain
    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    State::Instance().SCLastFlags = SwapChainFlags;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();

        // Let's try Dx11 like approach on Dx12
        std::shared_lock<std::shared_mutex> resizeLock(_resizeMutex, std::defer_lock);
        if (State::Instance().activeFgOutput == FGOutput::XeFG &&
            State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
        {
            resizeLock.lock();
        }
    }

    // Release menu render targets
    if (Config::Instance()->OverlayMenu.value_or_default())
        MenuOverlayDx::CleanupRenderTarget(false, NULL);


    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        ScopedFGResizeForward forwarding(This, true);

        result = o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);

    }

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    if (result == S_OK)
    {
        if (fg != nullptr)
        {
            State::Instance().fgChanged = true;
            fg->Deactivate();
            fg->UpdateTarget();
        }
    }

    // Resize window to cover the screen
    if (result == S_OK && !GenshinVisibleChild::IsOwnedChild(_hwnd) &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default() &&
        State::Instance().SCExclusiveFullscreen)
    {
        SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    return result;
}

void FGHooks::DrainStarRailAttribution() noexcept
{
    // R897: called only by the factory-admitted outer StarRail source wrapper,
    // before its LowLatency/PresentScope and before entering any provider guard.
    if(GenshinHookCpuTiming::starRailSourceDepth!=0)return;
    try {
        starRailFgPacingWindow.Emit(true);
        GenshinHookCpuTiming::DrainStarRailCalls([](DWORD tid,unsigned call,const GenshinHookCpuTiming::StarRailData& d){
            using namespace GenshinHookCpuTiming;
            const auto hz=frequency.load(std::memory_order_relaxed);
            const auto us=[&](uint64_t ticks){return hz>0?double(ticks)*1000000.0/double(hz):-1.0;};
            // Fixed completed snapshots only. No provider access, QI, fence query, retry or wait.
            for(unsigned row=0;row<2;++row){
                const auto& s=row==0?d.last:d.maximum;
                LOG_INFO("R897 StarRail attribution: pid={} thread={} timing_epoch={} call={} row={} completed_calls={} samples={} stride=64 clock_failed={} wall_avg_us={} wall_sampled_max_us={} sample_known={} parent_known={} entry_qpc={} parent_pid={} parent_tid={} wrapper={:X} child={:X} source_sequence={} frame={}{} chain={:X} copy_queue_observed={:X} present_game_queue_observed={:X} host_copy_fence_observed={:X} copy_ring={} slot={} required_fence={} completed_before={} completed_before_known={} sync={} flags={:X} flags_known={} origin={} marker={} marker_frame={} marker_frame_known={} result_kind={} result_known={} result_raw={:08X} begin_qpc={} end_qpc={} qpc_hz={} sampled_wall_us={} same_sample_metadata=true source_sequence_zero_is_unknown=true zero_identity_is_unknown=true in_flight=UNKNOWN sdk_private_fence=UNKNOWN sdk_queue=UNKNOWN worker_source_join=UNKNOWN identities_observed_not_lifetime_proof=true wall_not_cpu_busy=true inclusive_not_additive=true",
                    GetCurrentProcessId(),tid,d.epoch,StarRailCallName(call),row==0?"last_sample":"maximum",
                    d.calls,d.samples,d.clockFailed,d.samples?us(d.ticks)/double(d.samples):-1.0,d.samples?us(d.maxTicks):-1.0,
                    d.samples!=0,s[SrParentKnown],s[SrEntryQpc],s[SrPid],s[SrTid],s[SrWrapper],s[SrChild],
                    s[SrSourceSequence],s[SrFrame],
                    call==static_cast<unsigned>(StarRailCall::HiddenPresent) || call==static_cast<unsigned>(StarRailCall::CopyWait)
                        ? " frame_known=false frame_status=UNKNOWN frame_zero_is_unknown=true" : "",
                    s[SrChain],s[SrQueue],s[SrPresentQueue],s[SrFence],
                    StarRailRingName(s[SrRing]),s[SrSlot],s[SrRequired],s[SrCompleted],s[SrCompletedKnown],
                    s[SrSync],s[SrFlags],s[SrFlagsKnown],StarRailOriginName(s[SrOrigin]),s[SrMarker],s[SrMarkerFrame],s[SrMarkerFrameKnown],
                    call==static_cast<unsigned>(StarRailCall::PclMarker)?"sl::Result":
                        (call==static_cast<unsigned>(StarRailCall::CopyWait)?"WaitResult":"HRESULT"),
                    s[SrResultKnown],s[SrResult],s[SrBegin],s[SrEnd],hz,
                    d.samples && s[SrEnd]>=s[SrBegin]?us(s[SrEnd]-s[SrBegin]):-1.0);
            }
        });
    }catch(...){} // Statistics/logging failure never changes the game's original call.
}

HRESULT FGHooks::hkFGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    if (HoYoShade::ColorConsumers(This) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
    // Skip XeFG's internal call
    if (_skipPresent)
    {
        LOG_DEBUG("XeFG call skipping");

        IDXGISwapChain* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            result = sc->Present(SyncInterval, Flags);
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            result = o_FGSCPresent(This, SyncInterval, Flags);
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);

    _skipPresent1 = true;
    auto result = FGPresent(This, SyncInterval, Flags, nullptr);
    _skipPresent1 = false;

    return result;
}

HRESULT FGHooks::hkFGPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags,
                              const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (HoYoShade::ColorConsumers(This) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
    // Skip XeFG's internal call
    if (_skipPresent1)
    {
        LOG_DEBUG("XeFG call skipping");

        IDXGISwapChain3* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            result = sc->Present1(SyncInterval, Flags, pPresentParameters);
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            result = o_FGSCPresent1(This, SyncInterval, Flags, pPresentParameters);
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);
    _skipPresent = true;
    auto result = FGPresent(This, SyncInterval, Flags, pPresentParameters);
    _skipPresent = false;

    return result;
}

HRESULT FGHooks::FGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags,
                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (HoYoShade::ColorConsumers(This) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
    _lastPresentFlags = Flags;

    auto& state = State::Instance();
    auto config = Config::Instance();

    if (state.isShuttingDown)
    {
        GenshinFg::Current().Reset();
        GenshinBridgeFg::Revoke();
        if (pPresentParameters == nullptr)
            return o_FGSCPresent(This, SyncInterval, Flags);
        else
            return o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, pPresentParameters);
    }

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;
    const auto pacingPresentFrame = state.frameCount; // Actual FGPresent entry, not a displayed-child ID.

    if (willPresent)
    {
        // Let's try Dx11 like approach on Dx12
        std::shared_lock<std::shared_mutex> resizeLock(_resizeMutex, std::defer_lock);
        if (State::Instance().activeFgOutput == FGOutput::XeFG &&
            State::Instance().swapchainInteropApi == SwapchainInteropApi::None)
        {
            resizeLock.lock();
        }

        state.fgLastFrame++;

        double ftDelta = 0.0f;
        auto now = Util::MillisecondsNow();

        if (_lastFGFrameTime > 0.0)
            ftDelta = now - _lastFGFrameTime;

        _lastFGFrameTime = now;
        state.lastFGFrameTime = ftDelta;

        LOG_DEBUG("flags: {:X}, Frametime: {}", Flags, ftDelta);

#ifdef LOW_LATENCY_INPUTS
        IUnknown* device = state.currentD3D12Device ? state.currentD3D12Device : (IUnknown*) state.currentD3D11Device;
        InputCommon::mark_present_start(device);
#endif
    }

    // Work on the game's picture before frame generation takes it must go on the queue frame generation takes it on.
    ID3D12CommandQueue* gameQueue = state.currentCommandQueue;

    if (auto* fg12 = dynamic_cast<IFGFeature_Dx12*>(state.currentFG); fg12 != nullptr && fg12->GameCommandQueue() != nullptr)
    {
        static bool logged = false;

        if (!logged && fg12->GameCommandQueue() != state.currentCommandQueue)
        {
            logged = true;
            LOG_INFO("FGPresent: frame generation's game queue {:X} is not State::currentCommandQueue {:X}; native input "
                     "and the finished-picture pass use frame generation's",
                     (size_t) fg12->GameCommandQueue(), (size_t) state.currentCommandQueue);
        }

        gameQueue = fg12->GameCommandQueue();
    }

    // Native input runs here, on the game's picture and queue, ahead of frame generation and before its lock below: when it
    // presents to the virtual upscaler, that call feeds frame generation (UpscaleStart takes the same lock).
    //
    // Not under the Vulkan bridge's D3D12 swapchain: its picture belongs to the Vulkan driver (hooks/Vulkan_Hooks.cpp), which
    // runs the same step on the Vulkan game's presents. The interop state alone is not enough to tell: it is reset when a
    // device of the game's is destroyed, and this driver then ran on the bridge's own D3D12 device.
    if (willPresent && state.swapchainInteropApi == SwapchainInteropApi::None && gameQueue != nullptr &&
        state.currentD3D12Device != nullptr && !VkPresentBridge::OutputActive())
    {
        if (NativeMotionDx12::OnFGPresent(This, gameQueue, state.currentD3D12Device) != HoYoShade::ConsumerAccess::Safe)
            return E_FAIL; // no FG Prepare/NR/provider/overlay/real Present on unsafe color
    }

    IFGFeature* fg = state.currentFG;

    static const bool starRailExecutableForTiming =
        _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0;
    const bool starRailTimingRoute = willPresent && starRailExecutableForTiming &&
        !state.externalFrameGeneration && state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
        state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG &&
        This == state.currentFGSwapchain;
    if (starRailTimingRoute)
    {
        // The existing Genshin publisher rejects other executables. Publish the
        // same explicit default-off control once at this source-Present boundary.
        const auto controls=config->AcquireGenshinControls();
        PublishStarRailFgTiming(controls.hookCpuTiming);
    }
    const bool starRailTimingRequested=starRailTimingRoute && GenshinHookCpuTiming::Enabled();
    uint64_t starRailPresentSequence=0;
    if(starRailTimingRequested)
    {
        starRailPresentSequence=++starRailSourceSequence;
        starRailFgPacingWindow.Begin(GenshinHookCpuTiming::monitorEpoch.load(std::memory_order_acquire));
        // R897: drain prior completed rows at the outer admitted wrapper entry, outside owner scopes.
    }

    if (fg != nullptr && willPresent && fg->IsActive() && !fg->IsPaused())
    {
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            std::optional<double> upscalerTimeOpt {};

            // ReadUpscalerTime takes a void*: a plain D3D12 feature casts it to ID3D12CommandQueue*, so it must never get
            // the D3D11 context. Under Dx11wDx12 that feature is native::VirtualUpscalerDriver's, recorded on the queue
            // gameQueue names (WithDx12's paired one).
            const bool pureDx12Feature = currentFeature->Api() == API::DX12 && !currentFeature->IsWithDx12();

            if (pureDx12Feature && state.swapchainInteropApi != SwapchainInteropApi::None)
            {
                if (gameQueue != nullptr)
                {
                    if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(gameQueue); upscalerTimeOpt.has_value())
                        currentFeature->ReadDetailedGpuTimes(gameQueue, state.detailedGpuTimes);
                }
            }
            else if (state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 && state.currentD3D11Device != nullptr)
            {
                ID3D11DeviceContext* context = nullptr;
                state.currentD3D11Device->GetImmediateContext(&context);

                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(context); upscalerTimeOpt.has_value())
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);

                context->Release();
            }
            else if (state.swapchainInteropApi == SwapchainInteropApi::None && state.currentCommandQueue != nullptr)
            {
                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(state.currentCommandQueue);
                    upscalerTimeOpt.has_value())
                {
                    currentFeature->ReadDetailedGpuTimes(state.currentCommandQueue, state.detailedGpuTimes);
                }
            }

            if (upscalerTimeOpt.has_value())
            {
                auto upscalerTime = upscalerTimeOpt.value();
                // filter out possibly wrong measured high values
                if (upscalerTime < 100.0)
                {
                    State::Instance().frameTimeMutex.lock();
                    State::Instance().upscaleTimes.push_back(upscalerTime);
                    State::Instance().upscaleTimes.pop_front();
                    State::Instance().frameTimeMutex.unlock();
                }
            }
        }
    }

    // State transitions acquire FG Mutex -> continuity. Never call EvaluateState
    // from the adapter nested under the Present execution/owner2 guards below.
    if (willPresent) GenshinBridgeFg::PresentScope::Prepare(This);

    bool mutexUsed = false;
    native::presenttiming::NoteFrameGenerationMutexWait(0.0);
    if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
        config->FGUseMutexForSwapchain.value_or_default() && fg->Mutex.getOwner() != 2)
    {
        LOG_TRACE("Waiting FG->Mutex 2, current: {}", fg->Mutex.getOwner());
        const auto mutexWaitStartMs = Util::MillisecondsNow();
        fg->Mutex.lock(2);
        // For the stall timing: on the Vulkan game's present thread this is the wait inside the bridge's present
        native::presenttiming::NoteFrameGenerationMutexWait(Util::MillisecondsNow() - mutexWaitStartMs);
        mutexUsed = true;
        LOG_TRACE("Accuired FG->Mutex: {}", fg->Mutex.getOwner());
    }

    auto* executionOwner = dynamic_cast<IFGFeature_Dx12*>(fg);
    auto presentExecution = executionOwner != nullptr ? executionOwner->GuardExecution(FGExecutionScope::Present) : FGExecutionGuard {};
    // Off can prevent the Genshin adapter from entering EvaluateState. Apply
    // its provider transition under the existing execution guard as well.
    // CompletePendingTeardown below still owns the real drain/retirement.
    if (willPresent && !config->FGEnabled.value_or_default() && fg != nullptr && fg->IsActive())
    {
        if (auto* dlssg = dynamic_cast<DLSSG_Dx12*>(fg))
            dlssg->Deactivate();
    }
    // F5-DS scheduling is selected only by the exact StarRail producer route.
    // FinishedPicture NR remains an independent once-per-source consumer.
    auto* starRailDlssg = starRailTimingRoute ? dynamic_cast<DLSSG_Dx12*>(fg) : nullptr;
    const auto starRailSourcePresent = starRailDlssg ? starRailDlssg->PrepareStarRailPresent() :
        DLSSG_Dx12::StarRailPresent::Proceed;
    bool starRailSourceFallback = starRailSourcePresent == DLSSG_Dx12::StarRailPresent::NativeFallback;
    bool starRailSourceFailed = starRailSourcePresent == DLSSG_Dx12::StarRailPresent::Failed;
    const bool starRailF5DsScheduling = starRailDlssg && starRailDlssg->UsesStarRailF5DsScheduling();
    const auto fgPacingActive = [&]() {
        const bool runtimePacing = !starRailSourceFallback || starRailF5DsScheduling;
        return runtimePacing && !starRailSourceFailed && config->FGEnabled.value_or_default() &&
               fg != nullptr && state.currentFG == fg && fg->IsActive() && !fg->IsPaused();
    };
    const bool fgFeatureActive = fgPacingActive();
    // Inspect the provider only after the existing Present execution guard.
    // Disabled timing performs no added provider RTTI or metadata reads.
    const bool collectStarRailPacing = starRailTimingRequested && executionOwner != nullptr &&
        state.currentFG == fg && dynamic_cast<DLSSG_Dx12*>(fg) != nullptr;
    const auto starRailPacingContext=[&](const char* reason) {
        FgPacingContext c;
        if(collectStarRailPacing)
        {
            c.presentFrame=pacingPresentFrame; // guardFrame unavailable: LocalPresent does not share this guard.
            c.sourceSequence=starRailPresentSequence;
            c.providerKnown=fg!=nullptr && state.currentFG==fg;
            if(c.providerKnown)c.providerFrame=fg->FrameCount();
            c.active=fgPacingActive();
            c.preparedGeneratedCount=c.active?fg->GetInterpolatedFrameCount():0;
            c.capFps=config->FramerateLimit.value_or_default();c.reflexLimitsFps=state.reflexLimitsFps;
            c.reason=reason; // Snapshots at this API boundary, never effective-output proof.
            c.attribution=GenshinHookCpuTiming::MakeStarRailStamp(GenshinHookCpuTiming::StarRailOrigin::SdkPresent,This);
            c.attribution.sourceSequence=starRailPresentSequence;c.attribution.frame=pacingPresentFrame;
            c.attribution.chain=reinterpret_cast<uintptr_t>(This);c.attribution.presentQueue=reinterpret_cast<uintptr_t>(gameQueue);
            c.attribution.sync=SyncInterval;c.attribution.flags=Flags;c.attribution.flagsKnown=true;
        }
        return c;
    };

    const bool genshinDlssg = GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring()) &&
        state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG;
    const bool genshinSdkTransaction = willPresent && genshinDlssg && This==state.currentFGSwapchain &&
        GenshinBridgeFg::PresentScope::Current()!=nullptr;
    struct {
        bool submitted=false,ownMarkers=false,startCalled=false,endCalled=false,sleepCalled=false;
        bool beforeActive=false,beforePaused=false,afterActive=false,afterPaused=false,afterOwner=false;
        uint64_t frame=0,epoch=0,startToken=0,endToken=0,sleepToken=0;
        int startResult=-1,endResult=-1,sleepResult=-1; // -1: API not called
    } genshinReflex;
    sl::FrameToken* localToken = nullptr;
    sl::Result tokenResult = sl::Result::eErrorReflexAPI;
    if (willPresent && fg != nullptr && !fgFeatureActive)
        state.dlssgDetectedInterpolationCount = 0;

    if (willPresent && fgFeatureActive && state.activeFgOutput == FGOutput::DLSSG && !genshinSdkTransaction)
    {
        if ((!ReflexHooks::gameIsSendingMarkers() || !config->FGDLSSGUseGamesReflexMarkers.value_or_default()))
        {
            if (StreamlineProxy::PCLSetMarker() != nullptr)
            {
                ((IDXGISwapChain4*) This)->GetCurrentBackBufferIndex();
                const uint32_t frameId = (uint32_t) fg->FrameCount();
                tokenResult = StreamlineProxy::GetNewFrameToken()(localToken, &frameId);

                if (tokenResult == sl::Result::eOk && localToken != nullptr) {
                    auto markerStamp=collectStarRailPacing ? GenshinHookCpuTiming::MakeStarRailStamp(
                        GenshinHookCpuTiming::StarRailOrigin::FgPresentStart,This) : GenshinHookCpuTiming::StarRailStamp{};
                    if(collectStarRailPacing){
                        markerStamp.sourceSequence=starRailPresentSequence;markerStamp.frame=frameId;
                        markerStamp.chain=reinterpret_cast<uintptr_t>(This);markerStamp.marker=static_cast<uint32_t>(sl::PCLMarker::ePresentStart);
                        markerStamp.markerFrame=static_cast<uint64_t>(*localToken);markerStamp.markerFrameKnown=true;
                    }
                    GenshinHookCpuTiming::Scope markerTiming(GenshinHookCpuTiming::Stage::FgPclMarker,collectStarRailPacing);
                    const auto markerResult=StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentStart, *localToken);
                    const auto markerWall=markerTiming.Finish();
                    if(collectStarRailPacing){
                        markerStamp.result=static_cast<uint32_t>(markerResult);markerStamp.resultKnown=true;
                        GenshinHookCpuTiming::RecordStarRailCall(GenshinHookCpuTiming::StarRailCall::PclMarker,markerStamp,markerWall);
                    }
                }
            }
        }
    }

    const bool fgPresentAdmitted = willPresent && fg != nullptr &&
        (fgFeatureActive || starRailSourceFallback || (genshinDlssg && GenshinBridgeFg::PresentScope::Current()));
    static const bool zzzExecutable =
        _wcsicmp(Util::ExePath().filename().c_str(), L"ZenlessZoneZero.exe") == 0;
    // This is the game's source Present on its real queue, before the provider
    // creates physical/generated children. NR owns a successful SR receipt even
    // when FG is Off/paused/refused; provider activity cannot revoke that consumer.
    // Late validates the source/endpoint/device and claims an attempt before any
    // private recording, so the wrapped output/child Presents cannot run NR again.
    const bool zzzNrSourcePresent = willPresent && zzzExecutable && fg != nullptr &&
        !state.externalFrameGeneration && state.swapchainInteropApi == SwapchainInteropApi::None &&
        state.activeFgInput == FGInput::Upscaler && state.activeFgOutput == FGOutput::DLSSG &&
        This == state.currentFGSwapchain && gameQueue != nullptr && state.currentD3D12Device != nullptr &&
        !VkPresentBridge::OutputActive() && config->DlssNrEnabled.value_or_default() &&
        config->DlssNrFinishedPicture.value_or_default() && !config->DlssNrRunBeforeSr.value_or_default() &&
        !config->DlssNrDeferredDlss.value_or_default();
    if (fgPresentAdmitted || zzzNrSourcePresent)
    {
        if (fgPresentAdmitted && state.activeFgInput == FGInput::FSRFG)
            ffxPresentCallback();
        else if (fgPresentAdmitted && state.activeFgInput == FGInput::FSRFG30)
            FSR3FG::ffxPresentCallback();

        const auto finishedPicture = DlssNr::ApplyToFinishedPicture(This, gameQueue);
        if (HoYoShade::ColorConsumers(This) != HoYoShade::ConsumerAccess::Safe) return E_FAIL;
        if (fgPresentAdmitted && genshinDlssg)
        {
            // The adapter performs real SetResource and provider Present only
            // after the exact final-color/producer/NR join. No observation bool
            // or scalar-state transition is a replacement for that work.
            const bool bridgeSubmitted=GenshinBridgeFg::PresentScope::Dispatch(finishedPicture);
            if(genshinSdkTransaction){
                genshinReflex.submitted=bridgeSubmitted;
                genshinReflex.epoch=GenshinBridgeFg::PresentScope::Current()->ObservedPresentEpoch();
                genshinReflex.frame=fg->FrameCount();
                genshinReflex.ownMarkers=!ReflexHooks::gameIsSendingMarkers() ||
                    !config->FGDLSSGUseGamesReflexMarkers.value_or_default();
                // Dispatch applies this frame's Reflex On options. A marker
                // sent before it can observe the previous Off state at startup
                // or input-gap recovery. Keep its token through End and Sleep.
                if(bridgeSubmitted && genshinReflex.ownMarkers){
                    const auto getToken=StreamlineProxy::GetNewFrameToken();
                    const auto marker=StreamlineProxy::PCLSetMarker();
                    if(getToken && marker){
                        ((IDXGISwapChain4*)This)->GetCurrentBackBufferIndex();
                        const uint32_t frameId=static_cast<uint32_t>(genshinReflex.frame);
                        tokenResult=getToken(localToken,&frameId);
                        if(tokenResult==sl::Result::eOk && localToken &&
                            genshinReflex.frame<=UINT32_MAX && static_cast<uint64_t>(*localToken)==genshinReflex.frame){
                            genshinReflex.startToken=static_cast<uint64_t>(*localToken);
                            genshinReflex.startCalled=true;
                            genshinReflex.startResult=static_cast<int>(marker(sl::PCLMarker::ePresentStart,*localToken));
                        }
                    }
                }
            }
            if (!bridgeSubmitted)
                LOG_TRACE("Genshin FG skipped: no authenticated same-present input/outcome transaction");
            if(const auto* scope=GenshinBridgeFg::PresentScope::Current()) {
                static unsigned dispatchReports=0;if(dispatchReports<32){++dispatchReports;
                    LOG_INFO("R827 FG dispatch: pid={} scope_epoch={} current_epoch={} prepare={} stage={} result={} submitted={} no_authority=true",
                        GetCurrentProcessId(),scope->ObservedPresentEpoch(),state.frameCount,scope->PrepareStage(),scope->DispatchStage(),
                        static_cast<unsigned>(scope->LastResult()),bridgeSubmitted);
                }
            }
        }
        else if (fgPresentAdmitted && !starRailSourceFallback)
            fg->Present();
    }
    else if (willPresent && fg != nullptr)
    {
        LOG_TRACE("FGHooks::FGPresent: FG feature exists but is inactive/paused; pass-through present only");
    }

    // The captured StarRail physical output may run on the SDK worker while
    // this source Present holds continuity and waits inside o_FGSCPresent.
    // Own cap updates here, after Dispatch applied its count; the authenticated
    // physical endpoint must not re-enter the provider from that worker.
    // This route is independent of the default-off timing control above.
    if (starRailTimingRoute && executionOwner != nullptr && state.currentFG == fg &&
        dynamic_cast<DLSSG_Dx12*>(fg) != nullptr)
        ReflexHooks::update(fgPacingActive(), false);

    if (willPresent && state.swapchainInteropApi == SwapchainInteropApi::None)
    {
        ResTrack_Dx12::ClearPossibleHudless();
        Hudfix_Dx12::PresentStart();
    }

    if (willPresent && config->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  config->ForceVsync.value(), config->VsyncInterval.value_or_default(), state.SCAllowTearing,
                  state.realExclusiveFullscreen);

        if (!config->ForceVsync.value())
        {
            SyncInterval = 0;

            if (state.SCAllowTearing && !state.realExclusiveFullscreen)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            SyncInterval = config->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            // XeFG runs with XeLL, and XeLL needs sync interval 1 with VSync on (Intel's XeLL guide)
            if (SyncInterval > 1 && state.activeFgOutput == FGOutput::XeFG)
            {
                static bool logged = false;

                if (!logged)
                {
                    logged = true;
                    LOG_WARN("VsyncInterval {} with XeFG: using 1, XeLL needs sync interval 1", SyncInterval);
                }

                SyncInterval = 1;
            }

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    // Used at wrapped_swapchain LocalPresent to determine is frame is interpolated or not
    if (willPresent)
        state.fgPresentIsCalled = true;

    // Optical F5Low's low latency (native/NativeLowLatency.h): this is the game's present; the wrapped swap chain's
    // presents behind it are frame generation's own. (A D3D11 game's is Dx11wDx12SC::Present.)
    IUnknown* lowLatencyDevice = nullptr;

    if (willPresent && state.swapchainInteropApi == SwapchainInteropApi::None)
    {
        lowLatencyDevice = state.currentD3D12Device;

        if (lowLatencyDevice != nullptr)
            native::lowlatency::OnPresentBegin(lowLatencyDevice, native::lowlatency::PresentSource::FrameGeneration);
    }

    // Draw once on the real FG backbuffer before the provider takes it. GetCommandQueue() is the
    // queue passed to the FG swapchain at creation; the DX11 bridge orders its copy onto it first.
    // Inner wrapped presents for this window leave menu ownership here, including system DXGI.
    // Keep menu CPU/GPU submission outside the provider Present timing. TEST never renders.
    // Wrapped DLSSG output is presented once per displayed/generated frame on its captured queue.
    // Leave the input backbuffer clean on that route; plain/unwrapped providers keep the S fallback.
    MenuOverlayDx::PresentResult overlayResult;
    const bool menuOwner = willPresent && This == state.currentFGSwapchain && OwnsMenuOverlay(_hwnd);
    const bool wrappedMenuOutput = menuOwner && HasWrappedMenuOutput(_hwnd);
    IUnknown* menuIncomingQueue = menuOwner ? state.currentFG->GetCommandQueue() : nullptr;
    overlayResult.Decision = "fg-owner-excluded";
    if (menuOwner && !wrappedMenuOutput)
        MenuOverlayDx::Present(This, SyncInterval, Flags, pPresentParameters,
                               menuIncomingQueue, _hwnd, false, false, &overlayResult);
    // Same conservative policy as the wrapped physical owner. Truly incremental
    // dirty frames require coherent background reconstruction before widening.
    DXGI_PRESENT_PARAMETERS overlayPresentParameters {};
    const DXGI_PRESENT_PARAMETERS* effectivePresentParameters = pPresentParameters;
    bool overlayScrollSuppressed = false;
    if (overlayResult.OverlayWritten && pPresentParameters != nullptr && pPresentParameters->DirtyRectsCount == 0 &&
        (pPresentParameters->pScrollRect != nullptr || pPresentParameters->pScrollOffset != nullptr))
    {
        overlayPresentParameters = *pPresentParameters;
        overlayPresentParameters.pScrollRect = nullptr;
        overlayPresentParameters.pScrollOffset = nullptr;
        effectivePresentParameters = &overlayPresentParameters;
        overlayScrollSuppressed = true;
    }

    HRESULT result;
    const auto presentStartMs = Util::MillisecondsNow();
    // Last admission consumes one real Dispatch or advances F5-DS's original
    // missing-source Present counter. No old guide is tagged or dispatched.
    if (!starRailSourceFailed && starRailDlssg)
    {
        const bool pacingBeforeAdmission = fgPacingActive();
        const auto admission = starRailDlssg->PrepareStarRailPresent(true);
        const bool newlySuppressed = !starRailSourceFallback &&
            admission == DLSSG_Dx12::StarRailPresent::NativeFallback;
        starRailSourceFailed = admission == DLSSG_Dx12::StarRailPresent::Failed;
        starRailSourceFallback = admission == DLSSG_Dx12::StarRailPresent::NativeFallback;
        if (starRailF5DsScheduling ? pacingBeforeAdmission != fgPacingActive() : newlySuppressed)
            ReflexHooks::update(fgPacingActive(), false);
    }
    if(genshinSdkTransaction){
        genshinReflex.beforeActive=fgPacingActive();
        genshinReflex.beforePaused=fg && state.currentFG==fg && fg->IsPaused();
    }
    framegen::final_output::InputScope physicalInput(This, Flags, effectivePresentParameters);
    auto starRailSdkContext=starRailPacingContext("before-sdk-present");
    if(collectStarRailPacing){
        starRailSdkContext.attribution.sync=SyncInterval;starRailSdkContext.attribution.flags=Flags;
    }
    if(willPresent && genshinDlssg) {
        auto* current=GenshinBridgeFg::PresentScope::Current();
        // The accepted PresentStart marker selects this SDK input token.
        // NV_ASYNC_FRAME_MARKER_PARAMS has queue/frameID/presentFrameID, but
        // the public header does not bind either ID to this source receipt or
        // guarantee a same-thread raw bracket. Never export this TLS as an
        // asynchronous source association or use GetState's input fence for it.
        const uint64_t markerFrame=genshinReflex.startCalled &&
            genshinReflex.startResult==static_cast<int>(sl::Result::eOk) && localToken &&
            static_cast<uint64_t>(*localToken)==genshinReflex.startToken ? genshinReflex.startToken : 0;
        GenshinVisibleChild::PresentPermit::BeforePresent(This,
            current ? current->EnterSdkPresent(This,markerFrame) : nullptr);
    }
    GenshinHookCpuTiming::Scope sdkPresentTiming(GenshinHookCpuTiming::Stage::FgSdkPresent,
                                               collectStarRailPacing && !starRailSourceFailed);
    if (starRailSourceFailed)
        result = DXGI_ERROR_INVALID_CALL; // Unknown Off/tag outcome cannot consume stale guides.
    else if (pPresentParameters == nullptr)
        result = o_FGSCPresent(This, SyncInterval, Flags);
    else
        result = o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, effectivePresentParameters);
    const auto starRailSdkResult=static_cast<uint32_t>(result); // Freeze the actual SDK return before postwork.
    sdkPresentTiming.Returned(starRailSdkResult,GenshinHookCpuTiming::ResultKind::Hresult);
    const auto starRailSdkWall=sdkPresentTiming.Finish(); // Before logging/overlay postwork/Reflex.
    if(collectStarRailPacing && !starRailSourceFailed){
        starRailSdkContext.attribution.result=starRailSdkResult;starRailSdkContext.attribution.resultKnown=true;
        GenshinHookCpuTiming::RecordStarRailCall(GenshinHookCpuTiming::StarRailCall::SdkPresent,starRailSdkContext.attribution,starRailSdkWall);
    }

    if (willPresent && genshinDlssg) {
        if(auto* current=GenshinBridgeFg::PresentScope::Current())current->ReturnSdkPresent(This,result);
        GenshinVisibleChild::PresentPermit::AfterPresent(This, result);
    }
    if(genshinSdkTransaction){
        genshinReflex.afterOwner=fg && state.currentFG==fg;
        genshinReflex.afterActive=fgPacingActive();
        genshinReflex.afterPaused=genshinReflex.afterOwner && fg->IsPaused();
    }

    if (willPresent)
    {
        static std::atomic<UINT64> calls { 0 }, written { 0 }, skipped { 0 };
        static thread_local const char* lastDecision = nullptr;
        static thread_local bool lastWritten = false;
        static thread_local ULONGLONG lastAt = 0;
        const UINT64 count = ++calls;
        if (overlayResult.OverlayWritten) ++written; else ++skipped;
        const ULONGLONG now = GetTickCount64();
        const bool changed = lastDecision != overlayResult.Decision || lastWritten != overlayResult.OverlayWritten;
        if (count <= 12 || count % 180 == 0 || (changed && now - lastAt >= 250))
        {
            lastDecision = overlayResult.Decision;
            lastWritten = overlayResult.OverlayWritten;
            lastAt = now;
            LOG_INFO("Overlay FG-owner evidence: pid={} tick={} calls={} written={} skipped={} sc={:X} incomingQueue={:X} queue={:X} window={:X} owner={} wrapped={} decision={} overlayWritten={} index={} slot={} busySlots={} route={} refresh={} generation={}/{} epoch={} completion={} fence={}/{} bufferFence={} drain={}:{} wait={} present1={} dirty={} scroll={}/{} scrollSuppressed={} sync={} flags={:X} hr={:X}",
                     GetCurrentProcessId(), now, count, written.load(), skipped.load(), (size_t)This,
                     (size_t)menuIncomingQueue, (size_t)overlayResult.Queue, (size_t)_hwnd, menuOwner, wrappedMenuOutput, overlayResult.Decision,
                     overlayResult.OverlayWritten, overlayResult.BackbufferIndex, overlayResult.ReplaySlot,
                     overlayResult.BusySlots, overlayResult.RouteReason, overlayResult.RefreshReason,
                     overlayResult.Generation, overlayResult.SnapshotGeneration, overlayResult.Epoch,
                     overlayResult.CompletionState, overlayResult.CompletedFence, overlayResult.SubmittedFence,
                     overlayResult.BackbufferFence, overlayResult.DrainPending, overlayResult.DrainFence,
                     overlayResult.WaitAttempted, pPresentParameters != nullptr,
                     pPresentParameters != nullptr ? pPresentParameters->DirtyRectsCount : 0,
                     pPresentParameters != nullptr && pPresentParameters->pScrollRect != nullptr,
                     pPresentParameters != nullptr && pPresentParameters->pScrollOffset != nullptr,
                     overlayScrollSuppressed, SyncInterval, Flags, (unsigned long)result);
        }
    }

    // Where a stall sits when the game's present is held up inside frame generation's own (the game's present thread
    // only; native/PresentStageTiming.h). Only for the Vulkan bridge: other games log nothing of this.
    if (willPresent &&
        native::presenttiming::FrameGenerationTimingWanted(state.swapchainInteropApi == SwapchainInteropApi::VkwDx12))
    {
        static native::presenttiming::StageTiming presentTiming;

        const auto now = Util::MillisecondsNow();
        presentTiming.Record(native::presenttiming::Stage::FrameGenerationPresent, now - presentStartMs);
        presentTiming.Record(native::presenttiming::Stage::FrameGenerationMutexWait,
                             native::presenttiming::PeekFrameGenerationMutexWait());
        const auto outcome = presentTiming.EndPresent(now);

        if (outcome.stall)
            LOG_WARN("Frame generation present stall: {}", presentTiming.StallLine());

        if (outcome.summary)
        {
            LOG_INFO("Frame generation present timing, {}", presentTiming.SummaryLine());
            presentTiming.Restart();
        }
    }

    if (result == S_OK)
    {
        LOG_DEBUG("Result: {:X}", result);
    }
    else
    {
        if (result == DXGI_ERROR_DEVICE_REMOVED && state.currentD3D12Device != nullptr)
            Util::GetDeviceRemovedReason(state.currentD3D12Device);
    }

    auto starRailReflexContext=starRailPacingContext("reflex-branch-skipped");
    bool starRailReflexCalled=false;
    GenshinHookCpuTiming::WallSample starRailReflexWall;
    const bool genshinPacing = willPresent && GenshinFsr::IsGenshinExecutable(Util::ExePath().wstring());
    if (tokenResult == sl::Result::eOk && localToken != nullptr &&
        (genshinSdkTransaction ? genshinReflex.startCalled : fgFeatureActive) &&
        (genshinSdkTransaction ? genshinReflex.ownMarkers :
            (!ReflexHooks::gameIsSendingMarkers() || !config->FGDLSSGUseGamesReflexMarkers.value_or_default())) &&
        willPresent && state.activeFgOutput == FGOutput::DLSSG)
    {
        if (StreamlineProxy::PCLSetMarker() != nullptr) {
            auto markerStamp=collectStarRailPacing ? GenshinHookCpuTiming::MakeStarRailStamp(
                GenshinHookCpuTiming::StarRailOrigin::FgPresentEnd,This) : GenshinHookCpuTiming::StarRailStamp{};
            if(collectStarRailPacing){
                markerStamp.sourceSequence=starRailPresentSequence;markerStamp.frame=pacingPresentFrame;
                markerStamp.chain=reinterpret_cast<uintptr_t>(This);markerStamp.marker=static_cast<uint32_t>(sl::PCLMarker::ePresentEnd);
                markerStamp.markerFrame=static_cast<uint64_t>(*localToken);markerStamp.markerFrameKnown=true;
            }
            GenshinHookCpuTiming::Scope markerTiming(GenshinHookCpuTiming::Stage::FgPclMarker,collectStarRailPacing);
            if(genshinSdkTransaction)genshinReflex.endToken=static_cast<uint64_t>(*localToken);
            const auto markerResult=StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentEnd, *localToken);
            const auto markerWall=markerTiming.Finish();
            if(genshinSdkTransaction){
                genshinReflex.endCalled=true;genshinReflex.endResult=static_cast<int>(markerResult);
            }
            if(collectStarRailPacing){
                markerStamp.result=static_cast<uint32_t>(markerResult);markerStamp.resultKnown=true;
                GenshinHookCpuTiming::RecordStarRailCall(GenshinHookCpuTiming::StarRailCall::PclMarker,markerStamp,markerWall);
            }
        }

        // Genshin's FG execution and continuity locks must not cover a driver
        // pacing wait. The marker and native->child submission order above stay
        // unchanged; only the blocking Sleep is deferred until both unlock.
        if (!genshinPacing && fgPacingActive())
        {
            LOG_DEBUG("Calling ReflexSleep");
            starRailReflexContext=starRailPacingContext("before-reflex-sleep");
            GenshinHookCpuTiming::Scope reflexTiming(GenshinHookCpuTiming::Stage::FgReflexSleep,collectStarRailPacing);
            StreamlineProxy::ReflexSleep()(*localToken);
            starRailReflexWall=reflexTiming.Finish();
            starRailReflexCalled=true;
        }
    }

    if (state.swapchainInteropApi == SwapchainInteropApi::None)
        Hudfix_Dx12::PresentEnd();

    bool deferStarRailLimiter = false;
    // F5-DS keeps the existing FrameLimit decision, with no new 200-fps cap.
    // Its actual wait remains after owner unlock to preserve current safety.
    // A dxvk game's frame generation through the Vulkan present bridge (NativeDxvkVulkan) presents a real D3D12 swapchain
    // here, as a native Vulkan game's does: the limiter applies to it.
    if (willPresent && !state.reflexLimitsFps && !native::lowlatency::OwnsFrameCap() &&
        state.activeFgOutput != FGOutput::NoFG &&
        (!IdentifyGpu::getPrimaryGpu().usesDxvk || VkPresentBridge::DxvkThroughVulkan()) && !XellHooks::canLimit())
    {
        if (!genshinPacing)
        {
            static const bool starRailExecutable =
                _wcsicmp(Util::ExePath().filename().c_str(), L"StarRail.exe") == 0;
            deferStarRailLimiter = starRailExecutable &&
                state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
                state.activeFgOutput == FGOutput::DLSSG && dynamic_cast<DLSSG_Dx12*>(fg) != nullptr;
            if (!deferStarRailLimiter)
            {
                const bool pacingActive = fgPacingActive();
                const auto generatedFrames = pacingActive ? fg->GetInterpolatedFrameCount() : 0;
                FrameLimit::sleep(pacingActive, generatedFrames);
            }
        }
    }

    if ((config->SimulateWaitableObject.value_or_default() ||
         (state.gameEngine == GameEngineType::Unity && state.activeFgOutput == FGOutput::XeFG)) &&
        _semaphore != nullptr)
    {
        ReleaseSemaphore(_semaphore, 1, nullptr);
    }

    presentExecution.FinishWork();
    if (willPresent && executionOwner != nullptr && !executionOwner->CompletePendingTeardown())
        LOG_WARN("F5 FG pending teardown incomplete; generation remains blocked");

    // Teardown can itself deactivate the provider. Snapshot scheduling only
    // after that boundary, while its guard/FG mutex are still held. No provider
    // pointer is dereferenced for pacing after those guards are released.
    const bool fgPacingAfterPresent = fgPacingActive();
    const auto fgPacingGeneratedFrames = fgPacingAfterPresent ? fg->GetInterpolatedFrameCount() : 0;
    // All provider reads stay inside the existing guard, after teardown. The
    // optional scope epoch is its real Present epoch, never a fabricated provider epoch.
    FgPacingContext pacingContext;
    const bool collectPacing = genshinPacing && GenshinHookCpuTiming::Enabled();
    if (collectPacing)
    {
        fgPacingWindow.Begin(GenshinHookCpuTiming::monitorEpoch.load(std::memory_order_acquire));
        pacingContext.presentFrame = pacingPresentFrame;
        pacingContext.guardFrame = state.frameCount;
        pacingContext.providerKnown = fg != nullptr && state.currentFG == fg;
        if (pacingContext.providerKnown) pacingContext.providerFrame = fg->FrameCount();
        if (const auto* scope = GenshinBridgeFg::PresentScope::Current())
        {
            pacingContext.epochKnown = true;
            pacingContext.presentEpoch = scope->ObservedPresentEpoch();
        }
        pacingContext.active = fgPacingAfterPresent;
        pacingContext.preparedGeneratedCount = fgPacingGeneratedFrames;
    }
    if (genshinPacing) presentExecution.Finish();

    if (mutexUsed && fg != nullptr)
    {
        LOG_TRACE("Releasing FG->Mutex: {}", fg->Mutex.getOwner());
        fg->Mutex.unlockThis(2);
    }

    if (deferStarRailLimiter)
    {
        // Preserve explicit user caps and other StarRail backends at their old boundary.
        presentExecution.Finish();
        FrameLimit::sleep(fgPacingAfterPresent, fgPacingGeneratedFrames);
    }
    if (starRailF5DsScheduling) presentExecution.Finish();

    if (genshinPacing)
    {
        // Keep the original short-circuit order; observe only values the real
        // branches evaluate. In particular, do not ask extra cap/driver APIs.
        const char* reflexReason = "UNKNOWN";
        const bool callReflex = [&]() {
            if (tokenResult != sl::Result::eOk) { reflexReason = "token-result"; return false; }
            if (localToken == nullptr) { reflexReason = "no-token"; return false; }
            if (!fgPacingAfterPresent) { reflexReason = "fg-inactive-guard"; return false; }
            if (!config->FGEnabled.value_or_default()) { reflexReason = "fg-disabled"; return false; }
            if (genshinSdkTransaction ? !genshinReflex.ownMarkers :
                (ReflexHooks::gameIsSendingMarkers() && config->FGDLSSGUseGamesReflexMarkers.value_or_default()))
            { reflexReason = "game-markers"; return false; }
            if (state.activeFgOutput != FGOutput::DLSSG) { reflexReason = "not-dlssg"; return false; }
            if(genshinSdkTransaction && StreamlineProxy::ReflexSleep()==nullptr)
            { reflexReason = "no-sleep-api"; return false; }
            reflexReason = "called";return true;
        }();
        FgPacingContext reflexContext = pacingContext;
        if (collectPacing)
        {
            reflexContext.capFps = config->FramerateLimit.value_or_default();
            reflexContext.reflexLimitsFps = state.reflexLimitsFps;
            // This path can sleep for low latency without owning the FPS cap.
            reflexContext.reason = reflexReason;
        }
        GenshinHookCpuTiming::WallSample reflexWall;
        if (callReflex)
        {
            LOG_DEBUG("Calling ReflexSleep after Genshin FG unlock");
            GenshinHookCpuTiming::Scope timing(GenshinHookCpuTiming::Stage::FgReflexSleep,collectPacing);
            if(genshinSdkTransaction)genshinReflex.sleepToken=static_cast<uint64_t>(*localToken);
            const auto sleepResult=StreamlineProxy::ReflexSleep()(*localToken);
            reflexWall = timing.Finish();
            if(genshinSdkTransaction){
                genshinReflex.sleepCalled=true;genshinReflex.sleepResult=static_cast<int>(sleepResult);
            }
        }

        FgPacingContext limitContext = pacingContext;
        const bool callLimiter = [&]() {
            limitContext.reflexLimitsFps = state.reflexLimitsFps;
            if (limitContext.reflexLimitsFps) { limitContext.capOwner = "Reflex"; limitContext.reason = "reflex-cap"; return false; }
            const bool nativeOwns = native::lowlatency::OwnsFrameCap();
            limitContext.nativeOwnsCap = nativeOwns ? 1 : 0;
            if (nativeOwns) { limitContext.capOwner = "NativeLowLatency"; limitContext.reason = "native-cap"; return false; }
            if (state.activeFgOutput == FGOutput::NoFG) { limitContext.reason = "no-fg-output"; return false; }
            if (IdentifyGpu::getPrimaryGpu().usesDxvk && !VkPresentBridge::DxvkThroughVulkan())
            { limitContext.reason = "dxvk-excluded"; return false; }
            const bool xellCanLimit = XellHooks::canLimit();
            limitContext.xellCanLimit = xellCanLimit ? 1 : 0;
            if (xellCanLimit) { limitContext.capOwner = "Xell"; limitContext.reason = "xell-cap"; return false; }
            limitContext.capOwner = "FrameLimit";limitContext.reason = "called";return true;
        }();
        if (collectPacing)
        {
            limitContext.capFps = config->FramerateLimit.value_or_default();
            if (callLimiter && !(limitContext.capFps > 0.0f))
            {
                limitContext.capOwner = "no-configured-cap";
                limitContext.reason = "called-cap-disabled";
            }
            // Bind the actual same-Present cap selection to either sampled
            // call. The log names its phase; it is not a pre-Reflex API query.
            reflexContext.capOwner = limitContext.capOwner;
            reflexContext.nativeOwnsCap = limitContext.nativeOwnsCap;
            reflexContext.xellCanLimit = limitContext.xellCanLimit;
        }
        GenshinHookCpuTiming::WallSample limiterWall;
        if (callLimiter)
        {
            GenshinHookCpuTiming::Scope timing(GenshinHookCpuTiming::Stage::FgFrameLimitSleep,collectPacing);
            FrameLimit::sleep(fgPacingAfterPresent, fgPacingGeneratedFrames);
            limiterWall = timing.Finish();
        }
        if (collectPacing)
        {
            fgPacingWindow.rows[0].Record(reflexContext,callReflex,reflexWall);
            fgPacingWindow.rows[1].Record(limitContext,callLimiter,limiterWall);
            fgPacingWindow.Emit(); // Piggyback the existing R448 Present drain, at most once/second.
        }
    }

    if(genshinSdkTransaction){
        // No new state query or completion claim. Preserve the actual SDK and
        // same-token API results, including failed/absent calls after unlock.
        static thread_local uint64_t calls=0,lastTick=0,lastFlags=UINT64_MAX;
        static thread_local unsigned burst=0;
        static thread_local int lastStart=-2,lastEnd=-2,lastSleep=-2,lastToken=-2;
        static thread_local uint32_t lastSdk=0;
        const auto& r=genshinReflex;
        const uint64_t bits=uint64_t(r.submitted)|(uint64_t(r.ownMarkers)<<1)|(uint64_t(r.startCalled)<<2)|
            (uint64_t(r.endCalled)<<3)|(uint64_t(r.sleepCalled)<<4)|(uint64_t(r.beforeActive)<<5)|
            (uint64_t(r.beforePaused)<<6)|(uint64_t(r.afterActive)<<7)|(uint64_t(r.afterPaused)<<8)|
            (uint64_t(r.afterOwner)<<9)|(uint64_t(fgPacingAfterPresent)<<10);
        const auto now=GetTickCount64();const bool initial=++calls<=4;
        if(now-lastTick>=1000)burst=0;
        const bool changed=bits!=lastFlags || r.startResult!=lastStart || r.endResult!=lastEnd ||
            r.sleepResult!=lastSleep || static_cast<int>(tokenResult)!=lastToken || starRailSdkResult!=lastSdk;
        if(initial || now-lastTick>=5000 || (changed && burst<4)){
            if(!burst)lastTick=now;
            ++burst;lastFlags=bits;lastStart=r.startResult;lastEnd=r.endResult;lastSleep=r.sleepResult;
            lastToken=static_cast<int>(tokenResult);lastSdk=starRailSdkResult;
            LOG_INFO("R1069 sdk-reflex: pid={} thread={} chain={:X} input_queue_ptr={:X} provider_frame={} present_epoch={} submitted={} own_markers={} token_result={} start_called={} start_token={} start_result={} end_called={} end_token={} end_result={} sleep_called={} sleep_token={} sleep_result={} start_boundary=after_dispatch sleep_boundary=after_unlock before_sdk_active={} before_sdk_paused={} after_sdk_owner_current={} after_sdk_active={} after_sdk_paused={} after_teardown_active={} sync={} sdk_flags={:X} sdk_hr={:X} api_not_called=-1 generated=UNKNOWN visual=UNKNOWN",
                GetCurrentProcessId(),GetCurrentThreadId(),reinterpret_cast<uintptr_t>(This),
                reinterpret_cast<uintptr_t>(gameQueue),r.frame,r.epoch,r.submitted,r.ownMarkers,
                static_cast<int>(tokenResult),r.startCalled,r.startToken,r.startResult,r.endCalled,r.endToken,
                r.endResult,r.sleepCalled,r.sleepToken,r.sleepResult,r.beforeActive,r.beforePaused,r.afterOwner,
                r.afterActive,r.afterPaused,fgPacingAfterPresent,SyncInterval,Flags,starRailSdkResult);
        }
    }
    if(collectStarRailPacing)
    {
        // Fixed TLS scalars only; no logging, provider/token access or new lock.
        // Independent stride samples pair only when sourceSequence matches.
        starRailFgPacingWindow.rows[0].Record(starRailReflexContext,starRailReflexCalled,starRailReflexWall);
        if (!starRailSourceFailed) starRailFgPacingWindow.rows[1].Record(starRailSdkContext,true,starRailSdkWall,starRailSdkResult,true);
    }

    // After frame generation's lock is released: Sleep holds this thread until the next frame should start
    if (lowLatencyDevice != nullptr)
        native::lowlatency::OnPresentEnd(lowLatencyDevice, native::lowlatency::PresentSource::FrameGeneration);

    LOG_DEBUG("Present finished");

    return result;
}

ULONG FGHooks::hkFGRelease(IUnknown* This)
{
    // A contended/internal/foreign/shutdown call STILL consumes exactly its incoming caller reference.
    // The existing F5 pin protects an active owner; probes below are balanced, never caller refs.
    if (fgLifecycleCallback) return o_FGRelease(This);
    std::unique_lock lifetime(fgLifetimeMutex, std::try_to_lock);
    if (!lifetime.owns_lock() || !fgOwner || fgOwner->quarantined || fgOwner->pin != This || State::Instance().isShuttingDown)
        return o_FGRelease(This);

    // Consume exactly the incoming caller ref. Pending/refusal retains our one
    // F5 pin; neither result is an excuse to consume another caller reference.
    auto owner = fgOwner;
    ScopedFGCallback callback;
    const auto remaining = o_FGRelease(This);
    if (!remaining)
    {
        owner->quarantined = true; // Broken ownership/count contract: never dereference the chain again.
        return remaining;
    }
    auto references = remaining;
    if (owner->retireStage == FGSwapchainOwner::RetireStage::Idle)
    {
        // An already-requested provider drain may itself hold input leases and
        // chain refs. Finish that exact terminal target BEFORE count gating.
        // This does not deactivate an otherwise active provider on every Release.
        const auto completion = CompleteExistingFGTeardown(owner, true);
        if (completion.state != FGHookRetirementState::Completed) return remaining;
        owner->pin->AddRef();
        references = o_FGRelease(owner->pin); // Balance the probe; refresh after internal lease retirement.
        if (!references) { owner->quarantined = true; return references; }
        if (Config::Instance()->FGPreserveSwapChain.value_or_default() || references != 1) return references;
    }
    const auto result = RetireFGResources(owner, true); // Propagated to the provider's bounded terminal drain.
    if (result.state != FGHookRetirementState::Completed) return remaining;
    owner->pin->AddRef();
    references = o_FGRelease(owner->pin); // Balance only the final F5 probe.
    if (references != 1)
    {
        owner->retirement = {}; // Release the current-thread execution lease before returning.
        if (!references) owner->quarantined = true;
        return references;
    }
    return ReleaseFGPin(owner, o_FGRelease); // Exactly one F5 pin; never loop to guessed zero.
}
