#include <pch.h>
#include <native/OpticalFgNrPolicy.h>
#include <integration/HoYoShade.h>
#include "IFeature_Dx11wDx12.h"
#include <genshin/GenshinFsrSubstageProgress.h>
#include <genshin/GenshinLegacyCapture.h>
#include "GameRoutePolicy.h"
#include "Dx11BridgeStateGuard.h"
#include "BridgeResourceParameterGuard.h"
#include "NgxOptionalDx12Inputs.h"
#include "CallerCpuTrace.h"
#include <genshin/GenshinBridgeConsumer.h>
#include <genshin/GenshinCpuStageTiming.h>
#include <genshin/GenshinHookCpuTiming.h>
#include <optional>
#include <format>

#include <dlssnr/DlssNr.h>

#include <Util.h>
#include <Config.h>

#include <proxies/DXGI_Proxy.h>
#include <proxies/D3D12_Proxy.h>
#include <misc/IdentifyGpu.h>

#include <with_dx12/with_dx12.h>

static_assert(DX11WDX12_COMMAND_BUFFER_COUNT == RCAS_Dx12::RecordingSlotCount,
              "Bridge command lists and RCAS recording resources must have the same slot count");

void IFeature_Dx11wDx12::ResourceBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

// Failed submitted creations remain process-pinned: no provider termination
// contract is assumed from a timeout, a device-loss sentinel or a COM reference.
struct Dx11BridgeInitRetention
{
    bool claimed = false;
    Dx11BridgeInitRetention* next = nullptr;
    std::unique_ptr<IFeature_Dx12> feature;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    GenshinTransport::GpuWorkDomain workDomain; // Both roles and creation receipts survive quarantine.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators[DX11WDX12_COMMAND_BUFFER_COUNT];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> lists[DX11WDX12_COMMAND_BUFFER_COUNT];
    HANDLE event = nullptr;
};
namespace
{
struct InitRetentionSlots
{
    std::mutex mutex;
    Dx11BridgeInitRetention slots[2];
};
InitRetentionSlots& RetentionSlots()
{
    // Intentionally no static destructor: process-exit/unload is not a GPU fence.
    static auto* slots = new InitRetentionSlots;
    return *slots;
}
Dx11BridgeInitRetention* ReserveInitRetention()
{
    auto& pool = RetentionSlots();
    GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::ProviderPoolLock);
    std::lock_guard<std::mutex> lock(pool.mutex);r448Wait.Finish();
    GenshinHookCpuTiming::Scope r448Held(GenshinHookCpuTiming::Stage::ProviderPoolLocked);
    for (auto& slot : pool.slots)
        if (!slot.claimed) { slot.claimed = true; return &slot; }
    return nullptr;
}
void ReleaseInitReservation(Dx11BridgeInitRetention*& slot)
{
    if (!slot) return;
    auto& pool = RetentionSlots();
    GenshinHookCpuTiming::Scope r448Wait(GenshinHookCpuTiming::Stage::ProviderPoolLock);
    std::lock_guard<std::mutex> lock(pool.mutex);r448Wait.Finish();
    GenshinHookCpuTiming::Scope r448Held(GenshinHookCpuTiming::Stage::ProviderPoolLocked);
    slot->device.Reset(); slot->queue.Reset(); slot->workDomain={};
    slot->claimed = false;
    slot = nullptr;
}

