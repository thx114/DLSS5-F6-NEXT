#include <pch.h>
#include <native/OpticalFgNrPolicy.h>
#include <integration/HoYoShade.h>
#include <integration/HoYoShadeNaturalAssociation.h>
#include <integration/HoYoShadeBridgeDepth.h>
#include "IFeature_Dx11wDx12.h"
#include <runtime/GuideCapture.h>
#include <runtime/GuideSourceMetadataNgx.h>
#include <runtime/FenceCompletion.h>
#include "Dx11BridgeStateGuard.h"
#include "BridgeResourceParameterGuard.h"
#include "NgxDx12ResourceView.h"
#include "NgxOptionalDx12Inputs.h"
#include "CallerCpuTrace.h"
#include <runtime/HookCpuTiming.h>
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
    HookCpuTiming::Scope r448Wait(HookCpuTiming::Stage::ProviderPoolLock);
    std::lock_guard<std::mutex> lock(pool.mutex);r448Wait.Finish();
    HookCpuTiming::Scope r448Held(HookCpuTiming::Stage::ProviderPoolLocked);
    for (auto& slot : pool.slots)
        if (!slot.claimed) { slot.claimed = true; return &slot; }
    return nullptr;
}
void ReleaseInitReservation(Dx11BridgeInitRetention*& slot)
{
    if (!slot) return;
    auto& pool = RetentionSlots();
    HookCpuTiming::Scope r448Wait(HookCpuTiming::Stage::ProviderPoolLock);
    std::lock_guard<std::mutex> lock(pool.mutex);r448Wait.Finish();
    HookCpuTiming::Scope r448Held(HookCpuTiming::Stage::ProviderPoolLocked);
    slot->device.Reset(); slot->queue.Reset();
    slot->claimed = false;
    slot = nullptr;
}

