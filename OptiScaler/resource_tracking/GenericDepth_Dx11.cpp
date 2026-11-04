#include "pch.h"
#include <resource_tracking/DrawObservation_Dx11.h>
#include <ui_mask/DrawUiMask.h>

#include <native/OpticalFgNrPolicy.h>
#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "GenericDepth_Dx11.h"
#include <mods/gimi/GimiRuntime.h>
#include <hooks/D3D11_Hooks.h>
#include <genshin/GenshinOwnHookProvenance.h>
#include <genshin/GenshinHookCpuTiming.h>

#include <genshin/GenshinLegacyCapture.h>
#include <genshin/GenshinPsStateCache.h>
#include <genshin/GenshinDepthMapping.h>
#include <genshin/GenshinReturnedRaster.h>
#include <genshin/GenshinCpuWriterProbe.h>

#include <Config.h>
#include <Util.h>
#include <dlssnr/DlssNr_NativeMode.h>

#include <native/DepthCopyDx11.h>
#include <native/DepthFinderCore.h>
#include <native/NativeLowLatency.h>

#include <detours/detours.h>

#include <imgui/imgui.h>
#include <menu/SettingsSwitchRow.h>

#include <d3d11_4.h>
#include <TlHelp32.h>
#include <intrin.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <format>
#include <string>
#include <cstring>

// See GenericDepth_Dx11.h. Reuses native::DepthFinderCore (resource_tracking/GenericDepth_Dx12.cpp is the D3D12 sibling of this
// file; both observers feed the same core).

namespace
{

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D11DeviceContext* This, UINT NumViews,
                                                        ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                        ID3D11DepthStencilView* pDepthStencilView);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargetsAndUnorderedAccessViews)(
    ID3D11DeviceContext* This, UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts);
typedef void(STDMETHODCALLTYPE* PFN_ClearDepthStencilView)(ID3D11DeviceContext* This, ID3D11DepthStencilView* pView,
                                                           UINT ClearFlags, FLOAT Depth, UINT8 Stencil);
typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D11DeviceContext* This, UINT NumViewports,
                                                    const D3D11_VIEWPORT* pViewports);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation,
                                                 INT BaseVertexLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext* This, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext* This, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawAuto)(ID3D11DeviceContext* This);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndirect)(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                                  UINT AlignedByteOffsetForArgs);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext* This, UINT X, UINT Y, UINT Z);
typedef void(STDMETHODCALLTYPE* PFN_SetConstantBuffers)(ID3D11DeviceContext* This, UINT StartSlot,
                                                         UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers);

PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
PFN_OMSetRenderTargetsAndUnorderedAccessViews o_OMSetRenderTargetsAndUAV = nullptr;
PFN_ClearDepthStencilView o_ClearDepthStencilView = nullptr;
PFN_RSSetViewports o_RSSetViewports = nullptr;
using PFN_ClearRtv=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT*);
using PFN_ClearUavUint=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11UnorderedAccessView*,const UINT*);
using PFN_ClearUavFloat=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11UnorderedAccessView*,const FLOAT*);
PFN_ClearRtv o_ClearRtv=nullptr;PFN_ClearUavUint o_ClearUavUint=nullptr;PFN_ClearUavFloat o_ClearUavFloat=nullptr;
using PFN_CreateDepthCs=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11ComputeShader**);
PFN_CreateDepthCs o_CreateDepthCs=nullptr;
using PFN_CopyStructure=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Buffer*,UINT,ID3D11UnorderedAccessView*);
PFN_CopyStructure o_DepthCopyStructure=nullptr;
using PFN_CopyResource=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
using PFN_CopyRegion=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
using PFN_Update=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
using PFN_Map=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
using PFN_Resolve=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,ID3D11Resource*,UINT,DXGI_FORMAT);
using PFN_Mips=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11ShaderResourceView*);
using PFN_CopyRegion1=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*,UINT);
using PFN_Update1=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT,UINT);
using PFN_ClearView=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11View*,const FLOAT*,const D3D11_RECT*,UINT);
PFN_CopyRegion1 o_DepthCopyRegion1=nullptr;PFN_Update1 o_DepthUpdate1=nullptr;PFN_ClearView o_DepthClearView=nullptr;
PFN_DrawIndirect o_DepthDispatchIndirect=nullptr;
PFN_CopyResource o_DepthCopyResource=nullptr;PFN_CopyRegion o_DepthCopyRegion=nullptr;PFN_Update o_DepthUpdate=nullptr;
PFN_Map o_DepthMap=nullptr;PFN_Unmap o_DepthUnmap=nullptr;PFN_Resolve o_DepthResolve=nullptr;PFN_Mips o_DepthMips=nullptr;
PFN_Draw o_Draw = nullptr;
PFN_DrawIndexed o_DrawIndexed = nullptr;
PFN_DrawInstanced o_DrawInstanced = nullptr;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
PFN_DrawAuto o_DrawAuto = nullptr;
PFN_DrawIndirect o_DrawIndexedInstancedIndirect = nullptr;
PFN_DrawIndirect o_DrawInstancedIndirect = nullptr;
PFN_Dispatch o_Dispatch = nullptr;
PFN_SetConstantBuffers o_VSSetConstantBuffers = nullptr;
PFN_SetConstantBuffers o_PSSetConstantBuffers = nullptr;
PFN_SetConstantBuffers o_CSSetConstantBuffers = nullptr;
std::atomic_bool g_modWriteHooksComplete{false};
using PFN_ModSOSetTargets = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11Buffer* const*, const UINT*);
PFN_ModSOSetTargets o_ModSOSetTargets = nullptr;
// Optional SO owns a separate transaction AFTER the mandatory context hooks.
// A rejected, soundly rolled-back SO install cannot abort/revoke that core.
// Entries stay immutable after commit; never overwrite a live original.
GenshinNativeQuery::Own::Slot g_modSOOwner;
std::atomic_bool g_modSOHookCommitted{false};
PVOID g_modSOHookTarget = nullptr;
std::array<PVOID,3> g_modSOBoundaryTargets{}; // Execute, Clear, Finish
PVOID g_modSOSwapTarget = nullptr;
ID3D11DeviceContext* g_modSOContext = nullptr; // identity only; never dereferenced
ID3D11Device* g_modSODevice = nullptr; // immutable committed runtime identity
bool g_modSONativeApplication = false;
bool ModStreamHooksCover(ID3D11DeviceContext*) noexcept;
bool TrackingInternal();
void STDMETHODCALLTYPE hkModSOSetTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11Buffer* const* buffers, const UINT* offsets)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkModSOSetTargets);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    r448Hook.Original([&]{return o_ModSOSetTargets(context,count,buffers,offsets);});
    if(gimi::runtime::Enabled() && !TrackingInternal())
        gimi::runtime::StreamOutputTargets(context,count,buffers,offsets);
}
using PFN_QueryRange = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
PFN_QueryRange o_QueryBegin = nullptr, o_QueryEnd = nullptr;
PFN_QueryRange o_StateQueryBegin=nullptr,o_StateQueryEnd=nullptr;
uintptr_t g_stateBeginReturn=0,g_stateEndReturn=0;
std::atomic_flag g_queryInstallBusy = ATOMIC_FLAG_INIT;
class QueryInstallAccess final {
    bool acquired_=!g_queryInstallBusy.test_and_set();
public:
    ~QueryInstallAccess(){if(acquired_)g_queryInstallBusy.clear();}
    explicit operator bool()const noexcept {return acquired_;}
    QueryInstallAccess()=default;
    QueryInstallAccess(const QueryInstallAccess&)=delete;
};
using PFN_ClearState = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN_ExecuteCommandList = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_FinishCommandList = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PFN_DiscardResource = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*);
using PFN_DiscardView = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*);
using PFN_DiscardView1 = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*, const D3D11_RECT*, UINT);
using PFN_SwapDeviceContextState = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**);
PFN_ClearState o_ClearState = nullptr;
PFN_ExecuteCommandList o_ExecuteCommandList = nullptr;
PFN_FinishCommandList o_FinishCommandList = nullptr;
PFN_DiscardResource o_DiscardResource = nullptr;
PFN_DiscardView o_DiscardView = nullptr;
PFN_DiscardView1 o_DiscardView1 = nullptr;
PFN_SwapDeviceContextState o_SwapDeviceContextState = nullptr;
using PFN_CreateContextState = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device1*,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,REFIID,D3D_FEATURE_LEVEL*,ID3DDeviceContextState**);
PFN_CreateContextState o_CreateContextState = nullptr;
PFN_ExecuteCommandList o_StateExecute=nullptr;PFN_FinishCommandList o_StateFinish=nullptr;
PFN_ClearState o_StateClear=nullptr;

// ID3D11DeviceContext vtable indices (checked against the SDK header with offsetof; see tests/ -- a host-only sanity program,
// not shipped): DrawIndexed 12, Draw 13, DrawIndexedInstanced 20, DrawInstanced 21, OMSetRenderTargets 33,
// OMSetRenderTargetsAndUnorderedAccessViews 34, SOSetTargets 37, DrawAuto 38, DrawIndexedInstancedIndirect 39, DrawInstancedIndirect 40,
// Dispatch 41, RSSetViewports 44, ClearDepthStencilView 53. The constant-buffer
// setter indices below are pinned to the same Windows SDK ID3D11DeviceContext
// declaration: VS 7, PS 16 and CS 71.
constexpr int kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21, kOMSetRenderTargets = 33,
             kOMSetRenderTargetsAndUAV = 34, kSOSetTargets = 37, kDrawAuto = 38, kDrawIndexedInstancedIndirect = 39,
             kDrawInstancedIndirect = 40, kDispatch = 41, kRSSetViewports = 44, kClearDepthStencilView = 53,
             kVSSetConstantBuffers = 7, kPSSetConstantBuffers = 16, kCSSetConstantBuffers = 71;

// These process-wide function targets are immutable after the first common
// installation. A later bridge upgrade must use the same complete hook set.
constexpr std::array<int,12> kCommonHookSlots {kDrawIndexed,kDraw,kDrawIndexedInstanced,kDrawInstanced,
    kOMSetRenderTargets,kOMSetRenderTargetsAndUAV,kDrawAuto,kDrawIndexedInstancedIndirect,
    kDrawInstancedIndirect,kDispatch,kRSSetViewports,kClearDepthStencilView};
std::array<PVOID,kCommonHookSlots.size()> g_commonHookTargets {};

std::mutex g_mutex;
native::DepthFinderCore g_core;

// The context the game draws its frames through: its device's immediate context, unwrapped from a wrapping layer's proxy
// (Streamline's), a reference held. The hooks patch the functions themselves, so every context of the same kind runs them (a
// deferred context under the debug layer, another device's immediate context): only this one's calls are counted. Followed to
// the device the game presents with (FollowPresentDevice), so a device the game makes again is watched too. Set at Install and
// on the present thread only.
ID3D11DeviceContext* g_context = nullptr;
std::atomic<uint64_t> g_contextId = 0; // g_context, as the core's id for it; read by the hooks on the game's threads
PVOID g_hookedOMSetRenderTargets = nullptr; // the function hooked, to tell whether a context runs the hooks at all
// The watched context set render targets since the last present (it is still the one the game draws through).
std::atomic<bool> g_watchedBound = false;

// The device of g_context, and the real device behind it when that is a proxy; a buffer of either can be copied on g_context.
// Not held (g_context holds its device). Under g_mutex.
ID3D11Device* g_contextDevice = nullptr;
ID3D11Device* g_contextRealDevice = nullptr;

// The copy of the picked buffer for the frame: one context means one copy, unlike the D3D12 finder's several command lists.
// Under g_mutex. Let go at stand-down and when the context changes. Never destroyed (no COM release at process exit).
native::DepthCopyDx11& g_copy = *new native::DepthCopyDx11();