// DRED retains only the last 64K operations; its counter is not ring-bounded.
// A late failure report must never index the history with that absolute counter.
int GenshinDredOperation(const D3D12_AUTO_BREADCRUMB_NODE1& node,UINT ordinal) noexcept
{
    constexpr UINT historyCapacity=65536;
    const auto retainedBegin=node.BreadcrumbCount>historyCapacity?node.BreadcrumbCount-historyCapacity:0;
    if(!node.pCommandHistory || ordinal<retainedBegin || ordinal>=node.BreadcrumbCount)return -1;
    return static_cast<int>(node.pCommandHistory[ordinal%historyCapacity]);
}
const char* GenshinDredOperationName(int op) noexcept
{
    switch(op) {
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:return "SetMarker";
    default:return op<0?"Unavailable":"Other";
    }
}
void LogGenshinDred(ID3D12Device* device,HRESULT reason)
{
    // An early ordinary refusal must not consume the later device-loss report.
    static std::atomic<unsigned> commandReports{0},deviceReports{0};
    auto& reports=device&&FAILED(device->GetDeviceRemovedReason())?deviceReports:commandReports;
    if(reports.fetch_add(1,std::memory_order_relaxed)>=4)return;
    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    const auto qi=device?device->QueryInterface(IID_PPV_ARGS(&dred)):E_POINTER;
    if(FAILED(qi) || !dred) {LOG_WARN("R24 DRED unavailable: reason={:X} qi={:X}",static_cast<UINT>(reason),static_cast<UINT>(qi));return;}
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    const auto bhr=dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    LOG_WARN("R24 DRED breadcrumbs: pid={} reason={:X} hr={:X}",GetCurrentProcessId(),static_cast<UINT>(reason),static_cast<UINT>(bhr));
    if(SUCCEEDED(bhr)) {
        unsigned index=0,logged=0,unfinished=0,detailNodes=0;
        auto* node=breadcrumbs.pHeadAutoBreadcrumbNode;
        // Preserve the original first 96 rows and inspect a bounded tail for
        // unfinished work. Linked-list order is explicitly not causal order.
        for(;node && index<512;node=node->pNext,++index) {
            const bool known=node->pLastBreadcrumbValue!=nullptr;
            const auto completed=known?*node->pLastBreadcrumbValue:0;
            const bool pending=known&&completed<node->BreadcrumbCount;
            if(pending)++unfinished;
            if(index>=96 && (!pending || logged>=128))continue;
            ++logged;
            const int nextOp=known?GenshinDredOperation(*node,completed):-1;
            const int priorOp=known&&completed?GenshinDredOperation(*node,completed-1):-1;
            const std::string listA=node->pCommandListDebugNameA?node->pCommandListDebugNameA:"";
            const std::string listW=node->pCommandListDebugNameW?wstring_to_string(node->pCommandListDebugNameW):"";
            const std::string queueA=node->pCommandQueueDebugNameA?node->pCommandQueueDebugNameA:"";
            const std::string queueW=node->pCommandQueueDebugNameW?wstring_to_string(node->pCommandQueueDebugNameW):"";
            LOG_WARN("R24 DRED node: index={} list={} queue={} count={} completed={} prior_op={} next_op={} listA={} listW={} "
                "completed_known={} prior_name={} next_name={} queueA={} queueW={} causal_order=false",
                index,reinterpret_cast<uint64_t>(node->pCommandList),reinterpret_cast<uint64_t>(node->pCommandQueue),
                node->BreadcrumbCount,completed,priorOp,nextOp,listA,listW,known,
                GenshinDredOperationName(priorOp),GenshinDredOperationName(nextOp),queueA,queueW);
            if(!pending || detailNodes++>=16)continue;
            const UINT begin=completed>4?completed-4:0;
            const UINT end=completed+std::min<UINT>(node->BreadcrumbCount-completed,5);
            for(UINT ordinal=begin;ordinal<end;++ordinal) {
                const auto op=GenshinDredOperation(*node,ordinal);
                LOG_WARN("R1068 DRED history: index={} list={} ordinal={} completed={} op={} name={} "
                    "retained={} failed_operation=UNKNOWN",index,reinterpret_cast<uint64_t>(node->pCommandList),
                    ordinal,completed,op,GenshinDredOperationName(op),op>=0);
            }
            // Runtime/vendor context strings may identify the recording phase;
            // never dereference removed command-list/queue pointers as live COM.
            if(node->pBreadcrumbContexts)for(UINT i=0;i<std::min<UINT>(node->BreadcrumbContextsCount,32);++i) {
                const auto& context=node->pBreadcrumbContexts[i];
                if(context.BreadcrumbIndex<begin || context.BreadcrumbIndex>=end)continue;
                LOG_WARN("R1068 DRED context: index={} ordinal={} text={}",index,context.BreadcrumbIndex,
                    context.pContextString?wstring_to_string(context.pContextString):"");
            }
        }
        LOG_WARN("R1068 DRED summary: scanned={} logged={} unfinished={} scan_clipped={} row_limit=128 "
            "detail_limit=16 causal_order=false exact_fault_operation=UNKNOWN",index,logged,unfinished,node!=nullptr);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    const auto fhr=dred->GetPageFaultAllocationOutput1(&fault);
    LOG_WARN("R24 DRED page fault: pid={} hr={:X} va={:X}",GetCurrentProcessId(),static_cast<UINT>(fhr),fault.PageFaultVA);
    if(SUCCEEDED(fhr))for(unsigned kind=0;kind<2;++kind) {
        unsigned index=0;auto* node=kind?fault.pHeadRecentFreedAllocationNode:fault.pHeadExistingAllocationNode;
        for(;node && index<96;node=node->pNext,++index) {
            const std::string nameA=node->ObjectNameA?node->ObjectNameA:"";
            const std::string nameW=node->ObjectNameW?wstring_to_string(node->ObjectNameW):"";
            LOG_WARN("R24 DRED allocation: freed={} index={} type={} object={} nameA={} nameW={}",kind,index,
                static_cast<unsigned>(node->AllocationType),reinterpret_cast<uint64_t>(node->pObject),nameA,nameW);
        }
    }
}
}
void IFeature_Dx11wDx12::LogGenshinFailure(const char* phase,HRESULT reason,UINT commandFrame,
                                        UINT64 waitTarget,bool waitTargetKnown) noexcept
{
    if(!genshinWorkDomain.Creator())return;
    const auto lastError=GetLastError();
    const auto deviceReason=BridgeWorkDevice()->GetDeviceRemovedReason();
    auto& reported=FAILED(deviceReason)?genshinDeviceFailureReported:genshinCommandFailureReported;
    if(reported){SetLastError(lastError);return;}
    reported=true; // One ordinary failure plus one actual device failure per feature.
    try {
        const auto* current=GenshinTransport::Scope::Current();
        const auto requested=current?current->owner:GenshinTransport::Owner{};
        const auto completed=Dx12Fence?Dx12Fence->GetCompletedValue():UINT64_MAX;
        LOG_WARN("R1068 SR first failure: pid={} handle={} phase={} status_hr={:X} requested_slot={} instance={} "
            "generation={} source_frame={} queue={} feature_fence={} feature_value={} completed={} "
            "last_submitted={} last_isolated={} path_failed={} unfenced={} provider={} "
            "device_hr={:X} hr_is_failure={} wait_target={} wait_target_known={} max_is_not_complete=true observation_only=true",
            GetCurrentProcessId(),Handle()?Handle()->Id:0,phase,static_cast<UINT>(reason),commandFrame,
            requested.instance,requested.generation,requested.sourceFrame,reinterpret_cast<uint64_t>(Dx12CommandQueue),
            reinterpret_cast<uint64_t>(Dx12Fence),Dx12FenceValue,completed,genshinLastSubmittedFence,
            genshinLastSubmissionIsolated,genshinCommandPathFailed,genshinSubmissionUnfenced,
            dx12Feature?static_cast<int>(dx12Feature->GetUpscalerType()):-1,static_cast<UINT>(deviceReason),
            FAILED(reason),waitTarget,waitTargetKnown);
        for(UINT i=0;i<DX11WDX12_COMMAND_BUFFER_COUNT;++i) {
            const auto& r=genshinRecordings[i];auto lease=r.lease.lock();
            LOG_WARN("R1068 SR recording: pid={} handle={} slot={} valid={} instance={} generation={} source_frame={} "
                "frame={} phase={} list={} allocator={} required_fence={} completed_fence={} "
                "reset_hr={:X} close_hr={:X} provider_returned={} provider_ok={} executed={} execute_returned={} "
                "transport_submitted={} submit_hr={:X} signal_hr={:X} copyback_ordered={} isolated={} lease_alive={} observation_only=true",
                GetCurrentProcessId(),Handle()?Handle()->Id:0,i,r.valid,r.owner.instance,r.owner.generation,r.owner.sourceFrame,
                r.frame,r.phase,reinterpret_cast<uint64_t>(Dx12CommandList[i]),reinterpret_cast<uint64_t>(Dx12CommandAllocator[i]),
                Dx12CommandAllocatorFenceValue[i],completed,static_cast<UINT>(r.resetHr),static_cast<UINT>(r.closeHr),
                r.providerReturned,r.providerOk,r.executed,r.executeReturned,r.submitted,static_cast<UINT>(r.submitHr),
                static_cast<UINT>(r.signalHr),r.copyback,r.isolated,bool(lease));
            if(lease)lease->LogFailureSnapshot(i);
        }
        LogGenshinDred(BridgeWorkDevice(),reason);
    }catch(...){} // Logging cannot change submission, copyback or quarantine.
    SetLastError(lastError);
}
void IFeature_Dx11wDx12::RetainFailedInitialization()
{
    if (!initRetention) return;
    initRetention->feature = std::move(dx12Feature);
    for (unsigned i = 0; i < DX11WDX12_COMMAND_BUFFER_COUNT; ++i)
    {
        initRetention->allocators[i].Attach(Dx12CommandAllocator[i]); Dx12CommandAllocator[i] = nullptr;
        initRetention->lists[i].Attach(Dx12CommandList[i]); Dx12CommandList[i] = nullptr;
        Dx12CommandAllocatorFenceValue[i] = 0;
    }
    initRetention->fence.Attach(Dx12Fence); Dx12Fence = nullptr;
    initRetention->event = Dx12FenceEvent; Dx12FenceEvent = nullptr;
    initRetention = nullptr; // pool keeps this claimed slot permanently
    SetInit(false);
    LOG_ERROR("DX11 bridge init failed after submission; provider and command resources retained. Restart required.");
}
void IFeature_Dx11wDx12::RetainGenshinRuntime() noexcept
{
    // No allocation after Execute or during destruction. Retain the provider,
    // upload/descriptors, device, queue, allocators, lists, fence and event together.
    auto* retained=genshinRuntimeRetention.release();
    retained->feature=std::move(dx12Feature);
    for(unsigned i=0;i<DX11WDX12_COMMAND_BUFFER_COUNT;++i){
        retained->allocators[i].Attach(Dx12CommandAllocator[i]);Dx12CommandAllocator[i]=nullptr;
        retained->lists[i].Attach(Dx12CommandList[i]);Dx12CommandList[i]=nullptr;
    }
    retained->fence.Attach(Dx12Fence);Dx12Fence=nullptr;
    retained->event=Dx12FenceEvent;Dx12FenceEvent=nullptr;
    // Like failed initialization, unproven submitted work has no exit destructor.
    static std::atomic<Dx11BridgeInitRetention*> head{nullptr};
    auto* previous=head.load(std::memory_order_relaxed);
    do {retained->next=previous;}
    while(!head.compare_exchange_weak(previous,retained,std::memory_order_release,std::memory_order_relaxed));
}
bool IFeature_Dx11wDx12::CreateD3D12Objects()
{
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::CreateObjects, this, Handle() ? Handle()->Id : 0, _frameCount);
    HRESULT result;

    for (size_t i = 0; i < DX11WDX12_COMMAND_BUFFER_COUNT; i++)
    {
        if(genshinWorkDomain.Creator()) {
            static_assert(DX11WDX12_COMMAND_BUFFER_COUNT==GenshinTransport::GpuWorkDomain::RecordingCount);
            if(!Dx12CommandAllocator[i] && !Dx12CommandList[i]) {
                GenshinTransport::GpuWitness witness;
                if(!genshinWorkDomain.CreateRecording(i,&Dx12CommandAllocator[i],&Dx12CommandList[i],witness)) {
                    LOG_ERROR("Genshin SR recording creation refused: slot={} reason={} hr={:X}",
                        i,witness.reason,static_cast<UINT>(witness.hr));
                    return false;
                }
                // Debug identity belongs to the real recording's creation,
                // not every dispatch/generation. Runtime owner/generation remain
                // in the existing submission/return logs.
                try {
                    const auto name=std::format(L"OptiGenshinSR_h{}_slot{}",Handle()?Handle()->Id:0,i);
                    Dx12CommandList[i]->SetName(name.c_str());
                }catch(...){} // A debug label cannot refuse a valid recording.
            }
            continue;
        }
        if (Dx12CommandAllocator[i] == nullptr)
        {
            result =
                BridgeWorkDevice()->CreateCommandAllocator(Dx12CommandListType, IID_PPV_ARGS(&Dx12CommandAllocator[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator error: {:X}", result);
                return false;
            }
        }

        if (Dx12CommandList[i] == nullptr && Dx12CommandAllocator[i] != nullptr)
        {
            // CreateCommandList
            result = BridgeWorkDevice()->CreateCommandList(0, Dx12CommandListType, Dx12CommandAllocator[i], nullptr,
                                                        IID_PPV_ARGS(&Dx12CommandList[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList error: {:X}", result);
                return false;
            }

            Dx12CommandList[i]->Close();
        }
    }

    if (Dx12Fence == nullptr)
    {
        result = BridgeWorkDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Dx12Fence));

        if (result != S_OK)
        {
            LOG_ERROR("CreateFence error: {0:X}", result);
            return false;
        }

        Dx12FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        if (Dx12FenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent error!");
            return false;
        }
    }

    if(genshinWorkDomain.Creator())for(size_t i=0;i<DX11WDX12_COMMAND_BUFFER_COUNT;++i){
        GenshinTransport::GpuWitness witness;
        if(!genshinWorkDomain.Commands(Dx12CommandQueue,Dx12CommandList[i],Dx12CommandAllocator[i],Dx12Fence,witness)){
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<6)
                LOG_ERROR("Genshin SR created object refused: slot={} reason={} hr={:X} expected_device={} observed_device={}",
                    i,witness.reason,static_cast<UINT>(witness.hr),reinterpret_cast<uint64_t>(BridgeWorkDevice()),
                    reinterpret_cast<uint64_t>(witness.actual.Get()));
            return false;
        }
    }
    return true;
}

bool IFeature_Dx11wDx12::ReleaseSharedResources(bool releaseProvider)
{
    // An exception after Execute but before its successful Signal has no safe
    // fence target. Do not manufacture a future target or release those owners.
    if(genshinSubmissionUnfenced)return false;
    UINT64 lastFence = Dx12FenceValue;
    for (const auto value : Dx12CommandAllocatorFenceValue)
        lastFence = std::max(lastFence, value);

    if (lastFence != 0 && Dx12Fence != nullptr)
    {
        const auto completedBefore = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
        if (completedBefore == UINT64_MAX)
        {
            LOG_ERROR("Dx11wDx12 release fence returned UINT64_MAX; retaining bridge resources, fence {}", lastFence);
            return false;
        }

        if (completedBefore < lastFence)
        {
            if (Dx12FenceEvent == nullptr)
            {
                LOG_ERROR("Dx11wDx12 release cannot wait for fence {}; event is unavailable; retaining bridge resources",
                          lastFence);
                return false;
            }

            const auto setEventResult = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceArmApi,[&]{return Dx12Fence->SetEventOnCompletion(lastFence, Dx12FenceEvent);},GenshinHookCpuTiming::ResultKind::Hresult);
            if (setEventResult != S_OK)
            {
                LOG_ERROR("Dx11wDx12 release SetEventOnCompletion failed for fence {}: {:X}; retaining bridge resources",
                          lastFence, (UINT) setEventResult);
                return false;
            }

            const auto waitResult = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::CpuEventWait,[&]{return WaitForSingleObject(Dx12FenceEvent, 5000);},GenshinHookCpuTiming::ResultKind::Event);
            const auto completedAfter = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
            if (waitResult != WAIT_OBJECT_0 || completedAfter < lastFence || completedAfter == UINT64_MAX)
            {
                LOG_ERROR("Dx11wDx12 release fence drain failed: fence {}, completed before {}, after {}, wait {:X}; "
                          "retaining bridge resources",
                          lastFence, completedBefore, completedAfter, (UINT) waitResult);
                return false;
            }

            LOG_INFO("Dx11wDx12 release fence drain completed: fence {}, completed before {}, after {}",
                     lastFence, completedBefore, completedAfter);
        }
    }

    // Final Genshin teardown must release the provider while its borrowed
    // device/queue and creation recordings are still owned by this wrapper.
    // Context-only recreation keeps the provider and uses the default false.
    // All unproven-submission/fence exits above leave it available for retention.
    if (releaseProvider)
        dx12Feature.reset();

    for (size_t i = 0; i < DX11WDX12_COMMAND_BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(Dx12CommandList[i]);
        SAFE_RELEASE(Dx12CommandAllocator[i]);
        Dx12CommandAllocatorFenceValue[i] = 0;
    }

    SAFE_RELEASE(Dx12Fence);
    Dx12FenceValue = 0;
    genshinLastSubmittedFence = 0;
    genshinLastSubmissionIsolated = false;
    genshinRecordings = {}; // Only after the existing physical drain above.

    if (Dx12FenceEvent)
    {
        CloseHandle(Dx12FenceEvent);
        Dx12FenceEvent = nullptr;
    }

    genshinWorkDomain.RetireRecordings(); // Reached only after the existing fence drain.
    // SAFE_RELEASE(Dx12Device);
    return true;
}