// DRED retains only the last 64K operations; its counter is not ring-bounded.
// A late failure report must never index the history with that absolute counter.
int DredOperation(const D3D12_AUTO_BREADCRUMB_NODE1& node,UINT ordinal) noexcept
{
    constexpr UINT historyCapacity=65536;
    const auto retainedBegin=node.BreadcrumbCount>historyCapacity?node.BreadcrumbCount-historyCapacity:0;
    if(!node.pCommandHistory || ordinal<retainedBegin || ordinal>=node.BreadcrumbCount)return -1;
    return static_cast<int>(node.pCommandHistory[ordinal%historyCapacity]);
}
const char* DredOperationName(int op) noexcept
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
void LogBridgeDred(ID3D12Device* device,HRESULT reason)
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
            const int nextOp=known?DredOperation(*node,completed):-1;
            const int priorOp=known&&completed?DredOperation(*node,completed-1):-1;
            const std::string listA=node->pCommandListDebugNameA?node->pCommandListDebugNameA:"";
            const std::string listW=node->pCommandListDebugNameW?wstring_to_string(node->pCommandListDebugNameW):"";
            const std::string queueA=node->pCommandQueueDebugNameA?node->pCommandQueueDebugNameA:"";
            const std::string queueW=node->pCommandQueueDebugNameW?wstring_to_string(node->pCommandQueueDebugNameW):"";
            LOG_WARN("R24 DRED node: index={} list={} queue={} count={} completed={} prior_op={} next_op={} listA={} listW={} "
                "completed_known={} prior_name={} next_name={} queueA={} queueW={} causal_order=false",
                index,reinterpret_cast<uint64_t>(node->pCommandList),reinterpret_cast<uint64_t>(node->pCommandQueue),
                node->BreadcrumbCount,completed,priorOp,nextOp,listA,listW,known,
                DredOperationName(priorOp),DredOperationName(nextOp),queueA,queueW);
            if(!pending || detailNodes++>=16)continue;
            const UINT begin=completed>4?completed-4:0;
            const UINT end=completed+std::min<UINT>(node->BreadcrumbCount-completed,5);
            for(UINT ordinal=begin;ordinal<end;++ordinal) {
                const auto op=DredOperation(*node,ordinal);
                LOG_WARN("R1068 DRED history: index={} list={} ordinal={} completed={} op={} name={} "
                    "retained={} failed_operation=UNKNOWN",index,reinterpret_cast<uint64_t>(node->pCommandList),
                    ordinal,completed,op,DredOperationName(op),op>=0);
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

void IFeature_Dx11wDx12::ReportInteropFenceFault(const char* phase,UINT slot,UINT64 target,
                                               UINT64 completed,DWORD waitResult) noexcept
{
    // Also covers direct NGX: the old Genshin transport-domain logger is not
    // active on that route. Observation never authorizes Reset or reuse.
    const auto lastError=GetLastError();
    auto* device=BridgeWorkDevice();
    const auto dx12=device?device->GetDeviceRemovedReason():E_POINTER;
    const auto dx11=Dx11Device?Dx11Device->GetDeviceRemovedReason():E_POINTER;
    auto& reported=FAILED(dx12)||FAILED(dx11)?interopDeviceFaultReported:interopFenceFaultReported;
    if(!reported.exchange(true,std::memory_order_relaxed))try {
        LOG_ERROR("F6 interop fence fault: pid={} phase={} provider_frame={} slot={} queue={} fence={} target={} "
            "completed={} wait_result={} dx12_removed={:X} dx11_removed={:X} max_is_not_complete=true observation_only=true",
            GetCurrentProcessId(),phase,_frameCount,slot,reinterpret_cast<uintptr_t>(Dx12CommandQueue),
            reinterpret_cast<uintptr_t>(Dx12Fence),target,completed,waitResult,static_cast<UINT>(dx12),static_cast<UINT>(dx11));
        if(FAILED(dx12))LogBridgeDred(device,dx12);
    }catch(...){}
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
void IFeature_Dx11wDx12::RetainRuntime() noexcept
{
    // No allocation after Execute or during destruction. Retain the provider,
    // upload/descriptors, device, queue, allocators, lists, fence and event together.
    auto* retained=runtimeRetention.release();
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

    return true;
}

bool IFeature_Dx11wDx12::ReleaseSharedResources(bool releaseProvider)
{
    // An exception after Execute but before its successful Signal has no safe
    // fence target. Do not manufacture a future target or release those owners.
    if(submissionUnfenced)return false;
    UINT64 lastFence = Dx12FenceValue;
    for (const auto value : Dx12CommandAllocatorFenceValue)
        lastFence = std::max(lastFence, value);
    if (lastFence == UINT64_MAX || (lastFence != 0 && Dx12Fence == nullptr)) return false;

    if (lastFence != 0 && Dx12Fence != nullptr)
    {
        const auto completedBefore = HookCpuTiming::Call(HookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},HookCpuTiming::ResultKind::Fence);
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

            const auto setEventResult = HookCpuTiming::Call(HookCpuTiming::Stage::FenceArmApi,[&]{return Dx12Fence->SetEventOnCompletion(lastFence, Dx12FenceEvent);},HookCpuTiming::ResultKind::Hresult);
            if (setEventResult != S_OK)
            {
                LOG_ERROR("Dx11wDx12 release SetEventOnCompletion failed for fence {}: {:X}; retaining bridge resources",
                          lastFence, (UINT) setEventResult);
                return false;
            }

            const auto waitResult = HookCpuTiming::Call(HookCpuTiming::Stage::CpuEventWait,[&]{return WaitForSingleObject(Dx12FenceEvent, 5000);},HookCpuTiming::ResultKind::Event);
            const auto completedAfter = HookCpuTiming::Call(HookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},HookCpuTiming::ResultKind::Fence);
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

    // GPU completion alone leaves these executable recordings pinned by NR.
    // Resolution/context recreation must close each old generation through a
    // successful real Reset before destroying its provider and command owners.
    for (size_t i = 0; i < DX11WDX12_COMMAND_BUFFER_COUNT; ++i)
    {
        if (!Dx12CommandList[i] && !Dx12CommandAllocator[i]) continue;
        const auto retired = DlssNr::RetireFinishedPictureRecording(Dx12CommandList[i], Dx12CommandAllocator[i]);
        if (FAILED(retired))
        {
            LOG_WARN("Dx11wDx12 release recording closure refused: slot {} hr {:X}; retaining bridge resources",
                     i, static_cast<UINT>(retired));
            return false;
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

    if (Dx12FenceEvent)
    {
        CloseHandle(Dx12FenceEvent);
        Dx12FenceEvent = nullptr;
    }

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
    const auto completedBefore = HookCpuTiming::Call(HookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},HookCpuTiming::ResultKind::Fence);
    const auto completionBefore = FramePipeline::ObserveFence(completedBefore, allocatorFenceValue);
    if (completionBefore == FramePipeline::FenceCompletion::Invalid)
    { ReportInteropFenceFault("allocator-fence-invalid",commandFrame,allocatorFenceValue,completedBefore,WAIT_FAILED);return false; }
    if (completionBefore == FramePipeline::FenceCompletion::Pending)
    {
        result = HookCpuTiming::Call(HookCpuTiming::Stage::FenceArmApi,[&]{return Dx12Fence->SetEventOnCompletion(allocatorFenceValue, Dx12FenceEvent);},HookCpuTiming::ResultKind::Hresult);
        if (result != S_OK)
        {
            LOG_ERROR("SetEventOnCompletion error for allocator {} fence {}: {:X}", commandFrame, allocatorFenceValue,
                      (UINT) result);
            return false;
        }

        const auto waitStart = Util::MillisecondsNow();
        CallerCpuTrace::Scope waitCpu(CallerCpuTrace::Op::AllocatorWait, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame, allocatorFenceValue, completedBefore, false, true);
        const auto waitResult = HookCpuTiming::Call(HookCpuTiming::Stage::AllocatorCpuEventWait,[&]{return WaitForSingleObject(Dx12FenceEvent, 5000);},HookCpuTiming::ResultKind::Event);
        waitCpu.Stop();
        const auto waitMs = Util::MillisecondsNow() - waitStart;
        const auto completedAfter = HookCpuTiming::Call(HookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},HookCpuTiming::ResultKind::Fence);
        waitCpu.Finish(waitResult, true);

        if (waitMs > 0.25)
        {
            LOG_WARN("Dx11wDx12 allocator wait: {:.3f} ms, slot: {}, required fence: {}, completed before: {}, "
                     "completed after: {}",
                     waitMs, commandFrame, allocatorFenceValue, completedBefore, completedAfter);
        }

        if (waitResult != WAIT_OBJECT_0 ||
            FramePipeline::ObserveFence(completedAfter, allocatorFenceValue) != FramePipeline::FenceCompletion::Complete)
        {
            ReportInteropFenceFault("allocator-wait-not-complete",commandFrame,allocatorFenceValue,completedAfter,waitResult);
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
                  allocatorFenceValue, HookCpuTiming::Call(HookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},HookCpuTiming::ResultKind::Fence), (UINT) result);
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

    DlssNr::FinishedPictureResetCommandList(Dx12CommandList[commandFrame]);
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
    DlssNr::FinishedPictureResetCommandList(Dx12CommandList[0]);
    CallerCpuTrace::Scope providerInitCpu(CallerCpuTrace::Op::ProviderInit, this, Handle() ? Handle()->Id : 0, _frameCount);
    const bool initialised = dx12Feature->Init(BridgeWorkDevice(), Dx12CommandList[0], InParameters);
    providerInitCpu.Finish();
    const HRESULT closed = Dx12CommandList[0]->Close();
    if (FAILED(closed))
    {
        LOG_ERROR("Init command-list Close failed, creation not submitted: {:X}", (UINT)closed);
        return failUnsubmitted();
    }
    ID3D12CommandList* lists[] = { Dx12CommandList[0] };
    bridgeInitState.Submitted();
    CallerCpuTrace::Scope initExecuteCpu(CallerCpuTrace::Op::Execute, this, Handle() ? Handle()->Id : 0, _frameCount, 0);
    const auto initSubmitCallbacks=DlssNr::FinishedPictureSubmissionCallbacks();
    const auto initPreviewReceipt=DlssNr::FinishedPictureSubmitting(Dx12CommandQueue,1,lists);
    HookCpuTiming::Call(HookCpuTiming::Stage::Dx12Submit,[&]{return Dx12CommandQueue->ExecuteCommandLists(1, lists);},HookCpuTiming::ResultKind::None);
    if(initSubmitCallbacks==DlssNr::FinishedPictureSubmissionCallbacks())
        DlssNr::FinishedPictureSubmitted(Dx12CommandQueue,1,lists,initPreviewReceipt);
    initExecuteCpu.Finish();
    const UINT64 signalled = ++Dx12FenceValue;
    CallerCpuTrace::Scope initSignalCpu(CallerCpuTrace::Op::Signal, this, Handle() ? Handle()->Id : 0, _frameCount, 0, signalled);
    const HRESULT signalResult = HookCpuTiming::Call(HookCpuTiming::Stage::Dx12SignalApi,[&]{return Dx12CommandQueue->Signal(Dx12Fence, signalled);},HookCpuTiming::ResultKind::Hresult);
    initSignalCpu.Finish();
    bridgeInitState.Signalled(signalled, SUCCEEDED(signalResult));
    if (FAILED(signalResult))
    {
        bridgeInitState.Observe(HookCpuTiming::Call(HookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},HookCpuTiming::ResultKind::Fence), FAILED(initRetention->device->GetDeviceRemovedReason()));
        LOG_ERROR("Init queue Signal failed after Execute: {:X}; phase {}", (UINT)signalResult, (unsigned)bridgeInitState.Get());
        RetainFailedInitialization();
        return false;
    }
    Dx12CommandAllocatorFenceValue[0] = signalled;
    UINT64 completed = HookCpuTiming::Call(HookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},HookCpuTiming::ResultKind::Fence);
    bool ready = bridgeInitState.Observe(completed, FAILED(initRetention->device->GetDeviceRemovedReason()));
    if (!ready && bridgeInitState.Get() == Dx11BridgeInit::Phase::SubmittedFenced)
    {
        const HRESULT armed = HookCpuTiming::Call(HookCpuTiming::Stage::FenceArmApi,[&]{return Dx12Fence->SetEventOnCompletion(signalled, Dx12FenceEvent);},HookCpuTiming::ResultKind::Hresult);
        DWORD waited = WAIT_FAILED;
        if (SUCCEEDED(armed))
        {
            CallerCpuTrace::Scope initWaitCpu(CallerCpuTrace::Op::InitWait, this, Handle() ? Handle()->Id : 0, _frameCount, 0, signalled, completed, false, true);
            waited = HookCpuTiming::Call(HookCpuTiming::Stage::InitCpuEventWait,[&]{return WaitForSingleObject(Dx12FenceEvent, 5000);},HookCpuTiming::ResultKind::Event);
            initWaitCpu.Finish(waited, true);
        }
        completed = HookCpuTiming::Call(HookCpuTiming::Stage::FenceQueryApi,[&]{return Dx12Fence->GetCompletedValue();},HookCpuTiming::ResultKind::Fence);
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


bool IFeature_Dx11wDx12::Evaluate(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    HoYoShade::BeginBeforeSrSource(InDeviceContext, reinterpret_cast<uint64_t>(this));
    const uint64_t nrSourceAttempt = nrSourceAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto presentSourceAttempt = DlssNr::BeginGenshinPresentSource();
    // Revoke the last successful observation even on the earliest failure.
    currentInputMetadata.reset();
    GuideSourceMetadata::Ptr evaluatedInputMetadata;

    CallerCpuTrace::Scope callerCpuScope(CallerCpuTrace::Op::Evaluate, this, Handle() ? Handle()->Id : 0, _frameCount, UINT64_MAX, 0, 0, true);
    LOG_FUNC();

    if (!IsInited() || submissionUnfenced || !InParameters)
        return false;

    // Real bridge owner, before BeforeSR admission and any SDK work.
    HoYoShade::RenderOwnerScope hoYoSettingsOwner;
    // Read DX11 inputs through the caller until translation, then expose only
    // the exact DX12 cache resources to SR/NR. Never write cross-API pointers
    // into a native DX11 NGX table. The private legacy transport retains its
    // separately authenticated parameter identity.
    OptiBridge::NgxDx12ResourceView dx12Parameters(*InParameters);
    InParameters = &dx12Parameters;
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
    savedState=std::make_unique<Dx11BridgeStateGuard>(InDeviceContext);
    OptiBridge::ResourceParameterGuard<NVSDK_NGX_Parameter,ID3D11Resource,NVSDK_NGX_Result,6> restoreParameters(
        InParameters,
        {NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_MotionVectors,
         NVSDK_NGX_Parameter_Output,NVSDK_NGX_Parameter_ExposureTexture,NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask},
        {restoreParamColor,restoreParamDepth,restoreParamMv,restoreParamOutput,restoreParamExposure,restoreParamReactive},
        {hasRestoreParamColor,hasRestoreParamDepth,
         hasRestoreParamMv,hasRestoreParamOutput,true,true},
        NVSDK_NGX_Result_Success);

    bool dx12EvalResult = false;
    bool commandListRecording = false;
    bool commandListExecuted = false;
    bool commandListExecuteReturned = false;
    try
    {
    do
    {
        if (!ProcessDx11Textures(InParameters))
        {
            LOG_ERROR("Can't process Dx11 textures!");
            break;
        }

        if (State::Instance().changeBackend[Handle()->Id])
        {
            break;
        }

        commandListRecording = true;


            InParameters->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D12Resource*>(dx11Color.Dx12Resource));
            InParameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<ID3D12Resource*>(dx11Mv.Dx12Resource));
            InParameters->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D12Resource*>(dx11Out.Dx12Resource));
            InParameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D12Resource*>(dx11Depth.Dx12Resource));


        SetOptionalDx12Inputs(InParameters, dx11Exp.Dx12Resource, dx11Reactive.Dx12Resource, AutoExposure(),
                                  Config::Instance()->DisableReactiveMask.value_or(false));

        LOG_DEBUG("Dispatch!!");
        HoYoShade::StageResult beforeSrFilter;
        HoYoShade::ColorEndpoint beforeSrInput;
        IFeature_Dx12* beforeSrProvider = nullptr;
        bool beforeSrFlagsKnown = false;
        decltype(dx12Feature->GetFeatureFlags()) beforeSrFlags{};
        // Read the selected provider's rectangle without mutating this bridge's
        // resolution/jitter history. Match IFeature::GetRenderResolution's
        // fallback, including the provider's own current render dimensions.
        const auto readBeforeSrRegion = [&](HoYoShade::ColorEndpoint& e) {
            auto* provider = dx12Feature.get();
            e.width = provider->RenderWidth(); e.height = provider->RenderHeight();
            unsigned width = 0, height = 0, outWidth = 0, outHeight = 0;
            if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width) == NVSDK_NGX_Result_Success &&
                InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height) == NVSDK_NGX_Result_Success) {
                e.width = width; e.height = height;
            } else if (InParameters->Get(NVSDK_NGX_Parameter_Width, &width) == NVSDK_NGX_Result_Success &&
                       InParameters->Get(NVSDK_NGX_Parameter_Height, &height) == NVSDK_NGX_Result_Success) {
                const bool outputKnown =
                    InParameters->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth) == NVSDK_NGX_Result_Success &&
                    InParameters->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight) == NVSDK_NGX_Result_Success;
                if (outputKnown) {
                    // The actual provider selects OutWidth/OutHeight when the
                    // requested render extent is not smaller than its output.
                    e.width=width<outWidth?width:outWidth;
                    e.height=width<outWidth?height:outHeight;
                } else if (width<e.width) { e.width=width; e.height=height; }
            }
            e.colorX = e.colorY = 0;
            const auto selected = provider->GetUpscalerType();
            if (selected == Upscaler::DLSS || selected == Upscaler::DLSSD || selected == Upscaler::XeSS) {
                InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &e.colorX);
                InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &e.colorY);
            }
        };
        ID3D12Resource* hoyoColor = dx11Color.Dx12Resource;
        if (HoYoShade::Selected(HoYoShade::Stage::BeforeSr) && hoyoColor && Dx12Fence &&
            Dx12FenceValue < UINT64_MAX - 1)
        {
            beforeSrProvider = dx12Feature.get();
            beforeSrFlagsKnown = beforeSrProvider->GetFeatureFlagsKnown();
            beforeSrFlags = beforeSrProvider->GetFeatureFlags();
            // Preallocate the existing failure owner before the filter can
            // record. Legacy bridge routes need the same no-release handling
            // as Genshin when a source barrier/submission becomes uncertain.
            if (!runtimeRetention) {
                runtimeRetention = std::make_unique<Dx11BridgeInitRetention>();
                runtimeRetention->device = BridgeWorkDevice();
                runtimeRetention->queue = Dx12CommandQueue;
            }
            const auto producerValue = ++Dx12FenceValue;
            const auto producerHr = HookCpuTiming::Call(HookCpuTiming::Stage::Dx12SignalApi,[&]{return Dx12CommandQueue->Signal(Dx12Fence, producerValue);},HookCpuTiming::ResultKind::Hresult);
            if (FAILED(producerHr)) {   break; }
            Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice;
            InDeviceContext->GetDevice(&sourceDevice);
            HoYoShade::NaturalAssociation::Source::Scope naturalSource(
                HoYoShade::CaptureBridgeSource(sourceDevice.Get(), reinterpret_cast<uint64_t>(this)));
            HoYoShade::BindNaturalSource(naturalSource.Get());
            HoYoShade::ColorEndpoint endpoint;
            endpoint.stage = HoYoShade::Stage::BeforeSr; endpoint.color = hoyoColor; endpoint.queue = Dx12CommandQueue;
            endpoint.ready = {Dx12CommandQueue, Dx12Fence, producerValue};
            endpoint.stream = reinterpret_cast<uint64_t>(this);
            endpoint.generation = 1;
            endpoint.frame = Dx11WithDx12::GetLastPreparedUpscalerFrameId();
            readBeforeSrRegion(endpoint);
            endpoint.state = D3D12_RESOURCE_STATE_COMMON; endpoint.stateKnown = true;

                // Other games/routes retain the transfer-qualified contract.
                endpoint.sdr = dx12Feature->GetFeatureFlagsKnown() &&
                    (dx12Feature->GetFeatureFlags() & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) == 0 &&
                    State::Instance().outputColorSpace.valid &&
                    State::Instance().outputColorSpace.dxgiColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

            // The actual source chain proves selection through proxy devices.
            // Its backbuffer is association evidence, never the low-resolution SR input.
            endpoint.naturalContract = HoYoShade::NaturalSourceContract::BeforeSrBridgeInput;
            endpoint.naturalSource = naturalSource.Get();
            if (const auto* proof = naturalSource.Get()) {
                const auto& observation = proof->Observation();
                endpoint.naturalDevice = observation.sourceDevice;
                endpoint.naturalBackbuffer = observation.buffer;
                endpoint.generation = observation.generation;
                endpoint.frame = observation.frame;
            }
            const auto sharedRead = Dx11WithDx12::AcquireUpscalerInputRead(Dx12CommandQueue, Dx12Fence, producerValue,
                endpoint.stream, endpoint.generation, endpoint.frame);
            if (!sharedRead) break;
            endpoint.inputOwner = sharedRead;
            unsigned depthX = 0, depthY = 0;
            InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &depthX);
            InParameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &depthY);
            if (beforeSrFlagsKnown && !endpoint.colorX && !endpoint.colorY && !depthX && !depthY)
                endpoint.depth = HoYoShade::Depth::FromBridge(sharedRead, endpoint.width, endpoint.height,
                    (beforeSrFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0);
            const auto filtered = HoYoShade::RenderStage(endpoint);
            if (filtered.Uncertain()) {
                // No SR consumer may read a color with uncertain barriers or
                // an unsignalled owned batch. Keep this feature's transport.
                sharedRead->Quarantine(); submissionUnfenced = true;  break;
            }
            if (filtered.Submitted()) {
                // RenderStage submitted and restored THIS input on THIS queue.
                // SR's later Execute therefore consumes the real copyback;
                // CPU return alone is never interpreted as GPU completion.
                if (!filtered.completion.fence || !filtered.completion.value ||
                    filtered.completion.value == UINT64_MAX ||
                    filtered.completion.fence->GetCompletedValue() == UINT64_MAX ||
                    !sharedRead->BindCompletion(filtered.completion.fence.Get(), filtered.completion.value)) {
                    sharedRead->Quarantine(); submissionUnfenced = true;  break;
                }
                if (
                    (WithDx12::GetD3D12CommandQueue() != Dx12CommandQueue ||
                     !Dx11WithDx12::SyncDx12ToDx11(Dx12CommandQueue)) || !sharedRead->ReturnedAfterGpuWait()) {
                    // Even if provider evaluation later fails, the legacy
                    // D3D11 cache/unknown readers must not race filter copyback.
                    // Failed handoff keeps the actual owners until restart.
                    sharedRead->Quarantine(); submissionUnfenced = true;  break;
                }
                beforeSrFilter = filtered;
                beforeSrInput = endpoint;
            } else sharedRead->CancelUntouched();
        }
        const bool fixedPresentNr = DlssNr::UsesGenshinPresentPipeline();
        const bool bridgeFinishedNr = fixedPresentNr || (Config::Instance()->DlssNrFinishedPicture.value_or_default() &&
            !Config::Instance()->DlssNrRunBeforeSr.value_or_default() &&
            !Config::Instance()->DlssNrDeferredDlss.value_or_default());
        CallerCpuTrace::Scope nrBeforeCpu(CallerCpuTrace::Op::NrBefore, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
        HoYoShade::ColorEndpoint nrSourceRegion;
        const bool diagnosticSource=!Config::Instance()->DlssNrEnabled.value_or_default() && GuideCapture::Wanted(GuideCapture::Stream::Source);
        readBeforeSrRegion(nrSourceRegion);GuideSourceMetadata::Descriptor raw;
            raw.feature=dx12Feature->Handle()?dx12Feature->Handle()->Id:0;
            raw.context=reinterpret_cast<uintptr_t>(dx12Feature.get());
            raw.generation=Handle()?Handle()->Id:0;
            raw.evaluateFrame=nrSourceAttempt;
            raw.flags=static_cast<unsigned>(dx12Feature->GetFeatureFlags());
            raw.flagsKnown=dx12Feature->GetFeatureFlagsKnown();
            raw.renderWidth=nrSourceRegion.width;raw.renderHeight=nrSourceRegion.height;
            // Match the actual NGX output allocation used by Late::Capture.
            ID3D12Resource* evaluatedOutput=nullptr;
            if(InParameters->Get(NVSDK_NGX_Parameter_Output,&evaluatedOutput)!=NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_Output,reinterpret_cast<void**>(&evaluatedOutput));
            if(evaluatedOutput){const auto desc=evaluatedOutput->GetDesc();
                if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.Width<=UINT_MAX){
                    raw.outputWidth=static_cast<unsigned>(desc.Width);raw.outputHeight=desc.Height;}}
            raw.rr=dx12Feature->GetUpscalerType()==Upscaler::DLSSD;
            raw.inputDepth=reinterpret_cast<uintptr_t>(restoreParamDepth);
            raw.inputMotion=reinterpret_cast<uintptr_t>(restoreParamMv);
            ID3D12Resource* depth=nullptr;ID3D12Resource* motion=nullptr;
            if(InParameters->Get(NVSDK_NGX_Parameter_Depth,&depth)!=NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_Depth,reinterpret_cast<void**>(&depth));
            if(InParameters->Get(NVSDK_NGX_Parameter_MotionVectors,&motion)!=NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_MotionVectors,reinterpret_cast<void**>(&motion));
            evaluatedInputMetadata=GuideSourceMetadata::ReadNgx(*InParameters,std::move(raw),depth,motion);
        if ( (fixedPresentNr || !native::OpticalFgNrEnabled()) && ((bridgeFinishedNr && Config::Instance()->DlssNrEnabled.value_or_default()) || diagnosticSource))
            DlssNr::CaptureFinishedPictureDx11Bridge(cmdList, Dx12CommandQueue, InParameters,
                                                    dx12Feature->GetUpscalerType() == Upscaler::DLSSD,
                                                    (unsigned) dx12Feature->GetFeatureFlags(),
                                                    dx12Feature->GetFeatureFlagsKnown(),
                                                    { dx12Feature->Handle() ? dx12Feature->Handle()->Id : 0,
                                                      dx12Feature.get(), Handle() ? Handle()->Id : 0,
                                                      (unsigned)dx12Feature->GetFeatureFlags(), dx12Feature->GetFeatureFlagsKnown(),
                                                      nrSourceRegion.width, nrSourceRegion.height,
                                                      nrSourceRegion.width!=0 && nrSourceRegion.height!=0,
                                                      evaluatedInputMetadata });
        else if ( !native::OpticalFgNrEnabled() && !bridgeFinishedNr)
            DlssNr::EvaluateBeforeUpscale(cmdList, InParameters, Dx12CommandQueue, nrSourceAttempt,
                                      dx12Feature->GetUpscalerType() == Upscaler::DLSSD,
                                      { dx12Feature->Handle() ? dx12Feature->Handle()->Id : 0,
                                        dx12Feature.get(), Handle() ? Handle()->Id : 0,
                                        (unsigned)dx12Feature->GetFeatureFlags(), dx12Feature->GetFeatureFlagsKnown(),
                                        nrSourceRegion.width, nrSourceRegion.height,
                                        nrSourceRegion.width != 0 && nrSourceRegion.height != 0,
                                        evaluatedInputMetadata });
        nrBeforeCpu.Finish();
        CallerCpuTrace::Scope providerEvaluateCpu(CallerCpuTrace::Op::ProviderEvaluate, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
        const auto beforeSrConsumerMatches = [&] {
            // Scratch warmup has a real retirement tail but no copyback to SR.
            if (!beforeSrFilter.EffectsExecuted()) return true;
            if (dx12Feature.get() != beforeSrProvider || Dx12CommandQueue != beforeSrInput.queue ||
                dx12Feature->GetFeatureFlagsKnown() != beforeSrFlagsKnown ||
                dx12Feature->GetFeatureFlags() != beforeSrFlags) return false;
            auto actual = beforeSrInput;
            actual.queue = Dx12CommandQueue;
            actual.stream = reinterpret_cast<uint64_t>(this);
            // Preserve this attempt's frozen source identity. Success counters
            // do not identify the bridge generation or failed Evaluate attempts.
            actual.color = nullptr;
            if (InParameters->Get(NVSDK_NGX_Parameter_Color, &actual.color) != NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_Color, reinterpret_cast<void**>(&actual.color));
            readBeforeSrRegion(actual);
            if (!beforeSrFilter.BeforeSrInputMatches(actual)) return false;
            return true;
        };
        const auto evaluateProvider = [&] {
            // Only the authenticated Genshin transport has acquired its
            // selected recording dependencies. Other routes retain their legacy entry.
            if (!beforeSrConsumerMatches()) {  return false; }
            return HookCpuTiming::Call(HookCpuTiming::Stage::ProviderEvaluate,[&]{return dx12Feature->Evaluate(cmdList, InParameters);},HookCpuTiming::ResultKind::Boolean);
        };
        dx12EvalResult = evaluateProvider();
        providerEvaluateCpu.Finish();
        static bool reportedNrOffer = false;

        if (!reportedNrOffer)
        {
            reportedNrOffer = true;
            LOG_INFO("DLSS-NR: the D3D11 bridge reached the hand-off (upscale ok: {}, enabled: {})",
                     dx12EvalResult, Config::Instance()->DlssNrEnabled.value_or_default());

        }

        CallerCpuTrace::Scope nrAfterCpu(CallerCpuTrace::Op::NrAfter, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
        if ( !native::OpticalFgNrEnabled() && dx12EvalResult && Config::Instance()->DlssNrEnabled.value_or_default())
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
        nrAfterCpu.Finish();

    } while (false);

    }
    catch (...)
    {
        if(commandListRecording)cmdList->Close();
        throw; // Six resource parameters and actual D3D11 state unwind before the SR scope adopts the lease.
    }
    const bool parametersRestored=restoreParameters.RestoreNow();
    if (commandListRecording)
    {
        const auto closeResult = cmdList->Close();
        if (closeResult != S_OK)
        {
            LOG_ERROR("CommandList Close error: {:X}", (UINT) closeResult);
            dx12EvalResult = false;
            }
    }

    if (dx12EvalResult)
    {
        ID3D12CommandList* ppCommandLists[] = { cmdList };
        CallerCpuTrace::Scope executeCpu(CallerCpuTrace::Op::Execute, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame);
        submissionUnfenced = true; // every bridge Execute needs its own successful Signal
        const auto nrSubmitCallbacksBefore = DlssNr::FinishedPictureSubmissionCallbacks();
        GuideCapture::ExecutionReceipt previewReceipt;

            previewReceipt=DlssNr::FinishedPictureSubmitting(Dx12CommandQueue,1,ppCommandLists);
            HookCpuTiming::Call(HookCpuTiming::Stage::Dx12Submit,[&]{return Dx12CommandQueue->ExecuteCommandLists(1, ppCommandLists);},HookCpuTiming::ResultKind::None);commandListExecuted=commandListExecuteReturned=true;

        executeCpu.Finish();
        // Fall back only when this synchronous Execute did not reach the real submit hook.
        // Persistent recording watchers must not mistake duplicate notification for replay.
        if (commandListExecuteReturned && DlssNr::FinishedPictureSubmissionCallbacks() == nrSubmitCallbacksBefore)
            DlssNr::FinishedPictureSubmitted(Dx12CommandQueue, 1, ppCommandLists,previewReceipt);

        if(commandListExecuted){
            const auto fenceValue = ++Dx12FenceValue;
            CallerCpuTrace::Scope signalCpu(CallerCpuTrace::Op::Signal, this, Handle() ? Handle()->Id : 0, _frameCount, commandFrame, fenceValue);
            FramePipeline::GpuWitness featureFence;

                result = HookCpuTiming::Call(HookCpuTiming::Stage::Dx12SignalApi,[&]{return Dx12CommandQueue->Signal(Dx12Fence, fenceValue);},HookCpuTiming::ResultKind::Hresult);

            signalCpu.Finish();
            if (result != S_OK)
            {
                LOG_ERROR("Dx12CommandQueue Signal failed for feature fence {}: {:X}", fenceValue, (UINT) result);
                dx12EvalResult = false;
                }
            else
            {
                Dx12CommandAllocatorFenceValue[commandFrame] = fenceValue;
                submissionUnfenced = false;
                }
        }
    }

    // Execute is irreversible even when its later Signal fails. Such work
    // retains every slot through the existing unfenced/quarantine owner.
    // Only unexecuted lists may roll back CPU resource-state bookkeeping.
    auto evalResult = false;

    do
    {
        if (!dx12EvalResult || !commandListExecuted)
            break;

        if ( !CopyBackOutput())
        {
            LOG_ERROR("Can't copy output texture back!");
            break;
        }

        DlssNr::FinishedPictureDx11BridgeOutputHandoff(Dx12CommandQueue, cmdList);evalResult = true;

    } while (false);

    if (evalResult)
        _frameCount++;
    else Dx11WithDx12::ClearLastPreparedUpscalerFrameState();callerCpuScope.Result(evalResult);
    DlssNr::CompleteGenshinPresentSource(presentSourceAttempt, evaluatedInputMetadata, restoreParamOutput, evalResult);
    if(evalResult)currentInputMetadata=std::move(evaluatedInputMetadata);
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

    // An internal captured source owns a private submission domain. Ordinary
    // native upscaler inputs retain the supplied interop queue, including the
    // existing StarRail schedule. The receipt's real resources are validated
    // separately before Evaluate; a process name grants no ownership.
    try
    {
        if (!runtimeRetention) runtimeRetention = std::make_unique<Dx11BridgeInitRetention>();
        runtimeRetention->device = BridgeWorkDevice();
        runtimeRetention->queue = Dx12CommandQueue;
    }
    catch (...) { return false; }
    if (!CreateD3D12Objects())
    {
        LOG_ERROR("Failed to create D3D12 objects!");
        return false;
    }

    if (!Dx11WithDx12::Init(Dx11Device, Dx11DeviceContext)) return false;

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

    if(runtimeRetention){
        // Shutdown is not completion. Do not let member destruction free the
        // provider/pass uploads while a prior submission lacks a safe witness.
        if(State::Instance().isShuttingDown || !ReleaseSharedResources(true))RetainRuntime();
        return;
    }
    if (State::Instance().isShuttingDown)
        return;

    // This also covers an initialized Genshin feature destroyed before Evaluate,
    // when the runtime retention slot has not yet been allocated.
    ReleaseSharedResources(true);
}