// The finder's own pass is being recorded on this thread: its Dispatch is not the game's.
thread_local bool t_copying = false;
thread_local bool t_genshinCapture = false;
bool TrackingInternal()
{
    return t_copying || t_genshinCapture || DrawObservationDx11::Internal() || GenshinLegacy::IsInternalCallActive();
}
// Drain only immutable published diagnostics; no COM call, query-state
// creation, native validation, GPU work or extra state lock is performed.
void LogGenshinQueryHealth()
{
    static std::atomic<ULONGLONG> nextCostLog{0};const auto now=GetTickCount64();auto next=nextCostLog.load();
    if(now>=next && nextCostLog.compare_exchange_strong(next,now+5000)){
        static const auto frequency=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return f.QuadPart;}();
        const auto retention=GenshinDepthRetention::ReadTelemetry();
        LOG_INFO("R829 depth retention: pid={} buffer_bytes={} payload_bytes={} resources={} payloads={} refused={} peak_total_bytes={} releases={} buffer_budget={} payload_budget={} not_total_gpu_memory=true",
            GetCurrentProcessId(),retention.bytes,retention.payloadBytes,retention.resources,retention.payloads,
            retention.refused,retention.peak,retention.releases,GenshinDepthRetention::BufferBudget,GenshinDepthRetention::PayloadBudget);
        const auto source=GenshinDepthRetention::ReadSourceTelemetry();
        const auto refusal=GenshinDepthRetention::ReadRefusalTelemetry();
        LOG_INFO("R1064 source residency: pid={} source_bytes={} ordinary_source_bytes={} source_allocations={} source_refused={} source_budget={} source_reserve={} source_evictions={} source_record_refused={} source_epoch={} retention_contention={} retention_capacity={} retention_bytes={} retention_conflict={} retention_allocation={} source_separate_from_frame_payload=true no_authority=true",
            GetCurrentProcessId(),source.bytes,source.ordinaryBytes,source.allocations,source.refused,
            GenshinDepthRetention::SourceBudget,GenshinDepthRetention::SourceReserve,GenshinDepthPortable::evictions.load(),GenshinDepthPortable::recordRefused.load(),GenshinDepthPortable::epoch.load(),
            refusal.contention,refusal.capacity,refusal.bytes,refusal.conflict,refusal.allocation);
        const auto& mapping=GenshinDepthMapping::requestCost;
        LOG_INFO("R827 phase22 mapping requests: pid={} immediate={} no_work={} admission={} refused={} no_authority=true",
            GetCurrentProcessId(),mapping.immediate.load(),mapping.noWork.load(),mapping.admission.load(),mapping.refused.load());
        for(unsigned i=0;i<GenshinCpuStageTiming::Count;++i){
            const auto stage=static_cast<GenshinCpuStageTiming::Stage>(i);const auto sample=GenshinCpuStageTiming::Read(stage);
            LOG_INFO("R833 CPU stage: pid={} stage={} samples={} sampled_inclusive_wall_ms={} stride={} sampled_only=true not_gpu_time=true no_authority=true cpu_samples={} scheduled_cpu_ms={} thread_cycles={} cpu_failures={} nested=true cpu_time_units_100ns=true cpu_quantization=platform_dependent name={} sampled_max_wall_ms={} host_max_and_calls_recorded={} calls_lower_bound={} cumulative=true inclusive_not_additive=true wall_not_cpu_busy=true",
                GetCurrentProcessId(),i,sample.samples,frequency>0?1000.0*static_cast<double>(sample.ticks)/static_cast<double>(frequency):0.0,GenshinCpuStageTiming::SampleStride(stage),
                sample.cpuSamples,static_cast<double>(sample.cpu100ns)/10000.0,sample.cycles,sample.cpuFailures,
                GenshinCpuStageTiming::Name(stage),frequency>0?1000.0*static_cast<double>(sample.maxTicks)/static_cast<double>(frequency):0.0,
                GenshinCpuStageTiming::Host(stage),sample.calls);
        }
        const auto& cost=GenshinQueryScope::workCost;
        LOG_INFO("Genshin query work cost: pid={} probes={} early_refused={} native_checks={} validation_scope_wall_ms={} native_integrity_wall_ms={} hook_targets_wall_ms={} synchronous_scope_only=true not_gpu_time=true",
                 GetCurrentProcessId(),cost.probes.load(),cost.earlyRefused.load(),cost.validationCalls.load(),
                 frequency>0?1000.0*static_cast<double>(cost.validationTicks.load())/static_cast<double>(frequency):0.0,
                 frequency>0?1000.0*static_cast<double>(cost.nativeTicks.load())/static_cast<double>(frequency):0.0,
                 frequency>0?1000.0*static_cast<double>(cost.targetTicks.load())/static_cast<double>(frequency):0.0);
        // R698: retain fixed first receipts in rolling-log tails. Read only
        // published immutable scalars here; never repeat the one-shot COM,
        // native-ledger, caller/module or context-table observations below.
        const auto& firstLoss=GenshinQueryScope::firstCoverageLoss;
        if(firstLoss.ready.load(std::memory_order_acquire)==2)
            LOG_INFO("R698 query first coverage retained: pid={} status=RECORDED tid={} object={:X} reason={} first_frame_status=UNKNOWN observation_only=true",
                GetCurrentProcessId(),firstLoss.thread,firstLoss.object,firstLoss.reason);
        else
            LOG_INFO("R698 query first coverage retained: pid={} status=UNKNOWN first_frame_status=UNKNOWN observation_only=true",GetCurrentProcessId());
        bool anyFirstRefusal=false;
        for(unsigned i=0;i<GenshinQueryScope::workRefusals.size();++i){const auto& receipt=GenshinQueryScope::workRefusals[i];
            if(receipt.ready.load(std::memory_order_acquire)!=2)continue;
            anyFirstRefusal=true;
            LOG_INFO("R698 query first refusal retained: pid={} kind={} tid={} context={:X} refusal_tick={} first_state_tick={} factory_attach_tick={} context_hooks_tick={} first_frame_status=UNKNOWN certificate={} epoch={} active={} active_type_mask={:X} complete={} history={} coverage_lost={} hooks={} range_reason={} native_reason={} birth_status={} receipt_scope=FIRST_PER_KIND_NOT_CURRENT unpublished_kinds=UNKNOWN observation_only=true",
                GetCurrentProcessId(),i,receipt.thread,receipt.context,receipt.refusalTick,receipt.firstObservedTick,
                receipt.attachTick,receipt.hookCommitTick,receipt.certificate,receipt.epoch,
                receipt.active,receipt.activeTypes,receipt.complete,receipt.history,receipt.coverage,receipt.hooks,
                receipt.rangeReason,receipt.nativeReason,receipt.birthObserved?"RECORDED":"UNKNOWN");
            if(receipt.birthObserved)
                LOG_INFO("R698 query first birth retained: pid={} kind={} context={:X} refusal_tick={} first_state_tick={} factory_attach_tick={} context_hooks_tick={} birth_kind={} birth_entry_tick={} birth_return_tick={} birth_tid={} birth_device={:X} birth_context={:X} birth_caller={:X} birth_executed={:X} birth_attach_tick={} birth_hooks_tick={} observation_only=true no_birth_authority=true",
                    GetCurrentProcessId(),i,receipt.context,receipt.refusalTick,receipt.firstObservedTick,receipt.attachTick,
                    receipt.hookCommitTick,static_cast<unsigned>(receipt.birth.kind),receipt.birth.entryTick,
                    receipt.birth.returnTick,receipt.birth.thread,receipt.birth.device,receipt.birth.context,
                    receipt.birth.caller,receipt.birth.executed,receipt.birth.factoryAttach,receipt.birth.contextHookCommit);
        }
        if(!anyFirstRefusal)
            LOG_INFO("R698 query first refusal retained: pid={} status=UNKNOWN first_frame_status=UNKNOWN observation_only=true",GetCurrentProcessId());
        const auto nativeLatency=GenshinQueryScope::ReadLatency(cost.nativeLatencyUs);
        const auto validationLatency=GenshinQueryScope::ReadLatency(cost.validationLatencyUs);
        const auto& rw=GenshinCpuStageTiming::readWork;
        LOG_INFO("R835 native read work: pid={} samples={} queries={} snapshots={} live_bytes={} proof_bytes={} stride=64 successful_sampled_admissions_only=true no_authority=true",GetCurrentProcessId(),rw.samples.load(),rw.queries.load(),rw.snapshots.load(),rw.liveBytes.load(),rw.proofBytes.load());
        LOG_INFO("R835 native latency histogram: pid={} native_samples={} native_p50_us={} native_p95_us={} native_p99_us={} native_overflow={} validation_samples={} validation_p50_us={} validation_p95_us={} validation_p99_us={} validation_overflow={} bin_width_us=1 cumulative=true not_gpu_time=true",
            GetCurrentProcessId(),nativeLatency.samples,nativeLatency.p50Us,nativeLatency.p95Us,nativeLatency.p99Us,nativeLatency.overflow,
            validationLatency.samples,validationLatency.p50Us,validationLatency.p95Us,validationLatency.p99Us,validationLatency.overflow);
    }
    // R791: only after a real sticky coverage loss, inspect immutable/native
    // receipts without retrying authentication, adding images or changing any
    // ledger field. A later mismatch is an observation, not the original cause.
    static std::atomic<bool> nativeLogged{false};
    if(GenshinQueryScope::firstCoverageLoss.ready.load()==2 && !nativeLogged.load()){
        namespace d=GenshinNativeQuery::detail;
        struct BadPatch {unsigned index=0;uintptr_t entry=0;bool original=false,executed=false,mirror=false,entryBytes=false,trampoline=false,hook=false;};
        struct BadImport {unsigned index=0;uintptr_t slot=0,expected=0,actual=0,actualBase=0;const char* dll="none";const char* name="ordinal";unsigned ordinal=0,kind=0;bool read=false,lazy=false;};
        std::array<BadPatch,8> patches{};std::array<BadImport,8> imports{};
        unsigned badPatches=0,badImports=0,patchCount=0,importCount=0;bool read=false,revoked=false;uint64_t generation=0;
        {
            d::Guard guard;
            if(guard.held && !nativeLogged.exchange(true)){
                read=true;revoked=d::Revoked();generation=d::store.generation.load();const auto& ledger=d::Ledger();
                patchCount=ledger.patchCount;importCount=ledger.importCount;
                for(unsigned i=0;i<ledger.patchCount;++i){const auto& patch=ledger.patches[i];
                    if(d::PatchIntact(patch))continue;
                    if(badPatches<patches.size()){auto& out=patches[badPatches];out.index=i;out.entry=reinterpret_cast<uintptr_t>(patch.entry);void* value=nullptr;
                        out.original=d::Memory(patch.original,&value,sizeof(value)) && value==patch.trampoline;
                        out.executed=!patch.executed || (d::Memory(patch.executed,&value,sizeof(value)) && value==patch.trampoline);
                        out.mirror=!patch.mirror || (d::Memory(patch.mirror,&value,sizeof(value)) && value==patch.trampoline);
                        out.entryBytes=d::Equal(patch.entry,patch.edit.data(),patch.size);out.trampoline=d::Equal(patch.trampoline,patch.code.data(),patch.code.size());
                        out.hook=d::Equal(patch.hook,patch.ownEntry.data(),patch.ownEntry.size());}
                    ++badPatches;
                }
                for(unsigned i=0;i<ledger.importCount;++i){const auto& item=ledger.imports[i];uintptr_t value=0;const bool got=d::Memory(item.slot,&value,sizeof(value));
                    if(got && value==item.target)continue;
                    if(badImports<imports.size()){auto& out=imports[badImports];out.index=i;out.slot=reinterpret_cast<uintptr_t>(item.slot);out.expected=item.target;out.actual=value;
                        out.dll=item.dll?item.dll:"none";out.name=item.name?item.name:"ordinal";out.ordinal=item.ordinal;out.kind=static_cast<unsigned>(item.binding.kind);out.read=got;out.lazy=item.lazy;
                        MEMORY_BASIC_INFORMATION info{};if(value && VirtualQuery(reinterpret_cast<void*>(value),&info,sizeof(info))==sizeof(info) && info.Type==MEM_IMAGE)out.actualBase=reinterpret_cast<uintptr_t>(info.AllocationBase);}
                    ++badImports;
                }
            }
        }
        if(read){
            LOG_WARN("Genshin live native receipt observation: pid={} revoked={} generation={} patches={} bad_patches={} imports={} bad_imports={} post_refusal_only=true no_authority=true",
                     GetCurrentProcessId(),revoked,generation,patchCount,badPatches,importCount,badImports);
            if(d::firstRevocationState.load(std::memory_order_acquire)==2){const auto& first=d::firstRevocation;
                LOG_WARN("Genshin live native first revocation: pid={} tid={} file={} line={} function={} observation_only=true",GetCurrentProcessId(),first.thread,first.file,first.line,first.function);}
            if(const auto* first=GenshinNativeQuery::FirstImportFailure())
                LOG_WARN("Genshin live import first failure: pid={} tid={} stage={} dll={} symbol={} importer={} slot={:X} expected={:X} actual={:X} resolver_dll={} resolver_symbol={} resolver_actual={:X} expected_known={} actual_known={} resolver_known={} observation_only=true",
                         GetCurrentProcessId(),first->thread,first->stage,first->dll.data(),first->symbol.data(),first->importerName.data(),reinterpret_cast<uintptr_t>(first->slot),first->expected,first->actual.value,first->resolverDll.data(),first->resolverSymbol.data(),first->resolverActual.value,first->expectedKnown,first->actualKnown,first->resolverActualKnown);
            for(unsigned i=0;i<(std::min)(badPatches,static_cast<unsigned>(patches.size()));++i){const auto& out=patches[i];
                LOG_WARN("Genshin live native bad patch: pid={} index={} entry={:X} original={} executed={} mirror={} entry_bytes={} trampoline={} hook={} observation_only=true",
                         GetCurrentProcessId(),out.index,out.entry,out.original,out.executed,out.mirror,out.entryBytes,out.trampoline,out.hook);}
            for(unsigned i=0;i<(std::min)(badImports,static_cast<unsigned>(imports.size()));++i){const auto& out=imports[i];
                LOG_WARN("Genshin live native bad import: pid={} index={} slot={:X} expected={:X} actual={:X} actual_base={:X} dll={} symbol={} ordinal={} kind={} read={} lazy={} observation_only=true",
                         GetCurrentProcessId(),out.index,out.slot,out.expected,out.actual,out.actualBase,out.dll,out.name,out.ordinal,out.kind,out.read,out.lazy);}
        }
    }
    // R1024 followup3: drain the ORIGINAL immutable context refusal, including
    // failures after factory birth. No context re-read or admission retry.
    static std::atomic<bool> contextFailureLogged{false};
    namespace native=GenshinNativeQuery::detail;
    if(native::firstContextFailureState.load(std::memory_order_acquire)==2 && !contextFailureLogged.exchange(true)){
        const auto& f=native::firstContextFailure;
        const auto leaf=[&](const char* role,const native::ContextTableLeaf& value){
            LOG_WARN("R1024 first context refusal: pid={} thread={} object={:X} table={:X} factory={:X} generation={} slots={} role={} scope={} predicate={} status_kind={} status={} slot={} mode0={} mode1={} rva={:X} offset={} span_offset={} actual0={:X} expected0={:X} actual1={:X} expected1={:X} read_bytes={} requested_bytes={} binding_shape={} execution_only={} observation_only=true no_authority=true",
                GetCurrentProcessId(),f.thread,f.object,f.table,f.factory,f.generation,f.slots,role,value.scope,value.predicate,value.statusKind,value.status,value.slot,value.modeMismatch[0],value.modeMismatch[1],value.rva,value.offset,value.spanOffset,
                value.actual[0],value.expected[0],value.actual[1],value.expected[1],value.readBytes,value.requestedBytes,f.bindingShape,f.executionOnly);
        };
        leaf("native-table",f.tableLeaf);leaf("constructor",f.inlineLeaf);
        leaf("state-swap",f.swapLeaf);leaf("binding-shape",f.bindingLeaf);
    }
    static std::atomic<bool> lossLogged{false};
    const auto& loss=GenshinQueryScope::firstCoverageLoss;
    if(loss.ready.load()==2 && !lossLogged.exchange(true))
        LOG_WARN("Genshin query coverage first loss: pid={} tid={} reason={} object={:X} observation_only=true",
                 GetCurrentProcessId(),loss.thread,loss.reason,loss.object);
    static std::array<std::atomic<bool>,static_cast<unsigned>(GenshinQueryScope::RefusalKind::Count)> logged{};
    for(unsigned i=0;i<logged.size();++i){const auto& receipt=GenshinQueryScope::workRefusals[i];
        if(receipt.ready.load()!=2 || logged[i].exchange(true))continue;
        LOG_INFO("R587 query first refusal order: pid={} tid={} kind={} context={:X} refusal_tick={} first_state_tick={} factory_attach_tick={} context_hooks_tick={} birth_observed={} birth_kind={} birth_entry_tick={} birth_return_tick={} birth_tid={} birth_device={:X} birth_context={:X} birth_caller={:X} birth_executed={:X} birth_attach_tick={} birth_hooks_tick={} certificate={} observation_only=true no_birth_authority=true",
            GetCurrentProcessId(),receipt.thread,i,receipt.context,receipt.refusalTick,receipt.firstObservedTick,
            receipt.attachTick,receipt.hookCommitTick,receipt.birthObserved,static_cast<unsigned>(receipt.birth.kind),
            receipt.birth.entryTick,receipt.birth.returnTick,receipt.birth.thread,receipt.birth.device,receipt.birth.context,
            receipt.birth.caller,receipt.birth.executed,receipt.birth.factoryAttach,receipt.birth.contextHookCommit,receipt.certificate);
        // Post-refusal observation only: do not retry native admission or
        // treat a sampled table as an authenticated constructor recipe.
        static bool contextObserved=false;
        if(!contextObserved && receipt.context && receipt.coverage){
            namespace d=GenshinNativeQuery::detail;namespace cr=GenshinNativeQuery::context_recipe;
            d::Guard guard;
            if(guard.held){contextObserved=true;const auto* object=reinterpret_cast<const void*>(receipt.context);
                void* table=nullptr;const bool got=d::Memory(object,&table,sizeof(table));
                std::array<uintptr_t,cr::Count> actual{};const bool read=got && d::Memory(table,actual.data(),sizeof(actual));
                const auto& ledger=d::Ledger();const auto* image=ledger.imageCount?&ledger.images[0]:nullptr;
                LOG_WARN("R815 context recipe observation: pid={} object={:X} table={:X} read={} private_inline={} slots={} factory={:X} copy_recipe={} patch_recipe={} post_refusal_only=true no_authority=true",
                    GetCurrentProcessId(),receipt.context,reinterpret_cast<uintptr_t>(table),read,got && cr::PrivateInline(object,table),cr::Count,
                    image?reinterpret_cast<uintptr_t>(image->live):0,
                    image && cr::RecipeRange(*image,0x11770,cr::CopyRecipe.data(),cr::CopyRecipe.size()),
                    image && cr::RecipeRange(*image,0x11c31,cr::PatchRecipe.data(),cr::PatchRecipe.size()));
                if(read && image && image->live)for(unsigned mode=0;mode<2;++mode){
                    const auto rva=cr::TableRva+mode*cr::Stride;unsigned differences=0;
                    const bool source=image->SectionHas(rva,sizeof(actual),IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_DISCARDABLE);
                    if(source)for(unsigned slot=0;slot<cr::Count;++slot){
                        auto expected=d::Read<uintptr_t>(image->clean.data()+rva+slot*sizeof(uintptr_t));
                        if(slot==111)expected=reinterpret_cast<uintptr_t>(image->live)+0xfd1e0;
                        if(slot==144)expected=reinterpret_cast<uintptr_t>(image->live)+0x1b1780;
                        if(actual[slot]==expected)continue;++differences;
                        if(differences>16)continue;MEMORY_BASIC_INFORMATION memory{};uintptr_t base=0;std::array<char,128> name{};
                        if(actual[slot] && VirtualQuery(reinterpret_cast<const void*>(actual[slot]),&memory,sizeof(memory))==sizeof(memory) && memory.Type==MEM_IMAGE){
                            base=reinterpret_cast<uintptr_t>(memory.AllocationBase);d::ImportModuleName(static_cast<HMODULE>(memory.AllocationBase),name);}
                        LOG_WARN("R815 context recipe difference: pid={} mode={} slot={} actual={:X} expected={:X} actual_module={} actual_rva={:X} expected_rva={:X} post_refusal_only=true no_authority=true",
                            GetCurrentProcessId(),mode,slot,actual[slot],expected,name.data(),base?actual[slot]-base:0,expected-reinterpret_cast<uintptr_t>(image->live));
                    }
                    LOG_WARN("R815 context recipe summary: pid={} mode={} source={} source_live_equal={} differences={} post_refusal_only=true no_authority=true",
                        GetCurrentProcessId(),mode,source,source && d::Equal(reinterpret_cast<const unsigned char*>(image->live)+rva,image->clean.data()+rva,sizeof(actual)),differences);
                }
            }
        }
        MEMORY_BASIC_INFORMATION callerInfo{};uintptr_t callerBase=0;std::array<char,128> callerName{};
        if(receipt.activeFirstCaller && VirtualQuery(reinterpret_cast<void*>(receipt.activeFirstCaller),&callerInfo,sizeof(callerInfo))==sizeof(callerInfo) && callerInfo.Type==MEM_IMAGE){
            callerBase=reinterpret_cast<uintptr_t>(callerInfo.AllocationBase);
            GenshinNativeQuery::detail::ImportModuleName(static_cast<HMODULE>(callerInfo.AllocationBase),callerName);
        }
        LOG_WARN("Genshin query caller observation: pid={} kind={} caller={:X} image={} base={:X} rva={:X} no_authority=true",
                 GetCurrentProcessId(),i,receipt.activeFirstCaller,callerName.data(),callerBase,callerBase?receipt.activeFirstCaller-callerBase:0);
        LOG_WARN("Genshin query work refusal: pid={} kind={} tid={} context={:X} certificate={} epoch={} active={} active_type_mask={:X} complete={} history={} coverage_lost={} hooks={} range_reason={} native_reason={} observation_only=true",
                 GetCurrentProcessId(),i,receipt.thread,receipt.context,receipt.certificate,receipt.epoch,
                 receipt.active,receipt.activeTypes,receipt.complete,receipt.history,receipt.coverage,receipt.hooks,receipt.rangeReason,receipt.nativeReason);
    }
}
// R805 bounded passive observations: no GPU work, query-state creation,
// semantic/cache mutation, native validation or admission decision.
uintptr_t ObservedComIdentity(IUnknown* object)noexcept {
    Microsoft::WRL::ComPtr<IUnknown> id;if(object)object->QueryInterface(IID_PPV_ARGS(&id));
    return reinterpret_cast<uintptr_t>(id.Get());
}
void ObserveSelectedTransfer(ID3D11DeviceContext* c,ID3D11Resource* dst,ID3D11Resource* src,
    const char* operation,UINT dsub=UINT_MAX,UINT ssub=UINT_MAX,const D3D11_BOX* box=nullptr)noexcept {
    if(TrackingInternal())return;const auto& e=GenshinDepthEvidence::Expected();
    if(!e.resource || e.context.Get()!=c || e.thread!=GetCurrentThreadId())return;
    static unsigned attempts=0,count=0;if(count>=32 || attempts>=4096)return;++attempts;
    const auto selected=ObservedComIdentity(e.resource.Get()),d=ObservedComIdentity(dst),source=ObservedComIdentity(src);
    if(!selected || (selected!=d && selected!=source))return;
    ++count;
    LOG_INFO("R805 selected transfer: pid={} op={} generation={} frame={} selected={} dst={} src={} dst_selected={} src_selected={} dst_interface={} src_interface={} selected_interface={} dsub={} ssub={} has_box={} command_returned=true gpu_completion=UNKNOWN no_authority=true",
        GetCurrentProcessId(),operation,e.generation,e.frame,selected,d,source,d==selected,source==selected,
        reinterpret_cast<uintptr_t>(dst),reinterpret_cast<uintptr_t>(src),reinterpret_cast<uintptr_t>(e.resource.Get()),dsub,ssub,box!=nullptr);
}
// Diagnostic only: immutable scalar snapshot, no Access/Lose/Owner or GPU work.
void ObserveSelectedCopyTracks(ID3D11DeviceContext* c,ID3D11Resource* dst,ID3D11Resource* src)noexcept {
    if(TrackingInternal())return;const auto& expected=GenshinDepthEvidence::Expected();
    if(!src || !dst || expected.context.Get()!=c || expected.resource.Get()!=dst || expected.thread!=GetCurrentThreadId())return;
    static thread_local uintptr_t previousSource=0,previousDestination=0;
    static thread_local unsigned reports=0;
    const auto source=reinterpret_cast<uintptr_t>(src),destination=reinterpret_cast<uintptr_t>(dst);
    if(reports>=32 || (source==previousSource && destination==previousDestination))return;
    struct Row {uintptr_t resource=0;uint64_t version=0,clear=0,returned=0,writers=0,loss=0;unsigned hops=0,mip=0;bool cleared=false;};
    std::array<Row,4> rows{};uint64_t serial=0,ownerLoss=0;int pending=-1;bool pendingClear=false;
    {
        auto& state=GenshinReturnedRaster::detail::Store();std::unique_lock lock(state.mutex,std::try_to_lock);
        if(!lock.owns_lock())return;
        if(state.context.Get()!=c || state.thread!=GetCurrentThreadId())return;
        serial=state.serial;ownerLoss=state.loss;pending=state.pendingSlot;pendingClear=state.clear.serial!=0;
        for(unsigned i=0;i<rows.size();++i){const auto& in=state.tracks[i].interval;auto& out=rows[i];
            out={reinterpret_cast<uintptr_t>(in.resource.Get()),in.version,in.clearSerial,in.returnedSerial,in.returnedDraws,in.loss,
                in.copyHops,in.clearDesc.Texture2D.MipSlice,in.clearReturned};}
    }
    previousSource=source;previousDestination=destination;++reports;
    try {
        LOG_INFO("R827 phase34 selected copy: pid={} source={} destination={} serial={} owner_loss={} actual_loss={} pending_slot={} pending_clear={} observation_only=true",
            GetCurrentProcessId(),source,destination,serial,ownerLoss,GenshinReturnedRaster::detail::loss.load(),pending,pendingClear);
        for(unsigned i=0;i<rows.size();++i){const auto& row=rows[i];
            LOG_INFO("R827 phase34 copy track: pid={} source={} slot={} resource={} source_match={} version={} clear={} returned={} writers={} loss={} hops={} mip={} clear_returned={} observation_only=true",
                GetCurrentProcessId(),source,i,row.resource,row.resource==source,row.version,row.clear,row.returned,row.writers,row.loss,row.hops,row.mip,row.cleared);}
    }catch(...){}
}
void ObserveSelectedDsv(ID3D11DeviceContext* c,ID3D11DepthStencilView* view,const char* stage)noexcept {
    if(TrackingInternal() || !view)return;const auto& e=GenshinDepthEvidence::Expected();
    if(!e.resource || e.context.Get()!=c || e.thread!=GetCurrentThreadId())return;
    static unsigned attempts=0,count=0;if(count>=24 || attempts>=4096)return;++attempts;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,expected;e.resource.As(&expected);if(resource)resource.As(&texture);
    if(!texture || !expected)return;D3D11_TEXTURE2D_DESC actual{},wanted{};texture->GetDesc(&actual);expected->GetDesc(&wanted);
    if(actual.Width!=wanted.Width || actual.Height!=wanted.Height)return;
    ++count;
    const auto id=ObservedComIdentity(resource.Get()),target=ObservedComIdentity(e.resource.Get());
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};view->GetDesc(&vd);
    LOG_INFO("R805 selected binding: pid={} stage={} generation={} frame={} selected={} dsv_resource={} canonical_match={} interface_match={} size={}x{} format={} mip={} flags={} no_authority=true",
        GetCurrentProcessId(),stage,e.generation,e.frame,target,id,id && id==target,resource.Get()==e.resource.Get(),actual.Width,actual.Height,static_cast<unsigned>(actual.Format),
        vd.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D?vd.Texture2D.MipSlice:UINT_MAX,vd.Flags);
}
void ObserveSelectedTiming(ID3D11DeviceContext* c,bool present)noexcept {
    if(TrackingInternal())return;const auto& e=GenshinDepthEvidence::Expected();
    if(!e.resource || e.context.Get()!=c || e.thread!=GetCurrentThreadId())return;
    static unsigned presents=0,ends=0;auto& count=present?presents:ends;if(count>=12)return;++count;
    IUnknown* raw=nullptr;UINT size=sizeof(raw);bool observed=false,history=false,quiescent=false;uint64_t active=0,types=0;
    Microsoft::WRL::ComPtr<IUnknown> held;Microsoft::WRL::ComPtr<GenshinQueryScope::ContextState> query;
    if(SUCCEEDED(c->GetPrivateData(GenshinQueryScope::StateKey,&size,&raw)) && raw){held.Attach(raw);
        if(size==sizeof(raw) && SUCCEEDED(raw->QueryInterface(GenshinQueryScope::StateKey,reinterpret_cast<void**>(query.GetAddressOf()))) &&
           !query->busy.test_and_set()){
            observed=true;history=query->ranges.KnownHistory();quiescent=query->ranges.KnownQuiescent();
            active=query->ranges.Active();types=query->ranges.ActiveTypes();query->busy.clear();
        }}
    unsigned buffers=0,ready=0,mapped=0;bool cache=false;
    {auto& state=GenshinDepthSemantic::detail::StateStore();std::unique_lock lock(state.mutex,std::try_to_lock);
        if(lock.owns_lock() && state.context.Get()==c){cache=true;for(const auto& b:state.buffers)if(b.object){++buffers;mapped+=b.mapped;ready+=b.payload && b.payload->ready && !b.payload->failed;}}}
    LOG_INFO("R805 selected timing: pid={} stage={} generation={} frame={} query_observed={} history={} quiescent={} active={} types={:X} cache_observed={} cb_buffers={} cb_ready={} cb_mapped={} no_authority=true",
        GetCurrentProcessId(),present?"Present":"QueryEnd",e.generation,e.frame,observed,history,quiescent,active,types,cache,buffers,ready,mapped);
}

// Scalar TLS classification only; no COM/getter/stackwalk or new hook. The
// existing internal/handled/unconditional revocation paths remain mandatory.
GenshinReturnedRaster::UntrackedCall UntrackedDiagnostic(unsigned method,GenshinReturnedRaster::UntrackedSite site,uintptr_t caller,unsigned action=0)noexcept
{
    return {method,(t_copying?1u:0u)|(t_genshinCapture?2u:0u)|(GenshinLegacy::IsInternalCallActive()?4u:0u),site,action,caller};
}
// Common depth hooks do not imply coverage of query/state/lifetime boundaries.
// Written only by the try-only installer, after the bridge transaction commits.
std::atomic<bool> g_genshinInstalled {false};
// Process identity is immutable. Shared hooks must not retain another game's
// indirect allocation while waiting for the Genshin Legacy gate to reject it.
bool GenshinIndirectObservationEnabled()noexcept
{
    static const bool game=[]()noexcept{
        wchar_t path[1024]{};const DWORD n=GetModuleFileNameW(nullptr,path,1024);
        return n && n<1024 && GenshinFsr::IsGenshinExecutable(std::wstring_view(path,n));
    }();
    return game && g_genshinInstalled.load();
}
bool BeforeGenshinDraw(ID3D11DeviceContext* context,uintptr_t diagnosticCaller,
    std::optional<GenshinLegacy::DrawControlsSnapshot>* drawControls,UINT count,bool countKnown=true,unsigned drawKind=0,
    UINT start=0,INT base=0,UINT instances=1,UINT instanceStart=0,UINT actualCount=0,
    ID3D11Buffer* indirectBuffer=nullptr,UINT indirectOffset=0)
{
    if (TrackingInternal()) {GenshinReturnedRaster::BeforeUntrackedRaster(context,UntrackedDiagnostic(drawKind,GenshinReturnedRaster::UntrackedSite::InternalBranch,diagnosticCaller));return false;}
    GenshinCpuStageTiming::Scope hostHook(GenshinIndirectObservationEnabled()?
        GenshinCpuStageTiming::Stage::HostDrawHook:GenshinCpuStageTiming::Stage::Count);
    std::optional<GenshinLegacy::ControlsSnapshot> acquiredControls;
    bool inputsEnabled=true;
    bool observationActive=true;
    if(g_genshinInstalled.load(std::memory_order_acquire)){
        acquiredControls=Config::Instance()->AcquireGenshinControls();
        if(drawControls)drawControls->emplace(GenshinLegacy::DrawControlsSnapshot{*acquiredControls,context,GetCurrentThreadId()});
        inputsEnabled=acquiredControls->enableFsr2Inputs && acquiredControls->useFsr2Inputs;
        observationActive=GenshinLegacy::UpdateNativeObservationDemand(*acquiredControls);
        // The first disabled draw still enters the legacy owner to drain/revoke
        // admitted work. Subsequent no-consumer draws bypass all optional probes.
        if(!observationActive && !GenshinLegacy::NativeObservationDrainNeeded())return false;
    }
    if(observationActive){
        GenshinDepthMapping::ObserveDrawArguments(context,drawKind,actualCount?actualCount:count,start,base,instances,instanceStart,GenshinIndirectObservationEnabled()?indirectBuffer:nullptr,indirectOffset);
        // Writer subscription is selected under the existing native owner lock.
        // Keep the actual draw metadata above even when optional collection is off.
    }
    // Target-only hint after current demand and generic draw observations.
    // Zero/unknown (including DrawAuto/Indirect) cannot enter the fast reject.
    const bool knownNonTargetDraw=countKnown && count!=0 && count!=3;
    struct Guard { Guard(){ t_genshinCapture=true; } ~Guard(){ t_genshinCapture=false; } } guard;
    GenshinLegacy::OptionalDrawObserver optional;
    if(observationActive)optional.collect=+[](ID3D11DeviceContext* context)noexcept{
        const auto& draw=GenshinDepthMapping::CurrentDraw();
        GenshinCpuWriterProbe::BeforeDraw(context,draw.kind,draw.count,draw.start,draw.base,draw.instances,draw.instanceStart);
    };
    if(!acquiredControls)inputsEnabled=Config::Instance()->EnableFsr2Inputs.value_or_default() &&
        Config::Instance()->UseFsr2Inputs.value_or_default();
    const auto action=GenshinLegacy::BeforeNativeDraw(context,count,inputsEnabled,countKnown,drawKind,
        acquiredControls?&*acquiredControls:nullptr,knownNonTargetDraw,&optional);
    if(action!=GenshinFsr::NativeDrawAction::ContinueOriginal){
        GenshinReturnedRaster::BeforeUntrackedRaster(context,UntrackedDiagnostic(drawKind,GenshinReturnedRaster::UntrackedSite::BridgeHandled,diagnosticCaller,static_cast<unsigned>(action)));return true;}
    if(observationActive && optional.full)GenshinDepthMapping::Deferred::Outputs(context,false,true);
    return false;
}
void RevokeRasterView(ID3D11DeviceContext* context,ID3D11View* view,GenshinReturnedRaster::Boundary boundary,const char* diagnosticOperation=nullptr)
{
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;if(view)view->GetResource(&resource);
    GenshinReturnedRaster::Invalidate(context,resource.Get(),boundary,diagnosticOperation);
}
void AfterGenshinBoundary(ID3D11DeviceContext* context)
{
    if (!TrackingInternal()) GenshinCpuWriterProbe::Revoke(context);
    if (!TrackingInternal()) GenshinLegacy::AfterContextBoundary(context);
}
// R719-P1: the bridge-only upgrade patches methods already reachable by
// peers. Installer serialization and a fresh device do not make them idle.
// Stage all provenance work first, then enlist every snapshot peer immediately
// before commit. Keep handles until Detours has resumed/adjusted those threads.
// A denied/exited/unlisted-capacity thread refuses the whole upgrade; never
// skip it, suspend it ourselves, or publish partial query coverage.
class UpgradePeers final
{
    std::array<HANDLE,1024> handles_{};
    std::array<DWORD,1024> threadIds_{};
    unsigned count_=0;
    static LONG LastFailure()noexcept
    { const auto error=GetLastError();return error?static_cast<LONG>(error):ERROR_GEN_FAILURE; }
    static LONG Validate(HANDLE thread,DWORD expectedId)noexcept
    {
        // Snapshot IDs may be recycled before OpenThread. Authority comes
        // from this retained handle, never from the enumerated owner alone.
        if(GetProcessIdOfThread(thread)!=GetCurrentProcessId() || GetThreadId(thread)!=expectedId)
            return ERROR_INVALID_DATA;
        DWORD exitCode=0;
        if(!GetExitCodeThread(thread,&exitCode))return LastFailure();
        return exitCode==STILL_ACTIVE?NO_ERROR:ERROR_INVALID_DATA;
    }
public:
    ~UpgradePeers(){for(unsigned i=0;i<count_;++i)CloseHandle(handles_[i]);}
    UpgradePeers()=default;
    UpgradePeers(const UpgradePeers&)=delete;
    UpgradePeers& operator=(const UpgradePeers&)=delete;
    LONG Prepare()noexcept
    {
        const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if(snapshot==INVALID_HANDLE_VALUE)return LastFailure();
        THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
        LONG result=NO_ERROR;
        if(!Thread32First(snapshot,&entry))result=LastFailure();
        else do {
            if(entry.th32OwnerProcessID!=GetCurrentProcessId() || entry.th32ThreadID==GetCurrentThreadId())continue;
            if(count_==handles_.size()){result=ERROR_INSUFFICIENT_BUFFER;break;}
            const auto thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|
                                         THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);
            if(!thread){result=LastFailure();break;}
            handles_[count_]=thread;threadIds_[count_++]=entry.th32ThreadID;
            result=Validate(thread,entry.th32ThreadID);
            if(result!=NO_ERROR)break;
            entry.dwSize=sizeof(entry);
        }while(Thread32Next(snapshot,&entry));
        if(result==NO_ERROR && GetLastError()!=ERROR_NO_MORE_FILES)result=LastFailure();
        CloseHandle(snapshot);return result;
    }
    LONG Enlist()noexcept
    {
        for(unsigned i=0;i<count_;++i){
            // Recheck lifetime immediately before asking Detours to suspend it.
            // No denied, exited or mismatched peer is silently omitted.
            auto result=Validate(handles_[i],threadIds_[i]);
            if(result==NO_ERROR)result=DetourUpdateThread(handles_[i]);
            if(result!=NO_ERROR)return result;
        }
        return NO_ERROR;
    }
};
// Query observation is independent of depth selection/NR/internal-call state.
// Begin is recorded before forwarding; End retires only after the real End.
// Never end/restart a game query or change its original arguments/results.
// Query markers do not write depth content. Keep the completed writer while
// the independent query tracker continues to forbid private GPU work inside
// active or unproved ranges. In particular, End-only EVENT/TIMESTAMP markers
// must not erase the source before the native FSR transaction can claim it.
void STDMETHODCALLTYPE hkQueryBegin(ID3D11DeviceContext* context,ID3D11Asynchronous* async)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkQueryBegin);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
  if(!GenshinQueryScope::ClaimRangeForward(context,async,true,caller))GenshinQueryScope::Observe(context,async,true,caller);
  r448Hook.Original([&]{return o_QueryBegin(context,async);}); }