bool IFeature_Dx11wDx12::ProcessDx11Textures(const NVSDK_NGX_Parameter* InParameters)
{
    HRESULT result;

    const auto commandFrame = (UINT) (_frameCount % DX11WDX12_COMMAND_BUFFER_COUNT);
    const auto resourceFrame = (UINT) (_frameCount % DX11_WITH_DX12_CACHED_FRAMES);
    const auto cacheFrameKey = Dx11WithDx12::NextUpscalerFrameId();
    Dx11WithDx12::SetUpscalerFrameIndex(resourceFrame);

    auto mask = Dx11WithDx12::ResourceMask::Color | Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth |
                Dx11WithDx12::ResourceMask::Output;

    if (!AutoExposure())
        mask |= Dx11WithDx12::ResourceMask::Exposure;
    else
        LOG_DEBUG("AutoExposure enabled!");

    const bool reactiveDisabled = Config::Instance()->DisableReactiveMask.value_or(false);
    const bool reactiveRequired = Config::Instance()->Dx11Upscaler.value_or_default() == Upscaler::XeSS ||
                                  Config::Instance()->Dx11Upscaler.value_or_default() == Upscaler::XeSS_on12;

    if (!reactiveDisabled)
        mask |= Dx11WithDx12::ResourceMask::Reactive;
    else
        LOG_DEBUG("ReactiveMask disabled!");

    CallerCpuTrace::Scope prepareCpu(CallerCpuTrace::Op::PrepareResources, this, Handle() ? Handle()->Id : 0, _frameCount, resourceFrame);
    const auto prepareResult = Dx11WithDx12::PrepareUpscalerResources(
        InParameters, mask, resourceFrame, cacheFrameKey, Config::Instance()->DontUseNTShared.value_or_default(),
        reactiveRequired, true);
    prepareCpu.Finish();

    if (!prepareResult.Success)
    {
        if (prepareResult.MissingExposure)
        {
            LOG_WARN("AutoExposure disabled but ExposureTexture does not exist, enabling auto exposure and changing "
                     "backend");
            State::Instance().autoExposure = true;
            State::Instance().changeBackend[Handle()->Id] = true;
            return true;
        }

        if (prepareResult.MissingReactive && reactiveRequired)
        {
            LOG_WARN("Bias mask does not exist and is required by the current DX11 upscaler, disabling reactive mask");
            Config::Instance()->DisableReactiveMask.set_volatile_value(true);
            State::Instance().changeBackend[Handle()->Id] = true;
            return true;
        }

        LOG_ERROR("Dx11wDx12 resource cache preparation failed");
        return false;
    }

    const auto allocatorFenceValue = Dx12CommandAllocatorFenceValue[commandFrame];
    const auto completedBefore = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
    if (allocatorFenceValue != 0 && completedBefore < allocatorFenceValue)
    {
        result = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceArmApi,[&]{return Dx12Fence->SetEventOnCompletion(allocatorFenceValue, Dx12FenceEvent);},GenshinHookCpuTiming::ResultKind::Hresult);
        if (result != S_OK)
        {
            LOG_ERROR("SetEventOnCompletion error for allocator {} fence {}: {:X}", commandFrame, allocatorFenceValue,
                      (UINT) result);
            return false;
        }

        const auto waitStart = Util::MillisecondsNow();
        CallerCpuTrace::Scope waitCpu(CallerCpuTrace::Op::AllocatorWait, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame, allocatorFenceValue, completedBefore, false, true);
        const auto waitResult = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::AllocatorCpuEventWait,[&]{return WaitForSingleObject(Dx12FenceEvent, 5000);},GenshinHookCpuTiming::ResultKind::Event);
        waitCpu.Stop();
        const auto waitMs = Util::MillisecondsNow() - waitStart;
        const auto completedAfter = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
        waitCpu.Finish(waitResult, true);

        if (waitMs > 0.25)
        {
            LOG_WARN("Dx11wDx12 allocator wait: {:.3f} ms, slot: {}, required fence: {}, completed before: {}, "
                     "completed after: {}",
                     waitMs, commandFrame, allocatorFenceValue, completedBefore, completedAfter);
        }

        if (waitResult != WAIT_OBJECT_0)
        {
            if (waitResult == WAIT_TIMEOUT)
            {
                LOG_ERROR("Dx11wDx12 allocator wait timed out after {:.3f} ms, slot: {}, required fence: {}, "
                          "completed: {}",
                          waitMs, commandFrame, allocatorFenceValue, completedAfter);
            }
            else
            {
                LOG_ERROR("WaitForSingleObject failed for allocator {} fence {}: {:X}", commandFrame,
                          allocatorFenceValue, (UINT) waitResult);
            }
            return false;
        }
    }

    CallerCpuTrace::Scope resetCpu(CallerCpuTrace::Op::AllocatorReset, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame, allocatorFenceValue, completedBefore, false, true);
    result = Dx12CommandAllocator[commandFrame]->Reset();
    resetCpu.Finish();
    if (result != S_OK)
    {
        LOG_ERROR("CommandAllocator Reset error for frame {}, allocator fence {}, completed {}: {:X}", commandFrame,
                  allocatorFenceValue, GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence), (UINT) result);
        return false;
    }

    CallerCpuTrace::Scope listResetCpu(CallerCpuTrace::Op::ListReset, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
    result = Dx12CommandList[commandFrame]->Reset(Dx12CommandAllocator[commandFrame], nullptr);
    listResetCpu.Finish();
    if (result != S_OK)
    {
        LOG_ERROR("CommandList Reset error: {:X}", (UINT) result);
        return false;
    }

    LOG_DEBUG("Shared handles prepared and synchronized by Dx11WithDx12 cache, frameKey: {}", cacheFrameKey);
    return true;
}

bool IFeature_Dx11wDx12::CopyBackOutput()
{
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::CopyBack, this, Handle() ? Handle()->Id : 0, _frameCount);
    const auto resourceFrame = (UINT) (_frameCount % DX11_WITH_DX12_CACHED_FRAMES);
    return Dx11WithDx12::CopyUpscalerOutputToDx11(resourceFrame);
}

