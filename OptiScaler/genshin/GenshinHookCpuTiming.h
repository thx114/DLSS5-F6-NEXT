// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>
#include "GenshinShaderCacheMetrics.h"
#include "../runtime/FsrHookHudMetrics.h"

// R448: bounded observational wall timing only. No resource/route authority.
// Fixed TLS counters; only sampled calls read QPC. Publication uses per-thread
// atomic cells (no logging/heap/mutex on a hook). Rows are inclusive, never additive.
namespace GenshinHookCpuTiming {
inline constexpr unsigned Stride=64, ThreadLimit=32, PathCount=6;
inline std::atomic<unsigned> control{0}; // bit0 timing, bit1 controls.Active() snapshot
inline bool Enabled() noexcept {return (control.load(std::memory_order_relaxed)&1)!=0;}
// Test-process override only: read once, preserve the caller's Win32 error and
// leave the persisted controls/profile untouched. No graphics authority changes.
inline bool ProcessTimingRequested() noexcept {
    static const bool requested=[]() noexcept {
        const auto error=GetLastError();
        wchar_t value[2]{};
        const auto size=GetEnvironmentVariableW(L"OPTISCALER_HOOK_CPU_TIMING",value,2);
        SetLastError(error);
        return size==1 && value[0]==L'1';
    }();
    return requested;
}
enum class Hook : unsigned {
    hkCachePSSetShader,
    hkCacheStatePSSetShader,
    hkQueryBegin,
    hkQueryEnd,
    hkStateQueryBegin,
    hkStateQueryEnd,
    hkStateExecute,
    hkStateFinish,
    hkStateClear,
    hkClearState,
    hkExecuteCommandList,
    hkFinishCommandList,
    hkDiscardResource,
    hkDiscardView,
    hkDiscardView1,
    hkSwapDeviceContextState,
    hkCreateContextState,
    hkOMSetRenderTargets,
    hkOMSetRenderTargetsAndUAV,
    hkClearDepthStencilView,
    hkCreateDepthCs,
    hkDepthCopyRegion1,
    hkDepthUpdate1,
    hkDepthClearView,
    hkDepthDispatchIndirect,
    hkDepthCopyStructure,
    hkDepthCopyResource,
    hkDepthCopyRegion,
    hkDepthUpdate,
    hkDepthMap,
    hkDepthUnmap,
    hkDepthResolve,
    hkDepthMips,
    hkClearRtv,
    hkClearUavUint,
    hkClearUavFloat,
    hkVSSetConstantBuffers,
    hkPSSetConstantBuffers,
    hkCSSetConstantBuffers,
    hkRSSetViewports,
    hkDraw,
    hkDrawIndexed,
    hkDrawInstanced,
    hkDrawIndexedInstanced,
    hkDrawAuto,
    hkDrawIndexedInstancedIndirect,
    hkDrawInstancedIndirect,
    hkDispatch,
    Present,
    Present1,
    hkModSOSetTargets,
    Count
};
enum class Stage : unsigned {
    ControlsLock,
    ControlsLocked,
    OwnerLock,
    OwnerLocked,
    OwnerTryLock,
    TransportPoolLock,
    TransportPoolLocked,
    TransportRetiredLock,
    TransportRetiredLocked,
    ProviderPoolLock,
    ProviderPoolLocked,
    BeforeDraw,
    AfterDraw,
    Replacement,
    Configure,
    StateSave,
    Prepare,
    HostSubmit,
    GameCopyback,
    StateRestore,
    TransportBegin,
    TransportPoll,
    TransportCopyback,
    RecordingWait,
    ProviderEvaluate,
    Dx12Submit,
    CpuEventWait,
    DepthCpuEventWait,
    InputFlush,
    OutputFlush,
    Dx12QueueWaitApi,
    Dx11WaitApi,
    FenceQueryApi,
    QueryGetDataDontFlush,
    PresentResizeLock,
    PresentResizeLocked,
    StateLock,
    StateLocked,
    RecordingBlocked,
    AllocatorCpuEventWait,
    InitCpuEventWait,
    RecordingCpuEventWait,
    Dx12SignalApi,
    Dx11SignalApi,
    FenceArmApi,
    DeferredInputOutputFlush,
    FgReflexSleep,
    FgFrameLimitSleep,
    FgSdkPresent, // R725: saved DLSSG Present API stack wall, includes vendor callbacks.
    FgPclMarker, // R897: existing host PCL forwarding only; inclusive, stride64.
    MapPreimage,
    MapInvalidate,
    MapSemantic,
    MapPortable,
    MapProbe,
    UnmapInvalidate,
    UnmapProbe,
    UnmapPortable,
    UnmapSemantic,
    Count
};
enum Field : unsigned {
    Calls,
    Samples,
    Total,
    TotalMax,
    Original,
    OriginalMax,
    Before,
    BeforeMax,
    After,
    AfterMax,
    Wrapper,
    WrapperMax,
    OriginalCalls,
    OriginalSamples,
    Nested,
    Refused,
    NoOriginal,
    ResultOk,
    ResultPending,
    ResultFail,
    LastResult,
    SampleOver100us,
    SampleOver1000us,
    SampleOver16000us,
    ClockFailed,
     FieldCount
};
inline constexpr unsigned HookCount=static_cast<unsigned>(Hook::Count);
inline constexpr unsigned StageCount=static_cast<unsigned>(Stage::Count);
inline constexpr unsigned BucketCount=HookCount*PathCount+StageCount*2;
inline const char* HookName(unsigned i) noexcept {
    static constexpr const char* names[]={
        "hkCachePSSetShader",
        "hkCacheStatePSSetShader",
        "hkQueryBegin",
        "hkQueryEnd",
        "hkStateQueryBegin",
        "hkStateQueryEnd",
        "hkStateExecute",
        "hkStateFinish",
        "hkStateClear",
        "hkClearState",
        "hkExecuteCommandList",
        "hkFinishCommandList",
        "hkDiscardResource",
        "hkDiscardView",
        "hkDiscardView1",
        "hkSwapDeviceContextState",
        "hkCreateContextState",
        "hkOMSetRenderTargets",
        "hkOMSetRenderTargetsAndUAV",
        "hkClearDepthStencilView",
        "hkCreateDepthCs",
        "hkDepthCopyRegion1",
        "hkDepthUpdate1",
        "hkDepthClearView",
        "hkDepthDispatchIndirect",
        "hkDepthCopyStructure",
        "hkDepthCopyResource",
        "hkDepthCopyRegion",
        "hkDepthUpdate",
        "hkDepthMap",
        "hkDepthUnmap",
        "hkDepthResolve",
        "hkDepthMips",
        "hkClearRtv",
        "hkClearUavUint",
        "hkClearUavFloat",
        "hkVSSetConstantBuffers",
        "hkPSSetConstantBuffers",
        "hkCSSetConstantBuffers",
        "hkRSSetViewports",
        "hkDraw",
        "hkDrawIndexed",
        "hkDrawInstanced",
        "hkDrawIndexedInstanced",
        "hkDrawAuto",
        "hkDrawIndexedInstancedIndirect",
        "hkDrawInstancedIndirect",
        "hkDispatch",
        "Present",
        "Present1",
        "hkModSOSetTargets"};return names[i];
}
inline const char* StageName(unsigned i) noexcept {
    static constexpr const char* names[]={
        "ControlsLock",
        "ControlsLocked",
        "OwnerLock",
        "OwnerLocked",
        "OwnerTryLock",
        "TransportPoolLock",
        "TransportPoolLocked",
        "TransportRetiredLock",
        "TransportRetiredLocked",
        "ProviderPoolLock",
        "ProviderPoolLocked",
        "BeforeDraw",
        "AfterDraw",
        "Replacement",
        "Configure",
        "StateSave",
        "Prepare",
        "HostSubmit",
        "GameCopyback",
        "StateRestore",
        "TransportBegin",
        "TransportPoll",
        "TransportCopyback",
        "RecordingWait",
        "ProviderEvaluate",
        "Dx12Submit",
        "CpuEventWait",
        "DepthCpuEventWait",
        "InputFlush",
        "OutputFlush",
        "Dx12QueueWaitApi",
        "Dx11WaitApi",
        "FenceQueryApi",
        "QueryGetDataDontFlush",
        "PresentResizeLock",
        "PresentResizeLocked",
        "StateLock",
        "StateLocked",
        "RecordingBlocked",
        "AllocatorCpuEventWait",
        "InitCpuEventWait",
        "RecordingCpuEventWait",
        "Dx12SignalApi",
        "Dx11SignalApi",
        "FenceArmApi",
        "DeferredInputOutputFlush",
        "FgReflexSleep",
        "FgFrameLimitSleep",
        "FgSdkPresent",
        "FgPclMarker",
        "MapPreimage","MapInvalidate","MapSemantic","MapPortable","MapProbe",
        "UnmapInvalidate","UnmapProbe","UnmapPortable","UnmapSemantic"};return names[i];
}
using Values=std::array<uint64_t,FieldCount>;
// R684: out-of-line, non-constexpr constructors bound initialization at
// each level instead of expanding all threads/buckets/atomic fields.
struct Cell {
    std::atomic<uint64_t> version{0};std::array<std::atomic<uint64_t>,FieldCount> values{};
    Cell() noexcept;
};
// R897: CPU-only attribution. Observed pointer values never grant GPU/lifetime authority.
enum class StarRailCall : unsigned { HiddenPresent, CopyWait, SdkPresent, PclMarker, Count };
enum class StarRailCopyRing : unsigned { Unknown, SharedColor, NativeOverwrite };
enum class StarRailOrigin : unsigned { Unknown, Hidden, CopyRetirement, AllocatorReset, SdkPresent, FgPresentStart, FgPresentEnd, NvapiForwardGame, NvapiForwardOwn, NvapiForwardOther };
struct StarRailInvocation {
    uint64_t entryQpc=0,copyTarget=0;uintptr_t wrapper=0,child=0,copyQueue=0,copyFence=0;
    DWORD pid=0,tid=0;bool known=false;
};
extern constinit thread_local StarRailInvocation starRailInvocation;
struct StarRailStamp {
    StarRailInvocation parent{};uint64_t timingEpoch=0,sourceSequence=0,frame=0;
    uintptr_t chain=0,queue=0,presentQueue=0,fence=0;uint64_t slot=UINT64_MAX,required=0,completed=0;
    uint64_t markerFrame=0;uint32_t sync=0,flags=0,marker=UINT32_MAX,result=0;
    StarRailOrigin origin=StarRailOrigin::Unknown;StarRailCopyRing ring=StarRailCopyRing::Unknown;
    bool completedKnown=false,flagsKnown=false,markerFrameKnown=false,resultKnown=false;
};
enum StarRailWord : unsigned {
    SrParentKnown,SrEntryQpc,SrPid,SrTid,SrWrapper,SrChild,SrSourceSequence,SrFrame,
    SrChain,SrQueue,SrPresentQueue,SrFence,SrSlot,SrRequired,SrCompleted,SrCompletedKnown,
    SrSync,SrFlags,SrFlagsKnown,SrOrigin,SrRing,SrMarker,SrMarkerFrame,SrMarkerFrameKnown,
    SrResult,SrResultKnown,SrBegin,SrEnd,SrWordCount
};
using StarRailWords=std::array<uint64_t,SrWordCount>;
struct StarRailData {
    uint64_t epoch=UINT64_MAX,calls=0,samples=0,ticks=0,maxTicks=0,clockFailed=0;
    StarRailWords last{},maximum{};
};
inline constexpr unsigned StarRailCallCount=static_cast<unsigned>(StarRailCall::Count);
inline constexpr unsigned StarRailPayloadCount=6+SrWordCount*2;
struct StarRailCell {
    std::atomic<uint64_t> version{0};std::array<std::atomic<uint64_t>,StarRailPayloadCount> values{};
    StarRailCell() noexcept;
};
struct ThreadSlot {
    std::atomic<DWORD> tid{0};std::array<Cell,BucketCount> cells{};
    Cell finalRawPresent; // R830: completed observations survive worker TLS exit.
    std::array<StarRailCell,StarRailCallCount> starRailCalls{};
    ThreadSlot() noexcept;
};
// Fixed storage is initialized eagerly once by the DLL CRT before hooks.
extern std::array<ThreadSlot,ThreadLimit> slots;
inline std::atomic<unsigned> nextSlot{0};
inline std::atomic<uint64_t> refusedThreads{0};
inline int64_t Tick() noexcept {
    const auto error=GetLastError();LARGE_INTEGER v{};
    const bool ok=QueryPerformanceCounter(&v)!=FALSE;SetLastError(error);
    return ok?v.QuadPart:0;
}
// Independent of Debug/control and the inclusive monitor rows. Admission is
// established by Family's existing serialized, current Genshin capture Present;
// individual roots still require an existing direct owner/context check.
struct FsrHudClock {static int64_t Tick() noexcept {return GenshinHookCpuTiming::Tick();}};
using FsrHudSampler=FsrHookHudMetrics::RootSampler<HookCount,FsrHudClock>;
inline constinit thread_local FsrHudSampler fsrHudLocal;
inline std::atomic<uint64_t> fsrHudGeneration{1};
inline FsrHookHudMetrics::SnapshotCell fsrHudPublication;
inline void InvalidateFsrHudOwner() noexcept {
    fsrHudGeneration.fetch_add(1,std::memory_order_acq_rel);fsrHudLocal.Reset(0);
}
inline void BeginFsrHudOwner() noexcept {
    fsrHudLocal.Reset(fsrHudGeneration.fetch_add(1,std::memory_order_acq_rel)+1);
}
inline void ConfirmFsrHudRoot(bool owned) noexcept {
    if(fsrHudLocal.Generation()==fsrHudGeneration.load(std::memory_order_acquire))fsrHudLocal.Confirm(owned);
}
class FsrHudHookScope final {
    FsrHudSampler::Ticket ticket_{};
public:
    FsrHudHookScope(Hook hook,bool collect) noexcept {
        // Present has a broader owner and includes unrelated frame/SDK work.
        if(hook==Hook::Present || hook==Hook::Present1 || !fsrHudLocal.Generation())return;
        if(fsrHudLocal.Generation()!=fsrHudGeneration.load(std::memory_order_acquire))return;
        ticket_=fsrHudLocal.Enter(static_cast<unsigned>(hook),collect);
    }
    void Finish() noexcept {
        if(!ticket_.generation)return;
        if(ticket_.generation==fsrHudGeneration.load(std::memory_order_acquire))fsrHudLocal.Leave(ticket_);
        else if(fsrHudLocal.Generation()==ticket_.generation)fsrHudLocal.Reset(0);
        ticket_={};
    }
    ~FsrHudHookScope(){Finish();}
    FsrHudHookScope(const FsrHudHookScope&)=delete;
};
class FsrHudOriginalScope final {
    FsrHudSampler::NativeTicket ticket_{};
public:
    FsrHudOriginalScope() noexcept {ticket_=fsrHudLocal.BeginOriginal();}
    ~FsrHudOriginalScope(){fsrHudLocal.EndOriginal(ticket_);}
    FsrHudOriginalScope(const FsrHudOriginalScope&)=delete;
};
using FsrHudSnapshot=FsrHookHudMetrics::Snapshot;
// HUD contract: unknown fields display "--". A known hookHost value MUST use
// "~%.2fms/s" (Prefix='~'). Coverage is confirmed hook roots on this capture
// thread only; unconfirmedRoots are omitted, NOT extrapolated into a total.
// Ratios use their own actual hits/(hits+misses) in the SAME completed window.
inline FsrHudSnapshot ReadFsrHudSnapshot() noexcept {
    const auto generation=fsrHudGeneration.load(std::memory_order_acquire);
    const auto view=FsrHookHudMetrics::Fresh(fsrHudPublication.Read(),generation,GetTickCount64());
    if(generation!=fsrHudGeneration.load(std::memory_order_acquire))return {};
    return view;
}
struct Local {
    int slot=-1;bool attempted=false;unsigned depth=0;
    std::array<Values,BucketCount> values{};
    std::array<uint64_t,HookCount> hookSequence{};
    Values finalRawPresent{};bool finalRawActive=false;
    std::array<StarRailData,StarRailCallCount> starRailCalls{};
    void Claim() noexcept {
        if(attempted)return;attempted=true;
        const auto i=nextSlot.fetch_add(1,std::memory_order_relaxed);
        if(i<ThreadLimit){slot=static_cast<int>(i);slots[i].tid.store(GetCurrentThreadId(),std::memory_order_release);}
        else refusedThreads.fetch_add(1,std::memory_order_relaxed);
    }
    void Publish(unsigned id) noexcept {
        if(slot<0)return;auto& c=slots[slot].cells[id];
        c.version.fetch_add(1,std::memory_order_acq_rel);
        for(unsigned i=0;i<FieldCount;++i)c.values[i].store(values[id][i],std::memory_order_relaxed);
        c.version.fetch_add(1,std::memory_order_release);
    }
    void PublishFinalRaw() noexcept {
        if(slot<0)return;auto& c=slots[slot].finalRawPresent;
        c.version.fetch_add(1,std::memory_order_acq_rel);
        for(unsigned i=0;i<FieldCount;++i)c.values[i].store(finalRawPresent[i],std::memory_order_relaxed);
        c.version.fetch_add(1,std::memory_order_release);
    }
    void PublishStarRail(unsigned id) noexcept {
        if(slot<0)return;auto& c=slots[slot].starRailCalls[id];const auto& d=starRailCalls[id];
        c.version.fetch_add(1,std::memory_order_acq_rel);
        const uint64_t head[]={d.epoch,d.calls,d.samples,d.ticks,d.maxTicks,d.clockFailed};
        for(unsigned i=0;i<6;++i)c.values[i].store(head[i],std::memory_order_relaxed);
        for(unsigned i=0;i<SrWordCount;++i){
            c.values[6+i].store(d.last[i],std::memory_order_relaxed);
            c.values[6+SrWordCount+i].store(d.maximum[i],std::memory_order_relaxed);
        }
        c.version.fetch_add(1,std::memory_order_release);
    }
    void PublishAll() noexcept {if(slot>=0)for(unsigned i=0;i<BucketCount;++i)if(values[i][Calls])Publish(i);}
    // No TLS destructor: first use must not register CRT heap bookkeeping.
    // Worker tails remain bounded/unpublished until a later sample; slots never recycle.
};
// The fixed, destructor-free TLS payload also has exactly one definition.
extern constinit thread_local Local local;
inline uint64_t Span(int64_t begin,int64_t end) noexcept {return begin>0 && end>=begin?uint64_t(end-begin):0;}
inline void Add(Values& v,Field sum,Field maximum,uint64_t ticks) noexcept {
    v[sum]+=ticks;if(v[maximum]<ticks)v[maximum]=ticks;
}
inline std::atomic<int64_t> frequency{0}; // Published atomically before enabling at the Present boundary.
inline void RecordWall(Values& v,uint64_t ticks) noexcept {
    ++v[Samples];Add(v,Total,TotalMax,ticks);
    // Sampled wall thresholds, not evidence of a blocked/sleeping thread.
    const auto f=frequency.load(std::memory_order_relaxed);
    if(f>0){v[SampleOver100us]+=ticks>=uint64_t(f)/10000;
        v[SampleOver1000us]+=ticks>=uint64_t(f)/1000;v[SampleOver16000us]+=ticks>=uint64_t(f)*16/1000;}
}
enum class ResultKind {None,Hresult,Event,Fence,Boolean};
inline void Result(Values& v,uint64_t raw,ResultKind kind) noexcept {
    if(kind==ResultKind::None)return;v[LastResult]=raw;
    if(kind==ResultKind::Hresult){const auto hr=static_cast<int32_t>(raw);
        if(hr<0)++v[ResultFail];else if(hr==1)++v[ResultPending];else ++v[ResultOk];}
    else if(kind==ResultKind::Event){
        if(raw==WAIT_OBJECT_0)++v[ResultOk];else if(raw==WAIT_TIMEOUT)++v[ResultPending];else ++v[ResultFail];}
    else if(kind==ResultKind::Boolean){if(raw)++v[ResultOk];else ++v[ResultFail];}
    else {if(raw==UINT64_MAX)++v[ResultFail];else ++v[ResultOk];} // Fence query value is not a completion/cost proof.
}
// R654: an explicitly finished scope can retain its exact sampled wall endpoints.
// Unsampled/unavailable endpoints stay zero; no additional QPC reads.
struct WallSample {bool sampled=false;int64_t begin=0,end=0;};
class Scope final {
    unsigned id_=BucketCount;bool sampled_=false;int64_t begin_=0;
public:
    explicit Scope(Stage stage,bool collect=true) noexcept {
        const auto mode=control.load(std::memory_order_relaxed);
        if(!collect || !(mode&1))return;local.Claim();if(local.slot<0)return;
        id_=HookCount*PathCount+static_cast<unsigned>(stage)*2+((mode&2)?1:0);
        auto& v=local.values[id_];++v[Calls];
        const bool blocking=stage==Stage::CpuEventWait || stage==Stage::DepthCpuEventWait || stage==Stage::RecordingBlocked || stage==Stage::AllocatorCpuEventWait ||
            stage==Stage::InitCpuEventWait || stage==Stage::RecordingCpuEventWait;
        sampled_=blocking || (v[Calls]&(Stride-1))==1;
        if(sampled_)begin_=Tick();
    }
    void Returned(uint64_t raw,ResultKind kind) noexcept {if(id_<BucketCount)Result(local.values[id_],raw,kind);}
    template<class T> T Return(T result,ResultKind kind=ResultKind::Hresult) noexcept {
        Returned(static_cast<uint64_t>(result),kind);return result;
    }
    WallSample Finish() noexcept {
        if(id_==BucketCount)return {};auto& v=local.values[id_];
        WallSample sample{sampled_,begin_,0};
        if(sampled_){sample.end=Tick();if(begin_>0 && sample.end>=begin_)RecordWall(v,uint64_t(sample.end-begin_));else ++v[ClockFailed];}
        if(sampled_ || (v[Calls]&(Stride-1))==0)local.Publish(id_);
        id_=BucketCount;return sample;
    }
    ~Scope() noexcept {Finish();}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
template<class F> decltype(auto) Call(Stage stage,F&& f,ResultKind kind=ResultKind::None) {
    Scope scope(stage);
    if constexpr(std::is_void_v<std::invoke_result_t<F>>){std::forward<F>(f)();}
    else {auto result=std::forward<F>(f)();scope.Returned(static_cast<uint64_t>(result),kind);return result;}
}
// Attribute a real observer call without another logger, timer or resource read.
// Return type and exceptions pass through unchanged; existing sampler controls,
// stride and per-thread publication apply. These nested intervals are inclusive.
template<class Work> decltype(auto) ObserveWork(Stage stage,Work&& work) {
    Scope timing(stage);
    return std::forward<Work>(work)();
}
class HookScope final {
    FsrHudHookScope fsrHud_;
    unsigned hook_=HookCount,path_=0;bool sampled_=false,nested_=false,refused_=false;
    int64_t begin_=0,firstOriginal_=0;uint64_t original_=0,originalCalls_=0;bool inOriginal_=false,clockFailed_=false;
public:
    explicit HookScope(Hook hook,bool collect=true) noexcept :fsrHud_(hook,collect) {
        const auto mode=control.load(std::memory_order_relaxed);
        if(!collect || !(mode&1))return;path_=(mode&2)?3:0;local.Claim();if(local.slot<0)return;
        hook_=static_cast<unsigned>(hook);nested_=local.depth++!=0;
        sampled_=(++local.hookSequence[hook_]&(Stride-1))==1;if(sampled_)begin_=Tick();
    }
    bool Active() const noexcept {return hook_<HookCount;}
    bool InOriginal() const noexcept {return inOriginal_;}
    void Internal(bool internal) noexcept {if(internal)path_=(path_>=3?3:0)+2;}
    void NonTarget(bool refused) noexcept {refused_=refused;if(refused && path_%3!=2)path_=(path_>=3?3:0)+1;}
    template<class F> decltype(auto) Original(F&& f) {
        FsrHudOriginalScope fsrOriginal;
        if(!Active())return std::forward<F>(f)();
        ++originalCalls_;
        if(!sampled_)return std::forward<F>(f)();
        // Same-owner recursion stays inside the outer native interval. It must
        // never add a second overlapping duration to original_.
        if(inOriginal_)return std::forward<F>(f)();
        inOriginal_=true;const auto start=Tick();if(!firstOriginal_)firstOriginal_=start;
        struct End {HookScope& self;int64_t start;~End(){const auto end=Tick();if(start<=0 || end<start)self.clockFailed_=true;else self.original_+=uint64_t(end-start);self.inOriginal_=false;}} end{*this,start};
        return std::forward<F>(f)();
    }
    ~HookScope() noexcept {
        fsrHud_.Finish();
        if(!Active())return;
        // Stop timing before counter aggregation/publication (documented observer cost).
        const auto end=sampled_?Tick():0;--local.depth;
        const auto id=hook_*PathCount+path_;auto& v=local.values[id];
        ++v[Calls];v[OriginalCalls]+=originalCalls_;v[Nested]+=nested_;v[Refused]+=refused_;v[NoOriginal]+=originalCalls_==0;
        if(sampled_){
            if(begin_>0 && end>=begin_ && !clockFailed_ && original_<=uint64_t(end-begin_)){
                const auto total=uint64_t(end-begin_);RecordWall(v,total);
                const auto before=firstOriginal_?Span(begin_,firstOriginal_):total;
                // With multiple real calls, after also contains intervals between originals.
                const auto wrapper=total>=original_?total-original_:0;
                const auto after=wrapper>=before?wrapper-before:0;
                Add(v,Before,BeforeMax,before);Add(v,After,AfterMax,after);Add(v,Wrapper,WrapperMax,wrapper);
                if(originalCalls_){++v[OriginalSamples];Add(v,GenshinHookCpuTiming::Original,OriginalMax,original_);}
            }else ++v[ClockFailed];
        }
        if(sampled_ || (v[Calls]&(Stride-1))==0)local.Publish(id);
    }
    HookScope(const HookScope&)=delete;HookScope& operator=(const HookScope&)=delete;
};
// Present wrapper and LocalPresent share this one observation, so only the
// actual native API is original time and the same wrapper is not counted twice.
inline thread_local HookScope* presentOwner=nullptr;
class PresentOwnerScope final {
    HookScope* prior_=presentOwner;
public:
    explicit PresentOwnerScope(HookScope& owner) noexcept {presentOwner=&owner;}
    ~PresentOwnerScope(){presentOwner=prior_;}
    PresentOwnerScope(const PresentOwnerScope&)=delete;
    PresentOwnerScope& operator=(const PresentOwnerScope&)=delete;
};
// The one existing Genshin Present boundary publishes the config snapshot and
// drains at most once/second. No INI reads here or on hook entries. Slots never
// recycle; dead-thread rows remain cumulative and tid disambiguates them.
// Only this Present thread reads published cells; all payload fields are atomic.
// R600: the existing Present drain also publishes these sampled per-kind means.
// Hook kinds and inclusive stages stay separate; this is not a CPU frame total.
struct MonitorReading {uint64_t samples=0,ticks=0;};
inline constexpr unsigned MonitorCount=HookCount+StageCount;
struct MonitorSnapshot {
    uint64_t beginMs=0,endMs=0,epoch=0;
    std::array<MonitorReading,MonitorCount> rows{};
};
struct MonitorCell {std::atomic<uint64_t> samples{0},ticks{0};};
inline std::array<MonitorCell,MonitorCount> monitorRows{};
inline std::atomic<uint64_t> monitorVersion{0},monitorBegin{0},monitorEnd{0},monitorPublishedEpoch{0},monitorEpoch{0};
// R654 uses the existing once/second Present drain, without another timer or logger.
// A raced/in-progress drain is deferred; never spin on a Present thread.
inline bool ConsumePresentDrain(uint64_t& previous,uint64_t epoch) noexcept {
    const auto version=monitorVersion.load(std::memory_order_acquire);if(!version || (version&1))return false;
    const auto end=monitorEnd.load(std::memory_order_relaxed);
    const auto publishedEpoch=monitorPublishedEpoch.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if(version!=monitorVersion.load(std::memory_order_acquire) || !Enabled() ||
       publishedEpoch!=epoch || epoch!=monitorEpoch.load(std::memory_order_acquire) || !end || end==previous)return false;
    previous=end;return true;
}
// R897: one same-thread source invocation; never propagate "latest" state to workers.
inline std::atomic<bool> starRailProcessAdmitted{false};
inline thread_local unsigned starRailSourceDepth=0;
inline bool StarRailTimingActive() noexcept {
    return Enabled() && starRailProcessAdmitted.load(std::memory_order_relaxed);
}
class StarRailInvocationScope final {
    StarRailInvocation previous_;bool entered_;
public:
    StarRailInvocationScope(bool admitted,const void* wrapper,const void* child,uint64_t entryQpc) noexcept
        :previous_(starRailInvocation),entered_(admitted) {
        starRailInvocation={};
        if(!admitted)return;
        ++starRailSourceDepth;
        if(!Enabled())return;
        starRailProcessAdmitted.store(true,std::memory_order_relaxed);
        starRailInvocation.entryQpc=entryQpc;
        starRailInvocation.wrapper=reinterpret_cast<uintptr_t>(wrapper);
        starRailInvocation.child=reinterpret_cast<uintptr_t>(child);
        starRailInvocation.pid=GetCurrentProcessId();starRailInvocation.tid=GetCurrentThreadId();
        starRailInvocation.known=entryQpc!=0 && wrapper!=nullptr && child!=nullptr;
    }
    ~StarRailInvocationScope() noexcept {
        starRailInvocation=previous_;if(entered_)--starRailSourceDepth;
    }
    StarRailInvocationScope(const StarRailInvocationScope&)=delete;
    StarRailInvocationScope& operator=(const StarRailInvocationScope&)=delete;
};
inline StarRailStamp MakeStarRailStamp(StarRailOrigin origin,const void* child=nullptr) noexcept {
    StarRailStamp s;s.origin=origin;s.timingEpoch=monitorEpoch.load(std::memory_order_acquire);
    if(StarRailTimingActive() && starRailInvocation.known &&
       (!child || starRailInvocation.child==reinterpret_cast<uintptr_t>(child))) {
        s.parent=starRailInvocation;s.queue=s.parent.copyQueue;s.fence=s.parent.copyFence;
        s.required=s.parent.copyTarget;
    }
    return s;
}
inline void ObserveStarRailCopyReceipt(const void* wrapper,const void* queue,const void* fence,uint64_t target) noexcept {
    if(!StarRailTimingActive() || !starRailInvocation.known ||
       starRailInvocation.wrapper!=reinterpret_cast<uintptr_t>(wrapper))return;
    starRailInvocation.copyQueue=reinterpret_cast<uintptr_t>(queue);
    starRailInvocation.copyFence=reinterpret_cast<uintptr_t>(fence);starRailInvocation.copyTarget=target;
}
inline StarRailWords EncodeStarRailStamp(const StarRailStamp& s,WallSample wall) noexcept {
    return {s.parent.known,s.parent.entryQpc,s.parent.pid,s.parent.tid,s.parent.wrapper,s.parent.child,
        s.sourceSequence,s.frame,s.chain,s.queue,s.presentQueue,s.fence,s.slot,s.required,s.completed,s.completedKnown,
        s.sync,s.flags,s.flagsKnown,static_cast<unsigned>(s.origin),static_cast<unsigned>(s.ring),
        s.marker,s.markerFrame,s.markerFrameKnown,s.result,s.resultKnown,
        static_cast<uint64_t>(wall.begin),static_cast<uint64_t>(wall.end)};
}
inline void RecordStarRailCall(StarRailCall call,const StarRailStamp& stamp,WallSample wall,bool callerStride=false) noexcept {
    if(!StarRailTimingActive() || stamp.timingEpoch!=monitorEpoch.load(std::memory_order_acquire))return;
    local.Claim();if(local.slot<0)return;
    const auto id=static_cast<unsigned>(call);auto& d=local.starRailCalls[id];
    if(d.epoch!=stamp.timingEpoch){d={};d.epoch=stamp.timingEpoch;}
    ++d.calls;
    // SDK/PCL wall endpoints already use their existing Stage stride64.
    // CallerCPU already read QPC: downsample its metadata without another clock read.
    if(callerStride && (d.calls&(Stride-1))!=1)wall.sampled=false;
    if(wall.sampled){
        if(wall.begin<=0 || wall.end<wall.begin)++d.clockFailed;
        else {
            const auto ticks=uint64_t(wall.end-wall.begin);const auto words=EncodeStarRailStamp(stamp,wall);
            ++d.samples;d.ticks+=ticks;d.last=words;
            if(d.samples==1 || ticks>d.maxTicks){d.maxTicks=ticks;d.maximum=words;}
        }
    }
    if(wall.sampled || (d.calls&(Stride-1))==0)local.PublishStarRail(id);
}
inline const char* StarRailCallName(unsigned i) noexcept {
    static constexpr const char* names[]={"HiddenPresent","CopyWait","SdkPresent","PclMarker"};return names[i];
}
inline const char* StarRailOriginName(uint64_t i) noexcept {
    static constexpr const char* names[]={"UNKNOWN","Hidden","CopyRetirement","AllocatorReset","SdkPresent","FgPresentStart","FgPresentEnd","NvapiForwardGame","NvapiForwardOwn","NvapiForwardOther"};
    return i<sizeof(names)/sizeof(names[0])?names[i]:"UNKNOWN";
}
inline const char* StarRailRingName(uint64_t i) noexcept {
    return i==1?"SharedColor":(i==2?"NativeOverwrite":"UNKNOWN");
}
template<class Emit> void DrainStarRailCalls(Emit&& emit) {
    if(!StarRailTimingActive())return;
    static std::atomic<uint64_t> consumed{0};
    auto previous=consumed.load(std::memory_order_relaxed);const auto expected=previous;
    const auto epoch=monitorEpoch.load(std::memory_order_acquire);
    if(!ConsumePresentDrain(previous,epoch))return;
    auto seen=expected;if(!consumed.compare_exchange_strong(seen,previous))return;
    static std::array<std::array<std::atomic<uint64_t>,StarRailCallCount>,ThreadLimit> emitted{};
    for(unsigned t=0;t<ThreadLimit;++t)for(unsigned i=0;i<StarRailCallCount;++i){
        const auto& c=slots[t].starRailCalls[i];const auto before=c.version.load(std::memory_order_acquire);
        if(!before || (before&1))continue;
        auto old=emitted[t][i].load(std::memory_order_relaxed);if(old>=before)continue;
        std::array<uint64_t,StarRailPayloadCount> payload{};
        for(unsigned j=0;j<StarRailPayloadCount;++j)payload[j]=c.values[j].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(before!=c.version.load(std::memory_order_acquire) || payload[0]!=epoch || !payload[1])continue;
        if(!emitted[t][i].compare_exchange_strong(old,before))continue; // defer, never spin
        StarRailData d;d.epoch=payload[0];d.calls=payload[1];d.samples=payload[2];
        d.ticks=payload[3];d.maxTicks=payload[4];d.clockFailed=payload[5];
        for(unsigned j=0;j<SrWordCount;++j){d.last[j]=payload[6+j];d.maximum[j]=payload[6+SrWordCount+j];}
        emit(slots[t].tid.load(std::memory_order_acquire),i,d);
    }
}
// R830: reuse the fixed thread slots and once/second Present drain. Only
// completed raw calls publish; no source-frame association or live-object owner.
enum FinalRawField : unsigned {
    RawEpoch,RawCompleted,RawSamples,RawTicks,RawMax,RawSlow,RawClockFailed,RawNested,
    RawChain,RawQueue,RawEndpoint,RawSync,RawFlags,RawPresent1,RawBegin,RawEnd,
    RawHr,RawSequence,RawClockValid,RawCount
};
static_assert(static_cast<unsigned>(RawCount)<=FieldCount);
class FinalRawPresentScope final {
    Values* values_=nullptr;int64_t begin_=0;
    uintptr_t chain_,queue_,endpoint_;UINT sync_,flags_;bool present1_;
public:
    FinalRawPresentScope(const void* chain,const void* queue,const void* endpoint,
                         UINT sync,UINT flags,bool present1) noexcept
        :chain_(reinterpret_cast<uintptr_t>(chain)),queue_(reinterpret_cast<uintptr_t>(queue)),
         endpoint_(reinterpret_cast<uintptr_t>(endpoint)),sync_(sync),flags_(flags),present1_(present1) {
        if(!Enabled())return;local.Claim();if(local.slot<0)return;
        if(local.finalRawActive){++local.finalRawPresent[RawNested];return;}
        auto& v=local.finalRawPresent;const auto epoch=monitorEpoch.load(std::memory_order_acquire);
        if(v[RawEpoch]!=epoch){v={};v[RawEpoch]=epoch;}
        local.finalRawActive=true;values_=&v;begin_=Tick(); // Every qualified call, never stride64.
    }
    HRESULT Return(HRESULT hr) noexcept {
        if(!values_)return hr;const auto end=Tick();auto& v=*values_;
        ++v[RawCompleted];const bool valid=begin_>0 && end>=begin_;
        const auto ticks=Span(begin_,end);const auto hz=frequency.load(std::memory_order_relaxed);
        if(valid){++v[RawSamples];v[RawTicks]+=ticks;v[RawSlow]+=hz>0 && ticks>=uint64_t(hz)/20;}
        else ++v[RawClockFailed];
        // Retain the slowest completed call in this thread/timing epoch, even
        // when subsequent short calls run before the source's next drain.
        if(v[RawCompleted]==1 || (valid && (!v[RawClockValid] || ticks>v[RawMax]))){
            v[RawMax]=ticks;v[RawChain]=chain_;v[RawQueue]=queue_;v[RawEndpoint]=endpoint_;
            v[RawSync]=sync_;v[RawFlags]=flags_;v[RawPresent1]=present1_;
            v[RawBegin]=begin_;v[RawEnd]=end;v[RawHr]=static_cast<uint32_t>(hr);
            v[RawSequence]=v[RawCompleted];v[RawClockValid]=valid;
        }
        local.PublishFinalRaw();local.finalRawActive=false;values_=nullptr;return hr;
    }
    ~FinalRawPresentScope(){if(values_)local.finalRawActive=false;} // No fabricated return on unwind.
    FinalRawPresentScope(const FinalRawPresentScope&)=delete;
};
template<class Emit> void DrainFinalRawPresent(Emit&& emit) {
    if(!Enabled())return;
    static std::atomic<uint64_t> consumed{0};
    auto previous=consumed.load(std::memory_order_relaxed);const auto expected=previous;
    if(!ConsumePresentDrain(previous,monitorEpoch.load(std::memory_order_acquire)))return;
    auto seen=expected;if(!consumed.compare_exchange_strong(seen,previous))return;
    // Only the existing low-frequency drain reaches this nonblocking guard.
    static std::atomic_flag draining=ATOMIC_FLAG_INIT;
    if(draining.test_and_set(std::memory_order_acquire))return;
    struct Release {std::atomic_flag& flag;~Release(){flag.clear(std::memory_order_release);}} release{draining};
    static std::array<uint64_t,ThreadLimit> emitted{};
    for(unsigned t=0;t<ThreadLimit;++t){
        const auto& c=slots[t].finalRawPresent;const auto before=c.version.load(std::memory_order_acquire);
        if(!before || (before&1) || emitted[t]==before)continue;
        Values v{};for(unsigned i=0;i<FieldCount;++i)v[i]=c.values[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(before!=c.version.load(std::memory_order_acquire))continue; // Defer, never spin.
        if(v[RawCompleted])emit(t,slots[t].tid.load(std::memory_order_acquire),v);
        emitted[t]=before;
    }
}
inline void PublishMonitor(const MonitorSnapshot& view) noexcept {
    monitorVersion.fetch_add(1,std::memory_order_acq_rel);
    monitorBegin.store(view.beginMs,std::memory_order_relaxed);monitorEnd.store(view.endMs,std::memory_order_relaxed);
    monitorPublishedEpoch.store(view.epoch,std::memory_order_relaxed);
    for(unsigned i=0;i<MonitorCount;++i){monitorRows[i].samples.store(view.rows[i].samples,std::memory_order_relaxed);
        monitorRows[i].ticks.store(view.rows[i].ticks,std::memory_order_relaxed);}
    monitorVersion.fetch_add(1,std::memory_order_release);
}
inline MonitorSnapshot ReadMonitor() noexcept {
    MonitorSnapshot view{};if(!Enabled())return view;
    const auto version=monitorVersion.load(std::memory_order_acquire);if(!version || (version&1))return view;
    view.beginMs=monitorBegin.load(std::memory_order_relaxed);view.endMs=monitorEnd.load(std::memory_order_relaxed);
    view.epoch=monitorPublishedEpoch.load(std::memory_order_relaxed);
    for(unsigned i=0;i<MonitorCount;++i){view.rows[i].samples=monitorRows[i].samples.load(std::memory_order_relaxed);
        view.rows[i].ticks=monitorRows[i].ticks.load(std::memory_order_relaxed);}
    std::atomic_thread_fence(std::memory_order_acquire);
    if(version!=monitorVersion.load(std::memory_order_acquire) || !Enabled() ||
       view.epoch!=monitorEpoch.load(std::memory_order_acquire) || view.endMs<=view.beginMs ||
       GetTickCount64()-view.endMs>2000)return {};
    return view;
}
// Reuse the completed R858 query result; never start or wait on a query here.
struct BridgeGpuReading {double milliseconds=-1.0;uint64_t publishedMs=0,samples=0,sampleMs=0;};
inline std::atomic<uint64_t> bridgeGpuVersion{0},bridgeGpuTick{0},bridgeGpuSamples{0},bridgeGpuSampleMs{0};
inline std::atomic<double> bridgeGpuMs{-1.0};
inline std::atomic_flag bridgeGpuPublishing=ATOMIC_FLAG_INIT;
inline void PublishBridgeGpu(double milliseconds,uint64_t sampleMs) noexcept {
    if(bridgeGpuPublishing.test_and_set(std::memory_order_acquire))return;
    // Later consumer retirement can expose an older query after a newer one.
    if(!sampleMs || sampleMs<=bridgeGpuSampleMs.load(std::memory_order_relaxed)) {
        bridgeGpuPublishing.clear(std::memory_order_release);return;
    }
    bridgeGpuVersion.fetch_add(1,std::memory_order_acq_rel);
    bridgeGpuSampleMs.store(sampleMs,std::memory_order_relaxed);
    bridgeGpuMs.store(milliseconds,std::memory_order_relaxed);
    bridgeGpuTick.store(GetTickCount64(),std::memory_order_relaxed);bridgeGpuSamples.fetch_add(1,std::memory_order_relaxed);
    bridgeGpuVersion.fetch_add(1,std::memory_order_release);bridgeGpuPublishing.clear(std::memory_order_release);
}
inline BridgeGpuReading ReadBridgeGpu() noexcept {
    const auto version=bridgeGpuVersion.load(std::memory_order_acquire);if(!version || (version&1))return {};
    BridgeGpuReading view{bridgeGpuMs.load(std::memory_order_relaxed),bridgeGpuTick.load(std::memory_order_relaxed),
        bridgeGpuSamples.load(std::memory_order_relaxed),bridgeGpuSampleMs.load(std::memory_order_relaxed)};
    std::atomic_thread_fence(std::memory_order_acquire);
    if(version!=bridgeGpuVersion.load(std::memory_order_acquire) || !view.samples || !view.sampleMs ||
       GetTickCount64()-view.sampleMs>2000)return {};
    return view;
}
// R600/R606 seam: only a real, serialized Present owner may publish. R606 owns
// the Family::ReadMetadata / PsState::TryCurrent producer. Empty is unknown, not zero.
// Counts cover completed frames on ThreadId only, excluding reentrant/foreign reads.
// Read is an inclusive subset; detail adds complete Current roots and same-call direct children, never a frame CPU total.
struct ShaderCacheSnapshot {
    enum Index : unsigned { BeginMs,EndMs,ThreadId,Frames,TimingEpoch,MetadataState,BindingState,
        MetadataHits,MetadataMisses,BindingHits,BindingMisses,FallbackReads,
        HitSamples,HitTicks,MissSamples,MissTicks,QpcFrequency,Count };
    // State: 0 unknown, 1 unavailable, 2 available for the observed owner. A
    // published binding owner alone is NOT proof that TryCurrent can hit.
    std::array<uint64_t,Count> values{};
    GenshinLegacy::Family::ShaderCacheMetrics::Details detail{};
    uint64_t operator[](Index i)const noexcept{return values[i];}
};
inline std::array<std::atomic<uint64_t>,ShaderCacheSnapshot::Count> shaderCacheValues{};
inline std::array<std::atomic<uint64_t>,GenshinLegacy::Family::ShaderCacheMetrics::DetailFields> shaderCacheDetails{};
inline std::atomic<uint64_t> shaderCacheVersion{0};
inline std::atomic_flag shaderCachePublishing=ATOMIC_FLAG_INIT;
inline void PublishShaderCache(const ShaderCacheSnapshot& view) noexcept {
    if(shaderCachePublishing.test_and_set(std::memory_order_acquire))return;
    shaderCacheVersion.fetch_add(1,std::memory_order_acq_rel);
    for(unsigned i=0;i<ShaderCacheSnapshot::Count;++i)shaderCacheValues[i].store(view.values[i],std::memory_order_relaxed);
    auto detail=view.detail;unsigned field=0;
    detail.Fields([&](uint64_t& x){shaderCacheDetails[field++].store(x,std::memory_order_relaxed);});
    shaderCacheVersion.fetch_add(1,std::memory_order_release);shaderCachePublishing.clear(std::memory_order_release);
}
inline ShaderCacheSnapshot ReadShaderCache() noexcept {
    const auto version=shaderCacheVersion.load(std::memory_order_acquire);if(!version || (version&1))return {};
    ShaderCacheSnapshot view{};
    for(unsigned i=0;i<ShaderCacheSnapshot::Count;++i)view.values[i]=shaderCacheValues[i].load(std::memory_order_relaxed);
    unsigned field=0;view.detail.Fields([&](uint64_t& x){x=shaderCacheDetails[field++].load(std::memory_order_relaxed);});
    std::atomic_thread_fence(std::memory_order_acquire);
    if(version!=shaderCacheVersion.load(std::memory_order_acquire) || !view[ShaderCacheSnapshot::EndMs] ||
       GetTickCount64()-view[ShaderCacheSnapshot::EndMs]>2000)return {};
    return view;
}
// The existing one-second cache publisher calls this with the SAME completed
// window. No Debug frequency/control writes or additional draw clock reads.
inline void PublishFsrHudWindow(const ShaderCacheSnapshot& source) noexcept {
    FsrHudSnapshot view;
    view.beginMs=source[ShaderCacheSnapshot::BeginMs];view.endMs=source[ShaderCacheSnapshot::EndMs];
    view.frames=source[ShaderCacheSnapshot::Frames];view.threadId=source[ShaderCacheSnapshot::ThreadId];
    view.ownerGeneration=fsrHudLocal.Generation();
    view.windowKnown=view.ownerGeneration && view.endMs>view.beginMs && view.frames && view.threadId;
    view.shaderMetadata={source[ShaderCacheSnapshot::MetadataHits],source[ShaderCacheSnapshot::MetadataMisses]};
    constexpr auto current=static_cast<unsigned>(GenshinLegacy::Family::ShaderCacheMetrics::Consumer::Current);
    view.psBindingCurrent={source.detail.hits[current],source.detail.misses[current]};
    const auto error=GetLastError();LARGE_INTEGER hz{};
    const bool clock=QueryPerformanceFrequency(&hz)!=FALSE;SetLastError(error);
    view.hookHost=fsrHudLocal.Summarize(clock?hz.QuadPart:0,view.endMs>view.beginMs?view.endMs-view.beginMs:0);
    if(view.ownerGeneration!=fsrHudGeneration.load(std::memory_order_acquire))view={};
    fsrHudPublication.Publish(view);fsrHudLocal.ClearWindow();
}
struct PresentClock {static ULONGLONG Now() noexcept {return GetTickCount64();}};
template<class Clock=PresentClock,class Emit> void OnPresent(bool on,bool bridgeActive,Emit&& emit) {
    on=on || ProcessTimingRequested();
    if(on && !frequency.load(std::memory_order_relaxed)){const auto error=GetLastError();LARGE_INTEGER f{};
        if(QueryPerformanceFrequency(&f))frequency.store(f.QuadPart,std::memory_order_relaxed);SetLastError(error);}
    const auto previous=control.exchange((on?1u:0u)|(bridgeActive?2u:0u),std::memory_order_acq_rel);
    if((previous&1)!=(on?1u:0u))monitorEpoch.fetch_add(1,std::memory_order_acq_rel);
    static std::atomic<ULONGLONG> next{0};
    // One final drain on disable flushes the current thread, but does not touch
    // other threads' TLS. Their already published values remain readable.
    const bool drain=on || (previous&1);
    if(!drain)return;
    const auto now=Clock::Now();auto due=next.load(std::memory_order_relaxed);
    if(on && (now<due || !next.compare_exchange_strong(due,now+1000)))return;
    static std::atomic_flag draining=ATOMIC_FLAG_INIT;
    if(draining.test_and_set(std::memory_order_acquire))return;
    struct ReleaseDrain {std::atomic_flag& flag;~ReleaseDrain(){flag.clear(std::memory_order_release);}} release{draining};
    static std::array<std::array<uint64_t,BucketCount>,ThreadLimit> emitted{};
    static std::array<std::array<uint64_t,BucketCount>,ThreadLimit> reported{};
    static std::array<std::array<MonitorReading,BucketCount>,ThreadLimit> monitorPrevious{};
    static uint64_t previousEpoch=UINT64_MAX,previousDrain=0;
    MonitorSnapshot monitor{};monitor.epoch=monitorEpoch.load(std::memory_order_acquire);
    const bool baseline=previousEpoch!=monitor.epoch;
    // Keep live monitor/HUD publication at one second, independently of the
    // expensive detailed cumulative text dump. Disable always drains its tail.
    static uint64_t nextDetailed=0;
    const bool detailed=baseline || !on || now>=nextDetailed;
    if(detailed)nextDetailed=now+5000;
    monitor.beginMs=baseline?now:previousDrain;monitor.endMs=now;
    local.PublishAll();
    unsigned deferred=0;
    for(unsigned t=0;t<ThreadLimit;++t){
        const auto tid=slots[t].tid.load(std::memory_order_acquire);if(!tid)continue;
        for(unsigned id=0;id<BucketCount;++id){
            const auto& c=slots[t].cells[id];const auto before=c.version.load(std::memory_order_acquire);
            if(before&1){++deferred;continue;}
            const bool newMonitor=emitted[t][id]!=before;
            if(!before || (!newMonitor && (!detailed || reported[t][id]==before)))continue;Values v{};
            for(unsigned i=0;i<FieldCount;++i)v[i]=c.values[i].load(std::memory_order_relaxed);
            // Acquire fence pairs with the writer's odd-version acq_rel update;
            // every payload is atomic, and unstable rows are deferred (no spin).
            std::atomic_thread_fence(std::memory_order_acquire);
            if(before!=c.version.load(std::memory_order_acquire)){++deferred;continue;}
            if(!v[Calls])continue;emitted[t][id]=before;
            const bool hook=id<HookCount*PathCount;
            auto& old=monitorPrevious[t][id];
            const auto ticks=v[hook?Wrapper:Total];
            if(newMonitor && !baseline && v[Samples]>=old.samples && ticks>=old.ticks){
                auto& row=monitor.rows[hook?id/PathCount:HookCount+(id-HookCount*PathCount)/2];
                row.samples+=v[Samples]-old.samples;row.ticks+=ticks-old.ticks;
            }
            old={v[Samples],ticks};
            static constexpr const char* paths[]={"bridge_off_external",
        "bridge_off_external_non_target",
        "bridge_off_internal",
                "bridge_on_external",
        "bridge_on_external_non_target",
        "bridge_on_internal"};
            const char* path=hook?paths[id%PathCount]:((id-HookCount*PathCount)%2?"bridge_on_stage":"bridge_off_stage");
            if(detailed && reported[t][id]!=before){
                reported[t][id]=before;
                emit(tid,hook?HookName(id/PathCount):StageName((id-HookCount*PathCount)/2),path,v,frequency.load(std::memory_order_relaxed),deferred,refusedThreads.load(),on);
            }
        }
    }
    previousEpoch=monitor.epoch;previousDrain=now;PublishMonitor(monitor);
}
}