void STDMETHODCALLTYPE hkQueryEnd(ID3D11DeviceContext* context,ID3D11Asynchronous* async)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkQueryEnd);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
  const bool forwarded=GenshinQueryScope::ClaimRangeForward(context,async,false,caller);
  r448Hook.Original([&]{return o_QueryEnd(context,async);}); if(!forwarded)GenshinQueryScope::Observe(context,async,false);
  if(forwarded)return; // The alternate outer End owns its post-return timing sample.
  if(!TrackingInternal()){
    const auto& expected=GenshinDepthEvidence::Expected();
    if(expected.resource && expected.context.Get()==context && expected.thread==GetCurrentThreadId()){
      Microsoft::WRL::ComPtr<ID3D11Query> query;
      if(async && SUCCEEDED(async->QueryInterface(IID_PPV_ARGS(&query)))){D3D11_QUERY_DESC desc{};query->GetDesc(&desc);
        if(desc.Query==D3D11_QUERY_TIMESTAMP_DISJOINT){
          // Service the actual post-End window before the next game Begin or
          // CB overwrite. Poll retains the original same-revision eligibility,
          // full query/native admission and nonblocking GPU completion gates.
          // Forwarded inner End returns above; only the outer hook services it.
          GenshinDepthSemantic::Poll(context);ObserveSelectedTiming(context,false);
        }}
    }
  }
}
void STDMETHODCALLTYPE hkStateQueryBegin(ID3D11DeviceContext* context,ID3D11Asynchronous* async)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkStateQueryBegin);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); GenshinQueryScope::Observe(context,async,true,reinterpret_cast<uintptr_t>(_ReturnAddress()));
  GenshinQueryScope::RangeForward forward(context,async,true,g_stateBeginReturn);r448Hook.Original([&]{return o_StateQueryBegin(context,async);}); }
void STDMETHODCALLTYPE hkStateQueryEnd(ID3D11DeviceContext* context,ID3D11Asynchronous* async)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkStateQueryEnd);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); {GenshinQueryScope::RangeForward forward(context,async,false,g_stateEndReturn);r448Hook.Original([&]{return o_StateQueryEnd(context,async);});}
  GenshinQueryScope::Observe(context,async,false);
  if(!TrackingInternal()){
    const auto& expected=GenshinDepthEvidence::Expected();
    if(expected.resource && expected.context.Get()==context && expected.thread==GetCurrentThreadId()){
      Microsoft::WRL::ComPtr<ID3D11Query> query;
      if(async && SUCCEEDED(async->QueryInterface(IID_PPV_ARGS(&query)))){D3D11_QUERY_DESC desc{};query->GetDesc(&desc);
        if(desc.Query==D3D11_QUERY_TIMESTAMP_DISJOINT){
          // Service the actual post-End window before the next game Begin or
          // CB overwrite. Poll retains the original same-revision eligibility,
          // full query/native admission and nonblocking GPU completion gates.
          // Forwarded inner End returns above; only the outer hook services it.
          GenshinDepthSemantic::Poll(context);ObserveSelectedTiming(context,false);
        }}
    }
  }
}
void STDMETHODCALLTYPE hkStateExecute(ID3D11DeviceContext* context,ID3D11CommandList* list,BOOL restore)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(context);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::OpaqueBoundary(context);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkStateExecute);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); GenshinLegacy::PsState::Mutation psMutation(context); GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::State); GenshinQueryScope::OpaqueBoundary(context);r448Hook.Original([&]{return o_StateExecute(context,list,restore);}); }
HRESULT STDMETHODCALLTYPE hkStateFinish(ID3D11DeviceContext* context,BOOL restore,ID3D11CommandList** list)
{
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::OpaqueBoundary(context);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkStateFinish);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); GenshinLegacy::PsState::Mutation psMutation(context); GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::State); GenshinQueryScope::OpaqueBoundary(context);return r448Hook.Original([&]{return o_StateFinish(context,restore,list);}); }
void STDMETHODCALLTYPE hkStateClear(ID3D11DeviceContext* context)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkStateClear);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); GenshinLegacy::PsState::Mutation psMutation(context); GenshinReturnedRaster::BindingChanged(context);r448Hook.Original([&]{return o_StateClear(context);});
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputCleared(context);
  if(!TrackingInternal())GenshinCpuWriterProbe::Revoke(context); }
// These state/deferred/discard boundaries revoke source admission even when depth finding is off.
// ClearState does not certify query quiescence; its tracked ranges survive.
void STDMETHODCALLTYPE hkClearState(ID3D11DeviceContext* context)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkClearState);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); GenshinLegacy::PsState::Mutation psMutation(context); GenshinReturnedRaster::BindingChanged(context);r448Hook.Original([&]{return o_ClearState(context);});
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputCleared(context);
  if(!TrackingInternal())GenshinCpuWriterProbe::Revoke(context); }
void STDMETHODCALLTYPE hkExecuteCommandList(ID3D11DeviceContext* context,ID3D11CommandList* list,BOOL restore)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(context);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::OpaqueBoundary(context);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkExecuteCommandList);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); GenshinLegacy::PsState::Mutation psMutation(context); GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::State); GenshinQueryScope::OpaqueBoundary(context); r448Hook.Original([&]{return o_ExecuteCommandList(context,list,restore);}); AfterGenshinBoundary(context); }
HRESULT STDMETHODCALLTYPE hkFinishCommandList(ID3D11DeviceContext* context,BOOL restore,ID3D11CommandList** list)
{
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::OpaqueBoundary(context);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkFinishCommandList);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); GenshinLegacy::PsState::Mutation psMutation(context); GenshinReturnedRaster::Invalidate(context,nullptr,GenshinReturnedRaster::Boundary::State); GenshinQueryScope::OpaqueBoundary(context); const auto result=r448Hook.Original([&]{return o_FinishCommandList(context,restore,list);}); AfterGenshinBoundary(context); return result; }
void STDMETHODCALLTYPE hkDiscardResource(ID3D11DeviceContext1* context,ID3D11Resource* resource)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(context);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(context,resource);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDiscardResource);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); GenshinReturnedRaster::Invalidate(context,resource,GenshinReturnedRaster::Boundary::State); r448Hook.Original([&]{return o_DiscardResource(context,resource);}); AfterGenshinBoundary(context); }
void STDMETHODCALLTYPE hkDiscardView(ID3D11DeviceContext1* context,ID3D11View* view)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(context);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(context,view);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDiscardView);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); RevokeRasterView(context,view,GenshinReturnedRaster::Boundary::State); r448Hook.Original([&]{return o_DiscardView(context,view);}); AfterGenshinBoundary(context); }
void STDMETHODCALLTYPE hkDiscardView1(ID3D11DeviceContext1* context,ID3D11View* view,const D3D11_RECT* rects,UINT count)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(context);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(context,view);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDiscardView1);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal()); RevokeRasterView(context,view,GenshinReturnedRaster::Boundary::State); r448Hook.Original([&]{return o_DiscardView1(context,view,rects,count);}); AfterGenshinBoundary(context); }
void ObserveGenshinStateSwapTable(ID3D11DeviceContext1* context,unsigned sample,bool returned)
{
    // Bounded read-only observation. Never retries admission, mutates the
    // native ledger, invokes a COM method or supplies a query/depth witness.
    const auto savedError=GetLastError();
    struct ErrorRestore {DWORD value;~ErrorRestore(){SetLastError(value);}} restore{savedError};
    namespace d=GenshinNativeQuery::detail;namespace cr=GenshinNativeQuery::context_recipe;
    uintptr_t object=reinterpret_cast<uintptr_t>(context),table=0,base=0,device=0;
    std::array<uintptr_t,cr::Count> actual{},expected{};
    std::array<unsigned,3> differences{UINT_MAX,UINT_MAX,UINT_MAX};
    std::array<unsigned char,5> flags{0xff,0xff,0xff,0xff,0xff};
    uint32_t deferred=UINT_MAX;bool read=false,privateInline=false;unsigned readMask=0;
    {
        d::Guard guard;if(!guard.held)return;
        const auto& ledger=d::Ledger();if(!ledger.imageCount || !ledger.factoryModule)return;
        const auto& image=ledger.images[0];base=reinterpret_cast<uintptr_t>(image.live);
        if(image.live!=ledger.factoryModule)return;
        const bool got=d::Memory(context,&table,sizeof(table));
        privateInline=got && cr::PrivateInline(context,reinterpret_cast<const void*>(table));
        read=privateInline && d::Memory(reinterpret_cast<const void*>(table),actual.data(),sizeof(actual));
        if(read){
            for(unsigned mode=0;mode<3;++mode){
                differences[mode]=0;
                for(unsigned slot=0;slot<cr::Count;++slot){
                    uintptr_t value=0;
                    if(mode==2)value=base+cr::StateSwapExpectedRvas[slot];
                    else{
                        const auto offset=cr::TableRva+mode*cr::Stride+slot*sizeof(uintptr_t);
                        if(!image.SectionHas(static_cast<uint32_t>(offset),sizeof(uintptr_t),IMAGE_SCN_MEM_READ,IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE)){
                            differences[mode]=UINT_MAX;break;
                        }
                        value=d::Read<uintptr_t>(image.clean.data()+offset);
                        if(slot==111)value=base+0xfd1e0;if(slot==144)value=base+0x1b1780;
                    }
                    if(mode==2)expected[slot]=value;
                    differences[mode]+=(actual[slot]!=value);
                }
            }
            if(object>=0x108){
                const auto native=object-0x108;
                if(native<=UINTPTR_MAX-0xe84){
                    constexpr unsigned offsets[]{0xe76,0xe78,0xe79,0xe7a};
                    for(unsigned n=0;n<4;++n)if(d::Memory(reinterpret_cast<const void*>(native+offsets[n]),&flags[n],1))readMask|=1u<<n;
                    if(d::Memory(reinterpret_cast<const void*>(native+0xe80),&deferred,sizeof(deferred)))readMask|=16;
                    if(d::Memory(reinterpret_cast<const void*>(native+0xa0),&device,sizeof(device)) && device && device<=UINTPTR_MAX-0x470 &&
                       d::Memory(reinterpret_cast<const void*>(device+0x470),&flags[4],1))readMask|=32;
                }
            }
        }
    }
    LOG_WARN("R817 swap boundary: pid={} sample={} returned={} thread={} object={:X} table={:X} factory={:X} read={} private_inline={} constructor0_diff={} constructor1_diff={} swap_diff={} field_mask={} e76={} e78={} e79={} e7a={} e80={} device470={} observation_only=true no_authority=true",
        GetCurrentProcessId(),sample,returned,GetCurrentThreadId(),object,table,base,read,privateInline,differences[0],differences[1],differences[2],readMask,
        flags[0],flags[1],flags[2],flags[3],deferred,flags[4]);
    if(read && differences[2]){
        unsigned emitted=0;
        for(unsigned slot=0;slot<cr::Count && emitted<16;++slot)if(actual[slot]!=expected[slot]){
            ++emitted;
            LOG_WARN("R817 swap difference: pid={} sample={} returned={} slot={} actual={:X} expected={:X} actual_rva={:X} expected_rva={:X} observation_only=true no_authority=true",
                GetCurrentProcessId(),sample,returned,slot,actual[slot],expected[slot],actual[slot]>=base?actual[slot]-base:UINTPTR_MAX,expected[slot]-base);
        }
    }
}


void STDMETHODCALLTYPE hkSwapDeviceContextState(ID3D11DeviceContext1* context,ID3DDeviceContextState* state,ID3DDeviceContextState** previous)
{
    // An external state object can hide SO bindings/cursors and GPU writes.
    // Revoke before the opaque operation, not after an unobserved draw.
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::OpaqueBoundary(context);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkSwapDeviceContextState);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    GenshinLegacy::PsState::Mutation psMutation(context);
    static std::atomic<unsigned> samples{0};
    auto sample=samples.load();const bool observe=sample<3 && samples.compare_exchange_strong(sample,sample+1);
    if(observe)ObserveGenshinStateSwapTable(context,sample,false);
    GenshinReturnedRaster::BindingChanged(context);
    { GenshinQueryScope::StateSwap scope(context,state);
      r448Hook.Original([&]{return o_SwapDeviceContextState(context,state,previous);});
      if(observe)ObserveGenshinStateSwapTable(context,sample,true);
      scope.Returned(previous?*previous:nullptr); }
    // Even a private SR state swap can change the native dispatch table. It
    // grants no coverage of alternate draw/SO entries: refuse immediately if
    // the actual returned table no longer reaches this committed hook set.
    if(gimi::runtime::Enabled() && !ModStreamHooksCover(context))
        gimi::runtime::OpaqueBoundary(context);
    if(!TrackingInternal())GenshinCpuWriterProbe::Revoke(context);
}
HRESULT STDMETHODCALLTYPE hkCreateContextState(ID3D11Device1* device,UINT flags,const D3D_FEATURE_LEVEL* levels,
    UINT count,UINT sdk,REFIID emulated,D3D_FEATURE_LEVEL* chosen,ID3DDeviceContextState** state)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkCreateContextState);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    const auto result=r448Hook.Original([&]{return o_CreateContextState(device,flags,levels,count,sdk,emulated,chosen,state);});
    if(result==S_OK && state && *state && IsEqualIID(emulated,__uuidof(ID3D11Device)))
        GenshinQueryScope::NoteD3D11State(*state);
    return result;
}

std::atomic<bool> g_installed {false};
std::atomic<bool> g_installFailed {false};
// Protected by QueryInstallAccess. A committed partial set cannot be attached
// again: missing 11.1 coverage stays refused for this process lifetime.
bool g_bridgeHooksAttached = false;

// Query public SDK interfaces once, before the existing install transaction.
// A different raster interface address is not covered by this context's
// callbacks/identity. Absence is fine; failed or nonidentical aliases are not.
template<class Context>
bool ModStreamAliasMatches(ID3D11DeviceContext* context) noexcept
{
    Microsoft::WRL::ComPtr<Context> alias;
    const auto result=context->QueryInterface(IID_PPV_ARGS(&alias));
    if(result==E_NOINTERFACE)return true;
    return SUCCEEDED(result) && alias &&
        static_cast<ID3D11DeviceContext*>(alias.Get())==context;
}

// Off-draw publication and state-swap return only. No per-raster table scan,
// private ABI inference, native validation/IO, COM getter or new hook owner.
// Unknown alternate dispatch is refused; external Execute/Finish/Swap revoke
// before forwarding and both real Clear entries record their returned state.
bool ModStreamHooksCover(ID3D11DeviceContext* context) noexcept
{
    if(!g_modSOHookCommitted.load(std::memory_order_acquire) ||
       !g_modSOOwner.live || !g_modSOOwner.detachReady ||
       g_modSOOwner.entry!=g_modSOHookTarget || g_modSOOwner.executed!=&(PVOID&)o_ModSOSetTargets ||
       !g_modSONativeApplication || !context || context!=g_modSOContext ||
       context!=g_context || !g_installed.load() || !g_bridgeHooksAttached ||
       !g_genshinInstalled.load() || !g_modWriteHooksComplete.load() ||
       GenshinNativeQuery::detail::store.revoked.load(std::memory_order_acquire))
        return false;
    const auto table=*reinterpret_cast<PVOID**>(context);
    if(!table || table[kSOSetTargets]!=g_modSOHookTarget ||
       table[58]!=g_modSOBoundaryTargets[0] || table[110]!=g_modSOBoundaryTargets[1] ||
       table[114]!=g_modSOBoundaryTargets[2] || table[131]!=g_modSOSwapTarget)
        return false;
    for(size_t i=0;i<kCommonHookSlots.size();++i)
        if(table[kCommonHookSlots[i]]!=g_commonHookTargets[i])return false;
    return true;
}

// Only the game's own context is watched (see g_context).
// R520 shares the original suppression state, never an executor-owned TLS.
thread_local GenshinLegacy::ModRasterObservation t_modRaster;
const gimi::runtime::HookServices kModHooks{
    +[](ID3D11DeviceContext* c,uint64_t& token)noexcept{
        if(!c || c!=g_context || TrackingInternal())return false;
        token=(t_copying?1ull:0ull)|(t_genshinCapture?2ull:0ull);t_genshinCapture=true;return true;},
    +[](ID3D11DeviceContext* c)noexcept{return c && c==g_context && TrackingInternal();},
    +[](ID3D11DeviceContext*,uint64_t token)noexcept{t_copying=(token&1)!=0;t_genshinCapture=(token&2)!=0;},
    +[](ID3D11DeviceContext* c,UINT count,UINT first,INT base,bool)noexcept{
        if(c!=g_context)return false;
        if(!GenshinLegacy::BeforeModRaster(c,count,first,base,t_modRaster))return false;
        g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,1);
        native::lowlatency::OnFirstSubmit();return true;},
    +[](ID3D11DeviceContext* c,UINT,UINT,INT,bool)noexcept{return GenshinLegacy::AfterModRaster(c,t_modRaster);},
    +[](ID3D11DeviceContext* c,UINT count,UINT first,bool)noexcept{
        if(c!=g_context)return false;
        if(!GenshinLegacy::BeforeModDraw(c,count,first,t_modRaster))return false;
        g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,1);
        native::lowlatency::OnFirstSubmit();return true;},
    +[](ID3D11DeviceContext* c,UINT,UINT,bool)noexcept{return GenshinLegacy::AfterModDraw(c,t_modRaster);},
    nullptr,nullptr,nullptr, // authentic trampolines are bound after hook installation
    +[](ID3D11DeviceContext* c,UINT count,UINT instances,UINT first,INT base,UINT firstInstance,bool originalBoundary)noexcept{
        if(c!=g_context)return false;
        if(!GenshinLegacy::BeforeModIndexedInstanced(c,count,instances,first,base,firstInstance,originalBoundary,t_modRaster))return false;
        g_core.OnDraw(reinterpret_cast<uint64_t>(c),count,instances);
        native::lowlatency::OnFirstSubmit();return true;},
    +[](ID3D11DeviceContext* c,UINT count,UINT instances,UINT first,INT base,UINT firstInstance,bool originalBoundary)noexcept{
        return GenshinLegacy::AfterModIndexedInstanced(c,count,instances,first,base,firstInstance,originalBoundary,t_modRaster);}
};
bool Watched(ID3D11DeviceContext* context)
{
    return !TrackingInternal() && (uint64_t) (size_t) context == g_contextId.load(std::memory_order_relaxed);
}

// Watches `context` (already unwrapped) from now on, and lets go of the copy made for the one before.
void UseContext(ID3D11DeviceContext* context)
{
    if(g_context && g_context!=context && DrawObservationDx11::owner)DrawObservationDx11::Revoke();
    context->AddRef();

    if (g_context != nullptr)
        g_context->Release();

    g_context = context;

    GenshinHookCpuTiming::Scope r448StateWait(GenshinHookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
    GenshinHookCpuTiming::Scope r448StateHeld(GenshinHookCpuTiming::Stage::StateLocked);

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    g_contextDevice = device;
    g_contextRealDevice = device;

    IUnknown* real = nullptr;

    if (device != nullptr && Util::CheckForRealObject(__FUNCTION__, device, &real))
        g_contextRealDevice = (ID3D11Device*) real;

    if (device != nullptr)
        device->Release(); // the context holds it

    g_copy.Release();
    g_contextId.store((uint64_t) (size_t) context, std::memory_order_relaxed);
}

// The depth buffer a depth-stencil view points at, as a DepthBuffer (plain data for the core). `*outResource` gets an
// addref'd ID3D11Resource the caller must Release (the copy, if taken, is made from it).
bool Describe(ID3D11DepthStencilView* view, native::DepthBuffer* out, ID3D11Resource** outResource)
{
    if (view == nullptr)
        return false;

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);

    if (resource == nullptr)
        return false;

    ID3D11Texture2D* tex = nullptr;

    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex))) || tex == nullptr)
    {
        resource->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc {};
    tex->GetDesc(&desc);
    tex->Release();

    out->id = (uint64_t) (size_t) resource;
    out->width = desc.Width;
    out->height = desc.Height;
    out->format = (uint32_t) desc.Format;
    out->readOnlyDepth = false; // D3D11 has no read-only depth bind distinct from the view's own flags; not used by the pick

    *outResource = resource; // caller releases
    return true;
}