bool IFeature_Dx11wDx12::Init(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Init, this, Handle() ? Handle()->Id : 0, _frameCount);
    LOG_FUNC();

    if (IsInited())
        return true;

    if (State::Instance().NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
        State::Instance().gameEngine == GameEngineType::Unreal ||
        State::Instance().gameQuirks & GameQuirk::ForceUnrealEngine)
    {
        LOG_INFO("Dx11 detected, disabling UE resource barrier overrides");
        Config::Instance()->ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Config::Instance()->MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    Device = InDevice;
    DeviceContext = InContext;

    if (!bridgeInitState.Begin())
    {
        LOG_ERROR("DX11 bridge initialization cannot retry on the same failed/submitted generation");
        return false;
    }
    initRetention = ReserveInitRetention();
    if (!initRetention)
    {
        bridgeInitState.UnsubmittedFailure();
        LOG_ERROR("DX11 bridge init failure retention limit reached; restart required");
        return false;
    }
    if (!BaseInit(Device, InContext, InParameters))
    {
        bridgeInitState.UnsubmittedFailure();
        ReleaseInitReservation(initRetention);
        return false;
    }
    initRetention->device = BridgeWorkDevice();
    initRetention->workDomain = genshinWorkDomain;
    initRetention->queue = Dx12CommandQueue;
    SetInitParameters(InParameters);

    // The list has to be recording, and what is recorded on it has to run.
    //
    // Every command list here is closed the moment it is created, and the note this replaces said
    // non-DLSS upscalers do not use the list during Init -- which was true, and stopped being true the
    // day DLSS got a bridge variant. DLSS and Ray Reconstruction are the only features whose
    // InitInternal touches the argument at all: NVSDK_NGX_D3D12_CreateFeature records the model's
    // weight upload and history initialisation onto it, and documents that the caller must submit it
    // afterwards.
    //
    // Recorded onto a closed list, all of that is discarded. ID3D12GraphicsCommandList methods return
    // void, so nothing fails, nothing logs, and CreateFeature still answers Success -- the model then
    // runs against state that was never uploaded. That is the posterised, flat-blocked picture, and it
    // gets worse with model size: the old CNN degraded to soft, the transformer collapses to blocks.
    //
    // So: open the list, let Init record into it, submit it, and wait. The wait is not optional --
    // the first Evaluate resets this same allocator, and doing that under work still in flight is a
    // device removal rather than a bad picture.
    auto failUnsubmitted = [&]() {
        bridgeInitState.UnsubmittedFailure();
        ReleaseInitReservation(initRetention);
        SetInit(false);
        return false;
    };
    if (!Dx12CommandQueue || !Dx12Fence || !Dx12FenceEvent || !dx12Feature)
        return failUnsubmitted();
    CallerCpuTrace::Scope initResetCpu(CallerCpuTrace::Op::AllocatorReset, this, Handle() ? Handle()->Id : 0, _frameCount, 0);
    HRESULT prep = Dx12CommandAllocator[0]->Reset();
    initResetCpu.Finish();
    if (FAILED(prep))
    {
        LOG_ERROR("Init allocator Reset failed: {:X}", (UINT)prep);
        return failUnsubmitted();
    }
    CallerCpuTrace::Scope initListResetCpu(CallerCpuTrace::Op::ListReset, this, Handle() ? Handle()->Id : 0, _frameCount, 0);
    prep = Dx12CommandList[0]->Reset(Dx12CommandAllocator[0], nullptr);
    initListResetCpu.Finish();
    if (FAILED(prep))
    {
        LOG_ERROR("Init command-list Reset failed: {:X}", (UINT)prep);
        return failUnsubmitted();
    }
    if(genshinWorkDomain.Creator()) {
        GenshinTransport::GpuWitness witness;
        if(!genshinWorkDomain.Commands(Dx12CommandQueue,Dx12CommandList[0],Dx12CommandAllocator[0],Dx12Fence,witness)) {
            LOG_ERROR("Genshin SR provider init refused: reason={} hr={:X}",witness.reason,static_cast<UINT>(witness.hr));
            return failUnsubmitted();
        }
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)<4)
            LOG_INFO("Genshin SR provider creation domain: pid={} creator={} public_owner={} queue={} list={} allocator={} fence={} heap_probe={} list_owner=creator heap_owner=creator allocator_owner=public_owner fence_owner=public_owner exact_recording_receipt=true",
                GetCurrentProcessId(),reinterpret_cast<uint64_t>(genshinWorkDomain.Creator()),
                reinterpret_cast<uint64_t>(genshinWorkDomain.PublicOwner()),reinterpret_cast<uint64_t>(Dx12CommandQueue),
                reinterpret_cast<uint64_t>(Dx12CommandList[0]),reinterpret_cast<uint64_t>(Dx12CommandAllocator[0]),
                reinterpret_cast<uint64_t>(Dx12Fence),reinterpret_cast<uint64_t>(genshinWorkDomain.HeapProbe()));
    }
    CallerCpuTrace::Scope providerInitCpu(CallerCpuTrace::Op::ProviderInit, this, Handle() ? Handle()->Id : 0, _frameCount);
    const bool initialised = dx12Feature->Init(BridgeWorkDevice(), Dx12CommandList[0], InParameters);
    providerInitCpu.Finish();
    const HRESULT closed = Dx12CommandList[0]->Close();
    if (FAILED(closed))
    {
        LOG_ERROR("Init command-list Close failed, creation not submitted: {:X}", (UINT)closed);
        return failUnsubmitted();
    }
    if(genshinWorkDomain.Creator()) {
        GenshinTransport::GpuWitness witness;
        if(!genshinWorkDomain.Commands(Dx12CommandQueue,Dx12CommandList[0],Dx12CommandAllocator[0],Dx12Fence,witness)) {
            LOG_ERROR("Genshin SR init submit refused: reason={} hr={:X}",witness.reason,static_cast<UINT>(witness.hr));
            return failUnsubmitted();
        }
    }
    ID3D12CommandList* lists[] = { Dx12CommandList[0] };
    bridgeInitState.Submitted();
    CallerCpuTrace::Scope initExecuteCpu(CallerCpuTrace::Op::Execute, this, Handle() ? Handle()->Id : 0, _frameCount, 0);
    GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12Submit,[&]{return Dx12CommandQueue->ExecuteCommandLists(1, lists);},GenshinHookCpuTiming::ResultKind::None);
    initExecuteCpu.Finish();
    const UINT64 signalled = ++Dx12FenceValue;
    CallerCpuTrace::Scope initSignalCpu(CallerCpuTrace::Op::Signal, this, Handle() ? Handle()->Id : 0, _frameCount, 0, signalled);
    const HRESULT signalResult = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12SignalApi,[&]{return Dx12CommandQueue->Signal(Dx12Fence, signalled);},GenshinHookCpuTiming::ResultKind::Hresult);
    initSignalCpu.Finish();
    bridgeInitState.Signalled(signalled, SUCCEEDED(signalResult));
    if (FAILED(signalResult))
    {
        bridgeInitState.Observe(GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence), FAILED(initRetention->device->GetDeviceRemovedReason()));
        LOG_ERROR("Init queue Signal failed after Execute: {:X}; phase {}", (UINT)signalResult, (unsigned)bridgeInitState.Get());
        RetainFailedInitialization();
        return false;
    }
    Dx12CommandAllocatorFenceValue[0] = signalled;
    UINT64 completed = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
    bool ready = bridgeInitState.Observe(completed, FAILED(initRetention->device->GetDeviceRemovedReason()));
    if (!ready && bridgeInitState.Get() == Dx11BridgeInit::Phase::SubmittedFenced)
    {
        const HRESULT armed = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceArmApi,[&]{return Dx12Fence->SetEventOnCompletion(signalled, Dx12FenceEvent);},GenshinHookCpuTiming::ResultKind::Hresult);
        DWORD waited = WAIT_FAILED;
        if (SUCCEEDED(armed))
        {
            CallerCpuTrace::Scope initWaitCpu(CallerCpuTrace::Op::InitWait, this, Handle() ? Handle()->Id : 0, _frameCount, 0, signalled, completed, false, true);
            waited = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::InitCpuEventWait,[&]{return WaitForSingleObject(Dx12FenceEvent, 5000);},GenshinHookCpuTiming::ResultKind::Event);
            initWaitCpu.Finish(waited, true);
        }
        completed = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
        ready = bridgeInitState.Observe(completed, FAILED(initRetention->device->GetDeviceRemovedReason()));
        if (!ready)
            LOG_ERROR("Init completion not proven: SetEvent {:X}, wait {}, required {}, completed {}, phase {}",
                      (UINT)armed, waited, signalled, completed, (unsigned)bridgeInitState.Get());
    }
    if (!ready || !initialised)
    {
        // Even a completed failed provider stays pinned: async provider teardown is not established here.
        RetainFailedInitialization();
        return false;
    }
    ReleaseInitReservation(initRetention);
    SetInit(true);
    LOG_INFO("Init: creation fence {} completed; bridge initialized", signalled);
    return IsInited();
}

// Called only after authenticating the Genshin producer. No new input handoff
// has been queued yet. The previous invocation already flushed its input signal;
// its feature fence precedes D3D11 output retirement and depends on no new frame.
HRESULT IFeature_Dx11wDx12::WaitForGenshinRecording(UINT commandFrame, bool isolatedPassResources)
{
    GenshinCpuStageTiming::Scope cpu(GenshinCpuStageTiming::Stage::OptRecordingWait);
    GenshinHookCpuTiming::Scope r448Recording(GenshinHookCpuTiming::Stage::RecordingWait);
    std::optional<GenshinHookCpuTiming::Scope> r448BlockedRecording;
    UINT64 waitTarget=0;bool waitTargetKnown=false;
    const auto finish=[&](HRESULT hr) {
        if(r448BlockedRecording)r448BlockedRecording->Returned(static_cast<uint64_t>(hr),GenshinHookCpuTiming::ResultKind::Hresult);
        if(FAILED(hr))LogGenshinFailure("recording-wait",hr,commandFrame,waitTarget,waitTargetKnown);
        return r448Recording.Return(hr);
    };
    GenshinTransport::ClearRecordingWaitTimeout();
    if(commandFrame >= DX11WDX12_COMMAND_BUFFER_COUNT || genshinCommandPathFailed || genshinSubmissionUnfenced || !Dx12Fence || !Dx12FenceEvent || !BridgeWorkDevice())
        return finish(E_FAIL);
    // A completed value is meaningful only for this feature's actual work domain.
    GenshinTransport::GpuWitness witness;
    if(!genshinWorkDomain.Commands(Dx12CommandQueue,Dx12CommandList[commandFrame],
        Dx12CommandAllocator[commandFrame],Dx12Fence,witness)){
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)<6)
            LOG_WARN("Genshin SR recording witness refused: reason={} hr={:X} expected_device={} observed_device={}",
                witness.reason,static_cast<UINT>(witness.hr),reinterpret_cast<uint64_t>(BridgeWorkDevice()),
                reinterpret_cast<uint64_t>(witness.actual.Get()));
        return finish(witness.hr);
    }
    // Isolated passes cannot refer to another slot's CPU-mutable resources.
    // Their final dispatch/queries are on this exact list, before its Signal.
    // Unsupported routes still drain the latest submission as before.
    // Legacy OS/Magnifier timing queries use their own dispatch counters.
    // Drain once on entry/re-entry to isolated mode before binding every query
    // to commandFrame. No legacy query range can still alias the new slot.
    const bool isolatedReuse = isolatedPassResources && genshinLastSubmissionIsolated;
    const UINT64 target = isolatedReuse ? Dx12CommandAllocatorFenceValue[commandFrame]
        : std::max(genshinLastSubmittedFence,Dx12CommandAllocatorFenceValue[commandFrame]);
    waitTarget=target;waitTargetKnown=true;
    // These values are published only by successful Signals, never reservations.
    const auto start=GetTickCount64();
    bool armed=false;
    for(;;) {
        const auto removed=BridgeWorkDevice()->GetDeviceRemovedReason();
        if(FAILED(removed))return finish(removed);
        const auto completed=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
        if(completed==UINT64_MAX)return finish(DXGI_ERROR_DEVICE_REMOVED);
        if(completed>=target)return finish(S_OK);
        if(!armed){r448BlockedRecording.emplace(GenshinHookCpuTiming::Stage::RecordingBlocked);
            const auto hr=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceArmApi,[&]{return Dx12Fence->SetEventOnCompletion(target,Dx12FenceEvent);},GenshinHookCpuTiming::ResultKind::Hresult);
            if(FAILED(hr))return finish(hr);armed=true;}
        // Bound backpressure. Timeout never authorizes allocator/Map/descriptor reuse.
        if(GetTickCount64()-start>=1000) {
            LOG_WARN("R21 SR recording wait timeout: target={} completed={} last_submitted={} allocator_fence={} slot={}",
                target,completed,genshinLastSubmittedFence,Dx12CommandAllocatorFenceValue[commandFrame],commandFrame);
            GenshinTransport::NoteRecordingWaitTimeout();
            return finish(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        }
        const auto waited=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::RecordingCpuEventWait,[&]{return WaitForSingleObject(Dx12FenceEvent,10);},GenshinHookCpuTiming::ResultKind::Event);
        if(waited!=WAIT_OBJECT_0 && waited!=WAIT_TIMEOUT){
            const auto error=GetLastError();return finish(error?HRESULT_FROM_WIN32(error):E_FAIL);
        }
        // Always verify the fence: the persistent event may contain an older wake.
    }
}

