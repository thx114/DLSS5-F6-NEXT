// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <intrin.h>

// R247: performance default. Set to 1 consistently across the build to restore
// all legacy R833 Scope diagnostics, including scheduled-thread CPU sampling.
#ifndef GENSHIN_CPU_STAGE_TIMING_ENABLED
#define GENSHIN_CPU_STAGE_TIMING_ENABLED 0
#endif

// R833 observation only. Sampled inclusive CPU wall scopes are never read by
// admission, resource, query, frame or publication gates. No result is cached.
namespace GenshinCpuStageTiming {
enum class Stage:unsigned {Native,Representation,Fixups,Patches,Imports,Retention,MappingCapture,MappingReplay,Profile,RoleSet,File,Delayed,
    PlanResolve,PlanRegions,PlanReads,PlanMetadata,
    BridgeSubmit,BridgeDispatch,BridgeRegister,BridgeTransportBegin,BridgeSubmitQueue,BridgeCopyback,BridgePoll,BridgeDepthWait,
    OptSrEvaluate,OptProviderEvaluate,OptRecordingWait,
    BridgeCollect,BridgeSurfaces,BridgeFences,BridgeInputOrder,
    HostBeforeNativeDraw,HostObserveDraw,HostBeforeDraw,HostFamilyGate,HostNativeReturn,
    HostReplacement,HostConfigure,HostStateSave,HostPrepare,HostSubmit,HostGameCopyback,HostStateRestore,
    HostDrawHook,HostAfterNativeDraw,Count};
inline constexpr unsigned Count=static_cast<unsigned>(Stage::Count),Stride=64;
// Source-controlled sampling only: zero disables the new scopes. Existing
// R833 stages retain their original stride and scheduled-thread measurements.
inline constexpr unsigned HostStride=64;
static_assert(HostStride==0 || (HostStride&(HostStride-1))==0);
inline constexpr bool Host(Stage stage)noexcept {
    return stage>=Stage::HostBeforeNativeDraw && stage<Stage::Count;
}
inline constexpr bool ScheduledCpu(Stage stage)noexcept {
    return stage==Stage::Native || (stage>=Stage::BridgeSubmit && stage<=Stage::BridgeInputOrder);
}
inline constexpr unsigned SampleStride(Stage stage)noexcept {return Host(stage)?HostStride:Stride;}
inline const char* Name(Stage stage)noexcept {
    static constexpr const char* names[]={"before_native_draw","draw_observers","before_draw","family_gate",
        "native_return","replacement","configure","state_save","private_prepare","submit","game_copyback","state_restore",
        "before_genshin_draw_hook","after_native_draw"};
    if(Host(stage))return names[static_cast<unsigned>(stage)-static_cast<unsigned>(Stage::HostBeforeNativeDraw)];
    switch(stage){
    case Stage::Native:return "native_authority";
    case Stage::BridgeTransportBegin:return "lease_begin";
    case Stage::BridgeInputOrder:return "lease_input_copy_signal_flush";
    case Stage::BridgeSubmitQueue:return "lease_execute_signal";
    case Stage::BridgeCopyback:return "lease_copyback_wait_copy_signal_flush";
    case Stage::OptSrEvaluate:return "opt_sr_evaluate";
    case Stage::OptProviderEvaluate:return "opt_provider_evaluate";
    case Stage::OptRecordingWait:return "opt_recording_wait";
    default:return "existing";
    }
}
struct Counter {std::atomic<uint64_t> samples{0},ticks{0},cpuSamples{0},cpu100ns{0},cycles{0},cpuFailures{0},maxTicks{0},calls{0};};
inline std::array<Counter,Count> counters{};
inline thread_local std::array<uint64_t,Count> sequences{};
struct Reading {uint64_t samples=0,ticks=0,cpuSamples=0,cpu100ns=0,cycles=0,cpuFailures=0,maxTicks=0,calls=0;};
inline Reading Read(Stage stage)noexcept {
    const auto i=static_cast<unsigned>(stage);if(i>=Count)return {};
    const auto& c=counters[i];return {c.samples.load(std::memory_order_relaxed),c.ticks.load(std::memory_order_relaxed),
        c.cpuSamples.load(std::memory_order_relaxed),c.cpu100ns.load(std::memory_order_relaxed),
        c.cycles.load(std::memory_order_relaxed),c.cpuFailures.load(std::memory_order_relaxed),
        c.maxTicks.load(std::memory_order_relaxed),c.calls.load(std::memory_order_relaxed)};
}
struct ReadWork {std::atomic<uint64_t> samples{0},queries{0},snapshots{0},liveBytes{0},proofBytes{0};};
inline ReadWork readWork;
inline void RecordReadWork(uint64_t queries,uint64_t snapshots,uint64_t liveBytes,uint64_t proofBytes)noexcept {
    readWork.queries.fetch_add(queries,std::memory_order_relaxed);readWork.snapshots.fetch_add(snapshots,std::memory_order_relaxed);
    readWork.liveBytes.fetch_add(liveBytes,std::memory_order_relaxed);readWork.proofBytes.fetch_add(proofBytes,std::memory_order_relaxed);
    readWork.samples.fetch_add(1,std::memory_order_relaxed);
}
#if GENSHIN_CPU_STAGE_TIMING_ENABLED
class Scope final {
    unsigned index_=Count;int64_t start_=0;uint64_t cpuStart_=0,cycleStart_=0;bool cpu_=false;
    static bool Cpu(uint64_t& cpu,uint64_t& cycles)noexcept {
        const auto error=GetLastError();FILETIME created{},exited{},kernel{},user{};ULONG64 value=0;
        const bool good=GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)!=FALSE &&
            QueryThreadCycleTime(GetCurrentThread(),&value)!=FALSE;
        cpu=((uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)+
            ((uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime);cycles=value;SetLastError(error);return good;
    }
    static bool Tick(int64_t& value)noexcept {
        const auto error=GetLastError();LARGE_INTEGER now{};
        const bool good=QueryPerformanceCounter(&now)!=FALSE;
        SetLastError(error);value=now.QuadPart;return good;
    }
public:
    explicit Scope(Stage stage)noexcept {
        const auto i=static_cast<unsigned>(stage);
        const auto stride=SampleStride(stage);
        if(i<Count && stride && (++sequences[i]&(stride-1))==(stride==1?0:1)){
            // Publish completed call batches only on sampled entries. At most
            // stride-1 trailing calls per thread remain outside this lower bound.
            if(Host(stage))counters[i].calls.fetch_add(sequences[i]==1?1:stride,std::memory_order_relaxed);
            // Actual scheduled thread CPU for native and the coarse Bridge/Opt
            // stages only. Other high-frequency stages retain QPC-only sampling.
            if(ScheduledCpu(stage))cpu_=Cpu(cpuStart_,cycleStart_);
            if(Tick(start_))index_=i;
        }
    }
    ~Scope()noexcept {Finish();}
    void Finish()noexcept {
        if(index_==Count)return;
        const auto index=index_;index_=Count;int64_t finish=0;
        if(!Tick(finish) || finish<start_)return;
        const auto ticks=static_cast<uint64_t>(finish-start_);auto& c=counters[index];
        c.ticks.fetch_add(ticks,std::memory_order_relaxed);
        c.samples.fetch_add(1,std::memory_order_relaxed);
        if(Host(static_cast<Stage>(index))){
            auto maximum=c.maxTicks.load(std::memory_order_relaxed);
            while(maximum<ticks && !c.maxTicks.compare_exchange_weak(maximum,ticks,std::memory_order_relaxed)){}
        }
        if(ScheduledCpu(static_cast<Stage>(index))){
            uint64_t cpu=0,cycles=0;
            if(cpu_ && Cpu(cpu,cycles) && cpu>=cpuStart_ && cycles>=cycleStart_){
                c.cpu100ns.fetch_add(cpu-cpuStart_,std::memory_order_relaxed);
                c.cycles.fetch_add(cycles-cycleStart_,std::memory_order_relaxed);
                c.cpuSamples.fetch_add(1,std::memory_order_relaxed);
            }else c.cpuFailures.fetch_add(1,std::memory_order_relaxed);
        }
    }
    bool Sampled()const noexcept {return index_!=Count;}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
#else
class Scope final {
public:
    explicit constexpr Scope(Stage)noexcept {}
    ~Scope()noexcept=default;
    void Finish()noexcept {}
    constexpr bool Sampled()const noexcept {return false;}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
#endif
}