// Copies `resource` (the picked buffer) into the frame's copy on `context` (g_context, from a hook or Present). A buffer that
// is not copied leaves no copy for the frame: the copy of an earlier one is not offered for it.
void TakeSnapshot(ID3D11DeviceContext* context, ID3D11Resource* resource, const char* where)
{
    GenshinHookCpuTiming::Scope r448StateWait(GenshinHookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
    GenshinHookCpuTiming::Scope r448StateHeld(GenshinHookCpuTiming::Stage::StateLocked);

    static int calls = 0;

    // The first few at info; the rest at debug level: this runs under g_mutex in the game's bind hooks, and a log line
    // is a synchronous file write
    if (calls < 8)
        LOG_INFO("Depth finder (D3D11): TakeSnapshot call {} from {}, {:X}", calls, where, (size_t) resource);
    else if (calls % 200 == 0)
        LOG_DEBUG("Depth finder (D3D11): TakeSnapshot call {} from {}, {:X}", calls, where, (size_t) resource);

    ++calls;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    resource->GetDevice(&device);
    const char* failed = nullptr;

    if (device == nullptr || (device.Get() != g_contextDevice && device.Get() != g_contextRealDevice))
    {
        g_copy.Forget();
        failed = "the picked buffer is of another device than the context watched";
    }
    else
    {
        t_copying = true;
        failed = g_copy.Take(context, resource, g_core.CurrentPick().reversed);
        t_copying = false;
    }

    static const char* logged = nullptr;

    if (failed != nullptr && failed != logged)
        LOG_WARN("Depth finder (D3D11): {}; no copy", failed);

    logged = failed;

    static bool loggedFirst = false;

    if (failed == nullptr && !loggedFirst)
    {
        loggedFirst = true;
        LOG_INFO("Depth finder (D3D11): first depth copy taken, {}x{}, {} R32_FLOAT", g_copy.Width(), g_copy.Height(),
                 g_copy.Samples() > 1 ? std::format("{} samples resolved (nearest) into", g_copy.Samples())
                 : g_copy.Converted() ? std::string("converted to")
                                      : std::string("copied straight into"));
    }
}

void OnBound(ID3D11DeviceContext* context, ID3D11DepthStencilView* view)
{
    native::DepthBuffer buffer;
    ID3D11Resource* resource = nullptr;
    const bool hadDepth = view != nullptr;
    const bool have = hadDepth && Describe(view, &buffer, &resource);

    const auto request = g_core.OnDepthBound((uint64_t) (size_t) context, hadDepth, have ? &buffer : nullptr);

    if (request.take)
        TakeSnapshot(context, (ID3D11Resource*) (size_t) request.id, "unbind");

    if (resource != nullptr)
        resource->Release();
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* This, UINT NumViews,
                                            ID3D11RenderTargetView* const* ppRenderTargetViews,
                                            ID3D11DepthStencilView* pDepthStencilView)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkOMSetRenderTargets);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (Watched(This))
    {
        g_watchedBound.store(true, std::memory_order_relaxed);

        if (g_core.Active())
            OnBound(This, pDepthStencilView);
    }

    if(!TrackingInternal())GenshinCpuWriterProbe::Bound(This,NumViews,ppRenderTargetViews,pDepthStencilView);
    r448Hook.Original([&]{return o_OMSetRenderTargets(This, NumViews, ppRenderTargetViews, pDepthStencilView);});
    ObserveSelectedDsv(This,pDepthStencilView,"OMSetRenderTargets");
}

void STDMETHODCALLTYPE hkOMSetRenderTargetsAndUAV(ID3D11DeviceContext* This, UINT NumRTVs,
                                                  ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                  ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot,
                                                  UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
                                                  const UINT* pUAVInitialCounts)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkOMSetRenderTargetsAndUAV);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (Watched(This))
    {
        g_watchedBound.store(true, std::memory_order_relaxed);

        // KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL changes only the UAVs: the depth buffer bound stays bound.
        if (g_core.Active() && NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            OnBound(This, pDepthStencilView);
    }

    if(!TrackingInternal() && NumRTVs!=D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        GenshinCpuWriterProbe::Bound(This,NumRTVs,ppRenderTargetViews,pDepthStencilView);
    r448Hook.Original([&]{return o_OMSetRenderTargetsAndUAV(This, NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs,
                              ppUnorderedAccessViews, pUAVInitialCounts);});
    if(NumRTVs!=D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)ObserveSelectedDsv(This,pDepthStencilView,"OMSetRenderTargetsAndUAV");
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D11DeviceContext* This, ID3D11DepthStencilView* pView,
                                               UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(This);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(This,pView);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkClearDepthStencilView);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (g_core.Active() && (ClearFlags & D3D11_CLEAR_DEPTH) != 0 && Watched(This))
    {
        native::DepthBuffer buffer;
        ID3D11Resource* resource = nullptr;

        if (Describe(pView, &buffer, &resource))
        {
            const auto request = g_core.OnDepthClear((uint64_t) (size_t) This, buffer, Depth);

            if (request.take)
                TakeSnapshot(This, resource, "clear");

            resource->Release();
        }
    }

    if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(This);
    if(ClearFlags&D3D11_CLEAR_DEPTH)RevokeRasterView(This,pView,GenshinReturnedRaster::Boundary::Clear,"ClearDepthStencilView");
    if(!TrackingInternal())GenshinLegacy::BeforeNativeDepthClear(This,pView,ClearFlags,Depth,Stencil);
    r448Hook.Original([&]{return o_ClearDepthStencilView(This, pView, ClearFlags, Depth, Stencil);});
    ObserveSelectedDsv(This,pView,"ClearDepthStencilView");
    if(!TrackingInternal())GenshinLegacy::AfterNativeDepthClear(This,pView,ClearFlags,Depth,Stencil);
    if (!TrackingInternal()) GenshinDepthEvidence::ObserveDepthClear(This,pView,ClearFlags,Depth);
}

HRESULT STDMETHODCALLTYPE hkCreateDepthCs(ID3D11Device* device,const void* bytes,SIZE_T size,ID3D11ClassLinkage* linkage,ID3D11ComputeShader** shader)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkCreateDepthCs);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());const auto hr=r448Hook.Original([&]{return o_CreateDepthCs(device,bytes,size,linkage,shader);});
 if(!TrackingInternal() && SUCCEEDED(hr) && shader && *shader)GenshinLegacy::NoteRawShaderCreated(device,*shader,bytes,size,2);return hr;}
// A non-raster write invalidates optional mapping authority, never SR. These
// forward the exact original call. Internal copies/replays do not self-revoke.
void STDMETHODCALLTYPE hkDepthCopyRegion1(ID3D11DeviceContext1* c,ID3D11Resource* d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box,UINT flags)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthCopyRegion1);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    auto portableBefore=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Before(c,d):nullptr;
    GenshinReturnedRaster::Invalidate(c,d,GenshinReturnedRaster::Boundary::Copy,"CopySubresourceRegion1");
    if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(c);
    auto portable=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Copy(c,d,s,sub,x,y,z,ss,box,flags,portableBefore):GenshinDepthPortable::Upload{};
    auto gimiContent=(gimi::runtime::Enabled()&&!TrackingInternal())?gimi::runtime::CpuContentSubmission::Copy(c,d,s,sub,x,y,z,ss,box,flags,false):gimi::runtime::CpuContentSubmission{};
    r448Hook.Original([&]{return o_DepthCopyRegion1(c,d,sub,x,y,z,s,ss,box,flags);});
    if(gimiContent)gimiContent.Returned();
    portable.Returned();ObserveSelectedTransfer(c,d,s,"CopyRegion1",sub,ss,box);}
__declspec(noinline) void STDMETHODCALLTYPE hkDepthUpdate1(ID3D11DeviceContext1* c,ID3D11Resource* d,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice,UINT flags)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthUpdate1);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());const auto diagnosticUploadCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto portableBefore=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Before(c,d):nullptr;
    GenshinReturnedRaster::Invalidate(c,d,GenshinReturnedRaster::Boundary::Update,"UpdateSubresource1");
    if(!TrackingInternal())GenshinCpuWriterProbe::Updated(c,d,sub,box,data,flags);
    auto portable=(!TrackingInternal() && g_genshinInstalled.load())?GenshinDepthPortable::Update(c,d,sub,box,data,flags,portableBefore):GenshinDepthPortable::Upload{};
    auto semantic=GenshinDepthSemantic::Submission::Update(c,d,sub,box,data,flags,diagnosticUploadCaller);
    auto gimiContent=(gimi::runtime::Enabled()&&!TrackingInternal())?gimi::runtime::CpuContentSubmission::Update(c,d,sub,box,data,flags):gimi::runtime::CpuContentSubmission{};
    r448Hook.Original([&]{return o_DepthUpdate1(c,d,sub,box,data,row,slice,flags);});
    // Keep exact target/geometry/version invalidation above. Only a returned
    // full pure-CB payload can avoid a global depth-target mutation.
    if(gimiContent)gimiContent.Returned();
    portable.Returned();
    const bool frozenCb=semantic.Returned();
    if(!TrackingInternal() && !frozenCb)GenshinDepthMapping::ObserveNonRasterWrite(c);
}
void STDMETHODCALLTYPE hkDepthClearView(ID3D11DeviceContext1* c,ID3D11View* v,const FLOAT* values,const D3D11_RECT* rects,UINT count)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,v);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthClearView);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());RevokeRasterView(c,v,GenshinReturnedRaster::Boundary::Clear,"ClearView");if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(c);r448Hook.Original([&]{return o_DepthClearView(c,v,values,rects,count);});
 if(!TrackingInternal() && !count)GenshinDepthEvidence::ObserveSurfaceClear(c,v,"CLEAR_VIEW_FLOAT",values);}
void STDMETHODCALLTYPE hkDepthDispatchIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT offset)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthDispatchIndirect);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());GenshinReturnedRaster::BeforeCompute(c);if(!TrackingInternal()){GenshinDepthMapping::ObserveNonRasterWrite(c);GenshinLegacy::ObserveNativeDepthCompute(c,0,0,0);}r448Hook.Original([&]{return o_DepthDispatchIndirect(c,b,offset);});}
void STDMETHODCALLTYPE hkDepthCopyStructure(ID3D11DeviceContext* c,ID3D11Buffer* d,UINT offset,ID3D11UnorderedAccessView* view)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthCopyStructure);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());GenshinReturnedRaster::Invalidate(c,d,GenshinReturnedRaster::Boundary::Copy,"CopyStructureCount");if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(c);r448Hook.Original([&]{return o_DepthCopyStructure(c,d,offset,view);});}
void STDMETHODCALLTYPE hkDepthCopyResource(ID3D11DeviceContext* c,ID3D11Resource* d,ID3D11Resource* s)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthCopyResource);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());if(GenshinDepthSemantic::Internal()){r448Hook.Original([&]{return o_DepthCopyResource(c,d,s);});return;}
    auto portableBefore=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Before(c,d):nullptr;
    GenshinReturnedRaster::Invalidate(c,d,GenshinReturnedRaster::Boundary::Copy,"CopyResource");
    if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(c);
    auto copy=TrackingInternal()?GenshinReturnedRaster::CopySubmission{}:GenshinReturnedRaster::CopySubmission::Begin(c,d,s);
    auto portable=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Copy(c,d,s,0,0,0,0,0,nullptr,0,portableBefore,true):GenshinDepthPortable::Upload{};
    auto gimiContent=(gimi::runtime::Enabled()&&!TrackingInternal())?gimi::runtime::CpuContentSubmission::Copy(c,d,s):gimi::runtime::CpuContentSubmission{};
    r448Hook.Original([&]{return o_DepthCopyResource(c,d,s);});
    if(gimiContent)gimiContent.Returned();
    portable.Returned();copy.Returned();ObserveSelectedTransfer(c,d,s,"CopyResource");ObserveSelectedCopyTracks(c,d,s);}
void STDMETHODCALLTYPE hkDepthCopyRegion(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* s,UINT ss,const D3D11_BOX* box)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthCopyRegion);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    auto portableBefore=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Before(c,d):nullptr;
    GenshinReturnedRaster::Invalidate(c,d,GenshinReturnedRaster::Boundary::Copy,"CopySubresourceRegion");
    if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(c);
    auto portable=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Copy(c,d,s,sub,x,y,z,ss,box,0,portableBefore):GenshinDepthPortable::Upload{};
    auto gimiContent=(gimi::runtime::Enabled()&&!TrackingInternal())?gimi::runtime::CpuContentSubmission::Copy(c,d,s,sub,x,y,z,ss,box,0,false):gimi::runtime::CpuContentSubmission{};
    r448Hook.Original([&]{return o_DepthCopyRegion(c,d,sub,x,y,z,s,ss,box);});
    if(gimiContent)gimiContent.Returned();
    portable.Returned();ObserveSelectedTransfer(c,d,s,"CopyRegion",sub,ss,box);}
__declspec(noinline) void STDMETHODCALLTYPE hkDepthUpdate(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,const D3D11_BOX* box,const void* data,UINT row,UINT slice)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthUpdate);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());const auto diagnosticUploadCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto portableBefore=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Before(c,d):nullptr;
    GenshinReturnedRaster::Invalidate(c,d,GenshinReturnedRaster::Boundary::Update,"UpdateSubresource");
    if(!TrackingInternal())GenshinCpuWriterProbe::Updated(c,d,sub,box,data,0);
    auto portable=(!TrackingInternal() && g_genshinInstalled.load())?GenshinDepthPortable::Update(c,d,sub,box,data,0,portableBefore):GenshinDepthPortable::Upload{};
    auto semantic=GenshinDepthSemantic::Submission::Update(c,d,sub,box,data,0,diagnosticUploadCaller);
    auto gimiContent=(gimi::runtime::Enabled()&&!TrackingInternal())?gimi::runtime::CpuContentSubmission::Update(c,d,sub,box,data,0):gimi::runtime::CpuContentSubmission{};
    r448Hook.Original([&]{return o_DepthUpdate(c,d,sub,box,data,row,slice);});
    // Keep exact target/geometry/version invalidation above. Only a returned
    // full pure-CB payload can avoid a global depth-target mutation.
    if(gimiContent)gimiContent.Returned();
    portable.Returned();
    const bool frozenCb=semantic.Returned();
    if(!TrackingInternal() && !frozenCb)GenshinDepthMapping::ObserveNonRasterWrite(c);
}
// PID97088 passed a null resource through the game's Map error-handling path.
// Preserve every valid/native/internal call; only the authenticated Genshin
// bridge rejects that invalid input before the retail D3D11 entry dereferences it.
// This contains this exact signature; it does not establish the null's origin.
__declspec(noinline) static bool RefuseNullGenshinMap(ID3D11DeviceContext* context,ID3D11Resource* resource) noexcept
{
    if(resource || !context || !g_genshinInstalled.load())return false;
    wchar_t path[32768]{};const DWORD n=GetModuleFileNameW(nullptr,path,32768);
    if(!n || n>=32768 || !GenshinFsr::IsGenshinExecutable(std::wstring_view(path,n)))return false;
    static std::atomic<unsigned> reports{0};
    if(reports.fetch_add(1,std::memory_order_relaxed)<8){try{
        LOG_WARN("R827 phase39 Map refused: pid={} thread={} reason=NullGameResource result=E_INVALIDARG no_resource_substitution=true",GetCurrentProcessId(),GetCurrentThreadId());
    }catch(...){}}
    return true;
}
HRESULT STDMETHODCALLTYPE hkDepthMap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE* mapped)
{
    const auto gimiMap = (gimi::runtime::Enabled() && !TrackingInternal()) ? gimi::runtime::VertexMapStarting(c,r,sub,type) : 0;
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthMap);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if(GenshinDepthSemantic::Internal())
    {
        const auto result=r448Hook.Original([&]{return o_DepthMap(c,r,sub,type,flags,mapped);});
        if(FAILED(result) && gimiMap)gimi::runtime::VertexMapFailed(c,r,sub,gimiMap);
        return result;
    }
    auto portableBefore=(!TrackingInternal()&&g_genshinInstalled.load())?GenshinDepthPortable::Before(c,r):nullptr;
    { GenshinDepthPortable::MapInvalidation mapBoundary(r);
      GenshinReturnedRaster::Invalidate(c,r,GenshinReturnedRaster::Boundary::Map,"Map"); }
    // Portable content invalidation follows successful writable Map below.
    // Replay/semantic/query invalidation at entry is deliberately unchanged.
    GenshinDepthSemantic::Invalidate(c,r,true);
    const auto result=(!r && !TrackingInternal() && RefuseNullGenshinMap(c,r))?E_INVALIDARG:r448Hook.Original([&]{return o_DepthMap(c,r,sub,type,flags,mapped);});
    if(FAILED(result))
    {
        if(gimiMap)gimi::runtime::VertexMapFailed(c,r,sub,gimiMap);
        GenshinDepthSemantic::Unmapped(c,r);
    }
    bool gimiCpuShadow=false;
    if(!TrackingInternal())
    {
        GenshinCpuWriterProbe::Mapped(c,r,sub,type,mapped,result);
        // Only the existing successful whole WRITE_DISCARD shadow identifies
        // an isolated pure CB. Unmap must still return its exact payload receipt.
        const bool isolatedCb=GenshinDepthSemantic::Mapped(c,r,sub,type,flags,mapped,result);
        gimiCpuShadow=isolatedCb;
        if(g_genshinInstalled.load())GenshinDepthPortable::Mapped(c,r,sub,type,flags,mapped,result,portableBefore);
        if(type!=D3D11_MAP_READ && !isolatedCb)GenshinDepthMapping::ObserveNonRasterWrite(c);
    }
    if(gimiMap)gimi::runtime::ConstantMapped(c,r,sub,type,flags,mapped,result,gimiMap,gimiCpuShadow);
    return result;
}
__declspec(noinline) void STDMETHODCALLTYPE hkDepthUnmap(ID3D11DeviceContext* c,ID3D11Resource* r,UINT sub)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    const bool gimiUnmap = gimi::runtime::Enabled() && !TrackingInternal();
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthUnmap);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    // Exact pending shadow writeback must also run after GIMI is turned off.
    auto gimiContent=(gimiUnmap||gimi::runtime::CpuContentSubmission::HasPendingMaps())?
        gimi::runtime::CpuContentSubmission::Unmap(c,r,sub,gimiUnmap):gimi::runtime::CpuContentSubmission{};
    const auto diagnosticUploadCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(GenshinDepthSemantic::Internal())
    {
        auto portable=GenshinDepthPortable::BeforeUnmap(c,r,sub);
        r448Hook.Original([&]{return o_DepthUnmap(c,r,sub);});
        portable.Returned();
        if(gimiUnmap)gimi::runtime::VertexUnmapped(c,r,sub);
        if(gimiContent)gimiContent.Returned();
        return;
    }
    GenshinReturnedRaster::Invalidate(c,r,GenshinReturnedRaster::Boundary::Unmap,"Unmap");
    if(!TrackingInternal())GenshinCpuWriterProbe::BeforeUnmap(c,r,sub);
    auto portable=GenshinDepthPortable::BeforeUnmap(c,r,sub);
    auto semantic=GenshinDepthSemantic::Submission::Unmap(c,r,sub,diagnosticUploadCaller);
    r448Hook.Original([&]{return o_DepthUnmap(c,r,sub);});
    portable.Returned();
    const bool frozenCb=semantic.Returned();
    if(!TrackingInternal() && !frozenCb)GenshinDepthMapping::ObserveNonRasterWrite(c);
    if(gimiUnmap)gimi::runtime::VertexUnmapped(c,r,sub);
    if(gimiContent)gimiContent.Returned();
}
void STDMETHODCALLTYPE hkDepthResolve(ID3D11DeviceContext* c,ID3D11Resource* d,UINT sub,ID3D11Resource* s,UINT ss,DXGI_FORMAT f)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::Written(c,d);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthResolve);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());GenshinReturnedRaster::Invalidate(c,d,GenshinReturnedRaster::Boundary::Resolve,"ResolveSubresource");if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(c);r448Hook.Original([&]{return o_DepthResolve(c,d,sub,s,ss,f);});ObserveSelectedTransfer(c,d,s,"Resolve",sub,ss);}
void STDMETHODCALLTYPE hkDepthMips(ID3D11DeviceContext* c,ID3D11ShaderResourceView* view)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(c);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(c,view);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDepthMips);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());RevokeRasterView(c,view,GenshinReturnedRaster::Boundary::GenerateMips);if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(c);r448Hook.Original([&]{return o_DepthMips(c,view);});}
void STDMETHODCALLTYPE hkClearRtv(ID3D11DeviceContext* context,ID3D11RenderTargetView* view,const FLOAT* values)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(context);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(context,view);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkClearRtv);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());RevokeRasterView(context,view,GenshinReturnedRaster::Boundary::Clear,"ClearRenderOrUavView");if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(context);r448Hook.Original([&]{return o_ClearRtv(context,view,values);});if(!TrackingInternal())GenshinDepthEvidence::ObserveSurfaceClear(context,view,"RTV_FLOAT",values);}
void STDMETHODCALLTYPE hkClearUavUint(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* view,const UINT* values)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(context);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(context,view);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkClearUavUint);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());RevokeRasterView(context,view,GenshinReturnedRaster::Boundary::Clear,"ClearRenderOrUavView");if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(context);r448Hook.Original([&]{return o_ClearUavUint(context,view,values);});if(!TrackingInternal())GenshinDepthEvidence::ObserveSurfaceClear(context,view,"UAV_UINT",values);}
void STDMETHODCALLTYPE hkClearUavFloat(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* view,const FLOAT* values)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(context);
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::ViewWritten(context,view);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkClearUavFloat);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());RevokeRasterView(context,view,GenshinReturnedRaster::Boundary::Clear,"ClearRenderOrUavView");if(!TrackingInternal())GenshinDepthMapping::ObserveNonRasterWrite(context);r448Hook.Original([&]{return o_ClearUavFloat(context,view,values);});if(!TrackingInternal())GenshinDepthEvidence::ObserveSurfaceClear(context,view,"UAV_FLOAT",values);}

void STDMETHODCALLTYPE hkVSSetConstantBuffers(ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                               ID3D11Buffer* const* buffers)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkVSSetConstantBuffers);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (!TrackingInternal()) GenshinCpuWriterProbe::BoundConstantBuffers(
        context, GenshinCpuWriterProbe::BindingStage::Vertex, startSlot, count, buffers);
    r448Hook.Original([&]{return o_VSSetConstantBuffers(context, startSlot, count, buffers);});
}

void STDMETHODCALLTYPE hkPSSetConstantBuffers(ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                               ID3D11Buffer* const* buffers)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkPSSetConstantBuffers);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (!TrackingInternal()) GenshinCpuWriterProbe::BoundConstantBuffers(
        context, GenshinCpuWriterProbe::BindingStage::Pixel, startSlot, count, buffers);
    r448Hook.Original([&]{return o_PSSetConstantBuffers(context, startSlot, count, buffers);});
}

void STDMETHODCALLTYPE hkCSSetConstantBuffers(ID3D11DeviceContext* context, UINT startSlot, UINT count,
                                               ID3D11Buffer* const* buffers)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkCSSetConstantBuffers);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (!TrackingInternal()) GenshinCpuWriterProbe::BoundConstantBuffers(
        context, GenshinCpuWriterProbe::BindingStage::Compute, startSlot, count, buffers);
    r448Hook.Original([&]{return o_CSSetConstantBuffers(context, startSlot, count, buffers);});
}

void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* This, UINT NumViewports, const D3D11_VIEWPORT* pViewports)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkRSSetViewports);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    if (g_core.Active() && NumViewports > 0 && pViewports != nullptr && Watched(This))
        g_core.OnViewport((uint64_t) (size_t) This, pViewports[0].Width);

    r448Hook.Original([&]{return o_RSSetViewports(This, NumViewports, pViewports);});
}