bool IFeature_Dx11wDx12::Evaluate(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Evaluate, this, Handle() ? Handle()->Id : 0, _frameCount, UINT64_MAX, 0, 0, true);
    LOG_FUNC();

    if (!IsInited() || genshinSubmissionUnfenced)
        return false;

    // Real bridge owner, before BeforeSR admission and any SDK work.
    HoYoShade::RenderOwnerScope hoYoSettingsOwner;
    static const auto gameIdentity = [] {
        const auto executableName = Util::ExePath().filename().wstring();
        return GameRouting::IdentifyExecutable(executableName.c_str());
    }();
    auto* const producerScope = GenshinTransport::Scope::Current();
    auto* const genshinScope = GameRouting::UseGenshinBridgeTransport(gameIdentity, producerScope != nullptr)
                                   ? producerScope
                                   : nullptr;
    // The prior feature fence was signalled by its DX12 queue. Waiting for
    // recording reuse does not require a second D3D11 output Flush: the next
    // Lease input submit carries prior Wait/copy/retire commands as well.

    ID3D11DeviceContext4* dc;
    auto result = InDeviceContext->QueryInterface(IID_PPV_ARGS(&dc));

    if (result != S_OK)
    {
        LOG_ERROR("QueryInterface error: {0:x}", result);
        return false;
    }

    // Preserve the actual QI. The existing Context4 member is a borrowed
    // pointer, so it must not become a persistent pointer-only QI cache.
    const bool contextUnchanged = dc == Dx11DeviceContext;
    if (dc != Dx11DeviceContext)
    {
        LOG_WARN("Dx11DeviceContext changed!");
        if (!ReleaseSharedResources())
        {
            LOG_ERROR("Refusing Dx11 context switch while bridge GPU work is still unproven complete");
            dc->Release();
            return false;
        }
        Dx11DeviceContext = dc;

        if (!CreateD3D12Objects())
        {
            LOG_ERROR("Failed to recreate Dx11wDx12 D3D12 objects after context change");
            if (dc != nullptr)
                dc->Release();
            return false;
        }
    }

    if (dc != nullptr)
        dc->Release();

    // A TLS scope alone is not state-isolation proof. Only the exact internal
    // capture invocation can lend a single-use witness. Unknown backends and
    // changed contexts keep the full state guard.
    const bool isolatedSrState = genshinScope && dx12Feature && contextUnchanged &&
        dx12Feature->GetUpscalerType()==Upscaler::DLSS &&
        GenshinLegacy::InternalSrStateScope::Consume(InDeviceContext,*genshinScope,InParameters);

    const auto commandFrame = (UINT) (_frameCount % DX11WDX12_COMMAND_BUFFER_COUNT);
    const auto resourceFrame = (UINT) (_frameCount % DX11_WITH_DX12_CACHED_FRAMES);
    auto cmdList = Dx12CommandList[commandFrame];

    auto& cache = Dx11WithDx12::GetUpscalerResourceCache();
    auto& dx11Color = cache.Color;
    auto& dx11Mv = cache.Mv;
    auto& dx11Depth = cache.Depth;
    auto& dx11Reactive = cache.Reactive;
    auto& dx11Exp = cache.Exposure;
    auto& dx11Out = cache.Output[resourceFrame];

    auto getOriginalNgxResource = [](NVSDK_NGX_Parameter* parameters, const char* name, ID3D11Resource** outResource)
    {
        if (parameters == nullptr || name == nullptr || outResource == nullptr)
            return false;

        *outResource = nullptr;

        if (parameters->Get(name, outResource) != NVSDK_NGX_Result_Success)
            parameters->Get(name, (void**) outResource);

        return *outResource != nullptr;
    };

    ID3D11Resource* restoreParamColor = nullptr;
    ID3D11Resource* restoreParamMv = nullptr;
    ID3D11Resource* restoreParamOutput = nullptr;
    ID3D11Resource* restoreParamDepth = nullptr;
    ID3D11Resource* restoreParamExposure = nullptr;
    ID3D11Resource* restoreParamReactive = nullptr;

    const bool hasRestoreParamColor =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Color, &restoreParamColor);
    const bool hasRestoreParamMv =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_MotionVectors, &restoreParamMv);
    const bool hasRestoreParamOutput =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Output, &restoreParamOutput);
    const bool hasRestoreParamDepth =
        getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_Depth, &restoreParamDepth);
    getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_ExposureTexture, &restoreParamExposure);
    getOriginalNgxResource(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, &restoreParamReactive);

    // Pure internal SR hands only a D3D12 command list to the provider. Its
    // D3D11 operations here/inside Lease are copy, query and fence operations,
    // with no CS/OM pipeline setters; both private context-state swaps remain
    // owned by the synchronous producer. The game state is held outside them.
    // Do not unbind/rebind or snapshot all CS/OM slots a third time. Optional
    // consumers, unknown witnesses and external calls retain the complete guard.
    std::unique_ptr<Dx11BridgeStateGuard> savedState;
    if(!isolatedSrState)savedState=std::make_unique<Dx11BridgeStateGuard>(InDeviceContext);
    OptiBridge::ResourceParameterGuard<NVSDK_NGX_Parameter,ID3D11Resource,NVSDK_NGX_Result,6> restoreParameters(
        InParameters,
        {NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_MotionVectors,
         NVSDK_NGX_Parameter_Output,NVSDK_NGX_Parameter_ExposureTexture,NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask},
        {restoreParamColor,restoreParamDepth,restoreParamMv,restoreParamOutput,restoreParamExposure,restoreParamReactive},
        {hasRestoreParamColor||bool(genshinScope),hasRestoreParamDepth||bool(genshinScope),
         hasRestoreParamMv||bool(genshinScope),hasRestoreParamOutput||bool(genshinScope),true,true},
        NVSDK_NGX_Result_Success);

    bool dx12EvalResult = false;
    bool commandListRecording = false;
    bool commandListExecuted = false;
    bool commandListExecuteReturned = false;
    // No assumption that FG/Present NR/filter are absent: they consume leased
    // inputs/final output, not these private Opt pass intermediates. Inline NR,
    // optical NR, unknown providers and inline DX12 menu retain global wait.
    const auto canIsolatePassResources = [&] {
        const auto& cfg = *Config::Instance();
        return dx12Feature && dx12Feature->CanIsolateRecordingResources() && !native::OpticalFgNrEnabled() &&
            (!cfg.DlssNrEnabled.value_or_default() ||
             (cfg.DlssNrFinishedPicture.value_or_default() && !cfg.DlssNrRunBeforeSr.value_or_default() &&
              !cfg.DlssNrDeferredDlss.value_or_default()));
    };
    bool isolatedPassResources = false;
    GenshinSrRecordingCpu::ReportOnExit recordingCpuReport;
    GenshinCpuStageTiming::Scope genshinCpu(genshinScope?GenshinCpuStageTiming::Stage::OptSrEvaluate:GenshinCpuStageTiming::Stage::Count);
    std::shared_ptr<GenshinTransport::Lease> genshinTransport;
    if (auto* scope = genshinScope; scope != nullptr)
    {
        const std::array<ID3D11Resource*, 4> actual = {
            restoreParamColor, restoreParamDepth, restoreParamMv, restoreParamOutput };
        // No inferred owner, reused scope, or optional unleased guide may enter this seam.
        if (scope->parameters != InParameters || scope->resources != actual || scope->lease ||
            restoreParamExposure || restoreParamReactive || !AutoExposure())
            return false;
        if(InDeviceContext->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
           scope->owner.context!=reinterpret_cast<uint64_t>(InDeviceContext) ||
           scope->owner.thread!=GetCurrentThreadId())return false;
        try {
            if(!genshinRuntimeRetention)genshinRuntimeRetention=std::make_unique<Dx11BridgeInitRetention>();
            genshinRuntimeRetention->device=BridgeWorkDevice();
            genshinRuntimeRetention->workDomain=genshinWorkDomain;
            genshinRuntimeRetention->queue=Dx12CommandQueue;
        }catch(...){return false;}
        // Slot isolation is an actual resource choice, not a claim based on
        // inactive consumers. Unknown/inline paths retain the old global wait.
        isolatedPassResources = canIsolatePassResources();
        recordingCpuReport.feature = dx12Feature.get();
        GenshinSrRecordingCpu::Scope recordingWaitCpu(dx12Feature.get(), GenshinSrRecordingCpu::Stage::RecordingWait, true);
        const auto recordingReady=WaitForGenshinRecording(commandFrame, isolatedPassResources);
        recordingWaitCpu.Finish(recordingReady);
        if(FAILED(recordingReady)){
            LOG_WARN("Genshin recording resources not available: {:X}",static_cast<UINT>(recordingReady));
            return false;
        }
        GenshinSubstage::Event(GenshinSubstage::Stage::ResourceMapping,GenshinSubstage::State::Running);
        auto preparedInputs=scope->preparedInputs;
        // The known full stack uses the same prepared pair, pinned by every
        // exported packet until its existing reader retirement. Re-read actual
        // route selection here; changed/unknown consumers retain private copies.
        if(!GenshinTransport::PreparedSrOutput::PureSr() &&
           !GenshinTransport::CurrentOutputRoute().batchAtNativeAcquire)
            preparedInputs.reset();
        if (!scope->lease)
            scope->lease = GenshinTransport::Lease::Begin(scope->owner, InDeviceContext, genshinWorkDomain, Dx12CommandQueue,
                                                           scope->resources, InParameters, std::move(preparedInputs));
        genshinTransport = scope->lease;
        if (!genshinTransport)
        {
            GenshinSubstage::Event(GenshinSubstage::Stage::ResourceMapping,GenshinSubstage::State::Failed,
                GenshinSubstage::Resource::Inputs,GenshinSubstage::Domain::Mapping,0,GenshinSubstage::Issue::Rejected);
            LOG_WARN("Genshin observation transport refused this owner/resource set");
            return false;
        }
    }
    if(genshinTransport)GenshinSubstage::Event(GenshinSubstage::Stage::ResourceMapping,GenshinSubstage::State::Succeeded);
    std::unique_ptr<GenshinCaller::BridgeAfterUpscale> genshinNrCaller;
    bool genshinSubmitted=false;
    const char* genshinStop="provider-not-reached";
    HRESULT genshinStatus=S_OK;
    try
    {
    do
    {
        if (genshinTransport)
        {
            // This exact allocator slot was completed before Begin; other isolated slots may still run.
            const auto completed=GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence);
            if(genshinCommandPathFailed || completed==UINT64_MAX || completed<Dx12CommandAllocatorFenceValue[commandFrame]){genshinStop="allocator-fence-not-ready";break;}
            auto& recording=genshinRecordings[commandFrame];recording={};
            recording.valid=true;recording.owner=genshinScope->owner;recording.frame=_frameCount;
            recording.lease=genshinTransport;recording.phase="reset";
            auto reset=Dx12CommandAllocator[commandFrame]->Reset();
            if(SUCCEEDED(reset))reset=cmdList->Reset(Dx12CommandAllocator[commandFrame],nullptr);
            recording.resetHr=reset;
            if(FAILED(reset)){genshinStop="allocator-or-list-reset";genshinStatus=reset;genshinCommandPathFailed=true;break;}
            commandListRecording=true;
            genshinTransport->BeginGpuTimeline(genshinScope->owner,cmdList,Dx12CommandAllocator[commandFrame]);
        }
        else if (!ProcessDx11Textures(InParameters))
        {
            LOG_ERROR("Can't process Dx11 textures!");
            break;
        }

        if (State::Instance().changeBackend[Handle()->Id])
        {
            break;
        }

        commandListRecording = true;

        if (genshinTransport)
        {
            auto* scope = genshinScope;
            std::array<ID3D12Resource*, 4> imported{};
            for (size_t i = 0; i < imported.size(); ++i)
                imported[i] = genshinTransport->Resource(scope->owner, i);
            if (!imported[0] || !imported[1] || !imported[2] || !imported[3])
                break;
            // The private owner supplies a neutral table: native D3D11 tables
            // silently ignore this typed D3D12 setter. Prove the provider-facing
            // getter returns each exact imported resource before invoking it.
            InParameters->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D12Resource*>(imported[0]));
            InParameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D12Resource*>(imported[1]));
            InParameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D12Resource*>(imported[2]));
            InParameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D12Resource*>(imported[3]));
            const std::array<const char*,4> names={NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,
                NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Output};
            bool parametersMatch=true;
            const GenshinSubstage::Resource substageResources[]={GenshinSubstage::Resource::Color,
                GenshinSubstage::Resource::Depth,GenshinSubstage::Resource::Motion,GenshinSubstage::Resource::Output};
            GenshinSubstage::Event(GenshinSubstage::Stage::ParameterCheck,GenshinSubstage::State::Running);
            // Observe actual dimensions without changing this path's existing checks/gates.
            for(size_t i=0;i<imported.size();++i)if(imported[i]->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                GenshinSubstage::Event(GenshinSubstage::Stage::ParameterCheck,GenshinSubstage::State::Running,
                    substageResources[i],GenshinSubstage::Domain::Parameters,0,GenshinSubstage::Issue::NonTex2D);break;
            }
            for(size_t i=0;i<imported.size();++i)
            {
                ID3D12Resource* typed=nullptr;
                void* generic=nullptr;
                if(InParameters->Get(names[i],&typed)!=NVSDK_NGX_Result_Success || typed!=imported[i] ||
                   InParameters->Get(names[i],&generic)!=NVSDK_NGX_Result_Success || generic!=imported[i])
                {
                    GenshinSubstage::Event(GenshinSubstage::Stage::ParameterCheck,GenshinSubstage::State::Failed,
                        substageResources[i],GenshinSubstage::Domain::Parameters,E_INVALIDARG,GenshinSubstage::Issue::Rejected);
                    LOG_ERROR("Genshin D3D12 parameter roundtrip failed for {}",names[i]);
                    parametersMatch=false;
                    break;
                }
            }
            if(!parametersMatch){genshinStop="resource-parameter-roundtrip";genshinStatus=E_INVALIDARG;break;}
        }
        else
        {
            InParameters->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D12Resource*>(dx11Color.Dx12Resource));
            InParameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D12Resource*>(dx11Mv.Dx12Resource));
            InParameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D12Resource*>(dx11Out.Dx12Resource));
            InParameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D12Resource*>(dx11Depth.Dx12Resource));
        }

        if (genshinTransport)
        {
            // The observation seam leases exactly four resources; optional cache guides cannot escape into it.
            InParameters->Set(NVSDK_NGX_Parameter_ExposureTexture, (void*) nullptr);
            InParameters->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, (void*) nullptr);
        }
        else
            SetOptionalDx12Inputs(InParameters, dx11Exp.Dx12Resource, dx11Reactive.Dx12Resource, AutoExposure(),
                                  Config::Instance()->DisableReactiveMask.value_or(false));

        LOG_DEBUG("Dispatch!!");
        ID3D12Resource* beforeSrColor = nullptr;
        ID3D12Resource* hoyoColor = genshinTransport ? genshinTransport->Resource(genshinScope->owner, 0) : dx11Color.Dx12Resource;
        if (HoYoShade::Selected(HoYoShade::Stage::BeforeSr) && hoyoColor && Dx12Fence &&
            Dx12FenceValue < UINT64_MAX - 1)
        {
            // Preallocate the existing failure owner before the filter can
            // record. Legacy bridge routes need the same no-release handling
            // as Genshin when a source barrier/submission becomes uncertain.
            if (!genshinRuntimeRetention) {
                genshinRuntimeRetention = std::make_unique<Dx11BridgeInitRetention>();
                genshinRuntimeRetention->device = BridgeWorkDevice();
                genshinRuntimeRetention->workDomain = genshinWorkDomain;
                genshinRuntimeRetention->queue = Dx12CommandQueue;
            }
            const auto producerValue = ++Dx12FenceValue;
            const auto producerHr = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12SignalApi,[&]{return Dx12CommandQueue->Signal(Dx12Fence, producerValue);},GenshinHookCpuTiming::ResultKind::Hresult);
            if (FAILED(producerHr)) { genshinStop="hoyoshade-upload-signal"; genshinStatus=producerHr; break; }
            Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice;
            InDeviceContext->GetDevice(&sourceDevice);
            HoYoShade::ColorEndpoint endpoint;
            endpoint.stage = HoYoShade::Stage::BeforeSr; endpoint.color = hoyoColor; endpoint.queue = Dx12CommandQueue;
            endpoint.ready = {Dx12CommandQueue, Dx12Fence, producerValue};
            endpoint.stream = reinterpret_cast<uint64_t>(this);
            endpoint.generation = genshinScope ? genshinScope->owner.generation : 1;
            endpoint.frame = genshinScope ? genshinScope->owner.sourceFrame : _frameCount;
            endpoint.width = RenderWidth();
            endpoint.height = RenderHeight();
            if (!genshinTransport) {
                // Same render-size resolver as the selected DX12 providers;
                // querying before Evaluate avoids using last frame's extent.
                GetRenderResolution(InParameters, &endpoint.width, &endpoint.height);
                const auto selected = dx12Feature->GetUpscalerType();
                // DLSS forwards these keys; XeSS reads them into inputColorBase.
                // Current FSR/FFX adapters consume the origin-zero rectangle.
                if (selected == Upscaler::DLSS || selected == Upscaler::DLSSD || selected == Upscaler::XeSS) {
                    InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &endpoint.colorX);
                    InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &endpoint.colorY);
                }
            }
            endpoint.state = D3D12_RESOURCE_STATE_COMMON; endpoint.stateKnown = true;
            if (genshinTransport && genshinScope) {
                // API20 external RTV processing preserves native typed values.
                // Output swapchain metadata and requested !HDR cannot certify
                // this pre-SR color's transfer. Leave sdr=false, bind its real
                // producer, and authenticate again at recording boundaries.
                endpoint.colorMode = HoYoShade::ColorMode::GenshinNativeTypedBeforeSr;
                endpoint.nativeColor.producer = GenshinTransport::ProducerInvocation::Current();
                endpoint.nativeColor.transport = genshinTransport;
                endpoint.nativeColor.scope = genshinScope;
                endpoint.nativeColor.providerHdrKnown = dx12Feature->GetFeatureFlagsKnown();
                endpoint.nativeColor.providerHdr = endpoint.nativeColor.providerHdrKnown &&
                    (dx12Feature->GetFeatureFlags() & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
            } else {
                // Other games/routes retain the transfer-qualified contract.
                endpoint.sdr = dx12Feature->GetFeatureFlagsKnown() &&
                    (dx12Feature->GetFeatureFlags() & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) == 0 &&
                    State::Instance().outputColorSpace.valid &&
                    State::Instance().outputColorSpace.dxgiColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            }
            // Source observation only: a natural selection donor may belong
            // to another API/device. It never lends GPU objects to this stage.
            endpoint.naturalDevice = reinterpret_cast<uint64_t>(sourceDevice.Get());
            endpoint.naturalContract = genshinTransport && genshinScope
                ? HoYoShade::NaturalSourceContract::BeforeSrOwnedRuntimeWithSelectionDonor
                : HoYoShade::NaturalSourceContract::BeforeSrExactDevice;
            if (genshinTransport && genshinScope) {
                const bool directionKnown = dx12Feature->GetFeatureFlagsKnown();
                const bool reversed = directionKnown && (dx12Feature->GetFeatureFlags() &
                    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
                endpoint.depth = HoYoShade::Depth::FromBeforeSr(genshinTransport, genshinScope->owner,
                    Dx12CommandQueue, Dx12Fence, producerValue, endpoint.stream,
                    endpoint.generation, endpoint.frame, RenderWidth(), RenderHeight(), directionKnown, reversed);
            }
            const auto filtered = HoYoShade::RenderStage(endpoint);
            if (filtered.Uncertain()) {
                // No SR consumer may read a color with uncertain barriers or
                // an unsignalled owned batch. Keep this feature's transport.
                genshinSubmissionUnfenced = true; genshinStop="hoyoshade-before-SR-uncertain"; break;
            }
            if (filtered.Submitted()) {
                // RenderStage submitted and restored THIS input on THIS queue.
                // SR's later Execute therefore consumes the real copyback;
                // CPU return alone is never interpreted as GPU completion.
                if (!filtered.completion.fence || !filtered.completion.value ||
                    filtered.completion.value == UINT64_MAX ||
                    filtered.completion.fence->GetCompletedValue() == UINT64_MAX) {
                    genshinSubmissionUnfenced = true; genshinStop="hoyoshade-before-SR-tail"; break;
                }
                if (!genshinTransport &&
                    (WithDx12::GetD3D12CommandQueue() != Dx12CommandQueue ||
                     !Dx11WithDx12::SyncDx12ToDx11())) {
                    // Even if provider evaluation later fails, the legacy
                    // D3D11 cache/unknown readers must not race filter copyback.
                    // Failed handoff keeps the actual owners until restart.
                    genshinSubmissionUnfenced = true; genshinStop="hoyoshade-before-SR-handoff"; break;
                }
                beforeSrColor = hoyoColor; // certify consumption only at the provider boundary below
            }
        }
        const bool bridgeFinishedNr = Config::Instance()->DlssNrFinishedPicture.value_or_default() &&
            !Config::Instance()->DlssNrRunBeforeSr.value_or_default() &&
            !Config::Instance()->DlssNrDeferredDlss.value_or_default();
        CallerCpuTrace::Scope nrBeforeCpu(CallerCpuTrace::Op::NrBefore, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
        if (!genshinTransport && !native::OpticalFgNrEnabled() && bridgeFinishedNr && Config::Instance()->DlssNrEnabled.value_or_default())
            DlssNr::CaptureFinishedPictureDx11Bridge(cmdList, Dx12CommandQueue, InParameters,
                                                    dx12Feature->GetUpscalerType() == Upscaler::DLSSD,
                                                    (unsigned) dx12Feature->GetFeatureFlags(),
                                                    dx12Feature->GetFeatureFlagsKnown(),
                                                    { dx12Feature->Handle() ? dx12Feature->Handle()->Id : 0,
                                                      dx12Feature.get(), Handle() ? Handle()->Id : 0 });
        else if (!genshinTransport && !native::OpticalFgNrEnabled() && !bridgeFinishedNr)
            DlssNr::EvaluateBeforeUpscale(cmdList, InParameters, Dx12CommandQueue, _frameCount,
                                      dx12Feature->GetUpscalerType() == Upscaler::DLSSD);
        nrBeforeCpu.Finish();
        CallerCpuTrace::Scope providerEvaluateCpu(CallerCpuTrace::Op::ProviderEvaluate, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
        GenshinCpuStageTiming::Scope genshinProviderCpu(genshinScope?GenshinCpuStageTiming::Stage::OptProviderEvaluate:GenshinCpuStageTiming::Stage::Count);
        if (genshinTransport && !native::OpticalFgNrEnabled() &&
            Config::Instance()->DlssNrEnabled.value_or_default() && !bridgeFinishedNr &&
            !Config::Instance()->DlssNrRunBeforeSr.value_or_default() &&
            !Config::Instance()->DlssNrDeferredDlss.value_or_default())
            genshinNrCaller = GenshinCaller::BridgeAfterUpscale::Observe(
                InDeviceContext, InParameters, Dx12CommandQueue, cmdList);
        const auto evaluateProvider = [&] {
            // Recheck after optional consumers/callbacks, immediately before
            // selected SR records. Set(Color) never snapshots texture pixels.
            if (beforeSrColor) {
                ID3D12Resource* providerColor = nullptr;
                if (InParameters->Get(NVSDK_NGX_Parameter_Color, &providerColor) != NVSDK_NGX_Result_Success)
                    InParameters->Get(NVSDK_NGX_Parameter_Color, reinterpret_cast<void**>(&providerColor));
                if (providerColor != beforeSrColor || (genshinTransport &&
                    genshinTransport->Resource(genshinScope->owner, 0) != beforeSrColor)) {
                    genshinStop="hoyoshade-before-SR-consumer-color"; return false;
                }
            }
            // Only the authenticated Genshin transport has acquired its
            // selected recording dependencies. Other routes retain their legacy entry.
            if (genshinTransport)
            {
                GenshinSrRecordingCpu::Scope recordingCpu(dx12Feature.get(), GenshinSrRecordingCpu::Stage::ProviderRecord, true);
                // Upgrade to the old global wait before entering any newly
                // selected shared helper. No pass has been recorded here yet.
                if (isolatedPassResources && !canIsolatePassResources())
                {
                    const auto sharedReady = WaitForGenshinRecording(commandFrame, false);
                    if (FAILED(sharedReady)) return false;
                    isolatedPassResources = false;
                }
                auto& observation=genshinRecordings[commandFrame];observation.phase="provider-record";
                observation.isolated=isolatedPassResources;
                const bool evaluated = dx12Feature->EvaluateForRecordingSlot(cmdList, InParameters, commandFrame,
                                                                             isolatedPassResources);
                observation.providerReturned=true;observation.providerOk=evaluated;
                recordingCpu.Finish(evaluated ? S_OK : E_FAIL);
                return evaluated;
            }
            return GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::ProviderEvaluate,[&]{return dx12Feature->Evaluate(cmdList, InParameters);},GenshinHookCpuTiming::ResultKind::Boolean);
        };
        if (genshinNrCaller)
            dx12EvalResult = genshinNrCaller->EvaluateProvider(evaluateProvider);
        else
            dx12EvalResult = evaluateProvider();
        providerEvaluateCpu.Finish();
        if(genshinTransport)genshinStop=dx12EvalResult?"provider-succeeded":"provider-refused";

        // DLSS 5 Neural Rendering rides the bridge: at this moment the block carries the D3D12 copies
        // of every input, the list is still recording, and the model's edit lands on the D3D12 output
        // before it is copied back to the game's D3D11 texture. This one call is what makes the pass
        // work in DirectX 11 games, whatever upscaler carried it here.
        static bool reportedNrOffer = false;

        if (!reportedNrOffer)
        {
            reportedNrOffer = true;
            LOG_INFO("DLSS-NR: the D3D11 bridge reached the hand-off (upscale ok: {}, enabled: {})",
                     dx12EvalResult, Config::Instance()->DlssNrEnabled.value_or_default());

        }

        CallerCpuTrace::Scope nrAfterCpu(CallerCpuTrace::Op::NrAfter, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
        if (!genshinTransport && !native::OpticalFgNrEnabled() && dx12EvalResult && Config::Instance()->DlssNrEnabled.value_or_default())
        {
            if (!bridgeFinishedNr)
                DlssNr::EvaluateAfterUpscale(cmdList, InParameters, Dx12CommandQueue,
                                         dx12Feature->GetUpscalerType() == Upscaler::DLSSD,
                                         _frameCount);

            // Asked only after the D3D12 path has had its turn. Probing first would have made a D3D11
            // init the very first thing to ever touch the snippet, and if that had left its core
            // holding a D3D11 device the D3D12 create would have failed -- killing the feature in
            // exactly the games the probe was written to help. Off by default regardless.
            DlssNr::ProbeD3D11(Dx11Device);
        }
        // Only the existing after-SR inline NR lifecycle is supported here.
        // Before-SR/deferred/finished-picture and FG retain their previous admission.
        if (genshinNrCaller && !native::OpticalFgNrEnabled() && dx12EvalResult &&
            Config::Instance()->DlssNrEnabled.value_or_default() && !bridgeFinishedNr &&
            !Config::Instance()->DlssNrRunBeforeSr.value_or_default() &&
            !Config::Instance()->DlssNrDeferredDlss.value_or_default())
        {
            // Acquire inside the actual consumer branch, never through an
            // earlier config reread that can race this decision. NR cannot
            // run its shared helpers after only the private-slot wait.
            if (isolatedPassResources)
            {
                const auto sharedReady = WaitForGenshinRecording(commandFrame, false);
                if (FAILED(sharedReady)) dx12EvalResult = false;
                else isolatedPassResources = false;
            }
            if (dx12EvalResult)
                genshinNrCaller->OfferNr([&](unsigned long long sourceFrame) {
                    DlssNr::EvaluateAfterUpscale(cmdList, InParameters, Dx12CommandQueue,
                        dx12Feature->GetUpscalerType() == Upscaler::DLSSD, sourceFrame);
                });
        }
        nrAfterCpu.Finish();

    } while (false);

    }
    catch (...)
    {
        if(genshinTransport){genshinCommandPathFailed=true;genshinTransport->Revoke();}
        if(commandListRecording)cmdList->Close();
        if(genshinTransport && dx12Feature) dx12Feature->FinishRecordingSlotResources(commandFrame, false);
        if(genshinTransport)LogGenshinFailure("recording-exception",E_FAIL,commandFrame);
        throw; // Six resource parameters and actual D3D11 state unwind before the SR scope adopts the lease.
    }
    const bool parametersRestored=restoreParameters.RestoreNow();
    if (genshinTransport)
    {
        savedState.reset();
        auto* scope=genshinScope;
        if(!genshinTransport->Restored(scope->owner,parametersRestored,true))dx12EvalResult=false;
    }

    if (!dx12EvalResult && commandListRecording)
        DlssNr::FinishedPictureResetCommandList(cmdList);
    if (commandListRecording)
    {
        if(genshinTransport)genshinTransport->EndGpuTimeline(genshinScope->owner,cmdList);
        const auto closeResult = cmdList->Close();
        if(genshinTransport)genshinRecordings[commandFrame].closeHr=closeResult;
        if (closeResult != S_OK)
        {
            if(genshinTransport){genshinStop="command-list-close";genshinStatus=closeResult;}
            LOG_ERROR("CommandList Close error: {:X}", (UINT) closeResult);
            DlssNr::FinishedPictureResetCommandList(cmdList);
            dx12EvalResult = false;
            if(genshinTransport){genshinCommandPathFailed=true;genshinTransport->Revoke();}
        }
    }

    if (dx12EvalResult)
    {
        ID3D12CommandList* ppCommandLists[] = { cmdList };
        GenshinSrRecordingCpu::Scope executeSignalCpu(dx12Feature.get(), GenshinSrRecordingCpu::Stage::ExecuteSignal,
                                                    genshinTransport != nullptr);
        CallerCpuTrace::Scope executeCpu(CallerCpuTrace::Op::Execute, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
        if(genshinTransport)GenshinSubstage::Event(GenshinSubstage::Stage::QueueExecute,GenshinSubstage::State::Running);
        if(genshinTransport)genshinSubmissionUnfenced=true;
        const auto nrSubmitCallbacksBefore = DlssNr::FinishedPictureSubmissionCallbacks();
        if(genshinTransport){
            GenshinSrRecordingCpu::Scope transportCpu(dx12Feature.get(), GenshinSrRecordingCpu::Stage::TransportExecuteSignal, true);
            genshinRecordings[commandFrame].phase="execute";
            const auto submission=genshinTransport->Execute(genshinScope->owner,Dx12CommandQueue,cmdList,Dx12CommandAllocator[commandFrame]);
            auto& observation=genshinRecordings[commandFrame];
            observation.executed=submission.executed;observation.executeReturned=submission.returned;
            observation.submitted=submission.submitted;observation.submitHr=submission.hr;
            transportCpu.Finish(submission.submitted ? S_OK : E_FAIL);
            commandListExecuted=submission.executed;commandListExecuteReturned=submission.returned;
            genshinSubmitted=submission.submitted;
            if(!genshinSubmitted){
                genshinStop=submission.reason;genshinStatus=submission.hr;
                genshinCommandPathFailed=true;dx12EvalResult=false;
                // Preflight failure issued no Execute and needs no feature-fence quarantine.
                if(!commandListExecuted)genshinSubmissionUnfenced=false;
            }
            GenshinSubstage::Event(GenshinSubstage::Stage::QueueExecute,
                genshinSubmitted?GenshinSubstage::State::Succeeded:GenshinSubstage::State::Failed,
                GenshinSubstage::Resource::Queue,GenshinSubstage::Domain::Hresult,submission.hr);
        }else{
            GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12Submit,[&]{return Dx12CommandQueue->ExecuteCommandLists(1, ppCommandLists);},GenshinHookCpuTiming::ResultKind::None);commandListExecuted=commandListExecuteReturned=true;
        }
        executeCpu.Finish();
        // Fall back only when this synchronous Execute did not reach the real submit hook.
        // Persistent recording watchers must not mistake duplicate notification for replay.
        if (commandListExecuteReturned && DlssNr::FinishedPictureSubmissionCallbacks() == nrSubmitCallbacksBefore)
            DlssNr::FinishedPictureSubmitted(Dx12CommandQueue, 1, ppCommandLists);

        if(commandListExecuted){
            const auto fenceValue = ++Dx12FenceValue;
            CallerCpuTrace::Scope signalCpu(CallerCpuTrace::Op::Signal, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame, fenceValue);
            GenshinTransport::GpuWitness featureFence;
            if(genshinTransport && (!genshinWorkDomain.BoundQueue(Dx12CommandQueue,featureFence) ||
                !featureFence.Owns(genshinWorkDomain.PublicOwner(),Dx12Fence,"feature-fence-device"))){
                result=featureFence.hr;genshinStop=featureFence.reason;genshinStatus=result;
            }else {
                GenshinSrRecordingCpu::Scope featureSignalCpu(dx12Feature.get(), GenshinSrRecordingCpu::Stage::FeatureSignal,
                                                            genshinTransport != nullptr);
                result = GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::Dx12SignalApi,[&]{return Dx12CommandQueue->Signal(Dx12Fence, fenceValue);},GenshinHookCpuTiming::ResultKind::Hresult);
                featureSignalCpu.Finish(result);
            }
            signalCpu.Finish();
            if(genshinTransport)genshinRecordings[commandFrame].signalHr=result;
            if (result != S_OK)
            {
                if(genshinTransport){if(SUCCEEDED(featureFence.hr))genshinStop="feature-fence-signal";genshinStatus=result;}
                LOG_ERROR("Dx12CommandQueue Signal failed for feature fence {}: {:X}", fenceValue, (UINT) result);
                dx12EvalResult = false;
                if(genshinTransport){genshinCommandPathFailed=true;genshinTransport->Revoke();}
            }
            else
            {
                Dx12CommandAllocatorFenceValue[commandFrame] = fenceValue;
                if(genshinTransport){genshinLastSubmittedFence=fenceValue;
                    genshinLastSubmissionIsolated=isolatedPassResources;genshinSubmissionUnfenced=false;}
            }
        }
        executeSignalCpu.Finish(dx12EvalResult ? S_OK : E_FAIL);
    }

    // Execute is irreversible even when its later Signal fails. Such work
    // retains every slot through the existing unfenced/quarantine owner.
    // Only unexecuted lists may roll back CPU resource-state bookkeeping.
    if(genshinTransport) dx12Feature->FinishRecordingSlotResources(commandFrame, commandListExecuted);
    if(genshinTransport && !genshinSubmitted)dx12EvalResult=false;

    auto evalResult = false;

    do
    {
        if (!dx12EvalResult || !commandListExecuted)
            break;

        if (!genshinTransport && !CopyBackOutput())
        {
            LOG_ERROR("Can't copy output texture back!");
            break;
        }

        if (genshinTransport)
        {
            auto* scope = genshinScope;
            const auto outputRoute=GenshinTransport::CurrentOutputRoute();
            const bool deferOutputFlush=dx12Feature->GetUpscalerType()==Upscaler::DLSS && outputRoute.deferOutputFlush;
            if (!genshinTransport->Copyback(scope->owner,deferOutputFlush)){
                genshinStop="output-copyback-refused";
                break;
            }
            if(deferOutputFlush &&
               !GenshinTransport::QueueDeferredOutputFlush(genshinTransport,scope->owner)){
                genshinTransport->Revoke();genshinStop="output-flush-handoff-refused";break;
            }
            genshinStop="output-copyback-ordered";
        }

        if (!genshinTransport)
            DlssNr::FinishedPictureDx11BridgeOutputHandoff(Dx12CommandQueue, cmdList);

        evalResult = true;

    } while (false);

    if (evalResult)
        _frameCount++;
    else if (!genshinTransport)
        Dx11WithDx12::ClearLastPreparedUpscalerFrameState();

    if(genshinTransport){
        auto& observation=genshinRecordings[commandFrame];
        observation.phase=genshinStop;observation.copyback=evalResult;
        if(!evalResult)LogGenshinFailure(genshinStop,genshinStatus,commandFrame);
        static std::atomic<unsigned> failures{0},successes{0};
        auto& count=evalResult?successes:failures;
        if(count.fetch_add(1,std::memory_order_relaxed)<6){const auto o=genshinTransport->Snapshot();
            LOG_INFO("Genshin SR command return: pid={} instance={} generation={} stop={} hr={:X} "
                "recording={} executed={} submitted={} provider_ok={} result={} stage={} queue={} list={} "
                "allocator={} frame={} required_fence={} completed_fence={}",GetCurrentProcessId(),o.owner.instance,
                o.owner.generation,genshinStop,static_cast<UINT>(genshinStatus),commandListRecording,commandListExecuted,
                genshinSubmitted,dx12EvalResult,evalResult,static_cast<unsigned>(o.stage),o.queue,
                reinterpret_cast<uint64_t>(cmdList),reinterpret_cast<uint64_t>(Dx12CommandAllocator[commandFrame]),
                commandFrame,Dx12CommandAllocatorFenceValue[commandFrame],GenshinHookCpuTiming::Call(GenshinHookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},GenshinHookCpuTiming::ResultKind::Fence));
        }
    }
    callerCpuScope.Result(evalResult);
    return evalResult;
}