void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDraw);
    GenshinLegacy::PsState::NativeDrawReturnScope psReturned(g_genshinInstalled.load(std::memory_order_acquire)?This:nullptr);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    DrawObservationDx11::Scope drawScope(This,{DrawObservationDx11::Kind::Draw,VertexCount,StartVertexLocation},[]() noexcept{return !TrackingInternal();});
    gimi::runtime::RasterReturnScope gimiReturned(This);
    if(gimi::runtime::Enabled() && !TrackingInternal() &&
       gimi::runtime::TryDraw(This,VertexCount,StartVertexLocation,o_Draw,&gimiReturned))
        {drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    const auto diagnosticCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    std::optional<GenshinLegacy::DrawControlsSnapshot> drawControls;
    const bool skipObservation=!TrackingInternal() &&
        GenshinLegacy::TrySkipNonTargetDrawObservation(This,VertexCount,true,1);
    r448Hook.NonTarget(skipObservation);
    if (!skipObservation && BeforeGenshinDraw(This,diagnosticCaller,&drawControls,VertexCount,true,1,StartVertexLocation)) {drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    if (Watched(This))
    {
        g_core.OnDraw((uint64_t) (size_t) This, VertexCount, 1);
        native::lowlatency::OnFirstSubmit();
    }

    if (!skipObservation) GenshinLegacy::BeforeNativeDepthRaster(This,drawControls?&*drawControls:nullptr);
    r448Hook.Original([&]{return o_Draw(This, VertexCount, StartVertexLocation);});
    gimiReturned.Returned();
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputDrawn(This,VertexCount);
    drawScope.Returned();
    if (!skipObservation && !TrackingInternal()) GenshinLegacy::AfterNativeDraw(This,VertexCount,true,drawControls?&*drawControls:nullptr);
}

void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation,
                                     INT BaseVertexLocation)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDrawIndexed);
    GenshinLegacy::PsState::NativeDrawReturnScope psReturned(g_genshinInstalled.load(std::memory_order_acquire)?This:nullptr);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    DrawObservationDx11::Scope drawScope(This,{DrawObservationDx11::Kind::Indexed,IndexCount,StartIndexLocation,BaseVertexLocation},[]() noexcept{return !TrackingInternal();});
    gimi::runtime::RasterReturnScope gimiReturned(This);
    // Default-off gate is before metadata, getters and the SR owner lock.
    if(gimi::runtime::Enabled() && !TrackingInternal() &&
        gimi::runtime::TryDrawIndexed(This,IndexCount,StartIndexLocation,BaseVertexLocation,o_DrawIndexed,&gimiReturned)){drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    const auto diagnosticCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    std::optional<GenshinLegacy::DrawControlsSnapshot> drawControls;
    const bool skipObservation=!TrackingInternal() &&
        GenshinLegacy::TrySkipNonTargetDrawObservation(This,IndexCount,true,2);
    r448Hook.NonTarget(skipObservation);
    if (!skipObservation && BeforeGenshinDraw(This,diagnosticCaller,&drawControls,IndexCount,true,2,StartIndexLocation,BaseVertexLocation)) {drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    if (Watched(This))
    {
        g_core.OnDraw((uint64_t) (size_t) This, IndexCount, 1);
        native::lowlatency::OnFirstSubmit();
    }

    if (!skipObservation) GenshinLegacy::BeforeNativeDepthRaster(This,drawControls?&*drawControls:nullptr);
    r448Hook.Original([&]{return o_DrawIndexed(This, IndexCount, StartIndexLocation, BaseVertexLocation);});
    gimiReturned.Returned();
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputDrawn(This,IndexCount);
    drawScope.Returned();
    if (!skipObservation && !TrackingInternal()) GenshinLegacy::AfterNativeDraw(This,IndexCount,true,drawControls?&*drawControls:nullptr);
}

void STDMETHODCALLTYPE hkDrawInstanced(ID3D11DeviceContext* This, UINT VertexCountPerInstance, UINT InstanceCount,
                                       UINT StartVertexLocation, UINT StartInstanceLocation)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDrawInstanced);
    GenshinLegacy::PsState::NativeDrawReturnScope psReturned(g_genshinInstalled.load(std::memory_order_acquire)?This:nullptr);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    DrawObservationDx11::Scope drawScope(This,{DrawObservationDx11::Kind::Instanced,VertexCountPerInstance,StartVertexLocation,0,InstanceCount,StartInstanceLocation},[]() noexcept{return !TrackingInternal();});
    const auto diagnosticCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    std::optional<GenshinLegacy::DrawControlsSnapshot> drawControls;
    const bool skipObservation=!TrackingInternal() &&
        GenshinLegacy::TrySkipNonTargetDrawObservation(This,VertexCountPerInstance,true,3);
    r448Hook.NonTarget(skipObservation);
    if (!skipObservation && BeforeGenshinDraw(This,diagnosticCaller,&drawControls,InstanceCount==1?VertexCountPerInstance:0,InstanceCount==1,3,StartVertexLocation,0,InstanceCount,StartInstanceLocation,VertexCountPerInstance)) {drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    if (Watched(This))
    {
        g_core.OnDraw((uint64_t) (size_t) This, VertexCountPerInstance, InstanceCount);
        native::lowlatency::OnFirstSubmit();
    }

    if (!skipObservation) GenshinLegacy::BeforeNativeDepthRaster(This,drawControls?&*drawControls:nullptr);
    r448Hook.Original([&]{return o_DrawInstanced(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);});
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputDrawn(This,VertexCountPerInstance,InstanceCount);
    drawScope.Returned();
    if (!skipObservation && !TrackingInternal()) GenshinLegacy::AfterNativeDraw(This,InstanceCount==1?VertexCountPerInstance:0,InstanceCount==1,drawControls?&*drawControls:nullptr);
}

void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* This, UINT IndexCountPerInstance,
                                              UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                              UINT StartInstanceLocation)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDrawIndexedInstanced);
    GenshinLegacy::PsState::NativeDrawReturnScope psReturned(g_genshinInstalled.load(std::memory_order_acquire)?This:nullptr);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    DrawObservationDx11::Scope drawScope(This,{DrawObservationDx11::Kind::IndexedInstanced,IndexCountPerInstance,StartIndexLocation,BaseVertexLocation,InstanceCount,StartInstanceLocation},[]() noexcept{return !TrackingInternal();});
    gimi::runtime::RasterReturnScope gimiReturned(This);
    // The runtime result is authoritative after any partial submission:
    // close the outer observation as Consumed and never issue a fallback.
    if(gimi::runtime::Enabled() && !TrackingInternal() &&
        gimi::runtime::TryDrawIndexedInstanced(This,IndexCountPerInstance,InstanceCount,StartIndexLocation,
            BaseVertexLocation,StartInstanceLocation,o_DrawIndexedInstanced,&gimiReturned))
    {drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    const auto diagnosticCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    std::optional<GenshinLegacy::DrawControlsSnapshot> drawControls;
    const bool skipObservation=!TrackingInternal() &&
        GenshinLegacy::TrySkipNonTargetDrawObservation(This,IndexCountPerInstance,true,4);
    r448Hook.NonTarget(skipObservation);
    if (!skipObservation && BeforeGenshinDraw(This,diagnosticCaller,&drawControls,InstanceCount==1?IndexCountPerInstance:0,InstanceCount==1,4,StartIndexLocation,BaseVertexLocation,InstanceCount,StartInstanceLocation,IndexCountPerInstance)) {drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    if (Watched(This))
    {
        g_core.OnDraw((uint64_t) (size_t) This, IndexCountPerInstance, InstanceCount);
        native::lowlatency::OnFirstSubmit();
    }

    if (!skipObservation) GenshinLegacy::BeforeNativeDepthRaster(This,drawControls?&*drawControls:nullptr);
    r448Hook.Original([&]{return o_DrawIndexedInstanced(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                          StartInstanceLocation);});
    gimiReturned.Returned();
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputDrawn(This,IndexCountPerInstance,InstanceCount);
    drawScope.Returned();
    if (!skipObservation && !TrackingInternal()) GenshinLegacy::AfterNativeDraw(This,InstanceCount==1?IndexCountPerInstance:0,InstanceCount==1,drawControls?&*drawControls:nullptr);
}

void STDMETHODCALLTYPE hkDrawAuto(ID3D11DeviceContext* This)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDrawAuto);
    GenshinLegacy::PsState::NativeDrawReturnScope psReturned(g_genshinInstalled.load(std::memory_order_acquire)?This:nullptr);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    DrawObservationDx11::Scope drawScope(This,{DrawObservationDx11::Kind::Auto,0,0,0,1,0,false},[]() noexcept{return !TrackingInternal();});
    const auto diagnosticCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    // The vertex count stays unknown; an actual non-FSR PS can reject only
    // optional observation while the original stream-output draw still runs.
    const bool skipObservation=!TrackingInternal() &&
        GenshinLegacy::TrySkipNonTargetDrawObservation(This,0,false,5);
    r448Hook.NonTarget(skipObservation);
    if (!skipObservation && BeforeGenshinDraw(This,diagnosticCaller,nullptr,0,false,5)) {drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    if (Watched(This))
        g_core.OnIndirect((uint64_t) (size_t) This, 1);

    if (!skipObservation) GenshinReturnedRaster::BeforeUntrackedRaster(This,UntrackedDiagnostic(5,GenshinReturnedRaster::UntrackedSite::UnsupportedHook,diagnosticCaller));
    r448Hook.Original([&]{return o_DrawAuto(This);});
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputUnknownDraw(This);
    drawScope.Returned();
    if (!skipObservation && !TrackingInternal()) GenshinLegacy::AfterNativeDraw(This,0,false);
}

void STDMETHODCALLTYPE hkDrawIndexedInstancedIndirect(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                                      UINT AlignedByteOffsetForArgs)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDrawIndexedInstancedIndirect);
    GenshinLegacy::PsState::NativeDrawReturnScope psReturned(g_genshinInstalled.load(std::memory_order_acquire)?This:nullptr);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    DrawObservationDx11::Scope drawScope(This,{DrawObservationDx11::Kind::IndexedIndirect,0,0,0,1,0,false,pBufferForArgs,AlignedByteOffsetForArgs},[]() noexcept{return !TrackingInternal();});
    const auto diagnosticCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool skipObservation=!TrackingInternal() &&
        GenshinLegacy::TrySkipNonTargetDrawObservation(This,0,false,6);
    r448Hook.NonTarget(skipObservation);
    uint64_t observationCall=0;
    std::optional<GenshinDepthMapping::DrawObservationScope> observationScope;
    if(!skipObservation){
        const bool handled=BeforeGenshinDraw(This,diagnosticCaller,nullptr,0,false,6,0,0,0,0,0,pBufferForArgs,AlignedByteOffsetForArgs);
        observationCall=TrackingInternal()?0:GenshinDepthMapping::CurrentDraw().observationCall;
        observationScope.emplace(This,observationCall);
        if(handled){drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    }
    if (Watched(This))
        g_core.OnIndirect((uint64_t) (size_t) This, 1);

    if(!skipObservation){
        GenshinDepthMapping::Deferred::Outputs(This,false);
        if(!TrackingInternal())GenshinLegacy::BeforeNativeDepthRaster(This);
        if(!GenshinReturnedRaster::PendingIndirect(This,pBufferForArgs,AlignedByteOffsetForArgs,observationCall))
            GenshinReturnedRaster::BeforeUntrackedRaster(This,UntrackedDiagnostic(6,GenshinReturnedRaster::UntrackedSite::UnsupportedHook,diagnosticCaller));
    }
    r448Hook.Original([&]{return o_DrawIndexedInstancedIndirect(This, pBufferForArgs, AlignedByteOffsetForArgs);});
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputUnknownDraw(This);
    drawScope.Returned();
    if (!skipObservation && !TrackingInternal()){
        GenshinReturnedRaster::IndirectReturned(This,pBufferForArgs,AlignedByteOffsetForArgs,observationCall);
        GenshinLegacy::AfterNativeDraw(This,0,false);
        GenshinReturnedRaster::FinishIndirectObservation();
    }
}

void STDMETHODCALLTYPE hkDrawInstancedIndirect(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                               UINT AlignedByteOffsetForArgs)
{
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDrawInstancedIndirect);
    GenshinLegacy::PsState::NativeDrawReturnScope psReturned(g_genshinInstalled.load(std::memory_order_acquire)?This:nullptr);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());
    DrawObservationDx11::Scope drawScope(This,{DrawObservationDx11::Kind::Indirect,0,0,0,1,0,false,pBufferForArgs,AlignedByteOffsetForArgs},[]() noexcept{return !TrackingInternal();});
    const auto diagnosticCaller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool skipObservation=!TrackingInternal() &&
        GenshinLegacy::TrySkipNonTargetDrawObservation(This,0,false,7);
    r448Hook.NonTarget(skipObservation);
    if (!skipObservation && BeforeGenshinDraw(This,diagnosticCaller,nullptr,0,false,7)) {drawScope.Returned(DrawObservationDx11::Completion::Consumed);return;}
    if (Watched(This))
        g_core.OnIndirect((uint64_t) (size_t) This, 1);

    if (!skipObservation) GenshinReturnedRaster::BeforeUntrackedRaster(This,UntrackedDiagnostic(7,GenshinReturnedRaster::UntrackedSite::UnsupportedHook,diagnosticCaller));
    r448Hook.Original([&]{return o_DrawInstancedIndirect(This, pBufferForArgs, AlignedByteOffsetForArgs);});
    if(gimi::runtime::Enabled() && !TrackingInternal())gimi::runtime::StreamOutputUnknownDraw(This);
    drawScope.Returned();
    if (!skipObservation && !TrackingInternal()) GenshinLegacy::AfterNativeDraw(This,0,false);
}

void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* This, UINT X, UINT Y, UINT Z)
{
    if(DrawObservationDx11::NeedsDraw() && !TrackingInternal())DrawObservationDx11::NonDrawWrite(This);
    GenshinHookCpuTiming::HookScope r448Hook(GenshinHookCpuTiming::Hook::hkDispatch);
    if(r448Hook.Active())r448Hook.Internal(TrackingInternal());GenshinReturnedRaster::BeforeCompute(This);
    if (g_core.Active() && !t_copying && Watched(This))
        g_core.Counters().dispatches.fetch_add(1, std::memory_order_relaxed);

    if (!TrackingInternal()){GenshinDepthMapping::ObserveNonRasterWrite(This);GenshinLegacy::ObserveNativeDepthCompute(This,X,Y,Z);}
    r448Hook.Original([&]{return o_Dispatch(This, X, Y, Z);});
}

// The game can make its device again (after a device loss, a settings change); the device it presents with says which one it
// draws with now. Another device's context is followed only once the watched one has set no render targets for 3 presents in a
// row (a game still drawing through it is not left for a second swap chain's device, or for a wrapping layer's device the swap
// chain answers with), and only if its calls run the hooks at all. A swap chain of another API (D3D12, frame generation's under
// Dx11wDx12) says nothing: the context from Install is kept. Present thread only.
void FollowPresentDevice(IDXGISwapChain* swapChain)
{
    static ID3D11Device* presentDevice = nullptr;         // the device presented with last, a reference held
    static ID3D11DeviceContext* presentContext = nullptr; // its immediate context, unwrapped, a reference held
    static int streak = 0;

    ID3D11Device* device = nullptr;

    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    if (device != presentDevice)
    {
        SAFE_RELEASE(presentContext);
        SAFE_RELEASE(presentDevice);
        presentDevice = device;
        presentDevice->AddRef();

        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);

        if (context != nullptr)
        {
            ID3D11DeviceContext* real = nullptr;

            if (!Util::CheckForRealObject(__FUNCTION__, context, (IUnknown**) &real))
                real = context;

            presentContext = real;
            presentContext->AddRef();
            context->Release();
        }
    }

    device->Release();

    const bool watchedBusy = g_watchedBound.exchange(false, std::memory_order_relaxed);

    if (presentContext == nullptr || presentContext == g_context || watchedBusy)
    {
        streak = 0;
        return;
    }

    if (++streak < 3)
        return;

    streak = 0;

    // A context whose functions are not the ones hooked is never seen: watching it would see nothing at all.
    if ((*(PVOID**) presentContext)[kOMSetRenderTargets] != g_hookedOMSetRenderTargets)
    {
        static bool loggedUnhooked = false;

        if (!loggedUnhooked)
        {
            loggedUnhooked = true;
            LOG_WARN("Depth finder (D3D11): the game presents with another device, whose immediate context runs other "
                     "functions than the ones hooked; it is not watched");
        }

        return;
    }

    LOG_INFO("Depth finder (D3D11): the game presents with another device now and draws nothing through the one watched; "
             "watching the new one's immediate context");
    UseContext(presentContext);
}

// Application proxy hooks do not authenticate native query work. Select only
// the exact loaded registration core owning BOTH this actual interface table
// and its original entry; system entries forwarding into core remain native.
static bool IndependentReShadeDepthMethod(PVOID* table, PVOID entry) noexcept
{
    if (table == nullptr || entry == nullptr)
        return false;

    HMODULE core = nullptr;
    if (!GetModuleHandleExW(0, L"ReShade64.dll", &core))
        return false;
    struct ReleaseModule
    {
        HMODULE module;
        ~ReleaseModule() { FreeLibrary(module); }
    } release {core};

    const auto ownerIsCore = [core](const void* address) noexcept
    {
        HMODULE owner = nullptr;
        return address != nullptr &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &owner) && owner == core;
    };
    if (!ownerIsCore(table) || !ownerIsCore(entry))
        return false;

    // Public registration role only, never a signature/clean-code/unwrap proof.
    // No export is invoked and no core is loaded by this classification.
    for (const auto* name : {"ReShadeRegisterAddon", "ReShadeUnregisterAddon", "ReShadeRegisterEvent", "ReShadeUnregisterEvent"})
        if (!ownerIsCore(reinterpret_cast<const void*>(GetProcAddress(core, name))))
            return false;
    if (DetourCodeFromPointer(entry, nullptr) != entry)
        return false;

    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(entry), &pinned) && pinned == core;
}

static LONG AttachDepthMethod(PVOID* original, PVOID hook, PVOID* table,
    const std::source_location& caller = std::source_location::current()) noexcept
{
    const auto entry = original ? *original : nullptr;
    if (!IndependentReShadeDepthMethod(table, entry))
        return GenshinNativeQuery::Attach(original, hook, original, nullptr, nullptr, nullptr, true,
            nullptr, nullptr, nullptr, caller);

    // Use the SAME transaction and persistent handler-original slot. Detours
    // publishes it only on commit, and the installer's Abort cancels all work.
    PVOID target = nullptr, detour = nullptr;
    const auto result = DetourAttachEx(original, hook, nullptr, &target, &detour);
    if (result == NO_ERROR && (target != entry || detour != hook))
    {
        // A changed normalization between selection and staging is not an
        // independent entry. Abort the whole install, retaining no authority.
        GenshinNativeQuery::Revoke();
        return ERROR_INVALID_OPERATION;
    }
    return result;
}

// One bounded SO attempt in the existing Install flow, after the mandatory
// common/bridge commit. Use the existing owned Optional transaction so Abort
// restores the staged original and validates the old chain; genuine integrity,
// protection/rollback or receipt failures still revoke through that owner.
static LONG InstallOptionalModStreamOutput(ID3D11Device* device,ID3D11DeviceContext* context,
                                           PVOID* table,PVOID* context1Table,bool nativeAliases) noexcept
{
    if(g_modSOOwner.live || g_modSOHookCommitted.load(std::memory_order_acquire))
        return ERROR_INVALID_OPERATION; // no retry/reassignment of an installed trampoline
    if(!nativeAliases || !device || !context || context!=g_context || !table || !context1Table ||
       !g_installed.load() || !g_bridgeHooksAttached || !g_genshinInstalled.load() ||
       !g_modWriteHooksComplete.load() ||
       GenshinNativeQuery::detail::store.revoked.load(std::memory_order_acquire))
        return ERROR_NOT_SUPPORTED;
    const auto actualTable=*reinterpret_cast<PVOID**>(context);
    if(actualTable!=table || !actualTable[kSOSetTargets])return ERROR_NOT_SUPPORTED;
    for(size_t i=0;i<kCommonHookSlots.size();++i)
        if(actualTable[kCommonHookSlots[i]]!=g_commonHookTargets[i])return ERROR_NOT_SUPPORTED;

    UpgradePeers peers;
    auto result=peers.Prepare();
    if(result!=NO_ERROR)return result;
    GenshinNativeQuery::Own::Batch batch(nullptr,0,GenshinNativeQuery::InstallRequirement::Optional);
    result=batch.Add(g_modSOOwner,&(PVOID&)o_ModSOSetTargets,nullptr,
                     actualTable[kSOSetTargets],reinterpret_cast<PVOID>(hkModSOSetTargets));
    if(result!=NO_ERROR)return batch.Commit(); // real AbortBatch + original-slot restoration
    result=peers.Enlist();
    if(result!=NO_ERROR)return result; // Batch destructor aborts before this helper returns
    result=batch.Commit(); // peers are resumed on success and on failed commit/abort
    if(result!=NO_ERROR)return result;
    if(GenshinNativeQuery::detail::store.revoked.load(std::memory_order_acquire))
        return ERROR_INVALID_DATA; // physical hook may be owned, but grants no coverage
    if(!g_modSOOwner.live || !g_modSOOwner.detachReady)
    {
        GenshinNativeQuery::Revoke();return ERROR_INVALID_DATA;
    }

    // Publish only the successfully committed owned detour, never staged data.
    g_modSOHookTarget=actualTable[kSOSetTargets];
    g_modSOBoundaryTargets={actualTable[58],actualTable[110],actualTable[114]};
    g_modSOSwapTarget=context1Table[131];
    g_modSOContext=context;
    g_modSODevice=device;
    g_modSONativeApplication=nativeAliases;
    g_modSOHookCommitted.store(true,std::memory_order_release);
    return NO_ERROR;
}

// Optional optimization, AFTER all mandatory Bridge hooks have committed.
// Install errors cannot abort that transaction or set g_installFailed. Genuine
// native provenance damage still follows the existing ledger refusal policy.
// Installation-local return receipt, never native authority or an admission gate.
// Each failure returns immediately; Abort records separately and cannot replace it.
struct PsCacheInstallObservation final
{
    const char* phase="prepare";
    const char* predicate="application-context-is-native";
    const char* statusKind="bool";
    LONG status=FALSE;
    int setter=-1;
    bool abortAttempted=false,publicationAttempt=false;
    LONG abortStatus=NO_ERROR;
    uint64_t batchToken=0;
    unsigned missingRole=GenshinLegacy::PublicPsBinding::MethodCount;
    PVOID missingAlias=nullptr;
    GenshinLegacy::PublicPsBinding::Dispatch batchDispatch{};
    std::array<unsigned,GenshinLegacy::PublicPsBinding::MethodCount> batchAliases{},batchTargets{},batchAttaches{};
    GenshinLegacy::PsState::PrepareObservation prepare;
    GenshinLegacy::PsState::PublicationResult publication=GenshinLegacy::PsState::PublicationResult::Refused;

    bool Boolean(const char* nextPhase,const char* nextPredicate,bool value) noexcept
    {
        phase=nextPhase;predicate=nextPredicate;statusKind="bool";
        status=value?TRUE:FALSE;setter=-1;return value;
    }
    LONG Status(const char* nextPhase,const char* nextPredicate,LONG value,int setterIndex=-1) noexcept
    {
        phase=nextPhase;predicate=nextPredicate;statusKind="win32_long";
        status=value;setter=setterIndex;return value;
    }
};
// R864 public API observers. Each executing original has process lifetime and
// is assigned ONCE. Observed alternate public targets retain earlier detours,
// including callers which cached an earlier public method pointer. Nothing here
// registers a private native version or grants a query/GPU capability.
namespace public_ps {
namespace ps=GenshinLegacy::PsState;
namespace api=GenshinLegacy::PublicPsBinding;
namespace d=GenshinNativeQuery::detail;
struct Slot {
    PVOID original=nullptr;
    PVOID target=nullptr; // frozen physical native entry; also retained after failure
    d::Patch patch{};
    bool reserved=false,committed=false,ready=false;
};
std::array<std::array<Slot,api::Capacity>,api::MethodCount> slots{};

// All five documented roles invalidate BEFORE forwarding and through return.
// A void setter is certified by its actual post-return getter, not its args.
// Opaque boundaries never infer restoration from restore=true or HRESULT.
template<size_t I> void STDMETHODCALLTYPE Set(ID3D11DeviceContext* c,ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classes,UINT count) {
    GenshinHookCpuTiming::HookScope timing(GenshinHookCpuTiming::Hook::hkCachePSSetShader);
    if(timing.Active())timing.Internal(TrackingInternal());
    ps::Mutation mutation(c);
    using F=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11PixelShader*,ID3D11ClassInstance* const*,UINT);
    timing.Original([&]{reinterpret_cast<F>(slots[0][I].original)(c,shader,classes,count);});
    mutation.SetterReturned(shader,classes,count);
}
template<size_t I> void STDMETHODCALLTYPE Clear(ID3D11DeviceContext* c) {
    ps::Mutation mutation(c);
    reinterpret_cast<PFN_ClearState>(slots[1][I].original)(c);
}
template<size_t I> void STDMETHODCALLTYPE Execute(ID3D11DeviceContext* c,ID3D11CommandList* list,BOOL restore) {
    ps::Mutation mutation(c);
    reinterpret_cast<PFN_ExecuteCommandList>(slots[2][I].original)(c,list,restore);
}
template<size_t I> HRESULT STDMETHODCALLTYPE Finish(ID3D11DeviceContext* c,BOOL restore,ID3D11CommandList** list) {
    ps::Mutation mutation(c);
    return reinterpret_cast<PFN_FinishCommandList>(slots[3][I].original)(c,restore,list);
}
template<size_t I> void STDMETHODCALLTYPE Swap(ID3D11DeviceContext1* c,ID3DDeviceContextState* state,
    ID3DDeviceContextState** previous) {
    ps::Mutation mutation(c);
    using F=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3DDeviceContextState*,ID3DDeviceContextState**);
    reinterpret_cast<F>(slots[4][I].original)(c,state,previous);
}
#define R864_TARGETS(F) {reinterpret_cast<PVOID>(F<0>),reinterpret_cast<PVOID>(F<1>),reinterpret_cast<PVOID>(F<2>),reinterpret_cast<PVOID>(F<3>),reinterpret_cast<PVOID>(F<4>),reinterpret_cast<PVOID>(F<5>),reinterpret_cast<PVOID>(F<6>),reinterpret_cast<PVOID>(F<7>)}
const std::array<std::array<PVOID,api::Capacity>,api::MethodCount> hooks{{
    R864_TARGETS(Set),R864_TARGETS(Clear),R864_TARGETS(Execute),R864_TARGETS(Finish),R864_TARGETS(Swap)}};
#undef R864_TARGETS

bool PinNative(PVOID address) noexcept {
    HMODULE module=nullptr;
    if(!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module) || module!=GetModuleHandleW(L"d3d11.dll"))return false;
    HMODULE pinned=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(address),&pinned) && pinned==module;
}
bool PinOwn(PVOID address) noexcept {
    HMODULE self=nullptr,module=nullptr,pinned=nullptr;
    const auto flags=GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if(!address || !GetModuleHandleExW(flags,reinterpret_cast<LPCWSTR>(&PinOwn),&self) ||
       !GetModuleHandleExW(flags,reinterpret_cast<LPCWSTR>(address),&module) || self!=module)return false;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(address),&pinned) && pinned==self;
}
// Reuse the EXISTING boundary observer instead of detouring its handler.
// The native ledger certifies that handler's bytes; patching the handler again
// would invalidate its older receipt even when the new call chain was correct.
PVOID* ExistingHookOriginal(size_t role,PVOID target) noexcept {
    switch(role){
    case 1:if(target==reinterpret_cast<PVOID>(hkClearState))return &(PVOID&)o_ClearState;
        if(target==reinterpret_cast<PVOID>(hkStateClear))return &(PVOID&)o_StateClear;break;
    case 2:if(target==reinterpret_cast<PVOID>(hkExecuteCommandList))return &(PVOID&)o_ExecuteCommandList;
        if(target==reinterpret_cast<PVOID>(hkStateExecute))return &(PVOID&)o_StateExecute;break;
    case 3:if(target==reinterpret_cast<PVOID>(hkFinishCommandList))return &(PVOID&)o_FinishCommandList;
        if(target==reinterpret_cast<PVOID>(hkStateFinish))return &(PVOID&)o_StateFinish;break;
    case 4:if(target==reinterpret_cast<PVOID>(hkSwapDeviceContextState))return &(PVOID&)o_SwapDeviceContextState;break;
    }
    return nullptr;
}
bool ExistingOriginal(size_t role,PVOID& target,PVOID*& original) noexcept {
    original=ExistingHookOriginal(role,target);
    if(original)return true;
    // DetourCodeFromPointer need not follow an installed E9/relay into our
    // handler. The exact native entry can therefore still own a core hook.
    // Borrow only that role's committed, intact ledger receipt; never stage
    // another edit over it or adopt an arbitrary observed detour chain.
    d::Guard guard;if(!guard.held)return false;
    const auto* patch=d::Find(target);
    if(!patch)return true; // No existing native edit: retain ordinary selection.
    auto* owned=ExistingHookOriginal(role,patch->hook);
    if(!d::StableLocked() || !owned || patch->original!=owned || patch->executed!=owned ||
       !patch->ownSetup || patch->input.module || !patch->straight || !d::PatchIntact(*patch))return false;
    target=patch->hook;original=owned;return true;
}
bool BorrowExisting(Slot& slot,PVOID requested,PVOID target,PVOID* executed) noexcept {
    auto& p=slot.patch;p={};p.requested=requested;p.hook=target;p.original=p.executed=executed;
    if(!executed || !d::Memory(executed,&p.trampoline,sizeof(p.trampoline)) || !p.trampoline ||
       !d::Memory(p.trampoline,p.code.data(),p.code.size()) ||
       !d::Memory(target,p.ownEntry.data(),p.ownEntry.size()))return false;
    p.size=p.code[62];const auto remainder=d::Read<uintptr_t>(p.code.data()+72);
    if(p.size<5 || p.size>24 || remainder<p.size ||
       d::Read<uintptr_t>(p.code.data()+80)!=reinterpret_cast<uintptr_t>(target))return false;
    p.entry=reinterpret_cast<unsigned char*>(remainder-p.size);
    if(!PinNative(p.entry) || !d::CaptureNativeHops(p,requested,p.entry) || !d::FinalizeOwnPatch(p))return false;
    slot.committed=slot.ready=true;return true;
}
bool EndpointFor(const Slot& slot,PVOID requested,api::Endpoint& result) noexcept {
    if(!slot.ready || !slot.committed || !PinNative(slot.patch.entry) ||
       !PinOwn(slot.patch.hook) || !d::PatchIntact(slot.patch))return false;
    // Rebuild only the forwarding path for this newly exposed PUBLIC alias.
    // The edit/relay/trampoline still derive from the original precommit bytes.
    auto p=slot.patch;p.hopCount=0;
    if(!d::CaptureNativeHops(p,requested,p.entry))return false;
    result.requested=requested;result.hook=p.hook;result.executed=p.executed;
    result.entry=p.entry;result.trampoline=p.trampoline;result.size=p.size;
    result.edit=p.edit;result.code=p.code;result.ownEntry=p.ownEntry;result.hopCount=p.hopCount;
    for(unsigned i=0;i<p.hopCount;++i){const auto& h=p.hops[i];auto& out=result.hops[i];
        if(!PinNative(h.entry) && !PinOwn(h.entry))return false;
        if(h.size>out.code.size())return false;
        out.entry=h.entry;out.size=h.size;std::memcpy(out.code.data(),h.code.data(),h.size);
        if(h.iat){
            HMODULE dataOwner=nullptr,codeOwner=nullptr;
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(h.iat),&dataOwner) ||
               !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(h.entry),&codeOwner) || dataOwner!=codeOwner)return false;
        }
        out.indirect=static_cast<const void* const*>(h.iat);out.target=h.target;
    }
    return result.Current();
}
} // namespace public_ps

static bool InstallPsStateCache(ID3D11DeviceContext* context,ID3D11Device* device,
    PsCacheInstallObservation& observation,
    const std::shared_ptr<GenshinLegacy::PsState::OptionalInstallRequest>& request,
    const GenshinLegacy::PsState::InstallOwnerScope* owner=nullptr) noexcept {
    namespace ps=GenshinLegacy::PsState;namespace api=GenshinLegacy::PublicPsBinding;namespace h=public_ps;
    // Native recipe revocation is deliberately NOT a public COM API capability.
    // This path never clears it and never mints any native/GPU/query witness.
    const auto current=[&](){return request && !request->cancelled.load(std::memory_order_acquire) &&
        owner && owner->Current() && !api::stopped.load(std::memory_order_acquire);};
    if(!observation.Boolean("lifetime","public-cache-owner-current",current()))return false;
    auto candidate=ps::PrepareCoverage(context,device,observation.prepare);
    if(!observation.Boolean("prepare","public-PrepareCoverage-nonnull",static_cast<bool>(candidate)))return false;
    auto token=api::RefreshToken();auto installed=ps::refreshWitness;
    observation.batchToken=token;observation.batchDispatch=candidate->dispatch;
    const auto pending=[&](const char* why) noexcept {
        ps::RetireOwner();ps::publicationFailure=why;ps::publicationWitness=ps::refreshWitness;
        observation.publication=ps::PublicationResult::PendingRefresh;observation.prepare.initializationPending=true;
        return observation.Boolean("refresh",why,false);
    };
    const auto generationCurrent=[&]() noexcept {
        const auto now=api::RefreshToken();
        if(!observation.Boolean("refresh","public-generation-not-exhausted",now!=UINT64_MAX))return false;
        return now==token || pending("install-generation-changed-pending");
    };
    if(!generationCurrent())return false;
    if(!observation.Boolean("collect",installed.refusal,!installed.refused) ||
       !observation.Boolean("collect","public-witness-identity",!installed.context ||
           (installed.context==context && installed.deviceEpoch==request->deviceEpoch)) ||
       !observation.Boolean("collect","public-witness-generation",!installed.token || installed.token==token))return false;
    const auto prior=ps::coverage.load(std::memory_order_acquire);
    if(prior){
        if(!observation.Boolean("collect","public-prior-identity",prior->context.Get()==context && prior->device.Get()==device) ||
           !observation.Boolean("collect","public-prior-receipt-shape",ps::CoverageShapeIntact(*prior)) ||
           !observation.Boolean("collect","public-prior-receipt-intact",ps::PublicCoverageIntact(*prior)))return false;
        candidate->variants=prior->variants;candidate->variantCounts=prior->variantCounts;
    }
    const auto retained=candidate->variantCounts;
    // Complete union FIRST: no reservation or Attach before every role fits.
    // A transient alias remains mandatory even after its vector disappears.
    const auto collect=[&](unsigned m,PVOID alias) noexcept {
        observation.missingRole=m;observation.missingAlias=alias;
        if(!observation.Boolean("collect","public-alias-nonnull",alias!=nullptr))return false;
        auto& count=candidate->variantCounts[m];auto& variants=candidate->variants[m];
        for(unsigned n=0;n<count;++n)if(variants[n].requested==alias)return true;
        if(!observation.Boolean("collect","public-alias-capacity",count<variants.size()))return false;
        variants[count++].requested=alias;observation.batchAliases[m]=count;return true;
    };
    observation.batchAliases=retained;
    unsigned unwitnessedRoles=0;
    for(unsigned m=0;m<api::MethodCount;++m){
        observation.missingRole=m;observation.missingAlias=nullptr;
        if(!observation.Boolean("collect","public-witness-shape",installed.counts[m]<=installed.aliases[m].size()))return false;
        bool remembered=false;
        for(unsigned n=0;n<retained[m];++n)remembered|=candidate->variants[m][n].requested==candidate->dispatch[m];
        for(unsigned n=0;n<installed.counts[m];++n){
            if(!collect(m,installed.aliases[m][n]))return false;
            remembered|=installed.aliases[m][n]==candidate->dispatch[m];
        }
        if(!collect(m,candidate->dispatch[m]))return false;
        if(!remembered)unwitnessedRoles|=1u<<m;
    }
    struct Target final {
        h::Slot* slot=nullptr;
        PVOID entry=nullptr,requested=nullptr,normalized=nullptr;
        PVOID* borrowed=nullptr;
        bool fresh=false;
    };
    std::array<std::array<Target,api::Capacity>,api::MethodCount> targets{};
    std::array<std::array<unsigned,api::Capacity>,api::MethodCount> selected{};
    auto& targetCounts=observation.batchTargets;
    // Normalize each observed alias once. New same-role aliases can converge
    // before any slot is committed: they share ONE planned typed observer.
    for(unsigned m=0;m<api::MethodCount;++m)for(unsigned a=0;a<candidate->variantCounts[m];++a){
        const auto raw=candidate->variants[m][a].requested;
        observation.missingRole=m;observation.missingAlias=raw;
        if(!observation.Boolean("select","public-native-method",h::PinNative(raw)))return false;
        auto normalized=DetourCodeFromPointer(raw,nullptr);
        h::Slot* existing=nullptr;
        for(unsigned r=0;r<api::MethodCount;++r)for(auto& s:h::slots[r])if(s.reserved &&
            (raw==s.patch.requested || normalized==s.patch.hook || normalized==s.target || normalized==s.patch.entry)){
            if(!observation.Boolean("select","public-role-target-distinct",r==m) ||
               !observation.Boolean("select","public-existing-slot-unambiguous",!existing || existing==&s))return false;
            existing=&s;
        }
        PVOID entry=existing?existing->target:normalized;PVOID* borrowed=nullptr;
        if(!existing){
            if(!observation.Boolean("select","public-existing-native-boundary",h::ExistingOriginal(m,normalized,borrowed)))return false;
            if(!observation.Boolean("select","public-normalization-known",borrowed || h::PinNative(normalized)))return false;
            if(borrowed){
                h::Slot probe;
                if(!observation.Boolean("select","public-existing-boundary-receipt",h::BorrowExisting(probe,raw,normalized,borrowed)))return false;
                entry=probe.patch.entry;
            }
        }
        // Include ALL persistent reservations, even failed/unready ones. Their
        // executing originals can never be reclaimed or repointed on a retry.
        for(unsigned r=0;r<api::MethodCount;++r)for(auto& s:h::slots[r])if(s.reserved &&
            (entry==s.target || entry==s.patch.entry)){
            if(!observation.Boolean("select","public-role-target-distinct",r==m) ||
               !observation.Boolean("select","public-existing-slot-unambiguous",!existing || existing==&s))return false;
            existing=&s;
        }
        if(existing){
            api::Endpoint endpoint;
            if(!observation.Boolean("select","public-existing-hook-intact",entry && existing->target==entry &&
                h::EndpointFor(*existing,raw,endpoint)))return false;
        }
        for(unsigned r=0;r<m;++r)for(unsigned n=0;n<targetCounts[r];++n)
            if(!observation.Boolean("select","public-role-target-distinct",entry!=targets[r][n].entry))return false;
        unsigned n=0;for(;n<targetCounts[m];++n)if(targets[m][n].entry==entry)break;
        if(n==targetCounts[m]){
            if(!observation.Boolean("select","public-target-capacity",n<targets[m].size()))return false;
            auto& t=targets[m][n];t.slot=existing;t.entry=entry;t.requested=raw;t.normalized=normalized;
            t.borrowed=borrowed;t.fresh=!existing;++targetCounts[m];
            if(t.fresh && !t.borrowed)++observation.batchAttaches[m];
        }else if(!observation.Boolean("select","public-planned-slot-unambiguous",targets[m][n].slot==existing))return false;
        selected[m][a]=n;
    }
    // Reserve free slot indices in the LOCAL plan; mutate persistent originals
    // only after every role has enough free slots and the generation is stable.
    for(unsigned m=0;m<api::MethodCount;++m){
        std::array<bool,api::Capacity> claimed{};
        for(unsigned n=0;n<targetCounts[m];++n){auto& t=targets[m][n];if(!t.fresh)continue;
            for(unsigned s=0;s<api::Capacity;++s)if(!h::slots[m][s].reserved && !claimed[s]){
                t.slot=&h::slots[m][s];claimed[s]=true;break;
            }
            observation.missingRole=m;observation.missingAlias=t.requested;
            if(!observation.Boolean("select","public-variant-capacity",t.slot!=nullptr))return false;
        }
    }
    observation.missingRole=api::MethodCount;observation.missingAlias=nullptr;
    const bool needsAttach=std::any_of(observation.batchAttaches.begin(),observation.batchAttaches.end(),[](unsigned n){return n!=0;});
    UpgradePeers peers;
    if(needsAttach && observation.Status("peers","UpgradePeers-Prepare",peers.Prepare())!=NO_ERROR)return false;
    if(!generationCurrent())return false;
    if(needsAttach || unwitnessedRoles){
        unsigned attachedRoles=0;for(unsigned m=0;m<api::MethodCount;++m)if(observation.batchAttaches[m])attachedRoles|=1u<<m;
        // Also remember a newly exposed alias reusing an already committed
        // slot. A later-generation retry must not forget it if it disappears.
        // Only this already collected vector may join our exact next token.
        const auto expected=token>=UINT64_MAX-2?UINT64_MAX:((token+2)|uint64_t(1));
        ps::RetireOwner();
        const auto reason=needsAttach?ps::Metrics::Refresh::BeforeAttach:ps::Metrics::Refresh::PublishDispatchChanged;
        const auto ownToken=ps::RequestPublicRefresh(context,request->deviceEpoch,reason,needsAttach?attachedRoles:unwitnessedRoles,candidate->dispatch);
        if(!observation.Boolean("refresh","public-generation-not-exhausted",ownToken!=UINT64_MAX))return false;
        if(ownToken!=expected)return pending("install-generation-changed-before-attach");
        token=ownToken;installed=ps::refreshWitness;observation.batchToken=token;
        if(!observation.Boolean("refresh",installed.refusal,!installed.refused) || !generationCurrent())return false;
    }
    for(unsigned m=0;m<api::MethodCount;++m)for(unsigned n=0;n<targetCounts[m];++n){auto& t=targets[m][n];if(!t.fresh)continue;
        auto& s=*t.slot;s.reserved=true;s.target=t.entry;s.original=t.requested;s.patch={};
        const auto index=static_cast<size_t>(&s-h::slots[m].data());
        s.patch.requested=t.requested;s.patch.hook=h::hooks[m][index];s.patch.original=s.patch.executed=&s.original;
        observation.missingRole=m;observation.missingAlias=t.requested;
        if(t.borrowed && !observation.Boolean("select","public-existing-boundary-receipt",h::BorrowExisting(s,t.requested,t.normalized,t.borrowed) &&
            s.patch.entry==t.entry))return false;
    }
    observation.missingRole=api::MethodCount;observation.missingAlias=nullptr;
    if(needsAttach){
        if(!current())return false;
        if(observation.Status("begin","Begin-Optional",GenshinNativeQuery::Begin(GenshinNativeQuery::InstallRequirement::Optional))!=NO_ERROR)return false;
        const auto abort=[&](){observation.abortAttempted=true;observation.abortStatus=GenshinNativeQuery::Abort();return false;};
        if(observation.Status("thread","DetourUpdateThread",DetourUpdateThread(GetCurrentThread()))!=NO_ERROR)return abort();
        for(unsigned m=0;m<api::MethodCount;++m)for(unsigned n=0;n<targetCounts[m];++n){auto& t=targets[m][n];if(!t.fresh || t.borrowed)continue;
            auto& s=*t.slot;auto& p=s.patch;observation.missingRole=m;observation.missingAlias=t.requested;
            PDETOUR_TRAMPOLINE trampoline=nullptr;PVOID target=nullptr,detour=nullptr;
            if(observation.Status("attach","public-Attach",GenshinNativeQuery::Attach(&s.original,p.hook,&s.original,nullptr,nullptr,nullptr,true,
                &target,&trampoline,&detour),static_cast<int>(m))!=NO_ERROR)return abort();
            p.entry=static_cast<unsigned char*>(target);p.trampoline=reinterpret_cast<unsigned char*>(trampoline);
            // This is the bundled Detours layout already used by Own::Batch,
            // not a constructor/state-swap ABI from the operating system.
            if(!observation.Boolean("precommit","public-own-detour-preimage",
                target==t.entry && target==DetourCodeFromPointer(p.requested,nullptr) && detour==p.hook && trampoline &&
                h::d::Memory(trampoline,p.code.data(),p.code.size()) && h::d::Memory(p.hook,p.ownEntry.data(),p.ownEntry.size())))return abort();
            p.size=p.code[62];
            if(!observation.Boolean("precommit","public-own-detour-shape",p.size>=5 && p.size<=24 &&
                h::d::Read<uintptr_t>(p.code.data()+72)==reinterpret_cast<uintptr_t>(p.entry+p.size) &&
                h::d::Read<uintptr_t>(p.code.data()+80)==reinterpret_cast<uintptr_t>(p.hook) &&
                h::d::Equal(p.entry,p.code.data()+32,p.size) && h::d::CaptureNativeHops(p,p.requested,p.entry)))return abort();
        }
        observation.missingRole=api::MethodCount;observation.missingAlias=nullptr;
        if(!current())return abort();
        if(observation.Status("peers","UpgradePeers-Enlist",peers.Enlist())!=NO_ERROR)return abort();
        if(!current())return abort();
        const auto committed=GenshinNativeQuery::Commit();
        if(observation.Status("commit","public-Commit",committed)!=NO_ERROR)return false;
        // Mark ALL real commits before any finalization can fail. No failure
        // reclaims a slot or changes the already executing transparent original.
        for(unsigned m=0;m<api::MethodCount;++m)for(unsigned n=0;n<targetCounts[m];++n){auto& t=targets[m][n];
            if(t.fresh && !t.borrowed)t.slot->committed=true;
        }
        for(unsigned m=0;m<api::MethodCount;++m)for(unsigned n=0;n<targetCounts[m];++n){auto& t=targets[m][n];if(!t.fresh || t.borrowed)continue;
            auto& s=*t.slot;s.ready=h::d::FinalizeOwnPatch(s.patch);
            observation.missingRole=m;observation.missingAlias=t.requested;
            if(!observation.Boolean("commit","public-owned-patch-intact",s.ready))return false;
        }
    }
    // Authenticate EVERY alias independently after the single bounded batch.
    // A retained receipt must stay exactly equal; never refresh damaged bytes.
    for(unsigned m=0;m<api::MethodCount;++m)for(unsigned a=0;a<candidate->variantCounts[m];++a){
        auto& variant=candidate->variants[m][a];api::Endpoint endpoint;
        observation.missingRole=m;observation.missingAlias=variant.requested;
        if(!observation.Boolean("coverage","public-complete-mutation-hook",h::EndpointFor(*targets[m][selected[m][a]].slot,variant.requested,endpoint)))return false;
        if(a<retained[m] && !observation.Boolean("coverage","public-retained-receipt-equal",variant.SameReceipt(endpoint)))return false;
        variant=endpoint;
        if(variant.requested==candidate->dispatch[m])candidate->endpoints[m]=endpoint;
    }
    candidate->publicPsMutationCovered=true;
    observation.missingRole=api::MethodCount;observation.missingAlias=nullptr;
    if(!current())return false;
    // A newer generation stays pending for a LATER Present, even if this
    // transaction successfully installed some of that generation's targets.
    observation.publication=ps::PublishCoverage(std::move(candidate),request,*owner,installed,token);
    const bool ready=ps::Ready(observation.publication);
    observation.prepare.initializationPending=observation.publication==ps::PublicationResult::PendingRefresh;
    return observation.Boolean("publish",ready?"public-PublishCoverage":ps::publicationFailure,ready);
}