bool IFeature_Dx11wDx12::BaseInit(ID3D11Device* InDevice, ID3D11DeviceContext* InContext,
                                  NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (!InContext)
    {
        LOG_ERROR("context is null!");
        return false;
    }

    auto contextResult = InContext->QueryInterface(IID_PPV_ARGS(&Dx11DeviceContext));
    if (contextResult != S_OK)
    {
        LOG_ERROR("QueryInterface ID3D11DeviceContext4 result: {0:x}", contextResult);
        return false;
    }
    else
    {
        Dx11DeviceContext->Release();
    }

    if (!InDevice)
        Dx11DeviceContext->GetDevice(&InDevice);

    auto dx11DeviceResult = InDevice->QueryInterface(IID_PPV_ARGS(&Dx11Device));

    if (dx11DeviceResult != S_OK)
    {
        LOG_ERROR("QueryInterface ID3D11Device5 result: {0:x}", dx11DeviceResult);
        return false;
    }
    else
    {
        Dx11Device->Release();
    }

    if (!WithDx12::PrepareD3D12ForD3D11(InDevice, D3D_FEATURE_LEVEL_11_0))
    {
        LOG_ERROR("Cannot resolve D3D12 device/queue from WithDx12!");
        return false;
    }

    _dx11on12Device = WithDx12::GetD3D12Device();
    if (_dx11on12Device == nullptr)
    {
        LOG_ERROR("Cannot get D3D12 device from WithDx12!");
        return false;
    }

    Dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
    if (Dx12CommandQueue == nullptr)
    {
        LOG_ERROR("Cannot get D3D12 command queue from WithDx12!");
        return false;
    }

    Dx12CommandListType = WithDx12::GetD3D12CommandListType();

    // Only this executable receives a private SR queue. Keep the WithDx12
    // creator for NGX and every creation; retain the allocator's public owner
    // separately for exact forwarded-child witnesses.
    const auto executable=Util::ExePath().filename().wstring();
    if(GameRouting::IdentifyExecutable(executable.c_str())==GameRouting::Identity::Genshin){
        GenshinTransport::GpuWitness witness;
        if(!genshinWorkDomain.Create(_dx11on12Device,witness)){
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1,std::memory_order_relaxed)<6)
                LOG_ERROR("Genshin SR work domain refused: reason={} hr={:X}",witness.reason,static_cast<UINT>(witness.hr));
            return false;
        }
        Dx12CommandQueue=genshinWorkDomain.Queue();
        Dx12CommandListType=D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    if (!CreateD3D12Objects())
    {
        LOG_ERROR("Failed to create D3D12 objects!");
        return false;
    }

    Dx11WithDx12::Init(Dx11Device, Dx11DeviceContext);

    return true;
}

IFeature_Dx11wDx12::IFeature_Dx11wDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters)
{
}

IFeature_Dx11wDx12::~IFeature_Dx11wDx12()
{
    if (initRetention)
    {
        if (bridgeInitState.RequiresRetention() || bridgeInitState.Ready()) RetainFailedInitialization();
        else ReleaseInitReservation(initRetention);
    }

    if(genshinRuntimeRetention){
        // Shutdown is not completion. Do not let member destruction free the
        // provider/pass uploads while a prior submission lacks a safe witness.
        if(State::Instance().isShuttingDown || !ReleaseSharedResources(true))RetainGenshinRuntime();
        return;
    }
    if (State::Instance().isShuttingDown)
        return;

    // This also covers an initialized Genshin feature destroyed before Evaluate,
    // when the runtime retention slot has not yet been allocated.
    ReleaseSharedResources(genshinWorkDomain.Creator() != nullptr);
}