// Fixed TLS receipts only at installation/adaptation, never per draw.
// Present emits at most one summary per second and eight detail batches per
// thread lifetime. Alternating states cannot bypass either bound.
// Variant capacity bounds actual attachments; unknown/failed transactions stop.
struct PsInstallReceipt final {
    PsCacheInstallObservation observation{};
    GenshinLegacy::PsState::RefreshWitness witness{};
    bool ready=false,deferred=false,dirty=false,detailPending=false,emittedOnce=false;
    unsigned lastState=~0u,optionalPhase=0,detailBatches=0;
    uint64_t sequence=0,emitted=0,coalesced=0,refreshToken=0,publicationId=0,lastEmitMs=0;
    uint64_t attempts=0,scheduling=0,transitions=0,repeated=0,detailSuppressed=0;
    std::array<uint64_t,static_cast<unsigned>(GenshinLegacy::PsState::PublicationResult::Count)> results{};
};
static thread_local PsInstallReceipt psInstallReceipt;
static void LogPsCacheInstall(bool cacheReady,const PsCacheInstallObservation& cacheInstall,bool deferred) noexcept {
    namespace ps=GenshinLegacy::PsState;
    auto& r=psInstallReceipt;
    const auto state=cacheReady?1u:(cacheInstall.prepare.initializationPending?2u:3u);
    const auto id=ps::publicationSerial.load(std::memory_order_relaxed);
    const auto& witness=cacheReady?ps::publicationWitness:ps::refreshWitness;
    const auto bump=[](uint64_t& n) noexcept {if(n!=UINT64_MAX)++n;};
    const auto token=GenshinLegacy::PublicPsBinding::RefreshToken();
    const bool changed=r.lastState!=state || r.observation.publication!=cacheInstall.publication ||
        r.observation.publicationAttempt!=cacheInstall.publicationAttempt ||
        std::strcmp(r.observation.predicate,cacheInstall.predicate)!=0 ||
        r.witness.lastReason!=witness.lastReason || r.witness.lastRoleMask!=witness.lastRoleMask ||
        r.witness.counts!=witness.counts || r.witness.aliases!=witness.aliases ||
        r.witness.gap!=witness.gap || r.witness.refused!=witness.refused ||
        std::strcmp(r.witness.refusal,witness.refusal)!=0 || r.refreshToken!=token || r.publicationId!=id;
    // Counts include every receipt, even identical states. Initial scheduling
    // has no publication attempt and cannot inflate PendingRefresh attempts.
    bump(r.sequence);
    if(cacheInstall.publicationAttempt){bump(r.attempts);bump(r.results[static_cast<unsigned>(cacheInstall.publication)]);}
    else bump(r.scheduling);
    if(changed){
        bump(r.transitions);
        if(r.detailPending || r.detailBatches>=8)bump(r.detailSuppressed);
        r.detailPending=r.detailBatches<8;
    }else bump(r.repeated);
    if(r.dirty)bump(r.coalesced);
    r.lastState=state;r.observation=cacheInstall;r.witness=witness;
    r.ready=cacheReady;r.deferred=deferred;r.dirty=true;
    r.refreshToken=token;r.publicationId=id;
    r.optionalPhase=static_cast<unsigned>(ps::optionalInstall.Phase());
}
static void EmitPsCacheInstallReceiptImpl() noexcept {
    auto& r=psInstallReceipt;if(!r.dirty)return;
    const auto now=GetTickCount64();
    if(r.emittedOnce && now-r.lastEmitMs<1000)return;
    const bool detail=r.detailPending && r.detailBatches<8;
    const auto cacheInstall=r.observation;const bool cacheReady=r.ready,deferred=r.deferred;
    // Consume the opportunity before IO: logging failure cannot create retries.
    r.dirty=false;r.detailPending=false;r.emitted=r.sequence;r.lastEmitMs=now;r.emittedOnce=true;
    if(detail)++r.detailBatches;
    try {
        LOG_INFO("R898 public PS publication summary: thread={} receipt={} result={} publication_attempt={} refresh_token={} publication_id={} attempts={} scheduling_receipts={} pub_ReusedCoverage={} pub_ReplacedCoverage={} pub_PendingRefresh={} pub_Refused={} state_changes={} identical_receipts={} coalesced={} detail_batches={} detail_suppressed={} detail_limit=8 min_interval_ms=1000 cumulative=true complete_frame_denominator=false owner_lock_held=false no_performance_claim=true",
            GetCurrentThreadId(),r.emitted,GenshinLegacy::PsState::Metrics::PublicationName(cacheInstall.publication),
            cacheInstall.publicationAttempt,r.refreshToken,r.publicationId,r.attempts,r.scheduling,
            r.results[0],r.results[1],r.results[2],r.results[3],r.transitions,r.repeated,r.coalesced,r.detailBatches,r.detailSuppressed);
        if(!detail)return;
        const auto& prepared=cacheInstall.prepare;
        LOG_INFO("R864 public PS binding cache: ready={} fallback=getter bridge_committed=true phase={} status_kind={} status={} status_hex={:08X} predicate={} setter={} abort_attempted={} abort_status={:08X}"
            " prepare_scope={} prepare_predicate={} prepare_status_kind={} prepare_status={} prepare_status_hex={:08X} prepare_slot={} prepare_entry={} prepare_mask={} prepare_mode={} prepare_image={} prepare_patch={} prepare_section={} prepare_rva={:X}"
            " prepare_span_offset={} prepare_block_offset={} prepare_mode0_mismatch={} prepare_mode1_mismatch={} prepare_api_valid={} prepare_api_value={} prepare_requested_bytes={} prepare_read_bytes={} prepare_win32_valid={} prepare_win32={:08X} prepare_exception={} historical_native_reason={} prepare_initialized_valid={} prepare_initialized={} prepare_defer_eligible={} optional_phase={} deferred_attempt={} no_performance_claim=true",
            cacheReady,cacheInstall.phase,cacheInstall.statusKind,cacheInstall.status,static_cast<UINT>(cacheInstall.status),
            cacheInstall.predicate,cacheInstall.setter,cacheInstall.abortAttempted,static_cast<UINT>(cacheInstall.abortStatus),
            prepared.scope,prepared.predicate,prepared.statusKind,prepared.status,static_cast<UINT>(prepared.status),
            prepared.slot,prepared.entry,prepared.mask,prepared.mode,prepared.image,prepared.patch,prepared.section,prepared.rva,
            prepared.spanOffset,prepared.offset,prepared.modeMismatch[0],prepared.modeMismatch[1],
            prepared.apiValid,prepared.apiValue,prepared.requestedBytes,prepared.readBytes,
            prepared.win32Valid,static_cast<UINT>(prepared.win32),prepared.exception,prepared.historicalNativeReason,prepared.nativeInitializedValid,prepared.nativeInitialized,prepared.initializationPending,
            r.optionalPhase,deferred);
        std::string batch;
        for(unsigned m=0;m<cacheInstall.batchDispatch.size();++m)
            batch+=std::format(" {}=dispatch:{:X}/aliases:{}/targets:{}/attaches:{}",GenshinLegacy::PsState::Metrics::RoleName(m),
                reinterpret_cast<uintptr_t>(cacheInstall.batchDispatch[m]),cacheInstall.batchAliases[m],cacheInstall.batchTargets[m],cacheInstall.batchAttaches[m]);
        LOG_INFO("R915 public PS coverage batch: receipt={} frozen_token={} current_token={} missing_role={} missing_alias={:X} plan={} detail_limit=8 min_interval_ms=1000 owner_lock_held=false no_performance_claim=true",
            r.emitted,cacheInstall.batchToken,r.refreshToken,GenshinLegacy::PsState::Metrics::RoleName(cacheInstall.missingRole),
            reinterpret_cast<uintptr_t>(cacheInstall.missingAlias),batch);
        std::string aliases;
        for(unsigned m=0;m<r.witness.aliases.size();++m){
            aliases+=std::format(" {}=[",GenshinLegacy::PsState::Metrics::RoleName(m));
            for(unsigned n=0;n<r.witness.counts[m] && n<r.witness.aliases[m].size();++n)
                aliases+=std::format("{}{:X}",n?",":"",reinterpret_cast<uintptr_t>(r.witness.aliases[m][n]));
            aliases+="]";
        }
        LOG_INFO("R898 public PS publication transition: receipt={} coalesced={} result={} refresh_token={} publication_id={} publication_attempt={} reason={} first_role={} actual_role_mask={:X} witness_gap={} witness_refused={} witness_refusal={} observed_aliases={} receipt_state_at_attempt=true owner_lock_held=false no_performance_claim=true",
            r.emitted,r.coalesced,GenshinLegacy::PsState::Metrics::PublicationName(cacheInstall.publication),r.refreshToken,r.publicationId,cacheInstall.publicationAttempt,
            GenshinLegacy::PsState::Metrics::RefreshName(r.witness.lastReason),GenshinLegacy::PsState::Metrics::RoleName(r.witness.lastRole),
            r.witness.lastRoleMask,r.witness.gap,r.witness.refused,r.witness.refusal,aliases);
    }catch(...){} // Logging never changes cache or GPU authority.
}

// Called synchronously ONLY by GenshinLegacy::OnPresent's private owner scope.
// nativeMutex -> QueryInstallAccess -> brief lifecycle sections -> owned hook transaction.
// No lifecycle mutex spans any COM call, peer suspension, transaction or callback.
static void InstallDeferredPsCacheAtOwner(const GenshinLegacy::PsState::InstallOwnerScope& owner) noexcept
{
    namespace ps=GenshinLegacy::PsState;
    QueryInstallAccess install;if(!install)return; // Do not consume/duplicate an attempt while busy.
    auto request=ps::optionalInstall.Pending();if(!request)return;
    PsCacheInstallObservation observation;observation.publicationAttempt=true;
    const auto refuse=[&](){ps::RefusePublicCoverage();ps::optionalInstall.Finish(request,false);
        ps::Metrics::PublicationEvent(ps::PublicationResult::Refused,GenshinLegacy::PublicPsBinding::RefreshToken(),
            ps::publicationSerial.load(std::memory_order_relaxed),request->context.Get(),request->deviceEpoch);
        LogPsCacheInstall(false,observation,true);};
    if(!observation.Boolean("owner","scoped-owner-current",owner.Current()) ||
       !observation.Boolean("owner","mandatory-bridge-current",g_bridgeHooksAttached && g_genshinInstalled.load()) ||
       !observation.Boolean("owner","same-context",owner.Context()==request->context.Get()) ||
       !observation.Boolean("owner","same-device",owner.Device()==request->device.Get()) ||
       !observation.Boolean("owner","public-cache-not-stopped",!GenshinLegacy::PublicPsBinding::stopped.load(std::memory_order_acquire))){refuse();return;}
    // Record the first live SR owner, never the factory caller or a Status snapshot.
    // Until publication this retains the real CPU observation owner only.
    if(!request->owner){
        HANDLE thread=nullptr;
        if(!observation.Boolean("owner","DuplicateHandle-current-thread",DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),
            GetCurrentProcess(),&thread,SYNCHRONIZE|THREAD_QUERY_LIMITED_INFORMATION,FALSE,0)!=FALSE)){refuse();return;}
        request->thread=thread;request->threadId=GetCurrentThreadId();request->owner=owner.Owner();
        request->ownerEpoch=owner.OwnerEpoch();request->deviceEpoch=owner.DeviceEpoch();request->controlsEpoch=owner.ControlsEpoch();
    }
    if(!observation.Boolean("owner","same-owner-and-epochs",request->owner==owner.Owner() &&
        request->ownerEpoch==owner.OwnerEpoch() && request->deviceEpoch==owner.DeviceEpoch() &&
        request->threadId==GetCurrentThreadId())){refuse();return;}
    request->controlsEpoch=owner.ControlsEpoch();
    if(!observation.Boolean("owner","retained-thread-process",GetProcessIdOfThread(request->thread)==GetCurrentProcessId()) ||
       !observation.Boolean("owner","retained-thread-identity",GetThreadId(request->thread)==GetCurrentThreadId()) ||
       !observation.Boolean("owner","retained-thread-live",WaitForSingleObject(request->thread,0)==WAIT_TIMEOUT)){
        refuse();return;
    }
    if(!ps::optionalInstall.ClaimDeferred(request))return;
    const bool ready=InstallPsStateCache(request->context.Get(),request->device.Get(),observation,request,&owner);
    if(!ready && !observation.prepare.initializationPending)ps::RefusePublicCoverage();
    ps::optionalInstall.Finish(request,!ready && observation.prepare.initializationPending);
    ps::Metrics::PublicationEvent(observation.publication,GenshinLegacy::PublicPsBinding::RefreshToken(),
        ps::publicationSerial.load(std::memory_order_relaxed),request->context.Get(),request->deviceEpoch);
    // A newer public generation/vector may defer; capacity and transaction failures terminate.
    LogPsCacheInstall(ready,observation,true);
}

} // namespace

namespace GenericDepthDx11
{
void EmitPsCacheInstallReceipt() noexcept {EmitPsCacheInstallReceiptImpl();}
bool ModInternal() noexcept { return TrackingInternal(); }
void BindModRuntime(ID3D11Device* device) noexcept
{
    if(!gimi::runtime::Enabled() || !device || !g_context)return;
    auto hooks=kModHooks;hooks.draw=o_Draw;hooks.drawIndexed=o_DrawIndexed;
    hooks.drawIndexedInstanced=o_DrawIndexedInstanced;
    gimi::runtime::DeviceCreated(device,g_context,hooks,
        g_installed.load() && g_bridgeHooksAttached && g_genshinInstalled.load() && g_modWriteHooksComplete.load());
    gimi::runtime::StreamOutputCoverage(device,g_context,ModStreamHooksCover(g_context));
}
void CompleteModCreationHookInstall(ID3D11Device* device) noexcept
{
    if(!gimi::runtime::Enabled() || !device || device!=g_contextDevice || !g_context)return;
    // Hold the exact pair across the claim/transaction. No runtime owner/request
    // mutex spans the existing install serialization or an owned hook batch.
    Microsoft::WRL::ComPtr<ID3D11Device> retainedDevice=device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context=g_context;
    const auto request=gimi::runtime::ClaimCreationHooks(retainedDevice.Get(),context.Get());
    if(!request.revision)return;
    LONG result=ERROR_BUSY;
    {
        QueryInstallAccess install;
        if(install)result=retainedDevice.Get()==g_contextDevice && context.Get()==g_context ?
            D3D11Hooks::InstallGimiCreationHooks(retainedDevice.Get()) : ERROR_INVALID_HANDLE;
    }
    gimi::runtime::CompleteCreationHooks(retainedDevice.Get(),context.Get(),request,result);
}
void RevokeModStreamOutputCoverage() noexcept
{
    // Called before the existing StopAccepting/native-owner revoke at unload.
    // The process-lifetime context hooks retain their original trampolines,
    // like the seven common draw hooks; do not detach an unowned/foreign path.
    // A terminal publication never permits a new attach/re-arm in this owner.
    if(g_modSOHookCommitted.exchange(false,std::memory_order_acq_rel) && gimi::runtime::Enabled())
        gimi::runtime::StreamOutputCoverage(g_modSODevice,g_modSOContext,false);
}
bool Installed() { return g_installed; }
bool InstallFailed() { return g_installFailed; }

HRESULT NoteDeferredContextBirth(ID3D11Device* device,ID3D11DeviceContext* context,unsigned apiVersion,
    uint64_t entryTick,uint64_t returnTick,const void* caller,const void* executed) noexcept
{
    if(!device || !context || apiVersion>3 || context->GetType()!=D3D11_DEVICE_CONTEXT_DEFERRED)return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D11Device> owner;context->GetDevice(&owner);
    Microsoft::WRL::ComPtr<IUnknown> expected,actual;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&expected))) || !owner ||
       FAILED(owner->QueryInterface(IID_PPV_ARGS(&actual))) || expected.Get()!=actual.Get())return E_INVALIDARG;
    // A real deferred birth is observed, but its Begin/End/state entries are
    // not this adapter's immediate set. Never seed or install that set on it.
    return GenshinQueryScope::NoteContextBirth(device,context,
        static_cast<GenshinQueryScope::ContextBirthKind>(static_cast<unsigned>(GenshinQueryScope::ContextBirthKind::Deferred)+apiVersion),
        entryTick,returnTick,caller,executed);
}

bool PrepareCreatedContextQueries(ID3D11Device* device,GenshinQueryScope::CreationWitness& birth)
{
    const auto fail=[&](){birth.Cancel();return false;};
    if(!device)return fail();
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    if(!birth.Matches(device,context.Get()))return fail();
    // The issuer has authenticated Genshin and native creation. Complete the
    // existing hook transaction before any initializer can publish/use these
    // outputs: query-only hooks would miss opaque command-list/state work.
    Install(device,true);
    // Busy/failed/incomplete installs consume this birth without seeding it.
    // Do not ask QueryHooksCover to certify a hook set not yet published: that
    // would poison process-wide coverage even for a later, genuinely new birth.
    if(!g_genshinInstalled.load())return fail();
    // Install is also a late-attach entry and NEVER seeds fresh by itself.
    // Earlier observation creates UnknownPast, which Seed refuses to replace.
    return GenshinQueryScope::SeedCreatedContext(birth,device,context.Get());
}

void Install(ID3D11Device* device, bool genshinBridge)
{
    const bool depthFinder = Config::Instance()->DlssNrNativeDepthFinder.value_or_default();
    if (device == nullptr || (!depthFinder && !genshinBridge))
        return;
    if (genshinBridge)
        LOG_INFO("Genshin CPU writer CB hook install begin: pid={} depth_finder={} already_installed={} immediate_device={}",
            GetCurrentProcessId(), depthFinder, g_installed.load(), reinterpret_cast<uintptr_t>(device));
    QueryInstallAccess install;
    if (!install) return;
    const bool installCommon = !g_installed.load();
    const bool installBridge = genshinBridge && !g_bridgeHooksAttached;
    if (!installCommon && !installBridge)
    {
        // A repeat is a coverage refresh, never another attach or a live
        // trampoline reassignment. Bind also checks the current actual table.
        if(gimi::runtime::Enabled() && g_contextDevice && g_context)
            gimi::runtime::StreamOutputCoverage(g_contextDevice,g_context,ModStreamHooksCover(g_context));
        return;
    }
    // No failed/aborted mandatory transaction may retain SO admission.
    if(installBridge && gimi::runtime::Enabled() && g_contextDevice && g_context)
        gimi::runtime::StreamOutputCoverage(g_contextDevice,g_context,false);

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    if (context == nullptr)
    {
        LOG_WARN("Depth finder (D3D11): could not get the immediate context, not installed");
        g_installFailed = true;
        return;
    }

    ID3D11DeviceContext* realContext = nullptr;

    if (!Util::CheckForRealObject(__FUNCTION__, context, (IUnknown**) &realContext))
        realContext = context;

    PVOID* table = *(PVOID**) realContext;

    std::array<PVOID,kCommonHookSlots.size()> commonTargets {};
    for(size_t i=0;i<kCommonHookSlots.size();++i)
    {
        commonTargets[i]=table[kCommonHookSlots[i]];
        if(!installCommon && commonTargets[i]!=g_commonHookTargets[i])
        {
            // Never reuse another runtime/layer's partial hook coverage and
            // never overwrite already executing common originals on upgrade.
            LOG_WARN("D3D11 bridge hook upgrade refused: common target mismatch at slot {}",kCommonHookSlots[i]);
            context->Release();g_installFailed=true;return;
        }
    }
    if(installCommon)
    {
        o_DrawIndexed = (PFN_DrawIndexed) table[kDrawIndexed];
        o_Draw = (PFN_Draw) table[kDraw];
        o_DrawIndexedInstanced = (PFN_DrawIndexedInstanced) table[kDrawIndexedInstanced];
        o_DrawInstanced = (PFN_DrawInstanced) table[kDrawInstanced];
        o_OMSetRenderTargets = (PFN_OMSetRenderTargets) table[kOMSetRenderTargets];
        o_OMSetRenderTargetsAndUAV = (PFN_OMSetRenderTargetsAndUnorderedAccessViews) table[kOMSetRenderTargetsAndUAV];
        o_DrawAuto = (PFN_DrawAuto) table[kDrawAuto];
        o_DrawIndexedInstancedIndirect = (PFN_DrawIndirect) table[kDrawIndexedInstancedIndirect];
        o_DrawInstancedIndirect = (PFN_DrawIndirect) table[kDrawInstancedIndirect];
        o_Dispatch = (PFN_Dispatch) table[kDispatch];
        o_RSSetViewports = (PFN_RSSetViewports) table[kRSSetViewports];
        o_ClearDepthStencilView = (PFN_ClearDepthStencilView) table[kClearDepthStencilView];
    }

    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    std::array<void*,5> stateSwapTargets{};bool stateSwapCovered=false;
    std::array<uintptr_t,2> stateSwapReturns{};
    GenshinNativeQuery::detail::ContextTableLeaf stateSwapObservation;
    bool stateSwapAttempted=false;
    bool modSONativeAliases=false;
    PVOID* deviceTable = nullptr;
    PVOID* context1Table = nullptr;
    PVOID* device1Table = nullptr;
    // Bridge ownership requires 11.1 state isolation. Without it SR already rejects StateUnavailable.
    if (installBridge)
    {
        deviceTable=*reinterpret_cast<PVOID**>(device);
        o_CreateDepthCs=reinterpret_cast<PFN_CreateDepthCs>(deviceTable[18]);
        o_DepthDispatchIndirect=reinterpret_cast<PFN_DrawIndirect>(table[42]);
        o_DepthMap=reinterpret_cast<PFN_Map>(table[14]);o_DepthUnmap=reinterpret_cast<PFN_Unmap>(table[15]);
        o_VSSetConstantBuffers=reinterpret_cast<PFN_SetConstantBuffers>(table[kVSSetConstantBuffers]);
        o_PSSetConstantBuffers=reinterpret_cast<PFN_SetConstantBuffers>(table[kPSSetConstantBuffers]);
        o_CSSetConstantBuffers=reinterpret_cast<PFN_SetConstantBuffers>(table[kCSSetConstantBuffers]);
        o_DepthCopyRegion=reinterpret_cast<PFN_CopyRegion>(table[46]);o_DepthCopyResource=reinterpret_cast<PFN_CopyResource>(table[47]);
        o_DepthCopyStructure=reinterpret_cast<PFN_CopyStructure>(table[49]);
        o_DepthUpdate=reinterpret_cast<PFN_Update>(table[48]);o_DepthMips=reinterpret_cast<PFN_Mips>(table[54]);o_DepthResolve=reinterpret_cast<PFN_Resolve>(table[57]);
        o_ClearRtv=reinterpret_cast<PFN_ClearRtv>(table[50]);
        o_ClearUavUint=reinterpret_cast<PFN_ClearUavUint>(table[51]);
        o_ClearUavFloat=reinterpret_cast<PFN_ClearUavFloat>(table[52]);
        GenshinQueryScope::MappingHooksIncomplete(); // incomplete until the entire transaction commits
        o_QueryBegin = reinterpret_cast<PFN_QueryRange>(table[27]);
        o_QueryEnd = reinterpret_cast<PFN_QueryRange>(table[28]);
        o_ClearState = reinterpret_cast<PFN_ClearState>(table[110]);
        o_ExecuteCommandList = reinterpret_cast<PFN_ExecuteCommandList>(table[58]);
        o_FinishCommandList = reinterpret_cast<PFN_FinishCommandList>(table[114]);
        if (SUCCEEDED(realContext->QueryInterface(IID_PPV_ARGS(&context1))))
        {
            const auto table1=context1Table=*reinterpret_cast<PVOID**>(context1.Get());
            o_DepthCopyRegion1=reinterpret_cast<PFN_CopyRegion1>(table1[115]);
            o_DepthUpdate1=reinterpret_cast<PFN_Update1>(table1[116]);
            o_DepthClearView=reinterpret_cast<PFN_ClearView>(table1[132]);
            o_DiscardResource = reinterpret_cast<PFN_DiscardResource>(table1[117]);
            o_DiscardView = reinterpret_cast<PFN_DiscardView>(table1[118]);
            o_SwapDeviceContextState = reinterpret_cast<PFN_SwapDeviceContextState>(table1[131]);
            o_DiscardView1 = reinterpret_cast<PFN_DiscardView1>(table1[133]);
        }
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device1))))
        {
            device1Table=*reinterpret_cast<PVOID**>(device1.Get());
            o_CreateContextState=reinterpret_cast<PFN_CreateContextState>(device1Table[47]);
        }
        if(context1 && device1){
            stateSwapAttempted=true;
            if(GenshinNativeQuery::StateSwapHookTargets(realContext,stateSwapTargets,stateSwapReturns,&stateSwapObservation)){
                o_StateQueryBegin=reinterpret_cast<PFN_QueryRange>(stateSwapTargets[0]);
                o_StateQueryEnd=reinterpret_cast<PFN_QueryRange>(stateSwapTargets[1]);
                o_StateExecute=reinterpret_cast<PFN_ExecuteCommandList>(stateSwapTargets[2]);
                o_StateClear=reinterpret_cast<PFN_ClearState>(stateSwapTargets[3]);
                o_StateFinish=reinterpret_cast<PFN_FinishCommandList>(stateSwapTargets[4]);
                g_stateBeginReturn=stateSwapReturns[0];g_stateEndReturn=stateSwapReturns[1];stateSwapCovered=true;
            }
        }
        // All supported public context versions must reach the same actual
        // identity/table. This optional SO qualification never blocks SR or
        // changes the query birth/recipe predicates.
        modSONativeAliases=context==realContext && context1 &&
            static_cast<ID3D11DeviceContext*>(context1.Get())==realContext &&
            ModStreamAliasMatches<ID3D11DeviceContext>(realContext) &&
            ModStreamAliasMatches<ID3D11DeviceContext2>(realContext) &&
            ModStreamAliasMatches<ID3D11DeviceContext3>(realContext) &&
            ModStreamAliasMatches<ID3D11DeviceContext4>(realContext);
    }

    const auto beginResult=GenshinNativeQuery::Begin();
    if (beginResult!=NO_ERROR)
    { context->Release(); if(beginResult!=ERROR_RETRY)g_installFailed=true; return; }
    if (DetourUpdateThread(GetCurrentThread())!=NO_ERROR)
    { GenshinNativeQuery::Abort(); context->Release(); g_installFailed=true; return; }
    LONG hookError=NO_ERROR;
    const auto attach=[&](PVOID* original,PVOID hook,PVOID* methodTable)
    {
        // Classify each original against its actual queried interface table.
        // Native state-swap recipe targets have no application proxy table.
        const auto result=AttachDepthMethod(original,hook,methodTable);
        if (result!=NO_ERROR && hookError==NO_ERROR) hookError=result;
    };

    if(installCommon)
    {
        attach(&(PVOID&) o_DrawIndexed, hkDrawIndexed,table);
        attach(&(PVOID&) o_Draw, hkDraw,table);
        attach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced,table);
        attach(&(PVOID&) o_DrawInstanced, hkDrawInstanced,table);
        attach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets,table);
        attach(&(PVOID&) o_OMSetRenderTargetsAndUAV, hkOMSetRenderTargetsAndUAV,table);
        attach(&(PVOID&) o_DrawAuto, hkDrawAuto,table);
        attach(&(PVOID&) o_DrawIndexedInstancedIndirect, hkDrawIndexedInstancedIndirect,table);
        attach(&(PVOID&) o_DrawInstancedIndirect, hkDrawInstancedIndirect,table);
        attach(&(PVOID&) o_Dispatch, hkDispatch,table);
        attach(&(PVOID&) o_RSSetViewports, hkRSSetViewports,table);
        attach(&(PVOID&) o_ClearDepthStencilView, hkClearDepthStencilView,table);
    }

    if (installBridge)
    {
        attach(&(PVOID&) o_CreateDepthCs,hkCreateDepthCs,deviceTable);
        attach(&(PVOID&) o_DepthDispatchIndirect,hkDepthDispatchIndirect,table);
        attach(&(PVOID&) o_DepthMap,hkDepthMap,table);attach(&(PVOID&) o_DepthUnmap,hkDepthUnmap,table);
        attach(&(PVOID&) o_VSSetConstantBuffers,hkVSSetConstantBuffers,table);
        attach(&(PVOID&) o_PSSetConstantBuffers,hkPSSetConstantBuffers,table);
        attach(&(PVOID&) o_CSSetConstantBuffers,hkCSSetConstantBuffers,table);
        attach(&(PVOID&) o_DepthCopyRegion,hkDepthCopyRegion,table);attach(&(PVOID&) o_DepthCopyResource,hkDepthCopyResource,table);
        attach(&(PVOID&) o_DepthCopyStructure,hkDepthCopyStructure,table);
        attach(&(PVOID&) o_DepthUpdate,hkDepthUpdate,table);attach(&(PVOID&) o_DepthMips,hkDepthMips,table);attach(&(PVOID&) o_DepthResolve,hkDepthResolve,table);
        attach(&(PVOID&) o_ClearRtv,hkClearRtv,table);
        attach(&(PVOID&) o_ClearUavUint,hkClearUavUint,table);
        attach(&(PVOID&) o_ClearUavFloat,hkClearUavFloat,table);
        attach(&(PVOID&) o_QueryBegin,hkQueryBegin,table);
        attach(&(PVOID&) o_QueryEnd,hkQueryEnd,table);
        attach(&(PVOID&) o_ClearState,hkClearState,table);
        attach(&(PVOID&) o_ExecuteCommandList,hkExecuteCommandList,table);
        attach(&(PVOID&) o_FinishCommandList,hkFinishCommandList,table);
        if(stateSwapCovered){
            attach(&(PVOID&) o_StateQueryBegin,hkStateQueryBegin,nullptr);attach(&(PVOID&) o_StateQueryEnd,hkStateQueryEnd,nullptr);
            attach(&(PVOID&) o_StateExecute,hkStateExecute,nullptr);attach(&(PVOID&) o_StateClear,hkStateClear,nullptr);
            attach(&(PVOID&) o_StateFinish,hkStateFinish,nullptr);
        }
        if (device1) attach(&(PVOID&) o_CreateContextState,hkCreateContextState,device1Table);
        if (context1)
        {
            attach(&(PVOID&) o_DepthCopyRegion1,hkDepthCopyRegion1,context1Table);
            attach(&(PVOID&) o_DepthUpdate1,hkDepthUpdate1,context1Table);
            attach(&(PVOID&) o_DepthClearView,hkDepthClearView,context1Table);
            attach(&(PVOID&) o_DiscardResource,hkDiscardResource,context1Table);
            attach(&(PVOID&) o_DiscardView,hkDiscardView,context1Table);
            attach(&(PVOID&) o_SwapDeviceContextState,hkSwapDeviceContextState,context1Table);
            attach(&(PVOID&) o_DiscardView1,hkDiscardView1,context1Table);
        }
    }
    if (hookError!=NO_ERROR)
    {
        GenshinNativeQuery::Abort();
        LOG_ERROR("D3D11 context hook set incomplete ({:X}); Genshin capture disabled",(UINT)hookError);
        context->Release(); g_installFailed=true; return;
    }
    UpgradePeers upgradePeers;
    if(!installCommon && installBridge)
    {
        // No provenance/signature/file/logging/COM work while peers are
        // suspended. Detours resumes them on both commit and abort.
        auto peerResult=upgradePeers.Prepare();
        if(peerResult==NO_ERROR)peerResult=upgradePeers.Enlist();
        if(peerResult!=NO_ERROR)
        {
            const auto abortResult=GenshinNativeQuery::Abort();
            LOG_WARN("D3D11 bridge hook upgrade peer enlist refused ({:X}), abort={:X}",
                     static_cast<UINT>(peerResult),static_cast<UINT>(abortResult));
            context->Release();g_installFailed=true;return;
        }
    }
    const auto result = GenshinNativeQuery::Commit();

    if (result != NO_ERROR)
    {
        LOG_ERROR("Depth finder (D3D11): hooking failed ({:X}), not installed", (UINT) result);
        context->Release();
        g_installFailed = true;
        return;
    }

    // Install itself never seeds/promotes a context: late DXGI capture and
    // FollowPresentDevice remain UnknownPast. Without 11.1 opaque-state
    // coverage, optional mapping stays refused while ordinary SR continues.
    if (installBridge)
    {
        g_bridgeHooksAttached = true;
        g_modWriteHooksComplete.store(context1 && device1 && stateSwapCovered);
    }
    if (installBridge && context1 && device1) {
        const auto table1=*reinterpret_cast<PVOID**>(context1.Get());
        const auto deviceTable1=*reinterpret_cast<PVOID**>(device1.Get());
        GenshinQueryScope::HooksCommitted(table[27],table[28],table[58],table[114],table1[131],deviceTable1[47],
            stateSwapCovered?&stateSwapTargets:nullptr,table[110]);
        LOG_INFO("R816 state-swap query coverage: pid={} alternate={} begin={:X} end={:X} no_visual_claim=true",
            GetCurrentProcessId(),stateSwapCovered,reinterpret_cast<uintptr_t>(stateSwapTargets[0]),reinterpret_cast<uintptr_t>(stateSwapTargets[1]));
        if(!stateSwapCovered){const auto& o=stateSwapObservation;
            LOG_WARN("R1024 state-swap target refusal: pid={} attempted={} context={:X} scope={} predicate={} status_kind={} status={} slot={} rva={:X} offset={} span_offset={} read_bytes={} requested_bytes={} observation_only=true no_authority=true",
                GetCurrentProcessId(),stateSwapAttempted,reinterpret_cast<uintptr_t>(realContext),o.scope,o.predicate,o.statusKind,o.status,o.slot,o.rva,o.offset,o.spanOffset,o.readBytes,o.requestedBytes);
        }
        g_genshinInstalled = true;
    }
    if(installCommon)
    {
        g_commonHookTargets = commonTargets;
        g_hookedOMSetRenderTargets = table[kOMSetRenderTargets];
        UseContext(realContext); // holds its own reference; no reset during a bridge upgrade
        if (depthFinder) g_core.Start([](const std::string& line) { LOG_INFO("{}", line); });
        g_installed = true;
    }
    // Core originals/coverage are already committed and published. An SO
    // install error is reported separately and cannot set g_installFailed or
    // roll back that mandatory batch. No repeated/per-Present install retry.
    if(installBridge && g_genshinInstalled.load())
    {
        const auto soResult=InstallOptionalModStreamOutput(device,realContext,table,context1Table,modSONativeAliases);
        LOG_INFO("R608 optional GIMI SO install: result={:X} committed={} native_revoked={} core_committed=true no_runtime_claim=true",
            static_cast<UINT>(soResult),g_modSOHookCommitted.load(),
            GenshinNativeQuery::detail::store.revoked.load(std::memory_order_acquire));
    }
    // Retain the exact initial request, but do not install/publish its epoch
    // before HookToDeviceLocal finishes its remaining owned device hooks.
    // Query birth seeding still follows the committed mandatory context hooks.
    if(installBridge && g_genshinInstalled.load())
    {
        namespace ps=GenshinLegacy::PsState;
        PsCacheInstallObservation cacheInstall;
        std::shared_ptr<ps::OptionalInstallRequest> request;
        // An unwrapped context cannot grant coverage to an application proxy.
        if(cacheInstall.Boolean("prepare","application-context-is-native",context==realContext)){
            request=ps::optionalInstall.Begin(realContext,device);
            if(cacheInstall.Boolean("lifetime","OptionalInstallRequest-nonnull",static_cast<bool>(request)))
                ps::optionalOwnerConsumer.store(&InstallDeferredPsCacheAtOwner,std::memory_order_release);
        }
        if(!request)LogPsCacheInstall(false,cacheInstall,false);
    }
    context->Release();

    // A committed common/partial set must not claim complete bridge coverage.
    g_installFailed = installBridge && !g_genshinInstalled.load();
    if (genshinBridge)
        LOG_INFO("Genshin CPU writer CB hook install committed: pid={} vs={} ps={} cs={} query_begin={} query_end={} clear_state={} execute_command_list={} finish_command_list={} context1={} device1={} semantic=UNPROVED",
            GetCurrentProcessId(), o_VSSetConstantBuffers != nullptr, o_PSSetConstantBuffers != nullptr,
            o_CSSetConstantBuffers != nullptr, o_QueryBegin != nullptr, o_QueryEnd != nullptr,
            o_ClearState != nullptr, o_ExecuteCommandList != nullptr, o_FinishCommandList != nullptr,
            context1 != nullptr, device1 != nullptr);
    LOG_INFO("Depth finder (D3D11): observing the game's depth buffers (immediate context only), after {} frames of "
             "warm-up; it stands down if the game makes an upscaler call",
             Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());
}

// Called at the existing device-hook tail, including its sampler-already-
// installed return. It schedules, but does not execute, the optional CPU attach.
void CompletePsStateCacheInstall(ID3D11Device* device) noexcept
{
    namespace ps=GenshinLegacy::PsState;
    auto request=ps::optionalInstall.ClaimInitial(device);if(!request)return;
    QueryInstallAccess install;
    PsCacheInstallObservation cacheInstall;
    // Retain the original install serialization without leaving a claimed
    // request permanently Attempting on contention. There is no retry loop.
    // Defer the optional CPU hooks to the existing, locked Present owner.
    // Device-hook completion is scheduling only; no GPU/native setup is retried.
    const bool cacheReady=false;
    const bool mayDefer=cacheInstall.Boolean("lifetime","public-owner-publication-pending",static_cast<bool>(install));
    cacheInstall.prepare.initializationPending=mayDefer;
    cacheInstall.publication=mayDefer?ps::PublicationResult::PendingRefresh:ps::PublicationResult::Refused;
    ps::optionalInstall.Finish(request,mayDefer);
    LogPsCacheInstall(cacheReady,cacheInstall,false); // scheduling receipt, not a publication attempt counter
}

void PrepareModPresent(IDXGISwapChain* swapChain) noexcept
{
    if (!gimi::runtime::NeedsPresent() || !swapChain) return;
    try
    {
        // Use this exact native chain's owners, never State's backend device or
        // the previously watched context. OnPresent keeps its existing device
        // following/depth accounting; it is not called early a second time.
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device ||
            FAILED(swapChain->GetDesc(&desc))) return;
        device->GetImmediateContext(&context);
        if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
        if (g_installed.load() && gimi::runtime::Enabled() &&
            device.Get() == g_contextDevice && context.Get() == g_context)
        {
            BindModRuntime(device.Get());
            CompleteModCreationHookInstall(device.Get());
        }
        gimi::runtime::Present(device.Get(), context.Get(), desc.BufferDesc.Width, desc.BufferDesc.Height);
    }
    catch (...) { gimi::runtime::StopAccepting(); }
}

void OnPresent(IDXGISwapChain* swapChain)
{
    // Drain the existing diagnostics once per frame instead of every draw.
    if(swapChain && GenshinIndirectObservationEnabled())LogGenshinQueryHealth();
    // CPU binding publication follows the public context owner. Native recipe
    // revocation still disables all of its own GPU/query consumers unchanged.

    DXGI_SWAP_CHAIN_DESC desc {};
    const bool havePresentDevice=g_installed.load() && swapChain && SUCCEEDED(swapChain->GetDesc(&desc));
    // Follow first: binding/installing against the previous frame's device
    // would publish creation coverage for an owner this Present just replaced.
    if(havePresentDevice)FollowPresentDevice(swapChain);
    if(gimi::runtime::NeedsPresent()){
        if(havePresentDevice && gimi::runtime::Enabled() && g_contextDevice && g_context){
            BindModRuntime(g_contextDevice);
            CompleteModCreationHookInstall(g_contextDevice);
        }
        // Invalid/null Present still retires retained submitted owners, without
        // loading a plan or installing against a stale watched device.
        gimi::runtime::Present(havePresentDevice?g_contextDevice:nullptr,havePresentDevice?g_context:nullptr,
                               desc.BufferDesc.Width,desc.BufferDesc.Height);
    }
    if (!havePresentDevice)
        return;

    DrawObservationDx11::Present(g_context,State::Instance().frameCount);
    DrawUiMask::Present(swapChain,g_context);
    if(g_context){ObserveSelectedTiming(g_context,true);GenshinDepthSemantic::Poll(g_context);}

    // The copy taken is NOT forgotten here: the mid-frame "unbind" events that call TakeSnapshot happen while the game renders
    // the frame that is only now finishing (well before this Present call), so forgetting it at the top of this same function
    // would wipe out the copy they just took, before anything downstream (BestSnapshot, the diagnostic below) ever reads it. It
    // is instead let go below when the finder stands down, and kept fresh by every TakeSnapshot (a buffer not copied leaves no
    // copy).

    // The overlay preview (not yet drawn for D3D11) or the motion step needs the depth copy taken.
    g_core.SetSnapshotsWanted((Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                               Config::Instance()->DlssNrNativeDebugView.value_or_default()) ||
                              Config::Instance()->DlssNrNativeMotion.value_or_default());

    // A game that neither re-clears nor unbinds the picked buffer before Present (it stays bound across many frames) would
    // otherwise never offer a copy: the immediate context has no "list closes" moment the way a D3D12 command list does, so this
    // is its equivalent, once per presented frame.
    if (g_context != nullptr)
    {
        const auto request = g_core.FlushForPresent((uint64_t) (size_t) g_context);

        if (request.take)
            TakeSnapshot(g_context, (ID3D11Resource*) (size_t) request.id, "present");
    }

    g_core.BeginPresent(desc.BufferDesc.Width, desc.BufferDesc.Height);
    const bool stoodDown =
        g_core.EndPresent(desc.BufferDesc.Width, desc.BufferDesc.Height,
                          Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());

    // Self-diagnosis: a pick that never gets a copy despite wanting one is otherwise invisible until someone asks why the trust
    // mask never runs.
    static uint64_t noCopyStreak = 0;
    bool copyTakenNow = false;
    {
        GenshinHookCpuTiming::Scope r448StateWait(GenshinHookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
    GenshinHookCpuTiming::Scope r448StateHeld(GenshinHookCpuTiming::Stage::StateLocked);
        copyTakenNow = g_copy.Taken();
    }
    const bool stillNoCopy = g_core.Armed() && g_core.CurrentPick().valid && !copyTakenNow &&
                            (Config::Instance()->DlssNrNativeMotion.value_or_default() ||
                             (Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                              Config::Instance()->DlssNrNativeDebugView.value_or_default()));

    if (stillNoCopy)
    {
        if (++noCopyStreak % 300 == 0)
        {
            const auto pick = g_core.CurrentPick();
            const auto diag = g_core.Diagnose((uint64_t) (size_t) g_context);
            LOG_WARN("Depth finder (D3D11): {} frames wanting a copy with no copy taken. Picked {:X}. Context: {}, {}, "
                     "bound to {:X}, this stretch {} vertices / {} draws (floor {}), wanted={}",
                     noCopyStreak, pick.id, diag.hasContext ? "known" : "UNKNOWN",
                     diag.hasBoundBuffer ? "has a bound depth buffer" : "NOTHING BOUND", diag.boundResource,
                     diag.currentVertices, diag.currentDrawcalls, g_core.SnapshotFloor(), diag.wanted);
        }
    }
    else
    {
        noCopyStreak = 0;
    }

    // Stood down: nothing is copied while the game calls an upscaler, so what the copies need (and the device it was made on)
    // is let go until the finder wakes.
    if (stoodDown)
    {
        GenshinHookCpuTiming::Scope r448StateWait(GenshinHookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
    GenshinHookCpuTiming::Scope r448StateHeld(GenshinHookCpuTiming::Stage::StateLocked);
        g_copy.Release();
    }
}

GenericDepthSelect::Pick CurrentPick() { return g_core.CurrentPick(); }

Snapshot BestSnapshot()
{
    GenshinHookCpuTiming::Scope r448StateWait(GenshinHookCpuTiming::Stage::StateLock);
    std::lock_guard lock(g_mutex);r448StateWait.Finish();
    GenshinHookCpuTiming::Scope r448StateHeld(GenshinHookCpuTiming::Stage::StateLocked);
    const auto pick = g_core.CurrentPick();

    Snapshot snap;

    if (g_copy.Taken() && pick.valid)
    {
        snap.valid = true;
        snap.resource = g_copy.Copy();
        snap.typelessFormat = DXGI_FORMAT_R32_FLOAT;
        snap.viewFormat = DXGI_FORMAT_R32_FLOAT;
        snap.width = g_copy.Width();
        snap.height = g_copy.Height();
        snap.reversed = pick.reversed;
    }

    return snap;
}

void NoteUpscalerCall()
{
    // Noted with no finder installed too: the menu and the frame source must know the game has an upscaler either way.
    native::NoteGameUpscalerCall();

    if (g_installed && !native::OpticalFgNrEnabled())
        g_core.NoteUpscalerCall();
}

bool GameCallsUpscaler() { return native::GameUpscalerCalledRecently() || (g_installed && g_core.GameCallsUpscaler()); }
bool Armed() { return g_installed && g_core.Armed(); }

void DrawStatus()
{
    const bool wanted = Config::Instance()->DlssNrNativeDepthFinder.value_or_default();

    switch (DlssNrNativeMode::FinderFor(wanted, g_installed, g_installFailed))
    {
    case DlssNrNativeMode::Finder::Off:
        ImGui::TextDisabled("深度：关闭。NR 仅使用运动信息，不因游戏超分而让出控制。");
        return;
    case DlssNrNativeMode::Finder::NeedsRestart:
        ImGui::TextDisabled("深度：需要重启后启用搜索。");
        return;
    case DlssNrNativeMode::Finder::CouldNotStart:
        ImGui::TextDisabled("深度：搜索器启动失败，请查看日志；NR 仅使用运动信息。");
        return;
    case DlssNrNativeMode::Finder::Installed:
        break;
    }

    const auto pick = CurrentPick();
    const uint32_t warmup = Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default();
    const char* untilRestart = wanted ? "" : " (finder off at next start)";

    if (g_core.GameCallsUpscaler())
        ImGui::TextDisabled("深度：游戏正在调用超分，搜索已暂停；可在游戏设置关闭超分。");
    else if (!g_core.Armed())
        ImGui::TextDisabled("深度：观察中（%llu/%u 帧）…%s",
                            (unsigned long long) (g_core.Presents() - g_core.WarmupStart()), warmup, untilRestart);
    else if (!pick.valid)
        ImGui::TextDisabled("深度：尚未找到，NR 仅使用运动信息%s。", untilRestart);
    else
        ImGui::Text("深度：选中 %ux%u%s%s", pick.width, pick.height, pick.reversed ? ", reversed-Z" : "",
                    untilRestart);

    if (ImGui::IsItemHovered() && pick.valid)
        ImGui::SetTooltip("格式 %u，候选资源见日志的 Depth finder 记录。", pick.format);
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("候选资源见日志的 Depth finder 记录。");
}

void DrawAdvancedUi()
{
    auto* config = Config::Instance();
    bool finder = config->DlssNrNativeDepthFinder.value_or_default();

    if (SettingsUi::SwitchRow("使用游戏深度（改善质量，需重启）##depthfinder11", &finder))
        config->DlssNrNativeDepthFinder = finder;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s", "Watches the game's immediate context (Direct3D 11) and picks the scene's depth buffer, so\n"
                  "NR and the stabiliser get depth as well as motion. Optional: without it they run on motion\n"
                  "only. It is also what notices the game calling its own upscaler: without it, nothing here\n"
                  "stands aside for that. Deferred contexts are not watched yet. Choosing a mode above turns\n"
                  "it on, Off turns it off. Applies at the next start: save the settings and restart the game.");
}
} // namespace GenericDepthDx11
